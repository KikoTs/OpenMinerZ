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
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// Differential dragon probe. It loads the user's owned, unmodified Castle Miner Z
// assemblies by path and executes the ORIGINAL dragon code offline:
//
//   * EnemyManager.HandleMessage and every dragon handler behind it,
//   * DragonEntity (the owner's flight brain, its states and its send path),
//   * DragonClientEntity (the replica every peer presents), and
//   * the original message readers and writers.
//
// Nothing here starts Steam, opens a session or runs the game loop. A hidden
// 16x16 graphics device exists only so the original ContentManager can read
// the dragon's real skeleton and clips; without one the probe exits 77 (skip).
//
// What is NOT original code here, and nothing else:
//   * Target acquisition. DragonBaseState.FindBestTarget needs a live
//     NetworkSession and BlockTerrain, so the owner scenario writes the fields
//     SearchForNewTarget would write (TravelTarget, ChancesToNotAttack, the cry)
//     once the dragon is inside MaxViewDistance and leaves entity.Target null.
//     Every state then steers at the remembered position with its own code.
//   * Audio. SoundManager has no XACT bank: the two cue lookups on the flight
//     path (the cry, and the last statement of FireballEntity's constructor)
//     throw, are counted, and the interrupted frame is finished with a zero
//     time step. WingFlap's timer is parked so it never fires.
//   * Fireball flight after launch and its detonation (BlockTerrain), the death
//     fall (BlockTerrain) and loot. Only the launch is compared: tick, index,
//     origin (the real ponytail bone of the real skeleton) and velocity.
//   * The transport. Messages are serialized by the original writers and parsed
//     by the original readers, then handed to EnemyManager.HandleMessage a fixed
//     number of ticks later; Steam, relays, loss and reordering are not modelled.
//
// Trace lines (shared with tests/test_dragon_parity.cpp):
//   H dt kind owner_gid ticks player.xyz
//   M tick MessageName reliable sender_gid payload-hex
//   B tick DragonTime brain.xyz state        the owner's hidden DragonEntity
//   P tick xyz visible interpolation dead    what the owner's screen shows
//   R tick xyz visible interpolation dead    what a watcher's screen shows
//   F tick index origin.xyz velocity.xyz     a fireball appearing on that screen
//   Z tick animation muzzle-local.xyz        the ponytail bone in replica space
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

    public static object Invoke(MethodInfo method, object target, object[] arguments)
    {
        try { return method.Invoke(target, arguments); }
        catch (TargetInvocationException error)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
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

internal struct FireballRecord
{
    public int Tick;
    public int Index;
    public Vector3 Origin;
    public Vector3 Velocity;
    public Vector3 LocalMuzzle;
    public bool ConstructedByOriginal;
}

// One original peer: an inert CastleMinerZGame context plus a real EnemyManager
// message sink, a real DragonClientEntity and, when this peer is the spawner or
// migration target, a real DragonEntity.
internal sealed class Peer
{
    public Assembly Game;
    public Assembly Common;
    public object GameInstance;
    public object Manager;
    public object LocalGamer;
    public object LocalPlayer;
    public byte LocalGid;
    public Type MessageBase;
    public readonly List<FireballRecord> Fireballs = new List<FireballRecord>();
    public bool FireballConstructionTranscribed;
    public bool TerrainNeeded;
    public int SoundCallsSwallowed;
    private MethodInfo handleMessage;
    private FieldInfo fireballList;
    private int knownFireballs;

    public Type T(string name)
    {
        return Game.GetType(name, false) ?? Common.GetType(name, true);
    }

    public static Peer Create(string gameDirectory, byte localGid, Vector3 playerPosition)
    {
        var peer = new Peer { LocalGid = localGid };
        peer.Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
        peer.Common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
        // Message's static constructor assigns message ids from the assemblies
        // the game registered at start-up; do the same before any message exists.
        R.CallStatic(peer.Common.GetType("DNA.Reflection.ReflectionTools", true), "RegisterAssembly", peer.Common, peer.Game);
        peer.MessageBase = peer.Common.GetType("DNA.Net.Message", true);

        var device = new DeviceService();
        string profileDirectory = "HiDefContent";
        var form = new System.Windows.Forms.Form();
        var parameters = new PresentationParameters
        {
            BackBufferWidth = 16, BackBufferHeight = 16, DeviceWindowHandle = form.Handle,
            IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
        };
        try { device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.HiDef, parameters); }
        catch (Exception)
        {
            profileDirectory = "ReachContent";
            device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.Reach, parameters);
        }
        var services = new ServiceTable();
        services.Services[typeof(IGraphicsDeviceService)] = device;

        Type gameType = peer.T("DNA.CastleMinerZ.CastleMinerZGame");
        peer.GameInstance = FormatterServices.GetUninitializedObject(gameType);
        GC.SuppressFinalize(peer.GameInstance);
        R.SetStatic(gameType, "Instance", peer.GameInstance);
        ((Game)peer.GameInstance).Content = new OwnedContent(services, Path.Combine(gameDirectory, "Content"), profileDirectory);
        foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
            if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(peer.GameInstance, device);
        R.Set(peer.GameInstance, "CurrentGameTime", new GameTime());
        // Survival: the Endurance branches of the kill handler touch player stats.
        R.Set(peer.GameInstance, "GameMode", Enum.Parse(R.Get(peer.GameInstance, "GameMode").GetType(), "Survival"));

        // A disconnected local gamer. HasLeftSession makes Message.DoSendInternal
        // serialize every outgoing message and then skip the transport, so the
        // original send path runs to completion without a session.
        Type localGamerType = peer.T("DNA.Net.GamerServices.LocalNetworkGamer");
        peer.LocalGamer = FormatterServices.GetUninitializedObject(localGamerType);
        R.Set(peer.LocalGamer, "_globalId", localGid);
        R.Set(peer.LocalGamer, "_hasLeftSession", true);
        Type playerType = peer.T("DNA.CastleMinerZ.Player");
        peer.LocalPlayer = FormatterServices.GetUninitializedObject(playerType);
        GC.SuppressFinalize(peer.LocalPlayer);
        InitializeTreeNode(peer.LocalPlayer);
        R.Set(peer.LocalPlayer, "Gamer", peer.LocalGamer);
        peer.MovePlayer(playerPosition);
        R.Set(peer.GameInstance, "_localPlayer", peer.LocalPlayer);

        R.CallStatic(peer.T("DNA.CastleMinerZ.AI.DragonType"), "Init");
        R.CallStatic(peer.T("DNA.CastleMinerZ.AI.DragonClientEntity"), "Init");
        try { R.CallStatic(peer.T("DNA.CastleMinerZ.AI.FireballEntity"), "Init"); }
        catch (Exception) { /* fireball construction then falls back to the transcribed formula */ }

        Type managerType = peer.T("DNA.CastleMinerZ.AI.EnemyManager");
        peer.Manager = FormatterServices.GetUninitializedObject(managerType);
        GC.SuppressFinalize(peer.Manager);
        InitializeTreeNode(peer.Manager);
        R.SetStatic(managerType, "Instance", peer.Manager);
        Type fireball = peer.T("DNA.CastleMinerZ.AI.FireballEntity");
        // The live-fireball list is private in the shipped builds and is found by type.
        peer.fireballList = managerType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .First(field => field.FieldType == typeof(List<>).MakeGenericType(fireball));
        peer.fireballList.SetValue(peer.Manager, Activator.CreateInstance(peer.fireballList.FieldType));
        peer.handleMessage = managerType.GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .First(method => method.Name == "HandleMessage" && method.GetParameters().Length == 1);
        return peer;
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

    public void MovePlayer(Vector3 position)
    {
        R.Set(LocalPlayer, "_localToWorld", Matrix.CreateTranslation(position));
        R.Set(LocalPlayer, "_ltwDirty", false);
    }

    public object Brain { get { return R.Get(Manager, "_dragon"); } }
    public object Client { get { return R.Get(Manager, "_dragonClient"); } }

    public object Decode(string name, byte[] payload, byte senderGid)
    {
        Type type = T("DNA.CastleMinerZ.Net." + name);
        object message = Activator.CreateInstance(type, true);
        object sender = LocalGamer;
        if (senderGid != LocalGid)
        {
            sender = FormatterServices.GetUninitializedObject(T("DNA.Net.GamerServices.NetworkGamer"));
            R.Set(sender, "_globalId", senderGid);
        }
        R.Set(message, "_sender", sender);
        using (var stream = new MemoryStream(payload, false))
        using (var reader = new BinaryReader(stream))
        {
            R.Call(message, "RecieveData", reader);
            if (stream.Position != stream.Length)
                throw new InvalidDataException(name + ": original reader consumed " + stream.Position + "/" + stream.Length);
        }
        return message;
    }

    public byte[] Encode(object message)
    {
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            R.Call(message, "SendData", writer);
            writer.Flush();
            return stream.ToArray();
        }
    }

    public object SendInstance(string name)
    {
        Type type = T("DNA.CastleMinerZ.Net." + name);
        MethodInfo generic = MessageBase.GetMethod("GetSendInstance", BindingFlags.Static | BindingFlags.NonPublic);
        return generic.MakeGenericMethod(type).Invoke(null, null);
    }

    // EnemyManager.HandleMessage, exactly as CastleMinerZGame.OnMessage calls it.
    public void Deliver(string name, byte[] payload, byte senderGid)
    {
        object client = Client;
        R.Invoke(handleMessage, Manager, new object[] { Decode(name, payload, senderGid) });
        object replaced = Client;
        if (replaced != null && !ReferenceEquals(client, replaced))
        {
            // The one audio call in the flight path that cannot run without an
            // XACT sound bank: keep WingFlap's timer from ever expiring.
            Type timer = T("DNA.Timers.OneShotTimer");
            R.Set(replaced, "FlapTimer", Activator.CreateInstance(timer, TimeSpan.FromDays(30)));
        }
    }

    private static bool Mentions(Exception error, string text)
    {
        return error.StackTrace != null && error.StackTrace.IndexOf(text, StringComparison.Ordinal) >= 0;
    }

    public void UpdateClient(int tick, double totalSeconds, TimeSpan elapsed)
    {
        object client = Client;
        if (client == null || TerrainNeeded) return;
        MethodInfo update = R.Method(client.GetType(), "Update", 2);
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            try
            {
                R.Invoke(update, client, new object[] { null, new GameTime(TimeSpan.FromSeconds(totalSeconds), elapsed) });
                return;
            }
            catch (NullReferenceException error)
            {
                // Only two things may interrupt the original update here: the
                // XACT cue lookup, and the tail of FireballEntity's constructor
                // (which itself ends in that cue lookup).
                // The death fall's collision phase needs BlockTerrain: the
                // ballistic first reaction before it is all this probe can run.
                if (Mentions(error, "MoveDragonWithCollision"))
                {
                    TerrainNeeded = true;
                    return;
                }
                bool fireball = Mentions(error, "FireballEntity");
                if (!fireball && !Mentions(error, "SoundManager")) throw;
                if (fireball) CaptureFireball(client, tick, Mentions(error, "ProcessWaypoint"));
                else ++SoundCallsSwallowed;
                // Finish the interrupted frame without advancing time again.
                elapsed = TimeSpan.Zero;
            }
        }
        throw new InvalidOperationException("Original replica update did not settle");
    }

    // ShootFireball's own expression: the world translation of the ponytail
    // bone ("Bip01_Ponytail1" in Classic, "Bip01 Ponytail1" in Modern).
    public static Vector3 MuzzleWorld(object client)
    {
        var parts = (Array)R.Get(client, "_dragonModel");
        object body = parts.GetValue(0);
        object skeleton = R.Get(body, "Skeleton");
        string boneName = ((IEnumerable)skeleton).Cast<object>().Select(bone => (string)R.Get(bone, "Name"))
            .First(name => name.Replace(' ', '_') == "Bip01_Ponytail1");
        object ponytail = skeleton.GetType().GetProperty("Item", new[] { typeof(string) }).GetValue(skeleton, new object[] { boneName });
        var bones = (Matrix[])R.Get(body, "WorldBoneTransforms");
        return bones[(int)R.Get(ponytail, "Index")].Translation;
    }

    // The same point in the replica's own frame (right, up, backward).
    public static Vector3 MuzzleLocal(object client)
    {
        return Vector3.Transform(MuzzleWorld(client), Matrix.Invert((Matrix)R.Get(client, "LocalToWorld")));
    }

    private void CaptureFireball(object client, int tick, bool fromWaypoint)
    {
        var record = new FireballRecord { Tick = tick };
        var pending = (IList)R.Get(client, "NextFireballIndex");
        var live = (IList)fireballList.GetValue(Manager);
        Vector3 muzzle = MuzzleWorld(client);
        record.LocalMuzzle = MuzzleLocal(client);
        if (live.Count > knownFireballs)
        {
            object entity = live[live.Count - 1];
            record.Index = (int)R.Get(entity, "FireballIndex");
            record.Origin = (Vector3)R.Get(entity, "LocalPosition");
            record.Velocity = (Vector3)R.Get(entity, "Velocity");
            record.ConstructedByOriginal = true;
            knownFireballs = live.Count;
        }
        else
        {
            // The constructor stopped before it positioned the fireball (no
            // particle content in this configuration). Transcribed from
            // FireballEntity..ctor: Velocity = delta * (FireballVelocity / |delta|).
            FireballConstructionTranscribed = true;
            record.Index = (int)pending[0];
            record.Origin = muzzle;
            Vector3 target;
            if (fromWaypoint)
            {
                var waypoints = (IList)R.Get(client, "Waypoints");
                target = (Vector3)R.Get(waypoints[0], "ActionPosition");
            }
            else target = (Vector3)R.Get(client, "TargetPosition");
            Vector3 delta = target - muzzle;
            float speed = (float)R.Get(R.Get(client, "EType"), "FireballVelocity");
            record.Velocity = delta * (speed / delta.Length());
        }
        // ShootFireball never reached these two lines.
        pending.RemoveAt(0);
        if (!fromWaypoint) R.Set(client, "WaitingToShoot", false);
        Fireballs.Add(record);
    }
}

