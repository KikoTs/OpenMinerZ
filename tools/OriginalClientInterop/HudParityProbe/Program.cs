using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;
using HarmonyLib;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// HUD parity probe. It loads the user's owned, unmodified Castle Miner Z
// assemblies by path and runs the ORIGINAL InGameHUD.OnDraw offline, with a
// scripted player state, into a render target of a chosen back-buffer size.
// Every sprite and string the original submits to SpriteBatch is recorded in
// submission order; that list is the specification OpenMinerZ's HUD layout is
// tested against (tests/test_hud_layout.cpp), and the rendered frame is saved
// as a PNG for side-by-side review.
//
// Nothing here starts Steam, opens a session or runs the game loop, and no
// game or XNA assembly is rewritten or copied: SpriteBatch is observed with
// run-time method hooks. Without a graphics device the probe exits 77 (skip).
//
// What is NOT original code here, and nothing else:
//   * The game object. CastleMinerZGame, the local Player, the HUD and the
//     terrain singleton are created without their constructors and given
//     exactly the fields InGameHUD.OnDraw reads. The sprite sheet, the fonts
//     and the item classes are the original's own, loaded by the original
//     ContentManager readers and InventoryItem.Initalize.
//   * Item icons. InventoryItem.FinishInitialization renders every icon from
//     its 3D model through the terrain shaders; the probe supplies an empty
//     atlas instead, so icon PIXELS are blank while every icon RECTANGLE is
//     the original's.
//   * A few getters that would need a live session or terrain are answered by
//     the scenario instead (Player.Underwater, InGameHUD.GetBlock,
//     ScreenGroup.CurrentScreen, CastleMinerZGame.IsOnlineGame).
//
// Record lines (tab separated, shared with openminerz_hud_layout_tests --dump):
//   C  canvas-w canvas-h  present-x present-y present-w present-h
//   S  texture sprite  src(x y w h, sprite-relative)  dst(x y w h)  r g b a  rotation origin(x y) flip
//   T  font  x y  scale-x scale-y  r g b a  rotation origin(x y)  "string"
internal static class R
{
    private const BindingFlags All = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public |
                                     BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

