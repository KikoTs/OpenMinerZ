using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Resources;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// What a player READS and HEARS, taken from the user's owned, unmodified
// Castle Miner Z assemblies and Content folder rather than from a transcription.
//
//   strings       every entry of the shipped Globalization.Strings resource
//   items         InventoryItem.Initalize is executed; each registered class
//                 reports its Name and Description(1/2)
//   blocks        BlockType.GetType(id).Name for every block id
//   achievements  CastleMinerZAchievementManager.CreateAcheivements is executed;
//                 each achievement reports its API name, Name and HowToUnlock
//   fonts         each shipped SpriteFont's metrics, read with XNA's own content
//                 decompressor, and SpriteFont.MeasureString over sample text
//   cues          the cue names in the shipped XACT sound bank
//
// Nothing here starts Steam, opens a window, creates a graphics device or sends
// network traffic, and nothing from the game is copied: the output is a list of
// key=value facts under the directory the caller names.
internal static class Program
{
    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static |
                                     BindingFlags.Public | BindingFlags.NonPublic;

    private static string gameDirectory;
    private static Assembly game;
    private static Assembly common;
    private static readonly SortedDictionary<string, string> facts =
        new SortedDictionary<string, string>(StringComparer.Ordinal);
    private static int failures;

    private sealed class NoServices : IServiceProvider
    {
        public object GetService(Type serviceType) { return null; }
    }

    private sealed class InertContent : ContentManager
    {
        public InertContent() : base(new NoServices()) { }

        public override T Load<T>(string assetName)
        {
            return (T)Inert(typeof(T));
        }
    }

    private static object Inert(Type type)
    {
        object value = FormatterServices.GetUninitializedObject(type);
        GC.SuppressFinalize(value);
        return value;
    }

