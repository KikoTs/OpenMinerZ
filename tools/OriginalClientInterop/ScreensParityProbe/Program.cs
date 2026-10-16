using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;
using System.Threading;
using HarmonyLib;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Input;

// Presentation probe for the inventory, crate and crafting screens.
//
// It loads the user's owned, unmodified Castle Miner Z assemblies by path and
// runs the SHIPPED screens offline: 1.6.3.8's BlockPickerScreen,
// CraftingUIScreen and CrateScreen, 1.9.9.8's CraftingScreen and CrateScreen.
// The screens are constructed by their own constructors, pushed on the game's
// own ScreenGroup and driven through the game's own ProcessInput / Update /
// Draw, against a hidden graphics device of the requested back-buffer size.
//
// Three things come out, all under the output directory (never in git):
//   draws/<scenario>.txt   every quad and string the screen submitted, in order
//   frames/<scenario>.png  the composed back buffer
//   interaction.txt        the inventory after each step of every input script
// plus sprites.txt / fonts.txt / items.txt (sheet rectangles, font metrics and
// the item table the screens read).
//
// Nothing here starts Steam, opens a session or runs the game loop. What is
// NOT original code, and nothing else:
//   * The game object is created without its constructor (no window, no Steam,
//     no audio); the fields the screens read are filled in from the shipped
//     content by the same Content.Load calls SecondaryLoad makes.
//   * Sound cues, pickups, crate messages and the HUD's own stat panels are
//     intercepted at the call and logged instead of executed. A crate message
//     is applied to the crate at once, which is what the sender's own
//     ItemCrateMessage handler does when the message comes back to it.
//   * Mouse and keyboard states are written into the game's InputManager
//     instead of being read from the devices.
// Without a graphics device the probe exits 77 (skip).
internal static class R
{
    private const BindingFlags All = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public |
                                     BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

    private static MemberInfo Find(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, All);
            if (field != null) return field;
            PropertyInfo property = current.GetProperty(name, All);
            if (property != null) return property;
        }
        throw new MissingMemberException(type.FullName, name);
    }

    public static bool Has(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
            if (current.GetField(name, All) != null || current.GetProperty(name, All) != null) return true;
        return false;
    }

    public static object Get(object target, string name) { return Read(target.GetType(), target, name); }
    public static object GetStatic(Type type, string name) { return Read(type, null, name); }
    public static void Set(object target, string name, object value) { Write(target.GetType(), target, name, value); }
    public static void SetStatic(Type type, string name, object value) { Write(type, null, name, value); }

    private static object Read(Type type, object target, string name)
    {
        MemberInfo member = Find(type, name);
        FieldInfo field = member as FieldInfo;
        return field != null ? field.GetValue(target) : ((PropertyInfo)member).GetValue(target, null);
    }

    private static void Write(Type type, object target, string name, object value)
    {
        MemberInfo member = Find(type, name);
        FieldInfo field = member as FieldInfo;
        if (field != null) field.SetValue(target, value);
        else ((PropertyInfo)member).SetValue(target, value, null);
    }

    public static MethodInfo Method(Type type, string name, int parameters)
    {
        for (Type current = type; current != null; current = current.BaseType)
            foreach (MethodInfo method in current.GetMethods(All))
                if (method.Name == name && method.GetParameters().Length == parameters && !method.IsGenericMethodDefinition)
                    return method;
        throw new MissingMethodException(type.FullName, name);
    }

    public static object Call(object target, string name, params object[] arguments)
    {
        return Invoke(Method(target.GetType(), name, arguments.Length), target, arguments);
    }

    public static object CallStatic(Type type, string name, params object[] arguments)
    {
        return Invoke(Method(type, name, arguments.Length), null, arguments);
    }

    public static object Invoke(MethodBase method, object target, object[] arguments)
    {
        try { return method.Invoke(target, arguments); }
        catch (TargetInvocationException error)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
    }

    public static object New(Type type, params object[] arguments)
    {
        foreach (ConstructorInfo constructor in type.GetConstructors(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            ParameterInfo[] parameters = constructor.GetParameters();
            if (parameters.Length != arguments.Length) continue;
            bool match = true;
            for (int index = 0; index < parameters.Length && match; ++index)
                match = arguments[index] == null ? !parameters[index].ParameterType.IsValueType
                                                 : parameters[index].ParameterType.IsInstanceOfType(arguments[index]);
            if (!match) continue;
            try { return constructor.Invoke(arguments); }
            catch (TargetInvocationException error)
            {
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
                throw;
            }
        }
        throw new MissingMethodException(type.FullName, ".ctor/" + arguments.Length);
    }

    public static object Blank(Type type)
    {
        object value = FormatterServices.GetUninitializedObject(type);
        GC.SuppressFinalize(value);
        return value;
    }
}

