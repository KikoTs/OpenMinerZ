using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

// Turns a trace into tests/animation_parity_pinned_<generation>.inc, so the
// native animator stays pinned to what the original did where the game is not
// installed. Per scenario it keeps what the original code was GIVEN each frame
// (run-length encoded) and one 64-bit hash over everything it then SHOWED:
// every channel's clip, clock, speed, flags and cross-fade, for the local and
// the remote player, on every frame. tests/test_animation_parity.cpp replays
// the inputs through world::PlayerAnimator and must arrive at the same hash.
internal static class Pin
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    // The 60 Hz scenarios kept in the repository; the live test covers the rest.
    private static bool Pinned(string name)
    {
        if (name.Contains("@")) return false;
        string[] exact =
        {
            "move_0", "move_23", "move_1", "move_13", "move_110", "move_50", "move_52", "move_82", "move_83", "move_103",
            "use_0_none", "use_0_zombie", "use_23_block", "use_49_zombie", "use_13_none", "use_114_block", "place_1",
            "reload_key_51", "reload_key_52", "reload_key_82", "reload_ads_50", "reload_ads_82", "reload_ads_84",
            "switch_50", "switch_82", "switch_52", "fire_103", "ads_103",
            "grenade_tap", "grenade_cook", "grenade_auto", "grenade_switch", "grenade_last",
            "die_0", "die_50", "die_firing_50", "die_cooking"
        };
        if (exact.Contains(name)) return true;
        return name.StartsWith("fire_") || name.StartsWith("ads_") ||
               (name.StartsWith("reload_") && !name.StartsWith("reload_key_") && !name.StartsWith("reload_ads_"));
    }

    private static string Field(string line, string key)
    {
        string needle = " " + key + "=";
        int at = line.IndexOf(needle, StringComparison.Ordinal);
        if (at < 0) return "";
        int start = at + needle.Length;
        int end = line.IndexOf(' ', start);
        return end < 0 ? line.Substring(start) : line.Substring(start, end - start);
    }

    private static string ChannelsOf(string line)
    {
        int start = line.IndexOf(" c0=", StringComparison.Ordinal);
        int end = line.IndexOf(" ev=", start, StringComparison.Ordinal);
        return end < 0 ? line.Substring(start) : line.Substring(start, end - start);
    }

    private static ulong Fnv(ulong hash, string text)
    {
        foreach (char letter in text)
        {
            hash ^= (byte)letter;
            hash *= 1099511628211UL;
        }
        return hash;
    }

    private static string RunLength(List<string> tokens)
    {
        var text = new StringBuilder();
        int index = 0;
        while (index < tokens.Count)
        {
            int count = 1;
            while (index + count < tokens.Count && tokens[index + count] == tokens[index]) ++count;
            text.Append(count.ToString(Inv)).Append('*').Append(tokens[index]).Append(';');
            index += count;
        }
        return text.ToString();
    }

    // MSVC caps one string literal piece near 16 KB; adjacent pieces concatenate.
    private static string Literal(string text)
    {
        var pieces = new List<string>();
        for (int start = 0; start < text.Length; start += 1800)
            pieces.Add("\"" + text.Substring(start, Math.Min(1800, text.Length - start)) + "\"");
        return pieces.Count == 0 ? "\"\"" : string.Join("\n    ", pieces);
    }

    public static void Write(string header, string trace, bool modern)
    {
        var text = new StringBuilder();
        var scenarios = new StringBuilder();
        var usedItems = new SortedSet<int>();
        var itemLines = new Dictionary<int, string>();
        string version = "";
        string name = null;
        long ticks = 0;
        int frames = 0, firstItem = 0, lastUpdate = -1, pinned = 0;
        List<string> local = null, remote = null;
        ulong hash = 0;
        string inLine = null, localLine = null;
        foreach (string line in File.ReadLines(trace))
        {
            if (line.StartsWith("VERSION ")) version = line.Split(' ')[1];
            else if (line.StartsWith("ITEM "))
            {
                string[] tokens = line.Split(' ');
                bool gun = Field(line, "reload") != "";
                itemLines[int.Parse(tokens[1], Inv)] =
                    "ANIMATION_PINNED_ITEM(" + tokens[1] + ", " + Field(line, "mode") + ", " + (gun ? "true" : "false") + ", " +
                    (!gun && Field(line, "class").Contains("Grenade") ? "true" : "false") + ", " +
                    (gun ? Field(line, "reload") : "0") + "LL, " + (gun ? Field(line, "capacity") : "0") + ")";
            }
            else if (line.StartsWith("SCENARIO "))
            {
                name = line.Split(' ')[1];
                if (!Pinned(name)) { name = null; continue; }
                ticks = long.Parse(Field(line, "ticks"), Inv);
                frames = int.Parse(Field(line, "frames"), Inv);
                firstItem = int.Parse(Field(line, "tray").Split(',')[0], Inv);
                foreach (string item in Field(line, "tray").Split(',')) usedItems.Add(int.Parse(item, Inv));
                usedItems.Add(0);
                local = new List<string>();
                remote = new List<string>();
                lastUpdate = -1;
                hash = 14695981039346656037UL;
            }
            else if (name == null) continue;
            else if (line.StartsWith("IN ")) inLine = line;
            else if (line.StartsWith("L ")) localLine = line;
            else if (line.StartsWith("R "))
            {
                int frame = int.Parse(localLine.Split(' ')[1], Inv);
                string[] item = Field(localLine, "item").Split('/');
                local.Add(Field(inLine, "mv") + "," + Field(localLine, "f") + "," + Field(localLine, "fps") + "," +
                          Field(localLine, "m") + "," + item[0] + "," + (item.Length > 1 ? item[1] : "-1") + "," +
                          (Field(localLine, "nt").Contains("grenade:") ? "1" : "0"));
                string received = Field(line, "rx");
                if (received != "-")
                {
                    // pitch, flags, mode, movement (the yaw byte animates nothing).
                    remote.Add((frame - lastUpdate).ToString(Inv) + ":" + received.Substring(0, 2) + received.Substring(4, 6));
                    lastUpdate = frame;
                }
                string events = Field(localLine, "ev");
                string shownLocal = "L" + ChannelsOf(localLine) + " st=" + Field(localLine, "st") + " uap=" + Field(localLine, "uap") +
                                    " e=" + (events.Contains("cue:FootStep") ? "1" : "0") + (events.Contains("cue:GrenadeArm") ? "1" : "0") +
                                    (events.Contains("Reload") ? "1" : "0");
                string remoteEvents = Field(line, "ev");
                string shownRemote = "R" + ChannelsOf(line) + " st=" + Field(line, "st") + " uap=" + Field(line, "uap") +
                                     " e=" + (remoteEvents.Contains("cue:GrenadeArm") ? "1" : "0") +
                                     (remoteEvents.Contains("Reload") ? "1" : "0");
                hash = Fnv(Fnv(hash, shownLocal), "\n");
                hash = Fnv(Fnv(hash, shownRemote), "\n");
            }
            else if (line == "END")
            {
                scenarios.Append("ANIMATION_PINNED_SCENARIO(\"" + name + "\", " + ticks.ToString(Inv) + "LL, " + frames.ToString(Inv) + ", " +
                                 firstItem.ToString(Inv) + ",\n    " + Literal(RunLength(local)) + ",\n    " + Literal(RunLength(remote)) +
                                 ",\n    0x" + hash.ToString("x16", Inv) + "ULL)\n");
                ++pinned;
                name = null;
            }
        }
        text.Append("// GENERATED by tools/OriginalClientInterop/AnimationParityProbe (--pin) from the\n");
        text.Append("// shipped Castle Miner Z " + version + " client. Do not edit by hand.\n//\n");
        text.Append("// ANIMATION_PINNED_ITEM(id, PlayerMode, is gun, is grenade, ReloadTime ticks, ClipCapacity)\n");
        text.Append("// ANIMATION_PINNED_SCENARIO(name, frame ticks, frames, item in hand,\n");
        text.Append("//     per frame \"strafe,forward,flags,fps,mode,item,rounds,grenade\" (run-length encoded;\n");
        text.Append("//         movement as float bits, flags = UsingTool Shouldering Reloading Dead\n");
        text.Append("//         PlayGrenadeAnim ReadyToThrowGrenade as the frame left them),\n");
        text.Append("//     PlayerUpdateMessage arrivals \"frames since the last:pitch flags mode movement\",\n");
        text.Append("//     FNV-1a of every channel of both players on every frame)\n");
        text.Append("ANIMATION_PINNED_VERSION(\"" + version + "\", " + (modern ? "true" : "false") + ")\n");
        foreach (int item in usedItems)
            if (itemLines.ContainsKey(item)) text.Append(itemLines[item] + "\n");
        text.Append(scenarios);
        File.WriteAllText(header, text.ToString().Replace("\r\n", "\n"));
        if (pinned < 40) throw new InvalidDataException("Only " + pinned + " scenarios were pinned");
    }
}
