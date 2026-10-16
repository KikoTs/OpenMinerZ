using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// The offline stage the original simulation code runs on. It loads the user's
// owned, unmodified Castle Miner Z assemblies by path and builds just enough of
// a game around them for the ORIGINAL enemy, player and projectile classes to
// step over scripted time: a hidden 16x16 graphics device so the real models,
// skeletons and animation clips load, an inert CastleMinerZGame, a one-gamer
// host session whose sends land in the original local-echo queue, and a
// BlockTerrain whose block array the probe fills by hand.
//
// Nothing here starts Steam, opens a session or runs the game loop.
internal static class R
{
    public const BindingFlags All = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public |
                                    BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

    private static MemberInfo Find(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(name, All);
            if (field != null) return field;
            PropertyInfo property = current.GetProperty(name, All);
            if (property != null && property.GetIndexParameters().Length == 0) return property;
        }
        throw new MissingMemberException(type.FullName, name);
    }

    public static bool Has(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
            if (current.GetField(name, All) != null || current.GetProperty(name, All) != null) return true;
        return false;
    }

    public static FieldInfo Field(Type type, string name) { return (FieldInfo)Find(type, name); }
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

    public static MethodInfo Method(Type type, string name, params Type[] parameters)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            MethodInfo method = current.GetMethod(name, All, null, parameters, null);
            if (method != null) return method;
        }
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

    public static object Invoke(MethodBase method, object target, object[] arguments)
    {
        try { return method.Invoke(target, arguments); }
        catch (TargetInvocationException error)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
    }

    public static object Inert(Type type)
    {
        object value = FormatterServices.GetUninitializedObject(type);
        GC.SuppressFinalize(value);
        return value;
    }
}

