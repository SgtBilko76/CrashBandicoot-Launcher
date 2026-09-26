using Android.Content;
using Controller = RecompOne.Runtime.Hardware.Controller;

namespace CrashBandicoot.AndroidRuntime.QuestVr;

/// <summary>
/// Quest options. The launcher and the VR game run in separate processes and
/// SharedPreferences caches a whole file per process, so each file has one
/// writer: the launcher owns the layout and bindings, the VR process owns
/// what the player adjusts inside the headset.
/// </summary>
sealed class QuestVrSettings
{
    const string LauncherFile = "quest_vr";
    const string SessionFile = "quest_vr_session";

    /// <summary>Touch inputs in cxr_set_binding order (crashxr.c INPUT_*).</summary>
    public static readonly (string Key, string Label, ushort Default)[] Inputs =
    [
        ("a", "A button", Controller.Cross),
        ("b", "B button", Controller.Circle),
        ("x", "X button", Controller.Square),
        ("y", "Y button", Controller.Triangle),
        ("right_trigger", "Right trigger", Controller.R1),
        ("left_trigger", "Left trigger", Controller.L1),
        ("right_grip", "Right grip", Controller.R2),
        ("left_grip", "Left grip", Controller.L2),
        ("menu", "Menu button", Controller.Start),
        ("right_click", "Right stick click", Controller.Select),
    ];

    public static readonly (string Label, ushort Bits)[] PadButtons =
    [
        ("None", 0),
        ("Cross", Controller.Cross),
        ("Circle", Controller.Circle),
        ("Square", Controller.Square),
        ("Triangle", Controller.Triangle),
        ("L1", Controller.L1),
        ("R1", Controller.R1),
        ("L2", Controller.L2),
        ("R2", Controller.R2),
        ("Start", Controller.Start),
        ("Select", Controller.Select),
    ];

    readonly ISharedPreferences _launcher;
    readonly ISharedPreferences _session;

    public QuestVrSettings(Context context)
    {
        _launcher = context.GetSharedPreferences(LauncherFile, FileCreationMode.Private)!;
        _session = context.GetSharedPreferences(SessionFile, FileCreationMode.Private)!;
    }

    // --- launcher-owned

    public bool Curved => _launcher.GetBoolean("curved", true);

    public void SetCurved(bool curved) =>
        _launcher.Edit()!.PutBoolean("curved", curved)!.Commit();

    /// <summary>Stereo 3D strength, 0 (flat) to 100 (eyes parallel at infinity).</summary>
    public int Depth3D => _launcher.GetInt("depth_3d", 50);

    public void SetDepth3D(int percent) =>
        _launcher.Edit()!.PutInt("depth_3d", Math.Clamp(percent, 0, 100))!.Commit();

    public ushort Binding(int input) =>
        (ushort)_launcher.GetInt("bind_" + Inputs[input].Key, Inputs[input].Default);

    public void SetBinding(int input, ushort buttons) =>
        _launcher.Edit()!.PutInt("bind_" + Inputs[input].Key, buttons)!.Commit();

    public void ResetBindings()
    {
        var edit = _launcher.Edit()!;
        foreach (var input in Inputs)
            edit.Remove("bind_" + input.Key);
        edit.Remove("depth_3d");
        edit.Commit();
    }

    // --- VR-process-owned

    // "_v2": the first builds saved sizes for a screen 2.5 m away; the default
    // is now 1.8 m, so those are dropped once.
    const string ScreenWidthKey = "screen_width_v2";

    /// <summary>Screen width the player set with the right stick; 0 = default.
    /// Ignored after the launcher's RESET VIEW, until the player changes it again.</summary>
    public float ScreenWidth => ViewResetPending ? 0 : _session.GetFloat(ScreenWidthKey, 0);
    public bool Passthrough => _session.GetBoolean("passthrough", false);

    /// <summary>Stereo screen plane (GTE Z) the player set with the right stick; 0 = default.</summary>
    public float Convergence => ViewResetPending ? 0 : _session.GetFloat("convergence", 0);

    // RESET VIEW crosses processes without either side writing the other's
    // file: the launcher bumps a stamp in its file, and the VR process
    // records which stamp its saved view already accounts for.
    bool ViewResetPending =>
        _launcher.GetLong("view_reset", 0) != _session.GetLong("view_reset_applied", 0);

    public void RequestViewReset() =>
        _launcher.Edit()!.PutLong("view_reset", Java.Lang.JavaSystem.CurrentTimeMillis())!.Commit();

    public void SaveSession(float screenWidth, bool passthrough, float convergence) =>
        _session.Edit()!
            .PutFloat(ScreenWidthKey, screenWidth)!
            .PutBoolean("passthrough", passthrough)!
            .PutFloat("convergence", convergence)!
            .PutLong("view_reset_applied", _launcher.GetLong("view_reset", 0))!
            .Commit();

    /// <summary>Hands everything to libcrashxr.so; call after QuestXr.Start.</summary>
    public void Apply()
    {
        for (var i = 0; i < Inputs.Length; i++)
            QuestXr.SetBinding(i, Binding(i));
        QuestXr.SetView(Curved ? 1 : 0, Passthrough ? 1 : 0);
        if (ScreenWidth > 0)
            QuestXr.SetScreen(ScreenWidth, 0);
    }
}
