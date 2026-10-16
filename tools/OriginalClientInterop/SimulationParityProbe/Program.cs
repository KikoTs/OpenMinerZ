using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Microsoft.Xna.Framework;

// Differential probe for everything a peer SIMULATES FOR ITSELF: ordinary
// enemies, the local player's movement and survival, and thrown or fired
// objects. None of these are position-replicated, so a stock client and
// OpenMinerZ only agree about them if both run the same simulation.
//
// Usage: OpenMinerZ.SimulationParityProbe <game directory> <output directory> [name filter]
//
// Each scenario is written as a self-contained trace: its inputs (spawn message
// fields, terrain boxes, clip lengths, the scripted target path frame by
// frame) and what the original did with them. tests/test_simulation_parity.cpp
// replays the same inputs through the native simulation and compares.
// Exit code 77 means no graphics device was available (the original content
// reader needs one to load models); the CTest treats that as a skip.
internal static class Program
{
    private const int Rock = 6;
    private const int Dirt = 1;
    private const int Bedrock = 14;

    private static TargetSample Standing(Vector3 position)
    {
        return new TargetSample { Position = position, Velocity = Vector3.Zero, Contact = true };
    }

    private static List<Box> Floor()
    {
        return new List<Box> { new Box(-40, -3, -40, 60, -1, 40, Rock) };
    }

