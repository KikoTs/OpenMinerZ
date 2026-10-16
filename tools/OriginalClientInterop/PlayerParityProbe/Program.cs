using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

// Differential probe for everything one player shows another across a mixed
// stock / OpenMinerZ session. It loads the user's OWNED, unmodified game
// assemblies by path into an isolated x86 process and executes the original
// Player / PlayerUpdateMessage / PlayerExistsMessage / session-roster code on
// OpenMinerZ's bytes, and the original senders on the same scripted states.
//
// It never runs a Game constructor, opens a window or a graphics device, enters
// Steam, or sends network traffic, and it copies nothing out of the install.
//
// Executed for real (original IL):
//   * PlayerUpdateMessage.RecieveData and PlayerUpdateMessage.Apply onto a real
//     Player object (FPSRig/Entity constructed by their own constructors);
//   * PlayerUpdateMessage.Send -> Message.DoSend -> SendData, i.e. how the local
//     Player FILLS the packet, captured from the original write buffer;
//   * FPSRig.UpdateVelocity (controller Movement -> world velocity);
//   * the FPSRig camera chain (Entity transforms, PerspectiveCamera.View) that
//     the nametag projection reads;
//   * PlayerExistsMessage.Send / RecieveData;
//   * the session roster readers (NetBuffer, ReadConnectedMessage, ReadGamer)
//     that give a stock client every remote gamertag it will ever draw;
//   * XNA SpriteFont.MeasureString over the shipped NameTagFont glyph table.
// Transcribed (cannot run without a GraphicsDevice / loaded world):
//   * the ten arithmetic lines of GameScreen.gameScreen_AfterDraw that turn the
//     real view matrix into a screen position (evaluated here with XNA math);
//   * Player.UpdateAnimation and Player.ResolveCollsion.
internal static class Program
{
    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static |
                                     BindingFlags.Public | BindingFlags.NonPublic;
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    private static string gameDirectory;
    private static int passed;
    private static int failed;

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

    private static void Check(bool ok, string what)
    {
        if (ok) ++passed; else ++failed;
        Console.WriteLine((ok ? "PASS " : "FAIL ") + what);
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

    private static PropertyInfo Property(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            PropertyInfo property = current.GetProperty(name, Any | BindingFlags.DeclaredOnly);
            if (property != null) return property;
        }
        throw new MissingMemberException(type.FullName, name);
    }

    private static byte[] Hex(string text)
    {
        var bytes = new byte[text.Length / 2];
        for (int index = 0; index < bytes.Length; ++index)
            bytes[index] = byte.Parse(text.Substring(index * 2, 2), NumberStyles.HexNumber, Inv);
        return bytes;
    }

    private static string Hex(byte[] bytes)
    {
        return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
    }

    private static float[] Floats(string text)
    {
        return text.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(part => float.Parse(part, Inv)).ToArray();
    }

    private static string F(float value) { return value.ToString("R", Inv); }

    private sealed class Original
    {
        public Assembly Game, Common;
        public Type Player, FpsRig, Entity, NetworkGamer, LocalNetworkGamer, Message, Angle;
        public Type PlayerUpdate, PlayerExists, ControllerMapping, AvatarDescription, PlayerId;
        public Type NetBuffer, LidgrenExtensions;

        public object NewPlayer()
        {
            // Entity and FPSRig run their OWN constructors on the instance, so
            // the transform, physics, pivots and FPS camera are the original's.
            // Player's constructor (avatar model, content, HUD) is not run.
            object player = FormatterServices.GetUninitializedObject(Player);
            FpsRig.GetConstructor(Type.EmptyTypes).Invoke(player, null);
            Field(Player, "Gamer").SetValue(player, FormatterServices.GetUninitializedObject(NetworkGamer));
            Field(Player, "_playerMode").SetValue(player, Enum.ToObject(Field(Player, "_playerMode").FieldType, 3));
            return player;
        }

        public object NewAngleDegrees(float degrees)
        {
            return Angle.GetMethod("FromDegrees", new[] { typeof(float) }).Invoke(null, new object[] { degrees });
        }

        public float Degrees(object angle)
        {
            return (float)Angle.GetProperty("Degrees").GetValue(angle, null);
        }

        public object Physics(object player)
        {
            return Property(FpsRig, "PlayerPhysics").GetValue(player, null);
        }

