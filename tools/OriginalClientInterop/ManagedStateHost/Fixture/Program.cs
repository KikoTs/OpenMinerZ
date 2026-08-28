using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace DNA
{
    public struct IntVector3 { public int X, Y, Z; public IntVector3(int x, int y, int z) { X = x; Y = y; Z = z; } }
    public struct Angle { public float Radians; public Angle(float radians) { Radians = radians; } }
}

namespace DNA.CastleMinerZ
{
    public struct Vector3 { public float X, Y, Z; public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; } }
    public struct Quaternion { public float X, Y, Z, W; }

    public sealed class WorldInfo { public int Seed = 12345; }
    public sealed class InGameHUD { public float PlayerHealth = 0.79f; }
    public sealed class GameScreen { public InGameHUD HUD = new InGameHUD(); }
    public sealed class NetworkGamer
    {
        public byte Id { get; set; }
        public bool IsHost { get; set; }
        public bool IsLocal { get; set; }
        public object Tag { get; set; }
    }
    public sealed class NetworkSession { public List<NetworkGamer> AllGamers = new List<NetworkGamer>(); }
    public sealed class BlockTerrain
    {
        public int[] _blocks = new int[384 * 128 * 384];
        public DNA.IntVector3 _worldMin = new DNA.IntVector3(-192, -64, -192);
        public BlockTerrain()
        {
            Set(0, 0, 0, 70);
            Set(-1, 0, 0, 69);
            Set(1, 0, 0, 7);
        }
        private void Set(int x, int y, int z, int type)
        {
            int lx = x - _worldMin.X, ly = y - _worldMin.Y, lz = z - _worldMin.Z;
            _blocks[ly + lx * 128 + lz * 128 * 384] = type << 12;
        }
    }
    public sealed class Player
    {
        public Vector3 LocalPosition { get; set; }
        public Quaternion LocalRotation { get; set; }
        public DNA.Angle TorsoPitch;
        public Inventory.PlayerInventory PlayerInventory;
        public bool Dead { get; set; }
    }
    public sealed class CastleMinerZGame
    {
        public static CastleMinerZGame Instance;
        public Player LocalPlayer { get; private set; }
        public NetworkSession CurrentNetworkSession { get; private set; }
        public BlockTerrain _terrain;
        public WorldInfo CurrentWorld;
        public GameScreen GameScreen;
        public NetworkGamer MyNetworkGamer;

        public CastleMinerZGame()
        {
            Instance = this;
            _terrain = new BlockTerrain();
            CurrentWorld = new WorldInfo();
            GameScreen = new GameScreen();
            LocalPlayer = MakePlayer(12.25f, -10.5f, 7.75f, 0.75f, -0.25f, false);
            LocalPlayer.PlayerInventory = Inventory.PlayerInventory.CreateFixture();
            Player remote = MakePlayer(15.0f, -10.0f, 9.0f, -0.5f, 0.125f, false);
            MyNetworkGamer = new NetworkGamer { Id = 4, IsHost = true, IsLocal = true, Tag = LocalPlayer };
            CurrentNetworkSession = new NetworkSession();
            CurrentNetworkSession.AllGamers.Add(MyNetworkGamer);
            CurrentNetworkSession.AllGamers.Add(new NetworkGamer { Id = 9, IsHost = false, IsLocal = false, Tag = remote });
        }

        private static Player MakePlayer(float x, float y, float z, float yaw, float pitch, bool dead)
        {
            return new Player
            {
                LocalPosition = new Vector3(x, y, z),
                LocalRotation = new Quaternion { Y = (float)Math.Sin(yaw * 0.5f), W = (float)Math.Cos(yaw * 0.5f) },
                TorsoPitch = new DNA.Angle(pitch),
                Dead = dead,
                PlayerInventory = new Inventory.PlayerInventory()
            };
        }
    }
    public sealed class PickupManager
    {
        public static PickupManager Instance;
        public List<object> Pickups = new List<object>();
    }

    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            new CastleMinerZGame();
            AI.EnemyManager.Instance = new AI.EnemyManager();
            AI.EnemyManager.Instance._enemies.Add(new object());
            AI.EnemyManager.Instance._enemies.Add(new object());
            AI.EnemyManager.Instance._enemies.Add(new object());
            AI.EnemyManager.Instance.DragonIsActive = true;
            PickupManager.Instance = new PickupManager();
            PickupManager.Instance.Pickups.Add(new object());
            PickupManager.Instance.Pickups.Add(new object());
            while (!File.Exists(Path.Combine(Environment.CurrentDirectory, "OpenMinerZFixture.stop")))
                Thread.Sleep(25);
        }
    }
}