internal sealed class TraceMessage
{
    public int Tick;
    public string Name;
    public bool Reliable;
    public byte Sender;
    public byte[] Payload;
}

internal static class Program
{
    private static string gameDirectory;
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    // XNA's fixed step: Game.TargetElapsedTime = 166667 ticks.
    private static readonly TimeSpan Step = TimeSpan.FromTicks(166667);

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

    private static string F(float value) { return value.ToString("R", Inv); }
    private static string V(Vector3 value) { return F(value.X) + " " + F(value.Y) + " " + F(value.Z); }
    private static string Hex(byte[] bytes) { return bytes.Length == 0 ? "-" : BitConverter.ToString(bytes).Replace("-", ""); }

    private static byte[] Unhex(string text)
    {
        if (text == "-") return new byte[0];
        var bytes = new byte[text.Length / 2];
        for (int index = 0; index < bytes.Length; ++index)
            bytes[index] = byte.Parse(text.Substring(index * 2, 2), NumberStyles.HexNumber, Inv);
        return bytes;
    }

    private static void WriteFireballs(TextWriter output, Peer peer, ref int written)
    {
        for (; written < peer.Fireballs.Count; ++written)
        {
            FireballRecord fireball = peer.Fireballs[written];
            output.WriteLine("F " + fireball.Tick + " " + fireball.Index + " " + V(fireball.Origin) + " " +
                             V(fireball.Velocity) + " " + V(fireball.LocalMuzzle));
        }
    }