        public Vector3 WorldVelocity(object player)
        {
            object physics = Physics(player);
            return (Vector3)Property(physics.GetType(), "WorldVelocity").GetValue(physics, null);
        }

        public void SetWorldVelocity(object player, Vector3 value)
        {
            object physics = Physics(player);
            Property(physics.GetType(), "WorldVelocity").SetValue(physics, value, null);
        }

        public object SilentLocalGamer(string gamertag, byte[] playerId)
        {
            // HasLeftSession makes Message.DoSendInternal serialize the packet
            // into the original write buffer and then skip the transport.
            object gamer = FormatterServices.GetUninitializedObject(LocalNetworkGamer);
            Field(NetworkGamer, "_hasLeftSession").SetValue(gamer, true);
            if (gamertag != null)
                Property(LocalNetworkGamer, "Gamertag").SetValue(gamer, gamertag, null);
            if (playerId != null)
                Field(LocalNetworkGamer, "PlayerID").SetValue(gamer, Activator.CreateInstance(PlayerId, new object[] { playerId }));
            return gamer;
        }

        // The bytes the original just wrote: [message id][payload][xor checksum].
        public byte[] LastSentPayload()
        {
            object stream = Field(Message, "_writeBufferStream").GetValue(null);
            var memory = (MemoryStream)Property(stream.GetType(), "BaseStream").GetValue(stream, null);
            int length = (int)memory.Position;
            return memory.GetBuffer().Skip(1).Take(length - 2).ToArray();
        }
    }

    // Player's type initializer loads its shadow/proxy models and particle
    // effects through CastleMinerZGame.Instance.Content. None of that is used
    // by replication, so hand it an inert game object whose content manager
    // returns no asset: no Game constructor, graphics device or file is touched.
    private sealed class InertContent : Microsoft.Xna.Framework.Content.ContentManager
    {
        private sealed class NoServices : IServiceProvider
        {
            public object GetService(Type serviceType) { return null; }
        }

        public InertContent() : base(new NoServices()) { }
        public override T Load<T>(string assetName) { return default(T); }
    }

    private static void InstallInertGame(Assembly game)
    {
        Type gameType = game.GetType("DNA.CastleMinerZ.CastleMinerZGame", true);
        object context = FormatterServices.GetUninitializedObject(gameType);
        GC.SuppressFinalize(context);
        for (Type current = gameType; current != null; current = current.BaseType)
            foreach (FieldInfo field in current.GetFields(Any | BindingFlags.DeclaredOnly))
                if (!field.IsStatic && field.FieldType == typeof(Microsoft.Xna.Framework.Content.ContentManager))
                    field.SetValue(context, new InertContent());
        gameType.GetField("Instance", Any).SetValue(null, context);
    }

    // Message ids are positions in the name-sorted list of Message types found
    // in the assemblies the game registered at start-up. Register the same two
    // through the original's own registry so Message's type initializer builds
    // the original send instances (needed by every static Send method).
    private static void RegisterMessageAssemblies(Original original)
    {
        Type tools = original.Common.GetType("DNA.Reflection.ReflectionTools", true);
        tools.GetMethod("RegisterAssembly", Any).Invoke(null, new object[] { original.Game, original.Common });
    }

    private static Quaternion FacingRotation(float forwardX, float forwardZ)
    {
        // The rotation whose Vector3.Forward points along the given heading.
        return Quaternion.CreateFromYawPitchRoll((float)Math.Atan2(-forwardX, -forwardZ), 0f, 0f);
    }

