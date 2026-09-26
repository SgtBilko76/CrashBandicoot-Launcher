using RecompOne.Runtime;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Host.Cheats;
using Controller = RecompOne.Runtime.Hardware.Controller;

namespace CrashBandicoot.AndroidRuntime.QuestVr;

/// <summary>
/// Runtime host for Meta Quest: each finished PS1 frame goes to the OpenXR
/// presenter in libcrashxr.so, and the Touch controllers drive player one.
/// </summary>
sealed class VrPlatformHost(GlBackend backend, float depth3D) : IRuntimePlatformHost
{
    // Average interpupillary distance. At full strength, geometry at infinity
    // is shifted by just under this much on the virtual screen, so the eyes
    // end up parallel instead of being asked to diverge.
    const float EyeSeparationMetres = 0.063f;

    // Upper bound for the intermediate present texture; matches the
    // swapchain capacity in crashxr.c.
    const int PresentMaxWidth = 2048;
    const int PresentMaxHeight = 1536;

    readonly AndroidAudioOutput _audio = new();
    long _lastPresent = System.Diagnostics.Stopwatch.GetTimestamp();
    int _frame;
    double _presentMs, _wideMs;

    /// <summary>ADB debugging: write both eyes as PNGs at this presented frame (0 = never).</summary>
    public int DumpAtFrame { get; set; }
    public string? DumpDirectory { get; set; }
    long _fpsWindow = System.Diagnostics.Stopwatch.GetTimestamp();
    int _fpsFrames;
    bool _firstFrame = true;

    public void Initialize(string title) => Android.Util.Log.Info("CrashXR", $"{title}: starting");
    public void WaitForValidDisc() { }
    public void AttachAudio(Spu? spu) => _audio.Attach(spu);
    public void SetMasterVolume(float volume) => _audio.SetMasterVolume(volume);
    public void ShowNotice(string message) => Android.Util.Log.Info("CrashXR", message);

    public void PauseAudio()
    {
        RecompOne.Runtime.Host.FrameClock.PauseTiming();
        _audio.PauseOutput();
    }

    public void ResumeAudio()
    {
        _audio.ResumeOutput();
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
    }

    public void Present(Gpu? gpu)
    {
        long presentStart = System.Diagnostics.Stopwatch.GetTimestamp();
        try { PresentCore(gpu); }
        finally
        {
            _presentMs += (System.Diagnostics.Stopwatch.GetTimestamp() - presentStart) * 1000.0
                          / System.Diagnostics.Stopwatch.Frequency;
            _wideMs += RecompOne.Runtime.Host.FramePacing.LastNativeWideCpuMs;
        }
    }

    void PresentCore(Gpu? gpu)
    {
        CheatManager.Apply();
        Runtime.RamLog.Tick();

        // Controllers are sampled by the XR thread; unfocused sessions report
        // nothing pressed. Physical gamepads keep their own path.
        QuestXr.Poll(out var pad);
        Controller.SetVirtualPadState(pad.Buttons);
        AdjustConvergence(pad.RightX);

        if (gpu == null || !gpu.DisplayEnabled || !backend.Ready)
            return;
        if (gpu.DisplayWidth <= 0 || gpu.DisplayHeight <= 0)
            return;

        var presented = backend.PresentDisplay(
            gpu.DisplayX, gpu.DisplayY, gpu.DisplayWidth, gpu.DisplayHeight, gpu.Display24Bit,
            PresentMaxWidth, PresentMaxHeight);
        AndroidVramCapture.CaptureFromGlThread(backend);
        // The border hides the strip the right eye cannot fill: at most one
        // separation, in the same display pixels the shift is measured in.
        float pixelsAcross = gpu.DisplayWidth * Math.Max(1f, presented.aspect / GpuHle.BaseAspect);
        QuestXr.Submit(presented.tex, backend.LastRightEyeTexture,
            presented.w, presented.h, presented.aspect,
            GpuHle.StereoSeparation / Math.Max(1f, pixelsAcross));
        UpdateStereo(pixelsAcross);
        if (++_frame == DumpAtFrame && DumpDirectory != null)
            DumpEyes(presented.w, presented.h);

        if (_firstFrame)
        {
            _firstFrame = false;
            Android.Util.Log.Info("CrashXR",
                $"first frame {presented.w}x{presented.h} aspect {presented.aspect:0.###}, scale {GlVram.Scale}x");
        }

        _fpsFrames++;
        var now = System.Diagnostics.Stopwatch.GetTimestamp();
        var elapsed = (now - _fpsWindow) / (double)System.Diagnostics.Stopwatch.Frequency;
        if (elapsed >= 5.0)
        {
            Android.Util.Log.Info("CrashXR",
                $"{_fpsFrames / elapsed:F1} game FPS, display {QuestXr.RefreshRate():0} Hz, " +
                $"present {presented.w}x{presented.h}, stereo {(backend.LastRightEyeTexture != 0 ? "on" : "off")} " +
                $"sep {GpuHle.StereoSeparation:0.0}px conv {GpuHle.StereoConvergenceZ:0}, GL {backend.LastDiagnostic}");
            if (GpuHle.StereoActive)
                Android.Util.Log.Info("CrashXR", "GTE depth log2 buckets: " +
                    string.Join(' ', GpuHle.TakeStereoDepthHistogram()));
            LogCpuSplit(elapsed * 1000.0);
            _fpsFrames = 0;
            _fpsWindow = now;
        }
    }

