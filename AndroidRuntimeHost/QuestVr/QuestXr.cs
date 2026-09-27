using System.Runtime.InteropServices;
using Android.Content;
using Android.Content.PM;
using Silk.NET.Core.Contexts;

namespace CrashBandicoot.AndroidRuntime.QuestVr;

/// <summary>
/// True on standalone VR headsets (Meta Quest, Pico), where the game runs in
/// OpenXR instead of the phone's TextureView.
/// </summary>
static class QuestDevice
{
    public static bool IsVrHeadset(Context context) =>
        context.PackageManager?.HasSystemFeature("oculus.hardware.standalone_vr") == true ||
        Android.OS.Build.Manufacturer is { } maker &&
        (maker.Equals("Oculus", StringComparison.OrdinalIgnoreCase) ||
         maker.Equals("Meta", StringComparison.OrdinalIgnoreCase) ||
         maker.StartsWith("Pico", StringComparison.OrdinalIgnoreCase));
}

/// <summary>Bindings to libcrashxr.so (QuestVr/Native/crashxr.c).</summary>
static class QuestXr
{
    const string Library = "crashxr";

    [Flags]
    public enum Status
    {
        Running = 1 << 0,
        Visible = 1 << 1,
        Focused = 1 << 2,
        Exit = 1 << 3,
        Failed = 1 << 4,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Pad
    {
        public ushort Buttons;
        public float LeftX, LeftY;
        public float RightX, RightY;
    }

    [DllImport(Library, EntryPoint = "cxr_game_context_create")]
    public static extern int GameContextCreate();

    [DllImport(Library, EntryPoint = "cxr_game_context_destroy")]
    public static extern void GameContextDestroy();

    [DllImport(Library, EntryPoint = "cxr_start")]
    public static extern int Start(nint javaVm, nint activity);

    [DllImport(Library, EntryPoint = "cxr_stop")]
    public static extern void Stop();

    [DllImport(Library, EntryPoint = "cxr_submit")]
    public static extern void Submit(uint texture, uint textureRight, int width, int height, float aspect,
        float edgeMask);

    [DllImport(Library, EntryPoint = "cxr_poll")]
    static extern int PollNative(out Pad pad);

    [DllImport(Library, EntryPoint = "cxr_get_screen_width")]
    public static extern float GetScreenWidth();

    [DllImport(Library, EntryPoint = "cxr_set_screen")]
    public static extern void SetScreen(float width, float distance);

    [DllImport(Library, EntryPoint = "cxr_refresh_rate")]
    public static extern float RefreshRate();

    [DllImport(Library, EntryPoint = "cxr_set_binding")]
    public static extern void SetBinding(int input, int buttons);

    [DllImport(Library, EntryPoint = "cxr_set_view")]
    public static extern void SetView(int curved, int passthrough);

    [DllImport(Library, EntryPoint = "cxr_get_passthrough")]
    static extern int GetPassthroughNative();

    public static bool Passthrough => GetPassthroughNative() != 0;

    public static Status Poll(out Pad pad) => (Status)PollNative(out pad);
}

/// <summary>
/// The window-less GLES context the PS1 renderer draws with on Quest. The
/// XR thread in libcrashxr.so shares its objects.
/// </summary>
sealed class VrEglContext : INativeContext
{
    nint _glesLibrary;
    bool _disposed;

    public VrEglContext()
    {
        var result = QuestXr.GameContextCreate();
        if (result != 0)
            throw new InvalidOperationException($"The Quest GLES context failed to initialize ({result}).");
        NativeLibrary.TryLoad("libGLESv3.so", out _glesLibrary);
    }

    public nint GetProcAddress(string proc, int? slot = null)
    {
        var address = EglGetProcAddress(proc);
        if (address == 0 && _glesLibrary != 0)
            NativeLibrary.TryGetExport(_glesLibrary, proc, out address);
        return address;
    }

    public bool TryGetProcAddress(string proc, out nint addr, int? slot = null)
    {
        addr = GetProcAddress(proc, slot);
        return addr != 0;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        QuestXr.GameContextDestroy();
        if (_glesLibrary != 0) NativeLibrary.Free(_glesLibrary);
        _glesLibrary = 0;
    }

    [DllImport("libEGL.so", EntryPoint = "eglGetProcAddress")]
    static extern nint EglGetProcAddress([MarshalAs(UnmanagedType.LPUTF8Str)] string procname);
}
