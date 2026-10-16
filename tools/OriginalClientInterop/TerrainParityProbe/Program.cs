using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;

// Differential probe for the GENERATED world. Retail never streams base terrain:
// BlockTerrain.DoThreadedComputeBlocks runs the local WorldBuilder for the seed
// in WorldInfoMessage and only then applies the host's delta list, so every
// block the native generator places differently is a permanent disagreement
// between an OpenMinerZ peer and a stock peer.
//
// This loads the user's OWNED, unmodified CastleMinerZ.exe by path into an
// isolated x86 process and executes the original
// CastleMinerZBuilder.BuildWorldChunk IL -- every biome, depositer and the tree
// pass -- into a real BlockTerrain block array, then writes the resulting block
// ids. It never runs a Game constructor, opens a window or graphics device,
// enters Steam or touches the network, and it copies nothing out of the install.
//
// Usage:
//   TerrainParityProbe <gameDirectory> <requestFile> <outputFile>
//
// Request file (text, '#' comments):
//   world <seed> <gameMode>     start a NEW builder (Modern's cave/hell passes
//                               carry counters across chunks, so order matters)
//   <chunkX> <chunkZ>           build that chunk with the current builder
//
// Output file (binary): 32768 bytes per requested chunk, in request order, one
// block id per cell at index y + 128 * (x + 16 * z), exactly the layout of the
// original block array, after the same NumberOfBlocks -> Empty replacement
// DoThreadedComputeBlocks applies once the builder returns.
internal static class Program
{
    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static |
                                     BindingFlags.Public | BindingFlags.NonPublic;
    private const int ChunkCells = 16 * 128 * 16;
    private static string gameDirectory;

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

