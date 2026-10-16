#nullable disable
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// The retail character, read through the game's own code.
//
// Shared by ContentBridge (which writes DerivedData/character.omrc) and by
// tools/OriginalClientInterop/RigParityProbe (which records what the original
// engine does with it). Everything here loads the user's owned assemblies by
// path and asks THEM for the facts:
//
//   * Player's static initialisation loads the body every player is drawn with;
//   * a real DNA.Avatars.Avatar with that body as its ProxyModelEntity supplies
//     the two skeletons (the 71-bone attachment skeleton that carries the eye
//     camera and the held item, and the body's own skinning skeleton), the
//     name map between them and the constants the engine applies around a clip;
//   * the clips are the real AnimationClip objects, keys untouched;
//   * the clip registrations come out of CastleMinerZGame.SecondaryLoad's IL;
//   * InventoryItem.Initalize and every item class's CreateEntity(ItemUse.Hand)
//     supply the held-item transforms.
//
// No retargeting happens anywhere: bone spaces, key data and vertex weights are
// written exactly as the game holds them.
namespace OpenMinerZ.RetailCharacter
{
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

        public static MethodInfo Method(Type type, string name, params Type[] parameterTypes)
        {
            for (Type current = type; current != null; current = current.BaseType)
                foreach (MethodInfo method in current.GetMethods(All))
                {
                    if (method.Name != name || method.IsGenericMethodDefinition) continue;
                    ParameterInfo[] parameters = method.GetParameters();
                    if (parameters.Length != parameterTypes.Length) continue;
                    bool match = true;
                    for (int index = 0; index < parameters.Length && match; ++index)
                        match = parameterTypes[index] == null ||
                                parameters[index].ParameterType.IsAssignableFrom(parameterTypes[index]);
                    if (match) return method;
                }
            throw new MissingMethodException(type.FullName, name);
        }

        public static object Call(object target, string name, params object[] arguments)
        {
            Type[] types = arguments.Select(argument => argument == null ? null : argument.GetType()).ToArray();
            return Invoke(Method(target.GetType(), name, types), target, arguments);
        }

        public static object CallStatic(Type type, string name, params object[] arguments)
        {
            Type[] types = arguments.Select(argument => argument == null ? null : argument.GetType()).ToArray();
            return Invoke(Method(type, name, types), null, arguments);
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

    // The original ProfiledContentManager, minus its device lock: the asset at
    // the content root wins, otherwise the active graphics profile's sub-folder.
    // Every load is logged with the file it resolved to.
    internal sealed class OwnedContent : ContentManager
    {
        public readonly string ProfileDirectory;
        public readonly List<string> Log = new List<string>();

        public OwnedContent(IServiceProvider services, string root, string profileDirectory) : base(services, root)
        {
            ProfileDirectory = profileDirectory;
        }

        public string Resolve(string assetName)
        {
            if (File.Exists(Path.Combine(RootDirectory, assetName + ".xnb"))) return assetName;
            return Path.Combine(ProfileDirectory, assetName);
        }

        public override T Load<T>(string assetName)
        {
            string resolved = Resolve(assetName);
            Log.Add(typeof(T).Name + " " + assetName + " -> " + resolved + ".xnb");
            return base.Load<T>(resolved);
        }
    }

    // The owned game, loaded but not started: an inert CastleMinerZGame context
    // whose Content reads the real files through a hidden graphics device.
    internal sealed class Original
    {
        public string GameDirectory;
        public Assembly Game;
        public Assembly Common;
        public object GameInstance;
        public OwnedContent Content;
        public GraphicsDevice Device;
        public string Profile;                 // "HiDef" or "Reach": what the game itself would pick here
        public bool Modern;
        public string Version;
        public Model ProxyModel;
        public string ProxyModelAsset;
        public List<string> PlayerContentLog = new List<string>();
        public List<ClipRegistration> Registrations;
        public Dictionary<string, object> Clips = new Dictionary<string, object>(StringComparer.Ordinal);

        public Type T(string name) { return Game.GetType(name, false) ?? Common.GetType(name, true); }