// Replaces the body of an original method that only exists to reach a device
// the probe does not have (the XACT sound bank, streamed chunk geometry, the
// light table, the HUD). The replacement is entered through a five-byte jump
// written over the method's compiled entry point; every other original method
// keeps running its shipped IL. Each redirected method is listed where it is
// installed, so what was NOT original code is exact.
internal static class Detour
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualProtect(IntPtr address, UIntPtr size, uint protection, out uint previous);

    private static IntPtr Entry(IntPtr address)
    {
        // Follow the runtime's own precode jumps to the compiled body.
        for (int hop = 0; hop < 8; ++hop)
        {
            byte first = Marshal.ReadByte(address);
            if (first == 0xE9)
            {
                address = address + 5 + Marshal.ReadInt32(address + 1);
                continue;
            }
            if (first == 0xB8 && Marshal.ReadByte(address + 5) == 0x90 && Marshal.ReadByte(address + 6) == 0xE9)
            {
                address = address + 11 + Marshal.ReadInt32(address + 7);
                continue;
            }
            if (first == 0xB8 && Marshal.ReadByte(address + 5) == 0x89 && Marshal.ReadByte(address + 6) == 0xED &&
                Marshal.ReadByte(address + 7) == 0xE9)
            {
                address = address + 12 + Marshal.ReadInt32(address + 8);
                continue;
            }
            break;
        }
        return address;
    }

    public static void Redirect(MethodBase original, MethodInfo replacement)
    {
        if (IntPtr.Size != 4) throw new PlatformNotSupportedException("The probe is an x86 process");
        RuntimeHelpers.PrepareMethod(original.MethodHandle);
        RuntimeHelpers.PrepareMethod(replacement.MethodHandle);
        IntPtr from = Entry(original.MethodHandle.GetFunctionPointer());
        IntPtr to = Entry(replacement.MethodHandle.GetFunctionPointer());
        uint previous;
        if (!VirtualProtect(from, (UIntPtr)5, 0x40, out previous))
            throw new InvalidOperationException("VirtualProtect failed for " + original.Name);
        Marshal.WriteByte(from, 0xE9);
        Marshal.WriteInt32(from + 1, to.ToInt32() - from.ToInt32() - 5);
        VirtualProtect(from, (UIntPtr)5, previous, out previous);
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

internal sealed class Captured
{
    public string Name;
    public byte[] Payload;
    public byte Sender;
}

internal static class Rig
{
    public static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    public static string GameDirectory;
    public static Assembly Game;
    public static Assembly Common;
    public static object GameInstance;
    public static object Terrain;
    public static object Manager;
    public static object Session;
    public static object Provider;
    public static object LocalGamer;
    public static object LocalPlayer;
    public static object Stats;
    public static int[] Blocks;
    public static bool Modern;
    public static Type[] MessageTypes;
    public static Type IntVector3;
    public const byte LocalGid = 3;

    // What the redirected methods saw, drained by the scenario after each frame.
    public static readonly List<string> Events = new List<string>();
    public static readonly List<string> Redirected = new List<string>();

    public static Type T(string name)
    {
        Type type = Game.GetType(name, false) ?? Common.GetType(name, false);
        if (type == null) throw new InvalidDataException("Original type absent: " + name);
        return type;
    }

    public static Type TryT(string name)
    {
        return Game.GetType(name, false) ?? Common.GetType(name, false);
    }

    public static string F(float value) { return value.ToString("R", Inv); }
    public static string F(double value) { return ((float)value).ToString("R", Inv); }
    public static string V(Vector3 value) { return F(value.X) + "," + F(value.Y) + "," + F(value.Z); }

    private static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        foreach (string extension in new[] { ".dll", ".exe" })
        {
            string path = Path.Combine(GameDirectory, name + extension);
            if (File.Exists(path)) return Assembly.LoadFrom(path);
        }
        return null;
    }

    // ------------------------------------------------------------ redirected
    // Each of these stands in for an original method that needs a device.

    private static object NoCue3D(object self, string name, object emitter)
    {
        return null;
    }

    private static object NoCue(object self, string name)
    {
        return null;
    }

    private static bool RegionLoaded(object self, Vector3 position)
    {
        return true;
    }

    private static void NoLighting(object self, Vector3 position, ref Vector3 a, ref Vector3 b, ref Vector3 c,
                                   ref Vector3 d, ref Vector3 e)
    {
    }

    private struct Int3
    {
        public int X, Y, Z;
    }

    // The original consults each chunk's pending-edit queue first; the probe's
    // terrain has no chunks, only the block array the queue would be applied to.
    private static int BlockWithChanges(object self, Int3 cell)
    {
        int x = cell.X + 192, y = cell.Y + 64, z = cell.Z + 192;
        if (x < 0 || x >= 384 || y < 0 || y >= 128 || z < 0 || z >= 384) return 0;
        return (Blocks[y + x * 128 + z * 128 * 384] >> 12) & 0x7FFFF;
    }

    private static void Redirect(Type type, string name, int parameters, string replacement)
    {
        MethodInfo original = type.GetMethods(R.All).First(method => method.Name == name &&
                                                                     method.GetParameters().Length == parameters);
        Detour.Redirect(original, typeof(Rig).GetMethod(replacement, BindingFlags.Static | BindingFlags.NonPublic));
        Redirected.Add(type.Name + "." + name);
    }

    public static void Redirect(MethodBase original, MethodInfo replacement)
    {
        Detour.Redirect(original, replacement);
        Redirected.Add(original.DeclaringType.Name + "." + original.Name);
    }

    // ------------------------------------------------------------------ boot

    public static bool Boot(string gameDirectory)
    {
        GameDirectory = gameDirectory;
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
        Common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
        Modern = Game.GetName().Version.Minor >= 9;
        R.CallStatic(Common.GetType("DNA.Reflection.ReflectionTools", true), "RegisterAssembly", Common, Game);
        IntVector3 = T("DNA.IntVector3");

        var device = new DeviceService();
        string profileDirectory = "HiDefContent";
        var form = new System.Windows.Forms.Form();
        var parameters = new PresentationParameters
        {
            BackBufferWidth = 16, BackBufferHeight = 16, DeviceWindowHandle = form.Handle,
            IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
        };
        try
        {
            try { device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.HiDef, parameters); }
            catch (Exception)
            {
                profileDirectory = "ReachContent";
                device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.Reach, parameters);
            }
        }
        catch (Exception error)
        {
            Console.WriteLine("SKIP no graphics device for the original content reader: " + error.Message);
            return false;
        }
        var services = new ServiceTable();
        services.Services[typeof(IGraphicsDeviceService)] = device;

        Type gameType = T("DNA.CastleMinerZ.CastleMinerZGame");
        GameInstance = R.Inert(gameType);
        R.SetStatic(gameType, "Instance", GameInstance);
        ((Game)GameInstance).Content = new OwnedContent(services, Path.Combine(gameDirectory, "Content"), profileDirectory);
        foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
            if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(GameInstance, device);
        R.Set(GameInstance, "CurrentGameTime", new GameTime());
        R.Set(GameInstance, "GameMode", Enum.Parse(R.Field(gameType, "GameMode").FieldType, "Survival"));
        // The stats constructor binds to Steam; only its option flags are read
        // here. AutoClimb is the shipped default (true) unless a scenario says so.
        Stats = R.Inert(R.Field(gameType, "PlayerStats").FieldType);
        R.Set(Stats, "AutoClimb", true);
        R.Set(GameInstance, "PlayerStats", Stats);

        // The sound bank, the chunk streamer and the light table are devices.
        Type sound = T("DNA.Audio.SoundManager");
        Redirect(sound.GetMethods(R.All).First(method => method.Name == "PlayInstance" && method.GetParameters().Length == 2),
                 typeof(Rig).GetMethod("NoCue3D", BindingFlags.Static | BindingFlags.NonPublic));
        Redirect(sound.GetMethods(R.All).First(method => method.Name == "PlayInstance" && method.GetParameters().Length == 1),
                 typeof(Rig).GetMethod("NoCue", BindingFlags.Static | BindingFlags.NonPublic));
        Type terrainType = T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        Redirect(terrainType, "RegionIsLoaded", 1, "RegionLoaded");
        Redirect(terrainType, "GetEnemyLighting", 6, "NoLighting");
        Redirect(terrainType.GetMethods(R.All).First(method => method.Name == "GetBlockWithChanges" &&
                                                               method.GetParameters()[0].ParameterType == IntVector3),
                 typeof(Rig).GetMethod("BlockWithChanges", BindingFlags.Static | BindingFlags.NonPublic));

        SetupSession();
        SetupTerrain();
        return true;
    }

    private static object MakeGamer(Type type, byte id, bool local, bool host, string tag)
    {
        object gamer = R.Inert(type);
        R.Set(gamer, "_globalId", id);
        R.Set(gamer, "_isLocal", local);
        R.Set(gamer, "_isHost", host);
        R.Set(gamer, "_gamerTag", tag);
        FieldInfo back = R.Field(type, "_networkSession");
        R.Set(gamer, "_networkSession", Activator.CreateInstance(back.FieldType, Session));
        if (R.Has(type, "_pendingData"))
            R.Set(gamer, "_pendingData", Activator.CreateInstance(R.Field(type, "_pendingData").FieldType));
        return gamer;
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

    // A real Player whose Entity and FPSRig constructors ran (transform,
    // physics, pivots and FPS camera are the original's). Player's own
    // constructor, which builds the avatar and the HUD links, does not.
    public static object MakePlayer(object gamer)
    {
        Type playerType = T("DNA.CastleMinerZ.Player");
        object player = R.Inert(playerType);
        T("DNA.Drawing.FPSRig").GetConstructor(Type.EmptyTypes).Invoke(player, null);
        foreach (FieldInfo field in playerType.GetFields(R.All))
        {
            if (field.IsStatic) continue;
            if (field.Name == "MovementProbe") field.SetValue(player, Activator.CreateInstance(field.FieldType));
            if (field.Name == "PlayerAABB")
                field.SetValue(player, new BoundingBox(new Vector3(-0.35f, 0f, -0.35f), new Vector3(0.35f, 1.7f, 0.35f)));
        }
        R.Set(player, "Gamer", gamer);
        R.Set(gamer, "_tag", player);
        return player;
    }

    public static object AddRemote(byte id, string tag)
    {
        object gamer = MakeGamer(T("DNA.Net.GamerServices.NetworkGamer"), id, false, false, tag);
        object player = MakePlayer(gamer);
        ((IList)R.Get(Provider, "_allGamers")).Add(gamer);
        return player;
    }

    private static void SetupSession()
    {
        Type providerBase = T("DNA.Net.GamerServices.NetworkSessionProvider");
        Type providerType = TryT("DNA.Net.GamerServices.LidgrenProvider.LidgrenNetworkSessionProvider") ??
                            Common.GetTypes().First(type => !type.IsAbstract && providerBase.IsAssignableFrom(type));
        Type sessionType = T("DNA.Net.GamerServices.NetworkSession");
        Type localType = T("DNA.Net.GamerServices.LocalNetworkGamer");
        Type gamerType = T("DNA.Net.GamerServices.NetworkGamer");
        Provider = R.Inert(providerType);
        Session = R.Inert(sessionType);
        R.Set(Session, "_sessionProvider", Provider);
        R.Set(Provider, "_networkSession", Session);
        R.Set(Provider, "_isHost", true);
        R.Set(Provider, "_sessionType", Enum.Parse(T("DNA.Net.GamerServices.NetworkSessionType"), "PlayerMatch"));
        R.Set(Provider, "_properties", Activator.CreateInstance(T("DNA.Net.GamerServices.NetworkSessionProperties")));
        LocalGamer = MakeGamer(localType, LocalGid, true, true, "LocalStock");
        object locals = GenericList(localType);
        ((IList)locals).Add(LocalGamer);
        object all = GenericList(gamerType);
        ((IList)all).Add(LocalGamer);
        // No remote gamer is ever placed in the REMOTE list, so nothing can be
        // handed to a transport: a broadcast only reaches the local echo queue.
        object remotes = GenericList(gamerType);
        R.Set(Provider, "_localGamers", locals);
        R.Set(Provider, "_localGamerCollection", Collection(localType, locals));
        R.Set(Provider, "_remoteGamers", remotes);
        R.Set(Provider, "_remoteGamerCollection", Collection(gamerType, remotes));
        R.Set(Provider, "_allGamers", all);
        R.Set(Provider, "_allGamerCollection", Collection(gamerType, all));
        R.Set(GameInstance, "_networkSession", Session);
        R.Set(GameInstance, "MyNetworkGamer", LocalGamer);
        R.Set(GameInstance, "TerrainServerID", LocalGid);
        LocalPlayer = MakePlayer(LocalGamer);
        R.Set(GameInstance, "_localPlayer", LocalPlayer);
        MessageTypes = (Type[])R.GetStatic(T("DNA.Net.Message"), "_messageTypes");
        Drain();
    }

    public static List<Captured> Drain()
    {
        var result = new List<Captured>();
        var pending = (IList)R.Get(LocalGamer, "_pendingData");
        foreach (object packet in pending)
        {
            var data = (byte[])R.Get(packet, "Data");
            object sender = R.Get(packet, "Sender");
            var payload = new byte[data.Length - 2];
            Buffer.BlockCopy(data, 1, payload, 0, payload.Length);
            result.Add(new Captured
            {
                Name = MessageTypes[data[0]].Name, Payload = payload, Sender = (byte)R.Get(sender, "Id")
            });
        }
        pending.Clear();
        return result;
    }

    public static object Decode(Captured captured)
    {
        Type type = MessageTypes.First(candidate => candidate.Name == captured.Name);
        object message = Activator.CreateInstance(type, true);
        R.Set(message, "_sender", LocalGamer);
        using (var stream = new MemoryStream(captured.Payload, false))
        using (var reader = new BinaryReader(stream))
            R.Invoke(R.Method(type, "RecieveData", 1), message, new object[] { reader });
        return message;
    }

    // -------------------------------------------------------------- terrain

    public static object Cell(int x, int y, int z)
    {
        return Activator.CreateInstance(IntVector3, x, y, z);
    }

    private static void SetupTerrain()
    {
        Type terrainType = T("DNA.CastleMinerZ.Terrain.BlockTerrain");
        Terrain = R.Inert(terrainType);
        Blocks = new int[384 * 128 * 384];
        R.Set(Terrain, "_blocks", Blocks);
        R.Set(Terrain, "_worldMin", Cell(-192, -64, -192));
        R.Set(Terrain, "_initted", true);
        R.Set(Terrain, "_resetRequested", false);
        R.Set(Terrain, "_facePlanes", new[]
        {
            new Plane(1f, 0f, 0f, 0f), new Plane(0f, 0f, -1f, 0f), new Plane(-1f, 0f, 0f, 0f),
            new Plane(0f, 0f, 1f, 0f), new Plane(0f, 1f, 0f, 0f), new Plane(0f, -1f, 0f, 0f)
        });
        R.SetStatic(terrainType, "_theTerrain", Terrain);
        R.Set(GameInstance, "_terrain", Terrain);
        object world = R.Inert(T("DNA.CastleMinerZ.WorldInfo"));
        R.Set(world, "Crates", Activator.CreateInstance(R.Field(world.GetType(), "Crates").FieldType));
        R.Set(GameInstance, "CurrentWorld", world);
        ClearTerrain();
    }

    public static void ClearTerrain()
    {
        Array.Clear(Blocks, 0, Blocks.Length);
    }

    public static void SetBlock(int x, int y, int z, int block)
    {
        // Sunlit air above, the block type in the top bits: Block.GetTypeIndex.
        Blocks[(y + 64) + (x + 192) * 128 + (z + 192) * 128 * 384] = block << 12;
    }

    public static void Fill(int x0, int y0, int z0, int x1, int y1, int z1, int block)
    {
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    SetBlock(x, y, z, block);
    }

    public static void MoveEntity(object entity, Vector3 position)
    {
        R.Set(entity, "LocalPosition", position);
    }

    public static Vector3 WorldPosition(object entity)
    {
        return (Vector3)R.Get(entity, "WorldPosition");
    }

    public static object Physics(object entity)
    {
        return R.Get(entity, "Physics");
    }

    public static Vector3 Velocity(object entity)
    {
        return (Vector3)R.Get(Physics(entity), "WorldVelocity");
    }

    public static void SetVelocity(object entity, Vector3 value)
    {
        R.Set(Physics(entity), "WorldVelocity", value);
    }
}
