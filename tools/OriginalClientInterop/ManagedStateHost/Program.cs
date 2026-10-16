using Microsoft.Win32.SafeHandles;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;

namespace OpenMinerZ.OriginalStateHost
{
    internal static class Program
    {
        private const string EndpointName = "OpenMinerZOriginalState.endpoint.json";
        [STAThread]
        private static int Main(string[] args)
        {
            string directory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            bool probeOnly;
            string gameName = ParseGameName(args, out probeOnly);
            if (directory == null || gameName == null)
                return 2;
            string gamePath = Path.GetFullPath(Path.Combine(directory, gameName));
            if (!String.Equals(Path.GetDirectoryName(gamePath), directory,
                               StringComparison.OrdinalIgnoreCase) ||
                !File.Exists(gamePath))
                return 3;

            Directory.SetCurrentDirectory(directory);
            Environment.SetEnvironmentVariable("SteamAppId", "253430");
            Environment.SetEnvironmentVariable("SteamGameId", "253430");
            try
            {
                Assembly game = Assembly.LoadFrom(gamePath);
                if (probeOnly)
                {
                    try
                    {
                        Console.Out.WriteLine(ContractProbe.Verify(game, gamePath));
                        return 0;
                    }
                    catch (InvalidOperationException exception)
                    {
                        Console.Out.WriteLine("{\"schema\":\"openminerz.original-contract.v1\"," +
                                              "\"ok\":false,\"code\":\"" + exception.Message + "\"}");
                        return 9;
                    }
                    catch (FileNotFoundException exception)
                    {
                        string missing = "unknown";
                        try { missing = new AssemblyName(exception.FileName).Name; } catch { }
                        Console.Out.WriteLine("{\"schema\":\"openminerz.original-contract.v1\"," +
                                              "\"ok\":false,\"code\":\"MissingAssembly\"," +
                                              "\"assembly\":\"" + missing + "\"}");
                        return 10;
                    }
                }
                if (game.EntryPoint == null)
                    return 4;
                using (StateServer server = new StateServer(directory, game))
                {
                    if (!server.Start())
                        return 5;
                    object[] parameters = game.EntryPoint.GetParameters().Length == 0
                        ? null
                        : new object[] { new string[0] };
                    object result = game.EntryPoint.Invoke(null, parameters);
                    return result is int ? (int)result : 0;
                }
            }
            catch (TargetInvocationException exception)
            {
                if (probeOnly)
                    Console.Out.WriteLine("{\"schema\":\"openminerz.original-contract.v1\"," +
                                          "\"ok\":false,\"code\":\"TargetInvocationException\"}");
                return exception.InnerException == null ? 6 : 7;
            }
            catch (Exception exception)
            {
                if (probeOnly)
                    Console.Out.WriteLine("{\"schema\":\"openminerz.original-contract.v1\"," +
                                          "\"ok\":false,\"code\":\"" +
                                          exception.GetType().Name + "\"}");
                return 8;
            }
        }

        private static string ParseGameName(string[] args, out bool probeOnly)
        {
            probeOnly = false;
            if (args.Length == 0)
                return "CastleMinerZ.exe";
            if (args.Length != 2 || (args[0] != "--game" && args[0] != "--probe-game"))
                return null;
            probeOnly = args[0] == "--probe-game";
            string name = args[1];
            if (name.Length < 5 || name.Length > 96 ||
                !name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(name) != name)
                return null;
            foreach (char character in name)
                if (!Char.IsLetterOrDigit(character) && character != '.' &&
                    character != '_' && character != '-' && character != ' ')
                    return null;
            return name;
        }
    }

    internal static class ContractProbe
    {
        private const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic |
                                           BindingFlags.Instance | BindingFlags.Static |
                                           BindingFlags.DeclaredOnly;

