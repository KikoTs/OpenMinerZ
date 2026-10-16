#nullable disable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Threading;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;
using OpenMinerZ.ContentBridge;

// Art for the inventory, crate and crafting screens, taken from the owned
// install into DerivedData:
//
//   screen_<Sprite>.bmp     the sprites those screens draw, cropped from the
//                           build's SpriteSheet (BlockUIBack, Selector,
//                           SingleGrid, InventoryGrid, HudGrid; 1.9.9.8 adds
//                           Tier2Back, CraftSelector and the four group icons)
//   screen_CtrlFrame.bmp    the nine pieces of the button frame as one 5x5
//                           nine-slice (2 px corners and edges, 1 px centre)
//   screen_MousePointer.bmp the game's own pointer
//   item_icons.bmp          InventoryItem._2DImages: one 64x64 icon per item id,
//                           eight per row, exactly as the game renders it
//   item_icons_large.bmp    1.9.9.8's 128x128 atlas (HiDef profile only)
//   screen_assets.txt       sizes of everything above
//
// The icon atlas is not content: the game renders every item's 3D model into
// a render target the first time an icon is needed. This pass therefore runs
// the game's OWN InventoryItem.FinishInitialization on a hidden device, with
// the item classes its own InventoryItem.Initalize registers, and saves what
// comes out. Pixels are stored as XNA holds them (premultiplied alpha).
namespace OpenMinerZ.ScreenAssets
{
    internal static class ScreenAssetExporter
    {
        private const BindingFlags All = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public |
                                         BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

        private sealed class Device : IGraphicsDeviceService
        {
            public GraphicsDevice GraphicsDevice { get; set; }
#pragma warning disable 67
            public event EventHandler<EventArgs> DeviceCreated;
            public event EventHandler<EventArgs> DeviceDisposing;
            public event EventHandler<EventArgs> DeviceReset;
            public event EventHandler<EventArgs> DeviceResetting;
#pragma warning restore 67
        }

        private sealed class Services : IServiceProvider
        {
            public readonly Dictionary<Type, object> Table = new Dictionary<Type, object>();
            public object GetService(Type type) { object value; return Table.TryGetValue(type, out value) ? value : null; }
        }

        private static MemberInfo Find(Type type, string name)
        {
            for (Type current = type; current != null; current = current.BaseType)
            {
                FieldInfo field = current.GetField(name, All);
                if (field != null) return field;
                PropertyInfo property = current.GetProperty(name, All);
                if (property != null) return property;
            }
            return null;
        }

        private static object Get(Type type, object target, string name)
        {
            MemberInfo member = Find(type, name) ?? throw new MissingMemberException(type.FullName, name);
            return member is FieldInfo field ? field.GetValue(target) : ((PropertyInfo)member).GetValue(target, null);
        }

        private static void Set(Type type, object target, string name, object value)
        {
            MemberInfo member = Find(type, name) ?? throw new MissingMemberException(type.FullName, name);
            if (member is FieldInfo field) field.SetValue(target, value);
            else ((PropertyInfo)member).SetValue(target, value, null);
        }

        private static object Get(object target, string name) { return Get(target.GetType(), target, name); }
        private static void Set(object target, string name, object value) { Set(target.GetType(), target, name, value); }

        private static object Call(Type type, object target, string name, params object[] arguments)
        {
            for (Type current = type; current != null; current = current.BaseType)
                foreach (MethodInfo method in current.GetMethods(All))
                    if (method.Name == name && method.GetParameters().Length == arguments.Length && !method.IsGenericMethodDefinition)
                        return Unwrap(() => method.Invoke(target, arguments));
            throw new MissingMethodException(type.FullName, name);
        }

