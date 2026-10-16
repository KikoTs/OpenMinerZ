using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Audio;
using Microsoft.Xna.Framework.Content;

// The original animation timeline, recorded frame by frame.
//
// A real local Player and a real remote Player each own a real Avatar whose
// AvatarAnimationCollection plays the clips the shipped game registered, loaded
// from the owned install. Scripted input drives them through the original
// code in the order DNAGame.Update runs it:
//
//   ProcessNetworkMessages   the local echo queue, then the remote player's
//                            PlayerUpdateMessage (RecieveData + Apply)
//   SendNetworkUpdates       the original round-robin, which decides on which
//                            frames a PlayerUpdateMessage is written at all
//   InGameHUD.OnPlayerInput  Player.ProcessInput (-> UpdateAnimation), the
//                            inventory timers, the slot keys, then the held
//                            item's own ProcessInput
//   ScreenManager.Update     InGameHUD.EquipActiveItem, the grenade cook
//                            clock, footsteps, and Avatar.OnUpdate's
//                            LayeredAnimationPlayer.Update
//
// Executed for real (original IL): Player.ProcessInput, Player.UpdateAnimation,
// Player.SetupAnimation, Player.FinishReload, Player.Equip, Player.UpdateAudio,
// every InventoryItem/Gun/Block/Grenade/RocketLauncher ProcessInput,
// InventoryItem.Update, PlayerInventory.Update/RemoveEmptyItems,
// InGameHUD.Shoot and the messages it sends, GunInventoryItem.Reload,
// PlayerUpdateMessage.Send/SendData/RecieveData/Apply,
// CastleMinerZGame.SendNetworkUpdates, AvatarAnimationManager,
// AvatarAnimationCollection.Play, AnimBlender and AnimationPlayer.
// Redirected, recording only: SoundManager.PlayInstance (cue names),
// InGameHUD.Dig/Melee/MeleePlayer/Build/SetFuseForExplosive.
// Transcribed from the original because the method around them is a device:
// the order of InGameHUD.OnPlayerInput, the slot keys, EquipActiveItem,
// KillPlayer/RefreshPlayer's flag writes, Player.OnUpdate's cook clock,
// Player.PutItemInHand's `_playerMode = class.PlayerAnimationMode`, and
// Player.ProcessGrenadeMessage's `Play("Grenade_Reset", 3, 0)`.
internal struct InputSample
{
    public float Strafe, Forward;
    public bool Use, Shoulder, Reload, Jump;
    public int Slot;
    public bool Kill;
}

internal sealed class Scenario
{
    public string Name;
    public int[] Tray = new int[0];
    public int[] Counts = new int[0];
    public int[] Rounds = new int[0];
    public int Ammo = 400;
    public long Ticks = 166667;
    public float Seconds = 4f;
    // What the construction ray reports: "none", "block" (rock) or "zombie".
    public string Target = "none";
    public Func<float, InputSample> Input;
}

internal static class Stage
{
    public static object Local, Remote, Hud, Inventory, Mapping;
    private static Type playerType, hudType, itemType, mappingType, collectionType, blenderType;
    private static MethodInfo processInput, resolve, updateAudio;
    private static object lastItem;
    private static readonly List<byte[]> remoteInbox = new List<byte[]>();
    private static readonly List<string> remoteNotes = new List<string>();
    private static bool cadenceExecuted;
    public static readonly SortedDictionary<string, string> Clips = new SortedDictionary<string, string>(StringComparer.Ordinal);

    // ------------------------------------------------------------ redirected

    private static object RecordCue3D(object self, string name, object emitter)
    {
        Rig.Events.Add("cue:" + name);
        return null;
    }

    private static object RecordCue(object self, string name)
    {
        Rig.Events.Add("cue:" + name);
        return null;
    }

    private static void RecordDig(object self, object tool, bool effective)
    {
        Rig.Events.Add(effective ? "dig:1" : "dig:0");
    }

    private static void RecordMelee(object self, object tool)
    {
        Rig.Events.Add("melee");
    }

    private static void RecordMeleePlayer(object self, object tool, object player)
    {
        Rig.Events.Add("meleeplayer");
    }

    private static bool RecordBuildClassic(object self, object item)
    {
        Rig.Events.Add("build");
        return true;
    }

    private struct Int3
    {
        public int X, Y, Z;
    }

    // 1.9.9.8: IntVector3 Build(BlockInventoryItem, bool validateOnly). An
    // instance method returning a struct of IntVector3's layout takes `this`
    // and the hidden result pointer in the same registers as the original.
    private sealed class BuildShim
    {
        public Int3 Build(object item, bool validateOnly)
        {
            if (!validateOnly) Rig.Events.Add("build");
            return new Int3 { X = 3, Y = 1, Z = 0 };
        }
    }

    private static void RecordFuse(object self, Int3 cell, int kind)
    {
        Rig.Events.Add("fuse");
    }