    private static FieldInfo Field(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, Any | BindingFlags.DeclaredOnly);
            if (field != null) return field;
        }
        return null;
    }

    private static FieldInfo RequiredField(Type type, string name)
    {
        FieldInfo field = Field(type, name);
        if (field == null) throw new MissingFieldException(type.FullName, name);
        return field;
    }

    private sealed class Original
    {
        public Assembly Game;
        public Type Terrain, Builder, WorldInfo, IntVector3, GameType;
        public MethodInfo BuildWorldChunk;
        public ConstructorInfo NewBuilder, NewIntVector3;
        public object TerrainInstance;
        public int[] Blocks;
        public object GameInstance;

        public object NewWorldInfo(int seed)
        {
            // WorldInfo's constructor only reaches for localized display names;
            // the builder reads nothing but the seed (and, in Modern, the two
            // hell-boss counters, which a new world starts at zero).
            object info = FormatterServices.GetUninitializedObject(WorldInfo);
            RequiredField(WorldInfo, "_seed").SetValue(info, seed);
            FieldInfo version = Field(WorldInfo, "_terrainVersion");
            if (version != null) version.SetValue(info, Enum.ToObject(version.FieldType, 1));
            return info;
        }

        public void SetWorld(object worldInfo, int gameMode)
        {
            if (GameInstance == null) return;
            FieldInfo mode = Field(GameType, "GameMode");
            if (mode != null) mode.SetValue(GameInstance, Enum.ToObject(mode.FieldType, gameMode));
            FieldInfo current = Field(GameType, "CurrentWorld");
            if (current != null) current.SetValue(GameInstance, worldInfo);
        }
    }

    private static Original Load()
    {
        var original = new Original();
        original.Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
        original.Terrain = original.Game.GetType("DNA.CastleMinerZ.Terrain.BlockTerrain", true);
        original.Builder = original.Game.GetType(
            "DNA.CastleMinerZ.Terrain.WorldBuilders.CastleMinerZBuilder", true);
        original.WorldInfo = original.Game.GetType("DNA.CastleMinerZ.WorldInfo", true);
        original.GameType = original.Game.GetType("DNA.CastleMinerZ.CastleMinerZGame", true);
        original.NewBuilder = original.Builder.GetConstructor(Any, null, new[] { original.WorldInfo }, null);
        original.BuildWorldChunk = original.Builder.GetMethod("BuildWorldChunk", Any);
        original.IntVector3 = original.BuildWorldChunk.GetParameters()[1].ParameterType;
        original.NewIntVector3 = original.IntVector3.GetConstructor(
            new[] { typeof(int), typeof(int), typeof(int) });

        // A BlockTerrain with no graphics device behind it: the builder only
        // touches _blocks, _worldMin (through MakeIndexFromWorldIndexVector),
        // _resetRequested and WaterLevel. The array is the original 384x128x384
        // window so every index computation is the shipped one.
        original.TerrainInstance = FormatterServices.GetUninitializedObject(original.Terrain);
        original.Blocks = new int[384 * 128 * 384];
        RequiredField(original.Terrain, "_blocks").SetValue(original.TerrainInstance, original.Blocks);
        RequiredField(original.Terrain, "_resetRequested").SetValue(original.TerrainInstance, false);

        // Modern's cave and hell passes read CastleMinerZGame.Instance.GameMode
        // and .CurrentWorld. The singleton is populated without running the
        // Game constructor (window, device, Steam, audio).
        FieldInfo instance = Field(original.GameType, "Instance");
        if (instance != null)
        {
            original.GameInstance = instance.GetValue(null);
            if (original.GameInstance == null)
            {
                original.GameInstance = FormatterServices.GetUninitializedObject(original.GameType);
                instance.SetValue(null, original.GameInstance);
            }
        }
        return original;
    }

    private static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: TerrainParityProbe <gameDirectory> <requestFile> <outputFile>");
            return 2;
        }
        gameDirectory = Path.GetFullPath(args[0]);
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            return Run(args[1], args[2]);
        }
        catch (Exception error)
        {
            Exception inner = error;
            while (inner is TargetInvocationException && inner.InnerException != null)
                inner = inner.InnerException;
            Console.Error.WriteLine("FAIL " + inner);
            return 1;
        }
    }

    private static int Run(string requestFile, string outputFile)
    {
        Original original = Load();
        FieldInfo worldMin = RequiredField(original.Terrain, "_worldMin");
        // FillRegion(..., NumberOfBlocks): 45 in Classic, 94 in Modern. Taken from
        // the loaded generation and packed by the original Block.SetType.
        Type blockEnum = original.Game.GetType("DNA.CastleMinerZ.Terrain.BlockTypeEnum", true);
        object numberOfBlocks = Enum.Parse(blockEnum, "NumberOfBlocks");
        int uninitializedType = Convert.ToInt32(numberOfBlocks, CultureInfo.InvariantCulture);
        Type block = original.Game.GetType("DNA.CastleMinerZ.Terrain.Block", true);
        int uninitialized = (int)block.GetMethod("SetType", Any, null, new[] { typeof(int), blockEnum }, null)
            .Invoke(null, new object[] { 0, numberOfBlocks });
        object builder = null;
        var buffer = new byte[ChunkCells];
        int chunks = 0;
        int worlds = 0;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputFile)));
        using (var output = new FileStream(outputFile, FileMode.Create, FileAccess.Write))
        {
            foreach (string raw in File.ReadAllLines(requestFile))
            {
                string line = raw;
                int comment = line.IndexOf('#');
                if (comment >= 0) line = line.Substring(0, comment);
                string[] parts = line.Split(new[] { ' ', '\t', ',' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) continue;
                if (parts[0] == "world")
                {
                    int seed = int.Parse(parts[1], CultureInfo.InvariantCulture);
                    int mode = parts.Length > 2 ? int.Parse(parts[2], CultureInfo.InvariantCulture) : 0;
                    object info = original.NewWorldInfo(seed);
                    original.SetWorld(info, mode);
                    builder = original.NewBuilder.Invoke(new[] { info });
                    ++worlds;
                    continue;
                }
                if (builder == null) throw new InvalidDataException("chunk before any 'world' line");
                int chunkX = int.Parse(parts[0], CultureInfo.InvariantCulture);
                int chunkZ = int.Parse(parts[1], CultureInfo.InvariantCulture);
                object min = original.NewIntVector3.Invoke(new object[] { chunkX * 16, -64, chunkZ * 16 });
                // The chunk sits at the origin of the window, exactly as a chunk
                // at _worldMin does in the game.
                worldMin.SetValue(original.TerrainInstance, min);
                int[] blocks = original.Blocks;
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x)
                    {
                        int index = x * 128 + z * 128 * 384;
                        for (int y = 0; y < 128; ++y) blocks[index + y] = uninitialized;
                    }
                original.BuildWorldChunk.Invoke(builder, new[] { original.TerrainInstance, min });
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x)
                    {
                        int index = x * 128 + z * 128 * 384;
                        int target = 128 * (x + 16 * z);
                        for (int y = 0; y < 128; ++y)
                        {
                            // Block.GetTypeIndex, then the game's own
                            // ReplaceRegion(NumberOfBlocks -> Empty).
                            uint type = (uint)(blocks[index + y] & 0x7FFFF000) >> 12;
                            buffer[target + y] = type == (uint)uninitializedType ? (byte)0 : (byte)type;
                        }
                    }
                output.Write(buffer, 0, buffer.Length);
                ++chunks;
            }
        }
        Console.WriteLine("OK " + worlds.ToString(CultureInfo.InvariantCulture) + " worlds " +
                          chunks.ToString(CultureInfo.InvariantCulture) + " chunks from " +
                          original.Game.GetName().Version);
        return 0;
    }
}