    private static int PlayerUpdates(Original original, string fixtures, string output)
    {
        string path = Path.Combine(fixtures, "player_updates.txt");
        var results = new List<string>();
        MethodInfo read = original.PlayerUpdate.GetMethod("RecieveData", Any);
        MethodInfo apply = original.PlayerUpdate.GetMethod("Apply", Any);
        MethodInfo send = original.PlayerUpdate.GetMethod("Send", Any);
        MethodInfo updateVelocity = original.FpsRig.GetMethod("UpdateVelocity", Any);
        int count = 0;
        foreach (string line in File.ReadAllLines(path).Where(text => text.Length != 0 && text[0] != '#'))
        {
            string[] part = line.Split('|');
            string name = part[0];
            float[] position = Floats(part[1]), facing = Floats(part[2]), velocity = Floats(part[4]);
            float pitch = float.Parse(part[3], Inv);
            float[] movement = Floats(part[5]);
            bool[] flags = part[6].Select(digit => digit == '1').ToArray();
            int mode = int.Parse(part[7], Inv);
            byte[] ours = Hex(part[8]);
            ++count;

            // ---- OUR bytes into the ORIGINAL receive + apply path ----------
            object message = Activator.CreateInstance(original.PlayerUpdate, true);
            using (var input = new MemoryStream(ours, false))
            using (var reader = new BinaryReader(input))
            {
                read.Invoke(message, new object[] { reader });
                Check(input.Position == input.Length, name + " original reader consumed " + input.Position + "/" + input.Length);
            }
            object remote = original.NewPlayer();
            string applyStopped = "completed";
            try { apply.Invoke(message, new[] { remote }); }
            catch (TargetInvocationException error)
            {
                // Apply assigns every replicated field first and only then
                // calls Player.UpdateAnimation, which needs the loaded avatar.
                applyStopped = error.InnerException.GetType().Name + " in " +
                               new System.Diagnostics.StackTrace(error.InnerException).GetFrame(0).GetMethod().Name;
            }
            var believedPosition = (Vector3)Property(original.Entity, "LocalPosition").GetValue(remote, null);
            var believedRotation = (Quaternion)Property(original.Entity, "LocalRotation").GetValue(remote, null);
            Vector3 believedForward = Vector3.Transform(Vector3.Forward, believedRotation);
            Vector3 believedVelocity = original.WorldVelocity(remote);
            float believedPitch = original.Degrees(Field(original.PlayerUpdate, "TorsoPitch").GetValue(message));
            var believedMovement = (Vector2)Field(original.PlayerUpdate, "Movement").GetValue(message);
            bool dead = (bool)Field(original.Player, "Dead").GetValue(remote);
            Check(believedPosition == new Vector3(position[0], position[1], position[2]),
                name + " ours->original feet position " + believedPosition);
            float headingError = MathHelper.ToDegrees((float)Math.Acos(MathHelper.Clamp(
                believedForward.X * facing[0] + believedForward.Z * facing[1], -1f, 1f)));
            Check(Math.Abs(believedForward.Y) < 1e-5f && headingError <= 0.72f,
                name + " ours->original facing error " + F(headingError) + " deg (wire step 1.412)");
            Check(Math.Abs(believedPitch - pitch) <= 0.36f,
                name + " ours->original torso pitch " + F(believedPitch) + " deg, meant " + F(pitch));
            Check((believedVelocity - new Vector3(velocity[0], velocity[1], velocity[2])).Length() <=
                  0.001f * (1f + new Vector3(velocity[0], velocity[1], velocity[2]).Length()),
                name + " ours->original world velocity " + believedVelocity);
            // The wire truncates each axis to fifteen levels.
            Check(believedMovement.X <= movement[0] + 1e-4f && believedMovement.X >= movement[0] - 1f / 7f - 1e-4f &&
                  believedMovement.Y <= movement[1] + 1e-4f && believedMovement.Y >= movement[1] - 1f / 7f - 1e-4f,
                name + " ours->original movement right/forward " + believedMovement);
            string[] fields = { "UsingTool", "Dead", "Shouldering", "Reloading", "PlayGrenadeAnim", "ReadyToThrowGrenade" };
            for (int index = 0; index < fields.Length; ++index)
                Check((bool)Field(original.Player, fields[index]).GetValue(remote) == flags[index],
                    name + " ours->original Player." + fields[index] + "=" + flags[index]);
            Check(Convert.ToInt32(Field(original.Player, "_playerMode").GetValue(remote)) == mode,
                name + " ours->original Player._playerMode=" + Field(original.Player, "_playerMode").GetValue(remote));
            if (count == 1) Console.WriteLine("NOTE Apply stopped after assigning state: " + applyStopped);

            // ---- the ORIGINAL local player filling a packet ----------------
            object local = original.NewPlayer();
            Property(original.Entity, "LocalPosition").SetValue(local, new Vector3(position[0], position[1], position[2]), null);
            Property(original.Entity, "LocalRotation").SetValue(local, FacingRotation(facing[0], facing[1]), null);
            // The very float our player holds, so both senders start equal.
            Field(original.FpsRig, "TorsoPitch").SetValue(local,
                original.Angle.GetMethod("FromRadians", new[] { typeof(float) }).Invoke(null, new object[] { float.Parse(part[10], Inv) }));
            object mapping = FormatterServices.GetUninitializedObject(original.ControllerMapping);
            Field(original.ControllerMapping, "Movement").SetValue(mapping, new Vector2(movement[0], movement[1]));
            // FPSRig.UpdateVelocity: which world direction "right"/"forward" is.
            object gameTime = Activator.CreateInstance(updateVelocity.GetParameters()[1].ParameterType,
                TimeSpan.Zero, TimeSpan.FromSeconds(1.0 / 60.0));
            updateVelocity.Invoke(local, new[] { mapping, gameTime });
            Vector3 walked = original.WorldVelocity(local);
            float[] ourWalk = Floats(part[9]);
            Check((new Vector2(walked.X, walked.Z) - new Vector2(ourWalk[0], ourWalk[1])).Length() < 0.01f,
                name + " original UpdateVelocity walks (" + F(walked.X) + ", " + F(walked.Z) + "), our simulation (" +
                F(ourWalk[0]) + ", " + F(ourWalk[1]) + ")");
            original.SetWorldVelocity(local, new Vector3(velocity[0], velocity[1], velocity[2]));
            for (int index = 0; index < fields.Length; ++index)
                Field(original.Player, fields[index]).SetValue(local, flags[index]);
            Field(original.Player, "_playerMode").SetValue(local, Enum.ToObject(Field(original.Player, "_playerMode").FieldType, mode));
            send.Invoke(null, new[] { original.SilentLocalGamer(null, null), local, mapping });
            byte[] theirs = original.LastSentPayload();
            results.Add(name + "|" + Hex(theirs));
            bool identical = ours.SequenceEqual(theirs);
            // Yaw is the only field the two sides derive through different
            // arithmetic (quaternion vs scalar), so allow it one wire step.
            bool withinStep = ours.Length == theirs.Length && Enumerable.Range(0, ours.Length).All(index =>
                ours[index] == theirs[index] ||
                (index == 13 && (Math.Abs(ours[index] - theirs[index]) == 1 || Math.Abs(ours[index] - theirs[index]) == 255)));
            Check(withinStep, name + " original sender bytes " + (identical ? "identical" : "differ: ours=" + Hex(ours) + " original=" + Hex(theirs)));
        }
        File.WriteAllLines(Path.Combine(output, "original_player_updates.txt"), results);
        return count;
    }

