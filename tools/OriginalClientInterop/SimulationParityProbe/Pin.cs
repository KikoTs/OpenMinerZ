using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

// Turns scenario traces into the C++ tables of tests/simulation_parity_pinned.hpp,
// so the native simulation stays pinned to what the original did even where the
// game is not installed. Only scenarios whose scripted target is piecewise
// constant are pinned (their whole input fits in a few lines); the rest are
// covered by the live differential test.
//
// What is pinned per scenario: every state or clip change with the frame it
// happened on and where the enemy stood, every hit on the player, the removal
// frame, and how many speed-up and give-up messages and arrows it produced.
internal static class Pin
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    private static readonly StringBuilder tables = new StringBuilder();
    private static readonly List<string> rows = new List<string>();

    private static Dictionary<string, string> Fields(string[] tokens, int first)
    {
        var fields = new Dictionary<string, string>();
        for (int index = first; index < tokens.Length; ++index)
        {
            int equals = tokens[index].IndexOf('=');
            if (equals > 0) fields[tokens[index].Substring(0, equals)] = tokens[index].Substring(equals + 1);
        }
        return fields;
    }

    private static string Float(string text)
    {
        // Round-trip float text -> a C++ float literal.
        float value = float.Parse(text, Inv);
        string literal = value.ToString("R", Inv);
        if (literal.IndexOfAny(new[] { '.', 'E', 'e' }) < 0) literal += ".0";
        return literal + "F";
    }

    private static string Vector(string text)
    {
        return string.Join(", ", text.Split(',').Select(Float));
    }

    private static string Identifier(string name)
    {
        return new string(name.Select(letter => char.IsLetterOrDigit(letter) ? letter : '_').ToArray());
    }

    public static void Add(string trace)
    {
        string name = null;
        Dictionary<string, string> enemy = null;
        var boxes = new List<string>();
        var targets = new List<string>();
        var actions = new List<string>();
        var steps = new List<string>();
        var damages = new List<string>();
        string lastTarget = null, lastStep = null;
        int frames = 0, removed = -1, speedUps = 0, giveUps = 0, arrows = 0;
        foreach (string line in trace.Split('\n').Select(text => text.TrimEnd('\r')))
        {
            string[] tokens = line.Split(' ');
            if (tokens[0] == "SCENARIO") name = tokens[2];
            else if (tokens[0] == "ENEMY") enemy = Fields(tokens, 1);
            else if (tokens[0] == "BOX")
                boxes.Add("{" + string.Join(", ", tokens.Skip(1)) + "}");
            else if (tokens[0] == "FRAME")
            {
                Dictionary<string, string> fields = Fields(tokens, 2);
                string target = Vector(fields["t"]) + ", " + Vector(fields["tv"]) + ", " +
                                (fields["tc"] == "1" ? "true" : "false") + ", " + (fields["td"] == "1" ? "true" : "false");
                if (target != lastTarget) targets.Add("{" + tokens[1] + ", " + target + "}");
                lastTarget = target;
                string action = fields["act"];
                if (action == "kill") actions.Add("{" + tokens[1] + ", true, 0.0F}");
                else if (action.StartsWith("hit:")) actions.Add("{" + tokens[1] + ", false, " + Float(action.Substring(4)) + "}");
                frames = int.Parse(tokens[1], Inv) + 1;
            }
            else if (tokens[0] == "OUT")
            {
                Dictionary<string, string> fields = Fields(tokens, 2);
                foreach (string happened in fields["ev"].Split(';'))
                {
                    if (happened.StartsWith("dmg:")) damages.Add("{" + tokens[1] + ", " + Float(happened.Substring(4)) + "}");
                    else if (happened == "msg:SpeedUpEnemyMessage") ++speedUps;
                    else if (happened == "msg:EnemyGiveUpMessage") ++giveUps;
                    else if (happened.StartsWith("arrow:")) ++arrows;
                }
                if (tokens[2] == "removed")
                {
                    removed = int.Parse(tokens[1], Inv);
                    continue;
                }
                string step = fields["st"] + "/" + fields["clip"];
                if (step != lastStep)
                    steps.Add("{" + tokens[1] + ", \"" + fields["st"] + "\", \"" + fields["clip"] + "\", " + Vector(fields["p"]) + "}");
                lastStep = step;
            }
        }
        if (name == null || enemy == null || enemy["ticks"] != "166667" || targets.Count > 4) return;
        string id = Identifier(name);
        Action<string, string, List<string>> table = (type, suffix, entries) =>
        {
            if (entries.Count == 0) return;
            tables.Append("inline constexpr " + type + " " + id + "_" + suffix + "[] = {\n");
            foreach (string entry in entries) tables.Append("    " + entry + ",\n");
            tables.Append("};\n");
        };
        table("Box", "boxes", boxes);
        table("Target", "targets", targets);
        table("Action", "actions", actions);
        table("Step", "steps", steps);
        table("Damage", "damages", damages);
        Func<string, List<string>, string> span = (suffix, entries) => entries.Count == 0 ? "{}" : id + "_" + suffix;
        rows.Add("    {\"" + name + "\", " + enemy["type"] + ", " + enemy["id"] + ", " + enemy["seed"] + ", {" +
                 Vector(enemy["pos"]) + "}, {" + enemy["init"].Replace(",", ", ") + "}, " +
                 (enemy["local"] == "1" ? "true" : "false") + ", " + (enemy["fest"] == "1" ? "true" : "false") + ", " +
                 frames + ", " + removed + ", " + speedUps + ", " + giveUps + ", " + arrows + ",\n     " +
                 span("boxes", boxes) + ", " + span("targets", targets) + ", " + span("actions", actions) + ", " +
                 span("steps", steps) + ", " + span("damages", damages) + "},");
    }

    private static readonly List<string> playerRows = new List<string>();

    // A player scenario: the input from each frame it changes on, the frames
    // jump is pressed on, where the player stood every 20 frames and at the
    // end, whether it was in contact with the ground there, and every hit of
    // fall damage.
    public static void AddPlayer(string trace)
    {
        string name = null;
        Dictionary<string, string> player = null;
        var boxes = new List<string>();
        var blockers = new List<string>();
        var inputs = new List<string>();
        var jumps = new List<string>();
        var checks = new List<string>();
        var damages = new List<string>();
        string lastInput = null;
        int frames = 0;
        foreach (string line in trace.Split('\n').Select(text => text.TrimEnd('\r')))
        {
            string[] tokens = line.Split(' ');
            if (tokens[0] == "SCENARIO") name = tokens[2];
            else if (tokens[0] == "PLAYER") player = Fields(tokens, 1);
            else if (tokens[0] == "BOX") boxes.Add("{" + string.Join(", ", tokens.Skip(1)) + "}");
            else if (tokens[0] == "BLOCKER") blockers.Add("{" + Vector(tokens[1]) + "}");
            else if (tokens[0] == "FRAME")
            {
                Dictionary<string, string> fields = Fields(tokens, 2);
                string input = Float(fields["strafe"]) + ", " + Float(fields["forward"]);
                if (input != lastInput) inputs.Add("{" + tokens[1] + ", " + input + "}");
                lastInput = input;
                if (fields["jump"] == "1") jumps.Add(tokens[1]);
                frames = int.Parse(tokens[1], Inv) + 1;
            }
            else if (tokens[0] == "OUT")
            {
                Dictionary<string, string> fields = Fields(tokens, 2);
                foreach (string happened in fields["ev"].Split(';'))
                    if (happened.StartsWith("dmg:")) damages.Add("{" + tokens[1] + ", " + Float(happened.Substring(4)) + "}");
                checks.Add("{" + tokens[1] + ", " + Vector(fields["p"]) + ", " + (fields["c"] == "1" ? "true" : "false") + "}");
            }
        }
        if (name == null || player == null || player["ticks"] != "166667" || name.StartsWith("sprint")) return;
        checks = checks.Where((entry, index) => index % 20 == 19 || index == checks.Count - 1).ToList();
        string id = "player_" + Identifier(name);
        Action<string, string, List<string>> table = (type, suffix, entries) =>
        {
            if (entries.Count == 0) return;
            tables.Append("inline constexpr " + type + " " + id + "_" + suffix + "[] = {\n");
            foreach (string entry in entries) tables.Append("    " + entry + ",\n");
            tables.Append("};\n");
        };
        table("Box", "boxes", boxes);
        table("Point", "blockers", blockers);
        table("PlayerInput", "inputs", inputs);
        table("int", "jumps", jumps);
        table("PlayerCheck", "checks", checks);
        table("Damage", "damages", damages);
        Func<string, List<string>, string> span = (suffix, entries) => entries.Count == 0 ? "{}" : id + "_" + suffix;
        playerRows.Add("    {\"" + name + "\", {" + Vector(player["pos"]) + "}, " + Float(player["yaw"]) + ", " +
                       (player["autoclimb"] == "1" ? "true" : "false") + ", " + frames + ",\n     " +
                       span("boxes", boxes) + ", " + span("blockers", blockers) + ", " + span("inputs", inputs) + ", " +
                       span("jumps", jumps) + ", " + span("checks", checks) + ", " + span("damages", damages) + "},");
    }

    public static void Write(string path, string version)
    {
        var text = new StringBuilder();
        text.Append("// GENERATED by tools/OriginalClientInterop/SimulationParityProbe (--pin) from the\n");
        text.Append("// shipped Castle Miner Z " + version + " client. Do not edit by hand: every number\n");
        text.Append("// below is what the original BaseZombie did with the scripted inputs beside it.\n");
        text.Append("#pragma once\n\n#include <span>\n\nnamespace simulation_parity_pinned\n{\n\n");
        text.Append("struct Box final\n{\n    int x0, y0, z0, x1, y1, z1, block;\n};\n");
        text.Append("// The target player's state from this frame on.\n");
        text.Append("struct Target final\n{\n    int frame;\n    float x, y, z, vx, vy, vz;\n    bool contact, dead;\n};\n");
        text.Append("// BaseZombie.TakeExplosiveDamage applied before this frame.\n");
        text.Append("struct Action final\n{\n    int frame;\n    bool kill;\n    float damage;\n};\n");
        text.Append("// The state and clip from the end of this frame on, and where the enemy stood.\n");
        text.Append("struct Step final\n{\n    int frame;\n    const char *state;\n    const char *clip;\n    float x, y, z;\n};\n");
        text.Append("struct Damage final\n{\n    int frame;\n    float amount;\n};\n");
        text.Append("struct Enemy final\n{\n    const char *name;\n    int type;\n    int id;\n    int seed;\n    float spawn[3];\n");
        text.Append("    int init[5];\n    bool local;\n    bool fest;\n    int frames;\n    int removed_frame;\n    int speed_ups;\n");
        text.Append("    int give_ups;\n    int arrows;\n    std::span<const Box> boxes;\n    std::span<const Target> targets;\n");
        text.Append("    std::span<const Action> actions;\n    std::span<const Step> steps;\n    std::span<const Damage> damages;\n};\n");
        text.Append("struct Point final\n{\n    float x, y, z;\n};\n");
        text.Append("// The movement stick from this frame on (strafe right, forward).\n");
        text.Append("struct PlayerInput final\n{\n    int frame;\n    float strafe, forward;\n};\n");
        text.Append("// Where the original Player stood at the end of this frame.\n");
        text.Append("struct PlayerCheck final\n{\n    int frame;\n    float x, y, z;\n    bool contact;\n};\n");
        text.Append("// Player.ProcessInput + Player.ResolveCollsion over scripted input. `jumps`\n");
        text.Append("// are the frames jump is pressed on; `damages` is fall damage as a\n");
        text.Append("// fraction of full health.\n");
        text.Append("struct Player final\n{\n    const char *name;\n    float start[3];\n    float yaw;\n    bool auto_climb;\n    int frames;\n");
        text.Append("    std::span<const Box> boxes;\n    std::span<const Point> blockers;\n    std::span<const PlayerInput> inputs;\n");
        text.Append("    std::span<const int> jumps;\n    std::span<const PlayerCheck> checks;\n    std::span<const Damage> damages;\n};\n\n");
        text.Append(tables);
        text.Append("\ninline constexpr Enemy enemies[] = {\n");
        foreach (string row in rows) text.Append(row + "\n");
        text.Append("};\n\ninline constexpr Player players[] = {\n");
        foreach (string row in playerRows) text.Append(row + "\n");
        text.Append("};\n\n} // namespace simulation_parity_pinned\n");
        File.WriteAllText(path, text.ToString().Replace("\r\n", "\n"));
    }
}
