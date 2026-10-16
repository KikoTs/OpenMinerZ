using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using Microsoft.Xna.Framework;

// The local player's own movement. A stock host believes whatever position a
// client reports, so a client that walks, jumps, climbs or falls differently
// from the original is visibly out of step with every stock player.
//
// Executed for real (original IL), in the frame order of the shipped game:
//   Player.ProcessInput            the WHOLE method: Speed, FPSRig.ProcessInput
//                                  (UpdateRotation, UpdateVelocity, Jump) and
//                                  then Player.UpdateAnimation, which
//                                  OVERWRITES the velocity FPSRig just wrote
//                                  whenever the player is on the ground
//   BasicPhysics.Accelerate        gravity
//   Player.ResolveCollsion         EnemyManager.AttentuateVelocity, the swept
//                                  AABBTraceProbe with SimulateSlopedSides
//                                  (AutoClimb), the slide loop, InContact and
//                                  the fall damage sent to InGameHUD.ApplyDamage
// UpdateAnimation needs an avatar to animate. The player gets a real Avatar
// entity with a real AvatarAnimationCollection; AvatarAnimationCollection.Play
// is redirected to install a clip-less stand-in player under the requested
// name, because the avatar clip library is Xbox avatar content that only a
// running game loads. Nothing about velocity is touched by that.
// Set by the probe: WorldAcceleration = gravity, which Player.OnUpdate assigns
// each frame (read, not executed: it also drives audio and terrain scrolling).
// Blocking enemies are real BaseZombie objects created through the original
// spawn message and left in their emerge state (IsBlocking) where the script
// puts them.
internal struct PlayerInputSample
{
    public float Strafe, Forward;
    public bool Jump;
    public bool Sprint;
    // CastleMinerZControllerMapping.FlyMode.Pressed for this frame. Both
    // generations bind the key; only 1.9.9.8 Player.ProcessInput reads it.
    public bool Fly;
}

internal sealed class PlayerScenario
{
    public string Name;
    public Vector3 Start;
    public float Yaw;
    public bool AutoClimb = true;
    public long Ticks = 166667;
    public float Seconds = 6f;
    public List<Box> Boxes = new List<Box>();
    public List<Vector3> Blockers = new List<Vector3>();
    public Func<float, PlayerInputSample> Input;
    // CastleMinerZGame.GameMode for the scenario (Survival unless set).
    public bool Creative;
    // FPSRig.TorsoPitch, radians above the horizon. Flight follows the camera,
    // so it matters there; walking ignores it.
    public float Pitch;
}

internal static class Players
{
    private static object player;
    private static MethodInfo updateVelocity;
    private static MethodInfo resolve;
    private static MethodInfo processInput;
    private static Type mappingType;
    private static object standInClip;

    // AvatarAnimationCollection.Play(id, channel, blend): the original fetches
    // the named avatar clip; this installs an AnimationPlayer of that name over
    // an unrelated real clip so the callers' Name / Finished / Progress reads work.
    private static object StandInAnimation(object self, string id, int channel, TimeSpan blend)
    {
        object animation = Activator.CreateInstance(Rig.T("DNA.Drawing.Animation.AnimationPlayer"), standInClip);
        R.Set(animation, "Name", id);
        R.Call(self, "PlayAnimation", channel, animation, blend);
        return animation;
    }