    // Thousands of headings and pitches through the original sender, against
    // the byte our sender chose for the same angle. A wire step is 1.41 degrees
    // of yaw and 0.71 of pitch; the two senders may only ever disagree by one
    // step, at an angle that sits on the rounding boundary between two steps.
    private static void AngleSweep(Original original, string fixtures)
    {
        MethodInfo send = original.PlayerUpdate.GetMethod("Send", Any);
        MethodInfo fromRadians = original.Angle.GetMethod("FromRadians", new[] { typeof(float) });
        object mapping = FormatterServices.GetUninitializedObject(original.ControllerMapping);
        object gamer = original.SilentLocalGamer(null, null);
        object local = original.NewPlayer();
        var same = new Dictionary<string, int> { { "yaw", 0 }, { "pitch", 0 } };
        var step = new Dictionary<string, int> { { "yaw", 0 }, { "pitch", 0 } };
        var worse = new Dictionary<string, int> { { "yaw", 0 }, { "pitch", 0 } };
        foreach (string line in File.ReadAllLines(Path.Combine(fixtures, "angle_sweep.txt")).Where(text => text.Length != 0))
        {
            string[] part = line.Split('|');
            int ours = int.Parse(part[3], Inv);
            int theirs;
            if (part[0] == "yaw")
            {
                float[] facing = Floats(part[2]);
                Property(original.Entity, "LocalRotation").SetValue(local, FacingRotation(facing[0], facing[1]), null);
                Field(original.FpsRig, "TorsoPitch").SetValue(local, fromRadians.Invoke(null, new object[] { 0f }));
                send.Invoke(null, new[] { gamer, local, mapping });
                theirs = original.LastSentPayload()[13];
                // 255 and 0 are the same heading (255/255 of a turn).
                if (theirs == 255) theirs = 0;
                if (ours == 255) ours = 0;
            }
            else
            {
                Property(original.Entity, "LocalRotation").SetValue(local, Quaternion.Identity, null);
                Field(original.FpsRig, "TorsoPitch").SetValue(local, fromRadians.Invoke(null, new object[] { float.Parse(part[1], Inv) }));
                send.Invoke(null, new[] { gamer, local, mapping });
                theirs = original.LastSentPayload()[12];
            }
            int difference = Math.Abs(ours - theirs);
            if (part[0] == "yaw" && difference == 254) difference = 1;
            if (difference == 0) ++same[part[0]];
            else if (difference == 1) ++step[part[0]];
            else
            {
                if (++worse[part[0]] <= 5)
                    Console.WriteLine("FAIL sweep " + part[0] + " " + part[1] + " ours=" + ours + " original=" + theirs);
            }
        }
        foreach (string kind in new[] { "yaw", "pitch" })
            Check(worse[kind] == 0 && same[kind] != 0,
                "sweep " + kind + ": " + same[kind] + " identical bytes, " + step[kind] + " one step apart, " + worse[kind] + " further");
    }

