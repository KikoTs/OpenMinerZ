using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Microsoft.Xna.Framework;

// Water and lava, as the SHIPPED assembly has them.
//
// The original carries a complete water world: BlockTerrain.IsWaterWorld,
// WaterLevel, a WaterPlane entity, a reflection camera, swim physics, oxygen
// and drowning. Whether a retail game ever HAS water is therefore not a
// question about what that code does, but about whether anything switches it
// on. Two things settle it, and both are answered from the user's own
// unmodified assembly rather than from a decompilation:
//
//   1. every instruction in the assembly that writes IsWaterWorld, and every
//      one that constructs a WaterPlane or the CostalBiome that authors a sea
//      level, found by walking the IL of every method body;
//   2. the original Player's own InWater / Underwater / PercentSubmergedWater /
//      InLava / UnderLava / PercentSubmergedLava properties, EXECUTED at a
//      column of feet heights against a terrain in the state (1) proves the
//      running game is in.
//
// The native client's facts for the same keys are its default, retail-parity
// behaviour; Test-SemanticsParity.ps1 compares them.
internal static partial class Program
{
    // Shared with tests/test_semantics_parity.cpp (environment_sample_heights).
    private static readonly float[] environmentHeights =
    {
        64f, 2f, 1.5f, 1f, 0.5f, 0f, -0.5f, -10f, -59f, -60f, -60.5f, -61f, -62f, -63.5f
    };

    private struct Instruction
    {
        public OpCode Code;
        public int Operand;
        public float Single;
    }

    private static OpCode[] singleByteOpcodes;
    private static OpCode[] twoByteOpcodes;

    private static List<Instruction> Decode(MethodBase method)
    {
        var result = new List<Instruction>();
        MethodBody body;
        try { body = method.GetMethodBody(); }
        catch (Exception) { return result; }
        byte[] il = body == null ? null : body.GetILAsByteArray();
        if (il == null) return result;
        if (singleByteOpcodes == null)
        {
            singleByteOpcodes = new OpCode[256];
            twoByteOpcodes = new OpCode[256];
            foreach (FieldInfo field in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                var code = (OpCode)field.GetValue(null);
                if (code.Size == 1) singleByteOpcodes[code.Value & 0xFF] = code;
                else twoByteOpcodes[code.Value & 0xFF] = code;
            }
        }
        int offset = 0;
        while (offset < il.Length)
        {
            OpCode code = il[offset] == 0xFE ? twoByteOpcodes[il[offset + 1]] : singleByteOpcodes[il[offset]];
            // An opcode this runtime does not name would desynchronise the
            // walk; a body that cannot be decoded is one the scan did not see.
            if (code.Size == 0)
                throw new InvalidOperationException("Undecodable IL in " + method.DeclaringType + "." + method.Name);
            offset += code.Size;
            int size;
            switch (code.OperandType)
            {
                case OperandType.InlineNone: size = 0; break;
                case OperandType.ShortInlineBrTarget:
                case OperandType.ShortInlineI:
                case OperandType.ShortInlineVar: size = 1; break;
                case OperandType.InlineVar: size = 2; break;
                case OperandType.InlineI8:
                case OperandType.InlineR: size = 8; break;
                case OperandType.InlineSwitch: size = 4 + 4 * BitConverter.ToInt32(il, offset); break;
                default: size = 4; break;
            }
            var instruction = new Instruction { Code = code };
            if (size == 4 && code.OperandType != OperandType.InlineSwitch)
            {
                instruction.Operand = BitConverter.ToInt32(il, offset);
                if (code.OperandType == OperandType.ShortInlineR)
                    instruction.Single = BitConverter.ToSingle(il, offset);
            }
            result.Add(instruction);
            offset += size;
        }
        return result;
    }

    private static IEnumerable<MethodBase> EveryMethod()
    {
        Type[] types;
        try { types = game.GetTypes(); }
        catch (ReflectionTypeLoadException partial) { types = partial.Types.Where(type => type != null).ToArray(); }
        foreach (Type type in types)
        {
            foreach (MethodInfo method in type.GetMethods(Any | BindingFlags.DeclaredOnly)) yield return method;
            foreach (ConstructorInfo constructor in type.GetConstructors(Any | BindingFlags.DeclaredOnly))
                yield return constructor;
        }
    }

