using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;

// Executes GAMEPLAY code from the user's owned, unmodified Castle Miner Z
// assemblies offline and records what it does, so the native client can be
// compared with the original instead of with a transcription of it.
//
// PacketContractProbe proves the byte layout of every message. This probe is
// about what the bytes MEAN: which messages the original emits for an event,
// with which values, and what state its handlers end in. Nothing here starts
// Steam, opens a window, creates a graphics device or sends network traffic:
// the session is a host with no remote gamers, so every send lands only in the
// original's own local-echo queue, which is where the probe reads it back.
internal static partial class Program
{
    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static |
                                     BindingFlags.Public | BindingFlags.NonPublic;

    private static string gameDirectory;
    private static Assembly game;
    private static Assembly common;
    private static readonly SortedDictionary<string, string> facts =
        new SortedDictionary<string, string>(StringComparer.Ordinal);
    private static readonly List<string> notes = new List<string>();
    private static int failures;

    // ---------------------------------------------------------------- helpers

    private sealed class NoServices : IServiceProvider
    {
        public object GetService(Type serviceType) { return null; }
    }

    // Content is the only thing between the original's data tables and an
    // offline process. Handing back inert, never-finalized instances lets the
    // original constructors run and fill in every gameplay number; any code
    // that tried to DRAW with them would fail, and nothing here draws.
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

