using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Xna.Framework.Content;

// Runs the Xbox Avatar add-on's OWN code - a build of it (the mod-loader
// XboxAvatar.dll or the standalone XboxAvatarMod.dll), read into memory, never
// copied - inside the ORIGINAL engine's message layer, and has it talk to
// OpenMinerZ's native avatar sync.
//
// Part one is the PacketContractProbe pattern: OpenMinerZ's bytes go through the
// add-on's readers and writers and the engine's framing, and the add-on's bytes
// are written out for the native reader (`--check-probe`).
//
// Part two is a conversation. The add-on's AvatarNetworkBridge - its real state
// machine, driven through the same entry points the game calls: OnGamerJoined,
// OnMessage, Update - sits in an original NetworkSession as one gamer. The other
// gamer is OpenMinerZ: the native test executable in `--peer` mode, in its own
// process. Every packet either side produces is handed to the other unchanged
// until each holds the other's avatar, then each replaces its avatar mid-session.
//
// Nothing here starts the game, a graphics device, Steam or a socket. The
// session has one local gamer and no remote ones, so the engine's own
// LocalNetworkGamer.SendData can only append to in-memory queues: a broadcast
// to the sender's echo queue, a direct send to the recipient's (the OpenMinerZ
// gamer is given the local gamer TYPE for exactly that reason, and nothing else
// about it is local). Everything the add-on writes - its cache, its logs - goes
// under this executable's own directory, which the test script places in the
// build tree.
internal static class Program
{
    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    private const byte AddonGid = 1;
    private const byte OursGid = 2;
    private const byte StrangerGid = 4;

    private static string gameDirectory;
    private static string[] modDirectories;
    private static string fixtures;
    private static string output;
    private static Assembly game;
    private static Assembly common;
    private static Assembly addon;
    private static readonly Dictionary<string, Assembly> memory = new Dictionary<string, Assembly>(StringComparer.OrdinalIgnoreCase);
    private static int passed;

    // ------------------------------------------------------------ loading

    private static Assembly LoadIntoMemory(string key, byte[] image)
    {
        Assembly loaded = Assembly.Load(image);
        memory[key] = loaded;
        return loaded;
    }