        private static object New(Type type, params object[] arguments)
        {
            foreach (ConstructorInfo constructor in type.GetConstructors(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                ParameterInfo[] parameters = constructor.GetParameters();
                if (parameters.Length != arguments.Length) continue;
                bool match = true;
                for (int index = 0; index < parameters.Length && match; ++index)
                    match = arguments[index] == null ? !parameters[index].ParameterType.IsValueType
                                                     : parameters[index].ParameterType.IsInstanceOfType(arguments[index]);
                if (match) return Unwrap(() => constructor.Invoke(arguments));
            }
            throw new MissingMethodException(type.FullName, ".ctor/" + arguments.Length);
        }

        private static object Unwrap(Func<object> call)
        {
            try { return call(); }
            catch (TargetInvocationException error)
            {
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
                throw;
            }
        }

        private static object Blank(Type type)
        {
            object value = FormatterServices.GetUninitializedObject(type);
            GC.SuppressFinalize(value);
            return value;
        }

        // Tree<Entity>'s field initialisers, for an entity made without its constructor.
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

        private static Pixel[] Pixels(Texture2D texture, Rectangle? region = null)
        {
            Rectangle area = region ?? new Rectangle(0, 0, texture.Width, texture.Height);
            var colors = new Color[area.Width * area.Height];
            texture.GetData(0, area, colors, 0, colors.Length);
            var pixels = new Pixel[colors.Length];
            for (int index = 0; index < colors.Length; ++index)
                pixels[index] = new Pixel(colors[index].R, colors[index].G, colors[index].B, colors[index].A);
            return pixels;
        }

        public static int Run(string gameDirectory, string outputDirectory)
        {
            Thread.CurrentThread.CurrentCulture = new CultureInfo("en-US");
            Thread.CurrentThread.CurrentUICulture = new CultureInfo("en-US");
            Directory.CreateDirectory(outputDirectory);
            AppDomain.CurrentDomain.AssemblyResolve += (sender, resolve) =>
            {
                string name = new AssemblyName(resolve.Name).Name;
                foreach (string extension in new[] { ".dll", ".exe" })
                {
                    string path = Path.Combine(gameDirectory, name + extension);
                    if (File.Exists(path)) return Assembly.LoadFrom(path);
                }
                return null;
            };
            Assembly game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            Assembly common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            Func<string, Type> T = name => game.GetType(name, false) ?? common.GetType(name, true);
            bool modern = game.GetType("DNA.CastleMinerZ.UI.CraftingScreen", false) != null;
            var manifest = new List<string> { "generation=" + (modern ? "modern" : "classic") };

            var service = new Device();
            var form = new System.Windows.Forms.Form();
            var parameters = new PresentationParameters
            {
                BackBufferWidth = 64, BackBufferHeight = 64, DeviceWindowHandle = form.Handle,
                IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
            };
            GraphicsProfile profile = GraphicsProfile.HiDef;
            try { service.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, profile, parameters); }
            catch (Exception)
            {
                profile = GraphicsProfile.Reach;
                service.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, profile, parameters);
            }
            GraphicsDevice device = service.GraphicsDevice;
            manifest.Add("profile=" + profile);
            var services = new Services();
            services.Table[typeof(IGraphicsDeviceService)] = service;

            // An inert game: no window, Steam, audio or session. Only what the
            // item classes read while they draw their own icons is filled in.
            Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
            object instance = Blank(gameType);
            Set(gameType, null, "Instance", instance);
            foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
                if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(instance, service);
            Type profiles = T("DNA.CastleMinerZ.GraphicsProfileSupport.GraphicsProfileManager");
            Set(Get(profiles, null, "Instance"), "_profile", profile);
            Type locker = T("DNA.Drawing.GraphicsDeviceLocker");
            if (Get(locker, null, "Instance") == null) Set(locker, null, "Instance", New(locker));
            Type arguments = game.GetType("DNA.CastleMinerZ.CastleMinerZArgs", false);
            if (arguments != null && Find(arguments, "Instance") != null && Get(arguments, null, "Instance") == null)
            {
                object parsed = Blank(arguments);
                if (Find(arguments, "TextureQualityLevel") != null) Set(parsed, "TextureQualityLevel", 1);
                Set(arguments, null, "Instance", parsed);
            }
            // The game's own content manager: root first, then the profile folder.
            Type contentType = T("DNA.CastleMinerZ.GraphicsProfileSupport.ProfiledContentManager");
            var content = (ContentManager)(modern ? New(contentType, services, "ReachContent", "HiDefContent", 1)
                                                  : New(contentType, services, "ReachContent", "HiDefContent"));
            content.RootDirectory = Path.Combine(gameDirectory, "Content");
            ((Game)instance).Content = content;
            Set(instance, "CurrentGameTime", new GameTime());