    private static void PlayerExists(Original original, string fixtures, string output)
    {
        var results = new List<string>();
        MethodInfo read = original.PlayerExists.GetMethod("RecieveData", Any);
        MethodInfo send = original.PlayerExists.GetMethod("Send", Any);
        foreach (string line in File.ReadAllLines(Path.Combine(fixtures, "player_exists.txt")).Where(text => text.Length != 0))
        {
            string[] part = line.Split('|');
            string name = part[0];
            bool request = part[1] == "1";
            string tag = Encoding.UTF8.GetString(Hex(part[2]));
            byte[] playerId = Hex(part[3]), avatar = Hex(part[4]), ours = Hex(part[5]);
            object message = Activator.CreateInstance(original.PlayerExists, true);
            using (var input = new MemoryStream(ours, false))
            using (var reader = new BinaryReader(input))
            {
                read.Invoke(message, new object[] { reader });
                Check(input.Position == input.Length, name + " PlayerExists original reader consumed payload");
            }
            object gamer = Field(original.PlayerExists, "Gamer").GetValue(message);
            string believedTag = (string)Property(gamer.GetType(), "Gamertag").GetValue(gamer, null);
            Check(believedTag == tag && believedTag.Length != 0, name + " PlayerExists ours->original Gamertag \"" + believedTag + "\"");
            Check((bool)Field(original.PlayerExists, "RequestResponse").GetValue(message) == request, name + " PlayerExists RequestResponse");
            Check(((byte[])Field(original.PlayerExists, "AvatarDescriptionData").GetValue(message)).SequenceEqual(avatar), name + " PlayerExists avatar description bytes");
            // AvatarDescription(byte[]) is what _processPlayerExistsMessage
            // builds the Player from; it must accept what we send.
            object description = Activator.CreateInstance(original.AvatarDescription, new object[] { avatar });
            Check(description != null, name + " original AvatarDescription accepts " + avatar.Length + " bytes");
            send.Invoke(null, new[] { original.SilentLocalGamer(tag, playerId), description, (object)request });
            byte[] theirs = original.LastSentPayload();
            results.Add(name + "|" + Hex(theirs));
            Check(ours.SequenceEqual(theirs), name + " PlayerExists original sender bytes identical");
        }
        File.WriteAllLines(Path.Combine(output, "original_player_exists.txt"), results);
    }

