using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.OS;
using Android.Views;
using CrashBandicoot.Launcher.Recomp;
using RecompOne.Runtime;
using RecompOne.Runtime.Config;

namespace CrashBandicoot.AndroidRuntime.QuestVr;

/// <summary>
/// Immersive Quest activity. The 2D launcher prepares the game, then starts
/// this activity, which runs the PS1 core and shows it on a virtual screen.
/// It lives in its own process so every session starts from a clean runtime
/// and leaving VR simply ends that process.
/// </summary>
[Activity(
    Label = "Crash Bandicoot VR",
    Process = ":vr",
    TaskAffinity = "io.github.matteo842.crashlauncher.runtime.vr",
    Exported = true,
    LaunchMode = LaunchMode.SingleTask,
    ExcludeFromRecents = true,
    ScreenOrientation = ScreenOrientation.Landscape,
    Theme = "@android:style/Theme.Black.NoTitleBar.Fullscreen",
    ConfigurationChanges = ConfigChanges.Density | ConfigChanges.FontScale |
                           ConfigChanges.Keyboard | ConfigChanges.KeyboardHidden |
                           ConfigChanges.LayoutDirection | ConfigChanges.Locale |
                           ConfigChanges.Navigation | ConfigChanges.Orientation |
                           ConfigChanges.ScreenLayout | ConfigChanges.ScreenSize |
                           ConfigChanges.SmallestScreenSize | ConfigChanges.Touchscreen |
                           ConfigChanges.UiMode)]
[IntentFilter(
    new[] { Intent.ActionMain },
    // Each vendor ignores the other's category: Meta Quest, Pico.
    Categories = new[] { "com.oculus.intent.category.VR", "com.picovr.intent.category.VRAPP" })]
public sealed class VrGameActivity : Activity
{
    public const string ExtraGameDll = "game_dll";
    public const string ExtraCuePath = "cue_path";

    VrPlatformHost? _host;
    QuestVrSettings _settings = null!;
    volatile bool _closing;
    bool _pausedByLifecycle;
    bool _pausedByFocus;

    public static void Launch(Activity from, string gameDll, string cuePath)
    {
        var intent = new Intent(from, typeof(VrGameActivity));
        // ADB debug extras given to the launcher (boot_level, stereo_*) carry over.
        if (from.Intent?.Extras is { } extras)
            intent.PutExtras(extras);
        intent.PutExtra(ExtraGameDll, gameDll);
        intent.PutExtra(ExtraCuePath, cuePath);
        intent.AddFlags(ActivityFlags.NewTask);
        from.StartActivity(intent);
    }

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        Window?.AddFlags(WindowManagerFlags.KeepScreenOn | WindowManagerFlags.Fullscreen);

        var gameDll = Intent?.GetStringExtra(ExtraGameDll);
        var cuePath = Intent?.GetStringExtra(ExtraCuePath);
        if (string.IsNullOrEmpty(gameDll) || string.IsNullOrEmpty(cuePath) || !File.Exists(gameDll))
        {
            Android.Util.Log.Error("CrashXR", "VR activity started without a prepared game.");
            Finish();
            return;
        }

        AppPaths.SetRoot(Path.Combine(FilesDir!.AbsolutePath, "runtime"));
        AppPaths.EnsureCreated();
        ConfigManager.Load();
        ConfigManager.Game.CdPath = cuePath;

        _settings = new QuestVrSettings(this);
        AndroidGamepad.Attach(this);
        AndroidGamepad.ResolveRoute = () => GamepadRoute.Gameplay;

        var gameThread = new Thread(() => RunGame(gameDll, cuePath), 32 * 1024 * 1024)
        {
            IsBackground = true,
            Name = "PS1 Game Core",
            Priority = System.Threading.ThreadPriority.AboveNormal,
        };
        gameThread.Start();