        internal static string Verify(Assembly assembly, string gamePath)
        {
            Type game = RequireType(assembly, "DNA.CastleMinerZ.CastleMinerZGame");
            RequireMembers(game, "Instance", "LocalPlayer", "CurrentNetworkSession", "_terrain",
                           "CurrentWorld", "GameScreen", "MyNetworkGamer");
            Type player = RequireType(assembly, "DNA.CastleMinerZ.Player");
            RequireMembers(player, "LocalPosition", "LocalRotation", "TorsoPitch", "PlayerInventory", "Dead");
            Type terrain = RequireType(assembly, "DNA.CastleMinerZ.Terrain.BlockTerrain");
            RequireMembers(terrain, "_blocks", "_worldMin");
            Type inventory = RequireType(assembly, "DNA.CastleMinerZ.Inventory.PlayerInventory");
            RequireMembers(inventory, "Inventory", "SelectedInventoryIndex");
            Type item = RequireType(assembly, "DNA.CastleMinerZ.Inventory.InventoryItem");
            RequireMembers(item, "ItemClass", "StackCount", "ItemHealthLevel");
            RequireMembers(RequireType(assembly, "DNA.CastleMinerZ.Inventory.GunInventoryItem"), "RoundsInClip");
            RequireMembers(RequireType(assembly, "DNA.CastleMinerZ.Inventory.GPSItem"),
                           "PointToLocation", "_GPSname");
            RequireMembers(RequireType(assembly, "DNA.CastleMinerZ.AI.EnemyManager"),
                           "Instance", "_enemies", "DragonIsActive");
            RequireMembers(RequireType(assembly, "DNA.CastleMinerZ.PickupManager"), "Instance", "Pickups");

            Type blocks = RequireType(assembly, "DNA.CastleMinerZ.Terrain.BlockTypeEnum");
            bool modern = blocks.GetField("LuckyLootBlock", Flags) != null;
            if (modern)
            {
                RequireConstant(blocks, "LootBlock", 69);
                RequireConstant(blocks, "LuckyLootBlock", 70);
                Type manager = RequireType(assembly, "DNA.CastleMinerZ.Inventory.InventoryTrayManager");
                RequireMembers(manager, "Trays", "InventoryTray", "InventoryTray2", "_currentTrayIndex");
                RequireMembers(inventory, "TrayManager");
                RequireMembers(RequireType(assembly, "DNA.CastleMinerZ.Inventory.BlockInventoryItem"),
                               "BlockTypeID", "PointToLocation", "CustomBlockName");
            }
            else
            {
                RequireMembers(inventory, "InventoryTray");
            }

            string hash;
            using (SHA256 algorithm = SHA256.Create())
            using (FileStream stream = File.OpenRead(gamePath))
                hash = Hex(algorithm.ComputeHash(stream));
            string version = assembly.GetName().Version.ToString();
            return "{\"schema\":\"openminerz.original-contract.v1\",\"ok\":true," +
                   "\"profile\":\"" + (modern ? "modern" : "classic") + "\"," +
                   "\"protocol\":" + (modern ? "4" : "3") + ",\"assembly_version\":\"" +
                   version + "\",\"sha256\":\"" + hash + "\"}";
        }

        private static Type RequireType(Assembly assembly, string name)
        {
            Type type = assembly.GetType(name, false);
            if (type == null) throw new InvalidOperationException("ContractTypeMissing");
            return type;
        }

        private static void RequireMembers(Type type, params string[] names)
        {
            foreach (string name in names)
            {
                bool found = false;
                for (Type current = type; current != null && !found; current = current.BaseType)
                    found = current.GetField(name, Flags) != null || current.GetProperty(name, Flags) != null;
                if (!found) throw new InvalidOperationException("ContractMemberMissing");
            }
        }

        private static void RequireConstant(Type type, string name, int expected)
        {
            FieldInfo field = type.GetField(name, Flags);
            if (field == null || Convert.ToInt32(field.GetRawConstantValue(), CultureInfo.InvariantCulture) != expected)
                throw new InvalidOperationException("ContractConstantMismatch");
        }

        private static string Hex(byte[] bytes)
        {
            StringBuilder result = new StringBuilder(bytes.Length * 2);
            foreach (byte value in bytes) result.Append(value.ToString("x2", CultureInfo.InvariantCulture));
            return result.ToString();
        }
    }

    internal sealed class StateServer : IDisposable
    {
        private readonly string directory;
        private readonly Assembly game;
        private readonly string pipeName;
        private readonly string pipePath;
        private readonly string endpointPath;
        private readonly ManualResetEvent started = new ManualResetEvent(false);
        private readonly Thread thread;
        private volatile bool stopping;
        private volatile bool failed;

        internal StateServer(string directory, Assembly game)
        {
            this.directory = directory;
            this.game = game;
            pipeName = "OpenMinerZ.OriginalState." + System.Diagnostics.Process.GetCurrentProcess().Id;
            pipePath = @"\\.\pipe\" + pipeName;
            endpointPath = Path.Combine(directory, "OpenMinerZOriginalState.endpoint.json");
            thread = new Thread(Run) { IsBackground = true, Name = "OpenMinerZ original-state pipe" };
        }