    private static Type TryT(string name)
    {
        return game.GetType(name, false) ?? common.GetType(name, false);
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
                    if (wanted.IsByRef) wanted = wanted.GetElementType();
                    object argument = arguments[index];
                    fits = argument == null ? !wanted.IsValueType || Nullable.GetUnderlyingType(wanted) != null
                                            : wanted.IsInstanceOfType(argument);
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

    private static string Bits(float value)
    {
        return "f:" + BitConverter.ToUInt32(BitConverter.GetBytes(value), 0).ToString("x8");
    }

    private static string Bits(double value)
    {
        return Bits((float)value);
    }

    private static string Flag(bool value) { return value ? "1" : "0"; }

    private static void Fact(string key, object value)
    {
        string text = value is float f ? Bits(f)
                    : value is double d ? Bits(d)
                    : value is bool b ? Flag(b)
                    : Convert.ToString(value, CultureInfo.InvariantCulture);
        facts[key] = text;
    }

    // A member that one generation renamed or dropped is skipped, not fatal.
    private static void Optional(string key, Func<object> read)
    {
        object value;
        try { value = read(); }
        catch (MissingMemberException) { return; }
        if (value != null && value.GetType().Name == "Angle") value = (float)Get(value, "Radians");
        Fact(key, value);
    }

    private static void Note(string text)
    {
        notes.Add(text);
        Console.WriteLine("NOTE " + text);
    }

    private static void Check(bool condition, string what)
    {
        if (condition)
        {
            Console.WriteLine("PASS " + what);
            return;
        }
        ++failures;
        Console.WriteLine("FAIL " + what);
    }

    private static string Describe(Exception error)
    {
        while (error.InnerException != null) error = error.InnerException;
        string[] frames = (error.StackTrace ?? "").Split('\n').Select(frame => frame.Trim()).ToArray();
        string where = frames.FirstOrDefault() ?? "";
        string probe = frames.FirstOrDefault(frame => frame.Contains("Program.") && !frame.Contains("Program.Call") &&
                                                      !frame.Contains("Program.Get") && !frame.Contains("Program.Send("));
        return error.GetType().Name + ": " + error.Message.Replace('\r', ' ').Replace('\n', ' ') +
               " @" + where + (probe != null && probe != where ? " <- " + probe : "");
    }

    // Runs one scenario. A scenario that cannot execute offline is reported by
    // name and never silently counted as verified.
    private static bool Run(string name, Action body)
    {
        try
        {
            body();
            facts["scenario." + name] = "executed";
            return true;
        }
        catch (Exception error)
        {
            facts["scenario." + name] = "not-executable";
            Note("scenario " + name + " could not execute: " + Describe(error));
            return false;
        }
    }

    // ------------------------------------------------------------------- main

    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine(
                "Usage: OpenMinerZ.SemanticsParityProbe <owned game directory> <output directory> [native fixture directory]");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        string output = Path.GetFullPath(args[1]);
        string nativeFixtures = args.Length > 2 ? Path.GetFullPath(args[2]) : null;
        Directory.CreateDirectory(output);
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

            Run("bootstrap", Bootstrap);
            Run("enums", DumpEnums);
            Run("stock-rule-shape", DumpStockRuleShape);
            Run("random", DumpRandom);
            Run("blocks", DumpBlocks);
            Run("enemy-types", DumpEnemyTypes);
            Run("items", DumpItems);
            Run("dig-tables", DumpDigTables);
            RunSessionScenarios(Path.Combine(output, "fixtures"), nativeFixtures);
            // Last: it leaves a terrain installed that no earlier scenario expects.
            Run("environment", DumpEnvironment);
            Run("landing", DumpLanding);
            Run("spawn-rules", DumpSpawnRules);

            using (var writer = new StreamWriter(Path.Combine(output, "original-facts.txt"), false,
                                                 new UTF8Encoding(false)))
            {
                foreach (var fact in facts) writer.WriteLine(fact.Key + "=" + fact.Value);
                foreach (string note in notes) writer.WriteLine("note." + notes.IndexOf(note) + "=" + note);
            }
            Console.WriteLine("Original semantics facts: " + facts.Count + ", failures: " + failures);
            return failures == 0 ? 0 : 1;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 2;
        }
    }

    // -------------------------------------------------------------- bootstrap

    private static object gameInstance;

    private static void Bootstrap()
    {
        // An inert field container in place of the game object: no constructor,
        // window, graphics device, Steam or update loop is ever run.
        Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
        // What Program.Main does through CommonAssembly.Initalize: it is how the
        // original's Message registry finds the game's message classes, so the
        // ids it assigns here are the ones a running client assigns.
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
        Fact("info.network-version",
             Enum.GetValues(T("NetworkVersions")).Cast<object>()
                 .Where(value => value.ToString() == "CurrentVersion").Select(Convert.ToInt32).First());
    }

    // ------------------------------------------------------------------ enums

    private static void DumpEnums()
    {
        foreach (string name in new[]
                 {
                     "DNA.CastleMinerZ.Inventory.InventoryItemIDs", "DNA.CastleMinerZ.Terrain.BlockTypeEnum",
                     "DNA.CastleMinerZ.AI.EnemyTypeEnum", "DNA.CastleMinerZ.GrenadeTypeEnum",
                     "DNA.CastleMinerZ.Inventory.ExplosiveTypes", "DNA.CastleMinerZ.AI.DamageType",
                     "DNA.CastleMinerZ.UI.GameModeTypes", "DNA.CastleMinerZ.UI.GameDifficultyTypes",
                     "DNA.CastleMinerZ.AI.DragonTypeEnum"
                 })
        {
            Type type = TryT(name);
            if (type == null)
            {
                Note("enum absent in this generation: " + name);
                continue;
            }
            foreach (object value in Enum.GetValues(type))
                Fact("enum." + type.Name + "." + Convert.ToInt32(value), value.ToString());
        }
    }

    // ------------------------------------------------------ stock rule shape

    // Several rules were once attributed to "the original" from a source tree
    // that is in fact the OpenClassic mod. Each has a member only the mod has;
    // asking the SHIPPED assembly whether that member exists settles it.
    private static void DumpStockRuleShape()
    {
        Type terrain = T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        string[] overloads = terrain.GetMethods(Any).Where(method => method.Name == "SetBlock")
            .Select(method => string.Join(",", method.GetParameters().Select(parameter => parameter.ParameterType.Name)))
            .OrderBy(text => text, StringComparer.Ordinal).ToArray();
        Fact("stock.setblock_overloads", string.Join(" | ", overloads));
        Fact("stock.setblock_has_admin_argument",
             terrain.GetMethods(Any).Any(method => method.Name == "SetBlock" &&
                 method.GetParameters().Any(parameter => parameter.ParameterType == typeof(bool))));
        Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
        Fact("stock.has_block_edit_safety_sweep",
             gameType.GetFields(Any).Any(field => field.Name == "_safetyCheckTimer"));
        Type pickups = T("DNA.CastleMinerZ.PickupManager");
        Fact("stock.pickup_manager_has_create_queue",
             pickups.GetFields(Any).Any(field => field.Name == "_createQueue" || field.Name == "_maxPickups"));
        Check(!(bool)(facts["stock.setblock_has_admin_argument"] == "1") &&
              facts["stock.has_block_edit_safety_sweep"] == "0" &&
              facts["stock.pickup_manager_has_create_queue"] == "0",
              "the shipped assembly has no admin SetBlock, no edit-rate sweep and no pickup queue");
    }

    // --------------------------------------------------------- System.Random

    // BaseZombie seeds one System.Random per enemy from SpawnEnemyMessage and
    // every peer draws its animation and attack choices from it, so the draw
    // sequence is part of the wire contract. This is the real runtime's
    // generator, called in the exact shapes the enemy states use.
    private static void DumpRandom()
    {
        foreach (int seed in new[] { 0, 1, 2, 77, 12345, 987654321, int.MaxValue, 1073741823 })
        {
            var random = new Random(seed);
            var parts = new List<string>();
            for (int round = 0; round < 6; ++round)
            {
                parts.Add(random.Next(4).ToString(CultureInfo.InvariantCulture));
                parts.Add(Bits((float)random.NextDouble()));
                parts.Add(random.Next(2).ToString(CultureInfo.InvariantCulture));
                parts.Add(random.Next(5).ToString(CultureInfo.InvariantCulture));
                parts.Add(random.Next(0, 3).ToString(CultureInfo.InvariantCulture));
                parts.Add(random.Next(3, 5).ToString(CultureInfo.InvariantCulture));
                parts.Add(random.Next(1, 3).ToString(CultureInfo.InvariantCulture));
                parts.Add(random.Next(7).ToString(CultureInfo.InvariantCulture));
                parts.Add(Bits(1f + (float)random.NextDouble() * 2f));
            }
            Fact("random." + seed, string.Join(",", parts));
        }
    }

    // ----------------------------------------------------------------- blocks

    private static int blockCount;

    private static void DumpBlocks()
    {
        Type blockEnum = T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        Type blockType = T("DNA.CastleMinerZ.Terrain.BlockType");
        blockCount = Convert.ToInt32(Enum.Parse(blockEnum, "NumberOfBlocks"));
        Fact("block.count", blockCount);
        for (int id = 0; id < blockCount; ++id)
        {
            object block = Call(blockType, "GetType", Enum.ToObject(blockEnum, id));
            string key = "block." + id + ".";
            Fact(key + "blocks_player", (bool)Get(block, "BlockPlayer"));
            Fact(key + "can_be_dug", (bool)Get(block, "CanBeDug"));
            Fact(key + "can_be_touched", (bool)Get(block, "CanBeTouched"));
            Fact(key + "can_build_on", (bool)Get(block, "CanBuildOn"));
            Fact(key + "hardness", (int)Get(block, "Hardness"));
            Fact(key + "restitution", (float)Get(block, "BounceRestitution"));
            Fact(key + "bounces_lasers", (bool)Get(block, "BouncesLasers"));
            Fact(key + "damage_transmission", (float)Get(block, "DamageTransmision"));
            Fact(key + "facing", Convert.ToInt32(Get(block, "Facing")));
            Fact(key + "parent", Convert.ToInt32(Get(block, "ParentBlockType")));
        }
    }

    // ------------------------------------------------------------ enemy types

    private static Array enemyTypes;

    private static void DumpEnemyTypes()
    {
        Type enemyType = T("DNA.CastleMinerZ.AI.EnemyType");
        Type damageType = T("DNA.CastleMinerZ.AI.DamageType");
        // The real table builder. It loads textures and models through Content,
        // which is inert here, and then assigns every tier's gameplay numbers.
        Call(enemyType, "Init");
        enemyTypes = (Array)Get(enemyType, "Types");
        Fact("enemy.count", enemyTypes.Length);
        for (int index = 0; index < enemyTypes.Length; ++index)
        {
            object type = enemyTypes.GetValue(index);
            string key = "enemy." + index + ".";
            string className = type.GetType().Name;
            foreach (FieldInfo field in type.GetType().GetFields(Any))
                if (field.FieldType.Name == "SkeletonClassEnum")
                    className += "." + field.GetValue(type);
            if (Has(type, "IsBoss") && Get(type, "IsBoss") is bool boss && boss) className += ".BOSS";
            Fact(key + "class", className);
            Fact(key + "etype", Convert.ToInt32(Get(type, "EType")));
            Fact(key + "habitat", Convert.ToInt32(Get(type, "FoundIn")));
            Fact(key + "texture", Convert.ToInt32(Get(type, "TextureIndex")));
            Fact(key + "scale", (float)Get(type, "Scale"));
            Fact(key + "health", (float)Get(type, "StartingHealth"));
            Fact(key + "bullet_chance", (float)Get(type, "ChanceOfBulletStrike"));
            Fact(key + "spawn_radius", (int)Get(type, "SpawnRadius"));
            Fact(key + "attack_speed", (float)Get(type, "AttackAnimationSpeed"));
            Fact(key + "die_speed", (float)Get(type, "DieAnimationSpeed"));
            Fact(key + "hit_speed", (float)Get(type, "HitAnimationSpeed"));
            Fact(key + "spawn_speed", (float)Get(type, "SpawnAnimationSpeed"));
            Fact(key + "run_fast", (bool)Get(type, "HasRunFast"));
            Fact(key + "slow_base", (float)Get(type, "BaseSlowSpeed"));
            Fact(key + "slow_random", (float)Get(type, "RandomSlowSpeed"));
            Fact(key + "fast_base", (float)Get(type, "BaseFastSpeed"));
            Fact(key + "run_activation", (float)Get(type, "BaseRunActivationTime"));
            Fact(key + "run_activation_random", (float)Get(type, "RandomRunActivationTime"));
            Fact(key + "fast_jump", (float)Get(type, "FastJumpSpeed"));
            Fact(key + "dig_multiplier", (float)Get(type, "DiggingMultiplier"));
            Fact(key + "hardest_block", (int)Get(type, "HardestBlockThatCanBeDug"));
            foreach (object damage in Enum.GetValues(damageType))
                foreach (bool headshot in new[] { false, true })
                    Fact(key + "damage." + Convert.ToInt32(damage) + "." + Flag(headshot),
                         (float)Call(type, "GetDamageTypeMultiplier", damage, headshot));
        }
    }

    // -------------------------------------------------------------- dig tables

    // InventoryItem.TimeToDig and CreatesWhenDug, executed for every registered
    // item against every diggable block. InGameHUD always evaluates the dig
    // time on BlockType.ParentBlockType (torch and door variants share their
    // parent's time) and asks CreatesWhenDug with the block itself, so that is
    // how both are sampled here. TimeSpan.MaxValue is written as "never".
    private static void DumpDigTables()
    {
        Type item = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        Type blockEnum = T("DNA.CastleMinerZ.Terrain.BlockTypeEnum");
        Type blockType = T("DNA.CastleMinerZ.Terrain.BlockType");
        var catalog = (IDictionary)Get(item, "AllItems");
        int count = Convert.ToInt32(Enum.Parse(blockEnum, "NumberOfBlocks"));
        int digFacts = 0;
        int dropFacts = 0;
        foreach (DictionaryEntry entry in catalog)
        {
            int id = Convert.ToInt32(entry.Key);
            object instance = Call(entry.Value, "CreateItem", 1);
            MethodInfo timeToDig = null;
            MethodInfo createsWhenDug = null;
            for (Type current = instance.GetType(); current != null; current = current.BaseType)
            {
                foreach (MethodInfo method in current.GetMethods(Any | BindingFlags.DeclaredOnly))
                {
                    if (timeToDig == null && method.Name == "TimeToDig") timeToDig = method;
                    if (createsWhenDug == null && method.Name == "CreatesWhenDug") createsWhenDug = method;
                }
            }
            for (int block = 1; block < count; ++block)
            {
                object value = Enum.ToObject(blockEnum, block);
                object description = Call(blockType, "GetType", value);
                if (!(bool)Get(description, "CanBeDug")) continue;
                var time = (TimeSpan)timeToDig.Invoke(instance, new[] { Get(description, "ParentBlockType") });
                string key = id + "." + block;
                facts["dig." + key] = time == TimeSpan.MaxValue ? "never" : Bits((float)time.TotalSeconds);
                ++digFacts;
                if (time == TimeSpan.MaxValue) continue;
                try
                {
                    // 1.9.9.8 adds the block location (used only to find a
                    // door's model, which needs a loaded world: skipped below).
                    object[] arguments = createsWhenDug.GetParameters().Length == 1
                        ? new[] { value }
                        : new[] { value, Activator.CreateInstance(createsWhenDug.GetParameters()[1].ParameterType) };
                    object dropped = createsWhenDug.Invoke(instance, arguments);
                    facts["drop." + key] = dropped == null
                        ? "-1"
                        : Convert.ToInt32(Get(Get(dropped, "ItemClass"), "ID")).ToString(CultureInfo.InvariantCulture);
                    ++dropFacts;
                }
                catch (TargetInvocationException)
                {
                    // Door blocks in 1.9.9.8 read CurrentWorld; no fact, no comparison.
                }
            }
        }
        Fact("dig.facts", digFacts);
        Note("dig tables: " + digFacts + " TimeToDig and " + dropFacts + " CreatesWhenDug results executed");
    }

    // ------------------------------------------------------------------ items

    private static void DumpItems()
    {
        Type item = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        Type ids = T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        // The real registration routine: every class constructor runs and
        // assigns its damage, cooldown, stack and gun data.
        Call(item, "Initalize", (ContentManager)Get(gameInstance, "Content"));
        var catalog = (IDictionary)Get(item, "AllItems");
        Fact("item.count", catalog.Count);
        Type gun = T("DNA.CastleMinerZ.Inventory.GunInventoryItemClass");
        Type laser = TryT("DNA.CastleMinerZ.Inventory.LaserGunInventoryItemClass");
        Type rocket = TryT("DNA.CastleMinerZ.Inventory.RocketLauncherBaseInventoryItemClass");
        Type pump = TryT("DNA.CastleMinerZ.Inventory.PumpShotgunInventoryItemClass");
        Type laserShotgun = TryT("DNA.CastleMinerZ.Inventory.LaserShotgunClass");
        Type grenade = TryT("DNA.CastleMinerZ.Inventory.GrenadeInventoryItemClass");
        foreach (DictionaryEntry entry in catalog)
        {
            int id = Convert.ToInt32(entry.Key);
            object itemClass = entry.Value;
            string key = "item." + id + ".";
            Fact("info.item." + id, entry.Key + " : " + itemClass.GetType().Name);
            Fact(key + "max_stack", (int)Get(itemClass, "MaxStackCount"));
            Fact(key + "enemy_damage", (float)Get(itemClass, "EnemyDamage"));
            Fact(key + "damage_type", Convert.ToInt32(Get(itemClass, "EnemyDamageType")));
            Fact(key + "self_damage", (float)Get(itemClass, "ItemSelfDamagePerUse"));
            Fact(key + "cooldown", (float)((TimeSpan)Get(itemClass, "CoolDownTime")).TotalSeconds);
            Fact(key + "melee", (bool)Get(itemClass, "IsMeleeWeapon"));
            Fact(key + "pickup_timeout", (float)Get(itemClass, "PickupTimeoutLength"));
            if (grenade != null && grenade.IsInstanceOfType(itemClass))
                Fact(key + "grenade_type", Convert.ToInt32(Get(itemClass, "GrenadeType")));
            if (!gun.IsInstanceOfType(itemClass)) continue;
            Optional(key + "gun.clip", () => (int)Get(itemClass, "ClipCapacity"));
            Optional(key + "gun.rounds_per_reload", () => (int)Get(itemClass, "RoundsPerReload"));
            Optional(key + "gun.automatic", () => (bool)Get(itemClass, "Automatic"));
            Optional(key + "gun.inaccuracy", () => Get(itemClass, "Innaccuracy"));
            Optional(key + "gun.recoil", () => (float)Get(Get(itemClass, "Recoil"), "Radians"));
            Optional(key + "gun.drop_compensation", () => (bool)Get(itemClass, "NeedsDropCompensation"));
            Optional(key + "gun.velocity", () => (float)Get(itemClass, "Velocity"));
            Optional(key + "gun.flight_time", () => (float)Get(itemClass, "FlightTime"));
            Optional(key + "gun.reload", () => (float)((TimeSpan)Get(itemClass, "ReloadTime")).TotalSeconds);
            object ammo = Get(itemClass, "AmmoType");
            Fact(key + "gun.ammo", ammo == null ? -1 : Convert.ToInt32(Get(ammo, "ID")));
            Fact(key + "gun.laser", laser != null && laser.IsInstanceOfType(itemClass));
            Fact(key + "gun.rocket", rocket != null && rocket.IsInstanceOfType(itemClass));
            // InGameHUD.Shoot's own routing rule for ShotgunShotMessage.
            Fact(key + "gun.shotgun_message",
                 (pump != null && pump.IsInstanceOfType(itemClass)) ||
                 (laserShotgun != null && laserShotgun.IsInstanceOfType(itemClass)));
        }
    }
}