    private static List<string> Roster(Original original, string fixtures)
    {
        // What SteamNetworkSessionProvider.HandleClientSystemMessages does with
        // the host's channel-1 packets: byte kind, then ReadConnectedMessage or
        // (byte gid, ReadGamer). The resulting Gamer.Gamertag is the ONLY name a
        // stock client has for a remote player; PlayerExists does not rename it.
        var names = new List<string>();
        MethodInfo readConnected = original.LidgrenExtensions.GetMethod("ReadConnectedMessage", Any);
        MethodInfo readGamer = original.LidgrenExtensions.GetMethod("ReadGamer", Any);
        foreach (string line in File.ReadAllLines(Path.Combine(fixtures, "roster.txt")).Where(text => text.Length != 0))
        {
            string[] part = line.Split('|');
            byte[] payload = Hex(part[1]);
            object buffer = Activator.CreateInstance(original.NetBuffer, true);
            Property(original.NetBuffer, "Data").SetValue(buffer, payload, null);
            Property(original.NetBuffer, "LengthBytes").SetValue(buffer, payload.Length, null);
            Property(original.NetBuffer, "Position").SetValue(buffer, 0L, null);
            byte kind = (byte)original.NetBuffer.GetMethod("ReadByte", Type.EmptyTypes).Invoke(buffer, null);
            var expected = part[3].Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(entry => entry.Split(':'))
                .ToDictionary(entry => byte.Parse(entry[0], Inv), entry => Encoding.UTF8.GetString(Hex(entry[1])));
            var believed = new Dictionary<byte, string>();
            if (part[0] == "connected")
            {
                Check(kind == 1, "roster connected response kind ResponseToConnection");
                object connected = readConnected.Invoke(null, new[] { buffer });
                Type type = connected.GetType();
                Check((byte)Field(type, "PlayerGID").GetValue(connected) == byte.Parse(part[2], Inv), "roster local gid " + part[2]);
                var peers = (Array)Field(type, "Peers").GetValue(connected);
                var ids = (byte[])Field(type, "ids").GetValue(connected);
                for (int index = 0; index < peers.Length; ++index)
                    believed[ids[index]] = (string)Property(peers.GetValue(index).GetType(), "Gamertag").GetValue(peers.GetValue(index), null);
            }
            else
            {
                Check(kind == 0, "roster new peer kind NewPeer");
                byte gid = (byte)original.NetBuffer.GetMethod("ReadByte", Type.EmptyTypes).Invoke(buffer, null);
                object gamer = readGamer.Invoke(null, new[] { buffer });
                believed[gid] = (string)Property(gamer.GetType(), "Gamertag").GetValue(gamer, null);
            }
            long position = (long)Property(original.NetBuffer, "Position").GetValue(buffer, null);
            Check(position == payload.Length * 8L, "roster " + part[0] + " original reader consumed payload");
            Check(believed.Count == expected.Count, "roster " + part[0] + " lists " + believed.Count + " gamers");
            foreach (var entry in expected)
            {
                string tag;
                bool found = believed.TryGetValue(entry.Key, out tag);
                Check(found && tag == entry.Value && tag.Length != 0,
                    "roster " + part[0] + " gid " + entry.Key + " stock client learns Gamertag \"" + (found ? tag : "<missing>") + "\"");
                if (found) names.Add(tag);
            }
        }
        return names;
    }

    private static int Read7Bit(BinaryReader reader)
    {
        int value = 0, shift = 0;
        byte part;
        do { part = reader.ReadByte(); value |= (part & 0x7F) << shift; shift += 7; } while ((part & 0x80) != 0);
        return value;
    }

    // Reads the shipped sprite font's metrics and glyph table straight from the
    // owned install (XNA's own decompressor, no graphics device) and builds a
    // real XNA SpriteFont without a texture so MeasureString can run.
    private static string glyphReport;
    private static int capitalHeight;
    private static int capitalTop;

    private static SpriteFont LoadFont(string file, out int lineSpacing, out int tallest, out List<char> characters, out char? fallback)
    {
        Type contentReader = typeof(Microsoft.Xna.Framework.Content.ContentReader);
        MethodInfo prepare = contentReader.GetMethod("PrepareStream", BindingFlags.Static | BindingFlags.NonPublic);
        object[] arguments = { File.OpenRead(file), Path.GetFileName(file), 0 };
        using (var stream = (Stream)prepare.Invoke(null, arguments))
        using (var reader = new BinaryReader(stream, Encoding.UTF8))
        {
            int readers = Read7Bit(reader);
            for (int index = 0; index < readers; ++index) { reader.ReadString(); reader.ReadInt32(); }
            Read7Bit(reader);                       // shared resources
            Read7Bit(reader);                       // SpriteFont
            Read7Bit(reader);                       // Texture2D
            reader.ReadInt32(); reader.ReadUInt32(); reader.ReadUInt32();
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
            List<Rectangle> glyphs = rectangles(), cropping = rectangles();
            Read7Bit(reader);
            characters = new List<char>();
            for (uint count = reader.ReadUInt32(); count != 0; --count) characters.Add(reader.ReadChar());
            lineSpacing = reader.ReadInt32();
            float spacing = reader.ReadSingle();
            Read7Bit(reader);
            var kerning = new List<Vector3>();
            for (uint count = reader.ReadUInt32(); count != 0; --count)
                kerning.Add(new Vector3(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle()));
            fallback = reader.ReadBoolean() ? reader.ReadChar() : (char?)null;
            tallest = glyphs.Max(glyph => glyph.Height);
            var report = new StringBuilder();
            foreach (char sample in "HKxog")
            {
                int index = characters.IndexOf(sample);
                report.Append(" '" + sample + "'=" + glyphs[index].Width + "x" + glyphs[index].Height +
                              "@top" + cropping[index].Y + "/advance" + F(kerning[index].X + kerning[index].Y + kerning[index].Z));
            }
            glyphReport = report.ToString();
            capitalHeight = glyphs[characters.IndexOf('H')].Height;
            capitalTop = cropping[characters.IndexOf('H')].Y;
            ConstructorInfo constructor = typeof(SpriteFont).GetConstructors(BindingFlags.Instance | BindingFlags.NonPublic).Single();
            return (SpriteFont)constructor.Invoke(new object[] { null, glyphs, cropping, characters, lineSpacing, spacing, kerning, fallback });
        }
    }