        internal bool Start()
        {
            thread.Start();
            if (!started.WaitOne(5000) || failed)
                return false;
            string json = "{\"schema\":\"openminerz.original-state-endpoint.v1\",\"pipe\":\"" +
                          pipeName + "\",\"host_pid\":" +
                          System.Diagnostics.Process.GetCurrentProcess().Id.ToString(CultureInfo.InvariantCulture) +
                          "}\n";
            WriteAtomic(endpointPath, json);
            return true;
        }

        public void Dispose()
        {
            stopping = true;
            Wake();
            if (thread.IsAlive)
                thread.Join(5000);
            try { File.Delete(endpointPath); } catch { }
            started.Dispose();
        }

        private void Run()
        {
            bool first = true;
            while (!stopping)
            {
                IntPtr descriptor = IntPtr.Zero;
                SafeFileHandle handle = null;
                try
                {
                    string sid = WindowsIdentity.GetCurrent().User.Value;
                    if (!Native.ConvertStringSecurityDescriptorToSecurityDescriptor(
                            "D:P(A;;GA;;;SY)(A;;GA;;;" + sid + ")", 1, out descriptor, IntPtr.Zero))
                        throw new InvalidOperationException();
                    Native.SecurityAttributes security = new Native.SecurityAttributes
                    {
                        Length = Marshal.SizeOf(typeof(Native.SecurityAttributes)),
                        SecurityDescriptor = descriptor,
                        InheritHandle = 0
                    };
                    handle = Native.CreateNamedPipe(
                        pipePath, 0x00000003, 0x00000004 | 0x00000002 | 0x00000008,
                        1, 4096, 4096, 0, ref security);
                    if (handle == null || handle.IsInvalid)
                        throw new InvalidOperationException();
                    if (first)
                    {
                        started.Set();
                        first = false;
                    }
                    bool connected = Native.ConnectNamedPipe(handle, IntPtr.Zero);
                    int error = Marshal.GetLastWin32Error();
                    if (!connected && error != 535)
                        continue;
                    using (FileStream stream = new FileStream(handle, FileAccess.ReadWrite, 4096, false))
                    {
                        handle = null;
                        byte[] frame = new byte[513];
                        int length = stream.Read(frame, 0, frame.Length);
                        string response = length <= 0 || length > 512
                            ? Error("InvalidCommandFrame")
                            : Handle(Encoding.ASCII.GetString(frame, 0, length));
                        byte[] encoded = Encoding.ASCII.GetBytes(response);
                        stream.Write(encoded, 0, encoded.Length);
                        stream.Flush();
                        Native.FlushFileBuffers(stream.SafeFileHandle);
                        Native.DisconnectNamedPipe(stream.SafeFileHandle);
                    }
                }
                catch
                {
                    if (first)
                    {
                        failed = true;
                        started.Set();
                        return;
                    }
                }
                finally
                {
                    if (handle != null)
                        handle.Dispose();
                    if (descriptor != IntPtr.Zero)
                        Native.LocalFree(descriptor);
                }
            }
        }