    private static void WriteClient(TextWriter output, string tag, int tick, object client)
    {
        if (client == null) { output.WriteLine(tag + " " + tick + " 0 0 0 0 0 0"); return; }
        bool visible = (bool)R.Get(client, "Visible");
        bool dead = (bool)R.Get(client, "Dead");
        output.WriteLine(tag + " " + tick + " " + V((Vector3)R.Get(client, "WorldPosition")) + " " +
                         (visible ? 1 : 0) + " " + F((float)R.Get(client, "CurrentInterpolationTime")) + " " + (dead ? 1 : 0));
    }

    // The original owner. Everything a real spawner runs is run: the spawn
    // handler, DragonEntity.Update with its real state machine, the real send
    // path, and the owner's own echo into its DragonClientEntity.
    private static int Owner(string[] args)
    {
        string output = args[2];
        int kind = int.Parse(args[3], Inv);
        int ticks = (int)Math.Round(double.Parse(args[4], Inv) * 60.0);
        int seed = int.Parse(args[5], Inv);
        byte gid = byte.Parse(args[6], Inv);
        var player = new Vector3(8.5f, 64f, 8.5f);
        Peer peer = Peer.Create(gameDirectory, gid, player);
        R.SetStatic(peer.T("DNA.MathTools"), "Rnd", new Random(seed));
        Type updateType = peer.T("DNA.CastleMinerZ.Net.UpdateDragonMessage");
        Type states = peer.T("DNA.CastleMinerZ.AI.DragonBaseState");

        using (var writer = new StreamWriter(output, false, new UTF8Encoding(false)))
        {
            writer.WriteLine("# openminerz.dragon-trace.v1 source=original");
            writer.WriteLine("H " + F(1f / 60f) + " " + kind + " " + gid + " " + ticks + " " + V(player));
            // SpawnDragonMessage as the host would send it for this spawner.
            byte[] spawn;
            using (var stream = new MemoryStream())
            using (var binary = new BinaryWriter(stream))
            {
                binary.Write((byte)kind); binary.Write(gid); binary.Write(false); binary.Write(-1f);
                binary.Flush();
                spawn = stream.ToArray();
            }
            writer.WriteLine("M 0 SpawnDragonMessage 1 0 " + Hex(spawn));
            peer.Deliver("SpawnDragonMessage", spawn, 0);
            object brain = peer.Brain;
            if (brain == null) throw new InvalidOperationException("Original spawn handler created no DragonEntity");
            MethodInfo brainUpdate = R.Method(brain.GetType(), "Update", 2);
            object machine = R.Get(brain, "StateMachine");
            float viewDistance = (float)R.Get(R.Get(brain, "EType"), "MaxViewDistance");

            var echo = new List<TraceMessage>();
            bool acquired = false;
            int fireballsWritten = 0;
            for (int tick = 1; tick <= ticks; ++tick)
            {
                double total = tick / 60.0;
                foreach (TraceMessage message in echo) peer.Deliver(message.Name, message.Payload, gid);
                echo.Clear();

                int sentBefore = (int)R.Get(brain, "UpdatesSent");
                int shotsBefore = (int)R.GetStatic(brain.GetType(), "NextFireballIndex");
                R.Invoke(brainUpdate, brain, new object[] { null, new GameTime(TimeSpan.FromSeconds(total), Step) });
                int sentAfter = (int)R.Get(brain, "UpdatesSent");
                var position = (Vector3)R.Get(brain, "LocalPosition");
                if (sentAfter != sentBefore)
                {
                    // SendRegularUpdate sends exactly one of the two per slot.
                    var message = new TraceMessage { Tick = tick, Sender = gid };
                    // DragonEntity.NextFireballIndex only advances inside SendAttack.
                    bool attack = (int)R.GetStatic(brain.GetType(), "NextFireballIndex") != shotsBefore;
                    message.Name = attack ? "DragonAttackMessage" : "UpdateDragonMessage";
                    message.Payload = peer.Encode(peer.SendInstance(message.Name));
                    message.Reliable = attack || (int)R.GetStatic(updateType, "UpdateCount") <= 2;
                    writer.WriteLine("M " + tick + " " + message.Name + " " + (message.Reliable ? 1 : 0) + " " + gid + " " + Hex(message.Payload));
                    echo.Add(message);
                }
                // The whole observable DragonEntity state, for the brain differential.
                writer.WriteLine("B " + tick + " " + F((float)R.Get(brain, "DragonTime")) + " " + V(position) + " " +
                                 R.Get(machine, "_currentState").GetType().Name + " " +
                                 F((float)R.Get(brain, "_yaw")) + " " + F((float)R.Get(brain, "_pitch")) + " " +
                                 F((float)R.Get(brain, "_roll")) + " " + F((float)R.Get(brain, "_velocity")) + " " +
                                 Convert.ToInt32(R.Get(brain, "_currentAnimation")) + " " +
                                 Convert.ToInt32(R.Get(brain, "NextAnimation")) + " " +
                                 F((float)R.Get(brain, "FlapDebt")) + " " + (int)R.Get(brain, "ShotsLeft") + " " +
                                 ((bool)R.Get(brain, "ShotPending") ? 1 : 0) + " " +
                                 F((float)R.Get(brain, "TargetYaw")) + " " + (int)R.Get(brain, "ChancesToNotAttack"));

                // Target acquisition is the one brain decision that needs a live
                // NetworkSession and BlockTerrain. It is injected with exactly the
                // field writes SearchForNewTarget performs, minus entity.Target
                // (a null Target makes every state steer at the remembered
                // TravelTarget, which is the player's fixed position here).
                if (!acquired && Vector3.Distance(position, player) < viewDistance)
                {
                    acquired = true;
                    R.Set(brain, "TravelTarget", player);
                    R.Set(brain, "NextSound", Enum.Parse(peer.T("DNA.CastleMinerZ.AI.DragonSoundEnum"), "CRY"));
                    R.Set(brain, "ChancesToNotAttack", 0);
                    R.Set(brain, "TimeLeftTilShotsHeard", 0f);
                    R.Set(brain, "LoitersLeft", 1000000);
                    R.Call(machine, "ChangeState", R.CallStatic(states, "GetNextAttackType", brain));
                }

                peer.UpdateClient(tick, total, Step);
                WriteClient(writer, "P", tick, peer.Client);
                if ((bool)R.Get(peer.Client, "Visible"))
                    writer.WriteLine("Z " + tick + " " + Convert.ToInt32(R.Get(peer.Client, "CurrentAnimation")) + " " +
                                     V(Peer.MuzzleLocal(peer.Client)));
                WriteFireballs(writer, peer, ref fireballsWritten);
            }
            writer.WriteLine("# fireball_construction=" + (peer.FireballConstructionTranscribed ? "transcribed" : "original") +
                             " sound_calls_swallowed=" + peer.SoundCallsSwallowed);
        }
        Console.WriteLine("PASS original owner trace: " + output);
        return 0;
    }