        public static Original Create(string gameDirectory, bool forceReach)
        {
            var original = new Original { GameDirectory = gameDirectory };
            original.Game = Assembly.LoadFrom(Path.Combine(gameDirectory, "CastleMinerZ.exe"));
            original.Common = Assembly.LoadFrom(Path.Combine(gameDirectory, "DNA.Common.dll"));
            original.Version = original.Game.GetName().Version.ToString();
            original.Modern = File.Exists(Path.Combine(gameDirectory, "Content", "ReachContent", "Terrain", "Textures.xnb"));

            var device = new DeviceService();
            var form = new System.Windows.Forms.Form();
            var parameters = new PresentationParameters
            {
                BackBufferWidth = 16, BackBufferHeight = 16, DeviceWindowHandle = form.Handle,
                IsFullScreen = false, PresentationInterval = PresentInterval.Immediate,
            };
            // GraphicsProfileManager: HiDef whenever the adapter supports it.
            original.Profile = "HiDef";
            try
            {
                if (forceReach) throw new NotSupportedException();
                device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.HiDef, parameters);
            }
            catch (Exception)
            {
                original.Profile = "Reach";
                device.GraphicsDevice = new GraphicsDevice(GraphicsAdapter.DefaultAdapter, GraphicsProfile.Reach, parameters);
            }
            original.Device = device.GraphicsDevice;
            var services = new ServiceTable();
            services.Services[typeof(IGraphicsDeviceService)] = device;

            Type gameType = original.T("DNA.CastleMinerZ.CastleMinerZGame");
            original.GameInstance = FormatterServices.GetUninitializedObject(gameType);
            GC.SuppressFinalize(original.GameInstance);
            R.SetStatic(gameType, "Instance", original.GameInstance);
            original.Content = new OwnedContent(services, Path.Combine(gameDirectory, "Content"),
                                                original.Profile + "Content");
            ((Game)original.GameInstance).Content = original.Content;
            foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
                if (field.FieldType == typeof(IGraphicsDeviceService)) field.SetValue(original.GameInstance, device);
            try { R.Set(original.GameInstance, "CurrentGameTime", new GameTime()); } catch (MissingMemberException) { }
            return original;
        }

        // Runs Player's own static initialisation (a static constructor in Classic,
        // Player.Init in Modern) and takes the body it loaded.
        public void LoadBody()
        {
            Type playerType = T("DNA.CastleMinerZ.Player");
            int before = Content.Log.Count;
            string failure = null;
            try
            {
                RuntimeHelpers.RunClassConstructor(playerType.TypeHandle);
                MethodInfo init = playerType.GetMethod("Init", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic,
                                                       null, Type.EmptyTypes, null);
                if (R.GetStatic(playerType, "ProxyModel") == null && init != null) R.Invoke(init, null, new object[0]);
            }
            catch (Exception error)
            {
                Exception inner = error.InnerException ?? error;
                failure = inner.GetType().Name + ": " + inner.Message;
            }
            for (int index = before; index < Content.Log.Count; ++index) PlayerContentLog.Add(Content.Log[index]);
            ProxyModel = (Model)R.GetStatic(playerType, "ProxyModel");
            ProxyModelAsset = PlayerContentLog.Where(line => line.StartsWith("Model Character")).Select(
                line => line.Substring(line.IndexOf("-> ", StringComparison.Ordinal) + 3)).LastOrDefault();
            if (ProxyModel == null)
                throw new InvalidOperationException("The game's Player initialisation did not load a body" +
                                                    (failure == null ? "" : " (" + failure + ")"));
        }

