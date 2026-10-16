using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using Microsoft.Xna.Framework;

// Dropped items. Every peer flies each pickup itself from the position and
// vector in CreatePickupMessage, and whoever stands within two blocks of where
// ITS copy came to rest may collect it.
//
// Executed for real (original IL), in the frame order of Scene.OnUpdate:
//   BasicPhysics.Accelerate         a quarter of gravity
//   PickupEntity.ResolveCollsion    the swept 0.6 box and its slide loop
//   PickupEntity.OnUpdate           the per-frame damping (0.9 on the ground,
//                                   0.99 in the air, zero under 0.1), the
//                                   life timer and the bob
// The entity is built without its constructor (which needs the item's model):
// the probe sets exactly what the constructor sets for motion - the collision
// box, the movement probe, NoMovePhysics with Gravity * 0.25 and a plain
// display entity - and marks it already picked up so OnUpdate never asks an
// absent PickupManager to collect it.
internal sealed class PickupScenario
{
    public string Name;
    public Vector3 Start;
    public Vector3 Velocity;
    public long Ticks = 166667;
    public float Seconds = 5f;
    public List<Box> Boxes = new List<Box>();
}

internal static class Pickups
{
    public static void Run(PickupScenario scenario, TextWriter output)
    {
        Rig.ClearTerrain();
        foreach (Box box in scenario.Boxes) Rig.Fill(box.X0, box.Y0, box.Z0, box.X1, box.Y1, box.Z1, box.Block);
        Rig.MoveEntity(Rig.LocalPlayer, new Vector3(0f, 50f, 0f));
        R.Set(Rig.LocalPlayer, "Dead", false);

        Type type = Rig.T("DNA.CastleMinerZ.PickupEntity");
        Type entityType = Rig.T("DNA.Drawing.Entity");
        ConstructorInfo entityConstructor = entityType.GetConstructor(
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null);
        object pickup = R.Inert(type);
        entityConstructor.Invoke(pickup, null);
        R.Set(pickup, "MovementProbe", Activator.CreateInstance(R.Field(type, "MovementProbe").FieldType));
        R.Set(pickup, "CollisionAABB", new BoundingBox(new Vector3(-0.3f, -0.3f, -0.3f), new Vector3(0.3f, 0.3f, 0.3f)));
        R.Set(pickup, "_displayEntity", Activator.CreateInstance(entityType));
        R.Set(pickup, "_timeLeft", 600f);
        R.Set(pickup, "_pickedUp", true);
        R.Set(pickup, "Collider", true);
        Type noMove = Rig.T("DNA.CastleMinerZ.Player").GetNestedType("NoMovePhysics", BindingFlags.Public | BindingFlags.NonPublic);
        object physics = Activator.CreateInstance(noMove, pickup);
        R.Set(pickup, "Physics", physics);
        R.Set(physics, "WorldAcceleration", (Vector3)R.GetStatic(Rig.T("DNA.Drawing.BasicPhysics"), "Gravity") * 0.25f);
        R.Set(pickup, "LocalPosition", scenario.Start);
        R.Set(physics, "LocalVelocity", scenario.Velocity);

        MethodInfo resolve = type.GetMethod("ResolveCollsion", BindingFlags.Instance | BindingFlags.Public);
        MethodInfo update = R.Method(type, "OnUpdate", 1);
        int frames = (int)Math.Round(scenario.Seconds * 1e7 / scenario.Ticks);
        output.WriteLine("SCENARIO pickup " + scenario.Name);
        output.WriteLine("PICKUP pos=" + Rig.V(scenario.Start) + " vel=" + Rig.V(scenario.Velocity) + " ticks=" +
                         scenario.Ticks + " frames=" + frames + " modern=" + (Rig.Modern ? 1 : 0));
        foreach (Box box in scenario.Boxes)
            output.WriteLine("BOX " + box.X0 + " " + box.Y0 + " " + box.Z0 + " " + box.X1 + " " + box.Y1 + " " + box.Z1 + " " + box.Block);
        var elapsed = TimeSpan.FromTicks(scenario.Ticks);
        for (int frame = 0; frame < frames; ++frame)
        {
            var time = new GameTime(TimeSpan.FromTicks(scenario.Ticks * (frame + 1)), elapsed);
            R.Invoke(R.Method(physics.GetType(), "Accelerate", 1), physics, new object[] { elapsed });
            R.Invoke(resolve, pickup, new object[] { Rig.Terrain, null, time });
            R.Invoke(update, pickup, new object[] { time });
            output.WriteLine("OUT " + frame + " p=" + Rig.V((Vector3)R.Get(pickup, "LocalPosition")) + " v=" +
                             Rig.V(Rig.Velocity(pickup)) + " g=" + ((bool)R.Get(pickup, "OnGround") ? 1 : 0));
        }
        output.WriteLine("END");
    }
}