    // What retail does when a second SpawnDragonMessage names a peer that is
    // already flying a dragon (the host answers a RequestDragonMessage whenever
    // its DragonPending flag is clear, and that flag clears on its own echo, so
    // two requests one latency apart both get a spawn). HandleSpawnDragonMessage
    // overwrites `_dragon` without removing the old DragonEntity from the scene:
    // both brains keep running and both keep sending. This runs exactly that.
    private static int SecondBrain(string[] args)
    {
        var player = new Vector3(8.5f, 64f, 8.5f);
        Peer peer = Peer.Create(gameDirectory, 1, player);
        R.SetStatic(peer.T("DNA.MathTools"), "Rnd", new Random(12345));
        byte[] spawn = { 4, 1, 0, 0, 0, 0x80, 0xBF };
        peer.Deliver("SpawnDragonMessage", spawn, 0);
        object first = peer.Brain;
        object firstClient = peer.Client;
        MethodInfo update = R.Method(first.GetType(), "Update", 2);
        object second = null;
        var pending = new List<TraceMessage>();
        int accepted = 0, firstSent = 0, secondSent = 0;
        using (var writer = new StreamWriter(args[2], false, new UTF8Encoding(false)))
        {
            for (int tick = 1; tick <= 1200; ++tick)
            {
                double total = tick / 60.0;
                foreach (TraceMessage message in pending)
                {
                    int before = ((IList)R.Get(peer.Client, "Waypoints")).Count;
                    peer.Deliver(message.Name, message.Payload, 1);
                    if (((IList)R.Get(peer.Client, "Waypoints")).Count > before) ++accepted;
                }
                pending.Clear();
                if (tick == 301)
                {
                    peer.Deliver("SpawnDragonMessage", spawn, 0);
                    second = peer.Brain;
                    writer.WriteLine("# second spawn at tick 301: new DragonEntity=" + !ReferenceEquals(first, second) +
                                     " new DragonClientEntity=" + !ReferenceEquals(firstClient, peer.Client));
                }
                foreach (object brain in new[] { first, second })
                {
                    if (brain == null) continue;
                    int sentBefore = (int)R.Get(brain, "UpdatesSent");
                    int shotsBefore = (int)R.GetStatic(brain.GetType(), "NextFireballIndex");
                    R.Invoke(update, brain, new object[] { null, new GameTime(TimeSpan.FromSeconds(total), Step) });
                    if ((int)R.Get(brain, "UpdatesSent") == sentBefore) continue;
                    bool attack = (int)R.GetStatic(brain.GetType(), "NextFireballIndex") != shotsBefore;
                    string name = attack ? "DragonAttackMessage" : "UpdateDragonMessage";
                    pending.Add(new TraceMessage { Tick = tick, Name = name, Payload = peer.Encode(peer.SendInstance(name)) });
                    if (ReferenceEquals(brain, first)) ++firstSent; else ++secondSent;
                }
                peer.UpdateClient(tick, total, Step);
                object client = peer.Client;
                var shown = (Vector3)R.Get(client, "WorldPosition");
                var one = (Vector3)R.Get(first, "LocalPosition");
                writer.WriteLine("S " + tick + " " + ((bool)R.Get(client, "Visible") ? 1 : 0) + " " + V(shown) + " " +
                                 F(Vector3.Distance(shown, one)) + " " +
                                 (second == null ? "-" : F(Vector3.Distance(shown, (Vector3)R.Get(second, "LocalPosition")))) + " " +
                                 (second == null ? "-" : F(Vector3.Distance(one, (Vector3)R.Get(second, "LocalPosition")))));
            }
            writer.WriteLine("# after the second spawn: first brain sent " + firstSent + " messages in total, second brain " +
                             secondSent + "; the one DragonClientEntity accepted " + accepted + " waypoints");
        }
        Console.WriteLine("PASS second-brain trace: " + args[2]);
        return 0;
    }