        // Loads every registered clip with the real ContentManager and performs
        // the real AvatarAnimationManager.RegisterAnimation calls.
        public void RegisterClips()
        {
            Registrations = RegistrationScanner.Extract(Game);
            if (Registrations.Count == 0)
                throw new InvalidOperationException("No clip registrations were found in the game executable");
            Type managerType = T("DNA.Avatars.AvatarAnimationManager");
            object manager = R.GetStatic(managerType, "Instance");
            Type clipType = T("DNA.Drawing.Animation.AnimationClip");
            Type boneEnum = T("DNA.Net.GamerServices.AvatarBone");
            MethodInfo load = typeof(ContentManager).GetMethod("Load").MakeGenericMethod(clipType);
            foreach (ClipRegistration registration in Registrations)
            {
                object clip = R.Invoke(load, Content, new object[] { registration.Asset });
                Clips[registration.Name] = clip;
                Func<int[], object> roots = values =>
                {
                    Array array = Array.CreateInstance(boneEnum, values.Length);
                    for (int index = 0; index < values.Length; ++index)
                        array.SetValue(Enum.ToObject(boneEnum, values[index]), index);
                    return array;
                };
                int parameterCount = 3 + (registration.Bones != null ? 1 : 0) + (registration.MaskedBones != null ? 1 : 0);
                MethodInfo register = managerType.GetMethods().First(method =>
                    method.Name == "RegisterAnimation" && method.GetParameters().Length == parameterCount &&
                    method.GetParameters()[2].ParameterType == typeof(bool));
                var arguments = new List<object> { registration.Name, clip, registration.Looping };
                if (registration.Bones != null) arguments.Add(roots(registration.Bones));
                if (registration.MaskedBones != null) arguments.Add(roots(registration.MaskedBones));
                R.Invoke(register, manager, arguments.ToArray());
            }
        }

        // The influence set the real manager stored for a registration (null = all).
        public bool[] Influence(string name)
        {
            object manager = R.GetStatic(T("DNA.Avatars.AvatarAnimationManager"), "Instance");
            var clips = (IDictionary)R.Get(manager, "_clips");
            return (bool[])R.Get(clips[name], "InfluencedBones");
        }
    }

    // One character, assembled with the statements of Player's constructor.
    internal sealed class Character
    {
        public Original Original;
        public object Avatar;
        public object Proxy;
        public object RightHand;
        public object Animations;
        public TimeSpan Total = TimeSpan.Zero;

        public static Character Create(Original original)
        {
            var character = new Character { Original = original };
            Type avatarType = original.T("DNA.Avatars.Avatar");
            Type descriptionType = original.T("DNA.Net.GamerServices.AvatarDescription");
            object description = Activator.CreateInstance(
                descriptionType, new object[] { (byte[])R.GetStatic(avatarType, "DefaultDescriptionData") });
            character.Avatar = Activator.CreateInstance(avatarType, new object[] { description });
            // Classic wraps the body in DNA.Avatars.AvatarModelEntity, Modern in its
            // own PlayerModelEntity; both are SkinnedModelEntity plus lighting.
            Type proxyType = original.Game.GetType("DNA.CastleMinerZ.PlayerModelEntity", false) ??
                             original.T("DNA.Avatars.AvatarModelEntity");
            character.Proxy = Activator.CreateInstance(proxyType, new object[] { original.ProxyModel });
            R.Set(character.Avatar, "ProxyModelEntity", character.Proxy);
            Type boneEnum = original.T("DNA.Net.GamerServices.AvatarBone");
            MethodInfo getPart = avatarType.GetMethod("GetAvatarPart");
            character.RightHand = getPart.Invoke(character.Avatar, new object[] { Enum.ToObject(boneEnum, 48) });
            // Asking for every part makes Avatar.UpdateParts compute the whole
            // attachment skeleton; the values of PropRight and Head do not change.
            for (int bone = 0; bone < 71; ++bone)
                getPart.Invoke(character.Avatar, new object[] { Enum.ToObject(boneEnum, bone) });
            character.Animations = R.Get(character.Avatar, "Animations");
            return character;
        }

        public object Play(string name, int channel, long blendTicks)
        {
            return R.Invoke(R.Method(Animations.GetType(), "Play", typeof(string), typeof(int), typeof(TimeSpan)),
                            Animations, new object[] { name, channel, TimeSpan.FromTicks(blendTicks) });
        }

