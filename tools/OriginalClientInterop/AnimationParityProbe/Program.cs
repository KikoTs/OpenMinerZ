using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

// Differential probe for WHICH ANIMATION PLAYS WHEN.
//
// Usage: OpenMinerZ.AnimationParityProbe <game directory> <output directory> [name filter] [--pin=<header>]
//
// For every item family the original Player is driven through scripted input
// (stand, walk, run, strafe, backward, use, fire tap and hold, shoulder, reload,
// interrupted reload, item switch, grenade cook and throw, death and respawn),
// and for every frame the trace records each animation channel's clip, clip
// time in ticks, playback speed, looping/reversed flags and cross-fade, for the
// local first-person player and for the remote copy another peer animates from
// the PlayerUpdateMessage stream. tests/test_animation_parity.cpp replays the
// same inputs through the native animator and compares every frame.
// Exit code 77 means no graphics device was available (the original content
// reader needs one); the CTest treats that as a skip.
internal static class Program
{
    private static float frameSeconds;

    public static float FrameSeconds
    {
        set { frameSeconds = value; }
    }

    // True on the first frame that starts at or after `at`, at any frame time.
    private static bool Press(float time, float at)
    {
        return time >= at && time - frameSeconds < at;
    }

    private static bool Hold(float time, float from, float to)
    {
        return time >= from && time < to;
    }

    private static readonly int[] Guns = { 50, 51, 52, 53, 54, 119, 82, 83, 84, 85, 86 };
    private static readonly long[] OtherRates = { 69444, 333333, 41667 };

    private static string Suffix(long ticks)
    {
        return ticks == 166667 ? "" : "@" + ticks;
    }