namespace DNA.CastleMinerZ.AI
{
    public sealed class EnemyManager
    {
        public static EnemyManager Instance;
        public List<object> _enemies = new List<object>();
        public bool DragonIsActive { get; set; }
    }
}

namespace DNA.CastleMinerZ.Inventory
{
    public enum InventoryItemIDs { BareHands = 0, DirtBlock = 1, Pistol = 51, GPS = 77, PrecisionLaser = 149 }
    public class InventoryItemClass { public InventoryItemIDs ID; public InventoryItemClass(InventoryItemIDs id) { ID = id; } }
    public class InventoryItem
    {
        public InventoryItemClass ItemClass { get; private set; }
        public int StackCount { get; set; }
        public float ItemHealthLevel = 1.0f;
        public InventoryItem(InventoryItemIDs id, int count) { ItemClass = new InventoryItemClass(id); StackCount = count; }
    }
    public sealed class BlockInventoryItem : InventoryItem
    {
        public int BlockTypeID { get; private set; }
        public BlockInventoryItem(InventoryItemIDs id, int count, int block) : base(id, count) { BlockTypeID = block; }
    }
    public sealed class GunInventoryItem : InventoryItem
    {
        public int RoundsInClip { get; set; }
        public GunInventoryItem(InventoryItemIDs id, int count, int rounds) : base(id, count) { RoundsInClip = rounds; }
    }
    public sealed class GPSItem : InventoryItem
    {
        public DNA.CastleMinerZ.Vector3 PointToLocation { get; set; }
        private string _GPSname = "Alpha";
        public GPSItem(string name) : base(InventoryItemIDs.GPS, 1) { _GPSname = name; }
    }
    public sealed class InventoryTrayManager
    {
        public InventoryItem[,] Trays = new InventoryItem[2, 8];
        public InventoryItem[] InventoryTray;
        public InventoryItem[] InventoryTray2;
        public int _currentTrayIndex;
        public InventoryTrayManager() { InventoryTray = Row(0); InventoryTray2 = Row(1); }
        private InventoryItem[] Row(int row)
        {
            InventoryItem[] result = new InventoryItem[8];
            return result;
        }
    }
    public sealed class PlayerInventory
    {
        public InventoryItem[] Inventory = new InventoryItem[32];
        public InventoryTrayManager TrayManager = new InventoryTrayManager();
        public int SelectedInventoryIndex;
        public static PlayerInventory CreateFixture()
        {
            PlayerInventory result = new PlayerInventory();
            result.Inventory[0] = new BlockInventoryItem(InventoryItemIDs.DirtBlock, 12, 1);
            GPSItem gps = new GPSItem("Alpha Mine") { PointToLocation = new DNA.CastleMinerZ.Vector3(10, -3, 22), ItemHealthLevel = 0.5f };
            result.Inventory[3] = gps;
            result.TrayManager.InventoryTray[2] = new GunInventoryItem(InventoryItemIDs.Pistol, 1, 3) { ItemHealthLevel = 0.75f };
            result.TrayManager.InventoryTray2[5] = new GunInventoryItem(InventoryItemIDs.PrecisionLaser, 1, 7) { ItemHealthLevel = 0.625f };
            result.TrayManager._currentTrayIndex = 1;
            result.SelectedInventoryIndex = 5;
            return result;
        }
    }
}