    private static MemberInfo ResolveToken(MethodBase method, int token)
    {
        try
        {
            Type[] typeArguments = method.DeclaringType != null && method.DeclaringType.IsGenericType
                ? method.DeclaringType.GetGenericArguments() : null;
            Type[] methodArguments = method is MethodInfo && method.IsGenericMethod
                ? method.GetGenericArguments() : null;
            return method.Module.ResolveMember(token, typeArguments, methodArguments);
        }
        catch (Exception) { return null; }
    }

    private static void DumpEnvironment()
    {
        Type terrainType = T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        FieldInfo waterWorld = Field(terrainType, "IsWaterWorld");
        FieldInfo waterLevel = Field(terrainType, "WaterLevel");
        Type waterPlane = T("DNA.CastleMinerZ.Terrain.WaterPlane");
        Type costal = TryT("DNA.CastleMinerZ.Terrain.WorldBuilders.CostalBiome");

        int bodies = 0, writes = 0, writesNotFalse = 0, addressTaken = 0, reads = 0;
        int planes = 0, costalBiomes = 0;
        var levels = new SortedSet<string>(StringComparer.Ordinal);
        foreach (MethodBase method in EveryMethod())
        {
            List<Instruction> code = Decode(method);
            if (code.Count == 0) continue;
            ++bodies;
            for (int index = 0; index < code.Count; ++index)
            {
                OpCode op = code[index].Code;
                if (op == OpCodes.Stfld || op == OpCodes.Ldfld || op == OpCodes.Ldflda)
                {
                    MemberInfo member = ResolveToken(method, code[index].Operand);
                    if (member == null) continue;
                    if (member.MetadataToken == waterWorld.MetadataToken && member.Module == waterWorld.Module)
                    {
                        if (op == OpCodes.Ldfld) ++reads;
                        else if (op == OpCodes.Ldflda) ++addressTaken;
                        else
                        {
                            ++writes;
                            // `x.IsWaterWorld = false` compiles to ldc.i4.0; stfld.
                            if (index == 0 || code[index - 1].Code != OpCodes.Ldc_I4_0) ++writesNotFalse;
                        }
                    }
                    else if (op == OpCodes.Stfld && member.MetadataToken == waterLevel.MetadataToken &&
                             member.Module == waterLevel.Module)
                    {
                        string value = index > 0 && code[index - 1].Code == OpCodes.Ldc_R4
                            ? code[index - 1].Single.ToString("R", CultureInfo.InvariantCulture) : "computed";
                        levels.Add(method.DeclaringType.Name + "." + method.Name + "=" + value);
                    }
                }
                else if (op == OpCodes.Newobj)
                {
                    MemberInfo member = ResolveToken(method, code[index].Operand);
                    if (member == null) continue;
                    if (member.DeclaringType == waterPlane) ++planes;
                    if (costal != null && member.DeclaringType == costal) ++costalBiomes;
                }
            }
        }
        Fact("stock.method_bodies_scanned", bodies);
        Fact("stock.water_world_reads", reads);
        Fact("stock.water_world_writes", writes);
        Fact("stock.water_world_writes_not_false", writesNotFalse);
        Fact("stock.water_world_address_taken", addressTaken);
        Fact("stock.water_plane_constructions", planes);
        Fact("stock.costal_biome_constructions", costalBiomes);
        Fact("stock.water_level_writes", string.Join(" | ", levels));
        bool everWater = writesNotFalse != 0 || addressTaken != 0 || planes != 0;
        // The scan has to have SEEN the code it clears: the flag is read by the
        // player, HUD and renderer, and written by the terrain itself.
        Check(bodies > 2000 && reads >= 5 && writes >= 1,
              "the IL scan covered the assembly (" + bodies + " bodies, " + reads + " reads and " + writes +
              " writes of IsWaterWorld)");
        Check(!everWater,
              "no instruction in the shipped assembly enables a water world or constructs a WaterPlane");
        // Compared with the native client's default.
        Fact("environment.water_world", everWater);

        // The original Player, evaluated against the terrain a running game has:
        // IsWaterWorld false (all it is ever assigned) and the water level the
        // world builder writes on every chunk it generates.
        EnsureTerrain();
        Set(terrain, "IsWaterWorld", everWater);
        Set(terrain, "WaterLevel", 1.5f);
        object player = MakeEntity(T("DNA.CastleMinerZ.Player"));
        for (int sample = 0; sample < environmentHeights.Length; ++sample)
        {
            float feet = environmentHeights[sample];
            Set(player, "LocalPosition", new Vector3(3.5f, feet, -2.5f));
            string key = "environment." + sample + ".";
            Fact(key + "feet_y", feet);
            Fact(key + "in_water", (bool)Get(player, "InWater"));
            Fact(key + "underwater", (bool)Get(player, "Underwater"));
            Fact(key + "water_submersion", (float)Get(player, "PercentSubmergedWater"));
            Fact(key + "in_lava", (bool)Get(player, "InLava"));
            Fact(key + "under_lava", (bool)Get(player, "UnderLava"));
            Fact(key + "lava_submersion", (float)Get(player, "PercentSubmergedLava"));
        }

        // The water code is real, only unreachable: with the flag forced on,
        // the same Player swims and drowns. This is what the native client
        // keeps as its default-off "Lagoon water" enhancement.
        Set(terrain, "IsWaterWorld", true);
        Set(player, "LocalPosition", new Vector3(3.5f, -10f, -2.5f));
        bool swims = (bool)Get(player, "InWater") && (bool)Get(player, "Underwater") &&
                     (float)Get(player, "PercentSubmergedWater") == 1f;
        Set(player, "LocalPosition", new Vector3(3.5f, 1f, -2.5f));
        bool wades = (bool)Get(player, "InWater") && !(bool)Get(player, "Underwater") &&
                     (float)Get(player, "PercentSubmergedWater") == 0.5f;
        Set(terrain, "IsWaterWorld", false);
        Set(player, "LocalPosition", new Vector3(3.5f, -10f, -2.5f));
        bool dry = !(bool)Get(player, "InWater") && !(bool)Get(player, "Underwater") &&
                   (float)Get(player, "PercentSubmergedWater") == 0f;
        Fact("stock.water_code_is_gated_by_the_flag", swims && wades && dry);
        Check(swims && wades && dry,
              "the original's swim code runs only when IsWaterWorld is forced on, and is inert as shipped");
    }

