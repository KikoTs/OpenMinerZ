using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using Microsoft.Xna.Framework;

// Ordinary enemies. No peer replicates an enemy's position: every peer builds
// each enemy from its SpawnEnemyMessage and simulates it locally, so two peers
// only see it in the same place if the simulations agree. This steps the
// ORIGINAL BaseZombie, its states, its collision and its animation clock
// against scripted target paths and writes the result frame by frame.
//
// Executed for real (original IL), in the order Scene.OnUpdate / Entity.Update
// run them each frame:
//   SpawnEnemyMessage.SendData -> RecieveData -> EnemyManager.HandleMessage
//       -> BaseZombie..ctor (real model, skeleton and clips)
//   BasicPhysics.Accelerate                       gravity
//   BaseZombie.ResolveCollsion                    BlockTerrain.Trace / AABBTraceProbe
//   Entity.Update -> BaseZombie.OnUpdate          the state machine, then the
//       -> ModelEntity.OnUpdate                   LayeredAnimationPlayer clock
//   SpeedUpEnemyMessage / EnemyGiveUpMessage / KillEnemyMessage through the
//       original send path, the local echo queue and EnemyManager.HandleMessage
//   BaseZombie.TakeExplosiveDamage                the scripted hits and kills
//   Explosive.EnemyBreakBlocks                    ZombieDig
// Redirected (see Rig): sound cues, RegionIsLoaded, GetEnemyLighting, and here
//   InGameHUD.ApplyDamage and TracerManager.AddArrow, which only record.
// Scripted, not original: the target player's position, velocity, InContact
//   and Dead, written into a real Player object before each frame.
internal struct TargetSample
{
    public Vector3 Position;
    public Vector3 Velocity;
    public bool Contact;
    public bool Dead;
}

internal sealed class Box
{
    public int X0, Y0, Z0, X1, Y1, Z1, Block;
    public Box(int x0, int y0, int z0, int x1, int y1, int z1, int block)
    {
        X0 = x0; Y0 = y0; Z0 = z0; X1 = x1; Y1 = y1; Z1 = z1; Block = block;
    }
}

internal sealed class EnemyScenario
{
    public string Name;
    public int Type;
    public int Id = 7;
    public int Seed = 12345;
    public Vector3 Spawn;
    public float Slow = 1f, Fast = 6.5f, Emerge = 3f, RunActivation = 4f, NormalActivation = 45f;
    public bool Local = true;
    public bool Fest;
    public long Ticks = 166667;
    public float Seconds = 20f;
    public List<Box> Boxes = new List<Box>();
    public Func<float, TargetSample> Target;
    // Seconds -> "hit:<damage>" or "kill" (BaseZombie.TakeExplosiveDamage).
    public SortedDictionary<float, string> Actions = new SortedDictionary<float, string>();
}

internal static class Enemies
{
    private static Type zombieType;
    private static Type managerType;
    private static IList enemies;
    private static object remotePlayer;
    private static MethodInfo handleMessage;
    private static MethodInfo resolve;
    private static MethodInfo update;

    private static void RecordDamage(object self, float amount, Vector3 source)
    {
        Rig.Events.Add("dmg:" + Rig.F(amount));
    }

    private static void RecordArrow(object self, Vector3 position, Vector3 velocity, object target)
    {
        Rig.Events.Add("arrow:" + Rig.V(position) + ":" + Rig.V(velocity));
    }

    private static void NoFuse(object self, object position, object kind)
    {
        Rig.Events.Add("fuse");
    }