    private static void Fonts(List<string> rosterNames, string output)
    {
        var lines = new List<string>();
        foreach (string fontName in new[] { "NameTagFont", "MedFont" })
        {
            // Classic keeps its sprite fonts in Content/720, Modern in Content/Fonts.
            string file = new[] { "720", "Fonts" }
                .Select(folder => Path.Combine(gameDirectory, "Content", folder, fontName + ".xnb"))
                .First(File.Exists);
            int lineSpacing, tallest;
            List<char> characters;
            char? fallback;
            SpriteFont font = LoadFont(file, out lineSpacing, out tallest, out characters, out fallback);
            Vector2 capital = font.MeasureString("H");
            Console.WriteLine("FONT " + fontName + " lineSpacing=" + lineSpacing + " tallestGlyph=" + tallest +
                              " glyphs=" + characters.Count + " range=U+" + ((int)characters.Min()).ToString("X4") + "..U+" +
                              ((int)characters.Max()).ToString("X4") + " defaultCharacter=" +
                              (fallback.HasValue ? "U+" + ((int)fallback.Value).ToString("X4") : "none") +
                              " measure(H)=" + capital.X + "x" + capital.Y + " ink" + glyphReport);
            lines.Add("font|" + fontName + "|" + lineSpacing + "|" + tallest + "|" + characters.Count + "|" +
                      (fallback.HasValue ? ((int)fallback.Value).ToString(Inv) : "-1") + "|" +
                      ((int)capital.Y).ToString(Inv) + "|" + capitalHeight + "|" + capitalTop);
            foreach (string name in rosterNames.Distinct())
            {
                string outcome;
                try
                {
                    Vector2 size = font.MeasureString(name);
                    outcome = "ok|" + F(size.X) + "|" + F(size.Y);
                }
                catch (ArgumentException)
                {
                    // GameScreen.gameScreen_AfterDraw has no handler: this is
                    // an unhandled exception in the stock client's draw.
                    outcome = "throws|0|0";
                }
                Console.WriteLine("MEASURE " + fontName + " \"" + name + "\" " + outcome);
                lines.Add("measure|" + fontName + "|" + Hex(Encoding.UTF8.GetBytes(name)) + "|" + outcome);
            }
        }
        File.WriteAllLines(Path.Combine(output, "original_fonts.txt"), lines);
    }

