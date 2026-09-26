using Android.App;
using Android.Graphics;
using Android.Views;
using Android.Widget;
using static CrashBandicoot.AndroidRuntime.TouchControlsDialog;

namespace CrashBandicoot.AndroidRuntime.QuestVr;

/// <summary>The launcher's Controls screen on Meta Quest: screen shape and Touch bindings.</summary>
static class QuestControlsDialog
{
    public static void Show(Activity activity)
    {
        var settings = new QuestVrSettings(activity);
        var displayFont = LoadTypeface(activity, "Fonts/Bungee-Regular.ttf", Typeface.DefaultBold!);
        var bodyFont = LoadTypeface(activity, "Fonts/Nunito-SemiBold.ttf", Typeface.Default!);
        var bodyBold = LoadTypeface(activity, "Fonts/Nunito-ExtraBold.ttf", Typeface.DefaultBold!);

        var dialog = new Dialog(activity);
        dialog.RequestWindowFeature((int)WindowFeatures.NoTitle);
        dialog.SetCanceledOnTouchOutside(false);

        var root = new LinearLayout(activity) { Orientation = Orientation.Vertical };
        root.SetPadding(Dp(activity, 22), Dp(activity, 16), Dp(activity, 22), Dp(activity, 16));
        root.Background = RoundedBackground(Card, Color.Argb(170, 255, 176, 32),
            Dp(activity, 1), Dp(activity, 3));

        var card = new LinearLayout(activity) { Orientation = Orientation.Vertical };
        card.AddView(Text(activity, "VR CONTROLS", 19, Wumpa, displayFont));

        var explanation = Text(activity,
            "The left stick is the D-pad. 3D depth separates the eyes by GTE depth; " +
            "set it to 0% for a flat screen. In the headset, push the right stick up or down " +
            "to resize the screen, push it left or right to move the 3D depth plane, and click " +
            "the left stick to switch passthrough on or off. These are remembered; RESET VIEW " +
            "restores the default screen.",
            11, Muted, bodyFont);
        explanation.SetLineSpacing(0, 1.08f);
        card.AddView(explanation, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.WrapContent)
        {
            TopMargin = Dp(activity, 6),
            BottomMargin = Dp(activity, 7),
        });

        var curved = new Switch(activity)
        {
            Text = "Curved screen",
            TextSize = 12,
            Checked = settings.Curved,
        };
        curved.SetTypeface(bodyBold, TypefaceStyle.Normal);
        curved.SetTextColor(Sand);
        curved.CheckedChange += (_, e) => settings.SetCurved(e.IsChecked);
        card.AddView(curved, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MatchParent, Dp(activity, 40)));

        // 0 turns stereo off entirely (and its extra GPU pass).
        AddSlider(activity, card, "3D depth", 0, 100, settings.Depth3D,
            settings.SetDepth3D, bodyFont, bodyBold);

        var labels = QuestVrSettings.PadButtons.Select(button => button.Label).ToArray();
        for (var input = 0; input < QuestVrSettings.Inputs.Length; input++)
            AddBindingRow(activity, card, settings, input, labels, bodyBold);

        var scroll = new ScrollView(activity);
        scroll.AddView(card);
        root.AddView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MatchParent, 0, 1f));

        var actions = new LinearLayout(activity) { Orientation = Orientation.Horizontal };
        actions.SetGravity(GravityFlags.Center);
        var resetView = Button(activity, "RESET VIEW", bodyBold);
        var reset = Button(activity, "RESET", bodyBold);
        var back = Button(activity, "BACK", bodyBold);
        actions.AddView(resetView, WeightedButtonLayout(activity));
        actions.AddView(reset, WeightedButtonLayout(activity));
        actions.AddView(back, WeightedButtonLayout(activity));
        root.AddView(actions, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MatchParent, Dp(activity, 40))
        {
            TopMargin = Dp(activity, 7),
        });

        reset.Click += (_, _) =>
        {
            settings.ResetBindings();  // also restores the default 3D depth
            settings.SetCurved(true);
            dialog.Dismiss();
            Show(activity);
        };
        resetView.Click += (_, _) =>
        {
            settings.RequestViewReset();
            Toast.MakeText(activity, "Screen size and 3D depth plane reset for the next game.",
                ToastLength.Short)?.Show();
        };
        back.Click += (_, _) => dialog.Dismiss();

        dialog.SetContentView(root);
        dialog.DismissEvent += (_, _) => dialog.Dispose();
        AndroidGamepad.BindDialog(dialog);
        dialog.Show();
        ConfigureWindow(dialog, activity, 0.68f,
            (int)((activity.Resources?.DisplayMetrics?.HeightPixels ?? 900) * 0.86f), 0.76f);
    }

    static void AddBindingRow(Activity activity, LinearLayout parent, QuestVrSettings settings,
        int input, string[] labels, Typeface bodyBold)
    {
        var row = new LinearLayout(activity) { Orientation = Orientation.Horizontal };
        row.SetGravity(GravityFlags.CenterVertical);
        row.AddView(Text(activity, QuestVrSettings.Inputs[input].Label, 12, Sand, bodyBold),
            new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WrapContent, 1f));

        var picker = new Spinner(activity);
        var adapter = new ArrayAdapter<string>(activity,
            Android.Resource.Layout.SimpleSpinnerItem, labels);
        adapter.SetDropDownViewResource(Android.Resource.Layout.SimpleSpinnerDropDownItem);
        picker.Adapter = adapter;

        var current = settings.Binding(input);
        var selected = Array.FindIndex(QuestVrSettings.PadButtons, button => button.Bits == current);
        picker.SetSelection(Math.Max(0, selected), false);
        picker.ItemSelected += (_, e) =>
            settings.SetBinding(input, QuestVrSettings.PadButtons[e.Position].Bits);

        row.AddView(picker, new LinearLayout.LayoutParams(Dp(activity, 150), Dp(activity, 38)));
        parent.AddView(row, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MatchParent, ViewGroup.LayoutParams.WrapContent));
    }
}