            // ---- sprites ----------------------------------------------------
            Type managerType = T("DNA.Drawing.SpriteManager");
            MethodInfo load = typeof(ContentManager).GetMethod("Load");
            Func<string, object> loadSheet = asset => Unwrap(() => load.MakeGenericMethod(managerType).Invoke(content, new object[] { asset }));
            object sheet = loadSheet(modern ? "UI\\SpriteSheet" : "SpriteSheet");
            // 1.6.3.8 keeps the button frame in the TwitchLogos sheet.
            object frameSheet = modern ? sheet : loadSheet("TwitchLogos");
            Func<object, string, object> sprite = (manager, name) =>
            {
                var table = (IDictionary)Get(manager, "_sprites");
                return table.Contains(name) ? table[name] : null;
            };
            foreach (string name in new[] { "BlockUIBack", "Selector", "SingleGrid", "InventoryGrid", "HudGrid", "Tier2Back",
                                            "CraftSelector", "MaterialsIcon", "ToolsIcon", "WeaponsIcon", "StructuresIcon" })
            {
                object entry = sprite(sheet, name);
                if (entry == null) continue;
                var source = (Rectangle)Get(entry, "SourceRectangle");
                BitmapWriter.Write(Path.Combine(outputDirectory, "screen_" + name + ".bmp"),
                                   Pixels((Texture2D)Get(entry, "Texture"), source), source.Width, source.Height);
                manifest.Add("sprite " + name + " " + source.Width + " " + source.Height);
            }
            {
                // tl t tr / l c r / bl b br, each stretched piece one texel wide.
                var frame = new Pixel[25];
                string[] rows = { "_tl _t _tr", "_l _c _r", "_bl _b _br" };
                int[] originY = { 0, 2, 3 }, originX = { 0, 2, 3 };
                bool complete = true;
                for (int row = 0; row < 3 && complete; ++row)
                {
                    string[] suffixes = rows[row].Split(' ');
                    for (int column = 0; column < 3 && complete; ++column)
                    {
                        object entry = sprite(frameSheet, "CtrlFrame" + suffixes[column]);
                        if (entry == null) { complete = false; break; }
                        var source = (Rectangle)Get(entry, "SourceRectangle");
                        int width = column == 1 ? 1 : 2, height = row == 1 ? 1 : 2;
                        if (source.Width < width || source.Height < height) { complete = false; break; }
                        Pixel[] piece = Pixels((Texture2D)Get(entry, "Texture"), new Rectangle(source.X, source.Y, width, height));
                        for (int y = 0; y < height; ++y)
                            for (int x = 0; x < width; ++x)
                                frame[originX[column] + x + (originY[row] + y) * 5] = piece[x + y * width];
                    }
                }
                if (complete)
                {
                    BitmapWriter.Write(Path.Combine(outputDirectory, "screen_CtrlFrame.bmp"), frame, 5, 5);
                    manifest.Add("sprite CtrlFrame 5 5");
                }
            }
            var pointer = content.Load<Texture2D>("MousePointer");
            BitmapWriter.Write(Path.Combine(outputDirectory, "screen_MousePointer.bmp"), Pixels(pointer), pointer.Width, pointer.Height);
            manifest.Add("sprite MousePointer " + pointer.Width + " " + pointer.Height);