    private static IEnumerable<EnemyScenario> EnemyScenarios()
    {
        // The slowest surface zombie walking 12 blocks across flat rock to a
        // standing player, then attacking until the clock runs out.
        yield return new EnemyScenario
        {
            Name = "zombie0_flat", Type = 0, Seed = 12345, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 0.9f, Fast = 6.4f, Emerge = 3f, RunActivation = 3.8f, NormalActivation = 45f, Seconds = 40f,
            Boxes = Floor(), Target = time => Standing(new Vector3(12.5f, 0.001f, 0.5f))
        };
        // The same enemy as another player's client sees it (target not local).
        yield return new EnemyScenario
        {
            Name = "zombie0_flat_remote", Type = 0, Seed = 12345, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 0.9f, Fast = 6.4f, Emerge = 3f, RunActivation = 3.8f, NormalActivation = 45f, Seconds = 40f,
            Local = false, Boxes = Floor(), Target = time => Standing(new Vector3(12.5f, 0.001f, 0.5f))
        };
        // Frame-rate baselines: the original steps with the display.
        foreach (long ticks in new long[] { 69444, 333333, 41667 })
            yield return new EnemyScenario
            {
                Name = "zombie0_flat@" + ticks, Type = 0, Seed = 12345, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
                Slow = 0.9f, Fast = 6.4f, Emerge = 3f, RunActivation = 3.8f, NormalActivation = 45f, Seconds = 40f,
                Ticks = ticks, Boxes = Floor(), Target = time => Standing(new Vector3(12.5f, 0.001f, 0.5f))
            };
        // A mid-tier zombie whose target runs away, stops, and is caught: the
        // run-activation timer, SpeedUpEnemyMessage and the fast gait.
        Func<float, TargetSample> runner = time =>
        {
            float moving = Math.Max(0f, Math.Min(time - 5f, 7f));
            var sample = new TargetSample
            {
                Position = new Vector3(6.5f + 2.8f * moving, 0.001f, 0.5f + 1.1f * moving), Contact = true
            };
            sample.Velocity = time >= 5f && time < 12f ? new Vector3(2.8f, 0f, 1.1f) : Vector3.Zero;
            return sample;
        };
        yield return new EnemyScenario
        {
            Name = "zombie8_runner", Type = 8, Seed = 777, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 1.6f, Fast = 7.3f, Emerge = 2.2f, RunActivation = 3.1f, NormalActivation = 45f, Seconds = 30f,
            Boxes = Floor(), Target = runner
        };
        foreach (long ticks in new long[] { 69444, 333333 })
            yield return new EnemyScenario
            {
                Name = "zombie8_runner@" + ticks, Type = 8, Seed = 777, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
                Slow = 1.6f, Fast = 7.3f, Emerge = 2.2f, RunActivation = 3.1f, NormalActivation = 45f, Seconds = 30f,
                Ticks = ticks, Boxes = Floor(), Target = runner
            };
        // A target that strolls in a circle just out of reach: the 45-second
        // patience timer (NormalActivationTime) is what speeds the zombie up.
        Func<float, TargetSample> stroller = time =>
        {
            float angle = 0.2f * time;
            return new TargetSample
            {
                Position = new Vector3(10.5f + 6f * (float)Math.Cos(angle), 0.001f, 0.5f + 6f * (float)Math.Sin(angle)),
                Velocity = new Vector3(-1.2f * (float)Math.Sin(angle), 0f, 1.2f * (float)Math.Cos(angle)),
                Contact = true
            };
        };
        yield return new EnemyScenario
        {
            Name = "zombie5_patience", Type = 5, Seed = 4545, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 1.0f, Fast = 6.9f, Emerge = 3f, RunActivation = 3.5f, NormalActivation = 45f, Seconds = 60f,
            Boxes = Floor(), Target = stroller
        };
        // The midnight frenzy: EnemyManager.ZombieFestIsOn speeds every zombie up at once.
        yield return new EnemyScenario
        {
            Name = "zombie5_fest", Type = 5, Seed = 4546, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 1.0f, Fast = 6.9f, Emerge = 3f, RunActivation = 3.5f, NormalActivation = 45f, Seconds = 20f,
            Fest = true, Boxes = Floor(), Target = time => Standing(new Vector3(18.5f, 0.001f, -6.5f))
        };
        // Killed while still clawing out of the ground: one scenario per death clip.
        foreach (int seed in new[] { 101, 102, 103, 104, 105, 106 })
        {
            var dies = new EnemyScenario
            {
                Name = "zombie2_dies_" + seed, Type = 2, Seed = seed, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
                Slow = 1.0f, Fast = 6.6f, Emerge = 3f, RunActivation = 3.7f, NormalActivation = 45f, Seconds = 8f,
                Boxes = Floor(), Target = time => Standing(new Vector3(9.5f, 0.001f, 0.5f))
            };
            dies.Actions[0.2f] = "kill";
            yield return dies;
        }
        // Steps, a two-block wall and a pit between the enemy and its target.
        Func<List<Box>> course = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(4, 0, -6, 6, 0, 6, Rock));     // one-block step up
            boxes.Add(new Box(10, 0, -6, 10, 1, 6, Rock));   // two-block wall
            boxes.Add(new Box(14, -3, -6, 15, -1, 6, 0));    // three-deep pit
            boxes.Add(new Box(14, -5, -6, 15, -4, 6, Rock)); // its floor
            return boxes;
        };
        yield return new EnemyScenario
        {
            Name = "zombie17_course", Type = 17, Seed = 4242, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 2.3f, Fast = 8.0f, Emerge = 3f, RunActivation = 2.9f, NormalActivation = 45f, Seconds = 30f,
            Boxes = course(), Target = time => Standing(new Vector3(20.5f, 0.001f, 1.25f))
        };
        foreach (long ticks in new long[] { 69444, 333333 })
            yield return new EnemyScenario
            {
                Name = "zombie17_course@" + ticks, Type = 17, Seed = 4242, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
                Slow = 2.3f, Fast = 8.0f, Emerge = 3f, RunActivation = 2.9f, NormalActivation = 45f, Seconds = 30f,
                Ticks = ticks, Boxes = course(), Target = time => Standing(new Vector3(20.5f, 0.001f, 1.25f))
            };
        // Hit reactions while chasing and while attacking, then a kill.
        var hits = new EnemyScenario
        {
            Name = "zombie3_hits", Type = 3, Seed = 9001, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 1.1f, Fast = 6.7f, Emerge = 3f, RunActivation = 3.6f, NormalActivation = 45f, Seconds = 30f,
            Boxes = Floor(), Target = time => Standing(new Vector3(6.5f, 0.001f, 0.5f))
        };
        hits.Actions[2.0f] = "hit:0.5";
        hits.Actions[2.1f] = "hit:0.5";
        hits.Actions[9.0f] = "hit:0.5";
        hits.Actions[14.0f] = "hit:0.5";
        hits.Actions[20.0f] = "kill";
        yield return hits;
        // The target dies: every peer makes the enemy give up.
        yield return new EnemyScenario
        {
            Name = "zombie0_target_dies", Type = 0, Seed = 31337, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 0.8f, Fast = 6.5f, Emerge = 3f, RunActivation = 4f, NormalActivation = 45f, Seconds = 15f,
            Boxes = Floor(),
            Target = time =>
            {
                TargetSample sample = Standing(new Vector3(9.5f, 0.001f, 0.5f));
                sample.Dead = time >= 4f;
                return sample;
            }
        };
        // Out of range (25 blocks): ZombieChase -> ZombieDig.Enter -> give up.
        yield return new EnemyScenario
        {
            Name = "zombie0_too_far", Type = 0, Seed = 5, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 0.8f, Fast = 6.5f, Emerge = 3f, RunActivation = 4f, NormalActivation = 45f, Seconds = 15f,
            Boxes = Floor(), Target = time => Standing(new Vector3(28.5f, 0.001f, 3.5f))
        };
        // Walled in: frustration runs out and the zombie digs through dirt.
        Func<List<Box>> cell = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(-2, 0, -2, 3, 3, 3, Dirt));
            boxes.Add(new Box(-1, 0, -1, 2, 2, 2, 0));
            return boxes;
        };
        yield return new EnemyScenario
        {
            Name = "zombie17_walled_in", Type = 17, Seed = 8, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 2.3f, Fast = 8.0f, Emerge = 3f, RunActivation = 2.9f, NormalActivation = 45f, Seconds = 40f,
            Boxes = cell(), Target = time => Standing(new Vector3(9.5f, 0.001f, 0.5f))
        };
        // A target on a six-block tower: the dig state chews at its base.
        Func<List<Box>> tower = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(8, 0, 0, 8, 5, 0, Dirt));
            return boxes;
        };
        yield return new EnemyScenario
        {
            Name = "zombie10_tower", Type = 10, Seed = 99, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 1.7f, Fast = 7.4f, Emerge = 3f, RunActivation = 3.3f, NormalActivation = 45f, Seconds = 40f,
            Boxes = tower(), Target = time => Standing(new Vector3(8.5f, 6.001f, 0.5f))
        };
        // Cave skeletons: no emerge clip, no fast gait, give up instead of digging.
        foreach (int type in new[] { 26, 31, 38, 49 })
            yield return new EnemyScenario
            {
                Name = "skeleton" + type + "_flat", Type = type, Seed = 1000 + type, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
                Slow = 2f + 0.07f * (type - 26), Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f,
                Seconds = 30f, Boxes = Floor(), Target = time => Standing(new Vector3(10.5f, 0.001f, -3.5f))
            };
        yield return new EnemyScenario
        {
            Name = "skeleton26_flat@69444", Type = 26, Seed = 1026, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 2f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Ticks = 69444,
            Seconds = 30f, Boxes = Floor(), Target = time => Standing(new Vector3(10.5f, 0.001f, -3.5f))
        };
        yield return new EnemyScenario
        {
            Name = "skeleton30_course", Type = 30, Seed = 606, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 4.4f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 25f,
            Boxes = course(), Target = time => Standing(new Vector3(20.5f, 0.001f, 1.25f))
        };
        var skeletonHits = new EnemyScenario
        {
            Name = "skeleton33_hits", Type = 33, Seed = 71, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 3.1f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 25f,
            Boxes = Floor(), Target = time => Standing(new Vector3(8.5f, 0.001f, 0.5f))
        };
        skeletonHits.Actions[1.0f] = "hit:0.5";
        skeletonHits.Actions[6.0f] = "hit:0.5";
        skeletonHits.Actions[12.0f] = "kill";
        yield return skeletonHits;
        yield return new EnemyScenario
        {
            Name = "skeleton26_too_far", Type = 26, Seed = 3, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 2.5f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 15f,
            Boxes = Floor(), Target = time => Standing(new Vector3(27.5f, 0.001f, 0.5f))
        };
        // Archers: stand up, close to a clear line of sight, shoot, idle, repeat.
        yield return new EnemyScenario
        {
            Name = "archer18_open", Type = 18, Seed = 2024, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 3.2f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 60f,
            Boxes = Floor(), Target = time => Standing(new Vector3(20.5f, 0.001f, 4.5f))
        };
        Func<List<Box>> blind = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(10, 0, -8, 10, 3, 3, Rock));
            return boxes;
        };
        yield return new EnemyScenario
        {
            Name = "archer23_behind_wall", Type = 23, Seed = 515, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 2.6f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 40f,
            Boxes = blind(), Target = time => Standing(new Vector3(16.5f, 0.001f, 0.5f))
        };
        // The Felguard and the crash-site Alien.
        yield return new EnemyScenario
        {
            Name = "felguard_flat", Type = 50, Seed = 50, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 3.7f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 30f,
            Boxes = Floor(), Target = time => Standing(new Vector3(11.5f, 0.001f, 2.5f))
        };
        yield return new EnemyScenario
        {
            Name = "alien_flat", Type = Rig.Modern ? 52 : 51, Seed = 51, Spawn = new Vector3(0.5f, 0.1f, 0.5f),
            Slow = 4.9f, Fast = 0f, Emerge = 1f, RunActivation = 0f, NormalActivation = 0f, Seconds = 30f,
            Boxes = Floor(), Target = time => Standing(new Vector3(11.5f, 0.001f, 2.5f))
        };
    }

    private static PlayerInputSample Move(float strafe, float forward, bool jump = false)
    {
        return new PlayerInputSample { Strafe = strafe, Forward = forward, Jump = jump };
    }

    // True on the first frame that starts at or after `at`, at any frame time.
    private static float frameSeconds;

    public static float FrameSeconds
    {
        set { frameSeconds = value; }
    }

    private static bool Press(float time, float at)
    {
        return time >= at && time - frameSeconds < at;
    }

    private static IEnumerable<PlayerScenario> PlayerScenarios()
    {
        var start = new Vector3(0.5f, 0.001f, 0.5f);
        const float east = (float)Math.PI / 2f; // forward = +X
        // Forward, strafe, the faster diagonal, backward, stop.
        Func<float, PlayerInputSample> walk = time =>
            time < 2f ? Move(0f, 1f) : time < 3f ? Move(1f, 0f) : time < 4f ? Move(1f, 1f) :
            time < 5f ? Move(0f, -1f) : time < 5.5f ? Move(-1f, -1f) : Move(0f, 0f);
        yield return new PlayerScenario { Name = "walk_flat", Start = start, Yaw = east, Boxes = Floor(), Input = walk };
        yield return new PlayerScenario { Name = "walk_flat_heading", Start = start, Yaw = 0.7f, Boxes = Floor(), Input = walk };
        // A standing jump, a running jump, and a second press in mid-air.
        Func<float, PlayerInputSample> jumps = time =>
            Move(0f, time >= 1.5f && time < 3.5f ? 1f : 0f, Press(time, 0.5f) || Press(time, 2f) || Press(time, 2.2f) || Press(time, 4.5f));
        foreach (long ticks in new long[] { 166667, 69444, 333333, 41667 })
            yield return new PlayerScenario
            {
                Name = "jump_flat" + (ticks == 166667 ? "" : "@" + ticks), Start = start, Yaw = east, Ticks = ticks,
                Boxes = Floor(), Input = jumps
            };
        // Into a wall at an angle (slide along it), then into a corner.
        Func<List<Box>> walls = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(5, 0, -8, 5, 2, 8, Rock));
            boxes.Add(new Box(-4, 0, 6, 5, 2, 6, Rock));
            return boxes;
        };
        yield return new PlayerScenario
        {
            Name = "wall_slide", Start = start, Yaw = east + 0.6f, Seconds = 8f, Boxes = walls(),
            Input = time => Move(0f, 1f, Press(time, 3f))
        };
        // One-block steps, a two-block wall, then a staircase: AutoClimb on and off.
        Func<List<Box>> steps = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(3, 0, -6, 5, 0, 6, Rock));
            boxes.Add(new Box(8, 0, -6, 8, 1, 6, Rock));
            return boxes;
        };
        Func<List<Box>> stairs = () =>
        {
            List<Box> boxes = Floor();
            for (int step = 0; step < 5; ++step) boxes.Add(new Box(3 + step, 0, -6, 3 + step, step, 6, Rock));
            boxes.Add(new Box(8, 0, -6, 14, 4, 6, Rock));
            // A step under a low ceiling cannot be climbed.
            boxes.Add(new Box(12, 5, -6, 12, 5, 6, Rock));
            boxes.Add(new Box(12, 7, -6, 14, 7, 6, Rock));
            return boxes;
        };
        foreach (bool climb in new[] { true, false })
        {
            yield return new PlayerScenario
            {
                Name = "steps_" + (climb ? "autoclimb" : "manual"), Start = start, Yaw = east, AutoClimb = climb,
                Seconds = 6f, Boxes = steps(), Input = time => Move(0f, 1f, Press(time, 3.5f) || Press(time, 4.2f))
            };
            yield return new PlayerScenario
            {
                Name = "stairs_" + (climb ? "autoclimb" : "manual"), Start = start, Yaw = east + 0.15f, AutoClimb = climb,
                Seconds = 6f, Boxes = stairs(), Input = time => Move(0f, 1f, Press(time, 4.6f))
            };
        }
        foreach (long ticks in new long[] { 69444, 333333 })
            yield return new PlayerScenario
            {
                Name = "stairs_autoclimb@" + ticks, Start = start, Yaw = east + 0.15f, Ticks = ticks,
                Seconds = 6f, Boxes = stairs(), Input = time => Move(0f, 1f, Press(time, 4.6f))
            };
        // Running jumps onto a two-block ledge and under a low ceiling.
        Func<List<Box>> ledge = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(4, 0, -6, 12, 1, 6, Rock));
            boxes.Add(new Box(7, 4, -6, 12, 4, 6, Rock));
            return boxes;
        };
        yield return new PlayerScenario
        {
            Name = "ledge_jump", Start = start, Yaw = east, Seconds = 5f, Boxes = ledge(),
            Input = time => Move(0f, 1f, Press(time, 0.25f) || Press(time, 1.6f) || Press(time, 2.4f))
        };
        // Walking off a platform: the fall damage scale.
        foreach (int height in new[] { 5, 6, 8, 12, 20, 30 })
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(-2, 0, -2, 2, height - 1, 2, Rock));
            foreach (long ticks in height == 12 ? new long[] { 166667, 69444, 333333 } : new long[] { 166667 })
                yield return new PlayerScenario
                {
                    Name = "fall_" + height + (ticks == 166667 ? "" : "@" + ticks), Ticks = ticks,
                    Start = new Vector3(0.5f, height + 0.001f, 0.5f), Yaw = east, Seconds = 5f, Boxes = boxes,
                    Input = time => Move(0f, time < 1.2f ? 1f : 0f)
                };
        }
        // 1.9.9.8 only (the sprint key does not exist in 1.6.3.8): sprint to
        // exhaustion with a double jump on the way.
        yield return new PlayerScenario
        {
            Name = "sprint_double_jump", Start = start, Yaw = east, Seconds = 7f, Boxes = Floor(),
            Input = time => new PlayerInputSample
            {
                Forward = time < 6f ? 1f : 0f, Sprint = time >= 0.5f && time < 4.5f,
                Jump = Press(time, 1f) || Press(time, 1.3f) || Press(time, 5f) || Press(time, 5.2f) || Press(time, 5.4f)
            }
        };
        // Blocking enemies: walking and jumping past them.
        yield return new PlayerScenario
        {
            Name = "sticky_enemies", Start = start, Yaw = east, Seconds = 8f, Boxes = Floor(),
            Blockers = new List<Vector3> { new Vector3(3.2f, 0.001f, 0.5f), new Vector3(7f, 0.001f, 1.4f), new Vector3(7.4f, 0.001f, -0.3f) },
            Input = time => Move(time > 2.5f && time < 3.5f ? 1f : 0f, 1f, Press(time, 1.2f) || Press(time, 5f))
        };
        // Fly mode. The FlyMode key is pressed in every one of these; whether
        // it does anything is the generation's and the game mode's business.
        // Looking 20 degrees up: forward, forward with jump held, hovering with
        // jump held, strafe, backward, then the key again and the fall.
        Func<float, PlayerInputSample> flight = time => new PlayerInputSample
        {
            Fly = Press(time, 0.5f) || Press(time, 4f),
            Forward = time >= 1f && time < 2.5f ? 1f : time >= 3.5f && time < 4f ? -1f : 0f,
            Strafe = time >= 3f && time < 3.5f ? 1f : 0f,
            Jump = (time >= 2f && time < 2.5f) || (time >= 2.75f && time < 3f)
        };
        var runway = new Vector3(-30.5f, 0.001f, 0.5f);
        yield return new PlayerScenario
        {
            Name = "fly_creative", Start = runway, Yaw = east, Pitch = 0.35f, Creative = true, Seconds = 8f,
            Boxes = Floor(), Input = flight
        };
        // The same keys outside Creative.
        yield return new PlayerScenario
        {
            Name = "fly_survival", Start = runway, Yaw = east, Pitch = 0.35f, Seconds = 8f, Boxes = Floor(), Input = flight
        };
        // Level flight with the sprint key held, then a half-pushed stick.
        yield return new PlayerScenario
        {
            Name = "fly_creative_sprint", Start = runway, Yaw = east, Creative = true, Seconds = 5f, Boxes = Floor(),
            Input = time => new PlayerInputSample
            {
                Fly = Press(time, 0.25f), Forward = time >= 0.5f && time < 2f ? 1f : time >= 3f && time < 4f ? 0.5f : 0f,
                Sprint = time >= 1f && time < 2f, Strafe = time >= 4f && time < 4.5f ? -0.5f : 0f
            }
        };
        // Flown nose-down into the ground and along it: collision without gravity.
        yield return new PlayerScenario
        {
            Name = "fly_creative_dive", Start = runway, Yaw = east, Pitch = -0.6f, Creative = true, Seconds = 5f,
            Boxes = Floor(),
            Input = time => new PlayerInputSample
            {
                Fly = Press(time, 0.25f), Jump = time >= 0.5f && time < 1f,
                Forward = time >= 1.5f && time < 4f ? 1f : 0f
            }
        };
    }

    private static IEnumerable<PickupScenario> PickupScenarios()
    {
        Func<List<Box>> yard = () =>
        {
            List<Box> boxes = Floor();
            boxes.Add(new Box(6, 0, -6, 6, 3, 6, Rock));  // a wall
            boxes.Add(new Box(-3, 0, -1, -3, 1, 1, Rock)); // a two-block pillar
            return boxes;
        };
        // A stack thrown by a player (CreatePickup dropped: 4 units a second).
        foreach (long ticks in new long[] { 166667, 69444, 333333 })
            yield return new PickupScenario
            {
                Name = "thrown" + (ticks == 166667 ? "" : "@" + ticks), Ticks = ticks, Boxes = yard(),
                Start = new Vector3(0.5f, 1.4f, 0.5f), Velocity = new Vector3(3.9f, 0.4f, 0.8f)
            };
        // Thrown straight at the wall.
        yield return new PickupScenario
        {
            Name = "thrown_at_wall", Boxes = yard(), Start = new Vector3(4.2f, 1.4f, 0.5f),
            Velocity = new Vector3(3.6f, 0.4f, -1.7f)
        };
        // CreateUpwardPickup (1.5 units a second) from a mined block on a pillar.
        yield return new PickupScenario
        {
            Name = "upward_from_pillar", Boxes = yard(), Start = new Vector3(-2.5f, 2.5f, 0.5f),
            Velocity = new Vector3(1.06f, 0.15f, -1.05f)
        };
        // Enemy loot: a metre above where the enemy stood.
        yield return new PickupScenario
        {
            Name = "loot_drop", Boxes = yard(), Start = new Vector3(2.5f, 1.0f, -3.5f),
            Velocity = new Vector3(-0.9f, 0.15f, 1.19f)
        };
    }

    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: <game directory> <output directory> [name filter]");
            return 2;
        }
        string gameDirectory = Path.GetFullPath(args[0]);
        string output = Path.GetFullPath(args[1]);
        // --pin=<header> rewrites tests/simulation_parity_pinned.hpp from this run.
        string pin = args.Skip(2).Where(argument => argument.StartsWith("--pin=")).Select(argument => argument.Substring(6)).FirstOrDefault();
        string filter = args.Skip(2).FirstOrDefault(argument => !argument.StartsWith("--"));
        Directory.CreateDirectory(output);
        // The original loads sounds and some content relative to its install.
        Directory.SetCurrentDirectory(gameDirectory);
        if (!Rig.Boot(gameDirectory)) return 77;
        int failures = 0;
        var notes = new List<string>();
        using (var writer = new StreamWriter(Path.Combine(output, "enemies.trace")))
        {
            try
            {
                Enemies.Setup();
                foreach (EnemyScenario scenario in EnemyScenarios())
                {
                    if (filter != null && scenario.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    try
                    {
                        var buffer = new StringWriter();
                        Enemies.Run(scenario, buffer);
                        writer.Write(buffer.ToString());
                        Pin.Add(buffer.ToString());
                        Console.WriteLine("RAN enemy " + scenario.Name);
                    }
                    catch (Exception error)
                    {
                        ++failures;
                        Console.WriteLine("FAIL enemy " + scenario.Name + ": " + error);
                    }
                }
            }
            catch (Exception error)
            {
                ++failures;
                Console.WriteLine("FAIL enemy setup: " + error);
            }
        }
        using (var writer = new StreamWriter(Path.Combine(output, "players.trace")))
        {
            try
            {
                Players.Setup();
                foreach (PlayerScenario scenario in PlayerScenarios())
                {
                    if (filter != null && scenario.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    try
                    {
                        var buffer = new StringWriter();
                        Players.Run(scenario, buffer);
                        writer.Write(buffer.ToString());
                        Pin.AddPlayer(buffer.ToString());
                        Console.WriteLine("RAN player " + scenario.Name);
                    }
                    catch (Exception error)
                    {
                        ++failures;
                        Console.WriteLine("FAIL player " + scenario.Name + ": " + error);
                    }
                }
            }
            catch (Exception error)
            {
                ++failures;
                Console.WriteLine("FAIL player setup: " + error);
            }
        }
        using (var writer = new StreamWriter(Path.Combine(output, "pickups.trace")))
        {
            foreach (PickupScenario scenario in PickupScenarios())
            {
                if (filter != null && scenario.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                try
                {
                    var buffer = new StringWriter();
                    Pickups.Run(scenario, buffer);
                    writer.Write(buffer.ToString());
                    Console.WriteLine("RAN pickup " + scenario.Name);
                }
                catch (Exception error)
                {
                    ++failures;
                    Console.WriteLine("FAIL pickup " + scenario.Name + ": " + error);
                }
            }
        }
        if (pin != null && failures == 0 && filter == null)
        {
            Pin.Write(Path.GetFullPath(pin), Rig.Game.GetName().Version.ToString());
            Console.WriteLine("PINNED " + pin);
        }
        File.WriteAllLines(Path.Combine(output, "redirected.txt"), Rig.Redirected.Distinct());
        Console.WriteLine("REDIRECTED " + string.Join(", ", Rig.Redirected.Distinct()));
        return failures == 0 ? 0 : 1;
    }
}