    private static MethodInfo Mine(string name)
    {
        return typeof(Stage).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic);
    }

    // ----------------------------------------------------------- IL scanning

    private struct Instruction
    {
        public OpCode Code;
        public int Operand;
    }

    private static List<Instruction> Decode(MethodBase method)
    {
        var single = new OpCode[256];
        var pair = new OpCode[256];
        foreach (FieldInfo field in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            var code = (OpCode)field.GetValue(null);
            if (code.Size == 1) single[code.Value & 0xFF] = code;
            else pair[code.Value & 0xFF] = code;
        }
        var result = new List<Instruction>();
        byte[] il = method.GetMethodBody().GetILAsByteArray();
        int offset = 0;
        while (offset < il.Length)
        {
            OpCode code = il[offset] == 0xFE ? pair[il[offset + 1]] : single[il[offset]];
            if (code.Size == 0) throw new InvalidOperationException("Undecodable IL in " + method.Name);
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
            if (size == 4 && code.OperandType != OperandType.InlineSwitch) instruction.Operand = BitConverter.ToInt32(il, offset);
            else if (size == 1) instruction.Operand = (sbyte)il[offset];
            result.Add(instruction);
            offset += size;
        }
        return result;
    }

    private static bool Constant(Instruction instruction, out int value)
    {
        OpCode code = instruction.Code;
        value = 0;
        if (code == OpCodes.Ldc_I4 || code == OpCodes.Ldc_I4_S) { value = instruction.Operand; return true; }
        if (code == OpCodes.Ldc_I4_M1) { value = -1; return true; }
        OpCode[] small = { OpCodes.Ldc_I4_0, OpCodes.Ldc_I4_1, OpCodes.Ldc_I4_2, OpCodes.Ldc_I4_3, OpCodes.Ldc_I4_4,
                           OpCodes.Ldc_I4_5, OpCodes.Ldc_I4_6, OpCodes.Ldc_I4_7, OpCodes.Ldc_I4_8 };
        for (int index = 0; index < small.Length; ++index)
            if (code == small[index]) { value = index; return true; }
        return false;
    }

    // The shipped game registers its avatar clips inside CastleMinerZGame.
    // SecondaryLoad, between the sound bank, the fonts and the front end. The
    // method cannot run offline, so its RegisterAnimation call sites are read
    // out of its IL and replayed, in order, through the real
    // AvatarAnimationManager with the real clips from the owned install.
    public static void RegisterAnimations()
    {
        Type gameType = Rig.T("DNA.CastleMinerZ.CastleMinerZGame");
        Type managerType = Rig.T("DNA.Avatars.AvatarAnimationManager");
        object manager = R.GetStatic(managerType, "Instance");
        Type clipType = Rig.T("DNA.Drawing.Animation.AnimationClip");
        var content = (ContentManager)((Game)Rig.GameInstance).Content;
        MethodInfo load = typeof(ContentManager).GetMethod("Load").MakeGenericMethod(clipType);
        MethodBase source = gameType.GetMethods(R.All).First(method => method.Name == "SecondaryLoad");
        List<Instruction> code = Decode(source);
        var strings = new List<string>();
        var constants = new List<int>();
        var arrays = new List<Array>();
        Array open = null;
        Type element = null;
        int looping = -1;
        bool loaded = false;
        int registered = 0;
        for (int index = 0; index < code.Count; ++index)
        {
            OpCode op = code[index].Code;
            int constant;
            if (op == OpCodes.Ldsfld)
            {
                FieldInfo field = source.Module.ResolveField(code[index].Operand);
                if (field.DeclaringType == managerType && field.Name == "Instance")
                {
                    strings.Clear(); constants.Clear(); arrays.Clear(); open = null; looping = -1; loaded = false;
                }
            }
            else if (op == OpCodes.Ldstr) strings.Add(source.Module.ResolveString(code[index].Operand));
            else if (Constant(code[index], out constant))
            {
                constants.Add(constant);
                if (loaded && looping < 0) looping = constant;
            }
            else if (op == OpCodes.Newarr)
            {
                element = source.Module.ResolveType(code[index].Operand);
                open = Array.CreateInstance(element, constants[constants.Count - 1]);
                arrays.Add(open);
            }
            else if (op == OpCodes.Stelem_I4 || op == OpCodes.Stelem_I1 || op == OpCodes.Stelem_I2 || op == OpCodes.Stelem)
            {
                open.SetValue(Enum.ToObject(element, constants[constants.Count - 1]), constants[constants.Count - 2]);
            }
            else if (op == OpCodes.Call || op == OpCodes.Callvirt)
            {
                MethodBase called = source.Module.ResolveMethod(code[index].Operand);
                if (called.Name == "Load" && strings.Count == 2) loaded = true;
                if (called.DeclaringType != managerType || called.Name != "RegisterAnimation") continue;
                string name = strings[0], asset = strings[1];
                object clip = R.Invoke(load, content, new object[] { asset });
                var arguments = new List<object> { name, clip, looping != 0 };
                arguments.AddRange(arrays.Cast<object>());
                R.Invoke(called, manager, arguments.ToArray());
                ++registered;
                long ticks = ((TimeSpan)R.Get(clip, "Duration")).Ticks;
                string bones = arrays.Count == 0 ? "all" : string.Join("-", arrays.Select(array =>
                    string.Join("+", array.Cast<object>().Select(value => value.ToString()))));
                Clips[name] = "CLIP " + name + " asset=" + asset.Replace(' ', '_') + " ticks=" + ticks + " loop=" +
                              (looping != 0 ? 1 : 0) + " bones=" + bones + " keys=" + (int)R.Get(clip, "BoneCount");
            }
        }
        if (registered < 100) throw new InvalidDataException("Only " + registered + " RegisterAnimation call sites decoded");
    }

    // --------------------------------------------------------------- content

    // Real animation clips; inert stand-ins for everything a device would draw.
    private sealed class ClipContent : ContentManager
    {
        private readonly Type clipType;
        private readonly string profile;

        public ClipContent(IServiceProvider services, string root, string profile, Type clipType) : base(services, root)
        {
            this.clipType = clipType;
            this.profile = profile;
        }

        public override T Load<T>(string assetName)
        {
            if (typeof(T) != clipType) return (T)R.Inert(typeof(T));
            string profiled = Path.Combine(profile, assetName);
            return File.Exists(Path.Combine(RootDirectory, profiled + ".xnb")) ? base.Load<T>(profiled) : base.Load<T>(assetName);
        }
    }

    // ------------------------------------------------------------------ setup

    private static object Degrees(float value)
    {
        return R.CallStatic(Rig.T("DNA.Angle"), "FromDegrees", value);
    }

    private static object NewAvatar()
    {
        Type avatarType = Rig.T("DNA.Avatars.Avatar");
        object avatar = R.Inert(avatarType);
        Rig.T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                   null, Type.EmptyTypes, null).Invoke(avatar, null);
        R.Set(avatar, "_animations", Activator.CreateInstance(collectionType, avatar));
        R.Set(avatar, "EyePointCamera", Activator.CreateInstance(R.Field(avatarType, "EyePointCamera").FieldType));
        FieldInfo description = R.Field(avatarType, "_avatarDescription");
        if (description.FieldType.IsClass) description.SetValue(avatar, R.Inert(description.FieldType));
        return avatar;
    }

    // What Player's field initializers and constructor leave on a new player.
    private static void Prepare(object player, bool local)
    {
        R.Set(player, "Collider", true);
        Type noMove = playerType.GetNestedType("NoMovePhysics", BindingFlags.Public | BindingFlags.NonPublic);
        R.Set(player, "Physics", Activator.CreateInstance(noMove, player));
        R.Set(player, "DefaultFOV", Degrees(73f));
        R.Set(player, "DefaultAvatarFOV", Degrees(90f));
        R.Set(player, "ShoulderedAvatarFOV", Degrees(45f));
        R.Set(player, "recoilDecay", Degrees(30f));
        R.Set(player, "GunEyePointCamera", Activator.CreateInstance(R.Field(playerType, "GunEyePointCamera").FieldType));
        R.Set(player, "_playerMode", Enum.Parse(R.Field(playerType, "_playerMode").FieldType, "Fist"));
        R.Set(player, "underWaterSpeed", 1.894f);
        R.Set(player, "IsActive", true);
        R.Set(player, "_walkSpeed", 1f);
        R.Set(player, "SoundEmitter", new AudioEmitter());
        R.Set(player, "_footStepTimer", Activator.CreateInstance(R.Field(playerType, "_footStepTimer").FieldType, TimeSpan.FromSeconds(0.5)));
        R.Set(player, "_fastRand", Activator.CreateInstance(R.Field(playerType, "_fastRand").FieldType));
        R.Set(player, "rand", new Random(1));
        if (R.Has(playerType, "_sprintMultiplier")) R.Set(player, "_sprintMultiplier", 2f);
        if (R.Has(playerType, "JumpCountLimit")) R.Set(player, "JumpCountLimit", 2);
        if (R.Has(playerType, "m_baseJumpCost")) R.Set(player, "m_baseJumpCost", 0.25f);
        if (R.Has(playerType, "m_baseSprintCost")) R.Set(player, "m_baseSprintCost", 0.6f);
        object avatar = NewAvatar();
        R.Set(player, "Avatar", avatar);
        // Player..ctor adds the avatar as a child (third person); the local
        // player is then moved to the first-person scene by FPSMode = true.
        if (!local) R.Call(R.Get(player, "Children"), "Add", avatar);
        R.Call(player, "SetupAnimation");
    }

    public static void Setup()
    {
        playerType = Rig.T("DNA.CastleMinerZ.Player");
        hudType = Rig.T("DNA.CastleMinerZ.UI.InGameHUD");
        itemType = Rig.T("DNA.CastleMinerZ.Inventory.InventoryItem");
        collectionType = Rig.T("DNA.Avatars.AvatarAnimationCollection");
        blenderType = Rig.T("DNA.Drawing.Animation.AnimBlender");
        Type gameType = Rig.GameInstance.GetType();

        // Clips load for real; models, textures and effects stay inert.
        var game = (Game)Rig.GameInstance;
        var services = new ServiceTable();
        foreach (FieldInfo field in typeof(Game).GetFields(BindingFlags.Instance | BindingFlags.NonPublic))
            if (field.FieldType == typeof(Microsoft.Xna.Framework.Graphics.IGraphicsDeviceService))
                services.Services[typeof(Microsoft.Xna.Framework.Graphics.IGraphicsDeviceService)] = field.GetValue(game);
        string root = Path.Combine(Rig.GameDirectory, "Content");
        game.Content = new ClipContent(services, root, "HiDefContent", Rig.T("DNA.Drawing.Animation.AnimationClip"));

        Type sound = Rig.T("DNA.Audio.SoundManager");
        Rig.Redirect(sound.GetMethods(R.All).First(method => method.Name == "PlayInstance" && method.GetParameters().Length == 2),
                     Mine("RecordCue3D"));
        Rig.Redirect(sound.GetMethods(R.All).First(method => method.Name == "PlayInstance" && method.GetParameters().Length == 1),
                     Mine("RecordCue"));
        RegisterAnimations();

        R.Set(Rig.GameInstance, "_controllerMapping", Activator.CreateInstance(R.Field(gameType, "_controllerMapping").FieldType));
        R.Set(Rig.GameInstance, "Listener", new AudioListener());
        // The host's five-second TimeOfDayMessage needs the sky; it never comes due here.
        R.Set(Rig.GameInstance, "_worldUpdateTimer", Activator.CreateInstance(R.Field(gameType, "_worldUpdateTimer").FieldType, TimeSpan.FromDays(30.0)));
        object stats = Rig.Stats;
        R.Set(stats, "AllItemStats", Activator.CreateInstance(R.Field(stats.GetType(), "AllItemStats").FieldType));
        R.CallStatic(Rig.T("DNA.CastleMinerZ.MainThreadMessageSender"), "Init");
        Type messages = Rig.T("DNA.CastleMinerZ.GameMessageManager");
        if (R.GetStatic(messages, "Instance") == null) Activator.CreateInstance(messages);

        // Every item class constructor: PlayerAnimationMode, cooldowns, gun data.
        R.CallStatic(itemType, "Initalize", game.Content);

        Hud = R.Inert(hudType);
        R.SetStatic(hudType, "Instance", Hud);
        R.Set(Hud, "_game", Rig.GameInstance);
        R.Set(Hud, "ConstructionProbe", Activator.CreateInstance(R.Field(hudType, "ConstructionProbe").FieldType));
        Rig.Redirect(hudType.GetMethods(R.All).First(method => method.Name == "Dig" && method.GetParameters().Length == 2), Mine("RecordDig"));
        Rig.Redirect(hudType.GetMethods(R.All).First(method => method.Name == "Melee" && method.GetParameters().Length == 1), Mine("RecordMelee"));
        Rig.Redirect(hudType.GetMethods(R.All).First(method => method.Name == "MeleePlayer"), Mine("RecordMeleePlayer"));
        MethodInfo build = hudType.GetMethods(R.All).First(method => method.Name == "Build");
        Rig.Redirect(build, build.GetParameters().Length == 1 ? Mine("RecordBuildClassic") : typeof(BuildShim).GetMethod("Build"));
        Rig.Redirect(hudType.GetMethods(R.All).First(method => method.Name == "SetFuseForExplosive"), Mine("RecordFuse"));

        Type screenType = R.Field(gameType, "GameScreen").FieldType;
        object screen = R.Inert(screenType);
        R.Set(screen, "_inGameUI", Hud);
        R.Set(screen, "CrackBox", R.Inert(R.Field(screenType, "CrackBox").FieldType));
        object scene = R.Inert(R.Field(screenType, "_fpsScene").FieldType);
        Rig.T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                   null, Type.EmptyTypes, null).Invoke(scene, null);
        R.Set(screen, "_fpsScene", scene);
        R.Set(Rig.GameInstance, "GameScreen", screen);
        if (R.Has(hudType, "PlayerStamina"))
        {
            R.Set(Hud, "PlayerStamina", 1f);
            R.Set(Hud, "StaminaRecoverRate", 0.25f);
            foreach (string timer in new[] { "StaminaRecoverTimer", "StaminaBlockTimer" })
            {
                object value = Activator.CreateInstance(R.Field(hudType, timer).FieldType,
                                                        TimeSpan.FromSeconds(timer == "StaminaRecoverTimer" ? 1.0 : 2.0));
                R.Call(value, "Update", TimeSpan.FromSeconds(10.0));
                R.Set(Hud, timer, value);
            }
        }

        // Player.ResolveCollsion asks the enemy manager who is in the way: nobody.
        Type managerType = Rig.T("DNA.CastleMinerZ.AI.EnemyManager");
        object enemies = R.Inert(managerType);
        Rig.T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                   null, Type.EmptyTypes, null).Invoke(enemies, null);
        R.Set(enemies, "_enemies", Activator.CreateInstance(R.Field(managerType, "_enemies").FieldType));
        R.SetStatic(managerType, "Instance", enemies);

        Local = Rig.LocalPlayer;
        Prepare(Local, true);
        Remote = Rig.AddRemote(9, "RemoteStock");
        Prepare(Remote, false);
        processInput = playerType.GetMethod("ProcessInput", BindingFlags.Instance | BindingFlags.Public);
        resolve = playerType.GetMethod("ResolveCollsion", BindingFlags.Instance | BindingFlags.Public);
        updateAudio = R.Method(playerType, "UpdateAudio", 1);
        mappingType = Rig.T("DNA.CastleMinerZ.CastleMinerZControllerMapping");
    }

    // ----------------------------------------------------------------- tables

    public static IEnumerable<string> ItemTable()
    {
        var catalog = (IDictionary)R.GetStatic(itemType, "AllItems");
        Type gun = Rig.T("DNA.CastleMinerZ.Inventory.GunInventoryItemClass");
        var entries = new List<DictionaryEntry>();
        foreach (DictionaryEntry entry in catalog) entries.Add(entry);
        foreach (DictionaryEntry entry in entries.OrderBy(pair => Convert.ToInt32(pair.Key)))
        {
            object cls = entry.Value;
            string line = "ITEM " + Convert.ToInt32(entry.Key) + " name=" + entry.Key + " mode=" +
                          Convert.ToInt32(R.Get(cls, "PlayerAnimationMode")) + " cooldown=" + ((TimeSpan)R.Get(cls, "CoolDownTime")).Ticks +
                          " melee=" + ((bool)R.Get(cls, "IsMeleeWeapon") ? 1 : 0) + " class=" + cls.GetType().Name;
            if (gun.IsInstanceOfType(cls))
                line += " reload=" + ((TimeSpan)R.Get(cls, "ReloadTime")).Ticks + " auto=" + ((bool)R.Get(cls, "Automatic") ? 1 : 0) +
                        " capacity=" + (int)R.Get(cls, "ClipCapacity") + " per=" + (int)R.Get(cls, "RoundsPerReload");
            yield return line;
        }
    }

    // -------------------------------------------------------------- recording

    private static string Bits(float value)
    {
        return BitConverter.ToInt32(BitConverter.GetBytes(value), 0).ToString("x8");
    }

    private static string Describe(object animation)
    {
        if (animation == null) return "-";
        return (string)R.Get(animation, "Name") + "@" + ((TimeSpan)R.Get(animation, "CurrentTime")).Ticks + "x" +
               Bits((float)R.Get(animation, "Speed")) + ((bool)R.Get(animation, "Looping") ? "L" : "l") +
               ((bool)R.Get(animation, "Reversed") ? "R" : "r") + ((bool)R.Get(animation, "PingPong") ? "P" : "p") +
               ((bool)R.Get(animation, "_onPong") ? "O" : "o") + ((bool)R.Get(animation, "Playing") ? "" : "S");
    }

    private static string Channels(object player)
    {
        object animations = R.Get(R.Get(player, "Avatar"), "Animations");
        var blenders = (Array)R.Get(animations, "_blenders");
        var text = new StringBuilder();
        for (int channel = 0; channel < 6; ++channel)
        {
            object blender = blenders.GetValue(channel);
            object current = R.Get(blender, "_currentAnimation"), previous = R.Get(blender, "_previousAnimation");
            text.Append(" c").Append(channel).Append('=').Append(Describe(current));
            if (previous != null)
                text.Append('|').Append(Describe(previous)).Append('|').Append(((TimeSpan)R.Get(blender, "_blendCurrentTime")).Ticks)
                    .Append('/').Append(((TimeSpan)R.Get(blender, "_blendTotalTime")).Ticks);
        }
        return text.ToString();
    }

    private static string Flags(object player)
    {
        string[] names = { "UsingTool", "Shouldering", "Reloading", "Dead", "PlayGrenadeAnim", "ReadyToThrowGrenade" };
        var text = new StringBuilder();
        foreach (string name in names) text.Append((bool)R.Get(player, name) ? '1' : '0');
        return text.ToString();
    }

    private static string State(object player)
    {
        return "f=" + Flags(player) + " m=" + Convert.ToInt32(R.Get(player, "_playerMode")) + " st=" +
               Convert.ToInt32(R.Get(player, "currentAnimState")) + " uap=" + ((bool)R.Get(player, "usingAnimationPlaying") ? 1 : 0) +
               " fps=" + ((bool)R.Get(player, "FPSMode") ? 1 : 0);
    }

    private static string TakeEvents()
    {
        string joined = Rig.Events.Count == 0 ? "-" : string.Join(";", Rig.Events);
        Rig.Events.Clear();
        return joined;
    }

    // ------------------------------------------------------------------ frame

    private static object Trigger(bool held, bool previous)
    {
        object trigger = Activator.CreateInstance(R.Field(mappingType, "Use").FieldType);
        R.Set(trigger, "Held", held);
        R.Set(trigger, "Pressed", held && !previous);
        R.Set(trigger, "Released", !held && previous);
        return trigger;
    }

    private static void SetTrigger(object mapping, string name, bool held, bool previous)
    {
        // CastleMinerZControllerMapping re-declares some FPSControllerMapping
        // triggers with `new`; write every level so each reader sees the key.
        for (Type level = mappingType; level != null; level = level.BaseType)
        {
            FieldInfo field = level.GetField(name, R.All);
            if (field != null) field.SetValue(mapping, Trigger(held, previous));
        }
    }

    private static object ClassOf(int id)
    {
        Type ids = Rig.T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        return R.CallStatic(itemType, "GetClass", Enum.ToObject(ids, id));
    }

    private static object CreateItem(int id, int count)
    {
        Type ids = Rig.T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        return R.CallStatic(itemType, "CreateItem", Enum.ToObject(ids, id), count);
    }

    private static void SetTray(int slot, object item)
    {
        if (R.Has(Inventory.GetType(), "TrayManager"))
        {
            var trays = (Array)R.Get(R.Get(Inventory, "TrayManager"), "Trays");
            trays.SetValue(item, 0, slot);
        }
        else ((Array)R.Get(Inventory, "InventoryTray")).SetValue(item, slot);
    }

    private static void Reset(object player)
    {
        object animations = R.Get(R.Get(player, "Avatar"), "Animations");
        var blenders = (Array)R.Get(animations, "_blenders");
        for (int channel = 0; channel < blenders.Length; ++channel) blenders.SetValue(Activator.CreateInstance(blenderType), channel);
        R.Set(animations, "players", Array.CreateInstance(Rig.T("DNA.Drawing.Animation.AnimationPlayer"), 16, 3));
        R.Set(animations, "currentPlayers", new int[16]);
        foreach (string flag in new[] { "UsingTool", "Shouldering", "Reloading", "Dead", "PlayGrenadeAnim", "ReadyToThrowGrenade",
                                        "usingAnimationPlaying", "_isRunning", "_isMoveing", "LockedFromFalling", "_flyMode" })
            R.Set(player, flag, false);
        R.Set(player, "currentAnimState", Enum.ToObject(R.Field(playerType, "currentAnimState").FieldType, 0));
        R.Set(player, "_playerMode", Enum.Parse(R.Field(playerType, "_playerMode").FieldType, "Fist"));
        R.Set(player, "grenadeCookTime", TimeSpan.Zero);
        R.Set(player, "_walkSpeed", 1f);
        R.Set(player, "_footStepTimer", Activator.CreateInstance(R.Field(playerType, "_footStepTimer").FieldType, TimeSpan.FromSeconds(0.5)));
        R.Set(player, "ReloadSound", null);
        R.Set(player, "RecoilRotation", Quaternion.Identity);
        R.Set(player, "TorsoPitch", R.GetStatic(Rig.T("DNA.Angle"), "Zero"));
        R.Call(player, "SetupAnimation");
    }

    private static void SetFpsMode(object player, bool value)
    {
        object avatar = R.Get(player, "Avatar");
        if (R.Get(avatar, "Parent") != null) R.Call(avatar, "RemoveFromParent");
        object scene = R.Get(R.Get(Rig.GameInstance, "GameScreen"), "_fpsScene");
        R.Call(R.Get(value ? scene : player, "Children"), "Add", avatar);
        R.Set(avatar, "HideHead", value);
    }

    private static byte[] Encode(object message)
    {
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            R.Invoke(R.Method(message.GetType(), "SendData", 1), message, new object[] { writer });
            writer.Flush();
            return stream.ToArray();
        }
    }

    public static void Run(Scenario scenario, TextWriter output)
    {
        Rig.ClearTerrain();
        Rig.Fill(-40, -3, -40, 60, -1, 40, 6);
        Rig.SetBlock(3, 1, 0, 6);
        Rig.Drain();
        remoteInbox.Clear();
        Type inventoryType = R.Field(playerType, "PlayerInventory").FieldType;
        Inventory = Activator.CreateInstance(inventoryType, Local, false);
        R.Set(Local, "PlayerInventory", Inventory);
        R.Set(Inventory, "SelectedInventoryIndex", 0);
        var ammoTypes = new HashSet<object>();
        Type gunItem = Rig.T("DNA.CastleMinerZ.Inventory.GunInventoryItem");
        for (int slot = 0; slot < scenario.Tray.Length; ++slot)
        {
            if (scenario.Tray[slot] == 0) continue;
            object item = CreateItem(scenario.Tray[slot], slot < scenario.Counts.Length ? scenario.Counts[slot] : 1);
            if (gunItem.IsInstanceOfType(item))
            {
                if (slot < scenario.Rounds.Length && scenario.Rounds[slot] >= 0) R.Set(item, "RoundsInClip", scenario.Rounds[slot]);
                object ammo = R.Get(R.Get(item, "GunClass"), "AmmoType");
                if (ammo != null) ammoTypes.Add(ammo);
            }
            SetTray(slot, item);
        }
        int next = 0;
        foreach (object ammo in ammoTypes)
            if (scenario.Ammo > 0)
                ((Array)R.Get(Inventory, "Inventory")).SetValue(R.Call(ammo, "CreateItem", scenario.Ammo), next++);
        lastItem = null;
        R.Set(Rig.GameInstance, "_currentFrameNumber", 0);
        R.Set(Rig.GameInstance, "InfiniteResourceMode", false);

        foreach (object player in new[] { Local, Remote }) Reset(player);
        SetFpsMode(Local, true);
        var start = new Vector3(0.5f, 0.001f, 0.5f);
        object physics = Rig.Physics(Local);
        R.Set(physics, "WorldAcceleration", (Vector3)R.GetStatic(Rig.T("DNA.Drawing.BasicPhysics"), "Gravity"));
        R.Set(physics, "WorldVelocity", Vector3.Zero);
        R.Set(Local, "LocalPosition", start);
        R.Set(Local, "LocalRotation", Quaternion.Identity);
        R.Set(Local, "InContact", true);
        R.Set(Local, "Speed", 5f);
        if (R.Has(playerType, "m_jumpCount")) R.Set(Local, "m_jumpCount", 0);
        R.Set(Remote, "LocalPosition", start + new Vector3(6f, 0f, 0f));

        object probe = R.Get(Hud, "ConstructionProbe");
        R.Set(probe, "_collides", scenario.Target == "block");
        R.Set(probe, "_startsIn", false);
        R.Set(probe, "HitZombie", scenario.Target == "zombie");
        R.Set(probe, "HitPlayer", false);
        R.Set(probe, "_worldIndex", Rig.Cell(3, 1, 0));
        if (R.Has(hudType, "PlayerStamina")) R.Set(Hud, "PlayerStamina", 1f);

        int frames = (int)Math.Round(scenario.Seconds * 1e7 / scenario.Ticks);
        output.WriteLine("SCENARIO " + scenario.Name + " ticks=" + scenario.Ticks + " frames=" + frames + " target=" + scenario.Target +
                         " ammo=" + scenario.Ammo + " tray=" + string.Join(",", scenario.Tray) + " modern=" + (Rig.Modern ? 1 : 0));
        Rig.Events.Clear();
        var elapsed = TimeSpan.FromTicks(scenario.Ticks);
        Program.FrameSeconds = (float)(scenario.Ticks * 1e-7);
        var previous = new InputSample();
        Type updateType = Rig.T("DNA.CastleMinerZ.Net.PlayerUpdateMessage");
        MethodInfo sendUpdates = R.Method(Rig.GameInstance.GetType(), "SendNetworkUpdates", 2);
        int respawnFrame = -1;
        for (int frame = 0; frame < frames; ++frame)
        {
            float seconds = (float)(frame * (double)scenario.Ticks * 1e-7);
            var time = new GameTime(TimeSpan.FromTicks(scenario.Ticks * (frame + 1)), elapsed);
            R.Set(Rig.GameInstance, "CurrentGameTime", time);
            InputSample input = scenario.Input(seconds);
            var notes = new List<string>();

            // ---- ProcessNetworkMessages: the local echo queue ----
            foreach (Captured captured in Rig.Drain())
            {
                if (captured.Name == "ChangeCarriedItemMessage")
                {
                    int id = Convert.ToInt32(R.Get(Rig.Decode(captured), "ItemID"));
                    object cls = ClassOf(id);
                    object mode = cls != null ? R.Get(cls, "PlayerAnimationMode")
                                              : Enum.Parse(R.Field(playerType, "_playerMode").FieldType, "Fist");
                    R.Set(Local, "_playerMode", mode);
                    notes.Add("carried:" + id);
                }
                else if (captured.Name == "GrenadeMessage")
                {
                    foreach (object player in new[] { Local, Remote })
                        R.Call(R.Get(R.Get(player, "Avatar"), "Animations"), "Play", "Grenade_Reset", 3, TimeSpan.Zero);
                    object active = R.Get(Inventory, "ActiveInventoryItem");
                    if (active != null && R.Get(active, "ItemClass").GetType().Name.Contains("Grenade")) R.Call(active, "PopOneItem");
                    notes.Add("grenade:" + Bits((float)R.Get(Rig.Decode(captured), "SecondsLeft")));
                }
                else if (captured.Name != "PlayerUpdateMessage") notes.Add("recv:" + captured.Name.Replace("Message", ""));
            }
            // ---- the remote peer applies what the local player sent ----
            string remoteBytes = "-";
            if (remoteInbox.Count != 0)
            {
                byte[] bytes = remoteInbox[0];
                remoteInbox.RemoveAt(0);
                object message = Activator.CreateInstance(updateType, true);
                R.Set(message, "_sender", R.Get(Remote, "Gamer"));
                using (var stream = new MemoryStream(bytes, false))
                using (var reader = new BinaryReader(stream))
                    R.Invoke(R.Method(updateType, "RecieveData", 1), message, new object[] { reader });
                var movement = (Vector2)R.Get(message, "Movement");
                // Cues the receiving peer plays from the remote player's emitter.
                int before = Rig.Events.Count;
                R.Call(message, "Apply", Remote);
                remoteNotes.AddRange(Rig.Events.Skip(before));
                Rig.Events.RemoveRange(before, Rig.Events.Count - before);
                remoteBytes = BitConverter.ToString(bytes, 12).Replace("-", "") + ":" + Bits(movement.X) + ":" + Bits(movement.Y);
            }

            // ---- SendNetworkUpdates ----
            object sendInstance = Rig.T("DNA.Net.Message").GetMethods(R.All)
                .First(method => method.Name == "GetSendInstance" && method.IsGenericMethodDefinition)
                .MakeGenericMethod(updateType).Invoke(null, null);
            // A sentinel shows whether this frame's round-robin wrote the message.
            R.Set(sendInstance, "PlayerMode", Enum.ToObject(R.Field(updateType, "PlayerMode").FieldType, 255));
            try
            {
                R.Invoke(sendUpdates, Rig.GameInstance, new object[] { Rig.Session, time });
                cadenceExecuted = true;
            }
            catch (Exception error)
            {
                throw new InvalidOperationException("SendNetworkUpdates did not run offline: " + error.Message, error);
            }
            if (Convert.ToInt32(R.Get(sendInstance, "PlayerMode")) != 255) remoteInbox.Add(Encode(sendInstance));

            // ---- InGameHUD.OnPlayerInput ----
            Mapping = R.Get(Rig.GameInstance, "_controllerMapping");
            R.Set(Mapping, "Movement", new Vector2(input.Strafe, input.Forward));
            SetTrigger(Mapping, "Use", input.Use, previous.Use);
            SetTrigger(Mapping, "Shoulder", input.Shoulder, previous.Shoulder);
            SetTrigger(Mapping, "Reload", input.Reload, previous.Reload);
            SetTrigger(Mapping, "Jump", input.Jump, previous.Jump);
            foreach (string key in new[] { "Sprint" }) if (R.Has(mappingType, key)) SetTrigger(Mapping, key, false, false);
            if (R.Has(mappingType, "MoveForward"))
            {
                SetTrigger(Mapping, "MoveForward", input.Forward > 0f, false);
                SetTrigger(Mapping, "MoveBackward", input.Forward < 0f, false);
                SetTrigger(Mapping, "StrafeLeft", input.Strafe < 0f, false);
                SetTrigger(Mapping, "StrafeRight", input.Strafe > 0f, false);
            }
            R.Invoke(processInput, Local, new[] { Mapping, (object)time });
            R.Call(Inventory, "Update", time);
            bool dead = (bool)R.Get(Local, "Dead");
            if (dead)
            {
                R.Set(Local, "UsingTool", false);
                if (input.Jump && !previous.Jump && respawnFrame >= 0 && frame >= respawnFrame)
                {
                    // InGameHUD.RefreshPlayer.
                    R.Set(Local, "Dead", false);
                    SetFpsMode(Local, true);
                    notes.Add("respawn");
                }
            }
            else
            {
                if (input.Slot >= 0 && input.Slot != previous.Slot)
                {
                    if (!(bool)R.Get(Local, "UsingAnimationPlaying"))
                    {
                        R.Set(Inventory, "SelectedInventoryIndex", input.Slot);
                        R.Set(Local, "Shouldering", false);
                        R.Set(R.Get(Inventory, "ActiveInventoryItem"), "DigTime", TimeSpan.Zero);
                        notes.Add("slot:" + input.Slot);
                    }
                    else notes.Add("slotrefused:" + input.Slot);
                }
                object active = R.Get(Inventory, "ActiveInventoryItem");
                R.Call(active, "ProcessInput", Hud, Mapping);
                R.Call(Inventory, "RemoveEmptyItems");
            }

            // ---- ScreenManager.Update: InGameHUD.EquipActiveItem ----
            object now = R.Get(Inventory, "ActiveInventoryItem");
            if (!ReferenceEquals(lastItem, now))
            {
                if (lastItem != null) R.Call(R.Get(lastItem, "ItemClass"), "OnItemUnequipped");
                R.Call(R.Get(now, "ItemClass"), "OnItemEquipped");
                lastItem = now;
                R.Call(Local, "Equip", now);
                if (gunItem.IsInstanceOfType(now)) R.Set(Local, "ReloadSound", R.Get(R.Get(now, "GunClass"), "ReloadSound"));
            }
            // ---- the scene: gravity, collision, Player.OnUpdate, Avatar.OnUpdate ----
            R.Invoke(R.Method(physics.GetType(), "Accelerate", 1), physics, new object[] { elapsed });
            R.Invoke(resolve, Local, new object[] { Rig.Terrain, null, time });
            if ((bool)R.Get(Local, "PlayGrenadeAnim") && !(bool)R.Get(Local, "ReadyToThrowGrenade"))
            {
                var cook = (TimeSpan)R.Get(Local, "grenadeCookTime") + elapsed;
                R.Set(Local, "grenadeCookTime", cook);
                if (cook >= TimeSpan.FromSeconds(4.0)) R.Set(Local, "ReadyToThrowGrenade", true);
            }
            R.Invoke(updateAudio, Local, new object[] { time });
            if (input.Kill && !dead)
            {
                // InGameHUD.KillPlayer.
                R.Set(Local, "PlayGrenadeAnim", false);
                R.Set(Local, "ReadyToThrowGrenade", false);
                R.Set(Local, "Dead", true);
                SetFpsMode(Local, false);
                R.Set(R.Get(Local, "Avatar"), "HideHead", true);
                respawnFrame = frame + (int)Math.Ceiling(3.0 * 1e7 / scenario.Ticks);
                notes.Add("killed");
            }
            var bones = Activator.CreateInstance(typeof(List<>).MakeGenericType(Rig.T("DNA.Drawing.Bone")));
            foreach (object player in new[] { Local, Remote })
                R.Call(R.Get(R.Get(player, "Avatar"), "Animations"), "Update", elapsed, bones);

            object activeNow = R.Get(Inventory, "ActiveInventoryItem");
            string item = Convert.ToInt32(R.Get(R.Get(activeNow, "ItemClass"), "ID")).ToString();
            if (gunItem.IsInstanceOfType(activeNow)) item += "/" + (int)R.Get(activeNow, "RoundsInClip");
            output.WriteLine("IN " + frame + " mv=" + Bits(input.Strafe) + "," + Bits(input.Forward) + " k=" + (input.Use ? 1 : 0) +
                             (input.Shoulder ? 1 : 0) + (input.Reload ? 1 : 0) + (input.Jump ? 1 : 0) + " slot=" + input.Slot +
                             " kill=" + (input.Kill ? 1 : 0));
            object fov = R.Get(R.Get(Local, "FPSCamera"), "FieldOfView");
            output.WriteLine("L " + frame + " " + State(Local) + " item=" + item + " fov=" + Bits((float)R.Get(fov, "Radians")) +
                             Channels(Local) + " ev=" + TakeEvents() +
                             " nt=" + (notes.Count == 0 ? "-" : string.Join(";", notes)));
            output.WriteLine("R " + frame + " " + State(Remote) + " rx=" + remoteBytes + Channels(Remote) + " ev=" +
                             (remoteNotes.Count == 0 ? "-" : string.Join(";", remoteNotes)));
            remoteNotes.Clear();
            previous = input;
        }
        output.WriteLine("END");
        // The remote avatar goes back to third person for the next scenario.
        SetFpsMode(Local, true);
        if (!cadenceExecuted) throw new InvalidOperationException("the original round-robin never ran");
    }
}