    private static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string path = Path.Combine(gameDirectory, new AssemblyName(args.Name).Name + ".dll");
        return File.Exists(path) ? Assembly.LoadFrom(path) : null;
    }

    private static Type T(string name)
    {
        Type type = game.GetType(name, false) ?? common.GetType(name, false);
        if (type == null) throw new InvalidDataException("Original type absent: " + name);
        return type;
    }

    private static object Get(object target, string name)
    {
        Type type = target as Type ?? target.GetType();
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, Any | BindingFlags.DeclaredOnly);
            if (field != null) return field.GetValue(target is Type ? null : target);
            PropertyInfo property = current.GetProperty(name, Any | BindingFlags.DeclaredOnly);
            if (property != null && property.GetIndexParameters().Length == 0)
                return property.GetValue(target is Type ? null : target, null);
        }
        throw new MissingMemberException(type.FullName, name);
    }

    private static bool Has(object target, string name)
    {
        Type type = target as Type ?? target.GetType();
        for (Type current = type; current != null; current = current.BaseType)
            if (current.GetField(name, Any | BindingFlags.DeclaredOnly) != null ||
                current.GetProperty(name, Any | BindingFlags.DeclaredOnly) != null)
                return true;
        return false;
    }

    private static void Set(object target, string name, object value)
    {
        Type type = target as Type ?? target.GetType();
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, Any | BindingFlags.DeclaredOnly);
            if (field != null)
            {
                field.SetValue(target is Type ? null : target, value);
                return;
            }
            PropertyInfo property = current.GetProperty(name, Any | BindingFlags.DeclaredOnly);
            if (property != null && property.GetSetMethod(true) != null)
            {
                property.SetValue(target is Type ? null : target, value, null);
                return;
            }
        }
        throw new MissingMemberException(type.FullName, name);
    }

    private static object Call(object target, string name, params object[] arguments)
    {
        Type type = target as Type ?? target.GetType();
        for (Type current = type; current != null; current = current.BaseType)
        {
            foreach (MethodInfo method in current.GetMethods(Any | BindingFlags.DeclaredOnly))
            {
                if (method.Name != name || method.IsGenericMethodDefinition) continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length != arguments.Length) continue;
                bool fits = true;
                for (int index = 0; index < parameters.Length && fits; ++index)
                {
                    Type wanted = parameters[index].ParameterType;
                    object argument = arguments[index];
                    fits = argument == null ? !wanted.IsValueType : wanted.IsInstanceOfType(argument);
                }
                if (!fits) continue;
                try
                {
                    return method.Invoke(target is Type ? null : target, arguments);
                }
                catch (TargetInvocationException error)
                {
                    System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
                }
            }
        }
        throw new MissingMethodException(type.FullName, name);
    }

    // Values are single-line: the three characters that would break the
    // key=value format are written as the two-character escapes the native
    // side (tests/test_presentation_parity.cpp) writes too.
    private static string Escape(string text)
    {
        return (text ?? "").Replace("\\", "\\\\").Replace("\r", "\\r").Replace("\n", "\\n");
    }

    private static void Fact(string key, object value)
    {
        facts[key] = value is string text ? Escape(text) : Convert.ToString(value, CultureInfo.InvariantCulture);
    }

    private static void Run(string name, Action body)
    {
        try
        {
            body();
            facts["scenario." + name] = "executed";
        }
        catch (Exception error)
        {
            while (error.InnerException != null) error = error.InnerException;
            ++failures;
            facts["scenario." + name] = "failed";
            Console.WriteLine("FAIL " + name + ": " + error.GetType().Name + ": " + error.Message);
            Console.WriteLine(error.StackTrace);
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length == 4 && args[0] == "--generate")
            return Generator.Run(Path.GetFullPath(args[1]), Path.GetFullPath(args[2]), Path.GetFullPath(args[3]));
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.PresentationParityProbe <owned game directory> <output directory>");
            Console.Error.WriteLine("       OpenMinerZ.PresentationParityProbe --generate <classic facts> <modern facts> <repository root>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        string output = Path.GetFullPath(args[1]);
        Directory.CreateDirectory(output);
        // The shipped resources are looked up for the invariant culture so the
        // probe reports the English table whatever the machine's language is.
        System.Threading.Thread.CurrentThread.CurrentUICulture = CultureInfo.InvariantCulture;
        System.Threading.Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            using (var sha = SHA256.Create())
                facts["info.assembly"] = game.GetName().Version + " sha256=" +
                    BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(game.Location)))
                        .Replace("-", "").ToLowerInvariant();
            Console.WriteLine("Assembly " + facts["info.assembly"]);

            Run("strings", DumpStrings);
            Run("bootstrap", Bootstrap);
            Run("items", DumpItems);
            Run("blocks", DumpBlocks);
            Run("achievements", DumpAchievements);
            Run("fonts", DumpFonts);
            Run("cues", DumpCues);

            using (var writer = new StreamWriter(Path.Combine(output, "presentation-facts.txt"), false,
                                                 new UTF8Encoding(false)))
                foreach (var fact in facts) writer.WriteLine(fact.Key + "=" + fact.Value);
            Console.WriteLine("Presentation facts: " + facts.Count + ", failures: " + failures);
            return failures == 0 ? 0 : 1;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 2;
        }
    }

    // ---------------------------------------------------------------- strings

    private static void DumpStrings()
    {
        int count = 0;
        foreach (Assembly assembly in new[] { game, common })
        {
            foreach (string resource in assembly.GetManifestResourceNames())
            {
                if (!resource.EndsWith(".resources", StringComparison.Ordinal)) continue;
                string table = resource.Substring(0, resource.Length - ".resources".Length);
                // The game's own table is "strings.<key>"; DNA.Common's small
                // shared tables keep their resource name so keys cannot collide.
                string prefix = table == "DNA.CastleMinerZ.Globalization.Strings" ? "string." : "resource." + table + ".";
                using (Stream stream = assembly.GetManifestResourceStream(resource))
                using (var reader = new ResourceReader(stream))
                {
                    IDictionaryEnumerator entries = reader.GetEnumerator();
                    while (entries.MoveNext())
                    {
                        string type;
                        byte[] data;
                        reader.GetResourceData((string)entries.Key, out type, out data);
                        if (type != "ResourceTypeCode.String") continue;
                        using (var value = new BinaryReader(new MemoryStream(data), Encoding.UTF8))
                            Fact(prefix + entries.Key, value.ReadString());
                        if (prefix == "string.") ++count;
                    }
                }
            }
        }
        Fact("string.count", count);
        Console.WriteLine("PASS strings: " + count + " entries in Globalization.Strings");
    }

    // -------------------------------------------------------------- bootstrap

    private static object gameInstance;

    private static void Bootstrap()
    {
        // An inert field container in place of the game object: no constructor,
        // window, graphics device, Steam or update loop is ever run.
        Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
        Call(T("DNA.Reflection.ReflectionTools"), "RegisterAssembly", game, common);
        gameInstance = Inert(gameType);
        Set(gameType, "Instance", gameInstance);
        var content = new InertContent();
        bool placed = false;
        for (Type current = gameType; current != null && !placed; current = current.BaseType)
            foreach (FieldInfo field in current.GetFields(Any | BindingFlags.DeclaredOnly))
                if (!field.IsStatic && field.FieldType == typeof(ContentManager))
                {
                    field.SetValue(gameInstance, content);
                    placed = true;
                }
        if (!placed) throw new InvalidDataException("Game.Content backing field not found");
    }

    // ------------------------------------------------------------------ items

    private static void DumpItems()
    {
        Type item = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        // The real registration routine: every class constructor runs with the
        // strings the shipped build passes it.
        Call(item, "Initalize", (ContentManager)Get(gameInstance, "Content"));
        var catalog = (IDictionary)Get(item, "AllItems");
        Fact("item.count", catalog.Count);
        foreach (DictionaryEntry entry in catalog)
        {
            int id = Convert.ToInt32(entry.Key);
            object itemClass = entry.Value;
            string key = "item." + id + ".";
            Fact(key + "enum", entry.Key.ToString());
            Fact(key + "name", (string)Get(itemClass, "Name"));
            // 1.6.3.8 keeps two lines and draws them separately; 1.9.9.8 joins
            // them into one Description when the class is registered.
            if (Has(itemClass, "Description1"))
            {
                Fact(key + "description1", (string)Get(itemClass, "Description1"));
                Fact(key + "description2", (string)Get(itemClass, "Description2"));
            }
            if (Has(itemClass, "Description"))
                Fact(key + "description", (string)Get(itemClass, "Description"));
        }
        Console.WriteLine("PASS items: " + catalog.Count + " registered classes");
    }

    // ----------------------------------------------------------------- blocks

    private static void DumpBlocks()
    {
        Type blockEnum = T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        Type blockType = T("DNA.CastleMinerZ.Terrain.BlockType");
        int count = Convert.ToInt32(Enum.Parse(blockEnum, "NumberOfBlocks"));
        Fact("block.count", count);
        for (int block = 0; block < count; ++block)
        {
            object value = Enum.ToObject(blockEnum, block);
            object description = Call(blockType, "GetType", value);
            Fact("block." + block + ".enum", value.ToString());
            Fact("block." + block + ".name", (string)Get(description, "Name"));
        }
        Console.WriteLine("PASS blocks: " + count + " block types");
    }

    // ----------------------------------------------------------- achievements

    private static void DumpAchievements()
    {
        Type managerType = T("DNA.CastleMinerZ.Achievements.CastleMinerZAchievementManager");
        // The constructor only stores the game and its (absent) stats; the
        // achievements read them lazily, and nothing here asks for progress.
        object manager = Inert(managerType);
        FieldInfo list = null;
        for (Type current = managerType; current != null && list == null; current = current.BaseType)
            list = current.GetFields(Any | BindingFlags.DeclaredOnly)
                .FirstOrDefault(field => field.FieldType.IsGenericType &&
                                         field.FieldType.GetGenericTypeDefinition() == typeof(List<>));
        if (list != null && list.GetValue(manager) == null)
            list.SetValue(manager, Activator.CreateInstance(list.FieldType));
        FieldInfo array = managerType.GetField("Achievements", Any);
        if (array != null && array.GetValue(manager) == null)
        {
            // The field initializer did not run on the inert instance; the
            // shipped constructor sizes it, so read that size from its IL-free
            // source of truth: CreateAcheivements indexes 0..N-1, so grow on demand.
            array.SetValue(manager, Array.CreateInstance(array.FieldType.GetElementType(), 64));
        }
        Call(manager, "CreateAcheivements");
        var achievements = (Array)array.GetValue(manager);
        int count = 0;
        foreach (object achievement in achievements)
        {
            if (achievement == null) continue;
            string key = "achievement." + count + ".";
            Fact(key + "api", (string)Get(achievement, "APIName"));
            Fact(key + "name", (string)Get(achievement, "Name"));
            Fact(key + "how", (string)Get(achievement, "HowToUnlock"));
            Fact(key + "class", achievement.GetType().Name);
            ++count;
        }
        Fact("achievement.count", count);
        Console.WriteLine("PASS achievements: " + count);
    }

    // ------------------------------------------------------------------ fonts

    private static int Read7Bit(BinaryReader reader)
    {
        int value = 0, shift = 0, part;
        do { part = reader.ReadByte(); value |= (part & 0x7F) << shift; shift += 7; } while ((part & 0x80) != 0);
        return value;
    }

    private sealed class FontData
    {
        public SpriteFont Font;
        public List<Rectangle> Glyphs;
        public List<Rectangle> Cropping;
        public List<char> Characters;
        public List<Vector3> Kerning;
        public int LineSpacing;
        public float Spacing;
        public char? Fallback;
        public int TextureWidth;
        public int TextureHeight;
        public int TextureFormat;
    }

    // Reads a shipped sprite font with XNA's own decompressor and builds a real
    // XNA SpriteFont without a texture so MeasureString can run.
    private static FontData LoadFont(string file)
    {
        MethodInfo prepare = typeof(ContentReader).GetMethod("PrepareStream", BindingFlags.Static | BindingFlags.NonPublic);
        object[] arguments = { File.OpenRead(file), Path.GetFileName(file), 0 };
        using (var stream = (Stream)prepare.Invoke(null, arguments))
        using (var reader = new BinaryReader(stream, Encoding.UTF8))
        {
            var data = new FontData();
            int readers = Read7Bit(reader);
            for (int index = 0; index < readers; ++index) { reader.ReadString(); reader.ReadInt32(); }
            Read7Bit(reader);                       // shared resources
            Read7Bit(reader);                       // SpriteFont
            Read7Bit(reader);                       // Texture2D
            data.TextureFormat = reader.ReadInt32();
            data.TextureWidth = (int)reader.ReadUInt32();
            data.TextureHeight = (int)reader.ReadUInt32();
            uint mips = reader.ReadUInt32();
            for (uint mip = 0; mip < mips; ++mip) reader.ReadBytes((int)reader.ReadUInt32());
            Func<List<Rectangle>> rectangles = () =>
            {
                Read7Bit(reader);
                var list = new List<Rectangle>();
                for (uint count = reader.ReadUInt32(); count != 0; --count)
                    list.Add(new Rectangle(reader.ReadInt32(), reader.ReadInt32(), reader.ReadInt32(), reader.ReadInt32()));
                return list;
            };
            data.Glyphs = rectangles();
            data.Cropping = rectangles();
            Read7Bit(reader);
            data.Characters = new List<char>();
            for (uint count = reader.ReadUInt32(); count != 0; --count) data.Characters.Add(reader.ReadChar());
            data.LineSpacing = reader.ReadInt32();
            data.Spacing = reader.ReadSingle();
            Read7Bit(reader);
            data.Kerning = new List<Vector3>();
            for (uint count = reader.ReadUInt32(); count != 0; --count)
                data.Kerning.Add(new Vector3(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle()));
            data.Fallback = reader.ReadBoolean() ? reader.ReadChar() : (char?)null;
            ConstructorInfo constructor = typeof(SpriteFont).GetConstructors(BindingFlags.Instance | BindingFlags.NonPublic).Single();
            data.Font = (SpriteFont)constructor.Invoke(new object[]
            {
                null, data.Glyphs, data.Cropping, data.Characters, data.LineSpacing, data.Spacing, data.Kerning, data.Fallback
            });
            return data;
        }
    }

    // Shared with tests/test_presentation_parity.cpp (font_samples).
    private static readonly string[] fontSamples =
    {
        "H", "Hg", "Stone PickAxe", "Click To Respawn", "Loading The World... ", "AVATAR To.", "1234567890",
        "Two\nLines", "tab\there", "na\u00efve caf\u00e9", "\u2603 snow"
    };

    private static string Float(float value)
    {
        return value.ToString("R", CultureInfo.InvariantCulture);
    }

    private static void DumpFonts()
    {
        var files = new List<string>();
        foreach (string folder in new[] { "720", "Fonts", "UI", "" })
        {
            string directory = Path.Combine(gameDirectory, "Content", folder);
            if (!Directory.Exists(directory)) continue;
            foreach (string file in Directory.GetFiles(directory, "*.xnb"))
            {
                string name = Path.GetFileNameWithoutExtension(file);
                // The per-language variants (".ja", ".ru") are satellite fonts.
                if (name.Contains(".")) continue;
                if (!name.EndsWith("Font", StringComparison.Ordinal) && !name.StartsWith("Myriad", StringComparison.Ordinal) &&
                    name != "System" && !name.StartsWith("TwitchFont", StringComparison.Ordinal))
                    continue;
                files.Add(file);
            }
        }
        int count = 0;
        foreach (string file in files.OrderBy(Path.GetFileNameWithoutExtension, StringComparer.Ordinal))
        {
            string name = Path.GetFileNameWithoutExtension(file);
            FontData data;
            try { data = LoadFont(file); }
            catch (Exception) { continue; }     // not a SpriteFont
            string key = "font." + name + ".";
            Fact(key + "folder", Path.GetFileName(Path.GetDirectoryName(file)));
            Fact(key + "line_spacing", data.LineSpacing);
            Fact(key + "spacing", Float(data.Spacing));
            Fact(key + "glyphs", data.Characters.Count);
            Fact(key + "default", data.Fallback.HasValue ? (int)data.Fallback.Value : -1);
            Fact(key + "first", (int)data.Characters.Min());
            Fact(key + "last", (int)data.Characters.Max());
            Fact(key + "texture", data.TextureWidth + "x" + data.TextureHeight + " format " + data.TextureFormat);
            int capital = data.Characters.IndexOf('H');
            Fact(key + "cap_height", data.Glyphs[capital].Height);
            Fact(key + "cap_top", data.Cropping[capital].Y);
            // A checksum over the whole glyph table: the native loader's copy of
            // the imported font must reproduce every rectangle and advance.
            unchecked
            {
                uint hash = 2166136261u;
                Action<int> mix = value => { hash = (hash ^ (uint)value) * 16777619u; };
                for (int index = 0; index < data.Characters.Count; ++index)
                {
                    mix(data.Characters[index]);
                    Rectangle glyph = data.Glyphs[index], crop = data.Cropping[index];
                    mix(glyph.X); mix(glyph.Y); mix(glyph.Width); mix(glyph.Height);
                    mix(crop.X); mix(crop.Y); mix(crop.Width); mix(crop.Height);
                    Vector3 kern = data.Kerning[index];
                    mix(BitConverter.ToInt32(BitConverter.GetBytes(kern.X), 0));
                    mix(BitConverter.ToInt32(BitConverter.GetBytes(kern.Y), 0));
                    mix(BitConverter.ToInt32(BitConverter.GetBytes(kern.Z), 0));
                }
                Fact(key + "table_hash", hash.ToString("x8"));
            }
            for (int sample = 0; sample < fontSamples.Length; ++sample)
            {
                string outcome;
                try
                {
                    Vector2 size = data.Font.MeasureString(fontSamples[sample]);
                    outcome = Float(size.X) + "x" + Float(size.Y);
                }
                catch (ArgumentException)
                {
                    // No default character: SpriteFont throws on a glyph it lacks.
                    outcome = "throws";
                }
                Fact(key + "measure." + sample, outcome);
            }
            ++count;
        }
        Fact("font.count", count);
        Console.WriteLine("PASS fonts: " + count + " sprite fonts measured");
    }

    // ------------------------------------------------------------------- cues

    // XACT3 sound bank header (the documented layout MonoGame and FNA read).
    private static void DumpCues()
    {
        byte[] bank = File.ReadAllBytes(Path.Combine(gameDirectory, "Content", "Sounds.xsb"));
        using (var reader = new BinaryReader(new MemoryStream(bank)))
        {
            if (reader.ReadUInt32() != 0x4B424453u) throw new InvalidDataException("Sounds.xsb is not an XACT sound bank");
            reader.ReadUInt16();                    // tool version
            reader.ReadUInt16();                    // format version
            reader.ReadUInt16();                    // crc
            reader.ReadUInt32(); reader.ReadUInt32(); // last modified
            reader.ReadByte();                      // platform
            ushort simple = reader.ReadUInt16();
            ushort complex = reader.ReadUInt16();
            reader.ReadUInt16();
            ushort total = reader.ReadUInt16();
            reader.ReadByte();                      // wave banks
            reader.ReadUInt16();                    // sounds
            uint nameLength = reader.ReadUInt32();
            reader.ReadUInt32();                    // simple cues offset
            reader.ReadUInt32();                    // complex cues offset
            uint nameOffset = reader.ReadUInt32();
            var names = new List<string>();
            int position = (int)nameOffset;
            int end = position + (int)nameLength;
            while (position < end && names.Count < total)
            {
                int start = position;
                while (position < end && bank[position] != 0) ++position;
                names.Add(Encoding.ASCII.GetString(bank, start, position - start));
                ++position;
            }
            Fact("cue.count", names.Count);
            Fact("cue.simple", (int)simple);
            Fact("cue.complex", (int)complex);
            foreach (string name in names) Fact("cue.name." + name, 1);
            Console.WriteLine("PASS cues: " + names.Count + " (" + simple + " simple, " + complex + " complex)");
        }
    }
}