    private static List<TraceMessage> ReadMessages(string path, out int ticks, out int kind, out Vector3 player)
    {
        var messages = new List<TraceMessage>();
        ticks = 0; kind = 0; player = Vector3.Zero;
        foreach (string line in File.ReadLines(path))
        {
            string[] parts = line.Split(' ');
            if (parts[0] == "H")
            {
                kind = int.Parse(parts[2], Inv);
                ticks = int.Parse(parts[4], Inv);
                player = new Vector3(float.Parse(parts[5], Inv), float.Parse(parts[6], Inv), float.Parse(parts[7], Inv));
            }
            else if (parts[0] == "M")
                messages.Add(new TraceMessage
                {
                    Tick = int.Parse(parts[1], Inv), Name = parts[2], Reliable = parts[3] == "1",
                    Sender = byte.Parse(parts[4], Inv), Payload = Unhex(parts[5]),
                });
        }
        return messages;
    }

    // The original watcher: a stock client that receives a message stream and
    // presents the dragon. The stream may come from OpenMinerZ or from Owner().
    private static int Replica(string[] args)
    {
        string input = args[2], output = args[3];
        int latency = int.Parse(args[4], Inv);
        byte gid = byte.Parse(args[5], Inv);
        int ticks, kind;
        Vector3 player;
        List<TraceMessage> messages = ReadMessages(input, out ticks, out kind, out player);
        // A watcher well away from the fireball's target, so its own player box
        // never intercepts the shot in a comparison of two presentations.
        Peer peer = Peer.Create(gameDirectory, gid, player + new Vector3(40f, 0f, 0f));
        using (var writer = new StreamWriter(output, false, new UTF8Encoding(false)))
        {
            writer.WriteLine("# openminerz.dragon-replica.v1 source=original latency_ticks=" + latency);
            int next = 0, fireballsWritten = 0;
            for (int tick = 0; tick <= ticks; ++tick)
            {
                for (; next < messages.Count && messages[next].Tick + latency <= tick; ++next)
                    peer.Deliver(messages[next].Name, messages[next].Payload, messages[next].Sender);
                if (tick == 0) continue;
                peer.UpdateClient(tick, tick / 60.0, Step);
                WriteClient(writer, "R", tick, peer.Client);
                WriteFireballs(writer, peer, ref fireballsWritten);
            }
            writer.WriteLine("# fireball_construction=" + (peer.FireballConstructionTranscribed ? "transcribed" : "original") +
                             " sound_calls_swallowed=" + peer.SoundCallsSwallowed);
        }
        Console.WriteLine("PASS original replica trace: " + output);
        return 0;
    }