    public static void Setup()
    {
        Type hudType = Rig.T("DNA.CastleMinerZ.UI.InGameHUD");
        Rig.Redirect(hudType.GetMethods(R.All).First(method => method.Name == "ApplyDamage"),
                     typeof(Enemies).GetMethod("RecordDamage", BindingFlags.Static | BindingFlags.NonPublic));
        R.SetStatic(hudType, "Instance", R.Inert(hudType));
        Type tracerType = Rig.T("DNA.CastleMinerZ.TracerManager");
        Rig.Redirect(tracerType.GetMethods(R.All).First(method => method.Name == "AddArrow"),
                     typeof(Enemies).GetMethod("RecordArrow", BindingFlags.Static | BindingFlags.NonPublic));
        R.SetStatic(tracerType, "Instance", R.Inert(tracerType));

        R.CallStatic(Rig.T("DNA.CastleMinerZ.AI.EnemyType"), "Init");
        zombieType = Rig.T("DNA.CastleMinerZ.AI.BaseZombie");
        // 1.9.9.8 loads the shared shadow model from an explicit Init that the
        // game calls at start-up; 1.6.3.8 does it in the type initializer.
        MethodInfo zombieInit = zombieType.GetMethod("Init", BindingFlags.Static | BindingFlags.Public, null, Type.EmptyTypes, null);
        if (zombieInit != null) R.Invoke(zombieInit, null, null);
        managerType = Rig.T("DNA.CastleMinerZ.AI.EnemyManager");
        Rig.Manager = R.Inert(managerType);
        Rig.T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                   null, Type.EmptyTypes, null).Invoke(Rig.Manager, null);
        enemies = (IList)Activator.CreateInstance(R.Field(managerType, "_enemies").FieldType);
        R.Set(Rig.Manager, "_enemies", enemies);
        R.Set(Rig.Manager, "_rnd", new Random(1));
        R.Set(Rig.Manager, "zombieProbe", Activator.CreateInstance(R.Field(managerType, "zombieProbe").FieldType));
        R.SetStatic(managerType, "Instance", Rig.Manager);
        handleMessage = managerType.GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .First(method => method.Name == "HandleMessage" && method.GetParameters().Length == 1);
        resolve = zombieType.GetMethod("ResolveCollsion", BindingFlags.Instance | BindingFlags.Public);
        update = zombieType.GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .First(method => method.Name == "Update" && method.GetParameters().Length == 2);
        remotePlayer = Rig.AddRemote(9, "RemoteStock");
    }

    private static string StateName(object state)
    {
        string name = state.GetType().Name;
        if (name.EndsWith("Emerge")) return "emerge";
        if (name.EndsWith("Chase")) return "chase";
        if (name == "ZombieDig") return "dig";
        if (name.EndsWith("TryAttack") || name == "AlienAttack") return "attack";
        if (name.EndsWith("Hit")) return "hit";
        if (name.EndsWith("Die")) return "die";
        if (name.EndsWith("GiveUp")) return "give_up";
        if (name.EndsWith("Idle")) return "idle";
        return name;
    }

    private static string Describe(object zombie)
    {
        Vector3 position = (Vector3)R.Get(zombie, "LocalPosition");
        Vector3 velocity = Rig.Velocity(zombie);
        Vector3 forward = ((Matrix)R.Get(zombie, "LocalToWorld")).Forward;
        float yaw = (float)Math.Atan2(forward.X, -forward.Z);
        object machine = R.Get(zombie, "StateMachine");
        object player = R.Get(zombie, "CurrentPlayer");
        string clip = player == null ? "-" : (string)R.Get(player, "Name");
        long clipTicks = player == null ? 0 : ((TimeSpan)R.Get(player, "CurrentTime")).Ticks;
        float clipSpeed = player == null ? 0f : (float)R.Get(player, "Speed");
        return "p=" + Rig.V(position) + " v=" + Rig.V(velocity) + " yaw=" + Rig.F(yaw) +
               " st=" + StateName(R.Get(machine, "_currentState")) + " clip=" + clip + " ct=" + clipTicks +
               " spd=" + Rig.F(clipSpeed) + " g=" + ((bool)R.Get(zombie, "OnGround") ? 1 : 0) +
               " w=" + ((bool)R.Get(zombie, "TouchingWall") ? 1 : 0) + " hp=" + Rig.F((float)R.Get(zombie, "Health")) +
               " fr=" + Rig.F((float)R.Get(zombie, "FrustrationCount")) +
               " fast=" + ((bool)R.Get(zombie, "IsMovingFast") ? 1 : 0) + " hc=" + (int)R.Get(zombie, "HitCount") +
               " mc=" + (int)R.Get(zombie, "MissCount") + " sc=" + (int)R.Get(zombie, "SwingCount") +
               " ai=" + (int)R.Get(zombie, "AnimationIndex") +
               " blk=" + ((bool)R.Get(zombie, "IsBlocking") ? 1 : 0) + " hit=" + ((bool)R.Get(zombie, "IsHittable") ? 1 : 0);
    }

    private static string TakeEvents()
    {
        string joined = Rig.Events.Count == 0 ? "-" : string.Join(";", Rig.Events);
        Rig.Events.Clear();
        return joined;
    }

    private static readonly List<Captured> inbox = new List<Captured>();

    // What the frame sent. The original reads its echo queue at the top of the
    // next frame, before the scene updates.
    private static void Collect()
    {
        foreach (Captured captured in Rig.Drain())
        {
            Rig.Events.Add("msg:" + captured.Name);
            inbox.Add(captured);
        }
    }

    private static void Deliver()
    {
        Captured[] arrived = inbox.ToArray();
        inbox.Clear();
        foreach (Captured captured in arrived)
        {
            if (captured.Name == "RemoveBlocksMessage")
            {
                // BlockTerrain.SetBlock rebuilds chunk geometry; the block
                // array is cleared directly from the original message instead.
                int count = BitConverter.ToInt32(captured.Payload, 0);
                for (int index = 0; index < count; ++index)
                {
                    int x = BitConverter.ToInt32(captured.Payload, 4 + index * 12);
                    int y = BitConverter.ToInt32(captured.Payload, 8 + index * 12);
                    int z = BitConverter.ToInt32(captured.Payload, 12 + index * 12);
                    Rig.SetBlock(x, y, z, 0);
                    Rig.Events.Add("broke:" + x + "," + y + "," + z);
                }
                continue;
            }
            if (captured.Name != "SpeedUpEnemyMessage" && captured.Name != "EnemyGiveUpMessage" &&
                captured.Name != "KillEnemyMessage") continue;
            R.Invoke(handleMessage, Rig.Manager, new[] { Rig.Decode(captured) });
        }
    }

    public static void Clear()
    {
        foreach (object stale in enemies.Cast<object>().ToArray()) R.Call(Rig.Manager, "RemoveZombie", stale);
        Rig.Drain();
        inbox.Clear();
    }

    // A real zombie targeting the local player, left where it spawns in its
    // emerge state: IsBlocking, which is all Player.ResolveCollsion asks of it.
    public static void SpawnBlocker(Vector3 position)
    {
        var scenario = new EnemyScenario
        {
            Name = "blocker", Type = 0, Id = 100 + enemies.Count, Seed = 1 + enemies.Count, Spawn = position
        };
        int before = enemies.Count;
        byte[] ignored;
        Spawn(scenario, Rig.LocalPlayer, out ignored);
        if (enemies.Count != before + 1) throw new InvalidDataException("the original did not create the blocker");
    }

    // The spawn goes through the original writer and reader, so the init
    // package is the wire's (millisecond integers), not the scenario's floats.
    private static void Spawn(EnemyScenario scenario, object target, out byte[] payload)
    {
        Type messageType = Rig.T("DNA.CastleMinerZ.Net.SpawnEnemyMessage");
        object message = Activator.CreateInstance(messageType, true);
        R.Set(message, "SpawnPosition", scenario.Spawn);
        R.Set(message, "EnemyTypeID", Enum.ToObject(R.Field(messageType, "EnemyTypeID").FieldType, scenario.Type));
        R.Set(message, "EnemyID", scenario.Id);
        R.Set(message, "RandomSeed", scenario.Seed);
        object package = Activator.CreateInstance(R.Field(messageType, "InitPkg").FieldType);
        R.Set(package, "SlowSpeed", scenario.Slow);
        R.Set(package, "FastSpeed", scenario.Fast);
        R.Set(package, "EmergeSpeed", scenario.Emerge);
        R.Set(package, "RunActivationTime", scenario.RunActivation);
        R.Set(package, "NormalActivationTime", scenario.NormalActivation);
        R.Set(message, "InitPkg", package);
        // Modern adds the spawner fields; an unset string cannot be written.
        if (R.Has(messageType, "PlayerName")) R.Set(message, "PlayerName", "");
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            R.Invoke(R.Method(messageType, "SendData", 1), message, new object[] { writer });
            writer.Flush();
            payload = stream.ToArray();
        }
        object received = Activator.CreateInstance(messageType, true);
        R.Set(received, "_sender", R.Get(target, "Gamer"));
        using (var stream = new MemoryStream(payload, false))
        using (var reader = new BinaryReader(stream))
            R.Invoke(R.Method(messageType, "RecieveData", 1), received, new object[] { reader });
        R.Invoke(handleMessage, Rig.Manager, new[] { received });
    }

    public static void Run(EnemyScenario scenario, TextWriter output)
    {
        Rig.ClearTerrain();
        foreach (Box box in scenario.Boxes) Rig.Fill(box.X0, box.Y0, box.Z0, box.X1, box.Y1, box.Z1, box.Block);
        foreach (object stale in enemies.Cast<object>().ToArray()) R.Call(Rig.Manager, "RemoveZombie", stale);
        Rig.Drain();
        inbox.Clear();
        Rig.Events.Clear();
        R.Set(Rig.Manager, "ZombieFestIsOn", scenario.Fest);
        // MathTools.RandomBool decides which dig swing breaks a block.
        R.SetStatic(Rig.T("DNA.MathTools"), "Rnd", new Random(scenario.Seed));

        object target = scenario.Local ? Rig.LocalPlayer : remotePlayer;
        TargetSample first = scenario.Target(0f);
        Rig.MoveEntity(target, first.Position);
        Rig.SetVelocity(target, first.Velocity);
        R.Set(target, "InContact", first.Contact);
        R.Set(target, "Dead", first.Dead);

        byte[] payload;
        Spawn(scenario, target, out payload);
        if (enemies.Count != 1) throw new InvalidDataException(scenario.Name + ": the original did not create the enemy");
        object zombie = enemies[0];
        object physics = Rig.Physics(zombie);

        int frames = (int)Math.Round(scenario.Seconds * 1e7 / scenario.Ticks);
        var init = new int[5];
        for (int index = 0; index < 5; ++index) init[index] = BitConverter.ToInt32(payload, payload.Length - 20 + index * 4);
        output.WriteLine("SCENARIO enemy " + scenario.Name);
        output.WriteLine("ENEMY type=" + scenario.Type + " id=" + scenario.Id + " seed=" + scenario.Seed + " pos=" +
                         Rig.V(scenario.Spawn) + " init=" + string.Join(",", init) + " local=" + (scenario.Local ? 1 : 0) +
                         " ticks=" + scenario.Ticks + " frames=" + frames + " modern=" + (Rig.Modern ? 1 : 0) +
                         " fest=" + (scenario.Fest ? 1 : 0));
        foreach (Box box in scenario.Boxes)
            output.WriteLine("BOX " + box.X0 + " " + box.Y0 + " " + box.Z0 + " " + box.X1 + " " + box.Y1 + " " + box.Z1 + " " + box.Block);
        object data = R.Get(zombie, "_animationData");
        var clips = (IDictionary)R.Get(data, "AnimationClips");
        foreach (DictionaryEntry clip in clips)
            output.WriteLine("CLIP " + clip.Key + " " + ((TimeSpan)R.Get(clip.Value, "Duration")).Ticks);
        output.WriteLine("OUT -1 " + Describe(zombie) + " ev=" + TakeEvents());

        var elapsed = TimeSpan.FromTicks(scenario.Ticks);
        var pending = new SortedDictionary<float, string>(scenario.Actions);
        bool removed = false;
        for (int frame = 0; frame < frames && !removed; ++frame)
        {
            float seconds = (float)(frame * (double)scenario.Ticks * 1e-7);
            var time = new GameTime(TimeSpan.FromTicks(scenario.Ticks * (frame + 1)), elapsed);
            R.Set(Rig.GameInstance, "CurrentGameTime", time);
            Deliver();
            TargetSample sample = scenario.Target(seconds);
            Rig.MoveEntity(target, sample.Position);
            Rig.SetVelocity(target, sample.Velocity);
            R.Set(target, "InContact", sample.Contact);
            R.Set(target, "Dead", sample.Dead);
            string action = "-";
            while (pending.Count > 0 && pending.Keys.First() <= seconds)
            {
                float key = pending.Keys.First();
                action = pending[key];
                pending.Remove(key);
                Type items = Rig.T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
                float amount = action == "kill" ? 100000f : float.Parse(action.Substring(4), Rig.Inv);
                // A remote shooter, so no local kill statistics are touched.
                R.Call(zombie, "TakeExplosiveDamage", amount, (byte)9, Enum.Parse(items, "Grenade"));
            }
            output.WriteLine("FRAME " + frame + " t=" + Rig.V(sample.Position) + " tv=" + Rig.V(sample.Velocity) +
                             " tc=" + (sample.Contact ? 1 : 0) + " td=" + (sample.Dead ? 1 : 0) + " act=" + action);
            R.Invoke(R.Method(physics.GetType(), "Accelerate", 1), physics, new object[] { elapsed });
            R.Invoke(resolve, zombie, new object[] { Rig.Terrain, null, time });
            R.Invoke(R.Method(physics.GetType(), "Move", 1), physics, new object[] { elapsed });
            R.Invoke(update, zombie, new object[] { Rig.GameInstance, time });
            removed = !enemies.Contains(zombie);
            Collect();
            output.WriteLine("OUT " + frame + " " + (removed ? "removed" : Describe(zombie)) + " ev=" + TakeEvents());
        }
        output.WriteLine("END");
        foreach (object stale in enemies.Cast<object>().ToArray()) R.Call(Rig.Manager, "RemoveZombie", stale);
        Rig.Drain();
    }
}
