using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using Microsoft.Xna.Framework;

// Scripted events through the original's own send and apply code.
//
// The session below is the original NetworkSession with one local gamer who is
// the transport host and has no remote gamers. LocalNetworkGamer.SendData then
// does exactly one thing with a broadcast: append it to the local gamer's echo
// queue. Draining that queue yields the exact bytes, order and count of every
// message the original emits for an event, without a socket ever existing.
internal static partial class Program
{
    private sealed class Captured
    {
        public string Name;
        public byte[] Payload;
        public byte Sender;
    }

    private static object session;
    private static object provider;
    private static object localGamer;
    private static object localPlayer;
    private static Type[] messageTypes;
    private static string fixtures;
    private static string natives;

    private static object MakeGamer(Type type, byte id, bool local, bool host, string tag)
    {
        object gamer = Inert(type);
        Set(gamer, "_globalId", id);
        Set(gamer, "_isLocal", local);
        Set(gamer, "_isHost", host);
        Set(gamer, "_gamerTag", tag);
        FieldInfo back = Field(type, "_networkSession");
        Set(gamer, "_networkSession", Activator.CreateInstance(back.FieldType, session));
        if (Has(gamer, "_pendingData"))
            Set(gamer, "_pendingData", Activator.CreateInstance(Field(type, "_pendingData").FieldType));
        return gamer;
    }