    private static IEnumerable<Scenario> Scenarios()
    {
        // ---- standing, walking, running, strafing, backing up, per held item ----
        Func<bool, Func<float, InputSample>> move = gun => time => new InputSample
        {
            Forward = Hold(time, 0.5f, 1.5f) ? 0.5f : Hold(time, 1.5f, 2.5f) ? 1f : Hold(time, 3f, 3.5f) ? -1f :
                      Hold(time, 3.5f, 4f) ? 0.6f : Hold(time, 5.2f, 5.9f) ? 0.7f : 0f,
            Strafe = Hold(time, 2.5f, 3f) ? 1f : Hold(time, 3.5f, 4f) ? 0.6f : 0f,
            Shoulder = gun && Hold(time, 4.5f, 6.5f)
        };
        foreach (int item in new[] { 0, 23, 1, 13, 110, 39, 49, 14, 114 }.Concat(Guns).Concat(new[] { 103 }))
            yield return new Scenario
            {
                Name = "move_" + item, Tray = new[] { item }, Counts = new[] { item == 1 || item == 110 || item == 14 ? 20 : 1 },
                Seconds = Guns.Contains(item) || item == 103 ? 7.5f : 4.5f, Input = move(Guns.Contains(item) || item == 103)
            };
        foreach (long ticks in OtherRates)
            foreach (int item in new[] { 0, 50 })
                yield return new Scenario
                {
                    Name = "move_" + item + Suffix(ticks), Tray = new[] { item }, Ticks = ticks,
                    Seconds = item == 50 ? 7.5f : 4.5f, Input = move(item == 50)
                };

        // ---- use / dig / melee with the base input path ----
        Func<float, InputSample> use = time => new InputSample
        {
            Use = Hold(time, 0.5f, 2.5f) || Press(time, 3f), Shoulder = Hold(time, 3.5f, 4f)
        };
        foreach (int item in new[] { 0, 23, 49, 13, 39, 114, 29 })
            foreach (string target in new[] { "none", "block", "zombie" })
                yield return new Scenario
                {
                    Name = "use_" + item + "_" + target, Tray = new[] { item }, Target = target, Seconds = 5f, Input = use
                };
        foreach (long ticks in OtherRates)
            yield return new Scenario
            {
                Name = "use_23_block" + Suffix(ticks), Tray = new[] { 23 }, Target = "block", Ticks = ticks, Seconds = 5f, Input = use
            };
        // Blocks only ever build, and only on the press.
        yield return new Scenario
        {
            Name = "place_1", Tray = new[] { 1 }, Counts = new[] { 20 }, Target = "block", Seconds = 4f,
            Input = time => new InputSample { Use = Press(time, 0.5f) || Hold(time, 1f, 2f) || Press(time, 2.5f) || Press(time, 2.6f) }
        };
        yield return new Scenario
        {
            Name = "place_14", Tray = new[] { 14 }, Counts = new[] { 20 }, Target = "block", Seconds = 3f,
            Input = time => new InputSample { Use = Press(time, 0.5f) || Hold(time, 1f, 2f) }
        };

        // ---- guns ----
        foreach (int gun in Guns)
        {
            Func<float, InputSample> fire = time => new InputSample { Use = Press(time, 1.2f) || Press(time, 2.4f) || Hold(time, 3.6f, 6f) };
            yield return new Scenario { Name = "fire_" + gun, Tray = new[] { gun }, Seconds = 8f, Input = fire };
            yield return new Scenario
            {
                Name = "ads_" + gun, Tray = new[] { gun }, Seconds = 5f,
                Input = time => new InputSample
                {
                    Shoulder = Hold(time, 0.5f, 4f), Use = Press(time, 1.2f) || Hold(time, 2.4f, 3.2f),
                    Forward = Hold(time, 3.4f, 3.8f) ? 1f : 0f
                }
            };
            // The last two rounds: the empty clip asks for its own reload.
            yield return new Scenario
            {
                Name = "reload_" + gun, Tray = new[] { gun }, Rounds = new[] { 2 }, Seconds = 15f,
                Input = time => new InputSample { Use = Press(time, 1.2f) || Press(time, 2.6f) }
            };
            // The reload key with the trigger up, then a trigger pull through it.
            yield return new Scenario
            {
                Name = "reload_key_" + gun, Tray = new[] { gun }, Rounds = new[] { 3 }, Seconds = 10f,
                Input = time => new InputSample { Reload = Press(time, 1.2f) || Press(time, 5.5f), Use = Press(time, 1.8f) }
            };
            // Asked for while shouldered: the weapon comes down first.
            yield return new Scenario
            {
                Name = "reload_ads_" + gun, Tray = new[] { gun }, Rounds = new[] { 3 }, Seconds = 9f,
                Input = time => new InputSample { Shoulder = Hold(time, 0.3f, 7f), Reload = Press(time, 2f), Use = Press(time, 1.2f) }
            };
            // Slot keys during the fire clip (refused) and during a reload (taken).
            yield return new Scenario
            {
                Name = "switch_" + gun, Tray = new[] { gun, 23 }, Rounds = new[] { 3 }, Seconds = 7f,
                Input = time => new InputSample
                {
                    Use = Press(time, 1.2f), Reload = Press(time, 3.5f),
                    Slot = Hold(time, 1.22f, 1.3f) || Hold(time, 2f, 3f) || Hold(time, 4f, 5f) ? 1 : 0
                }
            };
        }
        foreach (long ticks in OtherRates)
        {
            foreach (int gun in new[] { 50, 51, 87, 52 })
            {
                yield return new Scenario
                {
                    Name = "fire_" + gun + Suffix(ticks), Tray = new[] { gun }, Ticks = ticks, Seconds = 8f,
                    Input = time => new InputSample { Use = Press(time, 1.2f) || Press(time, 2.4f) || Hold(time, 3.6f, 6f) }
                };
                yield return new Scenario
                {
                    Name = "reload_" + gun + Suffix(ticks), Tray = new[] { gun }, Rounds = new[] { 2 }, Ticks = ticks, Seconds = 15f,
                    Input = time => new InputSample { Use = Press(time, 1.2f) || Press(time, 2.6f) }
                };
            }
            yield return new Scenario
            {
                Name = "ads_50" + Suffix(ticks), Tray = new[] { 50 }, Ticks = ticks, Seconds = 5f,
                Input = time => new InputSample
                {
                    Shoulder = Hold(time, 0.5f, 4f), Use = Press(time, 1.2f) || Hold(time, 2.4f, 3.2f),
                    Forward = Hold(time, 3.4f, 3.8f) ? 1f : 0f
                }
            };
        }
        // The launcher is spent by its one shot and leaves the hand afterwards.
        yield return new Scenario
        {
            Name = "fire_103", Tray = new[] { 103 }, Seconds = 5f, Input = time => new InputSample { Use = Press(time, 1.2f) }
        };
        yield return new Scenario
        {
            Name = "ads_103", Tray = new[] { 103 }, Seconds = 5f,
            Input = time => new InputSample { Shoulder = Hold(time, 0.3f, 4f), Use = Press(time, 1.5f) }
        };

        // ---- grenades ----
        yield return new Scenario
        {
            Name = "grenade_tap", Tray = new[] { 110 }, Counts = new[] { 5 }, Seconds = 5f,
            Input = time => new InputSample { Use = Press(time, 0.5f) || Press(time, 3f) }
        };
        yield return new Scenario
        {
            Name = "grenade_cook", Tray = new[] { 110 }, Counts = new[] { 5 }, Seconds = 6f,
            Input = time => new InputSample { Use = Hold(time, 0.5f, 2.5f), Forward = Hold(time, 1f, 2f) ? 1f : 0f }
        };
        yield return new Scenario
        {
            Name = "grenade_auto", Tray = new[] { 110 }, Counts = new[] { 5 }, Seconds = 8f,
            Input = time => new InputSample { Use = Hold(time, 0.5f, 7f) }
        };
        yield return new Scenario
        {
            Name = "grenade_switch", Tray = new[] { 110, 23 }, Counts = new[] { 5 }, Seconds = 5f,
            Input = time => new InputSample { Use = Hold(time, 0.5f, 1.5f), Slot = time >= 1f ? 1 : 0 }
        };
        yield return new Scenario
        {
            Name = "grenade_last", Tray = new[] { 110 }, Counts = new[] { 1 }, Seconds = 4f,
            Input = time => new InputSample { Use = Hold(time, 0.5f, 1f) }
        };
        foreach (long ticks in OtherRates)
            yield return new Scenario
            {
                Name = "grenade_cook" + Suffix(ticks), Tray = new[] { 110 }, Counts = new[] { 5 }, Ticks = ticks, Seconds = 6f,
                Input = time => new InputSample { Use = Hold(time, 0.5f, 2.5f), Forward = Hold(time, 1f, 2f) ? 1f : 0f }
            };

        // ---- death and respawn ----
        Func<float, InputSample> die = time => new InputSample
        {
            Forward = Hold(time, 0.3f, 1.4f) || Hold(time, 6f, 7f) ? 1f : Hold(time, 2f, 2.6f) ? 0.5f : 0f,
            Strafe = Hold(time, 2.6f, 3.2f) ? 1f : 0f,
            Kill = Press(time, 1f), Jump = Press(time, 2f) || Press(time, 5f)
        };
        foreach (int item in new[] { 0, 23, 50 })
            yield return new Scenario { Name = "die_" + item, Tray = new[] { item }, Seconds = 8f, Input = die };
        yield return new Scenario
        {
            Name = "die_firing_50", Tray = new[] { 50 }, Seconds = 8f,
            Input = time => new InputSample
            {
                Use = Hold(time, 1.2f, 6f), Shoulder = Hold(time, 0.2f, 6f), Kill = Press(time, 1.6f), Jump = Press(time, 5f)
            }
        };
        yield return new Scenario
        {
            Name = "die_cooking", Tray = new[] { 110 }, Counts = new[] { 5 }, Seconds = 8f,
            Input = time => new InputSample { Use = Hold(time, 0.5f, 3f), Kill = Press(time, 1f), Jump = Press(time, 5f) }
        };
        foreach (long ticks in OtherRates)
            yield return new Scenario { Name = "die_0" + Suffix(ticks), Tray = new[] { 0 }, Ticks = ticks, Seconds = 8f, Input = die };
    }

    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: <game directory> <output directory> [name filter] [--pin=<header>]");
            return 2;
        }
        string gameDirectory = Path.GetFullPath(args[0]);
        string output = Path.GetFullPath(args[1]);
        string pin = args.Skip(2).Where(argument => argument.StartsWith("--pin=")).Select(argument => argument.Substring(6)).FirstOrDefault();
        string filter = args.Skip(2).FirstOrDefault(argument => !argument.StartsWith("--"));
        Directory.CreateDirectory(output);
        Directory.SetCurrentDirectory(gameDirectory);
        if (!Rig.Boot(gameDirectory)) return 77;
        int failures = 0;
        try
        {
            Stage.Setup();
        }
        catch (Exception error)
        {
            Console.WriteLine("FAIL setup: " + error);
            return 1;
        }
        string version = Rig.Game.GetName().Version.ToString();
        using (var writer = new StreamWriter(Path.Combine(output, "animation.trace")))
        {
            writer.WriteLine("VERSION " + version + " modern=" + (Rig.Modern ? 1 : 0));
            foreach (string clip in Stage.Clips.Values) writer.WriteLine(clip);
            foreach (string item in Stage.ItemTable()) writer.WriteLine(item);
            foreach (Scenario scenario in Scenarios())
            {
                if (filter != null && scenario.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                // 1.9.9.8 moved a few ids; skip a scenario whose item it does not register.
                try
                {
                    var buffer = new StringWriter();
                    Stage.Run(scenario, buffer);
                    writer.Write(buffer.ToString());
                    Console.WriteLine("RAN " + scenario.Name);
                }
                catch (Exception error)
                {
                    ++failures;
                    Console.WriteLine("FAIL " + scenario.Name + ": " + error);
                }
            }
        }
        File.WriteAllLines(Path.Combine(output, "redirected.txt"), Rig.Redirected.Distinct());
        Console.WriteLine("REDIRECTED " + string.Join(", ", Rig.Redirected.Distinct()));
        if (pin != null && failures == 0 && filter == null)
        {
            Pin.Write(Path.GetFullPath(pin), Path.Combine(output, "animation.trace"), Rig.Modern);
            Console.WriteLine("PINNED " + pin);
        }
        return failures == 0 ? 0 : 1;
    }
}