        public void Clear(int channel, long blendTicks)
        {
            R.Invoke(R.Method(Animations.GetType(), "ClearAnimation", typeof(int), typeof(TimeSpan)), Animations,
                     new object[] { channel, TimeSpan.FromTicks(blendTicks) });
        }

        public object Active(int channel)
        {
            return R.Invoke(R.Method(Animations.GetType(), "GetAnimation", typeof(int)), Animations,
                            new object[] { channel });
        }

        public void Replay(int channel, long blendTicks)
        {
            object player = Active(channel);
            MethodInfo method = Animations.GetType().BaseType.GetMethod("PlayAnimation");
            R.Invoke(method, Animations, new object[] { channel, player, TimeSpan.FromTicks(blendTicks) });
        }

        // The real Entity.Update: Avatar.OnUpdate, then the body's own update.
        public void Update(long elapsedTicks)
        {
            TimeSpan elapsed = TimeSpan.FromTicks(elapsedTicks);
            Total += elapsed;
            MethodInfo update = Avatar.GetType().GetMethods().First(m => m.Name == "Update" && m.GetParameters().Length == 2);
            R.Invoke(update, Avatar, new object[] { Original.GameInstance, new GameTime(Total, elapsed) });
        }
    }

    internal sealed class HeldItem
    {
        public int Id;
        public string Name;
        public string ItemClass;
        public string PlayerMode;
        public string Entity;
        public int Children;
        public Matrix Hand;        // CreateEntity(ItemUse.Hand, local player).LocalToParent
        public Matrix HandRemote;  // the same for a remote player
    }

    internal static class HeldItems
    {
        public static List<HeldItem> Read(Original original, List<string> notes)
        {
            var result = new List<HeldItem>();
            Type itemType = original.T("DNA.CastleMinerZ.Inventory.InventoryItem");
            try { R.CallStatic(itemType, "Initalize", original.Content); }
            catch (Exception error)
            {
                notes.Add("InventoryItem.Initalize stopped early: " + error.GetType().Name + ": " + error.Message);
            }
            // Modern replaced the held entities' static constructors with explicit
            // Init() calls in SecondaryLoad; make the same calls.
            foreach (string entityName in new[] { "GunEntity", "TorchEntity" })
                foreach (Type type in original.Game.GetTypes().Where(candidate => candidate.Name == entityName))
                {
                    MethodInfo init = type.GetMethod("Init", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic,
                                                     null, Type.EmptyTypes, null);
                    if (init == null) continue;
                    try { R.Invoke(init, null, new object[0]); }
                    catch (Exception error) { notes.Add(entityName + ".Init failed: " + error.GetType().Name + ": " + error.Message); }
                }
            Type idType = original.T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
            object hand = Enum.Parse(original.T("DNA.CastleMinerZ.Inventory.ItemUse"), "Hand");
            MethodInfo getClass = itemType.GetMethod("GetClass", BindingFlags.Static | BindingFlags.Public);
            foreach (object id in Enum.GetValues(idType))
            {
                object itemClass;
                try { itemClass = getClass.Invoke(null, new[] { id }); }
                catch (Exception) { continue; }
                if (itemClass == null) continue;
                var item = new HeldItem
                {
                    Id = Convert.ToInt32(id, System.Globalization.CultureInfo.InvariantCulture), Name = id.ToString(),
                    ItemClass = itemClass.GetType().Name, PlayerMode = "", Entity = "",
                    Hand = Matrix.Identity, HandRemote = Matrix.Identity,
                };
                try { item.PlayerMode = R.Get(itemClass, "PlayerAnimationMode").ToString(); } catch (Exception) { }
                bool any = false;
                foreach (bool local in new[] { true, false })
                {
                    object entity;
                    try { entity = R.Invoke(itemClass.GetType().GetMethod("CreateEntity"), itemClass, new[] { hand, local }); }
                    catch (Exception error)
                    {
                        notes.Add("item " + item.Id + " " + item.Name + " CreateEntity(Hand, local=" + local + ") failed: " +
                                  error.GetType().Name + ": " + error.Message);
                        continue;
                    }
                    if (entity == null) continue;
                    any = true;
                    Matrix matrix = (Matrix)R.Get(entity, "LocalToParent");
                    if (local)
                    {
                        item.Hand = matrix;
                        item.Entity = entity.GetType().Name;
                        item.Children = Convert.ToInt32(R.Get(R.Get(entity, "Children"), "Count"),
                                                        System.Globalization.CultureInfo.InvariantCulture);
                    }
                    else item.HandRemote = matrix;
                }
                if (any) result.Add(item);
            }
            return result;
        }
    }

#if OPENMINERZ_CONTENT_BRIDGE
    // Writes DerivedData/character.omrc, the runtime's retail character. Compiled only
    // into the importer: the probe shares everything above and none of this.
    internal static class RetailCharacterExporter
    {
        public const uint Magic = 0x43524D4FU; // OMRC
        public const int FormatVersion = 1;

