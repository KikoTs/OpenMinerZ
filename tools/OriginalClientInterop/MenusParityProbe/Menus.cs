using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Input;

// Presentation probe for the FRONT END and the in-game menu.
//
// It runs the shipped menu screens of the owned, unmodified Castle Miner Z
// assemblies offline - MainMenu over MenuBackdropScreen, GameModeMenu,
// DifficultyLevelScreen, 1.6.3.8's OptionsMenu and SettingsMenu, InGameMenu -
// through the game's own ScreenGroup, ProcessInput, Update and DNAGame.Draw,
// against the hidden device of the screens probe (Program.cs and Recorder.cs
// are that probe's, compiled into this one).
//
// Out, under the output directory (never in git):
//   draws/<build>.<scenario>.<W>x<H>.txt   every quad and string, in order
//   frames/<build>.<scenario>.<W>x<H>.png  the composed back buffer
//   fonts.<build>.txt                      line spacing and sample measurements
//
// OpenMinerZ's menus are its own, so nothing compares against these records
// line by line; they are the reference the menu style in src/app.cpp was taken
// from (fonts, sizes, colours, the focus pulse, alignment and spacing).
//
// What is NOT original code: the game object has no window (IsFullScreen is
// answered instead of asked of a GraphicsDeviceManager), the signed-in gamer is
// a blank one named "Player", and sound cues are logged instead of played.
internal static partial class Program
{
    private static object backdropScreen, menuGroup;
    private static bool frontEndReady;

    public static bool FullScreenGetPrefix(ref bool __result) { __result = false; return false; }
    public static bool SkipPrefix() { return false; }
    public static object Gamer;
    public static bool CurrentGamerPrefix(ref object __result) { __result = Gamer; return false; }