        new Thread(WatchSession) { IsBackground = true, Name = "XR session watch" }.Start();
    }

    void RunGame(string gameDll, string cuePath)
    {
        VrEglContext? egl = null;
        Silk.NET.OpenGL.GL? gl = null;
        RecompOne.Runtime.Hle.GlBackend? backend = null;
        GameGpuDiagnosticsSession? diagnostics = null;
        try
        {
            egl = new VrEglContext();
            gl = Silk.NET.OpenGL.GL.GetApi(egl);
            AndroidGraphics.ApplyLive();
            RecompOne.Runtime.Hle.GlVram.Scale = ConfigManager.View.InternalResolution;
            // ADB tuning: --ei depth_3d <0..100> overrides the launcher setting.
            var depth3D = (Intent?.GetIntExtra("depth_3d", -1) is int forced and >= 0
                ? Math.Min(forced, 100) : _settings.Depth3D) / 100f;
            RecompOne.Runtime.Hle.GpuHle.StereoActive = depth3D > 0f;
            // ADB tuning: --ef stereo_convergence <GTE Z on the screen plane>.
            var convergence = Intent?.GetFloatExtra("stereo_convergence", 0f) ?? 0f;
            if (convergence <= 0f)
                convergence = _settings.Convergence;
            if (convergence > 0f)
                RecompOne.Runtime.Hle.GpuHle.StereoConvergenceZ = convergence;
            backend = new RecompOne.Runtime.Hle.GlBackend(gl);
            var gpuInfo = AndroidGlesInfo.Capture(gl, egl);
            backend.InitGl(gles: true, framebufferFetch: gpuInfo.FramebufferFetchPath);
            if (!backend.Ready)
                throw new InvalidOperationException("The OpenGL ES renderer failed to initialize.");
            var configured = gpuInfo.ConfigureBackend(backend, ConfigManager.View.InternalResolution);
            backend.WarmGpuPipelines();
            Android.Util.Log.Info("CrashXR",
                $"GPU {gpuInfo.Vendor} / {gpuInfo.Renderer}; framebuffer fetch {gpuInfo.FramebufferFetchLabel}; " +
                $"scale {ConfigManager.View.InternalResolution}x");
            diagnostics = new GameGpuDiagnosticsSession(
                this, gpuInfo, ConfigManager.View.InternalResolution,
                configured.textureBarrier, configured.coarseShading);

            var javaVm = Java.Interop.JniEnvironment.Runtime.InvocationPointer;
            if (QuestXr.Start(javaVm, Handle) != 0)
                throw new InvalidOperationException("OpenXR could not be started.");
            _settings.Apply();

            RecompOne.Runtime.Hle.GpuHle.Backend = backend;
            RecompOne.Runtime.Hle.GpuHle.Active = true;
            RecompOne.Runtime.Hle.GpuHle.NativeResolution = ConfigManager.View.InternalResolution <= 1;
            RecompOne.Runtime.Host.FrameClock.SleepWhileWaiting = true;
            _host = new VrPlatformHost(backend, depth3D);
            // ADB debugging: --ei boot_level <id> --ei stereo_dump_frame <n>
            //                --es stereo_dump_dir <folder the app can write>
            var bootLevel = Intent?.GetIntExtra("boot_level", 0) ?? 0;
            if (bootLevel > 0)
                RecompOne.Runtime.Host.FramePacing.BootLevelOverride = (uint)bootLevel;
            _host.DumpAtFrame = Intent?.GetIntExtra("stereo_dump_frame", 0) ?? 0;
            _host.DumpDirectory = Intent?.GetStringExtra("stereo_dump_dir");
            RecompOne.Runtime.Runtime.SetPlatformHost(_host);
            GameLoader.Run(gameDll, cuePath);
        }
        catch (Exception ex)
        {
            Android.Util.Log.Error("CrashXR", ex.ToString());
            File.WriteAllText(Path.Combine(AppPaths.Root, "android-last-error.txt"), ex.ToString());
        }
        finally
        {
            diagnostics?.Complete();
            RecompOne.Runtime.Runtime.SetPlatformHost(null);
            _host?.Shutdown();
            QuestXr.Stop();
            RecompOne.Runtime.Hle.GpuHle.Active = false;
            RecompOne.Runtime.Hle.GpuHle.Backend = null;
            backend?.Dispose();
            gl?.Dispose();
            egl?.Dispose();
            Close();
        }
    }

    // The XR thread reports session state; pause the core when the player
    // opens the system menu or takes the headset off, and leave when the
    // runtime asks the app to quit.
    void WatchSession()
    {
        var wasFocused = false;
        while (!_closing)
        {
            Thread.Sleep(50);
            var status = QuestXr.Poll(out _);
            if ((status & QuestXr.Status.Exit) != 0)
            {
                Android.Util.Log.Info("CrashXR",
                    (status & QuestXr.Status.Failed) != 0 ? "OpenXR failed; closing." : "OpenXR session ended.");
                Close();
                return;
            }

            var focused = (status & QuestXr.Status.Focused) != 0;
            if (focused != wasFocused && _host != null)
            {
                wasFocused = focused;
                RunOnUiThread(() =>
                {
                    _pausedByFocus = !focused;
                    SyncPause();
                });

                // Remember screen size and passthrough as the player left them.
                if (!focused)
                    SaveSession();
            }
        }
    }

    void SaveSession() => _settings.SaveSession(QuestXr.GetScreenWidth(), QuestXr.Passthrough,
        RecompOne.Runtime.Hle.GpuHle.StereoConvergenceZ);

    void SyncPause()
    {
        if (_host == null) return;
        if (_pausedByLifecycle || _pausedByFocus) _host.PauseAudio();
        else _host.ResumeAudio();
    }

    void Close()
    {
        if (_closing) return;
        _closing = true;
        if (_host != null)
            SaveSession();
        RunOnUiThread(() =>
        {
            FinishAndRemoveTask();
            // Runtime state is process-wide and not built to restart in place.
            Process.KillProcess(Process.MyPid());
        });
    }

    public override bool DispatchKeyEvent(KeyEvent? e)
    {
        if (e != null && AndroidGamepad.TryHandleKey(e))
            return true;
        return base.DispatchKeyEvent(e);
    }

    public override bool DispatchGenericMotionEvent(MotionEvent? e)
    {
        if (e != null && AndroidGamepad.TryHandleMotion(e))
            return true;
        return base.DispatchGenericMotionEvent(e);
    }

    protected override void OnPause()
    {
        _pausedByLifecycle = true;
        AndroidGamepad.ReleaseHeld();
        SyncPause();
        base.OnPause();
    }

    protected override void OnResume()
    {
        base.OnResume();
        _pausedByLifecycle = false;
        SyncPause();
    }

    protected override void OnDestroy()
    {
        AndroidGamepad.Detach();
        base.OnDestroy();
        if (!_closing)
            Close();
    }
}
