using System;
using System.Collections.Generic;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;

// Runs serializers from the user's owned original assemblies in an isolated
// x86 process. It never runs a game constructor, enters Steam, captures audio,
// or sends network traffic.
internal static class Program
{
    private static string gameDirectory;

    private static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string path = Path.Combine(gameDirectory, new AssemblyName(args.Name).Name + ".dll");
        return File.Exists(path) ? Assembly.LoadFrom(path) : null;
    }

    private static IEnumerable<Type> Types(Assembly assembly)
    {
        try { return assembly.GetTypes(); }
        catch (ReflectionTypeLoadException error) { return error.Types.Where(type => type != null); }
    }

    private static void InitializeSerializerCatalog(Assembly game)
    {
        // InventoryItem.Create dispatches through a runtime catalog normally
        // populated while loading GPU models. Supply model-free instances of
        // the ORIGINAL base-item and gun factories needed by these fixtures.
        // This verifies original wire readers/writers, not item balance or models.
        Type item = game.GetType("DNA.CastleMinerZ.Inventory.InventoryItem", true);
        Type itemClass = item.GetNestedType("InventoryItemClass", BindingFlags.Public | BindingFlags.NonPublic);
        Type ids = game.GetType("DNA.CastleMinerZ.Inventory.InventoryItemIDs", true);
        var catalog = (IDictionary)item.GetField("AllItems", BindingFlags.Static | BindingFlags.NonPublic).GetValue(null);
        foreach (int id in Enum.GetValues(ids).Cast<object>().Select(Convert.ToInt32).Distinct())
        {
            Type factory = game.GetType(id == 51 ? "DNA.CastleMinerZ.Inventory.PistolInventoryItemClass" :
                "DNA.CastleMinerZ.Inventory.ModelInventoryItemClass", true);
            object value = FormatterServices.GetUninitializedObject(factory);
            object key = Enum.ToObject(ids, id);
            itemClass.GetField("ID").SetValue(value, key);
            itemClass.GetField("MaxStackCount").SetValue(value, id == 44 ? 200 : (id == 2 || id == 14 ? 64 : 1));
            catalog[key] = value;
        }
        // Modern's post-load recipe discovery reads GameMode from Instance.
        // Supply an inert field container without calling a Game constructor,
        // loading a graphics device, starting Steam or running an update loop.
        Type gameType = game.GetType("DNA.CastleMinerZ.CastleMinerZGame", true);
        var instance = gameType.GetField("Instance", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
        object context = FormatterServices.GetUninitializedObject(gameType);
        GC.SuppressFinalize(context);
        instance.SetValue(null, context);
    }

    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.PacketContractProbe <owned game directory> <native fixture directory>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            var game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            var common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            InitializeSerializerCatalog(game);
            var types = Types(game).Concat(Types(common)).Where(type => !type.IsAbstract)
                .GroupBy(type => type.Name).ToDictionary(group => group.Key, group => group.First());
            using (var sha = SHA256.Create())
                Console.WriteLine("Assembly " + game.GetName().Version + " SHA256=" +
                    BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(game.Location))).Replace("-", "").ToLowerInvariant());
            Type baseType = common.GetType("DNA.Net.Message", true);
            var expected = new HashSet<string>(types.Values.Where(type => baseType.IsAssignableFrom(type)).Select(type => type.Name));
            var exercised = new HashSet<string>();
            int passed = 0, failed = 0;
            foreach (string file in Directory.EnumerateFiles(args[1], "*.bin").OrderBy(path => path))
            {
                string name = Path.GetFileNameWithoutExtension(file);
                string messageName = name.Split('.')[0];
                try
                {
                    if (!types.TryGetValue(messageName, out Type type))
                        throw new InvalidDataException("Message absent from original assembly");
                    var read = type.GetMethod("RecieveData", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    var write = type.GetMethod("SendData", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    if (read == null || write == null)
                        throw new InvalidDataException("Original serializer methods absent");
                    object message = Activator.CreateInstance(type, true);
                    // Inventory messages read Sender.Tag to obtain their owning
                    // Player. A disconnected, inert gamer with a null tag is enough
                    // for the original PlayerInventory(player, false) constructor.
                    Type messageBase = common.GetType("DNA.Net.Message", true);
                    Type gamerType = common.GetType("DNA.Net.GamerServices.NetworkGamer", true);
                    messageBase.GetField("_sender", BindingFlags.Instance | BindingFlags.NonPublic)
                        .SetValue(message, FormatterServices.GetUninitializedObject(gamerType));
                    // Voice's original reader compares the existing array length.
                    var voice = type.GetField("AudioBuffer", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    if (voice != null) voice.SetValue(message, Array.Empty<byte>());
                    byte[] native = File.ReadAllBytes(file);
                    using (var input = new MemoryStream(native, false))
                    using (var reader = new BinaryReader(input))
                    {
                        read.Invoke(message, new object[] { reader });
                        if (input.Position != input.Length)
                            throw new InvalidDataException("Original reader did not consume entire payload: " + input.Position + "/" + input.Length);
                    }
                    using (var output = new MemoryStream())
                    using (var writer = new BinaryWriter(output))
                    {
                        write.Invoke(message, new object[] { writer });
                        writer.Flush();
                        byte[] original = output.ToArray();
                        if (!native.SequenceEqual(original))
                        {
                            int difference = Enumerable.Range(0, Math.Min(native.Length, original.Length))
                                .FirstOrDefault(index => native[index] != original[index]);
                            throw new InvalidDataException("Original writer differs: native=" + native.Length +
                                " original=" + original.Length + " firstDifference=" + difference);
                        }
                    }
                    ++passed;
                    exercised.Add(messageName);
                    Console.WriteLine("PASS " + name + " bytes=" + native.Length);
                }
                catch (Exception error)
                {
                    while (error.InnerException != null) error = error.InnerException;
                    ++failed;
                    Console.WriteLine("FAIL " + name + " " + error.GetType().Name + ": " + error.Message.Replace('\r', ' ').Replace('\n', ' '));
                    Console.WriteLine(error.StackTrace);
                }
            }
            foreach (string missing in expected.Except(exercised))
            {
                Console.WriteLine("FAIL missing original message: " + missing);
                ++failed;
            }
            string codebookFile = Path.Combine(args[1], "voice.alaw");
            if (!File.Exists(codebookFile))
                throw new InvalidDataException("Native A-law codebook missing");
            byte[] codebook = File.ReadAllBytes(codebookFile);
            if (codebook.Length != 65536 + 512)
                throw new InvalidDataException("Native A-law codebook size differs");
            Type voiceType = common.GetType("DNA.Net.VoiceChat", true);
            var encode = voiceType.GetMethod("EncodeALawSample", BindingFlags.Static | BindingFlags.NonPublic);
            var decode = voiceType.GetMethod("DecodeALawSample", BindingFlags.Static | BindingFlags.NonPublic);
            for (int sample = -32768; sample <= 32767; ++sample)
                if (codebook[sample + 32768] != (byte)encode.Invoke(null, new object[] { sample }))
                    throw new InvalidDataException("Original A-law encoder differs at " + sample);
            for (int code = 0; code < 256; ++code)
                if (BitConverter.ToInt16(codebook, 65536 + code * 2) != (short)decode.Invoke(null, new object[] { (byte)code }))
                    throw new InvalidDataException("Original A-law decoder differs at " + code);
            Console.WriteLine("PASS original A-law: all 65536 PCM samples and all 256 codewords");
            Console.WriteLine("Original message coverage: " + exercised.Count + "/" + expected.Count);
            Console.WriteLine("Original serializer parity: " + passed + " passed, " + failed + " failed");
            return failed == 0 && passed != 0 ? 0 : 1;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 2;
        }
    }
}