    public static MemberInfo Find(Type type, string name, bool required = true)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, All);
            if (field != null) return field;
            PropertyInfo property = current.GetProperty(name, All);
            if (property != null) return property;
        }
        if (required) throw new MissingMemberException(type.FullName, name);
        return null;
    }

    public static bool Has(object target, string name) { return Find(target.GetType(), name, false) != null; }
    public static object Get(object target, string name) { return Read(target.GetType(), target, name); }
    public static object GetStatic(Type type, string name) { return Read(type, null, name); }
    public static void Set(object target, string name, object value) { Write(target.GetType(), target, name, value); }
    public static void SetStatic(Type type, string name, object value) { Write(type, null, name, value); }
    public static bool TrySet(object target, string name, object value)
    {
        if (!Has(target, name)) return false;
        Set(target, name, value);
        return true;
    }

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

    public static MethodInfo Method(Type type, string name, int parameters = -1)
    {
        for (Type current = type; current != null; current = current.BaseType)
            foreach (MethodInfo method in current.GetMethods(All))
                if (method.Name == name && (parameters < 0 || method.GetParameters().Length == parameters) &&
                    !method.IsGenericMethodDefinition)
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
        try
        {
            ConstructorInfo constructor = method as ConstructorInfo;
            return constructor != null ? constructor.Invoke(arguments) : method.Invoke(target, arguments);
        }
        catch (TargetInvocationException error)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
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

// Modern keeps profile-specific assets under HiDefContent/ReachContent; Classic
// keeps everything at the root. Either way the bytes come from the owned install.
internal sealed class OwnedContent : ContentManager
{
    private readonly string profileDirectory;

    public OwnedContent(IServiceProvider services, string root, string profileDirectory) : base(services, root)
    {
        this.profileDirectory = profileDirectory;
    }

    public override T Load<T>(string assetName)
    {
        string profiled = Path.Combine(profileDirectory, assetName);
        return File.Exists(Path.Combine(RootDirectory, profiled + ".xnb")) ? base.Load<T>(profiled) : base.Load<T>(assetName);
    }
}

// What the original submitted to SpriteBatch, in order.
internal static class Recorder
{
    public static readonly List<string> Lines = new List<string>();
    public static readonly Dictionary<Texture2D, string> Textures = new Dictionary<Texture2D, string>();
    public static readonly Dictionary<SpriteFont, string> Fonts = new Dictionary<SpriteFont, string>();
    public static readonly List<KeyValuePair<string, Rectangle>> Sprites = new List<KeyValuePair<string, Rectangle>>();
    public static Texture2D Sheet;
    public static bool Recording;
    private static int textDepth;
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public static string F(float value)
    {
        // Enough digits to compare, few enough to read.
        return Math.Round(value, 3).ToString("0.###", Inv);
    }

    // SpriteBatch.InternalDraw(texture, ref Vector4 destination, bool scaleDestination,
    //                          ref Rectangle? source, color, rotation, ref Vector2 origin, effects, depth)
    public static void Sprite(object[] __args)
    {
        if (!Recording || textDepth > 0) return;
        var texture = (Texture2D)__args[0];
        var destination = (Vector4)__args[1];
        bool scaleDestination = (bool)__args[2];
        var sourceOrNull = (Rectangle?)__args[3];
        var color = (Color)__args[4];
        float rotation = (float)__args[5];
        var origin = (Vector2)__args[6];
        var effects = (SpriteEffects)__args[7];
        Rectangle source = sourceOrNull ?? new Rectangle(0, 0, texture.Width, texture.Height);
        float width = scaleDestination ? destination.Z * source.Width : destination.Z;
        float height = scaleDestination ? destination.W * source.Height : destination.W;

        string name;
        if (!Textures.TryGetValue(texture, out name)) name = "texture" + texture.Width + "x" + texture.Height;
        string sprite = "-";
        if (texture == Sheet)
        {
            // Report the source relative to the named sprite, as the HUD code does.
            foreach (KeyValuePair<string, Rectangle> candidate in Sprites)
                if (candidate.Value.Contains(source) ||
                    (source.Width == 0 && candidate.Value.Contains(new Point(source.X, source.Y))))
                {
                    sprite = candidate.Key;
                    source = new Rectangle(source.X - candidate.Value.X, source.Y - candidate.Value.Y, source.Width, source.Height);
                    break;
                }
        }
        Lines.Add(string.Join("\t", "S", name, sprite,
            source.X.ToString(Inv), source.Y.ToString(Inv), source.Width.ToString(Inv), source.Height.ToString(Inv),
            F(destination.X), F(destination.Y), F(width), F(height),
            color.R.ToString(Inv), color.G.ToString(Inv), color.B.ToString(Inv), color.A.ToString(Inv),
            F(rotation), F(origin.X), F(origin.Y), effects == SpriteEffects.None ? "-" : effects.ToString()));
    }

    // SpriteFont.InternalDraw(ref StringProxy text, spriteBatch, Vector2 position, color,
    //                         rotation, Vector2 origin, ref Vector2 scale, effects, depth)
    public static void TextBefore(SpriteFont __instance, object[] __args)
    {
        if (Recording && textDepth == 0)
        {
            object proxy = __args[0];
            string text = null;
            foreach (FieldInfo field in proxy.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                object value = field.GetValue(proxy);
                if (value is string) text = (string)value;
                else if (value is StringBuilder) text = value.ToString();
            }
            var position = (Vector2)__args[2];
            var color = (Color)__args[3];
            float rotation = (float)__args[4];
            var origin = (Vector2)__args[5];
            var scale = (Vector2)__args[6];
            string name;
            if (!Fonts.TryGetValue(__instance, out name)) name = "font" + __instance.LineSpacing;
            Lines.Add(string.Join("\t", "T", name, F(position.X), F(position.Y), F(scale.X), F(scale.Y),
                color.R.ToString(Inv), color.G.ToString(Inv), color.B.ToString(Inv), color.A.ToString(Inv),
                F(rotation), F(origin.X), F(origin.Y),
                "\"" + (text ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n") + "\""));
        }
        ++textDepth;
    }

    public static void TextAfter() { --textDepth; }
}

// Answers the scenario gives in place of a live session or terrain.
internal static class Scenario
{
    public static bool Underwater;
    public static int AimedBlock = -1;       // BlockTypeEnum value, or -1 for "nothing aimed"
    public static object Hud;
    public static bool Online;

    public static bool UnderwaterGetter(ref bool __result) { __result = Underwater; return false; }
    public static bool CurrentScreenGetter(ref object __result) { __result = Hud; return false; }
    public static bool IsOnlineGetter(ref bool __result) { __result = Online; return false; }
    public static bool GetBlock(MethodBase __originalMethod, ref object __result)
    {
        __result = Enum.ToObject(((MethodInfo)__originalMethod).ReturnType, AimedBlock < 0 ? 0 : AimedBlock);
        return false;
    }
    public static bool AbleToBuildGetter(ref bool __result) { __result = AimedBlock >= 0; return false; }
    public static bool Skip() { return false; }
}

internal sealed class Original
{
    public Assembly Game;
    public Assembly Common;
    public bool Modern;
    public object GameInstance;
    public object Player;
    public object Inventory;
    public object Hud;
    public GraphicsDevice Device;
    public SpriteBatch Batch;
    public int BackBufferWidth;
    public int BackBufferHeight;
    public readonly Dictionary<string, SpriteFont> Fonts = new Dictionary<string, SpriteFont>();
    public readonly Dictionary<string, Rectangle> SpriteRectangles = new Dictionary<string, Rectangle>();

    public Type T(string name)
    {
        return Game.GetType(name, false) ?? Common.GetType(name, true);
    }

    // The canvas the original lays the HUD out in. 1.6.3.8 always draws into a
    // 1280x720 offscreen buffer (CastleMinerZGame.SecondaryLoad) that DNAGame.Draw
    // then stretches, aspect preserved, onto the window; 1.9.9.8 draws straight
    // into the back buffer and scales by Screen.Adjuster.
    public int CanvasWidth { get { return Modern ? BackBufferWidth : 1280; } }
    public int CanvasHeight { get { return Modern ? BackBufferHeight : 720; } }

    public static Original Create(string gameDirectory, int width, int height)
    {
        var original = new Original { BackBufferWidth = width, BackBufferHeight = height };
        original.Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
        original.Common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
        // 1.9.9.8 moved the sprite sheet and fonts into UI\ and Fonts\.
        original.Modern = File.Exists(Path.Combine(gameDirectory, "Content", "UI", "SpriteSheet.xnb"));
        try { R.CallStatic(original.Common.GetType("DNA.Reflection.ReflectionTools", true), "RegisterAssembly", original.Common, original.Game); }
        catch (Exception) { /* only needed for network message ids */ }

        var service = new DeviceService();
        string profileDirectory = "HiDefContent";
        var form = new System.Windows.Forms.Form();
        var parameters = new PresentationParameters
        {
            BackBufferWidth = width, BackBufferHeight = height, DeviceWindowHandle = form.Handle,
            IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
        };
        try { service.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.HiDef, parameters); }
        catch (Exception)
        {
            profileDirectory = "ReachContent";
            service.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.Reach, parameters);
        }
        original.Device = service.GraphicsDevice;
        original.Batch = new SpriteBatch(original.Device);
        var services = new ServiceTable();
        services.Services[typeof(IGraphicsDeviceService)] = service;

        Type gameType = original.T("DNA.CastleMinerZ.CastleMinerZGame");
        object game = original.GameInstance = R.Blank(gameType);
        R.SetStatic(gameType, "Instance", game);
        var content = new OwnedContent(services, Path.Combine(gameDirectory, "Content"), profileDirectory);
        ((Game)game).Content = content;
        foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
            if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(game, service);
        R.TrySet(game, "CurrentGameTime", new GameTime());

        // DNAGame.LoadContent: a 1x1 white texture for every solid rectangle.
        var dummy = new Texture2D(original.Device, 1, 1);
        dummy.SetData(new[] { Color.White });
        R.Set(game, "DummyTexture", dummy);
        Recorder.Textures[dummy] = "solid";

        // CastleMinerZGame.SecondaryLoad: the fonts and the sprite sheet.
        string fontDirectory = original.Modern ? @"Fonts\" : @"720\";
        foreach (var font in new[] { new[] { "_consoleFont", "ConsoleFont" }, new[] { "_largeFont", "LargeFont" },
                                     new[] { "_medFont", "MedFont" }, new[] { "_medLargeFont", "MedLargeFont" },
                                     new[] { "_smallFont", "SmallFont" }, new[] { "_systemFont", "System" },
                                     new[] { "_nameTagFont", "NameTagFont" } })
        {
            SpriteFont loaded = content.Load<SpriteFont>(fontDirectory + font[1]);
            R.Set(game, font[0], loaded);
            original.Fonts[font[1]] = loaded;
            Recorder.Fonts[loaded] = font[1];
        }
        Type managerType = original.T("DNA.Drawing.SpriteManager");
        MethodInfo load = typeof(ContentManager).GetMethod("Load").MakeGenericMethod(managerType);
        object sprites = R.Invoke(load, content, new object[] { original.Modern ? @"UI\SpriteSheet" : "SpriteSheet" });
        R.Set(game, "_uiSprites", sprites);
        foreach (DictionaryEntry entry in (IDictionary)R.Get(sprites, "_sprites"))
        {
            var rectangle = (Rectangle)R.Get(entry.Value, "SourceRectangle");
            original.SpriteRectangles[(string)entry.Key] = rectangle;
            Recorder.Sprites.Add(new KeyValuePair<string, Rectangle>((string)entry.Key, rectangle));
            Recorder.Sheet = (Texture2D)R.Get(entry.Value, "Texture");
        }
        Recorder.Textures[Recorder.Sheet] = "sheet";

        if (original.Modern)
        {
            Type screen = original.T("DNA.Drawing.UI.Screen");
            object adjuster = R.GetStatic(screen, "Adjuster");
            Type size = original.T("DNA.Drawing.Size");
            R.Set(adjuster, "AuthoredSize", Activator.CreateInstance(size, 1280, 720));
            R.Set(adjuster, "ScreenSize", Activator.CreateInstance(size, width, height));
        }

        // A terrain singleton that answers "not a water world"; block lookups
        // are answered by the scenario.
        Type terrain = original.T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        try { R.SetStatic(terrain, "Instance", R.Blank(terrain)); }
        catch (Exception) { R.SetStatic(terrain, "_theTerrain", R.Blank(terrain)); }

        // The original item classes, names and all. Item models are real content.
        R.CallStatic(original.T("DNA.CastleMinerZ.Inventory.InventoryItem"), "Initalize", content);
        Type item = original.T("DNA.CastleMinerZ.Inventory.InventoryItem");
        int atlasHeight = original.Modern ? 1280 : 1024;
        var atlas = new RenderTarget2D(original.Device, 512, atlasHeight, false, SurfaceFormat.Color, DepthFormat.None);
        original.Device.SetRenderTarget(atlas);
        original.Device.Clear(Color.Transparent);
        original.Device.SetRenderTarget(null);
        R.SetStatic(item, "_2DImages", atlas);
        Recorder.Textures[atlas] = "items";
        if (original.Modern)
        {
            var large = new RenderTarget2D(original.Device, 1024, atlasHeight * 2, false, SurfaceFormat.Color, DepthFormat.None);
            original.Device.SetRenderTarget(large);
            original.Device.Clear(Color.Transparent);
            original.Device.SetRenderTarget(null);
            R.SetStatic(item, "_2DImagesLarge", large);
            Recorder.Textures[large] = "items2x";
        }

        original.CreatePlayer();
        original.CreateHud();
        original.Hook();
        return original;
    }

    private static void InitializeTreeNode(object entity)
    {
        // Entity extends Tree<Entity>; without its constructor the child list is null.
        for (Type current = entity.GetType(); current != null; current = current.BaseType)
            foreach (FieldInfo field in current.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
            {
                if (field.GetValue(entity) != null) continue;
                Type type = field.FieldType;
                if (type.IsGenericType && type.GetConstructor(Type.EmptyTypes) != null && typeof(IEnumerable).IsAssignableFrom(type))
                    try { field.SetValue(entity, Activator.CreateInstance(type)); } catch (Exception) { }
            }
    }

    private void CreatePlayer()
    {
        Type playerType = T("DNA.CastleMinerZ.Player");
        Player = R.Blank(playerType);
        InitializeTreeNode(Player);
        // Entity transforms default to zero matrices without the constructor.
        foreach (string matrix in new[] { "_localToParent", "_localToWorld", "_worldToLocal" })
            R.TrySet(Player, matrix, Matrix.Identity);
        R.TrySet(Player, "_scale", Vector3.One);
        R.TrySet(Player, "_rotation", Quaternion.Identity);
        object avatar = R.Blank(T("DNA.Avatars.Avatar"));
        InitializeTreeNode(avatar);
        R.Set(Player, "Avatar", avatar);
        R.Set(GameInstance, "_localPlayer", Player);
        Type inventoryType = T("DNA.CastleMinerZ.Inventory.PlayerInventory");
        ConstructorInfo constructor = inventoryType.GetConstructors().First(c => c.GetParameters().Length == 2);
        Inventory = R.Invoke(constructor, null, new object[] { Player, false });
        R.Set(Player, "PlayerInventory", Inventory);
        if (Modern)
        {
            // FPSCamera.FieldOfView feeds the accuracy reticle.
            Type camera = T("DNA.Drawing.PerspectiveCamera");
            object fps = Activator.CreateInstance(camera);
            R.Set(Player, "FPSCamera", fps);
            object stats = R.Blank(T("DNA.CastleMinerZ.CastleMinerZPlayerStats"));
            R.TrySet(GameInstance, "PlayerStats", stats);
        }
    }

    private void CreateHud()
    {
        Type hudType = T("DNA.CastleMinerZ.UI.InGameHUD");
        Hud = R.Blank(hudType);
        Scenario.Hud = Hud;
        R.SetStatic(hudType, "Instance", Hud);
        R.Set(Hud, "_game", GameInstance);
        object sprites = R.Get(GameInstance, "_uiSprites");
        PropertyInfo indexer = sprites.GetType().GetProperty("Item");
        // InGameHUD's constructor, minus the screens and dialogs it also builds.
        foreach (var sprite in new[] { new[] { "_damageArrow", "DamageArrow" }, new[] { "_gridSprite", "HudGrid" },
                                       new[] { "_selectorSprite", "Selector" }, new[] { "_crosshair", "CrossHair" },
                                       new[] { "_crosshairTick", "CrossHairTick" }, new[] { "_emptyStaminaBar", "StaminaBarEmpty" },
                                       new[] { "_emptyHealthBar", "HealthBarEmpty" }, new[] { "_fullHealthBar", "HealthBarFull" },
                                       new[] { "_bubbleBar", "BubbleBar" }, new[] { "_compass", "Compass" },
                                       new[] { "_sniperScope", "SniperScope" }, new[] { "_missileLocking", "MissleLocking" },
                                       new[] { "_missileLock", "MissleLock" } })
            if (R.Has(Hud, sprite[0]) && SpriteRectangles.ContainsKey(sprite[1]))
                R.Set(Hud, sprite[0], indexer.GetValue(sprites, new object[] { sprite[1] }));

        Type timer = T("DNA.Timers.OneShotTimer");
        Func<double, object> seconds = value => Activator.CreateInstance(timer, TimeSpan.FromSeconds(value));
        foreach (var entry in new Dictionary<string, double> { { "HealthRecoverTimer", 3 }, { "timeToRespawn", 20 },
                     { "timeToShowRespawnText", 3 }, { "fadeInGameStart", 1 }, { "drawDayTimer", 9 },
                     { "StaminaRecoverTimer", 1 }, { "StaminaBlockTimer", 2 }, { "acheivementDisplayTimer", 10 } })
            if (R.Has(Hud, entry.Key)) R.Set(Hud, entry.Key, seconds(entry.Value));
        // Field initializers the constructor would have run.
        for (Type current = hudType; current != null && current != typeof(object); current = current.BaseType)
            foreach (FieldInfo field in current.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
            {
                if (field.GetValue(Hud) != null) continue;
                Type type = field.FieldType;
                if (type == typeof(StringBuilder) ||
                    (type.IsGenericType && typeof(IEnumerable).IsAssignableFrom(type) && type.GetConstructor(Type.EmptyTypes) != null))
                    field.SetValue(Hud, Activator.CreateInstance(type));
            }
        R.Set(Hud, "PlayerHealth", 1f);
        R.Set(Hud, "PlayerOxygen", 1f);
        if (R.Has(Hud, "PlayerStamina")) R.Set(Hud, "PlayerStamina", 1f);
        if (R.Has(Hud, "_secondTrayAlpha")) R.Set(Hud, "_secondTrayAlpha", 0.4f);
        R.Set(Hud, "ConstructionProbe", R.Blank(T("DNA.CastleMinerZ.ConstructionProbeClass")));

        // console = new ConsoleElement(_game._consoleFont); Location = Zero;
        // Size = (BackBufferWidth * 0.3, BackBufferHeight * 0.25)
        Type consoleType = T("DNA.Drawing.UI.ConsoleElement");
        object console = Activator.CreateInstance(consoleType, Fonts["ConsoleFont"]);
        R.Set(console, "Location", Vector2.Zero);
        R.Set(console, "Size", new Vector2(BackBufferWidth * 0.3f, BackBufferHeight * 0.25f));
        R.Set(Hud, "console", console);
        if (R.Has(Hud, "_chatScreen"))
            R.Set(Hud, "_chatScreen", R.Blank(T("DNA.CastleMinerZ.UI.PlainChatInputScreen")));
        object screen = R.Blank(T("DNA.CastleMinerZ.GameScreen"));
        R.Set(screen, "_uiGroup", R.Blank(T("DNA.Drawing.UI.ScreenGroup")));
        R.Set(GameInstance, "GameScreen", screen);
    }

    private void Hook()
    {
        var harmony = new Harmony("openminerz.hud-parity-probe");
        MethodInfo spriteDraw = typeof(SpriteBatch).GetMethods(BindingFlags.Instance | BindingFlags.NonPublic)
            .Single(method => method.Name == "InternalDraw");
        MethodInfo fontDraw = typeof(SpriteFont).GetMethods(BindingFlags.Instance | BindingFlags.NonPublic)
            .Single(method => method.Name == "InternalDraw");
        harmony.Patch(spriteDraw, prefix: new HarmonyMethod(typeof(Recorder).GetMethod("Sprite")));
        harmony.Patch(fontDraw, prefix: new HarmonyMethod(typeof(Recorder).GetMethod("TextBefore")),
                      finalizer: new HarmonyMethod(typeof(Recorder).GetMethod("TextAfter")));

        Type player = T("DNA.CastleMinerZ.Player");
        harmony.Patch(player.GetProperty("Underwater").GetGetMethod(),
                      prefix: new HarmonyMethod(typeof(Scenario).GetMethod("UnderwaterGetter")));
        Type group = T("DNA.Drawing.UI.ScreenGroup");
        harmony.Patch(group.GetProperty("CurrentScreen").GetGetMethod(),
                      prefix: new HarmonyMethod(typeof(Scenario).GetMethod("CurrentScreenGetter")));
        Type game = T("DNA.CastleMinerZ.CastleMinerZGame");
        harmony.Patch(game.GetProperty("IsOnlineGame").GetGetMethod(),
                      prefix: new HarmonyMethod(typeof(Scenario).GetMethod("IsOnlineGetter")));
        Type hud = T("DNA.CastleMinerZ.UI.InGameHUD");
        harmony.Patch(hud.GetMethod("GetBlock", BindingFlags.Static | BindingFlags.Public),
                      prefix: new HarmonyMethod(typeof(Scenario).GetMethod("GetBlock")));
        Type probe = T("DNA.CastleMinerZ.ConstructionProbeClass");
        harmony.Patch(probe.GetProperty("AbleToBuild").GetGetMethod(),
                      prefix: new HarmonyMethod(typeof(Scenario).GetMethod("AbleToBuildGetter")));
    }

    public object Item(string id, int count)
    {
        Type item = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        Type ids = T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        return R.CallStatic(item, "CreateItem", Enum.Parse(ids, id), count);
    }

    public void SetTray(int tray, int index, object item)
    {
        if (Modern)
        {
            object manager = R.Get(Inventory, "TrayManager");
            R.Call(manager, "SetTrayItem", tray, index, item);
        }
        else if (tray == 0)
        {
            ((Array)R.Get(Inventory, "InventoryTray")).SetValue(item, index);
        }
    }

    public void ClearTrays()
    {
        for (int tray = 0; tray < 2; ++tray)
            for (int index = 0; index < 8; ++index)
                SetTray(tray, index, null);
    }

    // DNAGame.Draw's aspect-preserving blit of the offscreen buffer (1.6.3.8).
    public Rectangle PresentRectangle()
    {
        if (Modern) return new Rectangle(0, 0, BackBufferWidth, BackBufferHeight);
        float aspect = (float)BackBufferWidth / BackBufferHeight;
        float buffer = 1280f / 720f;
        if (aspect > buffer)
        {
            int width = BackBufferHeight * 1280 / 720;
            return new Rectangle((BackBufferWidth - width) / 2, 0, width, BackBufferHeight);
        }
        int height = BackBufferWidth * 720 / 1280;
        return new Rectangle(0, (BackBufferHeight - height) / 2, BackBufferWidth, height);
    }

    public void Draw(string recordPath, string imagePath)
    {
        var target = new RenderTarget2D(Device, CanvasWidth, CanvasHeight, false, SurfaceFormat.Color, DepthFormat.Depth24Stencil8);
        Device.SetRenderTarget(target);
        // A mid grey stands in for the world so white and black HUD pixels both show.
        Device.Clear(new Color(96, 110, 124));
        Recorder.Lines.Clear();
        Rectangle present = PresentRectangle();
        Recorder.Lines.Add(string.Join("\t", "C", CanvasWidth, CanvasHeight, present.X, present.Y, present.Width, present.Height));
        Recorder.Recording = true;
        try
        {
            MethodInfo draw = R.Method(Hud.GetType(), "OnDraw", 3);
            R.Invoke(draw, Hud, new object[] { Device, Batch, new GameTime(TimeSpan.FromSeconds(100), TimeSpan.Zero) });
        }
        finally { Recorder.Recording = false; }
        Device.SetRenderTarget(null);
        // 1.9.9.8's accuracy reticle opens by (spread angle / camera field of
        // view) * screen width. The quotient depends on the gun table and the
        // FPS camera, neither of which is layout, so it is handed to the native
        // layout as an input rather than re-derived there.
        if (Modern)
            Recorder.Lines[0] += "\t" + ((float)R.Get(Hud, "_crosshairTickDrawLocation") / CanvasWidth).ToString("R", CultureInfo.InvariantCulture);
        File.WriteAllLines(recordPath, Recorder.Lines, new UTF8Encoding(false));
        using (FileStream stream = File.Create(imagePath))
            target.SaveAsPng(stream, CanvasWidth, CanvasHeight);
        target.Dispose();
    }
}

internal static class Program
{
    private static string gameDirectory;
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    private static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        foreach (string extension in new[] { ".dll", ".exe" })
        {
            string path = Path.Combine(gameDirectory, name + extension);
            if (File.Exists(path)) return Assembly.LoadFrom(path);
        }
        return null;
    }

    private static void Reset(Original o)
    {
        Scenario.Underwater = false;
        Scenario.AimedBlock = -1;
        Scenario.Online = false;
        o.ClearTrays();
        R.Set(o.Inventory, "SelectedInventoryIndex", 0);
        R.Set(o.Player, "Dead", false);
        R.Set(o.Player, "Shouldering", false);
        R.Set(o.Player, "Reloading", false);
        R.Set(o.Hud, "PlayerHealth", 1f);
        R.Set(o.Hud, "PlayerOxygen", 1f);
        if (o.Modern) R.Set(o.Hud, "PlayerStamina", 1f);
        if (o.Modern) R.Set(o.Hud, "InnaccuracyMultiplier", 0f);
        R.Set(o.Hud, "maxDistanceTraveled", 0);
        R.Set(o.Hud, "currentDistanceTraveled", 0);
        R.Set(o.Hud, "currentDay", 1);
        ((IList)R.Get(o.Hud, "_damageIndicator")).Clear();
        // Past their end: no fade-in wash, no "Day N" banner.
        Elapse(o, "fadeInGameStart", 5);
        Elapse(o, "drawDayTimer", 20);
        Elapse(o, "timeToShowRespawnText", 0);
        if (o.Modern) Elapse(o, "StaminaBlockTimer", 5);
        Type mode = R.Get(o.GameInstance, "GameMode").GetType();
        R.Set(o.GameInstance, "GameMode", Enum.Parse(mode, "Survival"));
        R.Call(R.Get(o.Hud, "console"), "Clear");
    }

    private static void Elapse(Original o, string timer, double seconds)
    {
        object value = R.Get(o.Hud, timer);
        R.Call(value, "Reset");
        if (seconds > 0) R.Call(value, "Update", TimeSpan.FromSeconds(seconds));
    }

    private static void StandardTray(Original o)
    {
        object pick = o.Item("StonePickAxe", 1);
        R.Set(pick, "ItemHealthLevel", 0.5f);
        o.SetTray(0, 0, pick);
        o.SetTray(0, 1, o.Item("DirtBlock", 64));
        o.SetTray(0, 2, o.Item("Torch", 7));
        object pistol = o.Item("Pistol", 1);
        R.Set(pistol, "RoundsInClip", 5);
        o.SetTray(0, 3, pistol);
        o.SetTray(0, 4, o.Item("Bullets", 250));
        object spade = o.Item("IronSpade", 1);
        R.Set(spade, "ItemHealthLevel", 0.92f);
        o.SetTray(0, 6, spade);
        o.SetTray(0, 7, o.Item("RockBlock", 2));
    }

    private static void AddDamage(Original o, Vector3 source, double age)
    {
        Type indicator = o.T("DNA.CastleMinerZ.UI.InGameHUD+DamageIndicator");
        object value = Activator.CreateInstance(indicator, source);
        if (age > 0) R.Call(R.Get(value, "drawTimer"), "Update", TimeSpan.FromSeconds(age));
        ((IList)R.Get(o.Hud, "_damageIndicator")).Add(value);
    }

    private static void Apply(Original o, string scenario)
    {
        Reset(o);
        Type blocks = o.T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        switch (scenario)
        {
        case "empty":
            break;
        case "full":
            StandardTray(o);
            R.Set(o.Hud, "currentDistanceTraveled", 137);
            R.Set(o.Hud, "maxDistanceTraveled", 2048);
            break;
        case "hurt":
            StandardTray(o);
            R.Set(o.Inventory, "SelectedInventoryIndex", 3);
            R.Set(o.Hud, "PlayerHealth", 0.35f);
            if (o.Modern) R.Set(o.Hud, "PlayerStamina", 0.6f);
            if (o.Modern) R.Set(o.Hud, "InnaccuracyMultiplier", 0.5f);
            R.Set(o.Hud, "currentDistanceTraveled", 12);
            R.Set(o.Hud, "maxDistanceTraveled", 345);
            Scenario.AimedBlock = Convert.ToInt32(Enum.Parse(blocks, "GoldOre"));
            AddDamage(o, new Vector3(10, 0, 0), 0.5);
            AddDamage(o, new Vector3(0, 0, 10), 2.5);
            break;
        case "half":
            StandardTray(o);
            R.Set(o.Inventory, "SelectedInventoryIndex", 1);
            R.Set(o.Hud, "PlayerHealth", 0.5f);
            if (o.Modern) R.Set(o.Hud, "PlayerStamina", 0.25f);
            if (o.Modern) Elapse(o, "StaminaBlockTimer", 0.5);
            Scenario.AimedBlock = Convert.ToInt32(Enum.Parse(blocks, "Dirt"));
            break;
        case "day":
            StandardTray(o);
            R.Set(o.Hud, "currentDay", 12);
            Elapse(o, "drawDayTimer", 1.5);
            break;
        case "underwater":
            StandardTray(o);
            Scenario.Underwater = true;
            R.Set(o.Hud, "PlayerOxygen", 0.6f);
            break;
        case "dead":
            StandardTray(o);
            R.Set(o.Player, "Dead", true);
            R.Set(o.Hud, "PlayerHealth", 0f);
            Elapse(o, "timeToShowRespawnText", 5);
            break;
        case "endurance":
            StandardTray(o);
            R.Set(o.GameInstance, "GameMode", Enum.Parse(R.Get(o.GameInstance, "GameMode").GetType(), "Endurance"));
            R.Set(o.Hud, "PlayerHealth", 0.8f);
            break;
        case "chat":
            StandardTray(o);
            object console = R.Get(o.Hud, "console");
            foreach (string line in new[] { "Kiril has joined the game", "Kiril: hello there", "Todor Has Fallen" })
                R.Invoke(console.GetType().GetMethod("WriteLine", new[] { typeof(string) }), console, new object[] { line });
            break;
        case "tray2":
            StandardTray(o);
            if (o.Modern)
            {
                o.SetTray(1, 0, o.Item("Torch", 12));
                o.SetTray(1, 5, o.Item("LogBlock", 30));
            }
            R.Set(o.Inventory, "SelectedInventoryIndex", 7);
            break;
        default:
            throw new ArgumentException("unknown scenario " + scenario);
        }
    }

    private static int Tables(Original o, string outputDirectory)
    {
        // Sprite sizes and font metrics the layout needs, straight from the content.
        var lines = new List<string>();
        foreach (KeyValuePair<string, Rectangle> sprite in o.SpriteRectangles.OrderBy(pair => pair.Key, StringComparer.Ordinal))
            lines.Add(string.Join("\t", "sprite", sprite.Key, sprite.Value.X, sprite.Value.Y, sprite.Value.Width, sprite.Value.Height));
        foreach (KeyValuePair<string, SpriteFont> font in o.Fonts.OrderBy(pair => pair.Key, StringComparer.Ordinal))
            lines.Add(string.Join("\t", "font", font.Key, font.Value.LineSpacing, font.Value.Spacing.ToString("R", Inv)));
        Type blocks = o.T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        Type blockType = o.T("DNA.CastleMinerZ.Terrain.BlockType");
        foreach (object block in Enum.GetValues(blocks))
        {
            object type;
            try { type = R.CallStatic(blockType, "GetType", block); }
            catch (Exception) { continue; }
            if (type == null) continue;
            lines.Add(string.Join("\t", "block", Convert.ToInt32(block), block, R.Get(type, "Name"), type.ToString()));
        }
        File.WriteAllLines(Path.Combine(outputDirectory, "tables.tsv"), lines, new UTF8Encoding(false));
        return 0;
    }

    private static int Measure(Original o, string inputPath, string outputPath)
    {
        // font<TAB>string per line in; font<TAB>width<TAB>height<TAB>string out.
        var lines = new List<string>();
        foreach (string line in File.ReadAllLines(inputPath))
        {
            int tab = line.IndexOf('\t');
            if (tab < 0) continue;
            SpriteFont font = o.Fonts[line.Substring(0, tab)];
            string text = line.Substring(tab + 1);
            Vector2 size = font.MeasureString(text);
            lines.Add(string.Join("\t", line.Substring(0, tab), size.X.ToString("R", Inv), size.Y.ToString("R", Inv), text));
        }
        File.WriteAllLines(outputPath, lines, new UTF8Encoding(false));
        return 0;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.HudParityProbe draw <game dir> <out dir> <WxH> [scenario ...]");
            Console.Error.WriteLine("       OpenMinerZ.HudParityProbe tables <game dir> <out dir> <WxH>");
            Console.Error.WriteLine("       OpenMinerZ.HudParityProbe measure <game dir> <out file> <WxH> <in file>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[1]);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            string[] size = args[3].Split('x');
            int width = int.Parse(size[0], Inv), height = int.Parse(size[1], Inv);
            Original original = Original.Create(gameDirectory, width, height);
            if (args[0] == "tables")
            {
                Directory.CreateDirectory(args[2]);
                return Tables(original, args[2]);
            }
            if (args[0] == "measure") return Measure(original, args[4], args[2]);
            Directory.CreateDirectory(args[2]);
            string[] scenarios = args.Length > 4
                ? args.Skip(4).ToArray()
                : new[] { "empty", "full", "hurt", "half", "day", "underwater", "dead", "endurance", "chat", "tray2" };
            string generation = original.Modern ? "modern" : "classic";
            foreach (string scenario in scenarios)
            {
                Apply(original, scenario);
                string stem = Path.Combine(args[2], generation + "-" + width + "x" + height + "-" + scenario);
                original.Draw(stem + ".tsv", stem + ".png");
                Console.WriteLine(stem + ".tsv");
            }
            return 0;
        }
        catch (Exception error)
        {
            for (Exception inner = error; inner != null; inner = inner.InnerException)
                if (inner.GetType().Name.Contains("NoSuitableGraphicsDevice") || inner.GetType().Name.Contains("DeviceNotSupported"))
                {
                    Console.Error.WriteLine("SKIP no graphics device for the original content reader: " + inner.Message);
                    return 77;
                }
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