    public static void Setup()
    {
        Type playerType = Rig.T("DNA.CastleMinerZ.Player");
        player = Rig.LocalPlayer;
        // Player..ctor: Collider = true; Physics = new NoMovePhysics(this).
        R.Set(player, "Collider", true);
        Type noMove = playerType.GetNestedType("NoMovePhysics", BindingFlags.Public | BindingFlags.NonPublic);
        R.Set(player, "Physics", Activator.CreateInstance(noMove, player));
        updateVelocity = R.Method(Rig.T("DNA.Drawing.FPSRig"), "UpdateVelocity", 2);
        resolve = playerType.GetMethod("ResolveCollsion", BindingFlags.Instance | BindingFlags.Public);
        processInput = playerType.GetMethod("ProcessInput", BindingFlags.Instance | BindingFlags.Public);
        mappingType = Rig.TryT("DNA.CastleMinerZ.CastleMinerZControllerMapping") ?? updateVelocity.GetParameters()[0].ParameterType;

        // What the field initializers and constructor of Player give UpdateAnimation.
        Type angle = Rig.T("DNA.Angle");
        Func<float, object> degrees = value => R.CallStatic(angle, "FromDegrees", value);
        R.Set(player, "DefaultFOV", degrees(73f));
        R.Set(player, "DefaultAvatarFOV", degrees(90f));
        R.Set(player, "ShoulderedAvatarFOV", degrees(45f));
        R.Set(player, "GunEyePointCamera", Activator.CreateInstance(R.Field(playerType, "GunEyePointCamera").FieldType));
        R.Set(player, "_playerMode", Enum.Parse(R.Field(playerType, "_playerMode").FieldType, "Fist"));
        R.Set(player, "underWaterSpeed", 1.894f);
        if (R.Has(playerType, "_sprintMultiplier")) R.Set(player, "_sprintMultiplier", 2f);
        if (R.Has(playerType, "JumpCountLimit")) R.Set(player, "JumpCountLimit", 2);
        if (R.Has(playerType, "m_baseJumpCost")) R.Set(player, "m_baseJumpCost", 0.25f);
        if (R.Has(playerType, "m_baseSprintCost")) R.Set(player, "m_baseSprintCost", 0.6f);

        Type avatarType = Rig.T("DNA.Avatars.Avatar");
        object avatar = R.Inert(avatarType);
        Rig.T("DNA.Drawing.Entity").GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                   null, Type.EmptyTypes, null).Invoke(avatar, null);
        Type collection = Rig.T("DNA.Avatars.AvatarAnimationCollection");
        R.Set(avatar, "_animations", Activator.CreateInstance(collection, avatar));
        R.Set(avatar, "EyePointCamera", Activator.CreateInstance(R.Field(avatarType, "EyePointCamera").FieldType));
        // Not parented to the player: Player.FPSMode is `!Children.Contains(Avatar)`,
        // and the local player is always in first person.
        R.Set(player, "Avatar", avatar);
        Rig.Redirect(collection.GetMethods(R.All).First(method => method.Name == "Play" && method.GetParameters().Length == 3),
                     typeof(Players).GetMethod("StandInAnimation", BindingFlags.Static | BindingFlags.NonPublic));
        // Any real clip: the stand-in players are never sampled.
        object enemyTypes = R.GetStatic(Rig.T("DNA.CastleMinerZ.AI.EnemyType"), "Types");
        object zombieKind = ((Array)enemyTypes).GetValue(0);
        var model = (Microsoft.Xna.Framework.Graphics.Model)(R.Has(zombieKind.GetType(), "Model")
            ? R.Get(zombieKind, "Model")
            : ((Microsoft.Xna.Framework.Game)Rig.GameInstance).Content.Load<Microsoft.Xna.Framework.Graphics.Model>((string)R.Get(zombieKind, "ModelName")));
        standInClip = ((IDictionary)R.Get(model.Tag, "AnimationClips")).Values.Cast<object>().First();
        // Player.SetupAnimation: "Tilt" on channel 1; the body starts on "Stand".
        R.Set(player, "_torsoPitchAnimation", StandInAnimation(R.Get(avatar, "Animations"), "Tilt", 1, TimeSpan.Zero));
        StandInAnimation(R.Get(avatar, "Animations"), "Stand", 0, TimeSpan.Zero);