        private static void W(BinaryWriter writer, Matrix m)
        {
            writer.Write(m.M11); writer.Write(m.M12); writer.Write(m.M13); writer.Write(m.M14);
            writer.Write(m.M21); writer.Write(m.M22); writer.Write(m.M23); writer.Write(m.M24);
            writer.Write(m.M31); writer.Write(m.M32); writer.Write(m.M33); writer.Write(m.M34);
            writer.Write(m.M41); writer.Write(m.M42); writer.Write(m.M43); writer.Write(m.M44);
        }

        private static void W(BinaryWriter writer, Vector3 v) { writer.Write(v.X); writer.Write(v.Y); writer.Write(v.Z); }

        private static void W(BinaryWriter writer, Quaternion q)
        {
            writer.Write(q.X); writer.Write(q.Y); writer.Write(q.Z); writer.Write(q.W);
        }

        public static int Run(string gameDirectory, string outputDirectory)
        {
            Original original;
            try { original = Original.Create(gameDirectory, false); }
            catch (Exception error)
            {
                Console.Error.WriteLine("Retail character: no graphics device for the original content pipeline: " + error.Message);
                return 1;
            }
            original.LoadBody();
            original.RegisterClips();
            Export(original, outputDirectory);
            return 0;
        }

        public static void Export(Original original, string outputDirectory)
        {
            Directory.CreateDirectory(outputDirectory);
            var manifest = new List<string>();
            Character character = Character.Create(original);
            // One real, empty update: the attachment skeleton is built, the body
            // receives its root adjustment and the eye offset reaches its final value.
            character.Update(0);

            Type avatarType = original.T("DNA.Avatars.Avatar");
            var boneNames = ((IEnumerable)R.GetStatic(avatarType, "BoneNames")).Cast<string>().ToList();
            var parents = ((IEnumerable)R.GetStatic(avatarType, "DefaultParentBones")).Cast<int>().ToList();
            var lookup = (IDictionary)R.GetStatic(avatarType, "boneNameLookup");
            var bindPose = ((IEnumerable)R.Get(character.Avatar, "BindPose")).Cast<Matrix>().ToList();
            var avatarBones = ((IEnumerable)R.Get(character.Avatar, "Skeleton")).Cast<object>().ToList();
            var proxyBones = ((IEnumerable)R.Get(character.Proxy, "Skeleton")).Cast<object>().ToList();
            var proxyBind = ((IEnumerable)R.Get(character.Proxy, "BindPose")).Cast<Matrix>().ToList();
            var proxyDefault = (Matrix[])R.Get(character.Proxy, "DefaultPose");
            object skinning = original.ProxyModel.Tag;
            var inverseBind = (Matrix[])skinning.GetType().GetProperty("InverseBindPose").GetValue(skinning, null);
            if (avatarBones.Count != 71 || bindPose.Count != 71 || proxyBones.Count != inverseBind.Length)
                throw new InvalidDataException("Unexpected retail character skeleton");

            // Avatar.OnUpdate: body root = attachment root * (a constant matrix).
            Matrix attachmentRoot = (Matrix)R.Get(avatarBones[0], "Transform");
            Matrix rootAdjust = Matrix.Invert(attachmentRoot) * proxyDefault[0];
            Matrix eyeToHead = (Matrix)R.Get(character.Avatar, "eyeToHead");

            var notes = new List<string>();
            List<HeldItem> items;
            try { items = HeldItems.Read(original, notes); }
            catch (Exception error) { items = new List<HeldItem>(); notes.Add("held items unavailable: " + error.Message); }

            string path = Path.Combine(outputDirectory, "character.omrc");
            using (FileStream stream = File.Create(path))
            using (var writer = new BinaryWriter(stream, Encoding.UTF8))
            {
                writer.Write(Magic);
                writer.Write(FormatVersion);
                writer.Write(original.Modern ? "modern" : "classic");
                writer.Write(original.Version);
                writer.Write(original.Profile);
                writer.Write(original.ProxyModelAsset ?? "");

                writer.Write(avatarBones.Count);
                for (int bone = 0; bone < avatarBones.Count; ++bone)
                {
                    string native = lookup.Keys.Cast<string>().First(key => (int)lookup[key] == bone);
                    writer.Write(boneNames[bone]);
                    writer.Write(native);
                    writer.Write(parents[bone]);
                    W(writer, bindPose[bone]);
                    W(writer, (Vector3)R.Get(avatarBones[bone], "Translation"));
                    W(writer, (Quaternion)R.Get(avatarBones[bone], "Rotation"));
                    W(writer, (Vector3)R.Get(avatarBones[bone], "Scale"));
                }

                writer.Write(proxyBones.Count);
                for (int bone = 0; bone < proxyBones.Count; ++bone)
                {
                    string name = (string)R.Get(proxyBones[bone], "Name");
                    object parent = R.Get(proxyBones[bone], "Parent");
                    // Avatar.OnUpdate: bone 0 takes attachment bone 0; every other
                    // body bone is looked up by its name.
                    int source = bone == 0 ? 0 : (int)lookup[name];
                    writer.Write(name);
                    writer.Write(parent == null ? -1 : (int)R.Get(parent, "Index"));
                    writer.Write(source);
                    W(writer, proxyBind[bone]);
                    W(writer, inverseBind[bone]);
                }
                W(writer, rootAdjust);
                W(writer, eyeToHead);

                int partCount = original.ProxyModel.Meshes.Sum(mesh => mesh.MeshParts.Count);
                writer.Write(partCount);
                int exported = 0;
                foreach (ModelMesh mesh in original.ProxyModel.Meshes)
                    foreach (ModelMeshPart part in mesh.MeshParts)
                        WritePart(writer, mesh, part, outputDirectory, exported++, manifest);

                // Distinct clip assets, keys exactly as AnimationClip holds them.
                var clipIndex = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
                var clipOrder = new List<ClipRegistration>();
                foreach (ClipRegistration registration in original.Registrations)
                    if (!clipIndex.ContainsKey(registration.Asset))
                    {
                        clipIndex[registration.Asset] = clipOrder.Count;
                        clipOrder.Add(registration);
                    }
                writer.Write(clipOrder.Count);
                foreach (ClipRegistration registration in clipOrder)
                {
                    object clip = original.Clips[registration.Name];
                    var positions = (Vector3[][])R.Get(clip, "_positions");
                    var rotations = (Quaternion[][])R.Get(clip, "_rotations");
                    var scales = (Vector3[][])R.Get(clip, "_scales");
                    if (positions.Length != 71)
                        throw new InvalidDataException(registration.Asset + " is not a 71-bone character clip");
                    writer.Write(registration.Asset);
                    writer.Write((int)R.Get(clip, "_animationFrameRate"));
                    writer.Write(((TimeSpan)R.Get(clip, "Duration")).Ticks);
                    writer.Write(positions.Length);
                    for (int bone = 0; bone < positions.Length; ++bone)
                    {
                        writer.Write(positions[bone].Length);
                        foreach (Vector3 value in positions[bone]) W(writer, value);
                        writer.Write(rotations[bone].Length);
                        foreach (Quaternion value in rotations[bone]) W(writer, value);
                        writer.Write(scales[bone].Length);
                        foreach (Vector3 value in scales[bone]) W(writer, value);
                    }
                }

                writer.Write(original.Registrations.Count);
                foreach (ClipRegistration registration in original.Registrations)
                {
                    bool[] influence = original.Influence(registration.Name);
                    writer.Write(registration.Name);
                    writer.Write(clipIndex[registration.Asset]);
                    writer.Write(registration.Looping);
                    for (int bone = 0; bone < 71; ++bone)
                        writer.Write(influence == null || influence[bone]);
                    manifest.Add("clip " + registration.Name + "|" + original.Content.Resolve(registration.Asset) + "|" +
                                 (registration.Looping ? "loop" : "once") + "|" +
                                 (influence == null ? 71 : influence.Count(flag => flag)) + " bones");
                }

                writer.Write(items.Count);
                foreach (HeldItem item in items)
                {
                    writer.Write(item.Id);
                    writer.Write(item.Name);
                    writer.Write(item.PlayerMode);
                    writer.Write(item.Entity);
                    W(writer, item.Hand);
                    W(writer, item.HandRemote);
                }
            }

            manifest.Insert(0, "format=OpenMinerZ retail character " + FormatVersion);
            manifest.Insert(1, "game=" + original.Version + " generation=" + (original.Modern ? "modern" : "classic") +
                               " profile=" + original.Profile + " body=" + (original.ProxyModelAsset ?? "?"));
            manifest.Insert(2, "bones=" + proxyBones.Count + " attachmentBones=" + avatarBones.Count + " clips=" +
                               original.Registrations.Count + " heldItems=" + items.Count);
            foreach (string line in original.PlayerContentLog) manifest.Add("load " + line);
            foreach (string note in notes) manifest.Add("note " + note);
            File.WriteAllLines(Path.Combine(outputDirectory, "character.omrc.txt"), manifest);
            Console.WriteLine("Retail character: " + path + " (" + proxyBones.Count + " bones, " +
                              original.Registrations.Count + " clips, " + items.Count + " held items, " +
                              original.Profile + " " + (original.ProxyModelAsset ?? "") + ")");
        }

