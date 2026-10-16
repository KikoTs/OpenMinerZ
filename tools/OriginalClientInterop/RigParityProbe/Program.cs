using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;
using OpenMinerZ.RetailCharacter;

// Character rig probe. It loads the user's owned, unmodified Castle Miner Z
// assemblies by path and executes the ORIGINAL character code offline:
//
//   * Player's static initialisation, which loads the body every player is drawn
//     with (the content load is logged, so the file it resolves to is a fact),
//   * DNA.Avatars.Avatar, the body's SkinnedModelEntity/ModelEntity,
//     AvatarAnimationCollection, AnimBlender, AnimationPlayer, AnimationClip,
//     Bone and Skeleton, driven through the real Entity.Update, and
//   * InventoryItem.Initalize and every item class's CreateEntity(ItemUse.Hand).
//
// Nothing here starts Steam, opens a session or runs the game loop. A hidden
// 16x16 graphics device exists only so the original ContentManager can read the
// real model, clips and item models; without one the probe exits 77 (skip).
//
// What is NOT original code here, and nothing else:
//   * The statements of Player's constructor that assemble the character
//     (new Avatar, ProxyModelEntity = new ...ModelEntity(ProxyModel),
//     GetAvatarPart(PropRight)); the constructor itself needs a live gamer.
//     They live in tools/ContentBridge/RetailCharacter.cs, shared with the importer.
//   * WHICH clip plays WHEN. Player.UpdateAnimation is not run; the scenarios
//     below are scripted Play/Clear/Update calls against the real
//     AvatarAnimationCollection, recorded as ops so the native side replays the
//     identical sequence through its own port.
//   * The clip registrations are read out of CastleMinerZGame.SecondaryLoad's IL
//     (RegistrationScanner) and then performed with the real
//     AvatarAnimationManager.RegisterAnimation on clips loaded by the real
//     ContentManager.
//
// Output:
//   rig-facts.txt        human-readable facts (model, skeletons, materials, clips)
//   rig-reference.bin    scenario ops and the original's pose at each snapshot
//                        (shared with tests/test_rig_parity.cpp)
internal static class Program
{
    private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

    private static string F(float value) { return value.ToString("R", Invariant); }

    private static string M(Matrix m)
    {
        return string.Join(",", new[] { m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24,
                                         m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44 }.Select(F));
    }

    private static void W(BinaryWriter writer, Matrix m)
    {
        writer.Write(m.M11); writer.Write(m.M12); writer.Write(m.M13); writer.Write(m.M14);
        writer.Write(m.M21); writer.Write(m.M22); writer.Write(m.M23); writer.Write(m.M24);
        writer.Write(m.M31); writer.Write(m.M32); writer.Write(m.M33); writer.Write(m.M34);
        writer.Write(m.M41); writer.Write(m.M42); writer.Write(m.M43); writer.Write(m.M44);
    }