        // InGameHUD.ActiveInventoryItem -> LocalPlayer.PlayerInventory: empty hands.
        object hud = R.GetStatic(Rig.T("DNA.CastleMinerZ.UI.InGameHUD"), "Instance");
        R.Set(hud, "_game", Rig.GameInstance);
        Type inventoryType = R.Field(playerType, "PlayerInventory").FieldType;
        object inventory = R.Inert(inventoryType);
        foreach (FieldInfo field in inventoryType.GetFields(R.All))
            if (!field.IsStatic && field.FieldType.IsArray && field.Name == "InventoryTray")
                field.SetValue(inventory, Array.CreateInstance(field.FieldType.GetElementType(), 8));
        if (R.Has(inventoryType, "TrayManager"))
        {
            // 1.9.9.8: two trays behind a manager, and the getter writes back
            // to the owning player.
            R.Set(inventory, "TrayManager", Activator.CreateInstance(R.Field(inventoryType, "TrayManager").FieldType));
            R.Set(inventory, "_player", player);
        }
        R.Set(player, "PlayerInventory", inventory);
        if (R.Has(hud.GetType(), "PlayerStamina"))
        {
            // Player.CanJump reaches the HUD through GameScreen.HUD.
            Type screenType = R.Field(Rig.GameInstance.GetType(), "GameScreen").FieldType;
            object screen = R.Inert(screenType);
            R.Set(screen, "_inGameUI", hud);
            R.Set(Rig.GameInstance, "GameScreen", screen);
            // 1.9.9.8 stamina, as InGameHUD's field initializers leave it once
            // the two timers have run out: a full bar that may be spent.
            R.Set(hud, "PlayerStamina", 1f);
            R.Set(hud, "StaminaRecoverRate", 0.25f);
            foreach (string timer in new[] { "StaminaRecoverTimer", "StaminaBlockTimer" })
            {
                // InGameHUD: StaminaRecoverTimer is one second, StaminaBlockTimer two.
                object value = Activator.CreateInstance(R.Field(hud.GetType(), timer).FieldType,
                                                        TimeSpan.FromSeconds(timer == "StaminaRecoverTimer" ? 1.0 : 2.0));
                R.Call(value, "Update", TimeSpan.FromSeconds(10.0));
                R.Set(hud, timer, value);
            }
        }
    }

    // InGameHUD's own per-frame stamina recovery (1.9.9.8), transcribed: the
    // HUD update that owns it also drives the whole interface.
    private static void RecoverStamina(TimeSpan elapsed)
    {
        object hud = R.GetStatic(Rig.T("DNA.CastleMinerZ.UI.InGameHUD"), "Instance");
        if (!R.Has(hud.GetType(), "PlayerStamina")) return;
        object block = R.Get(hud, "StaminaBlockTimer"), recover = R.Get(hud, "StaminaRecoverTimer");
        R.Call(block, "Update", elapsed);
        R.Call(recover, "Update", elapsed);
        float stamina = (float)R.Get(hud, "PlayerStamina");
        if (stamina < 1f)
        {
            float rate = (bool)R.Get(recover, "Expired")
                ? (float)R.Get(hud, "StaminaRecoverRate")
                : (float)R.Get(hud, "StaminaRecoverRate") * (float)R.Get(hud, "StaminaDamagedRecoveryModifier");
            R.Set(hud, "PlayerStamina", Math.Min(1f, stamina + rate * (float)elapsed.TotalSeconds));
        }
    }

    public static float Stamina()
    {
        object hud = R.GetStatic(Rig.T("DNA.CastleMinerZ.UI.InGameHUD"), "Instance");
        return R.Has(hud.GetType(), "PlayerStamina") ? (float)R.Get(hud, "PlayerStamina") : 1f;
    }

    private static string TakeEvents()
    {
        string joined = Rig.Events.Count == 0 ? "-" : string.Join(";", Rig.Events);
        Rig.Events.Clear();
        return joined;
    }

    public static void Run(PlayerScenario scenario, TextWriter output)
    {
        Rig.ClearTerrain();
        foreach (Box box in scenario.Boxes) Rig.Fill(box.X0, box.Y0, box.Z0, box.X1, box.Y1, box.Z1, box.Block);
        Enemies.Clear();
        foreach (Vector3 blocker in scenario.Blockers) Enemies.SpawnBlocker(blocker);
        Rig.Drain();
        Rig.Events.Clear();
        R.Set(Rig.Stats, "AutoClimb", scenario.AutoClimb);

        object physics = Rig.Physics(player);
        R.Set(physics, "WorldAcceleration", (Vector3)R.GetStatic(Rig.T("DNA.Drawing.BasicPhysics"), "Gravity"));
        R.Set(physics, "WorldVelocity", Vector3.Zero);
        R.Set(player, "LocalPosition", scenario.Start);
        // Our yaw: forward is (sin yaw, -cos yaw). A rotation of -yaw about Y
        // gives the original rig that Forward.
        R.Set(player, "LocalRotation", Quaternion.CreateFromAxisAngle(Vector3.UnitY, -scenario.Yaw));
        R.Set(player, "InContact", true);
        R.Set(player, "Dead", false);
        R.Set(player, "Speed", 5f);
        // Player..ctor: FlyMode = false. Nothing but ProcessInput may change it.
        R.Set(player, "_flyMode", false);
        Type gameType = Rig.GameInstance.GetType();
        R.Set(Rig.GameInstance, "GameMode",
              Enum.Parse(R.Field(gameType, "GameMode").FieldType, scenario.Creative ? "Creative" : "Survival"));
        // FPSRig.UpdateRotation accumulates TorsoPitch from the stick and
        // FPSRig.OnUpdate turns the camera's pitch pivot to it:
        //     pitchPiviot.LocalRotation = CreateFromAxisAngle(UnitX, TorsoPitch.Radians)
        // (transcribed: OnUpdate is the virtual that also runs Player.OnUpdate).
        R.Set(player, "TorsoPitch", R.CallStatic(Rig.T("DNA.Angle"), "FromRadians", scenario.Pitch));
        R.Set(R.Get(player, "pitchPiviot"), "LocalRotation",
              Quaternion.CreateFromAxisAngle(Vector3.UnitX, scenario.Pitch));
        if (R.Has(player.GetType(), "m_jumpCount")) R.Set(player, "m_jumpCount", 0);
        object hud = R.GetStatic(Rig.T("DNA.CastleMinerZ.UI.InGameHUD"), "Instance");
        if (R.Has(hud.GetType(), "PlayerStamina"))
        {
            R.Set(hud, "PlayerStamina", 1f);
            R.Call(R.Get(hud, "StaminaRecoverTimer"), "Update", TimeSpan.FromSeconds(10.0));
            R.Call(R.Get(hud, "StaminaBlockTimer"), "Update", TimeSpan.FromSeconds(10.0));
        }

        int frames = (int)Math.Round(scenario.Seconds * 1e7 / scenario.Ticks);
        output.WriteLine("SCENARIO player " + scenario.Name);
        output.WriteLine("PLAYER pos=" + Rig.V(scenario.Start) + " yaw=" + Rig.F(scenario.Yaw) + " autoclimb=" +
                         (scenario.AutoClimb ? 1 : 0) + " ticks=" + scenario.Ticks + " frames=" + frames + " modern=" +
                         (Rig.Modern ? 1 : 0) + " speed=" + Rig.F((float)R.Get(player, "Speed")) + " jump=" +
                         Rig.F((float)R.Get(player, "JumpImpulse")) + " creative=" + (scenario.Creative ? 1 : 0) +
                         " pitch=" + Rig.F(scenario.Pitch) + " flykey=" + (R.Has(mappingType, "FlyMode") ? 1 : 0));
        foreach (Box box in scenario.Boxes)
            output.WriteLine("BOX " + box.X0 + " " + box.Y0 + " " + box.Z0 + " " + box.X1 + " " + box.Y1 + " " + box.Z1 + " " + box.Block);
        foreach (Vector3 blocker in scenario.Blockers) output.WriteLine("BLOCKER " + Rig.V(blocker));

        var elapsed = TimeSpan.FromTicks(scenario.Ticks);
        bool jumpWasHeld = false;
        for (int frame = 0; frame < frames; ++frame)
        {
            float seconds = (float)(frame * (double)scenario.Ticks * 1e-7);
            var time = new GameTime(TimeSpan.FromTicks(scenario.Ticks * (frame + 1)), elapsed);
            R.Set(Rig.GameInstance, "CurrentGameTime", time);
            Program.FrameSeconds = (float)(scenario.Ticks * 1e-7);
            PlayerInputSample input = scenario.Input(seconds);
            output.WriteLine("FRAME " + frame + " strafe=" + Rig.F(input.Strafe) + " forward=" + Rig.F(input.Forward) +
                             " jump=" + (input.Jump ? 1 : 0) + " sprint=" + (input.Sprint ? 1 : 0) +
                             " fly=" + (input.Fly ? 1 : 0));
            object mapping = R.Inert(mappingType);
            R.Set(mapping, "Movement", new Vector2(input.Strafe, input.Forward));
            object trigger = Activator.CreateInstance(R.Field(mappingType, "Jump").FieldType);
            // A Trigger is Pressed on the frame it goes down and Held for as
            // long as it stays down (fly mode rises on Held).
            R.Set(trigger, "Pressed", input.Jump && !jumpWasHeld);
            R.Set(trigger, "Held", input.Jump);
            jumpWasHeld = input.Jump;
            R.Set(mapping, "Jump", trigger);
            if (R.Has(mappingType, "FlyMode"))
            {
                // The key is bound in both generations (function 13 / 15).
                object fly = Activator.CreateInstance(R.Field(mappingType, "FlyMode").FieldType);
                R.Set(fly, "Pressed", input.Fly);
                R.Set(fly, "Held", input.Fly);
                R.Set(mapping, "FlyMode", fly);
            }
            if (R.Has(mappingType, "Sprint"))
            {
                // 1.9.9.8 sprints while a movement key and the sprint key are held.
                Action<string, bool> hold = (name, held) =>
                {
                    // CastleMinerZControllerMapping re-declares Sprint with `new`;
                    // Player.ProcessInput reads the FPSControllerMapping one.
                    for (Type level = mappingType; level != null; level = level.BaseType)
                    {
                        FieldInfo field = level.GetField(name, R.All);
                        if (field == null) continue;
                        object key = Activator.CreateInstance(field.FieldType);
                        R.Set(key, "Held", held);
                        field.SetValue(mapping, key);
                    }
                };
                hold("Sprint", input.Sprint);
                hold("MoveForward", input.Forward > 0f);
                hold("MoveBackward", input.Forward < 0f);
                hold("StrafeLeft", input.Strafe < 0f);
                hold("StrafeRight", input.Strafe > 0f);
            }
            R.Invoke(processInput, player, new[] { mapping, (object)time });
            // Player.OnUpdate, which runs before the scene's physics each frame
            // (transcribed, as above):
            //     WorldAcceleration = _flyMode ? Vector3.Zero : Gravity   (no water)
            bool flying = (bool)R.Get(player, "_flyMode");
            R.Set(physics, "WorldAcceleration",
                  flying ? Vector3.Zero : (Vector3)R.GetStatic(Rig.T("DNA.Drawing.BasicPhysics"), "Gravity"));
            R.Invoke(R.Method(physics.GetType(), "Accelerate", 1), physics, new object[] { elapsed });
            R.Invoke(resolve, player, new object[] { Rig.Terrain, null, time });
            RecoverStamina(elapsed);
            output.WriteLine("OUT " + frame + " p=" + Rig.V((Vector3)R.Get(player, "LocalPosition")) + " v=" +
                             Rig.V(Rig.Velocity(player)) + " c=" + ((bool)R.Get(player, "InContact") ? 1 : 0) +
                             " stam=" + Rig.F(Stamina()) + " ev=" + TakeEvents() + " fm=" + (flying ? 1 : 0));
        }
        output.WriteLine("END");
        Enemies.Clear();
        R.Set(Rig.Stats, "AutoClimb", true);
        R.Set(player, "_flyMode", false);
        R.Set(Rig.GameInstance, "GameMode",
              Enum.Parse(R.Field(Rig.GameInstance.GetType(), "GameMode").FieldType, "Survival"));
        R.Set(player, "TorsoPitch", R.CallStatic(Rig.T("DNA.Angle"), "FromRadians", 0f));
        R.Set(R.Get(player, "pitchPiviot"), "LocalRotation", Quaternion.Identity);
    }
}