        private static void WritePart(BinaryWriter writer, ModelMesh mesh, ModelMeshPart part, string outputDirectory,
                                      int index, List<string> manifest)
        {
            Texture2D diffuse = null;
            var described = new List<string>();
            foreach (EffectParameter parameter in part.Effect.Parameters)
            {
                if (parameter.ParameterType != EffectParameterType.Texture2D &&
                    parameter.ParameterType != EffectParameterType.Texture)
                    continue;
                Texture2D texture;
                try { texture = parameter.GetValueTexture2D(); }
                catch (Exception) { continue; }
                if (texture == null) continue;
                described.Add(parameter.Name + "=" + texture.Width + "x" + texture.Height);
                string name = parameter.Name.ToLowerInvariant();
                bool auxiliary = name.Contains("normal") || name.Contains("bump") || name.Contains("spec") ||
                                 name.Contains("material") || name.Contains("reflect") || name.Contains("metal");
                if (diffuse == null && !auxiliary) diffuse = texture;
            }
            writer.Write(mesh.Name ?? "");
            if (diffuse == null)
            {
                writer.Write(0);
                writer.Write(0);
            }
            else
            {
                OpenMinerZ.ContentBridge.Pixel[] pixels = OpenMinerZ.ContentBridge.TextureDecoder.Read(diffuse);
                OpenMinerZ.ContentBridge.BitmapWriter.Write(
                    Path.Combine(outputDirectory, "character_part_" + index + ".bmp"), pixels, diffuse.Width, diffuse.Height);
                writer.Write(diffuse.Width);
                writer.Write(diffuse.Height);
                foreach (OpenMinerZ.ContentBridge.Pixel pixel in pixels)
                {
                    writer.Write(pixel.Red); writer.Write(pixel.Green); writer.Write(pixel.Blue); writer.Write(pixel.Alpha);
                }
            }

            VertexDeclaration declaration = part.VertexBuffer.VertexDeclaration;
            int stride = declaration.VertexStride;
            byte[] bytes = new byte[part.VertexBuffer.VertexCount * stride];
            part.VertexBuffer.GetData(bytes);
            Func<VertexElementUsage, VertexElement?> find = usage =>
            {
                foreach (VertexElement element in declaration.GetVertexElements())
                    if (element.VertexElementUsage == usage && element.UsageIndex == 0) return element;
                return null;
            };
            VertexElement position = find(VertexElementUsage.Position).Value;
            VertexElement normal = find(VertexElementUsage.Normal).Value;
            VertexElement uv = find(VertexElementUsage.TextureCoordinate).Value;
            VertexElement blendIndices = find(VertexElementUsage.BlendIndices).Value;
            VertexElement blendWeights = find(VertexElementUsage.BlendWeight).Value;
            VertexElement? color = find(VertexElementUsage.Color);
            if (blendIndices.VertexElementFormat != VertexElementFormat.Byte4 ||
                blendWeights.VertexElementFormat != VertexElementFormat.Vector4)
                throw new NotSupportedException("Unsupported skinning vertex format on " + mesh.Name);

            writer.Write(part.NumVertices);
            Vector3 minimum = new Vector3(float.MaxValue), maximum = new Vector3(float.MinValue);
            bool whiteColors = true;
            for (int vertex = 0; vertex < part.NumVertices; ++vertex)
            {
                int offset = (part.VertexOffset + vertex) * stride;
                for (int component = 0; component < 3; ++component)
                    writer.Write(BitConverter.ToSingle(bytes, offset + position.Offset + component * 4));
                Vector3 point = new Vector3(BitConverter.ToSingle(bytes, offset + position.Offset),
                                            BitConverter.ToSingle(bytes, offset + position.Offset + 4),
                                            BitConverter.ToSingle(bytes, offset + position.Offset + 8));
                minimum = Vector3.Min(minimum, point);
                maximum = Vector3.Max(maximum, point);
                for (int component = 0; component < 3; ++component)
                    writer.Write(BitConverter.ToSingle(bytes, offset + normal.Offset + component * 4));
                for (int component = 0; component < 2; ++component)
                    writer.Write(BitConverter.ToSingle(bytes, offset + uv.Offset + component * 4));
                writer.Write(bytes, offset + blendIndices.Offset, 4);
                for (int component = 0; component < 4; ++component)
                    writer.Write(BitConverter.ToSingle(bytes, offset + blendWeights.Offset + component * 4));
                if (color.HasValue)
                {
                    writer.Write(bytes, offset + color.Value.Offset, 4);
                    for (int component = 0; component < 4; ++component)
                        whiteColors = whiteColors && bytes[offset + color.Value.Offset + component] == 255;
                }
                else
                {
                    writer.Write((byte)255); writer.Write((byte)255); writer.Write((byte)255); writer.Write((byte)255);
                }
            }
            int indexCount = part.PrimitiveCount * 3;
            writer.Write(indexCount);
            if (part.IndexBuffer.IndexElementSize == IndexElementSize.SixteenBits)
            {
                ushort[] indices = new ushort[part.IndexBuffer.IndexCount];
                part.IndexBuffer.GetData(indices);
                for (int element = 0; element < indexCount; ++element) writer.Write((uint)indices[part.StartIndex + element]);
            }
            else
            {
                uint[] indices = new uint[part.IndexBuffer.IndexCount];
                part.IndexBuffer.GetData(indices);
                for (int element = 0; element < indexCount; ++element) writer.Write(indices[part.StartIndex + element]);
            }
            manifest.Add("mesh " + mesh.Name + " vertices=" + part.NumVertices + " triangles=" + part.PrimitiveCount +
                         " effect=" + part.Effect.GetType().Name + "/" + part.Effect.CurrentTechnique.Name + " " +
                         string.Join(" ", described) + " bounds=" + minimum + ".." + maximum + " vertexColorsAllWhite=" +
                         whiteColors);
        }
    }
#endif
}