internal sealed class DeviceService : IGraphicsDeviceService
{
    public GraphicsDevice GraphicsDevice { get; set; }
#pragma warning disable 67
    public event EventHandler<EventArgs> DeviceCreated;
    public event EventHandler<EventArgs> DeviceDisposing;
    public event EventHandler<EventArgs> DeviceReset;
    public event EventHandler<EventArgs> DeviceResetting;
#pragma warning restore 67
}

internal sealed class ServiceTable : IServiceProvider
{
    public readonly Dictionary<Type, object> Services = new Dictionary<Type, object>();
    public object GetService(Type type) { object value; return Services.TryGetValue(type, out value) ? value : null; }
}

internal static partial class Program
{
    public static Assembly Game, Common;
    public static bool Modern;
    public static string GameDirectory, OutputDirectory;
    public static int Width, Height;
    public static GraphicsDevice Device;
    public static ContentManager Content;
    public static object GameObject;
    public static Type GameType;
    public static object Input, Player, Inventory, Hud, GameScreen, UiGroup, ScreenManager, PickupManager;
    public static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
    private static double totalSeconds;

    public static Type T(string name) { return Game.GetType(name, false) ?? Common.GetType(name, true); }

    private static int Main(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("usage: ScreensParityProbe <game directory> <output directory> <width> <height> [scenario filter]");
            return 2;
        }
        Thread.CurrentThread.CurrentCulture = new CultureInfo("en-US");
        Thread.CurrentThread.CurrentUICulture = new CultureInfo("en-US");
        GameDirectory = Path.GetFullPath(args[0]);
        OutputDirectory = Path.GetFullPath(args[1]);
        Width = int.Parse(args[2], Invariant);
        Height = int.Parse(args[3], Invariant);
        string filter = args.Length > 4 ? args[4] : null;
        Directory.CreateDirectory(Path.Combine(OutputDirectory, "draws"));
        Directory.CreateDirectory(Path.Combine(OutputDirectory, "frames"));
        AppDomain.CurrentDomain.AssemblyResolve += (sender, resolve) =>
        {
            string name = new AssemblyName(resolve.Name).Name;
            foreach (string extension in new[] { ".dll", ".exe" })
            {
                string path = Path.Combine(GameDirectory, name + extension);
                if (File.Exists(path)) return Assembly.LoadFrom(path);
            }
            return null;
        };
        try
        {
            if (!Setup()) return 77;
            DumpTables();
            RunScenarios(filter);
            if (Width == 1280 && Height == 720 && filter == null) RunInteractions();
            File.WriteAllLines(Path.Combine(OutputDirectory, "measures." + (Modern ? "modern" : "classic") + "." + Width + "x" + Height + ".txt"),
                               Recorder.Measures);
            Console.WriteLine("screens probe: " + (Modern ? "modern" : "classic") + " " + Width + "x" + Height + " ok");
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            Environment.Exit(1);
        }
        // Worker threads the shipped terrain starts must not keep the probe alive.
        Environment.Exit(0);
        return 0;
    }

    private static bool Setup()
    {
        Game = Assembly.LoadFrom(Path.Combine(GameDirectory, "CastleMinerZ.exe"));
        Common = Assembly.LoadFrom(Path.Combine(GameDirectory, "DNA.Common.dll"));
        Modern = Game.GetType("DNA.CastleMinerZ.UI.CraftingScreen", false) != null;
        GameType = T("DNA.CastleMinerZ.CastleMinerZGame");

        var harmony = new Harmony("openminerz.screens-parity-probe");
        Recorder.Install(harmony, Common);
        InstallStubs(harmony);

        var device = new DeviceService();
        var form = new System.Windows.Forms.Form();
        var parameters = new PresentationParameters
        {
            BackBufferWidth = Width, BackBufferHeight = Height, DeviceWindowHandle = form.Handle,
            IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
            BackBufferFormat = SurfaceFormat.Color, DepthStencilFormat = DepthFormat.Depth24,
        };
        GraphicsProfile profile = GraphicsProfile.HiDef;
        try { device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, profile, parameters); }
        catch (Exception)
        {
            profile = GraphicsProfile.Reach;
            try { device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, profile, parameters); }
            catch (Exception error)
            {
                Console.Error.WriteLine("no graphics device: " + error.Message);
                return false;
            }
        }
        Device = device.GraphicsDevice;
        var services = new ServiceTable();
        services.Services[typeof(IGraphicsDeviceService)] = device;

        GameObject = R.Blank(GameType);
        R.SetStatic(GameType, "Instance", GameObject);
        foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
            if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(GameObject, device);
        object profileManager = R.GetStatic(T("DNA.CastleMinerZ.GraphicsProfileSupport.GraphicsProfileManager"), "Instance");
        R.Set(profileManager, "_profile", profile);
        // GraphicsDeviceLocker.Create minus the GraphicsDeviceManager events.
        Type locker = T("DNA.Drawing.GraphicsDeviceLocker");
        if (R.GetStatic(locker, "Instance") == null) R.SetStatic(locker, "Instance", R.New(locker));
        // No command line: stock textures, no user texture folder.
        Type arguments = Game.GetType("DNA.CastleMinerZ.CastleMinerZArgs", false);
        if (arguments != null && R.Has(arguments, "Instance") && R.GetStatic(arguments, "Instance") == null)
        {
            object parsed = R.Blank(arguments);
            if (R.Has(arguments, "TextureQualityLevel")) R.Set(parsed, "TextureQualityLevel", 1);
            R.SetStatic(arguments, "Instance", parsed);
        }
        // The game's own content manager, so every asset resolves through the
        // shipped root-then-profile lookup.
        Type contentType = T("DNA.CastleMinerZ.GraphicsProfileSupport.ProfiledContentManager");
        Content = (ContentManager)(Modern ? R.New(contentType, services, "ReachContent", "HiDefContent", 1)
                                          : R.New(contentType, services, "ReachContent", "HiDefContent"));
        Content.RootDirectory = Path.Combine(GameDirectory, "Content");
        ((Game)GameObject).Content = Content;

        R.Set(GameObject, "CurrentGameTime", new GameTime());
        R.Set(GameObject, "GameMode", Enum.Parse(R.Get(GameObject, "GameMode").GetType(), "Survival"));
        if (R.Has(GameType, "TrialMode")) R.SetStatic(GameType, "TrialMode", false);
        // The stats constructor binds to Steam whenever a game instance exists;
        // built without one it is the plain per-item counter table.
        R.SetStatic(GameType, "Instance", null);
        object stats = R.New(T("DNA.CastleMinerZ.CastleMinerZPlayerStats"));
        R.SetStatic(GameType, "Instance", GameObject);
        R.Set(GameObject, "PlayerStats", stats);
        R.Set(GameObject, "SpriteBatch", new SpriteBatch(Device));
        var white = new Texture2D(Device, 1, 1);
        white.SetData(new[] { Color.White });
        R.Set(GameObject, "DummyTexture", white);
        Recorder.RegisterTexture(white, "@white");
        var pointer = Content.Load<Texture2D>("MousePointer");
        R.Set(GameObject, "MousePointer", pointer);
        Recorder.RegisterTexture(pointer, "@cursor");
        Input = R.New(T("DNA.Input.InputManager"), GameObject);
        R.Set(GameObject, "InputManager", Input);
        if (R.Has(GameType, "_deviceAlive")) R.Set(GameObject, "_deviceAlive", true);
        Type loadStatus = R.Get(GameObject, "_loadStatus").GetType();
        R.Set(GameObject, "_loadStatus", Enum.Parse(loadStatus, "Complete"));

        // The same loads CastleMinerZGame.SecondaryLoad performs, in its order.
        Type screenType = T("DNA.Drawing.UI.Screen");
        if (Modern)
        {
            object adjuster = R.GetStatic(screenType, "Adjuster");
            Type size = T("DNA.Drawing.Size");
            R.Set(adjuster, "AuthoredSize", R.New(size, 1280, 720));
            R.Set(adjuster, "ScreenSize", R.New(size, Width, Height));
            R.Call(GameObject, "CreateOffscreenBuffer", Width, Height);
            R.Set(GameObject, "_terrain", R.New(T("DNA.CastleMinerZ.Terrain.BlockTerrain"), Content));
        }
        else
        {
            R.Call(GameObject, "CreateOffscreenBuffer", 1280, 720);
            R.Set(GameObject, "_terrain", R.New(T("DNA.CastleMinerZ.Terrain.BlockTerrain"), Device, Content));
        }
        Type itemType = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        R.CallStatic(itemType, "Initalize", Content);
        R.CallStatic(T("DNA.CastleMinerZ.BlockEntity"), "Initialize");
        string fontDirectory = Modern ? "Fonts\\" : "720\\";
        MethodInfo localized = T("DNA.ContentTools").GetMethods(BindingFlags.Public | BindingFlags.Static)
            .First(method => method.Name == "LoadLocalized" && method.GetParameters().Length == 2)
            .MakeGenericMethod(typeof(SpriteFont));
        foreach (var font in new[]
                 {
                     new[] { "_consoleFont", "ConsoleFont" }, new[] { "_largeFont", "LargeFont" },
                     new[] { "_medFont", "MedFont" }, new[] { "_medLargeFont", "MedLargeFont" },
                     new[] { "_smallFont", "SmallFont" }, new[] { "_systemFont", "System" },
                     new[] { "_nameTagFont", "NameTagFont" }, new[] { "_myriadLarge", "MyriadLarge" },
                     new[] { "_myriadMed", "MyriadMedium" }, new[] { "_myriadSmall", "MyriadSmall" },
                 })
        {
            var loaded = (SpriteFont)R.Invoke(localized, null, new object[] { Content, fontDirectory + font[1] });
            R.Set(GameObject, font[0], loaded);
            Recorder.RegisterFont(loaded, font[1]);
        }
        Type spriteManager = T("DNA.Drawing.SpriteManager");
        MethodInfo load = typeof(ContentManager).GetMethod("Load");
        object uiSprites = R.Invoke(load.MakeGenericMethod(spriteManager), Content,
                                    new object[] { Modern ? "UI\\SpriteSheet" : "SpriteSheet" });
        R.Set(GameObject, "_uiSprites", uiSprites);
        Recorder.RegisterSheet(uiSprites);
        var dialog = Content.Load<Texture2D>(Modern ? "UI\\Screens\\DialogBack" : "DialogBack");
        R.Set(GameObject, "DialogScreenImage", dialog);
        Recorder.RegisterTexture(dialog, "@dialog");
        object frameSheet = uiSprites;
        if (!Modern)
        {
            frameSheet = R.Invoke(localized.GetGenericMethodDefinition().MakeGenericMethod(spriteManager), null,
                                  new object[] { Content, "TwitchLogos" });
            R.Set(GameObject, "_logoSprites", frameSheet);
            Recorder.RegisterSheet(frameSheet);
        }
        R.Set(GameObject, "ButtonFrame", R.New(T("DNA.Drawing.UI.ScalableFrame"), frameSheet, "CtrlFrame"));
        if (Modern)
        {
            Type dialogScreen = T("DNA.Drawing.UI.PCDialogScreen");
            R.SetStatic(dialogScreen, "DefaultTitlePadding", new Vector2(55f, 15f));
            R.SetStatic(dialogScreen, "DefaultDescriptionPadding", new Vector2(25f, 35f));
            R.SetStatic(dialogScreen, "DefaultButtonsPadding", new Vector2(15f, 23f));
            R.SetStatic(dialogScreen, "DefaultClickSound", "Click");
            R.SetStatic(dialogScreen, "DefaultOpenSound", "Popup");
        }
        // The static model/particle loads SecondaryLoad makes for the entities an
        // item class creates when it draws its own icon.
        foreach (string name in new[] { "RocketEntity", "BlasterShot", "GrenadeProjectile", "ExplosiveFlashEntity", "Explosive",
                                        "GPSMarkerEntity", "GunEntity", "TorchCloud", "TorchEntity" })
        {
            Type type = Game.GetTypes().FirstOrDefault(candidate => candidate.Name == name);
            MethodInfo init = type == null ? null : type.GetMethod("Init", BindingFlags.Public | BindingFlags.Static, null, Type.EmptyTypes, null);
            if (init == null) continue;
            try { R.Invoke(init, null, null); }
            catch (Exception error) { Console.Error.WriteLine("note: " + name + ".Init: " + error.GetType().Name + ": " + error.Message); }
        }
        Recorder.ItemAtlas = () => (Texture2D)R.GetStatic(itemType, "_2DImages");
        if (R.Has(itemType, "_2DImagesLarge"))
            Recorder.ItemAtlasLarge = () => (Texture2D)R.GetStatic(itemType, "_2DImagesLarge");
        Recorder.Offscreen = () => (Texture2D)R.Get(GameObject, "OffScreenBuffer");

        // A disconnected local gamer and an inert player at the origin.
        object gamer = R.Blank(T("DNA.Net.GamerServices.LocalNetworkGamer"));
        R.Set(gamer, "_globalId", (byte)1);
        R.Set(gamer, "_hasLeftSession", true);
        Player = R.Blank(T("DNA.CastleMinerZ.Player"));
        InitializeTreeNode(Player);
        R.Set(Player, "Gamer", gamer);
        R.Set(Player, "_localToWorld", Matrix.CreateTranslation(new Vector3(0.5f, 10f, 0.5f)));
        R.Set(Player, "_ltwDirty", false);
        R.Set(GameObject, "_localPlayer", Player);

        // Singletons the screens call into; the calls themselves are intercepted.
        Type sound = T("DNA.Audio.SoundManager");
        if (R.GetStatic(sound, "Instance") == null) R.SetStatic(sound, "Instance", R.Blank(sound));
        Type pickups = T("DNA.CastleMinerZ.PickupManager");
        PickupManager = R.Blank(pickups);
        // PickupManager.Instance reads a weak reference its constructor fills.
        R.Set(R.GetStatic(pickups, "_instance"), "Target", PickupManager);

        Hud = R.Blank(T("DNA.CastleMinerZ.UI.InGameHUD"));
        R.Set(Hud, "_game", GameObject);
        R.SetStatic(Hud.GetType(), "Instance", Hud);
        GameScreen = R.Blank(T("DNA.CastleMinerZ.GameScreen"));
        R.Set(GameScreen, "_game", GameObject);
        R.Set(GameScreen, "_inGameUI", Hud);
        R.Set(GameObject, "GameScreen", GameScreen);
        PrepareHud(uiSprites);

        // The game renders its item-icon atlas the first time an icon is drawn,
        // from the world it is in: the clock icon's hand shows the time of day
        // and the locator icons' needles point at the origin. Give it the state
        // of a new world (CastleMinerSky.Day = 0.41, empty hands) and render it
        // once, here, so every scenario shares one atlas.
        Type sky = T("DNA.CastleMinerZ.CastleMinerSky");
        object newSky = R.Blank(sky);
        R.Set(newSky, "Day", 0.41f);
        R.Set(GameScreen, "_sky", newSky);
        R.Set(Player, "PlayerInventory", R.New(T("DNA.CastleMinerZ.Inventory.PlayerInventory"), Player, false));
        R.CallStatic(itemType, "FinishInitialization", Device);

        R.SetStatic(screenType, "SelectedPlayerIndex", (PlayerIndex?)PlayerIndex.One);
        return true;
    }

    // An entity created without its constructor still needs the two fields
    // Tree<Entity>'s field initializers would have produced.
    private static void InitializeTreeNode(object entity)
    {
        for (Type type = entity.GetType(); type != null; type = type.BaseType)
        {
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
            FieldInfo parent = type.GetField("_parentReference", flags);
            if (parent == null) continue;
            parent.SetValue(entity, Activator.CreateInstance(parent.FieldType, new object[] { null }));
            FieldInfo children = type.GetField("_children", flags);
            children.SetValue(entity, Activator.CreateInstance(children.FieldType, new object[] { entity }));
            return;
        }
    }

    // ------------------------------------------------------------ interception

    private static void InstallStubs(Harmony harmony)
    {
        var skip = new HarmonyMethod(typeof(Recorder).GetMethod("Skip"));
        // Game.Draw walks the component list of a constructed Game.
        harmony.Patch(AccessTools.Method(typeof(Game), "Draw"), skip);
        harmony.Patch(AccessTools.Method(Common.GetType("DNA.DNAGame", true), "DrawBrightness"), skip);
        Type sound = T("DNA.Audio.SoundManager");
        foreach (MethodInfo method in sound.GetMethods(BindingFlags.Instance | BindingFlags.Public)
                     .Where(candidate => candidate.Name == "PlayInstance"))
            harmony.Patch(method, new HarmonyMethod(typeof(Recorder).GetMethod("SoundPrefix")));
        Type pickups = T("DNA.CastleMinerZ.PickupManager");
        harmony.Patch(AccessTools.Method(pickups, "CreatePickup"),
                      new HarmonyMethod(typeof(Program).GetMethod("PickupPrefix")));
        harmony.Patch(AccessTools.Method(T("DNA.CastleMinerZ.Net.ItemCrateMessage"), "Send"),
                      new HarmonyMethod(typeof(Program).GetMethod("CrateMessagePrefix")));
        harmony.Patch(AccessTools.Method(T("DNA.CastleMinerZ.Net.CrateFocusMessage"), "Send"),
                      new HarmonyMethod(typeof(Program).GetMethod("CrateFocusPrefix")));
        Type session = T("DNA.Net.GamerServices.NetworkSession");
        harmony.Patch(session.GetProperty("RemoteGamers").GetGetMethod(),
                      new HarmonyMethod(typeof(Program).GetMethod("RemoteGamersPrefix")));
        harmony.Patch(session.GetProperty("AllGamers").GetGetMethod(),
                      new HarmonyMethod(typeof(Program).GetMethod("AllGamersPrefix")));
        Type hud = T("DNA.CastleMinerZ.UI.InGameHUD");
        foreach (string name in new[] { "DrawPlayerStats", "DrawDistanceStr", "ExternalUpdate" })
        {
            MethodInfo method = AccessTools.Method(hud, name);
            if (method == null) continue;
            harmony.Patch(method, new HarmonyMethod(typeof(Program).GetMethod("HudPrefix")));
            // The two stat panels still draw (the frames are complete) but the
            // record only names them: they are the HUD's, not these screens'.
            if (name != "ExternalUpdate")
                harmony.Patch(method, new HarmonyMethod(typeof(Recorder).GetMethod("SuppressPrefix")),
                              new HarmonyMethod(typeof(Recorder).GetMethod("SuppressPostfix")));
        }
    }

    public static string ItemText(object item)
    {
        if (item == null) return "-";
        object itemClass = R.Get(item, "ItemClass");
        var health = (float)R.Get(item, "ItemHealthLevel");
        string text = Convert.ToInt32(R.Get(itemClass, "ID"), Invariant) + "x" + (int)R.Get(item, "StackCount");
        if (health != 1f) text += "@" + Recorder.F(health);
        return text;
    }

    public static bool PickupPrefix(object[] __args)
    {
        Recorder.Event("pickup " + ItemText(__args[0]));
        return false;
    }

    public static bool CrateMessagePrefix(object[] __args)
    {
        // ItemCrateMessage.Send(gamer, item, crate, index)
        var slots = (Array)R.Get(__args[2], "Inventory");
        slots.SetValue(__args[1], (int)__args[3]);
        Recorder.Event("crate " + __args[3] + " " + ItemText(__args[1]));
        return false;
    }

    public static object RemoteGamers, AllGamers;
    public static bool RemoteGamersPrefix(ref object __result) { __result = RemoteGamers; return false; }
    public static bool AllGamersPrefix(ref object __result) { __result = AllGamers; return false; }

    public static bool CrateFocusPrefix(object[] __args)
    {
        // CrateFocusMessage.Send(gamer, crate location, slot)
        var slot = (Point)__args[2];
        Recorder.Event("focus " + __args[1] + " " + slot.X + "," + slot.Y);
        return false;
    }

    // 1.9.9.8's inventory and crate screens call into the HUD for the health,
    // stamina and distance read-outs. Those two draws run for real (against a
    // full-health player who has not moved); the HUD's own per-frame update,
    // which needs the world, does not.
    public static bool HudPrefix(MethodBase __originalMethod)
    {
        if (__originalMethod.Name == "ExternalUpdate") return false;
        return hudDrawsRun;
    }

    private static bool hudDrawsRun;

    private static void PrepareHud(object uiSprites)
    {
        try
        {
            Type hud = Hud.GetType();
            if (!R.Has(hud, "_emptyStaminaBar")) return;
            MethodInfo sprite = uiSprites.GetType().GetProperty("Item").GetGetMethod();
            R.Set(Hud, "_emptyStaminaBar", sprite.Invoke(uiSprites, new object[] { "StaminaBarEmpty" }));
            R.Set(Hud, "_emptyHealthBar", sprite.Invoke(uiSprites, new object[] { "HealthBarEmpty" }));
            R.Set(Hud, "_fullHealthBar", sprite.Invoke(uiSprites, new object[] { "HealthBarFull" }));
            R.Set(Hud, "PlayerHealth", 1f);
            R.Set(Hud, "PlayerStamina", 1f);
            object timer = R.New(T("DNA.Timers.OneShotTimer"), TimeSpan.FromSeconds(2.0));
            R.Call(timer, "Update", TimeSpan.FromSeconds(3.0));
            R.Set(Hud, "StaminaBlockTimer", timer);
            R.Set(Hud, "sbuilder", new StringBuilder());
            R.Set(Hud, "distanceBuilder", new StringBuilder());
            if (R.Has(hud, "console")) R.Set(Hud, "console", R.Blank(R.Get(Hud, "console") == null ? hud.GetField("console").FieldType : R.Get(Hud, "console").GetType()));
            hudDrawsRun = true;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("note: HUD stat panels are logged, not drawn: " + error.GetType().Name + ": " + error.Message);
        }
    }

    // ------------------------------------------------------------------ frames

    public static GameTime Tick(double seconds)
    {
        totalSeconds += seconds;
        var time = new GameTime(TimeSpan.FromSeconds(totalSeconds), TimeSpan.FromSeconds(seconds));
        R.Set(GameObject, "CurrentGameTime", time);
        return time;
    }

    // One game frame, in DNAGame.Update's order and then DNAGame.Draw.
    public static void Frame(double seconds = 1.0 / 60.0, bool input = true)
    {
        GameTime time = Tick(seconds);
        if (input) R.Call(ScreenManager, "ProcessInput", Input, time);
        R.Call(ScreenManager, "Update", GameObject, time);
        Recorder.Lines.Clear();
        Recorder.Enabled = true;
        try { R.Invoke(R.Method(GameType, "Draw", 1), GameObject, new object[] { time }); }
        finally { Recorder.Enabled = false; }
    }

    // --------------------------------------------------------------- the mouse

    private static int mouseX, mouseY, mouseWheel;
    private static bool mouseLeft, mouseRight;
    private static readonly HashSet<Keys> keysDown = new HashSet<Keys>();

    // Commit the scripted device state as this frame's sample: the previous
    // sample becomes "last", exactly as MouseInput.Update / KeyboardInput.Update
    // shift them.
    public static void CommitInput()
    {
        object mouse = R.Get(Input, "Mouse");
        object current = R.Get(mouse, "_currentState");
        R.Set(mouse, "_lastState", current);
        object next = R.Get(mouse, "_currentState");
        R.Set(next, "MouseState", new MouseState(mouseX, mouseY, mouseWheel,
                                                 mouseLeft ? ButtonState.Pressed : ButtonState.Released,
                                                 ButtonState.Released,
                                                 mouseRight ? ButtonState.Pressed : ButtonState.Released,
                                                 ButtonState.Released, ButtonState.Released));
        R.Set(mouse, "_currentState", next);
        object keyboard = R.Get(Input, "Keyboard");
        R.Set(keyboard, "_lastState", R.Get(keyboard, "_currentState"));
        R.Set(keyboard, "_currentState", new KeyboardState(keysDown.ToArray()));
    }

    public static void MoveMouse(int x, int y) { mouseX = x; mouseY = y; }
    public static void Buttons(bool left, bool right) { mouseLeft = left; mouseRight = right; }
    public static void Wheel(int delta) { mouseWheel += delta; }
    public static void Key(Keys key, bool down) { if (down) keysDown.Add(key); else keysDown.Remove(key); }

    public static void ResetInput()
    {
        mouseX = mouseY = -100;
        mouseLeft = mouseRight = false;
        keysDown.Clear();
        CommitInput();
        CommitInput();
    }

    public static void SaveFrame(string path)
    {
        var pixels = new Color[Width * Height];
        Device.SetRenderTarget(null);
        Device.GetBackBufferData(pixels);
        SavePng(path, pixels, Width, Height);
    }

    public static void SavePng(string path, Color[] pixels, int width, int height)
    {
        using (var bitmap = new System.Drawing.Bitmap(width, height, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
        {
            var data = bitmap.LockBits(new System.Drawing.Rectangle(0, 0, width, height),
                                       System.Drawing.Imaging.ImageLockMode.WriteOnly, bitmap.PixelFormat);
            var bytes = new byte[width * height * 4];
            for (int index = 0; index < pixels.Length; ++index)
            {
                bytes[index * 4 + 0] = pixels[index].B;
                bytes[index * 4 + 1] = pixels[index].G;
                bytes[index * 4 + 2] = pixels[index].R;
                bytes[index * 4 + 3] = pixels[index].A;
            }
            System.Runtime.InteropServices.Marshal.Copy(bytes, 0, data.Scan0, bytes.Length);
            bitmap.UnlockBits(data);
            bitmap.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