    // ------------------------------------------------------------------ facts
    private static void DescribeModel(List<string> facts, string label, Model model)
    {
        facts.Add("");
        facts.Add("== model " + label);
        facts.Add("model.bones=" + model.Bones.Count + " meshes=" + model.Meshes.Count + " root=" + model.Root.Name);
        object tag = model.Tag;
        facts.Add("model.tag=" + (tag == null ? "null" : tag.GetType().FullName));
        if (tag != null && tag.GetType().GetProperty("Skeleton") != null)
        {
            var skeleton = ((IEnumerable)tag.GetType().GetProperty("Skeleton").GetValue(tag, null)).Cast<object>().ToList();
            var inverse = (Matrix[])tag.GetType().GetProperty("InverseBindPose").GetValue(tag, null);
            var clips = (IDictionary)tag.GetType().BaseType.GetProperty("AnimationClips").GetValue(tag, null);
            facts.Add("skeleton.bones=" + skeleton.Count + " inverseBind=" + inverse.Length + " embeddedClips=" +
                      (clips == null ? "0" : string.Join("/", clips.Keys.Cast<string>())));
            foreach (object bone in skeleton)
            {
                object parent = R.Get(bone, "Parent");
                facts.Add("  bone " + R.Get(bone, "Index") + " " + R.Get(bone, "Name") + " parent=" +
                          (parent == null ? -1 : (int)R.Get(parent, "Index")) + " bind=" +
                          M((Matrix)R.Get(bone, "Transform")));
            }
        }
        foreach (ModelMesh mesh in model.Meshes)
        {
            facts.Add("mesh " + mesh.Name + " parentBone=" + mesh.ParentBone.Index + ":" + mesh.ParentBone.Name +
                      " parts=" + mesh.MeshParts.Count + " sphere=" + F(mesh.BoundingSphere.Radius));
            foreach (ModelMeshPart part in mesh.MeshParts)
            {
                var elements = part.VertexBuffer.VertexDeclaration.GetVertexElements()
                    .Select(e => e.VertexElementUsage + "" + e.UsageIndex + ":" + e.VertexElementFormat + "@" + e.Offset);
                facts.Add("  part vertices=" + part.NumVertices + " triangles=" + part.PrimitiveCount + " stride=" +
                          part.VertexBuffer.VertexDeclaration.VertexStride + " [" + string.Join(" ", elements) + "]");
                Effect effect = part.Effect;
                facts.Add("  effect=" + effect.GetType().FullName + " technique=" + effect.CurrentTechnique.Name +
                          " techniques=" + string.Join("/", effect.Techniques.Select(t => t.Name)));
                foreach (EffectParameter parameter in effect.Parameters)
                {
                    string value;
                    try
                    {
                        if (parameter.ParameterType == EffectParameterType.Texture2D ||
                            parameter.ParameterType == EffectParameterType.Texture)
                        {
                            Texture2D texture = parameter.GetValueTexture2D();
                            value = texture == null ? "null" : texture.Width + "x" + texture.Height + " " + texture.Format;
                        }
                        else if (parameter.ParameterClass == EffectParameterClass.Vector && parameter.ColumnCount == 3 &&
                                 parameter.Elements.Count == 0)
                        {
                            Vector3 v = parameter.GetValueVector3();
                            value = F(v.X) + "," + F(v.Y) + "," + F(v.Z);
                        }
                        else if (parameter.ParameterClass == EffectParameterClass.Vector && parameter.ColumnCount == 4 &&
                                 parameter.Elements.Count == 0)
                        {
                            Vector4 v = parameter.GetValueVector4();
                            value = F(v.X) + "," + F(v.Y) + "," + F(v.Z) + "," + F(v.W);
                        }
                        else if (parameter.ParameterClass == EffectParameterClass.Scalar &&
                                 parameter.ParameterType == EffectParameterType.Single && parameter.Elements.Count == 0)
                            value = F(parameter.GetValueSingle());
                        else
                            continue;
                    }
                    catch (Exception) { continue; }
                    facts.Add("    " + parameter.Name + " (" + parameter.Semantic + ") = " + value);
                }
            }
        }
    }

    private static Vector3[] Positions(ModelMeshPart part)
    {
        VertexDeclaration declaration = part.VertexBuffer.VertexDeclaration;
        int stride = declaration.VertexStride;
        byte[] bytes = new byte[part.VertexBuffer.VertexCount * stride];
        part.VertexBuffer.GetData(bytes);
        int position = declaration.GetVertexElements().First(e => e.VertexElementUsage == VertexElementUsage.Position).Offset;
        var result = new Vector3[part.NumVertices];
        for (int vertex = 0; vertex < part.NumVertices; ++vertex)
        {
            int offset = (part.VertexOffset + vertex) * stride + position;
            result[vertex] = new Vector3(BitConverter.ToSingle(bytes, offset), BitConverter.ToSingle(bytes, offset + 4),
                                         BitConverter.ToSingle(bytes, offset + 8));
        }
        return result;
    }