        private string Handle(string command)
        {
            command = command.Trim();
            if (command == "status")
            {
                bool ready = ReflectionAccess.GetStatic(game, "DNA.CastleMinerZ.CastleMinerZGame", "Instance") != null;
                return "{\"schema\":\"openminerz.original-state-control.v1\",\"ok\":true," +
                       "\"ready\":" + (ready ? "true" : "false") + "}";
            }
            string[] parts = command.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            int centerX;
            int centerZ;
            if (parts.Length != 4 || parts[0] != "dump-state" ||
                !Int32.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out centerX) ||
                !Int32.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out centerZ) ||
                centerX < -1000000 || centerX > 1000000 || centerZ < -1000000 || centerZ > 1000000 ||
                !ValidStateName(parts[3]))
                return Error("InvalidCommand");
            try
            {
                string outputDirectory = Path.Combine(directory, "InteropStates");
                Directory.CreateDirectory(outputDirectory);
                string output = Path.Combine(outputDirectory, parts[3]);
                string json = OriginalStateCapture.Capture(game, centerX, centerZ);
                WriteAtomic(output, json + "\n");
                return "{\"schema\":\"openminerz.original-state-control.v1\",\"ok\":true," +
                       "\"command\":\"dump-state\",\"name\":\"" + parts[3] + "\"}";
            }
            catch (CaptureException exception)
            {
                return Error(exception.Code);
            }
            catch
            {
                return Error("CaptureFailed");
            }
        }

        private static bool ValidStateName(string name)
        {
            if (name.Length < 6 || name.Length > 96 ||
                !name.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                return false;
            foreach (char character in name)
                if (!Char.IsLetterOrDigit(character) && character != '.' && character != '_' && character != '-')
                    return false;
            return true;
        }

        private static string Error(string code)
        {
            return "{\"schema\":\"openminerz.original-state-control.v1\",\"ok\":false," +
                   "\"code\":\"" + code + "\"}";
        }

        private void Wake()
        {
            try
            {
                using (SafeFileHandle handle = Native.CreateFile(pipePath, 0xC0000000, 0, IntPtr.Zero,
                                                                  3, 0, IntPtr.Zero))
                {
                    if (handle == null || handle.IsInvalid)
                        return;
                    using (FileStream stream = new FileStream(handle, FileAccess.ReadWrite))
                    {
                        byte[] status = Encoding.ASCII.GetBytes("status");
                        stream.Write(status, 0, status.Length);
                    }
                }
            }
            catch { }
        }

        private static void WriteAtomic(string path, string contents)
        {
            string temporary = path + ".tmp." + System.Diagnostics.Process.GetCurrentProcess().Id;
            File.WriteAllText(temporary, contents, new UTF8Encoding(false));
            if (File.Exists(path))
                File.Replace(temporary, path, null);
            else
                File.Move(temporary, path);
        }
    }

    internal static class OriginalStateCapture
    {
        private const int Radius = 16;
        private const int WorldHeight = 128;
        private const int TerrainWidth = 384;

        internal static string Capture(Assembly gameAssembly, int centerX, int centerZ)
        {
            for (int attempt = 0; attempt != 3; ++attempt)
            {
                try { return CaptureOnce(gameAssembly, centerX, centerZ); }
                catch (CaptureException exception)
                {
                    bool transient = exception.Code == "TerrainShiftedDuringCapture" ||
                                     exception.Code == "InventoryChangedDuringCapture";
                    if (!transient || attempt == 2)
                        throw;
                    Thread.Sleep(25);
                }
            }
            throw new CaptureException("CaptureFailed");
        }

        private static string CaptureOnce(Assembly assembly, int centerX, int centerZ)
        {
            bool modern = assembly.GetType("DNA.CastleMinerZ.Inventory.InventoryTrayManager", false) != null;
            int protocol = modern ? 4 : 3;
            object game = ReflectionAccess.GetStatic(assembly, "DNA.CastleMinerZ.CastleMinerZGame", "Instance");
            if (game == null)
                throw new CaptureException("GameNotReady");
            object terrain = ReflectionAccess.Get(game, "_terrain");
            object localPlayer = ReflectionAccess.Get(game, "LocalPlayer");
            if (terrain == null || localPlayer == null)
                throw new CaptureException("WorldNotReady");

            int[] blocks = ReflectionAccess.Get(terrain, "_blocks") as int[];
            object worldMin = ReflectionAccess.Get(terrain, "_worldMin");
            if (blocks == null || blocks.Length != TerrainWidth * WorldHeight * TerrainWidth || worldMin == null)
                throw new CaptureException("UnsupportedTerrainLayout");
            int worldMinX = ReflectionAccess.Int(worldMin, "X");
            int worldMinY = ReflectionAccess.Int(worldMin, "Y");
            int worldMinZ = ReflectionAccess.Int(worldMin, "Z");
            int minimumX = centerX - Radius;
            int maximumX = centerX + Radius;
            int minimumZ = centerZ - Radius;
            int maximumZ = centerZ + Radius;
            if (worldMinY != -64 || minimumX < worldMinX || maximumX >= worldMinX + TerrainWidth ||
                minimumZ < worldMinZ || maximumZ >= worldMinZ + TerrainWidth)
                throw new CaptureException("RegionNotFullyLoaded");

            Fingerprint regionHash = new Fingerprint();
            regionHash.U32(unchecked((uint)minimumX));
            regionHash.U32(unchecked((uint)maximumX));
            regionHash.U32(unchecked((uint)minimumZ));
            regionHash.U32(unchecked((uint)maximumZ));
            for (int x = minimumX; x <= maximumX; ++x)
                for (int z = minimumZ; z <= maximumZ; ++z)
                    for (int y = -64; y < 64; ++y)
                    {
                        int localX = x - worldMinX;
                        int localY = y - worldMinY;
                        int localZ = z - worldMinZ;
                        int raw = blocks[localY + localX * WorldHeight + localZ * WorldHeight * TerrainWidth];
                        uint type = unchecked((uint)(raw & 0x7ffff000)) >> 12;
                        regionHash.U8((byte)(type < 94 ? type : 0));
                    }
            object worldMinAfter = ReflectionAccess.Get(terrain, "_worldMin");
            if (worldMinAfter == null || ReflectionAccess.Int(worldMinAfter, "X") != worldMinX ||
                ReflectionAccess.Int(worldMinAfter, "Y") != worldMinY ||
                ReflectionAccess.Int(worldMinAfter, "Z") != worldMinZ ||
                !Object.ReferenceEquals(blocks, ReflectionAccess.Get(terrain, "_blocks")))
                throw new CaptureException("TerrainShiftedDuringCapture");

            PlayerRecord local = ReadPlayer(localPlayer, 0, true, ReadLocalHealth(game));
            object session = ReflectionAccess.Get(game, "CurrentNetworkSession");
            object myGamer = ReflectionAccess.Get(game, "MyNetworkGamer");
            string role = "offline";
            string phase = "world";
            if (session != null)
            {
                bool isHost = myGamer != null && ReflectionAccess.Bool(myGamer, "IsHost", false);
                role = isHost ? "host" : "client";
                phase = isHost ? "hosting" : "connected";
            }
            if (myGamer != null)
                local.Gid = ReflectionAccess.Byte(myGamer, "Id");

            List<PlayerRecord> players = new List<PlayerRecord>();
            if (session != null)
            {
                foreach (object gamer in ReflectionAccess.Enumerate(ReflectionAccess.Get(session, "AllGamers"), 16))
                {
                    object player = ReflectionAccess.Get(gamer, "Tag");
                    if (player == null)
                        continue;
                    bool isLocal = ReflectionAccess.Bool(gamer, "IsLocal", false);
                    PlayerRecord record = ReadPlayer(player, ReflectionAccess.Byte(gamer, "Id"), isLocal,
                                                     isLocal ? (float?)local.Health : null);
                    players.Add(record);
                }
            }
            int localCount = 0;
            HashSet<byte> gids = new HashSet<byte>();
            foreach (PlayerRecord player in players)
            {
                if (!gids.Add(player.Gid))
                    throw new CaptureException("DuplicatePlayerGid");
                if (player.Local) ++localCount;
            }
            if (localCount == 0)
            {
                if (!gids.Add(local.Gid))
                    throw new CaptureException("DuplicatePlayerGid");
                players.Add(local);
                localCount = 1;
            }
            players.Sort((left, right) => left.Gid.CompareTo(right.Gid));
            if (players.Count == 0 || players.Count > 16 || localCount != 1)
                throw new CaptureException("InvalidPlayerSet");
            int remotePlayers = 0;
            foreach (PlayerRecord player in players)
                if (!player.Local)
                    ++remotePlayers;

            int worldSeed = 0;
            object world = ReflectionAccess.Get(game, "CurrentWorld");
            if (world != null)
                worldSeed = ReflectionAccess.Int(world, "Seed");
            object inventory = ReflectionAccess.Get(localPlayer, "PlayerInventory");
            if (inventory == null)
                throw new CaptureException("InventoryNotReady");
            ulong inventoryHash = InventoryFingerprint.Compute(inventory);
            if (inventoryHash != InventoryFingerprint.Compute(inventory))
                throw new CaptureException("InventoryChangedDuringCapture");
            int enemies = CountSingleton(assembly, "DNA.CastleMinerZ.AI.EnemyManager", "_enemies");
            int pickups = CountSingleton(assembly, "DNA.CastleMinerZ.PickupManager", "Pickups");
            object enemyManager = ReflectionAccess.GetStatic(assembly, "DNA.CastleMinerZ.AI.EnemyManager", "Instance");
            bool dragon = enemyManager != null && ReflectionAccess.Bool(enemyManager, "DragonIsActive", false);

            StringBuilder json = new StringBuilder(2048);
            json.Append("{\"schema\":\"openminerz.interop-state.v1\",\"unix_ms\":")
                .Append(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()).Append(",\"role\":\"")
                .Append(role).Append("\",\"phase\":\"").Append(phase)
                .Append("\",\"protocol\":").Append(protocol).Append(",\"world_seed\":").Append(worldSeed)
                .Append(",\"region\":{\"min_x\":").Append(minimumX)
                .Append(",\"max_x\":").Append(maximumX).Append(",\"min_z\":")
                .Append(minimumZ).Append(",\"max_z\":").Append(maximumZ)
                .Append(",\"hash\":\"").Append(regionHash.Hex()).Append("\"},\"player\":");
            AppendPlayer(json, local, false);
            json.Append(",\"inventory_hash\":\"").Append(inventoryHash.ToString("x16", CultureInfo.InvariantCulture))
                .Append("\",\"edit_hash\":null,\"edit_count\":null,\"remote_players\":")
                .Append(remotePlayers).Append(",\"enemies\":").Append(enemies)
                .Append(",\"pickups\":").Append(pickups).Append(",\"dragon_active\":")
                .Append(dragon ? "true" : "false").Append(",\"players\":[");
            for (int index = 0; index < players.Count; ++index)
            {
                if (index != 0) json.Append(',');
                AppendPlayer(json, players[index], true);
            }
            json.Append("],\"state_hash\":null}");
            return json.ToString();
        }

        private static float ReadLocalHealth(object game)
        {
            object gameScreen = ReflectionAccess.Get(game, "GameScreen");
            object hud = gameScreen == null ? null : ReflectionAccess.Get(gameScreen, "HUD");
            float normalized = hud == null ? 1.0f : ReflectionAccess.Float(hud, "PlayerHealth", 1.0f);
            return Math.Max(0.0f, Math.Min(100.0f, normalized * 100.0f));
        }

        private static PlayerRecord ReadPlayer(object player, byte gid, bool local, float? health)
        {
            object position = ReflectionAccess.Get(player, "LocalPosition");
            object rotation = ReflectionAccess.Get(player, "LocalRotation");
            object pitch = ReflectionAccess.Get(player, "TorsoPitch");
            if (position == null || rotation == null)
                throw new CaptureException("PlayerTransformUnavailable");
            float qx = ReflectionAccess.Float(rotation, "X");
            float qy = ReflectionAccess.Float(rotation, "Y");
            float qz = ReflectionAccess.Float(rotation, "Z");
            float qw = ReflectionAccess.Float(rotation, "W");
            float yaw = (float)Math.Atan2(2.0 * (qw * qy + qx * qz),
                                          1.0 - 2.0 * (qy * qy + qz * qz));
            return new PlayerRecord
            {
                Gid = gid,
                Local = local,
                X = ReflectionAccess.Float(position, "X"),
                Y = ReflectionAccess.Float(position, "Y"),
                Z = ReflectionAccess.Float(position, "Z"),
                // State barriers use the native camera convention (+yaw faces
                // +X). Original XNA Y-axis quaternion yaw faces -X.
                Yaw = -yaw,
                Pitch = pitch == null ? 0.0f : ReflectionAccess.Float(pitch, "Radians"),
                Health = health,
                Dead = ReflectionAccess.Bool(player, "Dead", false)
            };
        }

        private static int CountSingleton(Assembly assembly, string typeName, string member)
        {
            object instance = ReflectionAccess.GetStatic(assembly, typeName, "Instance");
            return instance == null ? 0 : ReflectionAccess.Count(ReflectionAccess.Get(instance, member), 1000000);
        }

        private static void AppendPlayer(StringBuilder json, PlayerRecord player, bool includeIdentity)
        {
            json.Append('{');
            if (includeIdentity)
                json.Append("\"gid\":").Append(player.Gid).Append(",\"local\":")
                    .Append(player.Local ? "true" : "false").Append(',');
            json.Append("\"x\":").Append(Number(player.X)).Append(",\"y\":").Append(Number(player.Y))
                .Append(",\"z\":").Append(Number(player.Z)).Append(",\"yaw\":")
                .Append(Number(player.Yaw)).Append(",\"pitch\":").Append(Number(player.Pitch))
                .Append(",\"health\":");
            if (player.Health.HasValue) json.Append(Number(player.Health.Value)); else json.Append("null");
            json.Append(",\"dead\":").Append(player.Dead ? "true" : "false").Append('}');
        }

        private static string Number(float value)
        {
            if (Single.IsNaN(value) || Single.IsInfinity(value))
                throw new CaptureException("NonFiniteState");
            return value.ToString("0.000000", CultureInfo.InvariantCulture);
        }

        private sealed class PlayerRecord
        {
            internal byte Gid;
            internal bool Local;
            internal float X;
            internal float Y;
            internal float Z;
            internal float Yaw;
            internal float Pitch;
            internal float? Health;
            internal bool Dead;
        }
    }

    internal static class InventoryFingerprint
    {
        internal static ulong Compute(object inventory)
        {
            Array storage = ReflectionAccess.Get(inventory, "Inventory") as Array;
            object manager = ReflectionAccess.Get(inventory, "TrayManager");
            bool modern = manager != null;
            Array first = (modern ? ReflectionAccess.Get(manager, "InventoryTray")
                                  : ReflectionAccess.Get(inventory, "InventoryTray")) as Array;
            Array second = modern ? ReflectionAccess.Get(manager, "InventoryTray2") as Array : null;
            if (storage == null || storage.Length != 32 || first == null || first.Length != 8 ||
                (modern && (second == null || second.Length != 8)))
                throw new CaptureException("UnsupportedInventoryLayout");
            Fingerprint hash = new Fingerprint();
            hash.U8((byte)(modern ? 1 : 0));
            hash.U64((ulong)(modern ? ReflectionAccess.Int(manager, "_currentTrayIndex") : 0));
            hash.U64((ulong)ReflectionAccess.Int(inventory, "SelectedInventoryIndex"));
            foreach (object item in storage) AddItem(hash, item, modern);
            foreach (object item in first) AddItem(hash, item, modern);
            if (modern)
                foreach (object item in second) AddItem(hash, item, true);
            else
                for (int index = 0; index != 8; ++index) AddItem(hash, null, false);
            return hash.Value;
        }

        private static void AddItem(Fingerprint hash, object item, bool modern)
        {
            hash.U8((byte)(item == null ? 0 : 1));
            if (item == null) return;
            object itemClass = ReflectionAccess.Get(item, "ItemClass");
            int wireId = ReflectionAccess.Int(itemClass, "ID");
            int internalId = modern && wireId >= 123 && wireId <= 154 ? wireId + 133 : wireId;
            int block = ItemBlock(internalId);
            object blockType = ReflectionAccess.Get(item, "BlockTypeID");
            if (blockType != null)
                block = Convert.ToInt32(blockType, CultureInfo.InvariantCulture);
            int count = ReflectionAccess.Int(item, "StackCount");
            int rounds = ReflectionAccess.Int(item, "RoundsInClip", 0);
            float durability = ReflectionAccess.Float(item, "ItemHealthLevel", 1.0f);
            if (internalId < 0 || internalId > 65535 || block < 0 || block > 255 ||
                count < 0 || count > 65535 || rounds < 0 || rounds > 65535)
                throw new CaptureException("InvalidInventoryValue");
            hash.U16((ushort)internalId);
            hash.U8((byte)block);
            hash.U16((ushort)count);
            hash.U16((ushort)rounds);
            hash.Float32(durability);

            bool locator = internalId == 77 || internalId == 78;
            bool spawn = internalId >= 266 && internalId <= 269;
            bool teleport = internalId == 284;
            object point = locator || spawn || teleport ? ReflectionAccess.Get(item, "PointToLocation") : null;
            hash.Float32(point == null ? 0.0f : ReflectionAccess.Float(point, "X"));
            hash.Float32(point == null ? 0.0f : ReflectionAccess.Float(point, "Y"));
            hash.Float32(point == null ? 0.0f : ReflectionAccess.Float(point, "Z"));
            string name = "Alpha";
            if (locator)
                name = Convert.ToString(ReflectionAccess.Get(item, "_GPSname"), CultureInfo.InvariantCulture) ?? "Alpha";
            else if (teleport)
                name = Convert.ToString(ReflectionAccess.Get(item, "CustomBlockName"), CultureInfo.InvariantCulture) ?? "Alpha";
            hash.Text(name);
            hash.U8((byte)(point == null ? 0 : 1));
        }

        private static int ItemBlock(int id)
        {
            switch (id)
            {
                case 1: return 1; case 2: return 3; case 3: return 6; case 4: return 17;
                case 5: return 19; case 6: return 4; case 7: return 20; case 8: return 21;
                case 9: return 22; case 10: return 23; case 11: return 24; case 12: return 25;
                case 14: return 26; case 73: return 33; case 74: return 15; case 75: return 16;
                case 76: return 34; case 79: return 41; case 80: return 42; case 102: return 43;
                case 107: return 44; case 123: return 2; case 124: return 7; case 125: return 8;
                case 126: return 9; case 127: return 10; case 128: return 11; case 129: return 12;
                case 130: return 18; case 260: return 49; case 261: return 50; case 262: return 51;
                case 263: return 52; case 264: return 53; case 265: return 54; case 266: return 56;
                case 267: return 58; case 268: return 59; case 269: return 57; case 270: return 45;
                case 271: return 46; case 272: return 47; case 273: return 48; case 280: return 62;
                case 281: return 67; case 284: return 77; case 286: return 62; case 287: return 62;
                default: return 0;
            }
        }
    }

    internal sealed class Fingerprint
    {
        internal ulong Value = 1469598103934665603UL;
        internal void U8(byte value) { unchecked { Value = (Value ^ value) * 1099511628211UL; } }
        internal void U16(ushort value) { U8((byte)value); U8((byte)(value >> 8)); }
        internal void U32(uint value) { for (int shift = 0; shift < 32; shift += 8) U8((byte)(value >> shift)); }
        internal void U64(ulong value) { for (int shift = 0; shift < 64; shift += 8) U8((byte)(value >> shift)); }
        internal void Float32(float value) { U32(unchecked((uint)BitConverter.ToInt32(BitConverter.GetBytes(value), 0))); }
        internal void Text(string value)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(value ?? String.Empty);
            U64((ulong)bytes.Length);
            foreach (byte character in bytes) U8(character);
        }
        internal string Hex() { return Value.ToString("x16", CultureInfo.InvariantCulture); }
    }

    internal static class ReflectionAccess
    {
        private const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic |
                                           BindingFlags.Instance | BindingFlags.Static;

        internal static object GetStatic(Assembly assembly, string typeName, string name)
        {
            Type type = assembly.GetType(typeName, false);
            return type == null ? null : Get(type, null, name);
        }

        internal static object Get(object target, string name)
        {
            return target == null ? null : Get(target.GetType(), target, name);
        }

        private static object Get(Type type, object target, string name)
        {
            for (Type current = type; current != null; current = current.BaseType)
            {
                PropertyInfo property = current.GetProperty(name, Flags | BindingFlags.DeclaredOnly);
                if (property != null && property.GetIndexParameters().Length == 0)
                {
                    try { return property.GetValue(target, null); } catch { return null; }
                }
                FieldInfo field = current.GetField(name, Flags | BindingFlags.DeclaredOnly);
                if (field != null)
                {
                    try { return field.GetValue(target); } catch { return null; }
                }
            }
            return null;
        }

        internal static int Int(object target, string name, int fallback = Int32.MinValue)
        {
            object value = Get(target, name);
            if (value == null)
            {
                if (fallback != Int32.MinValue) return fallback;
                throw new CaptureException("MissingNumericMember");
            }
            return Convert.ToInt32(value, CultureInfo.InvariantCulture);
        }

        internal static byte Byte(object target, string name)
        {
            int value = Int(target, name);
            if (value < 0 || value > 255) throw new CaptureException("InvalidGamerId");
            return (byte)value;
        }

        internal static float Float(object target, string name, float fallback = Single.NaN)
        {
            object value = Get(target, name);
            if (value == null)
            {
                if (!Single.IsNaN(fallback)) return fallback;
                throw new CaptureException("MissingFloatMember");
            }
            float result = Convert.ToSingle(value, CultureInfo.InvariantCulture);
            if (Single.IsNaN(result) || Single.IsInfinity(result))
                throw new CaptureException("NonFiniteState");
            return result;
        }

        internal static bool Bool(object target, string name, bool fallback)
        {
            object value = Get(target, name);
            return value == null ? fallback : Convert.ToBoolean(value, CultureInfo.InvariantCulture);
        }

        internal static IEnumerable<object> Enumerate(object collection, int maximum)
        {
            IEnumerable enumerable = collection as IEnumerable;
            if (enumerable == null) yield break;
            int count = 0;
            foreach (object value in enumerable)
            {
                if (++count > maximum) throw new CaptureException("CollectionLimitExceeded");
                yield return value;
            }
        }

        internal static int Count(object collection, int maximum)
        {
            if (collection == null) return 0;
            object countValue = Get(collection, "Count");
            if (countValue != null)
            {
                int count = Convert.ToInt32(countValue, CultureInfo.InvariantCulture);
                if (count < 0 || count > maximum) throw new CaptureException("CollectionLimitExceeded");
                return count;
            }
            int result = 0;
            foreach (object ignored in Enumerate(collection, maximum)) ++result;
            return result;
        }
    }

    internal sealed class CaptureException : Exception
    {
        internal readonly string Code;
        internal CaptureException(string code) { Code = code; }
    }

    internal static class Native
    {
        [StructLayout(LayoutKind.Sequential)]
        internal struct SecurityAttributes
        {
            internal int Length;
            internal IntPtr SecurityDescriptor;
            internal int InheritHandle;
        }

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(
            string descriptor, uint revision, out IntPtr securityDescriptor, IntPtr size);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern SafeFileHandle CreateNamedPipe(string name, uint openMode, uint pipeMode,
                                                               uint maximumInstances, uint outputSize,
                                                               uint inputSize, uint timeout,
                                                               ref SecurityAttributes security);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ConnectNamedPipe(SafeFileHandle pipe, IntPtr overlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DisconnectNamedPipe(SafeFileHandle pipe);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool FlushFileBuffers(SafeFileHandle file);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern SafeFileHandle CreateFile(string name, uint access, uint share,
                                                          IntPtr security, uint creation,
                                                          uint flags, IntPtr template);

        [DllImport("kernel32.dll")]
        internal static extern IntPtr LocalFree(IntPtr memory);
    }
}