    private static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        if (name.EndsWith(".resources", StringComparison.OrdinalIgnoreCase)) return null;
        // A mod's embedded dependency (each ships its own Harmony) belongs to
        // the mod that asks for it.
        foreach (Assembly owner in new[] { args.RequestingAssembly }.Concat(memory.Values).Where(a => a != null).Distinct().ToArray())
        {
            string resource = owner.GetManifestResourceNames()
                .FirstOrDefault(n => n.EndsWith(".Embedded." + name + ".dll", StringComparison.OrdinalIgnoreCase));
            if (resource == null) continue;
            string key = owner.GetName().Name + "/" + name;
            if (memory.TryGetValue(key, out Assembly embedded)) return embedded;
            using (Stream stream = owner.GetManifestResourceStream(resource))
            using (var copy = new MemoryStream())
            {
                stream.CopyTo(copy);
                return LoadIntoMemory(key, copy.ToArray());
            }
        }
        if (memory.TryGetValue(name, out Assembly known)) return known;
        // The owned game build always wins: the add-on's install folder holds a
        // game too, and two copies of the engine in one process would be two
        // different sets of types.
        foreach (string extension in new[] { ".dll", ".exe" })
        {
            string path = Path.Combine(gameDirectory, name + extension);
            if (File.Exists(path)) return Assembly.LoadFrom(path);
        }
        foreach (string directory in modDirectories)
        {
            string path = Path.Combine(directory, name + ".dll");
            if (File.Exists(path)) return LoadIntoMemory(name, File.ReadAllBytes(path));
        }
        return null;
    }

    // --------------------------------------------------------- reflection

    private sealed class NoServices : IServiceProvider
    {
        public object GetService(Type serviceType) { return null; }
    }

    // Player's static constructor loads four effects and two models through
    // the game's content manager. They are never drawn here; inert stand-ins
    // let the class exist without a graphics device or a content folder.
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

    private static Type T(string name)
    {
        Type type = game.GetType(name, false) ?? common.GetType(name, false);
        if (type == null) throw new InvalidDataException("Original type absent: " + name);
        return type;
    }

    private static Type A(string name)
    {
        return addon.GetType("XboxAvatar." + name, true);
    }

    private static FieldInfo Field(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, Any | BindingFlags.DeclaredOnly);
            if (field != null) return field;
        }
        throw new MissingFieldException(type.FullName, name);
    }

    private static object Get(object target, string name)
    {
        Type type = target as Type ?? target.GetType();
        return Field(type, name).GetValue(target is Type ? null : target);
    }

    private static void Set(object target, string name, object value)
    {
        Type type = target as Type ?? target.GetType();
        Field(type, name).SetValue(target is Type ? null : target, value);
    }

    private static object Call(object target, string name, params object[] args)
    {
        Type type = target as Type ?? target.GetType();
        for (Type current = type; current != null; current = current.BaseType)
            foreach (MethodInfo method in current.GetMethods(Any | BindingFlags.DeclaredOnly))
            {
                if (method.Name != name) continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length != args.Length) continue;
                bool fits = true;
                for (int index = 0; index < args.Length && fits; ++index)
                {
                    Type parameter = parameters[index].ParameterType;
                    if (parameter.IsByRef) parameter = parameter.GetElementType();
                    fits = args[index] == null ? !parameter.IsValueType || parameters[index].IsOut
                                               : parameter.IsInstanceOfType(args[index]);
                }
                if (!fits) continue;
                try
                {
                    return method.Invoke(method.IsStatic ? null : target, args);
                }
                catch (TargetInvocationException error)
                {
                    ExceptionDispatchInfo.Capture(error.InnerException).Throw();
                }
            }
        throw new MissingMethodException(type.FullName, name);
    }

    private static void Check(bool value, string what)
    {
        if (!value) throw new InvalidDataException("FAIL " + what);
    }

    private static void Pass(string what)
    {
        ++passed;
        Console.WriteLine("PASS " + what);
    }

    private static string Hex(byte[] bytes)
    {
        var text = new StringBuilder(bytes.Length * 2);
        foreach (byte value in bytes) text.Append(value.ToString("x2", CultureInfo.InvariantCulture));
        return text.ToString();
    }

    private static byte[] Unhex(string text)
    {
        var bytes = new byte[text.Length / 2];
        for (int index = 0; index < bytes.Length; ++index)
            bytes[index] = byte.Parse(text.Substring(index * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        return bytes;
    }

    private static byte[] Sha(byte[] bytes)
    {
        using (var sha = SHA256.Create()) return sha.ComputeHash(bytes);
    }

    // The engine's frame: message id, payload, XOR of everything before it.
    private static byte[] Frame(byte id, byte[] payload)
    {
        var frame = new byte[payload.Length + 2];
        frame[0] = id;
        Buffer.BlockCopy(payload, 0, frame, 1, payload.Length);
        byte checksum = 0;
        for (int index = 0; index < frame.Length - 1; ++index) checksum ^= frame[index];
        frame[frame.Length - 1] = checksum;
        return frame;
    }

    // ------------------------------------------------- an original session

    private static object session;
    private static object provider;
    private static object gameInstance;
    private static object localGamer;   // the add-on's client
    private static object oursGamer;    // OpenMinerZ
    private static Type messageType;
    private static Type bridge;
    private static Type playerType;

    private static object MakeGamer(Type type, byte id, bool local, bool host, string tag)
    {
        object gamer = Inert(type);
        Set(gamer, "_globalId", id);
        Set(gamer, "_isLocal", local);
        Set(gamer, "_isHost", host);
        Set(gamer, "_gamerTag", tag);
        Set(gamer, "PlayerID", Activator.CreateInstance(T("DNA.PlayerID"), new object[] { new byte[16] }));
        Set(gamer, "_networkSession", Activator.CreateInstance(Field(type, "_networkSession").FieldType, session));
        if (type.Name == "LocalNetworkGamer")
            Set(gamer, "_pendingData", Activator.CreateInstance(Field(type, "_pendingData").FieldType));
        // A gamer every client already has a Player for: its presence arrived
        // long ago, which is the only situation the add-on's marker is sent in.
        Set(gamer, "_tag", Inert(playerType));
        ((IList)Get(provider, "_allGamers")).Add(gamer);
        return gamer;
    }

    private static object Collection(Type element, object list)
    {
        Type collection = T("DNA.Net.GamerServices.GamerCollection`1").MakeGenericType(element);
        return Activator.CreateInstance(collection, BindingFlags.Instance | BindingFlags.NonPublic, null, new[] { list }, null);
    }

    private static void SetupSession()
    {
        Type providerBase = T("DNA.Net.GamerServices.NetworkSessionProvider");
        Type providerType = common.GetType("DNA.Net.GamerServices.LidgrenProvider.LidgrenNetworkSessionProvider", false) ??
                            common.GetTypes().First(type => !type.IsAbstract && providerBase.IsAssignableFrom(type));
        Type localType = T("DNA.Net.GamerServices.LocalNetworkGamer");
        Type gamerType = T("DNA.Net.GamerServices.NetworkGamer");
        playerType = T("DNA.CastleMinerZ.Player");
        // A game object nobody constructed: a holder for the three things the
        // add-on asks it for (the session, the local gamer, the local player).
        Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
        gameInstance = Inert(gameType);
        Set(gameType, "Instance", gameInstance);
        bool placed = false;
        for (Type current = gameType; current != null && !placed; current = current.BaseType)
            foreach (FieldInfo field in current.GetFields(Any | BindingFlags.DeclaredOnly))
                if (!field.IsStatic && field.FieldType == typeof(ContentManager))
                {
                    field.SetValue(gameInstance, new InertContent());
                    placed = true;
                }
        Check(placed, "the game's content manager field was not found");
        provider = Inert(providerType);
        session = Inert(T("DNA.Net.GamerServices.NetworkSession"));
        Set(session, "_sessionProvider", provider);
        Set(provider, "_networkSession", session);
        Set(provider, "_isHost", true);
        object locals = Activator.CreateInstance(typeof(List<>).MakeGenericType(localType));
        object all = Activator.CreateInstance(typeof(List<>).MakeGenericType(gamerType));
        // No gamer is ever placed in the REMOTE list: nothing can reach a transport.
        object remotes = Activator.CreateInstance(typeof(List<>).MakeGenericType(gamerType));
        Set(provider, "_localGamers", locals);
        Set(provider, "_localGamerCollection", Collection(localType, locals));
        Set(provider, "_remoteGamers", remotes);
        Set(provider, "_remoteGamerCollection", Collection(gamerType, remotes));
        Set(provider, "_allGamers", all);
        Set(provider, "_allGamerCollection", Collection(gamerType, all));
        localGamer = MakeGamer(localType, AddonGid, true, true, "AddonClient");
        ((IList)locals).Add(localGamer);
        oursGamer = MakeGamer(localType, OursGid, false, false, "OpenMinerZ");

        Set(gameInstance, "_networkSession", session);
        Set(gameInstance, "MyNetworkGamer", localGamer);
        // The local player and the description it announced: what the add-on
        // decorates with its marker (CastleMinerZGame.SetupNewGamer: ten zeros).
        object player = Get(localGamer, "_tag");
        object avatar = Inert(T("DNA.Avatars.Avatar"));
        Set(avatar, "_avatarDescription",
            Activator.CreateInstance(T("DNA.Net.GamerServices.AvatarDescription"), new object[] { new byte[10] }));
        Set(player, "Avatar", avatar);
        Set(player, "Gamer", localGamer);
        Set(gameInstance, "_localPlayer", player);
    }

    private static List<byte[]> Drain(object gamer)
    {
        var pending = (IList)Get(gamer, "_pendingData");
        var frames = new List<byte[]>();
        foreach (object packet in pending) frames.Add((byte[])Get(packet, "Data"));
        pending.Clear();
        return frames;
    }

    // The original receive path: the transport appends a packet to the local
    // gamer's queue and Message.GetMessage reads it.
    private static object Receive(byte[] frame, object sender)
    {
        Call(localGamer, "AppendNewDataPacket", frame, sender);
        return Call(messageType, "GetMessage", localGamer);
    }

    private static byte IdOf(string messageName)
    {
        var types = (Type[])Get(messageType, "_messageTypes");
        for (int index = 0; index < types.Length; ++index)
            if (types[index].Name == messageName) return (byte)index;
        throw new InvalidDataException("Message absent from the original table: " + messageName);
    }

    private static int stockPresenceCalls;

    // What the game does with a message the add-on's Harmony prefix let through.
    private static void StockPresence(object message)
    {
        MethodInfo method = T("DNA.CastleMinerZ.CastleMinerZGame").GetMethod("_processPlayerExistsMessage", Any);
        object[] arguments = method.GetParameters()
            .Select((parameter, index) => index == 0 ? message : (object)(parameter.Name == "isHost")).ToArray();
        // The gamer already has a Player, so this must return at its first
        // statement. If it did not, it would build a Player on a game object
        // that has no content, no screens and no world, and throw.
        try { method.Invoke(gameInstance, arguments); }
        catch (TargetInvocationException error) { ExceptionDispatchInfo.Capture(error.InnerException).Throw(); }
        ++stockPresenceCalls;
    }

    // --------------------------------------------------- part one: bytes

    private static byte[] Fixture(string name)
    {
        return File.ReadAllBytes(Path.Combine(fixtures, name));
    }

    private static byte[] WriteWith(object message)
    {
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            Call(message, "SendData", writer);
            writer.Flush();
            return stream.ToArray();
        }
    }

    private static string Thrown(Action action)
    {
        try { action(); }
        catch (Exception error)
        {
            string text = error.GetType().Name;
            for (Exception inner = error.InnerException; inner != null; inner = inner.InnerException)
                text += " <- " + inner.GetType().Name + ": " + inner.Message;
            return text;
        }
        return null;
    }

    private static byte StaticContracts(int protocol)
    {
        messageType = T("DNA.Net.Message");
        // What Program.Main does through CommonAssembly.Initalize.
        Call(T("DNA.Reflection.ReflectionTools"), "RegisterAssembly", game, common);
        int stock = ((Type[])Get(messageType, "_messageTypes")).Length;
        Check(stock == (protocol == 4 ? 55 : 53), "the stock message table has " + stock + " entries");
        SetupSession();

        // A client WITHOUT the add-on, handed the add-on's message: the engine
        // indexes its table with the id and throws. This is what the capability
        // gate exists to prevent.
        byte[] hello = Fixture("sync.1.0.payload.bin");
        string thrown = Thrown(() => Receive(Frame((byte)stock, hello), oursGamer));
        Check(thrown != null && thrown.StartsWith("InvalidMessageException", StringComparison.Ordinal),
              "a stock client did not throw on the avatar message: " + thrown);
        Pass("a client without the add-on throws on message id " + stock + " (" + thrown + ")");

        // The add-on registers its message the way it does in the game.
        bridge = A("AvatarNetworkBridge");
        Type registration = addon.GetType("XboxAvatar.AvatarMessageRegistration", false);
        string how;
        if (registration != null)
        {
            // The mod-loader build: loaded after the engine built its table, so
            // it registers its assembly and rebuilds the table itself.
            Call(registration, "Register");
            how = "AvatarMessageRegistration.Register";
        }
        else
        {
            // The standalone build: its installer patches a call to this into
            // Program.Main, ahead of the engine building its message table.
            // Here the table already exists (the check above made the engine
            // build it), so the engine's own builder runs once more.
            Call(bridge, "Register");
            Call(messageType, "PopulateMessageTypes");
            how = "AvatarNetworkBridge.Register + the engine's Message.PopulateMessageTypes";
        }
        int registered = (int)Call(A("ZZAvatarSyncMessage"), "LocalMessageId");
        Check(registered == stock, "the add-on registered its message at " + registered + ", expected " + stock);
        var id = (byte)registered;
        File.WriteAllBytes(Path.Combine(output, "addon.message-id.bin"), new[] { id });
        Pass(how + ": XboxAvatar.ZZAvatarSyncMessage is message id " + id);

        // Capability markers, OpenMinerZ's into the add-on's reader.
        byte[] description = Fixture("description.bin");
        var read = new object[] { Fixture("marker.form1.bin"), null, 0 };
        Check((bool)Call(bridge, "TryReadCapabilityMarker", read) && ((byte[])read[1]).SequenceEqual(description) && (int)read[2] == -1,
              "the add-on did not read OpenMinerZ's form-1 marker");
        foreach (int reported in new[] { 53, 55, 57 })
        {
            read = new object[] { Fixture("marker.form2." + reported + ".bin"), null, 0 };
            Check((bool)Call(bridge, "TryReadCapabilityMarker", read) && ((byte[])read[1]).SequenceEqual(description) && (int)read[2] == reported,
                  "the add-on did not read a form-2 marker for id " + reported);
        }
        read = new object[] { description, null, 0 };
        Check(!(bool)Call(bridge, "TryReadCapabilityMarker", read), "the add-on read a marker off a stock description");
        byte[] theirs = (byte[])Call(bridge, "AppendCapabilityMarker", description);
        File.WriteAllBytes(Path.Combine(output, "addon.marker.bin"), theirs);
        Pass("TryReadCapabilityMarker reads OpenMinerZ's markers (form 1, form 2 x3); AppendCapabilityMarker wrote " + Hex(theirs.Skip(description.Length).ToArray()));

        // The marked presence through the STOCK reader and writer.
        byte[] presence = Fixture("PlayerExistsMessage.marker.bin");
        object exists = Receive(Frame(IdOf("PlayerExistsMessage"), presence), oursGamer);
        Check(exists.GetType().Name == "PlayerExistsMessage" &&
                  ((byte[])Get(exists, "AvatarDescriptionData")).SequenceEqual(Fixture("marker.form1.bin")) &&
                  !(bool)Get(exists, "RequestResponse") && WriteWith(exists).SequenceEqual(presence),
              "the stock reader did not round-trip OpenMinerZ's marked presence");
        object stored = Activator.CreateInstance(T("DNA.Net.GamerServices.AvatarDescription"), new object[] { Get(exists, "AvatarDescriptionData") });
        Check(((byte[])Call(stored, "get_Description")).Length == description.Length + 8, "AvatarDescription did not keep the bytes");
        StockPresence(exists);
        Pass("OpenMinerZ's marked PlayerExistsMessage: read, rewritten identically and ignored as a duplicate by the stock client's own code");

        // Every avatar payload through the add-on's reader and writer, and
        // through the engine's frame reader and writer under the add-on's id.
        int payloads = 0;
        foreach (string file in Directory.EnumerateFiles(fixtures, "sync.*.payload.bin").OrderBy(path => path))
        {
            string stem = Path.GetFileName(file).Replace(".payload.bin", "");
            byte[] native = File.ReadAllBytes(file);
            object message = Receive(Frame(id, native), oursGamer);
            Check(message.GetType().Name == "ZZAvatarSyncMessage" && ReferenceEquals(Get(message, "_sender"), oursGamer),
                  "the engine did not hand " + stem + " to the add-on's message");
            byte[] rewritten = WriteWith(message);
            Check(rewritten.SequenceEqual(native), "ZZAvatarSyncMessage.SendData differs for " + stem);
            File.WriteAllBytes(Path.Combine(output, "addon." + stem + ".payload.bin"), rewritten);
            Drain(oursGamer);
            Call(message, "DoSend", localGamer, oursGamer);
            List<byte[]> sent = Drain(oursGamer);
            Check(sent.Count == 1 && sent[0].SequenceEqual(Frame(id, native)), "the engine's frame differs for " + stem);
            File.WriteAllBytes(Path.Combine(output, "addon." + stem + ".frame.bin"), sent[0]);
            ++payloads;
        }
        Check(payloads == 20, "expected 20 payload fixtures, found " + payloads);
        Pass(payloads + " avatar payloads: ZZAvatarSyncMessage.RecieveData/SendData and the engine's frames are byte-identical to OpenMinerZ's");

        // The add-on's own bound on a chunk.
        byte[] oversize = Fixture("sync.4.3000.payload.bin").Concat(new byte[] { 0 }).ToArray();
        oversize[46] = 0xB9; // 3001, little-endian, after the 46 bytes before the length
        oversize[47] = 0x0B;
        thrown = Thrown(() => Receive(Frame(id, oversize), oursGamer));
        Check(thrown != null && thrown.Contains("Invalid avatar chunk length"), "the add-on accepted a 3001-byte chunk: " + thrown);
        Pass("a 3001-byte chunk is refused by the add-on's reader");

        // OpenMinerZ's synthetic avatars through the add-on's file reader.
        foreach (string name in new[] { "synthetic", "avatar.small", "avatar.medium", "avatar.large" })
        {
            object asset = Call(A("AvatarAsset"), "Load", Path.Combine(fixtures, name + ".ocavatar"));
            Check(((Array)Get(asset, "Batches")).Length == 8 && ((Array)Get(asset, "InverseBindPose")).Length == 71,
                  "AvatarAsset.Load read " + name + " differently");
        }
        Pass("AvatarAsset.Load reads the four synthetic avatars the native writer made (8 batches, 71 bones)");
        return id;
    }

    // ------------------------------------------- part two: a conversation

    private static Process native;
    // The process's own input writer may begin with a byte-order mark,
    // depending on the console's code page; this one never does.
    private static StreamWriter toNative;
    private static readonly Queue<byte[]> toAddon = new Queue<byte[]>();
    private static readonly Queue<byte[]> fromAddon = new Queue<byte[]>();
    private static readonly List<string> events = new List<string>();
    private static byte[] oursPresence;
    private static byte[] oursLastManifest;
    private static int framesToAddon;
    private static int framesFromAddon;
    private static int[] kindsToAddon = new int[5];
    private static int[] kindsFromAddon = new int[5];
    private static byte addonId;

    private static List<string> Ask(string command)
    {
        toNative.Write(command + "\n");
        toNative.Flush();
        var lines = new List<string>();
        for (;;)
        {
            string line = native.StandardOutput.ReadLine();
            if (line == null) throw new InvalidDataException("the native peer ended during: " + command.Substring(0, Math.Min(60, command.Length)));
            if (line == "ok") return lines;
            if (line.StartsWith("error", StringComparison.Ordinal)) throw new InvalidDataException("native peer: " + line);
            lines.Add(line);
        }
    }

    private static void FromNative(IEnumerable<string> lines)
    {
        foreach (string line in lines)
        {
            if (!line.StartsWith("send ", StringComparison.Ordinal))
            {
                events.Add(line);
                continue;
            }
            string[] parts = line.Split(' ');
            byte[] frame = Unhex(parts[2]);
            if (parts[1] == "*")
            {
                // A broadcast reaches everybody, stock clients included: it has
                // to be a message every client can read.
                Check(frame[0] == IdOf("PlayerExistsMessage"), "OpenMinerZ broadcast message id " + frame[0]);
                oursPresence = frame;
            }
            else
            {
                Check(parts[1] == AddonGid.ToString(CultureInfo.InvariantCulture), "OpenMinerZ addressed gamer " + parts[1]);
                Check(frame[0] == addonId, "OpenMinerZ used message id " + frame[0] + " with a peer that reads " + addonId);
                ++kindsToAddon[frame[2]];
                if (frame[2] == 2) oursLastManifest = frame;
            }
            toAddon.Enqueue(frame);
        }
    }

    private static void ToAddon(byte[] frame)
    {
        ++framesToAddon;
        object message = Receive(frame, oursGamer);
        // CastleMinerZGame.OnMessage's Harmony prefix: the add-on first, the
        // game itself for whatever the add-on did not consume.
        bool consumed = (bool)Call(bridge, "OnMessage", message);
        if (consumed) return;
        Check(message.GetType().Name == "PlayerExistsMessage", "the add-on let through a " + message.GetType().Name);
        StockPresence(message);
    }

    private static void CollectFromAddon()
    {
        foreach (byte[] frame in Drain(oursGamer))
        {
            Check(frame[0] == addonId, "the add-on sent message id " + frame[0] + " directly");
            ++kindsFromAddon[frame[2]];
            fromAddon.Enqueue(frame);
        }
        foreach (byte[] frame in Drain(localGamer))
        {
            Check(frame[0] == IdOf("PlayerExistsMessage"), "the add-on broadcast message id " + frame[0]);
            if (!File.Exists(Path.Combine(output, "addon.presence.frame.bin")))
                File.WriteAllBytes(Path.Combine(output, "addon.presence.frame.bin"), frame);
            fromAddon.Enqueue(frame);
        }
    }

    private static void Settle()
    {
        for (;;)
        {
            CollectFromAddon();
            if (fromAddon.Count > 0)
            {
                ++framesFromAddon;
                FromNative(Ask("packet " + AddonGid + " " + Hex(fromAddon.Dequeue())));
            }
            else if (toAddon.Count > 0)
                ToAddon(toAddon.Dequeue());
            else
                return;
        }
    }

    private static void Round(double seconds)
    {
        Call(bridge, "Update");
        Settle();
        FromNative(Ask("tick " + seconds.ToString("0.###", CultureInfo.InvariantCulture)));
        Settle();
    }

    private static void RunUntil(Func<bool> done, string what)
    {
        for (int round = 0; round < 3000; ++round)
        {
            if (done()) return;
            Round(0.1);
        }
        throw new InvalidDataException("FAIL never happened: " + what);
    }

    private static byte[] AtAddon()
    {
        var path = (string)Call(bridge, "GetAssetPath", oursGamer);
        return path != null && File.Exists(path) ? File.ReadAllBytes(path) : null;
    }

    private static bool AddonHolds(byte[] bytes)
    {
        byte[] held = AtAddon();
        return held != null && held.SequenceEqual(bytes);
    }

    // "avatar <gid> <sha256> <bytes> <hash ok> <loads>"
    private static bool NativeHolds(byte[] bytes, bool loads)
    {
        string expected = "avatar " + AddonGid + " " + Hex(Sha(bytes)) + " " + bytes.Length + " 1 " + (loads ? "1" : "0");
        string last = events.LastOrDefault(line => line.StartsWith("avatar ", StringComparison.Ordinal));
        return last == expected;
    }

    // Where this build of the add-on keeps its avatar, cache and logs, by its
    // own account: `!Mods\XboxAvatar` for the mod-loader build, `Xbox Avatar`
    // for the standalone one - beside the hosting executable either way.
    private static string AddonFolder
    {
        get { return (string)Call(A("Branding"), "AvatarFolder", AppDomain.CurrentDomain.BaseDirectory); }
    }

    private static string AddonAvatarPath
    {
        get { return Path.Combine(AddonFolder, "avatar.ocavatar"); }
    }

    // The add-on throttles manifests by wall clock (five seconds per peer).
    // OpenMinerZ's clock here is the script's, so the add-on's is moved instead.
    private static void AgeAddonManifestClock()
    {
        var last = (IDictionary)Get(bridge, "LastManifestUtc");
        if (last.Contains(OursGid)) last[OursGid] = DateTime.UtcNow.AddSeconds(-6.0);
    }

    // The part of AvatarNetworkBridge.ReloadLocalAvatar (`/avatar reload`) that
    // concerns peers: forget the snapshot, drop the standing offers, offer
    // again. The part before it rebuilds the local player's model and needs a
    // graphics device, so the steps are taken here, each with the add-on's own
    // code. The standalone build has no reload command; there this is simply an
    // unsolicited second offer, which is what OpenMinerZ has to cope with.
    private static void AddonReloads(byte[] replacement)
    {
        File.WriteAllBytes(AddonAvatarPath, replacement);
        Set(bridge, "_localSnapshot", null);
        var offers = (IDictionary)Get(bridge, "Offers");
        foreach (object key in offers.Keys.Cast<object>().ToArray())
            if (ReferenceEquals(Get(offers[key], "Target"), oursGamer)) offers.Remove(key);
        foreach (object ready in ((IDictionary)Get(bridge, "PeerReady")).Values.Cast<object>().ToArray())
            Call(bridge, "HandleHello", ready);
    }

    private static void Conversation(int protocol, string nativeTests)
    {
        byte[] small = Fixture("avatar.small.ocavatar");
        byte[] medium = Fixture("avatar.medium.ocavatar");
        byte[] large = Fixture("avatar.large.ocavatar");
        Directory.CreateDirectory(Path.GetDirectoryName(AddonAvatarPath));
        File.WriteAllBytes(AddonAvatarPath, medium);

        native = Process.Start(new ProcessStartInfo(nativeTests, "--peer")
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true
        });
        toNative = new StreamWriter(native.StandardInput.BaseStream, new UTF8Encoding(false));
        FromNative(Ask("protocol " + protocol));
        FromNative(Ask("local " + Path.Combine(fixtures, "avatar.small.ocavatar")));

        // The session starts: both gamers join, as CastleMinerZGame.OnGamerJoined
        // reports them to the add-on.
        Call(bridge, "OnGamerJoined", localGamer);
        Call(bridge, "OnGamerJoined", oursGamer);
        Check(!(bool)Call(bridge, "IsPeerCapable", oursGamer), "a gamer was capable before announcing");

        RunUntil(() => AddonHolds(small) && NativeHolds(medium, true), "the first exchange");
        Check(events.Contains("capable " + AddonGid + " " + addonId + " 1"),
              "OpenMinerZ did not read the add-on's marker as form 2 with id " + addonId);
        Check((bool)Call(bridge, "IsPeerCapable", oursGamer), "the add-on did not accept OpenMinerZ's marker");
        Check(stockPresenceCalls >= 2, "the stock presence handler never saw OpenMinerZ's announcement");
        Check(!events.Any(line => line.StartsWith("refused", StringComparison.Ordinal)),
              "OpenMinerZ refused something the add-on sent: " + string.Join("; ", events.Where(line => line.StartsWith("refused", StringComparison.Ordinal)).Distinct()));
        object asset = Call(A("AvatarAsset"), "Load", (string)Call(bridge, "GetAssetPath", oursGamer));
        Check(((Array)Get(asset, "Batches")).Length == 8, "the add-on could not load what OpenMinerZ sent");
        Pass("handshake and first exchange: the add-on holds OpenMinerZ's avatar (" + small.Length + " bytes, loads with AvatarAsset.Load); OpenMinerZ holds the add-on's (" + medium.Length + " bytes, hash verified, loads)");

        // The add-on's manifest throttle, and why OpenMinerZ spaces its offers.
        int before = kindsFromAddon[3];
        ToAddon(oursLastManifest);
        Settle();
        Check(kindsFromAddon[3] == before && AddonHolds(small), "a manifest inside the add-on's five-second window was acted on");
        Pass("a second manifest within five seconds is ignored by the add-on (OpenMinerZ waits 5.5 s before re-offering)");

        // LIVE UPDATE, ours: the avatar file is re-exported while playing.
        AgeAddonManifestClock();
        FromNative(Ask("local " + Path.Combine(fixtures, "avatar.large.ocavatar")));
        RunUntil(() => AddonHolds(large), "the add-on receiving OpenMinerZ's new avatar");
        Pass("live update, OpenMinerZ -> add-on: the replacement (" + large.Length + " bytes, " + ((large.Length + 2999) / 3000) + " chunks) arrived in place");

        // LIVE UPDATE, theirs: `/avatar reload` on the add-on's side.
        AddonReloads(small);
        RunUntil(() => NativeHolds(small, true), "OpenMinerZ receiving the add-on's new avatar");
        Pass("live update, add-on -> OpenMinerZ: the replacement arrived in place, hash verified, loads");

        // Every length around one chunk, both ways. These are not avatars, so
        // OpenMinerZ verifies the hash and then (rightly) declines to load them.
        foreach (int size in new[] { 1, 2999, 3000, 3001 })
        {
            byte[] blob = Fixture("blob." + size + ".bin");
            AgeAddonManifestClock();
            FromNative(Ask("local " + Path.Combine(fixtures, "blob." + size + ".bin")));
            RunUntil(() => AddonHolds(blob), size + " bytes to the add-on");
            byte[] reply = blob.Reverse().ToArray();
            AddonReloads(reply);
            RunUntil(() => NativeHolds(reply, false), size + " bytes from the add-on");
        }
        Pass("payloads of 1, 2999, 3000 and 3001 bytes crossed intact in both directions");

        // Why OpenMinerZ announces with form 1: a form-2 marker naming an id
        // other than the add-on's own is refused by the add-on.
        object stranger = MakeGamer(T("DNA.Net.GamerServices.NetworkGamer"), StrangerGid, false, false, "Stranger");
        int descriptionLength = BitConverter.ToInt32(oursPresence, 2);
        byte[] tail = oursPresence.Skip(6 + descriptionLength).Take(oursPresence.Length - 7 - descriptionLength).ToArray();
        Func<byte[], byte[]> presenceWith = marked =>
        {
            var payload = new List<byte> { oursPresence[1] };
            payload.AddRange(BitConverter.GetBytes(marked.Length));
            payload.AddRange(marked);
            payload.AddRange(tail);
            return Frame(oursPresence[0], payload.ToArray());
        };
        int wrong = addonId == 57 ? 55 : 57;
        object mismatched = Receive(presenceWith(Fixture("marker.form2." + wrong + ".bin")), stranger);
        Check(!(bool)Call(bridge, "OnMessage", mismatched) && !(bool)Call(bridge, "IsPeerCapable", stranger),
              "the add-on accepted a marker naming message id " + wrong);
        object matching = Receive(presenceWith(Fixture("marker.form2." + addonId + ".bin")), stranger);
        Check(!(bool)Call(bridge, "OnMessage", matching) && (bool)Call(bridge, "IsPeerCapable", stranger),
              "the add-on refused a marker naming its own message id");
        Pass("the add-on refuses a form-2 marker naming id " + wrong + " and accepts one naming " + addonId + "; form 1 (what OpenMinerZ sends) needs no id");

        FromNative(Ask("left " + AddonGid));
        toNative.Write("quit\n");
        toNative.Flush();
        if (!native.WaitForExit(10000)) throw new InvalidDataException("the native peer did not exit");
        Check(native.ExitCode == 0, "the native peer exited with " + native.ExitCode);

        // The add-on swallows its own exceptions into renderer.log. The only
        // line it may have written is the id mismatch provoked just above.
        string log = Path.Combine(AddonFolder, "renderer.log");
        string[] failures = File.Exists(log)
            ? File.ReadAllLines(log).Where(line => line.Length > 19 && line[4] == '-' && line[13] == ':').ToArray()
            : new string[0];
        Check(failures.Length == 1 && failures[0].Contains("registers the avatar message at id " + wrong),
              "the add-on logged failures: " + string.Join(" | ", failures));
        Pass("the add-on logged no failure of its own during the exchange");
        Console.WriteLine("Packets: " + framesToAddon + " to the add-on (hello " + kindsToAddon[1] + ", manifest " + kindsToAddon[2] +
                          ", request " + kindsToAddon[3] + ", chunk " + kindsToAddon[4] + "); " + framesFromAddon +
                          " from it (hello " + kindsFromAddon[1] + ", manifest " + kindsFromAddon[2] + ", request " +
                          kindsFromAddon[3] + ", chunk " + kindsFromAddon[4] + ")");
    }

    private static int Main(string[] args)
    {
        if (args.Length != 6)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.AvatarAddonProbe <owned game directory> <XboxAvatar.dll> <native fixture directory> <output directory> <openminerz_xbox_avatar_tests.exe> <3|4>");
            return 2;
        }
        string home = AppDomain.CurrentDomain.BaseDirectory;
        if (File.Exists(Path.Combine(home, "CastleMinerZ.exe")) || File.Exists(Path.Combine(home, "steam_api.dll")))
        {
            Console.Error.WriteLine("Refusing to run from a game folder: the add-on writes its cache and logs beside this executable.");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        string addonFile = Path.GetFullPath(args[1]);
        string addonDirectory = Path.GetDirectoryName(addonFile);
        // The mod loader lives beside the mods folder or inside it.
        modDirectories = new[] { addonDirectory, Path.GetDirectoryName(addonDirectory) }.Where(directory => directory != null).ToArray();
        fixtures = Path.GetFullPath(args[2]);
        output = Path.GetFullPath(args[3]);
        int protocol = int.Parse(args[5], CultureInfo.InvariantCulture);
        Directory.CreateDirectory(output);
        // A clean slate: the add-on would otherwise find last run's cache.
        foreach (string stale in new[] { Path.Combine(home, "!Mods"), Path.Combine(home, "!Logs"), Path.Combine(home, "Xbox Avatar") })
            if (Directory.Exists(stale)) Directory.Delete(stale, true);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            byte[] image = File.ReadAllBytes(addonFile);
            addon = LoadIntoMemory(Path.GetFileNameWithoutExtension(addonFile), image);
            Console.WriteLine("Game " + game.GetName().Version + "; add-on SHA256=" + Hex(Sha(image)) + " (" + image.Length + " bytes, read into memory)");
            try { addon.GetTypes(); }
            catch (ReflectionTypeLoadException error)
            {
                // The engine skips an assembly whose types do not all load, and
                // the add-on's message would then never be registered.
                foreach (Exception loader in error.LoaderExceptions.Take(5)) Console.Error.WriteLine(loader.Message);
                throw;
            }
            addonId = StaticContracts(protocol);
            Conversation(protocol, Path.GetFullPath(args[4]));
            Console.WriteLine("Add-on interop: " + passed + " checks passed");
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(error.Message.StartsWith("FAIL", StringComparison.Ordinal) ? error.Message : "FAIL " + error);
            Console.Error.WriteLine(error);
            // What the add-on and its loader said for themselves on the way.
            foreach (string log in new[] { Path.Combine(home, "!Logs", "ModLoader.log"), Path.Combine(home, "!Mods", "XboxAvatar", "renderer.log"),
                                           Path.Combine(home, "Xbox Avatar", "renderer.log") })
                if (File.Exists(log)) Console.WriteLine(Path.GetFileName(log) + ": " + File.ReadAllText(log));
            if (events.Count > 0) Console.WriteLine("Last native events: " + string.Join(" | ", events.Skip(Math.Max(0, events.Count - 8))));
            return 1;
        }
        finally
        {
            try { if (native != null && !native.HasExited) native.Kill(); } catch { }
        }
    }
}