    // ---------------------------------------------------------------- landing

    // Where MakeAboveGround puts a relocated player. BlockTerrain's
    // FindSafeStartLocation (teleport to a player, Endurance respawn beside a
    // teammate) and FindTopmostGroundLocation (every return to the start, and
    // the menu's Surface) are EXECUTED over a battery of columns; the native
    // client's world/stock_landing.hpp answers the same keys.
    //
    // Shared with tests/test_semantics_parity.cpp: landing_block(),
    // landing_columns, landing_guesses, landing_z.
    private const int LandingColumns = 24;
    private const int LandingZ = 3;
    private static readonly int[] landingGuesses = { 0, 1, 5, 20, 30, 31, 40, 45, 55, 64, 70, 90, 100, 126, 127 };

    private static int[] LandingColumn(int column)
    {
        const int rock = 6, surfaceLava = 12, deepLava = 13;
        var cells = new int[128];
        uint state = 0x9E3779B9u ^ unchecked((uint)column * 0x85EBCA6Bu);
        uint threshold = (uint)(40 + (column % 4) * 40);
        for (int index = 0; index < 128; ++index)
        {
            state = unchecked(state * 1664525u + 1013904223u);
            int block;
            switch (column)
            {
                case 0: block = index <= 63 ? rock : 0; break;
                case 1: block = index <= 70 && index != 40 ? rock : 0; break;
                case 2: block = index <= 80 && index != 30 && index != 31 ? rock : 0; break;
                case 3: block = index <= 50 || (index >= 61 && index <= 70) ? rock : 0; break;
                case 4: block = 0; break;
                case 5: block = rock; break;
                case 6: block = index == 100 || index == 101 ? 0 : rock; break;
                case 7: block = index <= 9 ? rock : index <= 19 ? deepLava : index == 20 ? surfaceLava : 0; break;
                default: block = (state >> 24) < threshold ? rock : 0; break;
            }
            cells[index] = block;
        }
        return cells;
    }