    // Farthest any vertex of `other` lies from the nearest vertex of `reference`:
    // zero when two variants describe the same surface with different vertex splits.
    private static double SurfaceDifference(Model reference, Model other)
    {
        var cells = new Dictionary<long, List<Vector3>>();
        const float cell = 0.01f;
        Func<Vector3, int, int, int, long> key = (p, dx, dy, dz) =>
            (((long)Math.Floor(p.X / cell) + dx) & 0x1FFFFF) | ((((long)Math.Floor(p.Y / cell) + dy) & 0x1FFFFF) << 21) |
            ((((long)Math.Floor(p.Z / cell) + dz) & 0x1FFFFF) << 42);
        foreach (ModelMesh mesh in reference.Meshes)
            foreach (ModelMeshPart part in mesh.MeshParts)
                foreach (Vector3 point in Positions(part))
                {
                    List<Vector3> list;
                    long k = key(point, 0, 0, 0);
                    if (!cells.TryGetValue(k, out list)) cells[k] = list = new List<Vector3>();
                    list.Add(point);
                }
        double worst = 0.0;
        foreach (ModelMesh mesh in other.Meshes)
            foreach (ModelMeshPart part in mesh.MeshParts)
                foreach (Vector3 point in Positions(part))
                {
                    double best = double.PositiveInfinity;
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dz = -1; dz <= 1; ++dz)
                            {
                                List<Vector3> list;
                                if (!cells.TryGetValue(key(point, dx, dy, dz), out list)) continue;
                                foreach (Vector3 candidate in list)
                                    best = Math.Min(best, Vector3.Distance(candidate, point));
                            }
                    worst = Math.Max(worst, best);
                }
        return worst;
    }

    // -------------------------------------------------------------- scenarios
    private const byte OpPlay = 1, OpClear = 2, OpSet = 3, OpUpdate = 4, OpSnapshot = 5, OpHideHead = 6, OpReplay = 7,
                       OpItemSnapshot = 8;
    private const byte SetSpeed = 1, SetLooping = 2, SetReversed = 3, SetPingPong = 4, SetProgress = 5,
                       SetPause = 6, SetResume = 7;

    private sealed class Recorder
    {
        public readonly BinaryWriter Writer;
        public readonly MemoryStream Stream = new MemoryStream();
        public Character Character;
        public int Ops;
        public int Snapshots;

        public Recorder() { Writer = new BinaryWriter(Stream, Encoding.UTF8); }

        public void Play(string name, int channel, double blendSeconds)
        {
            long ticks = TimeSpan.FromSeconds(blendSeconds).Ticks;
            Character.Play(name, channel, ticks);
            Writer.Write(OpPlay); Writer.Write((byte)channel); Writer.Write(name); Writer.Write(ticks); ++Ops;
        }

        public void Clear(int channel, double blendSeconds)
        {
            long ticks = TimeSpan.FromSeconds(blendSeconds).Ticks;
            Character.Clear(channel, ticks);
            Writer.Write(OpClear); Writer.Write((byte)channel); Writer.Write(ticks); ++Ops;
        }

        public void Replay(int channel, double blendSeconds)
        {
            long ticks = TimeSpan.FromSeconds(blendSeconds).Ticks;
            Character.Replay(channel, ticks);
            Writer.Write(OpReplay); Writer.Write((byte)channel); Writer.Write(ticks); ++Ops;
        }

        public void Set(int channel, byte property, double value)
        {
            object player = Character.Active(channel);
            switch (property)
            {
                case SetSpeed: R.Set(player, "Speed", (float)value); break;
                case SetLooping: R.Set(player, "Looping", value != 0.0); break;
                case SetReversed: R.Set(player, "Reversed", value != 0.0); break;
                case SetPingPong: R.Set(player, "PingPong", value != 0.0); break;
                case SetProgress: R.Set(player, "Progress", (float)value); break;
                case SetPause: R.Call(player, "Pause"); break;
                case SetResume: R.Call(player, "Play"); break;
            }
            Writer.Write(OpSet); Writer.Write((byte)channel); Writer.Write(property); Writer.Write(value); ++Ops;
        }

        public void HideHead(bool hide)
        {
            R.Set(Character.Avatar, "HideHead", hide);
            Writer.Write(OpHideHead); Writer.Write(hide); ++Ops;
        }

        public void Update(long elapsedTicks)
        {
            Character.Update(elapsedTicks);
            Writer.Write(OpUpdate); Writer.Write(elapsedTicks); ++Ops;
        }

        public void Snapshot()
        {
            Writer.Write(OpSnapshot); ++Ops; ++Snapshots;
            var world = (Matrix[])R.Get(Character.Proxy, "WorldBoneTransforms");
            var skin = (Matrix[])R.Get(Character.Proxy, "_skinTransforms");
            var parts = (Matrix[])R.Get(Character.Avatar, "_bonesToAvatar");
            foreach (Matrix matrix in world) W(Writer, matrix);
            foreach (Matrix matrix in skin) W(Writer, matrix);
            foreach (Matrix matrix in parts) W(Writer, matrix);
            W(Writer, (Matrix)R.Get(R.Get(Character.Avatar, "EyePointCamera"), "LocalToParent"));
            W(Writer, (Matrix)R.Get(Character.RightHand, "LocalToWorld"));
            for (int channel = 0; channel < 6; ++channel)
            {
                object player = Character.Active(channel);
                Writer.Write(player != null);
                if (player == null) continue;
                Writer.Write((string)R.Get(player, "Name"));
                Writer.Write(((TimeSpan)R.Get(player, "CurrentTime")).Ticks);
                Writer.Write((float)R.Get(player, "Progress"));
                Writer.Write((bool)R.Get(player, "Finished"));
            }
        }
    }

    private const long Frame60 = 166667;   // XNA's fixed 60 Hz step, in ticks

    private static string Pick(Original original, params string[] names)
    {
        foreach (string name in names)
            if (original.Registrations.Any(r => r.Name == name)) return name;
        throw new InvalidOperationException("None of these clips is registered: " + string.Join(", ", names));
    }

    private static List<KeyValuePair<string, byte[]>> RecordScenarios(Original original, List<string> facts,
                                                                     List<HeldItem> items)
    {
        var scenarios = new List<KeyValuePair<string, byte[]>>();
        Action<string, Action<Recorder>> scenario = (name, body) =>
        {
            var recorder = new Recorder { Character = Character.Create(original) };
            body(recorder);
            recorder.Writer.Flush();
            using (var packed = new MemoryStream())
            using (var writer = new BinaryWriter(packed))
            {
                writer.Write(name);
                writer.Write(recorder.Ops);
                writer.Write(recorder.Stream.ToArray());
                writer.Flush();
                scenarios.Add(new KeyValuePair<string, byte[]>(name, packed.ToArray()));
            }
            facts.Add("scenario " + name + " ops=" + recorder.Ops + " snapshots=" + recorder.Snapshots);
        };

        string stand = Pick(original, "Stand");
        // Every registered clip, on the channel its influence set belongs to above
        // Stand, stepped at the game's 60 Hz frame time across its whole length and
        // past its end (the last-key hold, the loop wrap and the clamp are sampled).
        foreach (ClipRegistration registration in original.Registrations)
        {
            string name = registration.Name;
            scenario("clip:" + name, recorder =>
            {
                recorder.Play(stand, 0, 0.0);
                if (name != stand) recorder.Play(name, registration.Bones == null ? 0 : 2, 0.0);
                double duration = ((TimeSpan)R.Get(original.Clips[name], "Duration")).TotalSeconds;
                int frames = (int)Math.Ceiling(duration * 60.0 * 1.15) + 4;
                int stride = Math.Max(1, frames / 12);
                recorder.Update(0);
                recorder.Snapshot();
                for (int frame = 1; frame <= frames; ++frame)
                {
                    recorder.Update(Frame60);
                    if (frame % stride == 0 || frame > frames - 4) recorder.Snapshot();
                }
            });
        }

        string idleA = Pick(original, "GenericIdle"), idleB = Pick(original, "PickIdle", "GunIdle");
        string use = Pick(original, "PickUse", "GenericUse"), walk = Pick(original, "Walk"), run = Pick(original, "Run");
        string tilt = Pick(original, "Tilt"), head = Pick(original, "IdleHead"), die = Pick(original, "Die");
        string gunIdle = Pick(original, "GunIdle"), shoulder = Pick(original, "GunShoulder");
        string shoulderIdle = Pick(original, "GunShoulderIdle"), reload = Pick(original, "GunReload");
        string shoot = Pick(original, "GunShoot");

        scenario("blend:item-switch", recorder =>
        {
            recorder.Play(stand, 0, 0.0); recorder.Play(idleA, 2, 0.0);
            for (int frame = 0; frame < 7; ++frame) recorder.Update(Frame60);
            recorder.Snapshot();
            recorder.Play(idleB, 2, 0.25);
            for (int frame = 0; frame < 20; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
        });
        scenario("blend:action-and-clear", recorder =>
        {
            recorder.Play(stand, 0, 0.0); recorder.Play(idleB, 2, 0.0);
            for (int frame = 0; frame < 5; ++frame) recorder.Update(Frame60);
            recorder.Play(use, 3, 0.0);
            for (int frame = 0; frame < 12; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
            recorder.Clear(3, 0.25);
            for (int frame = 0; frame < 18; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
        });
        scenario("locomotion:speed-reversed", recorder =>
        {
            recorder.Play(stand, 0, 0.0); recorder.Play(idleA, 2, 0.0);
            recorder.Update(Frame60);
            recorder.Play(walk, 0, 0.25);
            recorder.Set(0, SetSpeed, 0.6);
            recorder.Set(0, SetReversed, 1.0);
            for (int frame = 0; frame < 40; ++frame) { recorder.Update(Frame60); if (frame % 2 == 0) recorder.Snapshot(); }
            recorder.Play(run, 0, 0.25);
            recorder.Set(0, SetSpeed, 0.93);
            for (int frame = 0; frame < 30; ++frame) { recorder.Update(Frame60); if (frame % 2 == 0) recorder.Snapshot(); }
        });
        scenario("layers:tilt-head-third-person", recorder =>
        {
            recorder.Play(stand, 0, 0.0);
            recorder.Play(tilt, 1, 0.0);
            recorder.Set(1, SetPause, 0.0);
            recorder.Play(idleA, 2, 0.0);
            recorder.Play(head, 4, 0.0);
            recorder.Set(4, SetPingPong, 1.0);
            double[] pitches = { -90, -45, -10, 0, 12.5, 30, 60, 90 };
            for (int frame = 0; frame < 96; ++frame)
            {
                // Player.UpdateAnimation re-plays the paused tilt player every
                // frame and then sets its progress from the torso pitch.
                recorder.Replay(1, 0.0);
                recorder.Set(1, SetProgress, (pitches[frame / 12] + 90.0) / 180.0);
                recorder.Update(Frame60);
                if (frame % 3 == 0) recorder.Snapshot();
            }
            // The head sweep is long; run it far enough to reverse at its end.
            for (int frame = 0; frame < 900; ++frame)
            {
                recorder.Replay(1, 0.0);
                recorder.Update(Frame60);
                if (frame % 60 == 59) recorder.Snapshot();
            }
        });
        scenario("layers:first-person", recorder =>
        {
            recorder.HideHead(true);
            recorder.Play(stand, 0, 0.0);
            recorder.Play(tilt, 1, 0.0);
            recorder.Set(1, SetPause, 0.0);
            recorder.Play(gunIdle, 2, 0.0);
            for (int frame = 0; frame < 30; ++frame)
            {
                recorder.Replay(1, 0.0);
                recorder.Set(1, SetProgress, 0.5);
                recorder.Update(Frame60);
                if (frame % 3 == 0) recorder.Snapshot();
            }
            recorder.Play(shoot, 3, 0.0);
            for (int frame = 0; frame < 12; ++frame)
            {
                recorder.Replay(1, 0.0);
                recorder.Set(1, SetProgress, 0.5);
                recorder.Update(Frame60);
                recorder.Snapshot();
            }
        });
        // A new Player in first person, call for call: SetupAnimation, then the
        // first UpdateAnimation passes with a rifle in hand. The item channel
        // fades from SetupAnimation's GenericIdle for a quarter second; every
        // frame of that fade is recorded.
        foreach (string held in new[] { gunIdle, Pick(original, "LaserGunIdle", "GunIdle"), idleB })
            scenario("spawn:first-person:" + held, recorder =>
            {
                recorder.HideHead(true);
                recorder.Play(stand, 0, 0.0);
                recorder.Play(tilt, 1, 0.0);
                recorder.Set(1, SetPause, 0.0);
                recorder.Play(idleA, 2, 0.0);
                recorder.Play(head, 4, 0.0);
                recorder.Set(4, SetPingPong, 1.0);
                recorder.Play(held, 2, 0.25);
                for (int frame = 0; frame < 24; ++frame)
                {
                    recorder.Replay(1, 0.0);
                    recorder.Clear(4, 0.0);
                    recorder.Set(1, SetProgress, 0.5);
                    recorder.Update(Frame60);
                    recorder.Snapshot();
                }
            });
        scenario("stance:shoulder-reload", recorder =>
        {
            recorder.Play(stand, 0, 0.0); recorder.Play(gunIdle, 2, 0.0);
            for (int frame = 0; frame < 4; ++frame) recorder.Update(Frame60);
            recorder.Play(shoulder, 2, 0.0);
            for (int frame = 0; frame < 30; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
            recorder.Play(shoulderIdle, 2, 0.1);
            for (int frame = 0; frame < 12; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
            recorder.Play(shoulder, 2, 0.0);
            recorder.Set(2, SetReversed, 1.0);
            for (int frame = 0; frame < 30; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
            recorder.Play(reload, 2, 0.25);
            for (int frame = 0; frame < 200; ++frame) { recorder.Update(Frame60); if (frame % 5 == 0) recorder.Snapshot(); }
        });
        scenario("death:faint-and-respawn", recorder =>
        {
            recorder.Play(stand, 0, 0.0); recorder.Play(idleA, 2, 0.0);
            for (int frame = 0; frame < 3; ++frame) recorder.Update(Frame60);
            recorder.Play(die, 5, 0.25);
            for (int frame = 0; frame < 260; ++frame) { recorder.Update(Frame60); if (frame % 6 == 0) recorder.Snapshot(); }
            recorder.Clear(5, 0.25);
            for (int frame = 0; frame < 20; ++frame) { recorder.Update(Frame60); recorder.Snapshot(); }
        });
        scenario("clock:odd-frame-times", recorder =>
        {
            // 144 Hz, 30 Hz, a hitch and sub-millisecond steps: the clip clock
            // rounds every step to whole milliseconds, the blend clock does not.
            recorder.Play(stand, 0, 0.0); recorder.Play(walk, 0, 0.25); recorder.Play(idleB, 2, 0.0);
            recorder.Set(0, SetSpeed, 0.8125);
            long[] steps = { 69444, 69444, 69445, 333333, 333334, 2500000, 4000, 4999, 5000, 5001, 166667, 100000, 1 };
            for (int round = 0; round < 6; ++round)
                foreach (long step in steps) { recorder.Update(step); recorder.Snapshot(); }
        });

        // The held item: for every item the game can put in a hand, the real
        // entity from CreateEntity(ItemUse.Hand) is parented to the real
        // PropRight part (as Player.PutItemInHand does) while that item's own
        // stance and use clips play, and its LocalToWorld is recorded. The
        // mode -> clip table below only chooses poses to sample; it is not the
        // game's state machine.
        var modeClips = new Dictionary<string, string[]>
        {
            { "Generic", new[] { "GenericIdle", "GenericWalk", "GenericUse" } },
            { "Pick", new[] { "PickIdle", "PickWalk", "PickUse" } },
            { "Block", new[] { "BlockIdle", "BlockWalk", "BlockUse" } },
            { "Fist", new[] { "FistIdle", "FistWalk", "FistUse" } },
            { "Assault", new[] { "GunIdle", "GunShoulderIdle", "GunShoot" } },
            { "BoltRifle", new[] { "RifleIdle", "RifleShoulderIdle", "RifleShoot" } },
            { "Pistol", new[] { "PistolIdle", "PistolShoulderIdle", "PistolShoot" } },
            { "PumnpShotgun", new[] { "GunIdle", "GunShoulderIdle", "PumpShotgunShoot" } },
            { "PumpShotgun", new[] { "PumpShotgunIdle", "PumpShotgunShoulderIdle", "PumpShotgunShoot" } },
            { "SMG", new[] { "SMGIdle", "SMGShoulderIdle", "SMGShoot" } },
            { "LMG", new[] { "LMGIdle", "LMGShoulderIdle", "LMGShoot" } },
            { "SpaceAssault", new[] { "LaserGunIdle", "LaserGunShoulderIdle", "LaserGunShoot" } },
            { "SpaceBoltRifle", new[] { "LaserRifleIdle", "LaserRifleShoulderIdle", "LaserRifleShoot" } },
            { "SpacePistol", new[] { "LaserPistolIdle", "LaserPistolShoulderIdle", "LaserPistolShoot" } },
            { "SpacePumpShotgun", new[] { "LaserGunIdle", "LaserGunShoulderIdle", "LaserShotgunShoot" } },
            { "SpaceSMG", new[] { "LaserSMGIdle", "LaserSMGShoulderIdle", "LaserSMGShoot" } },
            { "Grenade", new[] { "GrenadeIdle", "GrenadeWalk", "Grenade_Throw" } },
            { "RPG", new[] { "RPGIdle", "RPGWalk", "RPGShoot" } },
        };
        Type itemType = original.T("DNA.CastleMinerZ.Inventory.InventoryItem");
        Type idType = original.T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        object hand = Enum.Parse(original.T("DNA.CastleMinerZ.Inventory.ItemUse"), "Hand");
        MethodInfo getClass = itemType.GetMethod("GetClass", BindingFlags.Static | BindingFlags.Public);
        foreach (IGrouping<string, HeldItem> mode in items.GroupBy(item => item.PlayerMode))
        {
            string[] clips;
            if (!modeClips.TryGetValue(mode.Key, out clips)) clips = modeClips["Generic"];
            clips = clips.Select(clip => original.Registrations.Any(r => r.Name == clip) ? clip : "GenericIdle").ToArray();
            scenario("item:" + mode.Key, recorder =>
            {
                recorder.Play(stand, 0, 0.0);
                for (int phase = 0; phase < clips.Length; ++phase)
                {
                    // Idle and walk on the stance channel, the use clip on the action channel.
                    recorder.Play(clips[phase], phase == 2 ? 3 : 2, 0.0);
                    foreach (bool local in new[] { true, false })
                        foreach (HeldItem item in mode)
                        {
                            object itemClass = getClass.Invoke(null, new[] { Enum.ToObject(idType, item.Id) });
                            object entity = R.Invoke(itemClass.GetType().GetMethod("CreateEntity"), itemClass,
                                                     new[] { hand, (object)local });
                            // The item's own per-frame behaviour (particles, needles) is not
                            // part of where it is attached; keep it out of the offline update.
                            R.Set(entity, "DoUpdate", false);
                            object children = R.Get(recorder.Character.RightHand, "Children");
                            R.Call(children, "Clear");
                            children.GetType().GetMethod("Add").Invoke(children, new[] { entity });
                            recorder.Update(Frame60);
                            recorder.Update(Frame60);
                            recorder.Writer.Write(OpItemSnapshot);
                            recorder.Writer.Write(item.Id);
                            recorder.Writer.Write(local);
                            W(recorder.Writer, (Matrix)R.Get(entity, "LocalToWorld"));
                            ++recorder.Ops; ++recorder.Snapshots;
                        }
                }
            });
        }
        return scenarios;
    }

    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: RigParityProbe <game directory> <output directory> [--reach]");
            return 2;
        }
        string gameDirectory = Path.GetFullPath(args[0]);
        string outputDirectory = Path.GetFullPath(args[1]);
        bool forceReach = args.Skip(2).Contains("--reach");
        Directory.CreateDirectory(outputDirectory);
        AppDomain.CurrentDomain.AssemblyResolve += (_, request) =>
        {
            string path = Path.Combine(gameDirectory, new AssemblyName(request.Name).Name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };

        Original original;
        try { original = Original.Create(gameDirectory, forceReach); }
        catch (Exception error)
        {
            Console.WriteLine("SKIP: no graphics device for the original content pipeline: " + error.Message);
            return 77;
        }

        var facts = new List<string>();
        facts.Add("game.version=" + original.Version + " generation=" + (original.Modern ? "modern" : "classic") +
                  " graphicsProfile=" + original.Profile);

        // 1. Which body does the game draw? Run Player's own static initialisation.
        original.LoadBody();
        facts.Add("== content loaded by Player's static initialisation");
        foreach (string line in original.PlayerContentLog) facts.Add("  " + line);
        DescribeModel(facts, "Player.ProxyModel = " + original.ProxyModelAsset, original.ProxyModel);

        // Every body the install ships, for the record.
        string contentRoot = Path.Combine(gameDirectory, "Content");
        foreach (string candidate in new[] { @"HiDefContent\Character\SWATMale", @"ReachContent\Character\SWATMale",
                                             @"HiDefContent\Character\ProxyBoy", @"ReachContent\Character\ProxyBoy" })
        {
            facts.Add("");
            if (!File.Exists(Path.Combine(contentRoot, candidate + ".xnb"))) { facts.Add("absent: " + candidate); continue; }
            try
            {
                var manager = new ContentManager(original.Content.ServiceProvider, contentRoot);
                Model variant = manager.Load<Model>(candidate);
                facts.Add("shipped " + candidate + ": bones=" + variant.Bones.Count + " meshes=" + variant.Meshes.Count +
                          " vertices=" + variant.Meshes.Sum(m => m.MeshParts.Sum(p => p.NumVertices)) + " triangles=" +
                          variant.Meshes.Sum(m => m.MeshParts.Sum(p => p.PrimitiveCount)) +
                          " farthestVertexFromTheLoadedBody=" + SurfaceDifference(original.ProxyModel, variant).ToString("R", Invariant));
                if (candidate.EndsWith("ProxyBoy")) DescribeModel(facts, candidate + " (shipped, never loaded by the game)", variant);
            }
            catch (Exception error) { facts.Add("shipped " + candidate + " not loadable on this profile: " + error.Message); }
        }

        // 2. The attachment skeleton and the name map onto the body's bones.
        Type avatarType = original.T("DNA.Avatars.Avatar");
        var names = ((IEnumerable)R.GetStatic(avatarType, "BoneNames")).Cast<string>().ToList();
        var parents = ((IEnumerable)R.GetStatic(avatarType, "DefaultParentBones")).Cast<int>().ToList();
        var bind = ((IEnumerable)R.GetStatic(avatarType, "DefaultBindPose")).Cast<Matrix>().ToList();
        var lookup = (IDictionary)R.GetStatic(avatarType, "boneNameLookup");
        facts.Add("");
        facts.Add("== Avatar attachment skeleton (DefaultBindPose)");
        for (int bone = 0; bone < names.Count; ++bone)
        {
            string native = lookup.Keys.Cast<string>().First(key => (int)lookup[key] == bone);
            facts.Add("  avatar " + bone + " " + names[bone] + " native=" + native + " parent=" + parents[bone] +
                      " bind=" + M(bind[bone]));
        }

        // 3. Clip registrations, read from the game's own IL, performed for real.
        original.RegisterClips();
        facts.Add("");
        facts.Add("== clip registrations (" + original.Registrations.Count + ")");
        foreach (ClipRegistration registration in original.Registrations)
        {
            object clip = original.Clips[registration.Name];
            var positions = (Vector3[][])R.Get(clip, "_positions");
            var rotations = (Quaternion[][])R.Get(clip, "_rotations");
            var scales = (Vector3[][])R.Get(clip, "_scales");
            bool[] influence = original.Influence(registration.Name);
            facts.Add("  " + registration.Name + " <- " + original.Content.Resolve(registration.Asset) + " looping=" +
                      registration.Looping + " roots=" +
                      (registration.Bones == null ? "all" : string.Join("+", registration.Bones.Select(b => names[b]))) +
                      (registration.MaskedBones == null ? "" : " minus=" + string.Join("+", registration.MaskedBones.Select(b => names[b]))) +
                      " influenced=" + (influence == null ? 71 : influence.Count(flag => flag)) +
                      " fps=" + R.Get(clip, "_animationFrameRate") + " duration=" +
                      ((TimeSpan)R.Get(clip, "Duration")).TotalSeconds.ToString("R", Invariant) + " bones=" + positions.Length +
                      " keys(max pos/rot/scale)=" + positions.Max(k => k.Length) + "/" + rotations.Max(k => k.Length) + "/" +
                      scales.Max(k => k.Length) + " rootTravel=" +
                      F(positions[0].Max(p => p.Y) - positions[0].Min(p => p.Y)) + " nonUnitScaleBones=" +
                      scales.Count(k => k.Any(s => s != Vector3.One)));
        }

        // 4. Constants the engine applies around the clips, read off real objects.
        Character sample = Character.Create(original);
        sample.Update(0);
        var proxyBones = ((IEnumerable)R.Get(sample.Proxy, "Skeleton")).Cast<object>().ToList();
        object eye = R.Get(sample.Avatar, "EyePointCamera");
        facts.Add("");
        facts.Add("== engine constants (after one real, empty update)");
        facts.Add("eyeToHead=" + M((Matrix)R.Get(sample.Avatar, "eyeToHead")));
        facts.Add("bodyRootAdjust=" + M(Matrix.Invert((Matrix)R.Get(((IEnumerable)R.Get(sample.Avatar, "Skeleton")).Cast<object>().First(), "Transform")) *
                                        ((Matrix[])R.Get(sample.Proxy, "DefaultPose"))[0]));
        facts.Add("AvatarRenderer.State=" + R.Get(R.Get(sample.Avatar, "AvatarRenderer"), "State"));
        facts.Add("Avatar.IsMale=" + R.Get(sample.Avatar, "IsMale") + " AvatarHeight=" + R.Get(sample.Avatar, "AvatarHeight"));
        facts.Add("EyePointCamera.FieldOfView(default)=" + R.Get(eye, "FieldOfView") + " near=" + R.Get(eye, "NearPlane") +
                  " far=" + R.Get(eye, "FarPlane"));
        facts.Add("body class=" + sample.Proxy.GetType().FullName);
        facts.Add("body bone -> attachment bone:");
        for (int bone = 0; bone < proxyBones.Count; ++bone)
        {
            string name = (string)R.Get(proxyBones[bone], "Name");
            facts.Add("  body " + bone + " " + name + " -> " + (lookup.Contains(name) ? lookup[name].ToString() : "(none)"));
        }

        // 5. Scenarios and the original's answers.
        var notes = new List<string>();
        List<HeldItem> items = HeldItems.Read(original, notes);
        List<KeyValuePair<string, byte[]>> scenarios = RecordScenarios(original, facts, items);

        // 6. Held items.
        facts.Add("");
        facts.Add("== held items: CreateEntity(ItemUse.Hand, local player).LocalToParent");
        foreach (string note in notes) facts.Add("  note " + note);
        foreach (HeldItem item in items)
            facts.Add("  item " + item.Id + " " + item.Name + " class=" + item.ItemClass + " mode=" + item.PlayerMode +
                      " entity=" + item.Entity + " children=" + item.Children + " hand=" + M(item.Hand) +
                      (item.Hand == item.HandRemote ? "" : " remote=" + M(item.HandRemote)));

        using (FileStream stream = File.Create(Path.Combine(outputDirectory, "rig-reference.bin")))
        using (var writer = new BinaryWriter(stream, Encoding.UTF8))
        {
            writer.Write(0x52524D4FU); // OMRR
            writer.Write(1);
            writer.Write(original.Modern ? "modern" : "classic");
            writer.Write(original.Version);
            writer.Write(original.Profile);
            writer.Write(proxyBones.Count);
            writer.Write(scenarios.Count);
            foreach (var entry in scenarios) writer.Write(entry.Value);
            writer.Write(items.Count);
            foreach (HeldItem item in items)
            {
                writer.Write(item.Id);
                writer.Write(item.Name);
                writer.Write(item.PlayerMode);
                W(writer, item.Hand);
                W(writer, item.HandRemote);
            }
        }

        File.WriteAllLines(Path.Combine(outputDirectory, "rig-facts.txt"), facts);
        Console.WriteLine("Original " + original.Version + " (" + original.Profile + ", " + original.ProxyModelAsset + "): " +
                          original.Registrations.Count + " registered clips, " + proxyBones.Count + " body bones, " +
                          scenarios.Count + " scenarios, " + items.Count + " held items");
        return 0;
    }
}