            // ---- item icons -------------------------------------------------
            // CastleMinerZGame.SecondaryLoad, as far as the item classes need it.
            Set(instance, "_terrain", modern ? New(T("DNA.CastleMinerZ.Terrain.BlockTerrain"), content)
                                             : New(T("DNA.CastleMinerZ.Terrain.BlockTerrain"), device, content));
            Type itemType = T("DNA.CastleMinerZ.Inventory.InventoryItem");
            Call(itemType, null, "Initalize", content);
            Call(T("DNA.CastleMinerZ.BlockEntity"), null, "Initialize");
            foreach (string name in new[] { "RocketEntity", "BlasterShot", "GrenadeProjectile", "ExplosiveFlashEntity", "Explosive",
                                            "GPSMarkerEntity", "GunEntity", "TorchCloud", "TorchEntity" })
            {
                Type type = game.GetTypes().FirstOrDefault(candidate => candidate.Name == name);
                MethodInfo init = type == null ? null : type.GetMethod("Init", BindingFlags.Public | BindingFlags.Static, null, Type.EmptyTypes, null);
                if (init == null) continue;
                try { Unwrap(() => init.Invoke(null, null)); }
                catch (Exception error) { manifest.Add("note " + name + ".Init " + error.GetType().Name); }
            }
            // The clock icon's hand shows the time of day and the locator
            // icons' needles point from the player to the origin, as they stand
            // when the atlas is first drawn. Use a new world: CastleMinerSky.Day
            // at its initial 0.41, empty hands, the player beside the origin.
            object player = Blank(T("DNA.CastleMinerZ.Player"));
            InitializeTreeNode(player);
            Set(player, "_localToWorld", Matrix.CreateTranslation(new Vector3(0.5f, 10f, 0.5f)));
            Set(player, "_ltwDirty", false);
            Set(instance, "_localPlayer", player);
            Set(player, "PlayerInventory", New(T("DNA.CastleMinerZ.Inventory.PlayerInventory"), player, false));
            object hud = Blank(T("DNA.CastleMinerZ.UI.InGameHUD"));
            Set(hud, "_game", instance);
            Set(hud.GetType(), null, "Instance", hud);
            object gameScreen = Blank(T("DNA.CastleMinerZ.GameScreen"));
            Set(gameScreen, "_game", instance);
            Set(gameScreen, "_inGameUI", hud);
            object sky = Blank(T("DNA.CastleMinerZ.CastleMinerSky"));
            Set(sky, "Day", 0.41f);
            Set(gameScreen, "_sky", sky);
            Set(instance, "GameScreen", gameScreen);
            Call(itemType, null, "FinishInitialization", device);
            device.SetRenderTarget(null);

            foreach (var atlas in new[] { new { Field = "_2DImages", File = "item_icons.bmp", Cell = 64 },
                                          new { Field = "_2DImagesLarge", File = "item_icons_large.bmp", Cell = 128 } })
            {
                if (Find(itemType, atlas.Field) == null) continue;
                var texture = (Texture2D)Get(itemType, null, atlas.Field);
                if (texture == null) continue;
                BitmapWriter.Write(Path.Combine(outputDirectory, atlas.File), Pixels(texture), texture.Width, texture.Height);
                manifest.Add("atlas " + atlas.File + " " + texture.Width + " " + texture.Height + " cell=" + atlas.Cell);
            }
            // Which ids the build registers (an unregistered id has an empty cell).
            var registered = new List<int>();
            foreach (DictionaryEntry entry in (IDictionary)Get(itemType, null, "AllItems"))
                registered.Add(Convert.ToInt32(entry.Key, CultureInfo.InvariantCulture));
            registered.Sort();
            manifest.Add("items " + string.Join(",", registered));
            File.WriteAllLines(Path.Combine(outputDirectory, "screen_assets.txt"), manifest);
            Console.WriteLine("Inventory, crate and crafting screen art: " + outputDirectory);
            return 0;
        }
    }
}