    private static void DumpLanding()
    {
        EnsureTerrain();
        // The state a running game is in (see DumpEnvironment): no water world.
        Set(terrain, "IsWaterWorld", false);
        Set(terrain, "WaterLevel", 1.5f);
        bool everInColumn = true;
        for (int column = 0; column < LandingColumns; ++column)
        {
            int[] cells = LandingColumn(column);
            for (int index = 0; index < 128; ++index) SetBlock(column, index - 64, LandingZ, cells[index]);
        }
        for (int column = 0; column < LandingColumns; ++column)
        {
            var top = (Vector3)Call(terrain, "FindTopmostGroundLocation",
                                    new Vector3(column + 0.3f, 10.3f, LandingZ + 0.7f));
            bool topInColumn = top.X == column + 0.5f && top.Z == LandingZ + 0.5f;
            everInColumn &= topInColumn;
            Fact("landing.top." + column, top.Y);
            Fact("landing.top_column." + column, topInColumn);
            foreach (int guess in landingGuesses)
            {
                var safe = (Vector3)Call(terrain, "FindSafeStartLocation",
                                         new Vector3(column + 0.3f, guess - 64 + 0.3f, LandingZ + 0.7f));
                bool inColumn = safe.X == column + 0.5f && safe.Z == LandingZ + 0.5f;
                everInColumn &= inColumn;
                Fact("landing.safe." + column + "." + guess, safe.Y);
                Fact("landing.safe_column." + column + "." + guess, inColumn);
            }
        }
        Check(everInColumn,
              "FindSafeStartLocation and FindTopmostGroundLocation never left the target's own column");
    }

    // How often and how hard the ambient spawner runs: the two functions every
    // EnemyManager spawn decision starts from. Each player spawns their own
    // enemies and announces them, so a different rule here is a different
    // number of zombies for an OpenMinerZ player than for the Steam player
    // beside them.
    public static readonly float[] SpawnDistances = { 0f, 1000f, 3450f, 4312.5f, 5000f, 8000f };

    // percent midnight, distance (already including Endurance's day bonus), player height
    public static readonly float[][] MidnightSamples =
    {
        new[] { 0f, 0f, 10f }, new[] { 0.5f, 0f, 10f }, new[] { 0.6f, 0f, 10f }, new[] { 0.9f, 0f, 10f },
        new[] { 1f, 0f, 10f }, new[] { 0.3f, 2500f, 10f }, new[] { 0.6f, 2500f, 10f }, new[] { 1f, 2500f, 10f },
        new[] { 0.1f, 5000f, 10f }, new[] { 0.5f, 5000f, 10f }, new[] { 0.95f, 9000f, 10f },
        new[] { 0f, 0f, -39.9f }, new[] { 0f, 0f, -40.1f }, new[] { 1f, 4000f, -60f },
    };

    private static void DumpSpawnRules()
    {
        EnsureEnemyManager();
        EnsureTerrain();
        Type gameType = gameInstance.GetType();
        FieldInfo mode = Field(gameType, "GameMode");
        FieldInfo difficulty = Field(gameType, "Difficulty");
        object oldMode = mode.GetValue(gameInstance), oldDifficulty = difficulty.GetValue(gameInstance);
        try
        {
            foreach (string modeName in new[] { "Endurance", "Survival", "DragonEndurance" })
                foreach (string difficultyName in new[] { "EASY", "HARD", "NOENEMIES", "HARDCORE" })
                {
                    mode.SetValue(gameInstance, Enum.Parse(mode.FieldType, modeName));
                    difficulty.SetValue(gameInstance, Enum.Parse(difficulty.FieldType, difficultyName));
                    foreach (float distance in SpawnDistances)
                        Fact("spawn.min_time." + modeName + "." + difficultyName + "." +
                                 distance.ToString(CultureInfo.InvariantCulture),
                             (float)Call(enemyManager, "GetMinEnemySpawnTime", distance));
                }
        }
        finally
        {
            mode.SetValue(gameInstance, oldMode);
            difficulty.SetValue(gameInstance, oldDifficulty);
        }
        for (int index = 0; index < MidnightSamples.Length; ++index)
        {
            float[] sample = MidnightSamples[index];
            Set(terrain, "PercentMidnight", sample[0]);
            Set(enemyManager, "ZombieFestIsOn", false);
            Fact("spawn.midnight." + index, (float)Call(enemyManager, "CalculateMidnight", sample[1], sample[2]));
            // The midnight frenzy pins it to 1, except below y -40.
            Set(enemyManager, "ZombieFestIsOn", true);
            Fact("spawn.midnight_fest." + index, (float)Call(enemyManager, "CalculateMidnight", sample[1], sample[2]));
        }
        Set(enemyManager, "ZombieFestIsOn", false);
    }
}