    // The shift is in display pixels, so it follows the screen size the player
    // picks with the right stick. It applies from the next frame on.
    void UpdateStereo(float pixelsAcross)
    {
        if (!GpuHle.StereoActive) return;
        float metresPerPixel = QuestXr.GetScreenWidth() / Math.Max(1f, pixelsAcross);
        GpuHle.StereoSeparation = depth3D * 0.95f * EyeSeparationMetres / metresPerPixel;
    }

    // Game-thread time per presented frame. "other" is the recompiled PS1
    // code plus GPU command decoding (everything not measured separately).
    void LogCpuSplit(double windowMs)
    {
        int frames = Math.Max(1, _fpsFrames);
        double wait = RecompOne.Runtime.Host.FrameClock.TakeWaitMs();
        var (left, right) = backend.TakeFlushMs();
        double other = windowMs - wait - left - right - _presentMs - _wideMs;
        Android.Util.Log.Info("CrashXR",
            $"game thread per frame: wait {wait / frames:0.0} | GL left {left / frames:0.0} right {right / frames:0.0} | " +
            $"wide pass {_wideMs / frames:0.0} | present {_presentMs / frames:0.0} | other {other / frames:0.0} ms " +
            $"(frame budget {windowMs / frames:0.0})");
        _presentMs = _wideMs = 0;
    }

    // Right stick left/right moves the screen plane: left brings the scene
    // forward (more pops out), right pushes it back. Scaled per second.
    void AdjustConvergence(float stickX)
    {
        var now = System.Diagnostics.Stopwatch.GetTimestamp();
        var seconds = (float)((now - _lastPresent) / (double)System.Diagnostics.Stopwatch.Frequency);
        _lastPresent = now;
        if (!GpuHle.StereoActive || MathF.Abs(stickX) < 0.3f) return;
        var z = GpuHle.StereoConvergenceZ * (1f + stickX * 0.8f * Math.Min(seconds, 0.1f));
        GpuHle.StereoConvergenceZ = Math.Clamp(z, 128f, 16384f);
    }

    void DumpEyes(int width, int height)
    {
        var pixels = new int[width * height];
        foreach (var (eye, right) in new[] { ("left", false), ("right", true) })
        {
            if (!backend.ReadPresentArgb(pixels, right)) continue;
            using var bitmap = Android.Graphics.Bitmap.CreateBitmap(
                pixels, width, height, Android.Graphics.Bitmap.Config.Argb8888!)!;
            var path = Path.Combine(DumpDirectory!, $"stereo_{eye}.png");
            using var file = File.Create(path);
            bitmap.Compress(Android.Graphics.Bitmap.CompressFormat.Png!, 100, file);
            Android.Util.Log.Info("CrashXR", $"wrote {path}");
        }
    }

    public void Shutdown()
    {
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
        _audio.Dispose();
        Controller.SetVirtualPadState(0);
    }
}