    private static object MakeEntity(Type type)
    {
        // Run only Entity's own constructor on an inert instance: transforms and
        // the child list become valid while no derived constructor (which would
        // load models) runs.
        object entity = Inert(type);
        T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null).Invoke(entity, null);
        return entity;
    }

    private static object MakePlayer(object gamer)
    {
        object player = MakeEntity(T("DNA.CastleMinerZ.Player"));
        Set(player, "Gamer", gamer);
        Set(gamer, "_tag", player);
        return player;
    }

    private static object GenericList(Type element)
    {
        return Activator.CreateInstance(typeof(List<>).MakeGenericType(element));
    }

    private static object Collection(Type element, object list)
    {
        Type collection = T("DNA.Net.GamerServices.GamerCollection`1").MakeGenericType(element);
        return Activator.CreateInstance(collection, BindingFlags.Instance | BindingFlags.NonPublic, null,
                                        new[] { list }, null);
    }

    private static readonly List<object> remoteGamers = new List<object>();

    private static object AddRemote(byte id, string tag)
    {
        object gamer = MakeGamer(T("DNA.Net.GamerServices.NetworkGamer"), id, false, false, tag);
        MakePlayer(gamer);
        ((IList)Get(provider, "_allGamers")).Add(gamer);
        remoteGamers.Add(gamer);
        return gamer;
    }

    private static void SetupSession(byte localId)
    {
        Type providerBase = T("DNA.Net.GamerServices.NetworkSessionProvider");
        Type providerType = TryT("DNA.Net.GamerServices.LidgrenProvider.LidgrenNetworkSessionProvider") ??
                            common.GetTypes().First(type => !type.IsAbstract && providerBase.IsAssignableFrom(type));
        Type sessionType = T("DNA.Net.GamerServices.NetworkSession");
        Type localType = T("DNA.Net.GamerServices.LocalNetworkGamer");
        Type gamerType = T("DNA.Net.GamerServices.NetworkGamer");
        provider = Inert(providerType);
        session = Inert(sessionType);
        Set(session, "_sessionProvider", provider);
        Set(provider, "_networkSession", session);
        Set(provider, "_isHost", true);
        Set(provider, "_sessionType", Enum.Parse(T("DNA.Net.GamerServices.NetworkSessionType"), "PlayerMatch"));
        Set(provider, "_properties", Activator.CreateInstance(T("DNA.Net.GamerServices.NetworkSessionProperties")));
        localGamer = MakeGamer(localType, localId, true, true, "LocalStock");
        object locals = GenericList(localType);
        ((IList)locals).Add(localGamer);
        object all = GenericList(gamerType);
        ((IList)all).Add(localGamer);
        // No remote gamer is ever placed in the REMOTE list, so nothing can be
        // handed to a transport. Other players exist only in AllGamers, which is
        // what the gameplay code consults to resolve an id to a Player.
        object remotes = GenericList(gamerType);
        Set(provider, "_localGamers", locals);
        Set(provider, "_localGamerCollection", Collection(localType, locals));
        Set(provider, "_remoteGamers", remotes);
        Set(provider, "_remoteGamerCollection", Collection(gamerType, remotes));
        Set(provider, "_allGamers", all);
        Set(provider, "_allGamerCollection", Collection(gamerType, all));
        Set(gameInstance, "_networkSession", session);
        Set(gameInstance, "MyNetworkGamer", localGamer);
        Set(gameInstance, "TerrainServerID", localId);
        localPlayer = MakePlayer(localGamer);
        Set(gameInstance, "_localPlayer", localPlayer);
        messageTypes = (Type[])Get(T("DNA.Net.Message"), "_messageTypes");
        Drain();
    }

    private static List<Captured> Drain()
    {
        var result = new List<Captured>();
        var pending = (IList)Get(localGamer, "_pendingData");
        foreach (object packet in pending)
        {
            var data = (byte[])Get(packet, "Data");
            var sender = Get(packet, "Sender");
            var payload = new byte[data.Length - 2];
            Buffer.BlockCopy(data, 1, payload, 0, payload.Length);
            byte checksum = 0;
            for (int index = 0; index < data.Length - 1; ++index) checksum ^= data[index];
            if (checksum != data[data.Length - 1]) throw new InvalidDataException("Echo checksum mismatch");
            result.Add(new Captured
            {
                Name = messageTypes[data[0]].Name,
                Payload = payload,
                Sender = (byte)Get(sender, "Id")
            });
        }
        pending.Clear();
        return result;
    }

    private static void SaveFixture(string name, Captured message)
    {
        // <Message>.<label>.bin holds the payload between the id byte and the
        // checksum; the id differs per protocol and is the native side's to add.
        File.WriteAllBytes(Path.Combine(fixtures, message.Name + "." + name + ".bin"), message.Payload);
        facts["fixture." + message.Name + "." + name] = "sender=" + message.Sender;
    }

    private static object Send(string message, params object[] arguments)
    {
        Type type = T("DNA.CastleMinerZ.Net." + message);
        MethodInfo send = type.GetMethods(Any).First(method => method.Name == "Send");
        ParameterInfo[] parameters = send.GetParameters();
        var full = new object[parameters.Length];
        for (int index = 0; index < parameters.Length; ++index)
        {
            if (index < arguments.Length) full[index] = arguments[index];
            else if (parameters[index].HasDefaultValue) full[index] = parameters[index].DefaultValue;
            else if (parameters[index].ParameterType.IsValueType)
                full[index] = Activator.CreateInstance(parameters[index].ParameterType);
        }
        try
        {
            return send.Invoke(null, full);
        }
        catch (TargetInvocationException error)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
    }

    private static object Receive(string message, byte[] payload, object sender)
    {
        Type type = T("DNA.CastleMinerZ.Net." + message);
        object instance = Activator.CreateInstance(type, true);
        Set(instance, "_sender", sender);
        using (var input = new MemoryStream(payload, false))
        using (var reader = new BinaryReader(input))
        {
            type.GetMethod("RecieveData", Any).Invoke(instance, new object[] { reader });
            if (input.Position != input.Length)
                throw new InvalidDataException(message + " reader left " + (input.Length - input.Position) + " bytes");
        }
        return instance;
    }

    private static void SeedOriginalRandom(int seed)
    {
        Set(T("DNA.MathTools"), "Rnd", new Random(seed));
    }

    private static object ItemId(string name)
    {
        return Enum.Parse(T("DNA.CastleMinerZ.Inventory.InventoryItemIDs"), name);
    }

    private static object CreateItem(string name, int count)
    {
        return Call(T("DNA.CastleMinerZ.Inventory.InventoryItem"), "CreateItem", ItemId(name), count);
    }

    private static void Range(string key, IEnumerable<float> values)
    {
        float[] all = values.ToArray();
        Fact(key + ".min", all.Min());
        Fact(key + ".max", all.Max());
    }

    private static void RunSessionScenarios(string fixtureDirectory, string nativeFixtures)
    {
        fixtures = fixtureDirectory;
        natives = nativeFixtures;
        Directory.CreateDirectory(fixtures);
        foreach (string stale in Directory.GetFiles(fixtures, "*.bin")) File.Delete(stale);
        if (!Run("session", () => SetupSession(3))) return;
        Run("spawn-enemy-send", SpawnEnemySend);
        Run("kill-credit", KillCredit);
        Run("enemy-speed-up-apply", SpeedUpApply);
        Run("pickup-send", PickupSend);
        Run("pickup-host-grant", PickupHostGrant);
        Run("combat-send", CombatSend);
        Run("explosion-removal", ExplosionRemoval);
        Run("creative-consumption", CreativeConsumption);
        // Only when the native side produced payloads for this generation; its
        // SpawnEnemyMessage fixtures use the Classic field layout.
        if (natives != null && Directory.Exists(natives) && Directory.GetFiles(natives, "*.bin").Length > 0)
            Run("native-cross-feed", NativeCrossFeed);
    }

    // ------------------------------------------------------------ SpawnEnemy

    private static readonly float[] midnights = { 0f, 0.25f, 0.5f, 0.79f, 0.85f, 1f };

    private static int[] ReadInitPackage(byte[] payload)
    {
        // Vector3 position, byte type, int id, int seed, then the five
        // millisecond integers; Modern appends its spawner fields after them.
        var values = new int[5];
        for (int index = 0; index < 5; ++index)
            values[index] = BitConverter.ToInt32(payload, 21 + index * 4);
        return values;
    }

    private static void SpawnEnemySend()
    {
        Type typeEnum = T("DNA.CastleMinerZ.AI.EnemyTypeEnum");
        string[] names = { "slow", "fast", "emerge", "run_activation", "normal_activation" };
        var position = new Vector3(10.5f, 3f, -7.25f);
        for (int index = 0; index < enemyTypes.Length; ++index)
        {
            object type = Enum.ToObject(typeEnum, index);
            for (int m = 0; m < midnights.Length; ++m)
            {
                SeedOriginalRandom(1000 + index * 16 + m);
                var samples = new List<int[]>();
                for (int sample = 0; sample < 1024; ++sample)
                {
                    Send("SpawnEnemyMessage", localGamer, position, type, midnights[m], sample, 77 + sample);
                    Captured message = Drain().Single();
                    if (message.Name != "SpawnEnemyMessage" || message.Payload[12] != index ||
                        BitConverter.ToInt32(message.Payload, 13) != sample ||
                        BitConverter.ToInt32(message.Payload, 17) != 77 + sample)
                        throw new InvalidDataException("SpawnEnemyMessage did not carry its arguments");
                    samples.Add(ReadInitPackage(message.Payload));
                    if (sample == 0) SaveFixture(index + "." + m, message);
                }
                for (int field = 0; field < 5; ++field)
                {
                    string key = "init." + index + "." + m + "." + names[field];
                    facts[key + ".min"] = samples.Min(values => values[field]).ToString(CultureInfo.InvariantCulture);
                    facts[key + ".max"] = samples.Max(values => values[field]).ToString(CultureInfo.InvariantCulture);
                }
            }
        }
        Check(true, "SpawnEnemyMessage.Send executed for every type and midnight band");
    }

    // ------------------------------------------------------------ KillEnemy

    private static object enemyManager;

    private static object MakeZombie(int typeIndex, int enemyId, object target, Vector3 position, float health)
    {
        Type zombieType = T("DNA.CastleMinerZ.AI.BaseZombie");
        object zombie = MakeEntity(zombieType);
        object type = enemyTypes.GetValue(typeIndex);
        Set(zombie, "EType", type);
        Set(zombie, "EnemyID", enemyId);
        Set(zombie, "Target", target);
        Set(zombie, "Health", health);
        Set(zombie, "LocalPosition", position);
        Set(zombie, "_mgr", enemyManager);
        Set(zombie, "InitPkg", Call(type, "CreateInitPackage", 0.5f));
        Type machine = T("DNA.CastleMinerZ.AI.StateMachine`1").MakeGenericType(zombieType);
        object states = Activator.CreateInstance(machine, zombie);
        // A neutral state: IsDead compares against the type's die state, and
        // EnemyBaseState.HandleSpeedUp is the no-op base implementation.
        Set(states, "_currentState", Activator.CreateInstance(T("DNA.CastleMinerZ.AI.EnemyBaseState")));
        Set(zombie, "StateMachine", states);
        ((IList)Get(enemyManager, "_enemies")).Add(zombie);
        return zombie;
    }

    private static void EnsureEnemyManager()
    {
        if (enemyManager != null) return;
        Type managerType = T("DNA.CastleMinerZ.AI.EnemyManager");
        enemyManager = MakeEntity(managerType);
        Set(enemyManager, "_enemies", GenericList(T("DNA.CastleMinerZ.AI.BaseZombie")));
        Set(managerType, "Instance", enemyManager);
        // The statistics object's constructor talks to Steam; an inert one with
        // its per-item table present is all the kill handler touches.
        object stats = Inert(T("DNA.CastleMinerZ.CastleMinerZPlayerStats"));
        Set(stats, "AllItemStats", Activator.CreateInstance(Field(stats.GetType(), "AllItemStats").FieldType));
        Set(gameInstance, "PlayerStats", stats);
        // Not Endurance: that branch additionally scores through the HUD.
        FieldInfo mode = Field(gameInstance.GetType(), "GameMode");
        Set(gameInstance, "GameMode", Enum.Parse(mode.FieldType, "Survival"));
    }

    private static void KillCredit()
    {
        EnsureEnemyManager();
        object shooter = RemoteGamer(7);
        object target = RemoteGamer(5);
        var enemies = (IList)Get(enemyManager, "_enemies");

        // A remote player's explosion, simulated on THIS peer, takes this peer's
        // copy of another player's zombie to zero.
        enemies.Clear();
        MakeZombie(0, 42, Get(target, "_tag"), new Vector3(0f, 0f, 0f), 5f);
        Call(enemyManager, "ApplyExplosiveDamageToZombies", new Vector3(1f, 0f, 0f), 12f, (byte)7, ItemId("TNT"));
        Captured explosive = Drain().Single();
        Check(explosive.Name == "KillEnemyMessage", "an explosive kill emits exactly one KillEnemyMessage");
        Fact("kill.explosive.sender", explosive.Sender);
        Fact("kill.explosive.enemy", BitConverter.ToInt32(explosive.Payload, 0));
        Fact("kill.explosive.target", BitConverter.ToInt32(explosive.Payload, 4));
        Fact("kill.explosive.weapon", BitConverter.ToInt16(explosive.Payload, 8));
        Fact("kill.explosive.killer", explosive.Payload[10]);
        SaveFixture("explosive-cross-credit", explosive);

        // The same through a replicated bullet: TracerManager calls TakeDamage
        // on every peer with the SHOOTER's id.
        enemies.Clear();
        object zombie = MakeZombie(0, 43, Get(target, "_tag"), new Vector3(0f, 0f, 0f), 0.01f);
        object pistol = Call(T("DNA.CastleMinerZ.Inventory.InventoryItem"), "GetClass", ItemId("Pistol"));
        Call(zombie, "TakeDamage", new Vector3(0f, 0.5f, 0f), new Vector3(1f, 0f, 0f), pistol, (byte)7);
        Captured bullet = Drain().Single();
        Fact("kill.bullet.sender", bullet.Sender);
        Fact("kill.bullet.target", BitConverter.ToInt32(bullet.Payload, 4));
        Fact("kill.bullet.weapon", BitConverter.ToInt16(bullet.Payload, 8));
        Fact("kill.bullet.killer", bullet.Payload[10]);
        SaveFixture("bullet-cross-credit", bullet);
        Check(bullet.Sender == 3 && bullet.Payload[10] == 7 && explosive.Sender == 3 && explosive.Payload[10] == 7,
              "KillEnemyMessage names the damage owner (7) while the observing peer (3) sends it");

        // ApplyExplosiveDamageToZombies' falloff, in health removed.
        foreach (float range in new[] { 8f, 12f, 24f })
            foreach (float distance in new[] { 0f, 0.25f, 0.5f, 0.75f, 0.999f, 1f })
            {
                enemies.Clear();
                object probe = MakeZombie(0, 1, Get(target, "_tag"), new Vector3(0f, 0f, 0f), 1000f);
                // Already in its hit state, so surviving the blast changes no
                // animation (which would need the model) and only Health moves.
                Set(Get(probe, "StateMachine"), "_currentState",
                    Call(enemyTypes.GetValue(0), "GetHitState", probe));
                Call(enemyManager, "ApplyExplosiveDamageToZombies", new Vector3(distance * range, 0f, 0f), range,
                     (byte)7, ItemId("TNT"));
                Fact("explosive.enemy_damage." + range.ToString(CultureInfo.InvariantCulture) + "." +
                     distance.ToString(CultureInfo.InvariantCulture), 1000f - (float)Get(probe, "Health"));
                Drain();
            }

        // HandleKillEnemyMessage: loot is created by the peer the message names
        // as KILLER, at the enemy's position plus one unit up.
        EnsurePickupManager();
        int drops = 0;
        for (int seed = 0; seed < 64 && drops == 0; ++seed)
        {
            SeedOriginalRandom(seed);
            enemies.Clear();
            MakeZombie(0, 50 + seed, Get(target, "_tag"), new Vector3(40f, 6f, -12f), 1f);
            var payload = new byte[11];
            BitConverter.GetBytes(50 + seed).CopyTo(payload, 0);
            BitConverter.GetBytes(5).CopyTo(payload, 4);
            BitConverter.GetBytes((short)Convert.ToInt32(ItemId("Pistol"))).CopyTo(payload, 8);
            payload[10] = 3; // The local player is named as killer; the sender is someone else.
            object kill = Receive("KillEnemyMessage", payload, shooter);
            try { Call(enemyManager, "HandleKillEnemyMessage", kill); }
            catch (NullReferenceException) { /* Kill() then plays the death clip, which needs a model. */ }
            foreach (Captured message in Drain())
            {
                if (message.Name != "CreatePickupMessage") continue;
                ++drops;
                Fact("kill.loot.sender", message.Sender);
                Fact("kill.loot.x", BitConverter.ToSingle(message.Payload, 0));
                Fact("kill.loot.y", BitConverter.ToSingle(message.Payload, 4));
                Fact("kill.loot.z", BitConverter.ToSingle(message.Payload, 8));
                SaveFixture("enemy-loot-40-units-away", message);
            }
        }
        Check(drops == 1, "the named killer publishes the loot pickup at the enemy, however far away that is");
        enemies.Clear();
    }

    private static object EnsurePickupManager()
    {
        Type pickups = T("DNA.CastleMinerZ.PickupManager");
        object manager = Get(Get(pickups, "_instance"), "Target");
        if (manager != null) return manager;
        manager = MakeEntity(pickups);
        Set(manager, "Pickups", GenericList(T("DNA.CastleMinerZ.PickupEntity")));
        Set(manager, "PendingPickupList", GenericList(T("DNA.CastleMinerZ.PickupEntity")));
        Set(Get(pickups, "_instance"), "Target", manager);
        return manager;
    }

    private static object RemoteGamer(byte id)
    {
        object found = remoteGamers.FirstOrDefault(gamer => (byte)Get(gamer, "Id") == id);
        return found ?? AddRemote(id, "Remote" + id);
    }

    private static void SpeedUpApply()
    {
        EnsureEnemyManager();
        var enemies = (IList)Get(enemyManager, "_enemies");
        enemies.Clear();
        object target = RemoteGamer(5);
        object zombie = MakeZombie(3, 91, Get(target, "_tag"), new Vector3(0f, 0f, 0f), 4f);
        var payload = new byte[8];
        BitConverter.GetBytes(91).CopyTo(payload, 0);
        BitConverter.GetBytes(6).CopyTo(payload, 4);
        Call(enemyManager, "HandleMessage", Receive("SpeedUpEnemyMessage", payload, target));
        Check(!(bool)Get(zombie, "IsMovingFast"), "SpeedUpEnemyMessage for another target id leaves the enemy slow");
        BitConverter.GetBytes(5).CopyTo(payload, 4);
        Call(enemyManager, "HandleMessage", Receive("SpeedUpEnemyMessage", payload, target));
        Check((bool)Get(zombie, "IsMovingFast") &&
              (float)Get(zombie, "CurrentSpeed") == (float)Get(Get(zombie, "InitPkg"), "FastSpeed"),
              "SpeedUpEnemyMessage switches the matching enemy to its InitPkg.FastSpeed");
        enemies.Clear();
    }

    // --------------------------------------------------------------- pickups

    private static void PickupSend()
    {
        object manager = EnsurePickupManager();
        object item = CreateItem("WoodBlock", 1);
        var location = new Vector3(4.5f, 2.5f, -9.5f);

        // CreateUpwardPickup: enemy loot and dug blocks (1.5), crate contents (3).
        foreach (float speed in new[] { 1.5f, 3f })
        {
            SeedOriginalRandom(4242);
            var vectors = new List<Vector3>();
            for (int sample = 0; sample < 2048; ++sample)
            {
                Call(manager, "CreateUpwardPickup", item, location, speed);
                Captured message = Drain().Single();
                vectors.Add(new Vector3(BitConverter.ToSingle(message.Payload, 12),
                                        BitConverter.ToSingle(message.Payload, 16),
                                        BitConverter.ToSingle(message.Payload, 20)));
                if (sample < 4) SaveFixture("upward-" + speed.ToString(CultureInfo.InvariantCulture) + "-" + sample, message);
            }
            string key = "pickup.upward." + speed.ToString(CultureInfo.InvariantCulture);
            Range(key + ".speed", vectors.Select(vector => vector.Length()));
            Range(key + ".vy", vectors.Select(vector => vector.Y));
            Range(key + ".vx", vectors.Select(vector => vector.X));
            Range(key + ".vz", vectors.Select(vector => vector.Z));
        }

        // CreatePickup(dropped: true): thrown from the camera's flattened
        // forward at 4 units a second with a small sideways scatter.
        object camera = MakeEntity(T("DNA.Drawing.PerspectiveCamera"));
        Set(localPlayer, "FPSCamera", camera);
        Set(camera, "LocalRotation", Quaternion.CreateFromYawPitchRoll(0.7f, -0.6f, 0f));
        Set(camera, "LocalPosition", new Vector3(1f, 2f, 3f));
        Matrix world = (Matrix)Get(camera, "LocalToWorld");
        Vector3 flat = world.Forward;
        flat.Y = 0f;
        flat.Normalize();
        SeedOriginalRandom(5151);
        var thrown = new List<Vector3>();
        for (int sample = 0; sample < 2048; ++sample)
        {
            Call(manager, "CreatePickup", item, location, true);
            Captured message = Drain().Single();
            thrown.Add(new Vector3(BitConverter.ToSingle(message.Payload, 12),
                                   BitConverter.ToSingle(message.Payload, 16),
                                   BitConverter.ToSingle(message.Payload, 20)));
            if (sample < 4) SaveFixture("dropped-" + sample, message);
        }
        Range("pickup.dropped.speed", thrown.Select(vector => vector.Length()));
        Range("pickup.dropped.vy", thrown.Select(vector => vector.Y));
        Range("pickup.dropped.forward", thrown.Select(vector => Vector3.Dot(vector, flat)));
        Range("pickup.dropped.left", thrown.Select(vector => Vector3.Dot(vector, world.Left)));
        Check(true, "PickupManager.CreatePickup and CreateUpwardPickup executed");
    }

    private static void PickupHostGrant()
    {
        Type entityType = T("DNA.CastleMinerZ.PickupEntity");
        object manager = EnsurePickupManager();
        var list = (IList)Get(manager, "Pickups");
        list.Clear();
        ((IList)Get(manager, "PendingPickupList")).Clear();
        object entity = MakeEntity(entityType);
        Set(entity, "Item", CreateItem("WoodBlock", 3));
        Set(entity, "PickupID", 9);
        Set(entity, "SpawnerID", 4);
        Set(entity, "_displayEntity", MakeEntity(T("DNA.Drawing.Entity")));
        // Hundreds of units from every player: the original host never looks.
        Set(entity, "LocalPosition", new Vector3(300f, 20f, -250f));
        list.Add(entity);

        object requester = RemoteGamer(5);
        var payload = new byte[8];
        BitConverter.GetBytes(9).CopyTo(payload, 0);
        BitConverter.GetBytes(4).CopyTo(payload, 4);
        Call(manager, "HandleMessage", Receive("RequestPickupMessage", payload, requester));
        List<Captured> first = Drain();
        Check(first.Count == 1 && first[0].Name == "ConsumePickupMessage" && first[0].Payload[20] == 5,
              "the original host grants a pickup request with no distance test at all");
        Fact("pickup.host.grants_without_reach_test", first.Count == 1);
        if (first.Count == 1) SaveFixture("host-grant", first[0]);

        object second = RemoteGamer(7);
        Call(manager, "HandleMessage", Receive("RequestPickupMessage", payload, second));
        Check(Drain().Count == 0, "a second request for a pending pickup is ignored, not answered");
        Fact("pickup.host.second_request_ignored", true);

        // A whole pile requested in one frame: every request is granted.
        list.Clear();
        ((IList)Get(manager, "PendingPickupList")).Clear();
        for (int index = 0; index < 96; ++index)
        {
            object pile = MakeEntity(entityType);
            Set(pile, "Item", CreateItem("WoodBlock", 1));
            Set(pile, "PickupID", 100 + index);
            Set(pile, "SpawnerID", 4);
            Set(pile, "_displayEntity", MakeEntity(T("DNA.Drawing.Entity")));
            list.Add(pile);
        }
        for (int index = 0; index < 96; ++index)
        {
            BitConverter.GetBytes(100 + index).CopyTo(payload, 0);
            Call(manager, "HandleMessage", Receive("RequestPickupMessage", payload, requester));
        }
        int granted = Drain().Count(message => message.Name == "ConsumePickupMessage");
        Fact("pickup.host.burst_of_96_granted", granted);
        Check(granted == 96, "the original host grants 96 requests arriving in one frame");
        list.Clear();
        ((IList)Get(manager, "PendingPickupList")).Clear();
    }

    // ---------------------------------------------------------------- combat

    private static void CombatSend()
    {
        Matrix aim = Matrix.CreateWorld(new Vector3(12f, 5.4f, -3f),
                                        Vector3.Normalize(new Vector3(0.3f, 0.2f, -0.9f)), Vector3.Up);
        Type item = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        foreach (string gun in new[] { "Pistol", "AssultRifle", "BoltActionRifle", "BloodStoneSMGGun", "IronSpacePistol" })
        {
            object gunClass = Call(item, "GetClass", ItemId(gun));
            float inaccuracy = (float)Get(gunClass, "Innaccuracy");
            bool drop = (bool)Get(gunClass, "NeedsDropCompensation");
            SeedOriginalRandom(99);
            var directions = new List<Vector3>();
            for (int sample = 0; sample < 1024; ++sample)
            {
                Send("GunshotMessage", localGamer, aim, inaccuracy, ItemId(gun), drop);
                Captured message = Drain().Single();
                directions.Add(new Vector3(BitConverter.ToSingle(message.Payload, 0),
                                           BitConverter.ToSingle(message.Payload, 4),
                                           BitConverter.ToSingle(message.Payload, 8)));
                if (sample == 0) SaveFixture(gun, message);
            }
            string key = "gunshot." + Convert.ToInt32(ItemId(gun));
            Range(key + ".length", directions.Select(direction => direction.Length()));
            Range(key + ".right", directions.Select(direction => Vector3.Dot(direction, aim.Right)));
            Range(key + ".up", directions.Select(direction => Vector3.Dot(direction, aim.Up)));
        }
        foreach (string gun in new[] { "PumpShotgun", "IronSpacePumpShotgun" })
        {
            object gunClass = Call(item, "GetClass", ItemId(gun));
            SeedOriginalRandom(7);
            Send("ShotgunShotMessage", localGamer, aim, (float)Get(gunClass, "Innaccuracy"), ItemId(gun),
                 (bool)Get(gunClass, "NeedsDropCompensation"));
            SaveFixture(gun, Drain().Single());
        }
        Send("FireRocketMessage", localGamer, aim, ItemId("RocketLauncher"), false);
        Captured rocket = Drain().Single();
        SaveFixture("unguided", rocket);
        var rocketDirection = new Vector3(BitConverter.ToSingle(rocket.Payload, 0),
                                          BitConverter.ToSingle(rocket.Payload, 4),
                                          BitConverter.ToSingle(rocket.Payload, 8));
        var rocketPosition = new Vector3(BitConverter.ToSingle(rocket.Payload, 12),
                                         BitConverter.ToSingle(rocket.Payload, 16),
                                         BitConverter.ToSingle(rocket.Payload, 20));
        Fact("rocket.position_ahead_of_camera", Vector3.Distance(rocketPosition, aim.Translation + rocketDirection) < 1e-5f);

        // Player.OnUpdate adds whole frames to grenadeCookTime and only stops at
        // the first frame where it is >= four seconds, so a held grenade leaves
        // the hand with LESS than one second on its fuse. (That frame loop needs
        // the animated avatar and is reproduced here by arithmetic, not run.)
        Type grenadeTypes = T("DNA.CastleMinerZ.GrenadeTypeEnum");
        foreach (int fps in new[] { 30, 60, 144 })
        {
            TimeSpan cook = TimeSpan.Zero;
            TimeSpan frame = TimeSpan.FromTicks(TimeSpan.TicksPerSecond / fps);
            while (cook < TimeSpan.FromSeconds(4.0)) cook += frame;
            float secondsLeft = 5f - (float)cook.TotalSeconds;
            Send("GrenadeMessage", localGamer, aim, Enum.ToObject(grenadeTypes, 0), secondsLeft);
            Captured grenade = Drain().Single();
            SaveFixture("fully-cooked-" + fps + "fps", grenade);
            Fact("grenade.fully_cooked_seconds_left." + fps, secondsLeft);
            var position = new Vector3(BitConverter.ToSingle(grenade.Payload, 12),
                                       BitConverter.ToSingle(grenade.Payload, 16),
                                       BitConverter.ToSingle(grenade.Payload, 20));
            Fact("grenade.position_ahead_of_camera." + fps,
                 Vector3.Distance(position, aim.Translation + aim.Forward) < 1e-5f);
        }
        Send("GrenadeMessage", localGamer, aim, Enum.ToObject(grenadeTypes, 0), 5f);
        SaveFixture("uncooked", Drain().Single());
        Send("DetonateGrenadeMessage", localGamer, new Vector3(20f, 4f, -8f), Enum.ToObject(grenadeTypes, 0), true);
        SaveFixture("he", Drain().Single());
        Send("DetonateRocketMessage", localGamer, new Vector3(60f, 30f, -80f),
             Enum.Parse(T("DNA.CastleMinerZ.Inventory.ExplosiveTypes"), "Rocket"), ItemId("RocketLauncher"), false);
        SaveFixture("terrain", Drain().Single());
        Check(true, "the original combat senders executed");
    }

    // ------------------------------------------------------------ explosions

    private static int[] blocks;
    private static object terrain;
    private static object intVectorType;

    private static object Cell(int x, int y, int z)
    {
        return Activator.CreateInstance((Type)intVectorType, x, y, z);
    }

    private static void EnsureTerrain()
    {
        if (terrain != null) return;
        Type terrainType = T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        intVectorType = T("DNA.IntVector3");
        terrain = Inert(terrainType);
        blocks = new int[384 * 128 * 384];
        Set(terrain, "_blocks", blocks);
        Set(terrain, "_worldMin", Cell(-192, -64, -192));
        Set(terrainType, "_theTerrain", terrain);
        object world = Inert(T("DNA.CastleMinerZ.WorldInfo"));
        Set(world, "Crates", Activator.CreateInstance(Field(world.GetType(), "Crates").FieldType));
        Set(gameInstance, "CurrentWorld", world);
    }

    private static void SetBlock(int x, int y, int z, int block)
    {
        blocks[(y + 64) + (x + 192) * 128 + (z + 192) * 128 * 384] = block << 12;
    }

    private static void ExplosionRemoval()
    {
        EnsureTerrain();
        Type explosive = T("DNA.CastleMinerZ.Inventory.Explosive");
        Type kinds = T("DNA.CastleMinerZ.Inventory.ExplosiveTypes");
        Type blockEnum = T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        var destruction = (int[])Get(explosive, "cDestructionRanges");
        var chainSources = new HashSet<int>
        {
            Convert.ToInt32(Enum.Parse(blockEnum, "TNT")), Convert.ToInt32(Enum.Parse(blockEnum, "C4"))
        };
        int crate = Convert.ToInt32(Enum.Parse(blockEnum, "Crate"));
        const int reach = 5;
        foreach (object kind in Enum.GetValues(kinds))
        {
            int e = Convert.ToInt32(kind);
            if (e >= destruction.Length) continue;
            Fact("explosive." + e + ".destruction", destruction[e]);
            Fact("explosive." + e + ".enemy_range", ((float[])Get(explosive, "cEnemyDamageRanges"))[e]);
            Fact("explosive." + e + ".damage_range", ((float[])Get(explosive, "cDamageRanges"))[e]);
            Fact("explosive." + e + ".kill_range", ((float[])Get(explosive, "cKillRanges"))[e]);
            for (int block = 0; block < blockCount; ++block)
            {
                if (chainSources.Contains(block)) continue;
                // A solid cube of one material around the charge. Whatever
                // FindBlocksToRemove puts in RemoveBlocksMessage IS the answer.
                for (int x = -reach; x <= reach; ++x)
                    for (int y = -reach; y <= reach; ++y)
                        for (int z = -reach; z <= reach; ++z)
                            SetBlock(x, y, z, block);
                Call(explosive, "FindBlocksToRemove", Cell(0, 0, 0), kind, false);
                List<Captured> sent = Drain();
                Captured removal = sent.Last();
                if (removal.Name != "RemoveBlocksMessage")
                    throw new InvalidDataException("FindBlocksToRemove did not end with RemoveBlocksMessage");
                int count = BitConverter.ToInt32(removal.Payload, 0);
                var removed = new HashSet<Tuple<int, int, int>>();
                for (int index = 0; index < count; ++index)
                    removed.Add(Tuple.Create(BitConverter.ToInt32(removal.Payload, 4 + index * 12),
                                             BitConverter.ToInt32(removal.Payload, 8 + index * 12),
                                             BitConverter.ToInt32(removal.Payload, 12 + index * 12)));
                bool chargeCell = e <= 1; // TNT and C4 always remove their own cell.
                // Horizontal and downward reach. A lower door half also takes
                // the cell above it, so upward extent is reported separately.
                int radius = -1;
                foreach (var cell in removed)
                {
                    if (chargeCell && cell.Item1 == 0 && cell.Item2 == 0 && cell.Item3 == 0) continue;
                    radius = Math.Max(radius, Math.Max(Math.Abs(cell.Item1), Math.Max(-cell.Item2, Math.Abs(cell.Item3))));
                }
                int origin = removed.Contains(Tuple.Create(0, 0, 0)) ? 1 : 0;
                int side = 2 * radius + 1;
                int cube = radius < 0 ? 0 : side * side * side;
                if (radius >= 0 && origin == 0) --cube;
                if (radius < 0) cube = origin;
                Fact("explosive." + e + ".breaks." + block, radius);
                Fact("explosive." + e + ".removes_own_cell." + block, origin);
                Fact("explosive." + e + ".cells_beyond_cube." + block, removed.Count - cube);
                if (block == crate)
                    Fact("explosive." + e + ".crate_destroy_messages",
                         sent.Count(message => message.Name == "DestroyCrateMessage"));
            }
        }

        // The message train of one open-air HE grenade: nothing breakable is in
        // range, and the original still broadcasts an EMPTY RemoveBlocksMessage.
        for (int x = -reach; x <= reach; ++x)
            for (int y = -reach; y <= reach; ++y)
                for (int z = -reach; z <= reach; ++z)
                    SetBlock(x, y, z, 0);
        Call(explosive, "FindBlocksToRemove", Cell(0, 0, 0), Enum.Parse(kinds, "HEGrenade"), false);
        List<Captured> open = Drain();
        Fact("explosive.open_air.messages", string.Join(",", open.Select(message => message.Name)));
        Fact("explosive.open_air.block_count", BitConverter.ToInt32(open.Last().Payload, 0));
        SaveFixture("empty-open-air", open.Last());

        // One TNT beside a second TNT and some dirt: chain order and contents.
        int tnt = Convert.ToInt32(Enum.Parse(blockEnum, "TNT"));
        int dirt = Convert.ToInt32(Enum.Parse(blockEnum, "Dirt"));
        for (int x = -reach; x <= reach; ++x)
            for (int y = -reach; y <= reach; ++y)
                for (int z = -reach; z <= reach; ++z)
                    SetBlock(x, y, z, y < 0 ? dirt : 0);
        SetBlock(0, 0, 0, tnt);
        SetBlock(2, 0, 0, tnt);
        Call(explosive, "FindBlocksToRemove", Cell(0, 0, 0), Enum.Parse(kinds, "TNT"), false);
        List<Captured> chain = Drain();
        Fact("explosive.chain.messages", string.Join(",", chain.Select(message => message.Name)));
        Fact("explosive.chain.block_count", BitConverter.ToInt32(chain.Last().Payload, 0));
        foreach (Captured message in chain)
            if (message.Name == "DetonateExplosiveMessage")
            {
                Fact("explosive.chain.secondary_original_flag", message.Payload[12]);
                SaveFixture("chained", message);
            }
        SaveFixture("tnt-chain", chain.Last());
        Check(true, "Explosive.FindBlocksToRemove executed over every block type and explosive kind");
    }

    // ---------------------------------------------------------------- creative

    private static void CreativeConsumption()
    {
        Type inventoryType = T("DNA.CastleMinerZ.Inventory.PlayerInventory");
        object inventory = Inert(inventoryType);
        foreach (FieldInfo field in inventoryType.GetFields(Any))
            if (field.FieldType.IsArray && !field.IsStatic)
                field.SetValue(inventory, Array.CreateInstance(field.FieldType.GetElementType(), 48));
        MethodInfo consume = inventoryType.GetMethods(Any).First(method =>
            method.Name == "Consume" && method.GetParameters()[0].ParameterType.Name == "InventoryItem");
        // An online Classic session: PlayerMatch, with SessionProperties[0]
        // holding the network version, as CastleMinerZGame.HostGame sets it.
        object properties = Get(provider, "_properties");
        int version = Convert.ToInt32(facts["info.network-version"], CultureInfo.InvariantCulture);
        properties.GetType().GetProperty("Item").SetValue(properties, (int?)version, new object[] { 0 });
        foreach (bool infinite in new[] { false, true })
        {
            Set(gameInstance, "InfiniteResourceMode", infinite);
            object stack = CreateItem("WoodBlock", 10);
            object[] arguments = consume.GetParameters().Length == 2
                ? new object[] { stack, 3 }
                : new object[] { stack, 3, false };
            bool consumed = (bool)consume.Invoke(inventory, arguments);
            Fact("creative.online.consume_returns." + Flag(infinite), consumed);
            Fact("creative.online.stack_after_consuming_3_of_10." + Flag(infinite), (int)Get(stack, "StackCount"));
            object tool = CreateItem("StonePickAxe", 1);
            float before = (float)Get(tool, "ItemHealthLevel");
            Call(tool, "InflictDamage");
            Fact("creative.online.pick_wear_per_use." + Flag(infinite), before - (float)Get(tool, "ItemHealthLevel"));
        }
        Set(gameInstance, "InfiniteResourceMode", false);
        Check(facts["creative.online.stack_after_consuming_3_of_10.1"] == "10" &&
              facts["creative.online.stack_after_consuming_3_of_10.0"] == "7",
              "online Creative consumes nothing; the stock binary has no network-version guard on it");
    }

    // ----------------------------------------------------------- native bytes

    // The native client's own payloads, through the original's readers and
    // handlers: does a stock peer end where the native sender meant it to?
    private static void NativeCrossFeed()
    {
        if (!Directory.Exists(natives)) throw new DirectoryNotFoundException(natives);
        EnsureEnemyManager();
        object sender = RemoteGamer(5);
        int checkedFiles = 0;
        foreach (string file in Directory.GetFiles(natives, "*.bin").OrderBy(path => path, StringComparer.Ordinal))
        {
            string[] parts = Path.GetFileNameWithoutExtension(file).Split('.');
            byte[] payload = File.ReadAllBytes(file);
            object message = Receive(parts[0], payload, sender);
            ++checkedFiles;
            if (parts[0] == "SpawnEnemyMessage")
            {
                int type = int.Parse(parts[1], CultureInfo.InvariantCulture);
                int band = int.Parse(parts[2], CultureInfo.InvariantCulture);
                int[] values = ReadInitPackage(payload);
                string[] names = { "slow", "fast", "emerge", "run_activation", "normal_activation" };
                bool inside = Convert.ToInt32(Get(message, "EnemyTypeID")) == type;
                for (int field = 0; field < 5; ++field)
                {
                    string key = "init." + type + "." + band + "." + names[field];
                    int low = int.Parse(facts[key + ".min"], CultureInfo.InvariantCulture);
                    int high = int.Parse(facts[key + ".max"], CultureInfo.InvariantCulture);
                    // 1024 samples approach but need not touch the true bounds.
                    int slack = Math.Max(2, (high - low) / 50 + 2);
                    if (values[field] < low - slack || values[field] > high + slack)
                    {
                        inside = false;
                        Console.WriteLine("  native " + key + "=" + values[field] + " outside original [" + low + "," + high + "]");
                    }
                }
                if (!inside) Check(false, "native " + Path.GetFileName(file) + " lies inside the original's init envelope");
            }
            else if (parts[0] == "CreatePickupMessage")
            {
                var vector = (Vector3)Get(message, "SpawnVector");
                string kind = parts[1];
                string key = kind == "dropped" ? "pickup.dropped" : "pickup.upward." + parts[2].Replace('_', '.');
                float low = BitConverter.ToSingle(BitConverter.GetBytes(Convert.ToUInt32(facts[key + ".speed.min"].Substring(2), 16)), 0);
                float high = BitConverter.ToSingle(BitConverter.GetBytes(Convert.ToUInt32(facts[key + ".speed.max"].Substring(2), 16)), 0);
                if (vector.Length() < low - 0.01f || vector.Length() > high + 0.01f)
                    Check(false, "native " + Path.GetFileName(file) + " launch speed " + vector.Length() +
                                 " matches the original's [" + low + "," + high + "]");
            }
        }
        Fact("native.cross_feed.files", checkedFiles);
        Check(checkedFiles > 0, "native payloads were read by the original's readers (" + checkedFiles + " files)");
    }
}