    private static void NameTags(Original original, string fixtures)
    {
        FieldInfo pivotField = Field(original.FpsRig, "pitchPiviot");
        foreach (string line in File.ReadAllLines(Path.Combine(fixtures, "nametag.txt")).Where(text => text.Length != 0))
        {
            string[] part = line.Split('|');
            string name = part[0];
            float[] feet = Floats(part[1]), facing = Floats(part[2]), size = Floats(part[5]), target = Floats(part[6]), ours = Floats(part[8]);
            float pitch = float.Parse(part[3], Inv), fov = float.Parse(part[4], Inv);
            bool oursVisible = part[7] == "1";
            // The local player's real camera rig: feet -> pitch pivot at 1.4 ->
            // recoil pivot -> FPSCamera, every transform computed by Entity.
            object local = original.NewPlayer();
            Property(original.Entity, "LocalPosition").SetValue(local, new Vector3(feet[0], feet[1], feet[2]), null);
            Property(original.Entity, "LocalRotation").SetValue(local, FacingRotation(facing[0], facing[1]), null);
            // FPSRig.OnUpdate's single line (the override chain needs a world).
            object pivot = pivotField.GetValue(local);
            Property(original.Entity, "LocalRotation").SetValue(pivot,
                Quaternion.CreateFromAxisAngle(Vector3.UnitX, MathHelper.ToRadians(pitch)), null);
            object camera = Field(original.FpsRig, "FPSCamera").GetValue(local);
            Matrix view = (Matrix)Property(camera.GetType(), "View").GetValue(camera, null);
            // PerspectiveCamera.GetProjection with its own near/far planes.
            Matrix projection = Matrix.CreatePerspectiveFieldOfView(MathHelper.ToRadians(fov), size[0] / size[1],
                (float)Field(camera.GetType(), "NearPlane").GetValue(camera), (float)Field(camera.GetType(), "FarPlane").GetValue(camera));
            // GameScreen.gameScreen_AfterDraw, lines transcribed verbatim.
            Matrix matrix = view * projection;
            Vector3 vector = new Vector3(target[0], target[1], target[2]) + new Vector3(0f, 2f, 0f);
            Vector4 vector2 = Vector4.Transform(vector, matrix);
            bool visible = vector2.Z > 0f;
            Vector3 vector3 = new Vector3(vector2.X / vector2.W, vector2.Y / vector2.W, vector2.Z / vector2.W);
            vector3 *= new Vector3(0.5f, -0.5f, 1f);
            vector3 += new Vector3(0.5f, 0.5f, 0f);
            vector3 *= new Vector3(size[0], size[1], 1f);
            Check(visible == oursVisible, name + " nametag in front of camera: original " + visible + ", ours " + oursVisible);
            if (visible && oursVisible)
                Check(Math.Abs(vector3.X - ours[0]) <= 0.75f && Math.Abs(vector3.Y - ours[1]) <= 0.75f,
                    name + " nametag anchor original (" + F(vector3.X) + ", " + F(vector3.Y) + ") ours (" + F(ours[0]) + ", " + F(ours[1]) + ")");
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.PlayerParityProbe <owned game directory> <native fixture directory> <output directory>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            Directory.CreateDirectory(args[2]);
            var original = new Original();
            original.Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            original.Common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            Console.WriteLine("Assembly " + original.Game.GetName().Version);
            InstallInertGame(original.Game);
            original.Player = original.Game.GetType("DNA.CastleMinerZ.Player", true);
            original.PlayerUpdate = original.Game.GetType("DNA.CastleMinerZ.Net.PlayerUpdateMessage", true);
            original.PlayerExists = original.Game.GetType("DNA.CastleMinerZ.Net.PlayerExistsMessage", true);
            original.ControllerMapping = original.Game.GetType("DNA.CastleMinerZ.CastleMinerZControllerMapping", true);
            original.FpsRig = original.Common.GetType("DNA.Drawing.FPSRig", true);
            original.Entity = original.Common.GetType("DNA.Drawing.Entity", true);
            original.Angle = original.Common.GetType("DNA.Angle", true);
            original.Message = original.Common.GetType("DNA.Net.Message", true);
            original.NetworkGamer = original.Common.GetType("DNA.Net.GamerServices.NetworkGamer", true);
            original.LocalNetworkGamer = original.Common.GetType("DNA.Net.GamerServices.LocalNetworkGamer", true);
            original.AvatarDescription = original.Common.GetType("DNA.Net.GamerServices.AvatarDescription", true);
            original.PlayerId = original.Common.GetType("DNA.PlayerID", false) ?? Field(original.Common.GetType("DNA.Net.GamerServices.Gamer", true), "PlayerID").FieldType;
            original.NetBuffer = original.Common.GetType("DNA.Net.Lidgren.NetBuffer", true);
            original.LidgrenExtensions = original.Common.GetType("DNA.Net.GamerServices.LidgrenExtensions", true);
            RegisterMessageAssemblies(original);

            int scenarios = PlayerUpdates(original, args[1], args[2]);
            AngleSweep(original, args[1]);
            PlayerExists(original, args[1], args[2]);
            List<string> rosterNames = Roster(original, args[1]);
            Fonts(rosterNames, args[2]);
            NameTags(original, args[1]);
            Console.WriteLine("Original player parity: " + scenarios + " player states, " + passed + " passed, " + failed + " failed");
            return failed == 0 && passed != 0 ? 0 : 1;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 2;
        }
    }
}