    // Constants straight out of the original statics, for the native side to pin.
    private static int Tables(string[] args)
    {
        Peer peer = Peer.Create(gameDirectory, 0, Vector3.Zero);
        Type dragonType = peer.T("DNA.CastleMinerZ.AI.DragonType");
        var types = (Array)R.GetStatic(dragonType, "Types");
        var lookup = (bool[,])R.GetStatic(dragonType, "BreakLookup");
        using (var writer = new StreamWriter(args[2], false, new UTF8Encoding(false)))
        {
            writer.WriteLine("# openminerz.dragon-tables.v1");
            for (int index = 0; index < types.Length; ++index)
            {
                object type = types.GetValue(index);
                foreach (FieldInfo field in dragonType.GetFields(BindingFlags.Instance | BindingFlags.Public).OrderBy(field => field.Name))
                {
                    object value = field.GetValue(type);
                    if (value is float) writer.WriteLine("T " + index + " " + field.Name + " " + F((float)value));
                    else if (value is int) writer.WriteLine("T " + index + " " + field.Name + " " + value);
                    else if (value is Enum) writer.WriteLine("T " + index + " " + field.Name + " " + Convert.ToInt32(value));
                }
                var row = new StringBuilder();
                for (int block = 0; block < lookup.GetLength(1); ++block) row.Append(lookup[index, block] ? '1' : '0');
                writer.WriteLine("K " + index + " " + row);
            }
            // The authored clip lengths the death sequence waits on.
            object body = R.GetStatic(peer.T("DNA.CastleMinerZ.AI.DragonClientEntity"), "DragonBody");
            var clips = (IDictionary)R.Get(((Model)body).Tag, "AnimationClips");
            foreach (DictionaryEntry clip in clips)
                writer.WriteLine("C " + ((string)clip.Key).Replace(' ', '_') + " " +
                                 F((float)((TimeSpan)R.Get(clip.Value, "Duration")).TotalSeconds));
        }
        Console.WriteLine("PASS original dragon tables: " + args[2]);
        return 0;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length < 3)
        {
            Console.Error.WriteLine("Usage: OpenMinerZ.DragonParityProbe owner <game dir> <out> <kind> <seconds> <seed> <gid>");
            Console.Error.WriteLine("       OpenMinerZ.DragonParityProbe replica <game dir> <in> <out> <latency ticks> <gid>");
            Console.Error.WriteLine("       OpenMinerZ.DragonParityProbe tables <game dir> <out>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[1]);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            switch (args[0])
            {
            case "owner": return Owner(args);
            case "replica": return Replica(args);
            case "tables": return Tables(args);
            case "second-brain": return SecondBrain(args);
            default: return 2;
            }
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