    private static void PrepareFrontEnd()
    {
        if (frontEndReady) return;
        frontEndReady = true;
        var harmony = new Harmony("openminerz.menus-parity-probe");
        PropertyInfo fullScreen = GameType.GetProperty("IsFullScreen");
        harmony.Patch(fullScreen.GetGetMethod(), new HarmonyMethod(typeof(Program).GetMethod("FullScreenGetPrefix")));
        harmony.Patch(fullScreen.GetSetMethod(), new HarmonyMethod(typeof(Program).GetMethod("SkipPrefix")));
        Type screenType = T("DNA.Drawing.UI.Screen");
        harmony.Patch(screenType.GetProperty("CurrentGamer").GetGetMethod(),
                      new HarmonyMethod(typeof(Program).GetMethod("CurrentGamerPrefix")));
        Gamer = R.Blank(T("DNA.Net.GamerServices.SignedInGamer"));
        R.Set(Gamer, "Gamertag", "Player");
        // No audio device: the settings screen's volume write goes nowhere.
        FieldInfo music = GameType.GetField("MusicSounds", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (music != null)
            harmony.Patch(AccessTools.Method(music.FieldType, "SetVolume"),
                          new HarmonyMethod(typeof(Program).GetMethod("SkipPrefix")));
        // The key bindings a started game has (the Controls screen lists them).
        FieldInfo mapping = GameType.GetField("_controllerMapping", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (mapping != null && mapping.GetValue(GameObject) == null)
        {
            object bindings = R.New(mapping.FieldType);
            R.Call(bindings, "SetToDefault");
            mapping.SetValue(GameObject, bindings);
        }

        var backdrop = Content.Load<Texture2D>(Modern ? "UI\\Screens\\MenuBack" : "MenuBack");
        R.Set(GameObject, "MenuBackdrop", backdrop);
        Recorder.RegisterTexture(backdrop, "@menuback");
        object sprites = R.Get(GameObject, "_uiSprites");
        MethodInfo sprite = sprites.GetType().GetProperty("Item").GetGetMethod();
        R.Set(GameObject, "Logo", sprite.Invoke(sprites, new object[] { "Logo" }));
        if (Modern)
        {
            foreach (var pair in new[] { new[] { "CMZREAd", "UI\\CMZREAd" }, new[] { "CMZREAdSel", "UI\\CMZREAdSelected" } })
            {
                var texture = Content.Load<Texture2D>(pair[1]);
                R.Set(GameObject, pair[0], texture);
                Recorder.RegisterTexture(texture, "@advert");
            }
        }
        Type settings = T("DNA.CastleMinerZ.GlobalSettings");
        if (R.GetStatic(GameType, "GlobalSettings") == null) R.SetStatic(GameType, "GlobalSettings", R.Blank(settings));
        // Dragon Endurance reads as unlocked, as it is for a finished save.
        try { R.Set(R.Get(GameObject, "PlayerStats"), "UndeadDragonKills", 1); }
        catch (Exception error) { Console.Error.WriteLine("note: UndeadDragonKills: " + error.Message); }
        object gameScreen = R.Get(GameObject, "GameScreen");
        FieldInfo scene = gameScreen.GetType().GetField("mainScene", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (scene != null && scene.GetValue(gameScreen) == null) scene.SetValue(gameScreen, R.Blank(scene.FieldType));
    }

    // A front end as FrontEndScreen composes it: the backdrop, then a group
    // that draws behind itself holding the menu.
    private static void NewFrontEnd(object screen, bool inGame)
    {
        Type screenGroup = T("DNA.Drawing.UI.ScreenGroup");
        ScreenManager = R.New(screenGroup, false);
        R.Set(GameObject, "_screenManager", ScreenManager);
        if (inGame)
        {
            // Stands in for the world the real game draws behind this menu.
            R.Set(ScreenManager, "BackgroundColor", (Color?)new Color(58, 92, 122));
        }
        else
        {
            backdropScreen = R.New(T("DNA.CastleMinerZ.UI.MenuBackdropScreen"), GameObject);
            R.Call(ScreenManager, "PushScreen", backdropScreen);
        }
        menuGroup = R.New(screenGroup, true);
        R.Call(ScreenManager, "PushScreen", menuGroup);
        R.Call(menuGroup, "PushScreen", screen);
        ResetInput();
        Recorder.Events.Clear();
        Frame(input: false);
    }

    private sealed class Menu
    {
        public string Name;
        public Func<object> Make;
        public bool InGame;
        public string[] Actions;
    }

    private static object Ui(string name, params object[] arguments)
    {
        return R.New(T("DNA.CastleMinerZ.UI." + name), arguments);
    }

    private static IEnumerable<Menu> Menus()
    {
        yield return new Menu { Name = "main", Make = () => Ui("MainMenu", GameObject) };
        yield return new Menu { Name = "main.down2", Make = () => Ui("MainMenu", GameObject), Actions = new[] { "key Down", "key Down" } };
        // The focus pulse: 0.25 s from the item colour to the selected colour and back.
        yield return new Menu { Name = "main.pulse_0125", Make = () => Ui("MainMenu", GameObject), Actions = new[] { "frames 7" } };
        yield return new Menu { Name = "main.pulse_0250", Make = () => Ui("MainMenu", GameObject), Actions = new[] { "frames 14" } };
        yield return new Menu { Name = "gamemode", Make = () => Ui("GameModeMenu", GameObject) };
        yield return new Menu { Name = "gamemode.survival", Make = () => Ui("GameModeMenu", GameObject), Actions = new[] { "key Down" } };
        yield return new Menu { Name = "difficulty", Make = () => Ui("DifficultyLevelScreen", GameObject) };
        yield return new Menu { Name = "ingame", Make = () => Ui("InGameMenu", GameObject), InGame = true };
        yield return new Menu { Name = "ingame.down3", Make = () => Ui("InGameMenu", GameObject), InGame = true, Actions = new[] { "key Down", "key Down", "key Down" } };
        if (!Modern)
        {
            yield return new Menu { Name = "options", Make = () => R.New(T("DNA.CastleMinerZ.UI.OptionsMenu"), GameObject, menuGroupForOptions(), R.Get(GameObject, "SpriteBatch")) };
            yield return new Menu { Name = "settings", Make = () => Ui("SettingsMenu", GameObject) };
            yield return new Menu { Name = "settings.down3", Make = () => Ui("SettingsMenu", GameObject), Actions = new[] { "key Down", "key Down", "key Down" } };
        }
        else
        {
            yield return new Menu { Name = "options", Make = () => Ui("OptionsScreen", false, menuGroupForOptions()) };
        }
    }

    private static object menuGroupForOptions()
    {
        return R.New(T("DNA.Drawing.UI.ScreenGroup"), true);
    }

    private static void MenuAct(string action)
    {
        string[] words = action.Split(' ');
        if (words[0] == "key")
        {
            var key = (Keys)Enum.Parse(typeof(Keys), words[1]);
            Key(key, true); CommitInput(); Frame();
            Key(key, false); CommitInput(); Frame();
        }
        else if (words[0] == "frames")
        {
            for (int frame = int.Parse(words[1], Invariant); frame > 0; --frame) { CommitInput(); Frame(); }
        }
        else throw new ArgumentException(action);
    }

    private static void RunScenarios(string filter)
    {
        PrepareFrontEnd();
        string prefix = Modern ? "modern." : "classic.";
        string suffix = "." + Width + "x" + Height;
        foreach (Menu menu in Menus())
        {
            if (filter != null && menu.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
            try
            {
                NewFrontEnd(menu.Make(), menu.InGame);
                foreach (string action in menu.Actions ?? new string[0]) MenuAct(action);
            }
            catch (Exception error)
            {
                // A screen that needs more of the running game than the probe has
                // is reported and left out; the others are still recorded.
                Console.Error.WriteLine("note: " + prefix + menu.Name + " not recorded: " + error.GetType().Name + ": " + error.Message);
                if (Environment.GetEnvironmentVariable("OPENMINERZ_PROBE_TRACE") != null) Console.Error.WriteLine(error);
                continue;
            }
            var lines = new List<string> { "# " + prefix + menu.Name + " " + Width + "x" + Height };
            foreach (string action in menu.Actions ?? new string[0]) lines.Add("# " + action);
            lines.AddRange(Recorder.Lines);
            File.WriteAllLines(Path.Combine(OutputDirectory, "draws", prefix + menu.Name + suffix + ".txt"), lines);
            SaveFrame(Path.Combine(OutputDirectory, "frames", prefix + menu.Name + suffix + ".png"));
        }
    }

    private static void RunInteractions() { }

    private static void DumpTables()
    {
        string generation = Modern ? "modern" : "classic";
        var fonts = new List<string>();
        Type strings = T("DNA.CastleMinerZ.Globalization.Strings");
        var samples = new List<string> { "Ag", "<", ">", " " };
        foreach (string name in new[] { "Host_Online", "Join_Online", "Play_Offline", "Options", "Exit", "Return_To_Game",
                                        "Main_Menu", "Choose_a_Game_Mode", "Choose_a_Difficulty_Level", "Back", "Settings" })
        {
            PropertyInfo property = strings.GetProperty(name, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            if (property == null) continue;
            string value = (string)property.GetValue(null, null);
            fonts.Add("string " + name + " " + Recorder.Quote(value));
            samples.Add(value);
        }
        foreach (string field in new[] { "_smallFont", "_medFont", "_medLargeFont", "_largeFont", "_myriadSmall", "_myriadMed", "_myriadLarge", "_consoleFont", "_systemFont", "_nameTagFont" })
        {
            var font = (SpriteFont)R.Get(GameObject, field);
            fonts.Add("font " + field + " line=" + font.LineSpacing + " spacing=" + Recorder.F(font.Spacing) + " glyphs=" + font.Characters.Count);
            // The glyph sheet, for looking at (which typeface each font is).
            try
            {
                var sheet = (Texture2D)R.Get(font, "textureValue");
                Directory.CreateDirectory(Path.Combine(OutputDirectory, "fonts"));
                using (FileStream stream = File.Create(Path.Combine(OutputDirectory, "fonts", generation + "." + field.TrimStart('_') + ".png")))
                    sheet.SaveAsPng(stream, sheet.Width, sheet.Height);
            }
            catch (Exception error) { Console.Error.WriteLine("note: " + field + " sheet not saved: " + error.Message); }
            foreach (string sample in samples)
            {
                Vector2 size;
                try { size = font.MeasureString(sample); } catch (ArgumentException) { continue; }
                fonts.Add("measure " + field + " " + Recorder.F(size.X) + " " + Recorder.F(size.Y) + " " + Recorder.Quote(sample));
            }
        }
        File.WriteAllLines(Path.Combine(OutputDirectory, "fonts." + generation + ".txt"), fonts);
    }
}
