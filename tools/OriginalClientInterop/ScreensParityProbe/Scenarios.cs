using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Input;

// Scripted inventories, the frames recorded from them and the input scripts.
//
// Slots are addressed the same way in both generations and on the native side
// (tests/test_screens_parity.cpp): 0..31 the backpack in the game's own array
// order, 32..39 the first tray, 40..47 the second tray (1.9.9.8 only),
// 100..131 the open crate.
internal static partial class Program
{
    private static object inventoryScreen, craftingScreen, crateScreen, crate, hudStandIn;
    private static Type itemType, idsType;

    private static object MakeItem(int id, int count, float health = 1f)
    {
        object item = R.CallStatic(itemType, "CreateItem", Enum.ToObject(idsType, id), count);
        if (health != 1f) R.Set(item, "ItemHealthLevel", health);
        return item;
    }

    private static object ParseItem(string text)
    {
        if (text == "-") return null;
        float health = 1f;
        int at = text.IndexOf('@');
        if (at >= 0)
        {
            health = float.Parse(text.Substring(at + 1), Invariant);
            text = text.Substring(0, at);
        }
        string[] parts = text.Split('x');
        return MakeItem(int.Parse(parts[0], Invariant), int.Parse(parts[1], Invariant), health);
    }

    private static int BackpackSize { get { return 32; } }
    private static int CarriedSlots { get { return Modern ? 48 : 40; } }

    private static object GetSlot(int index)
    {
        if (index >= 100) return ((Array)R.Get(crate, "Inventory")).GetValue(index - 100);
        if (index < 32) return ((Array)R.Get(Inventory, "Inventory")).GetValue(index);
        if (!Modern) return ((Array)R.Get(Inventory, "InventoryTray")).GetValue(index - 32);
        return R.Call(R.Get(Inventory, "TrayManager"), "GetTrayItem", (index - 32) / 8, (index - 32) % 8);
    }

    private static void SetSlot(int index, object item)
    {
        if (index >= 100) ((Array)R.Get(crate, "Inventory")).SetValue(item, index - 100);
        else if (index < 32) ((Array)R.Get(Inventory, "Inventory")).SetValue(item, index);
        else if (!Modern) ((Array)R.Get(Inventory, "InventoryTray")).SetValue(item, index - 32);
        else R.Call(R.Get(Inventory, "TrayManager"), "SetTrayItem", (index - 32) / 8, (index - 32) % 8, item);
    }

    private static void NewSession()
    {
        itemType = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        idsType = T("DNA.CastleMinerZ.Inventory.InventoryItemIDs");
        Type screenGroup = T("DNA.Drawing.UI.ScreenGroup");
        Type screen = T("DNA.Drawing.UI.Screen");
        Inventory = R.New(T("DNA.CastleMinerZ.Inventory.PlayerInventory"), Player, false);
        R.Set(Player, "PlayerInventory", Inventory);
        R.Set(Player, "Dead", false);
        UiGroup = R.New(screenGroup, true);
        R.Set(GameScreen, "_uiGroup", UiGroup);
        ScreenManager = R.New(screenGroup, false);
        // Stands in for the world the real game draws behind these screens.
        R.Set(ScreenManager, "BackgroundColor", (Color?)new Color(58, 92, 122));
        R.Set(GameObject, "_screenManager", ScreenManager);
        R.Call(ScreenManager, "PushScreen", UiGroup);
        hudStandIn = R.New(screen, false, true);
        R.Call(UiGroup, "PushScreen", hudStandIn);
        if (Modern)
        {
            inventoryScreen = craftingScreen = R.New(T("DNA.CastleMinerZ.UI.CraftingScreen"), GameObject, Hud);
            R.Set(GameScreen, "_blockCraftingScreen", inventoryScreen);
        }
        else
        {
            inventoryScreen = R.New(T("DNA.CastleMinerZ.UI.BlockPickerScreen"), GameObject, Hud);
            craftingScreen = R.New(T("DNA.CastleMinerZ.UI.CraftingUIScreen"), GameObject, Hud);
            R.Set(GameScreen, "_pickerScreen", inventoryScreen);
            R.Set(GameScreen, "_craftingScreen", craftingScreen);
        }
        crateScreen = R.New(T("DNA.CastleMinerZ.UI.CrateScreen"), GameObject, Hud);
        crate = R.New(T("DNA.CastleMinerZ.Inventory.Crate"), R.New(T("DNA.IntVector3"), 7, 12, -4));
        R.Set(crateScreen, "CurrentCrate", crate);
        SetGamers();
        R.Set(GameObject, "InfiniteResourceMode", false);
        R.Set(GameObject, "GameMode", Enum.Parse(R.Get(GameObject, "GameMode").GetType(), "Survival"));
        ResetInput();
        Recorder.Events.Clear();
    }

    private static void Give(string slots)
    {
        foreach (string entry in slots.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
        {
            string[] parts = entry.Split('=');
            SetSlot(int.Parse(parts[0], Invariant), ParseItem(parts[1]));
        }
        R.Call(Inventory, "DiscoverRecipies");
    }

    private static void Open(string which)
    {
        if (which == "inventory") R.Call(GameScreen, "ShowBlockPicker");
        else if (which == "crafting")
        {
            if (Modern) R.Call(GameScreen, "ShowBlockPicker");
            else R.Call(GameScreen, "ShowCraftingScreen");
        }
        else if (which == "crate") R.Call(UiGroup, "PushScreen", crateScreen);
        else throw new ArgumentException(which);
        // The first frame computes the rectangles the screens hit-test against.
        Frame(input: false);
    }

    private static string TopScreen()
    {
        object top = R.Get(UiGroup, "CurrentScreen");
        if (top == null || ReferenceEquals(top, hudStandIn)) return "hud";
        if (ReferenceEquals(top, crateScreen)) return "crate";
        if (ReferenceEquals(top, inventoryScreen)) return Modern ? "inventory" : "inventory";
        if (ReferenceEquals(top, craftingScreen)) return "crafting";
        return top.GetType().Name;
    }

    // Rectangles the shipped screen itself hit-tests, in buffer coordinates.
    private static Rectangle SlotRectangle(int index)
    {
        object top = R.Get(UiGroup, "CurrentScreen");
        if (index >= 100) return ((Rectangle[])R.Get(top, "_crateItemLocations"))[index - 100];
        // CraftingUIScreen draws the same grid but keeps no rectangles for it.
        if (!Modern && ReferenceEquals(top, craftingScreen))
            return index < 32 ? new Rectangle(404 + 59 * (index % 8), 334 + 59 * (index / 8), 59, 59)
                              : new Rectangle(404 + 59 * (index - 32), 584, 59, 59);
        string field = ReferenceEquals(top, crateScreen) || Modern ? "_inventoryItemLocations" : "_itemLocations";
        return ((Rectangle[])R.Get(top, field))[index];
    }

    private static Point ToWindow(Point buffer)
    {
        if (Modern) return buffer;
        var destination = (Rectangle)R.Get(GameObject, "_bufferDestRect");
        if (destination.Width == 0) return buffer;
        // A window pixel inside the block the buffer pixel covers (its centre).
        return new Point((int)Math.Floor((buffer.X + 0.5) * destination.Width / 1280.0) + destination.X,
                         (int)Math.Floor((buffer.Y + 0.5) * destination.Height / 720.0) + destination.Y);
    }

    private static Rectangle Background()
    {
        object top = R.Get(UiGroup, "CurrentScreen");
        foreach (string name in new[] { "_backgroundRect", "_backgroundRectangle" })
            if (R.Has(top.GetType(), name)) return (Rectangle)R.Get(top, name);
        var area = new Rectangle(0, 0, 1280, 720);
        object sprite = R.Get(top, "_background");
        int width = (int)R.Get(sprite, "Width"), height = (int)R.Get(sprite, "Height");
        return new Rectangle(area.Center.X - width / 2, area.Center.Y - height / 2, width, height);
    }

    private static void Point(string target)
    {
        Point buffer;
        int slot;
        if (int.TryParse(target, out slot))
        {
            Rectangle rectangle = SlotRectangle(slot);
            buffer = rectangle.Center;
        }
        else if (target == "out") buffer = new Point(Background().Left - 40, Background().Top + 60);
        else if (target == "panel") buffer = new Point(Background().Left + 12, Background().Bottom - 12);
        else if (target == "close")
        {
            var rectangle = (Rectangle)R.Get(R.Get(UiGroup, "CurrentScreen"), "_closeRectangle");
            buffer = rectangle.Center;
        }
        else if (target == "button")
        {
            object button = R.Get(R.Get(UiGroup, "CurrentScreen"), "_craftButton");
            buffer = ((Rectangle)R.Get(button, "ScreenBounds")).Center;
        }
        else if (target.StartsWith("recipe", StringComparison.Ordinal) || target.StartsWith("ingredient", StringComparison.Ordinal))
        {
            object top = R.Get(UiGroup, "CurrentScreen");
            bool recipe = target.StartsWith("recipe", StringComparison.Ordinal);
            int index = int.Parse(target.Substring(recipe ? 6 : 10), Invariant);
            if (Modern)
            {
                object tier2 = R.Get(R.Get(top, "SelectedTier1Item"), "SelectedTier2Item");
                if (recipe) buffer = ((Rectangle[])R.Get(tier2, "_scaledItemLocations"))[index].Center;
                else
                {
                    var back = (Rectangle)R.Get(tier2, "_backgroundRectangle");
                    float scale = Height / 720f;
                    buffer = new Point((int)(back.X + (39 + 65 * index + 30) * scale), (int)(back.Y + (521 + 30) * scale));
                }
            }
            else
            {
                // recipe<n>: the n-th visible cell of the strip, 2 = the selected one.
                Rectangle rectangle = recipe ? ((Rectangle[])R.Get(top, "_itemLocations"))[index]
                                             : ((Rectangle[])R.Get(top, "_recipeIngredientsItemLocations"))[index];
                buffer = rectangle.Center;
            }
        }
        else if (target.StartsWith("tier", StringComparison.Ordinal))
        {
            // tier1:<n> / tier2:<n> of the open group (1.9.9.8).
            object top = R.Get(UiGroup, "CurrentScreen");
            int index = int.Parse(target.Substring(6), Invariant);
            object entry = target[4] == '1'
                ? ((IList)R.Get(top, "_tier1Items"))[index]
                : ((IList)R.Get(R.Get(top, "SelectedTier1Item"), "_items"))[index];
            buffer = ((Rectangle)R.Get(entry, "_scaledLocation")).Center;
        }
        else
        {
            string[] parts = target.Split(',');
            buffer = new Point(int.Parse(parts[0], Invariant), int.Parse(parts[1], Invariant));
        }
        Point window = ToWindow(buffer);
        MoveMouse(window.X, window.Y);
    }

    private static void InputFrame()
    {
        CommitInput();
        Frame();
    }

    // One scripted action. Buttons and keys are pressed for one frame and
    // released on the next, which is what the screens' *Pressed tests see from
    // a real click.
    private static void Act(string action)
    {
        string[] words = action.Split(' ');
        switch (words[0])
        {
            case "move":
                Point(words[1]);
                InputFrame();
                break;
            case "click":
            {
                bool shift = words.Length > 2 && words[2] == "shift";
                if (shift) Key(Keys.LeftShift, true);
                Buttons(words[1] == "left", words[1] == "right");
                InputFrame();
                Buttons(false, false);
                InputFrame();
                if (shift) { Key(Keys.LeftShift, false); InputFrame(); }
                break;
            }
            case "wheel":
                Wheel(words[1] == "up" ? 120 : -120);
                InputFrame();
                InputFrame();
                break;
            case "key":
                Key((Keys)Enum.Parse(typeof(Keys), words[1]), true);
                InputFrame();
                Key((Keys)Enum.Parse(typeof(Keys), words[1]), false);
                InputFrame();
                break;
            case "wait":
            {
                int frames = (int)Math.Round(double.Parse(words[1], Invariant) * 60.0);
                for (int frame = 0; frame < frames; ++frame) InputFrame();
                break;
            }
            case "hold":
            {
                // A key kept down for a while: the first frame is its press,
                // the rest the screens' own auto-repeat.
                var key = (Keys)Enum.Parse(typeof(Keys), words[1]);
                int frames = (int)Math.Round(double.Parse(words[2], Invariant) * 60.0);
                Key(key, true);
                for (int frame = 0; frame < frames; ++frame) InputFrame();
                Key(key, false);
                InputFrame();
                break;
            }
            case "mode":
                R.Set(GameObject, "GameMode", Enum.Parse(R.Get(GameObject, "GameMode").GetType(), words[1]));
                R.Set(GameObject, "InfiniteResourceMode", words.Length > 2 && words[2] == "infinite");
                break;
            case "lock":
                LockCrateSlot(int.Parse(words[1], Invariant));
                break;
            default:
                throw new ArgumentException(action);
        }
    }

    // A second player whose crate cursor rests on `slot` of the open crate:
    // what CrateScreen.IsSlotLocked reads from the session.
    private static void LockCrateSlot(int slot)
    {
        Type gamerType = T("DNA.Net.GamerServices.NetworkGamer");
        object remote = R.Blank(gamerType);
        R.Set(remote, "_globalId", (byte)2);
        object other = R.Blank(T("DNA.CastleMinerZ.Player"));
        InitializeTreeNode(other);
        R.Set(other, "FocusCrate", R.Get(crate, "Location"));
        int columns = Modern ? 4 : 8;
        R.Set(other, "FocusCrateItem", new Point(slot % columns, slot / columns));
        R.Set(remote, "Tag", other);
        SetGamers(remote);
    }

    // The session a running game always has (a local one when playing alone).
    // Its two gamer lists are answered here: the local player, plus `remotes`.
    private static void SetGamers(params object[] remotes)
    {
        Type gamerType = T("DNA.Net.GamerServices.NetworkGamer");
        Type collection = T("DNA.Net.GamerServices.GamerCollection`1").MakeGenericType(gamerType);
        object local = R.Get(Player, "Gamer");
        R.Set(local, "Tag", Player);
        Func<IEnumerable<object>, object> make = gamers =>
        {
            IList list = (IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(gamerType));
            foreach (object gamer in gamers) list.Add(gamer);
            return Activator.CreateInstance(collection, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                            null, new object[] { list }, null);
        };
        RemoteGamers = make(remotes);
        AllGamers = make(new[] { local }.Concat(remotes));
        R.Set(GameObject, "_networkSession", R.Blank(T("DNA.Net.GamerServices.NetworkSession")));
    }

    private static string State()
    {
        var text = new StringBuilder("state screen=" + TopScreen());
        object top = R.Get(UiGroup, "CurrentScreen");
        string screen = TopScreen();
        if (screen == "inventory" || screen == "crate")
        {
            var selected = (Point)R.Get(top, "_selectedLocation");
            int columns = Modern ? 4 : 8;
            int rows = Modern ? 8 : 4;
            int index = selected.Y < rows ? selected.X + selected.Y * columns : 32 + (Modern ? (selected.Y - 8) / 2 * 8 : 0) + selected.X;
            if (screen == "crate" && (bool)R.Get(top, "_selectorInCrateGrid")) index += 100;
            bool visible = (bool)R.Get(top, "_hitTestTrue") ||
                           (R.Has(top.GetType(), "_mousePointerActive") && !(bool)R.Get(top, "_mousePointerActive"));
            text.Append(" sel=" + (visible ? index.ToString(Invariant) : "none"));
        }
        if (screen == "crafting" && !Modern)
        {
            object recipe = R.Get(top, "_selectedRecipe");
            text.Append(" recipe=" + (recipe == null ? "-" : ItemText(R.Get(recipe, "Result"))));
            text.Append(" ingredient=" + R.Get(top, "_selectedIngredientIndex"));
            text.Append(" cursor=" + ((int)R.Get(top, "_drawSelectorIndex") - (int)R.Get(top, "SelectedRecipeIndex")));
            text.Append(" enabled=" + ((bool)R.Get(R.Get(top, "_craftButton"), "Enabled") ? 1 : 0));
        }
        if (Modern && screen == "inventory")
        {
            object tier1 = R.Get(top, "SelectedTier1Item");
            object tier2 = R.Get(tier1, "SelectedTier2Item");
            text.Append(" group=" + ((IList)R.Get(top, "_tier1Items")).IndexOf(tier1));
            text.Append(" type=" + ((IList)R.Get(tier1, "_items")).IndexOf(tier2));
            text.Append(" recipe=" + R.Get(tier2, "_selectedIndex"));
        }
        if (screen != "hud" && R.Has(top.GetType(), "_holdingItem"))
            text.Append(" held=" + ItemText(R.Get(top, "_holdingItem")));
        else text.Append(" held=-");
        text.Append(" inv=");
        for (int index = 0; index < CarriedSlots; ++index) text.Append(index == 0 ? "" : ",").Append(ItemText(GetSlot(index)));
        if (screen == "crate" || Recorder.Events.Any(entry => entry.StartsWith("crate ", StringComparison.Ordinal)))
        {
            text.Append(" crate=");
            for (int index = 0; index < 32; ++index) text.Append(index == 0 ? "" : ",").Append(ItemText(GetSlot(100 + index)));
        }
        text.Append(" events=[" + string.Join("; ", Recorder.Events) + "]");
        Recorder.Events.Clear();
        return text.ToString();
    }

    // --------------------------------------------------------------- scenarios

    // The same fills the native captures and tests use.
    public const string PartialFill =
        "0=1x64 1=3x37 2=4x12 3=5x8 5=15x20 6=17x9 7=20x5 9=13x16 12=14x10 17=21x4 31=19x2 " +
        "32=23x1@0.6 33=29x1 34=34x1@0.25 35=14x24 36=51x1@0.9 37=44x200 39=49x1";
    public const string CrateFill = "100=3x64 101=3x64 102=5x40 105=22x7 110=18x3 119=73x1 131=79x2";

    private static string FullFill()
    {
        int[] ids = { 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                      29, 30, 34, 35, 39, 40, 41, 44, 49, 50, 51, 52, 53, 54 };
        var text = new StringBuilder();
        for (int index = 0; index < 40; ++index)
        {
            object item = MakeItem(ids[index], 1);
            int limit = (int)R.Get(R.Get(item, "ItemClass"), "MaxStackCount");
            int count = limit == 1 ? 1 : Math.Max(2, Math.Min(limit, 3 + index * 7 % 61));
            text.Append(index + "=" + ids[index] + "x" + count + " ");
        }
        return text.ToString();
    }

    private sealed class Scenario
    {
        public string Name;
        public string Screen;
        public string Fill;
        public string[] Actions;
    }

    private static IEnumerable<Scenario> Scenarios()
    {
        yield return new Scenario { Name = "inventory.empty", Screen = "inventory", Fill = "" };
        yield return new Scenario { Name = "inventory.partial", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 1" } };
        yield return new Scenario { Name = "inventory.tray_hover", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 34" } };
        yield return new Scenario { Name = "inventory.full", Screen = "inventory", Fill = FullFill(), Actions = new[] { "move 20" } };
        yield return new Scenario { Name = "inventory.held", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 0", "click left", "move 10" } };
        yield return new Scenario { Name = "inventory.held_outside", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 0", "click left", "move out" } };
        yield return new Scenario { Name = "inventory.pointer_off_grid", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 1", "move panel" } };
        if (!Modern)
        {
            yield return new Scenario { Name = "inventory.close_hover", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move close" } };
            yield return new Scenario { Name = "inventory.button_hover", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move button" } };
            yield return new Scenario { Name = "inventory.keyboard", Screen = "inventory", Fill = PartialFill, Actions = new[] { "key Up", "key Right" } };
            yield return new Scenario { Name = "crafting.first", Screen = "crafting", Fill = PartialFill };
            yield return new Scenario { Name = "crafting.empty", Screen = "crafting", Fill = "" };
            yield return new Scenario { Name = "crafting.down3", Screen = "crafting", Fill = PartialFill, Actions = new[] { "key Down", "key Down", "key Down" } };
            yield return new Scenario { Name = "crafting.ingredient", Screen = "crafting", Fill = PartialFill, Actions = new[] { "move ingredient0" } };
            yield return new Scenario { Name = "crafting.hover_other", Screen = "crafting", Fill = PartialFill, Actions = new[] { "move recipe4" } };
            yield return new Scenario { Name = "crafting.uncraftable", Screen = "crafting", Fill = "0=3x1", Actions = new[] { "key Down" } };
            yield return new Scenario { Name = "crafting.button_hover", Screen = "crafting", Fill = PartialFill, Actions = new[] { "move button" } };
        }
        else
        {
            yield return new Scenario { Name = "crafting.materials", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move recipe0" } };
            yield return new Scenario { Name = "crafting.components", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move tier2:1", "move recipe2" } };
            yield return new Scenario { Name = "crafting.tools", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move tier1:1", "move recipe1" } };
            yield return new Scenario { Name = "crafting.weapons", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move tier1:2", "move tier2:5", "move recipe1" } };
            yield return new Scenario { Name = "crafting.structures", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move tier1:3", "move tier2:3", "move recipe0" } };
            yield return new Scenario { Name = "inventory.tooltip", Screen = "inventory", Fill = PartialFill, Actions = new[] { "move 1", "wait 0.6" } };
            yield return new Scenario { Name = "inventory.second_tray", Screen = "inventory", Fill = PartialFill + " 40=50x1 43=80x4 47=74x30", Actions = new[] { "move 43" } };
        }
        yield return new Scenario { Name = "crate.empty", Screen = "crate", Fill = PartialFill };
        yield return new Scenario { Name = "crate.partial", Screen = "crate", Fill = PartialFill + " " + CrateFill, Actions = new[] { "move 102" } };
        yield return new Scenario { Name = "crate.inventory_hover", Screen = "crate", Fill = PartialFill + " " + CrateFill, Actions = new[] { "move 5" } };
        yield return new Scenario { Name = "crate.held", Screen = "crate", Fill = PartialFill + " " + CrateFill, Actions = new[] { "move 100", "click left", "move 108" } };
        yield return new Scenario { Name = "crate.in_use", Screen = "crate", Fill = PartialFill + " " + CrateFill, Actions = new[] { "lock 2", "move 102" } };
        if (Modern)
            yield return new Scenario { Name = "crate.tooltip", Screen = "crate", Fill = PartialFill + " " + CrateFill, Actions = new[] { "move 102", "wait 1.4" } };
    }

    private static void RunScenarios(string filter)
    {
        string prefix = (Modern ? "modern." : "classic.");
        string suffix = "." + Width + "x" + Height;
        foreach (Scenario scenario in Scenarios())
        {
            if (filter != null && scenario.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
            NewSession();
            Give(scenario.Fill);
            Open(scenario.Screen);
            try
            {
                foreach (string action in scenario.Actions ?? new string[0]) Act(action);
            }
            catch (Exception error)
            {
                throw new InvalidOperationException(prefix + scenario.Name + ": " + State(), error);
            }
            // Settle: one more frame with the pointer at rest.
            InputFrame();
            var lines = new List<string>
            {
                "# " + prefix + scenario.Name + " " + Width + "x" + Height,
                "# given " + scenario.Fill.Trim(),
                "# open " + scenario.Screen,
            };
            foreach (string action in scenario.Actions ?? new string[0]) lines.Add("# " + action);
            lines.Add("# " + State());
            lines.AddRange(Recorder.Lines);
            File.WriteAllLines(Path.Combine(OutputDirectory, "draws", prefix + scenario.Name + suffix + ".txt"), lines);
            SaveFrame(Path.Combine(OutputDirectory, "frames", prefix + scenario.Name + suffix + ".png"));
        }
        var atlas = Recorder.ItemAtlas();
        if (atlas != null)
        {
            var pixels = new Color[atlas.Width * atlas.Height];
            Device.SetRenderTarget(null);
            atlas.GetData(pixels);
            SavePng(Path.Combine(OutputDirectory, "item-atlas." + (Modern ? "modern" : "classic") + ".png"), pixels,
                    atlas.Width, atlas.Height);
        }
    }

    // ------------------------------------------------------------ interactions

    private sealed class Script
    {
        public string Name, Screen, Fill;
        public string[] Steps;
    }

    private static IEnumerable<Script> Scripts()
    {
        string both = PartialFill;
        // Left button: pick up, place, merge, swap.
        yield return new Script { Name = "left.pick_place", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left", "move 4", "click left", "move 4", "click left", "move 1", "click left", "move 8", "click left" } };
        yield return new Script { Name = "left.merge_same", Screen = "inventory", Fill = "0=3x40 1=3x40 2=3x10",
            Steps = new[] { "move 0", "click left", "move 1", "click left", "move 2", "click left", "move 3", "click left" } };
        yield return new Script { Name = "left.empty_slot", Screen = "inventory", Fill = both,
            Steps = new[] { "move 4", "click left", "move 38", "click left" } };
        // Right button: split, place one, top up, swap, full target.
        yield return new Script { Name = "right.split_place_one", Screen = "inventory", Fill = "0=3x37 1=3x1 2=5x7",
            Steps = new[] { "move 0", "click right", "move 4", "click right", "click right", "move 1", "click right", "move 2", "click right", "move 5", "click right" } };
        yield return new Script { Name = "right.single_and_odd", Screen = "inventory", Fill = "0=3x1 1=3x5 2=23x1",
            Steps = new[] { "move 0", "click right", "move 3", "click right", "move 1", "click right", "move 4", "click left", "move 2", "click right" } };
        yield return new Script { Name = "right.full_target", Screen = "inventory", Fill = "0=3x64 1=3x64 2=3x2",
            Steps = new[] { "move 2", "click left", "move 0", "click right", "click left", "move 1", "click right" } };
        // Shift + left: backpack <-> tray, with and without a held stack.
        yield return new Script { Name = "shift.backpack_to_tray", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left shift", "move 12", "click left shift", "move 1", "click left shift", "move 2", "click left shift", "move 3", "click left shift", "move 5", "click left shift" } };
        yield return new Script { Name = "shift.tray_to_backpack", Screen = "inventory", Fill = both,
            Steps = new[] { "move 35", "click left shift", "move 32", "click left shift", "move 38", "click left shift" } };
        yield return new Script { Name = "shift.held_stack", Screen = "inventory", Fill = both,
            Steps = new[] { "move 1", "click left", "move 2", "click left shift", "move 5", "click left", "move 33", "click left shift" } };
        // Wheel: one item at a time between the slot and the hand.
        yield return new Script { Name = "wheel.move_one", Screen = "inventory", Fill = "0=3x3 1=5x64 2=3x64",
            Steps = new[] { "move 0", "wheel up", "wheel up", "wheel up", "wheel up", "move 4", "wheel down", "wheel down", "move 1", "wheel down", "wheel up", "move 2", "wheel down" } };
        // Q drops, clicking outside the panel drops, Escape/E close.
        yield return new Script { Name = "drop.q", Screen = "inventory", Fill = both,
            Steps = new[] { "move 1", "key Q", "move 0", "click left", "key Q", "move 4", "key Q" } };
        yield return new Script { Name = "drop.outside_left", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left", "move out", "click left", "click left" } };
        yield return new Script { Name = "drop.outside_right", Screen = "inventory", Fill = "0=3x3",
            Steps = new[] { "move 0", "click left", "move out", "click right", "click right", "click right", "click right" } };
        yield return new Script { Name = "drop.panel_miss", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left", "move panel", "click left", "click right" } };
        yield return new Script { Name = "close.escape_with_held", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left", "key Escape", "key Escape" } };
        yield return new Script { Name = "close.escape_full_bag", Screen = "inventory", Fill = FullFill() + (Modern ? "40=1x64 41=1x64 42=1x64 43=1x64 44=1x64 45=1x64 46=1x64 47=1x64" : ""),
            Steps = new[] { "move 39", "click left", "move 38", "click left", "key Escape", "key Escape" } };
        yield return new Script { Name = "close.e_key", Screen = "inventory", Fill = both,
            Steps = new[] { "move 0", "click left", "key E" } };
        if (!Modern)
        {
            yield return new Script { Name = "close.x_button", Screen = "inventory", Fill = both,
                Steps = new[] { "move 0", "click left", "move close", "click left" } };
            yield return new Script { Name = "craft.button_opens", Screen = "inventory", Fill = both,
                Steps = new[] { "move 0", "click left", "move button", "click left" } };
            yield return new Script { Name = "keys.navigate", Screen = "inventory", Fill = both,
                Steps = new[] { "key Up", "key Up", "key Left", "key Down", "key Right", "key Enter", "key Down", "key Enter" } };
            // 1.6.3.8 crafting: select, then click the selected icon or Create Item.
            yield return new Script { Name = "craft.select_then_craft", Screen = "crafting", Fill = both,
                Steps = new[] { "move recipe2", "click left", "move recipe3", "click left", "wait 0.5", "move recipe2", "click left", "move button", "click left" } };
            yield return new Script { Name = "craft.shift_all", Screen = "crafting", Fill = "0=4x10",
                Steps = new[] { "move recipe2", "click left shift", "key Down", "move recipe2", "click left shift" } };
            yield return new Script { Name = "craft.missing", Screen = "crafting", Fill = "0=3x1",
                Steps = new[] { "move recipe2", "click left", "move button", "click left", "key Down", "move recipe2", "click left" } };
            yield return new Script { Name = "craft.ingredient_jump", Screen = "crafting", Fill = both,
                Steps = new[] { "key Down", "key Down", "move ingredient0", "click left", "wait 0.6", "move ingredient1", "click left", "wait 0.6" } };
            yield return new Script { Name = "craft.wheel_and_keys", Screen = "crafting", Fill = both,
                Steps = new[] { "wheel down", "wheel down", "wheel up", "key Down", "key Right", "key Right", "key Left", "key Up", "key Up", "key Up" } };
            yield return new Script { Name = "craft.close", Screen = "crafting", Fill = both,
                Steps = new[] { "key E" } };
            yield return new Script { Name = "craft.grid_click_closes", Screen = "crafting", Fill = both,
                Steps = new[] { "move 5", "click left" } };
            yield return new Script { Name = "craft.creative", Screen = "crafting", Fill = "0=4x1",
                Steps = new[] { "mode Survival infinite", "move recipe2", "click left", "click left", "key Down", "move recipe2", "click left" } };
        }
        else
        {
            // 1.9.9.8 crafting: hover selects, a click crafts.
            yield return new Script { Name = "craft.hover_click", Screen = "inventory", Fill = both,
                Steps = new[] { "move recipe0", "click left", "move tier2:1", "move recipe0", "click left", "click left" } };
            yield return new Script { Name = "craft.shift_all", Screen = "inventory", Fill = "0=4x10",
                Steps = new[] { "move tier2:1", "move recipe0", "click left shift", "move recipe1", "click left shift" } };
            yield return new Script { Name = "craft.missing", Screen = "inventory", Fill = "0=3x1",
                Steps = new[] { "move tier1:1", "move recipe1", "click left", "move tier1:2", "move tier2:2", "move recipe0", "click left" } };
            yield return new Script { Name = "craft.tabs", Screen = "inventory", Fill = both,
                Steps = new[] { "move tier1:2", "move tier2:4", "move tier1:3", "move tier2:2", "move tier1:0", "move tier2:1", "move recipe3" } };
            yield return new Script { Name = "craft.held_blocks_tabs", Screen = "inventory", Fill = both,
                Steps = new[] { "move 0", "click left", "move tier1:2", "move recipe0", "click left" } };
            yield return new Script { Name = "craft.creative", Screen = "inventory", Fill = "",
                Steps = new[] { "mode Creative infinite", "move tier1:1", "move recipe4", "click left", "click left shift" } };
            yield return new Script { Name = "tray.second", Screen = "inventory", Fill = both + " 40=3x10",
                Steps = new[] { "move 40", "click left", "move 47", "click left", "move 1", "click left shift", "move 47", "click left shift" } };
        }
        // Crate.
        string crateFill = both + " " + CrateFill;
        yield return new Script { Name = "crate.pick_place", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 100", "click left", "move 0", "click left", "move 104", "click left", "move 101", "click right", "move 103", "click right", "click right" } };
        yield return new Script { Name = "crate.shift_transfer", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 100", "click left shift", "move 1", "click left shift", "move 33", "click left shift", "move 102", "click left shift" } };
        yield return new Script { Name = "crate.shift_held", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 1", "click left", "move 110", "click left shift", "move 105", "click left", "move 5", "click left shift" } };
        yield return new Script { Name = "crate.wheel", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 105", "wheel up", "wheel up", "move 106", "wheel down", "move 0", "wheel down" } };
        yield return new Script { Name = "crate.drop", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 110", "key Q", "move 2", "key Q", "move 100", "click left", "move out", "click left" } };
        yield return new Script { Name = "crate.escape_with_held", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 100", "click left", "key Escape" } };
        yield return new Script { Name = "crate.locked_slot", Screen = "crate", Fill = crateFill,
            Steps = new[] { "lock 2", "move 102", "click left", "click right", "key Q", "wheel up", "click left shift", "move 0", "click left shift", "move 3", "click left", "move 102", "click left", "wheel down" } };
        // When CrateFocusMessage is sent (the `focus` events): arrow keys from
        // the tray into the crate grid and round it, the pointer over crate and
        // inventory slots, a key after the pointer, a held key, then Escape.
        yield return new Script { Name = "crate.focus_keys", Screen = "crate", Fill = crateFill,
            Steps = new[] { "key Down", "key Down", "key Right", "key Left", "key Left", "key Up", "key Up", "key Up", "key Right",
                            "move 105", "move 106", "move 105", "move 3", "move 100", "move out", "key Down", "key Right",
                            "hold Right 0.8", "key Escape" } };
        yield return new Script { Name = "crate.focus_pointer", Screen = "crate", Fill = crateFill,
            Steps = new[] { "move 100", "move 101", "move 131", "click left", "move 5", "click left", "move 131", "move 34", "move 4", "click left", "key Escape" } };
    }

    private static void RunInteractions()
    {
        var lines = new List<string> { "# " + (Modern ? "modern" : "classic") + " interaction scripts, 1280x720" };
        foreach (Script script in Scripts())
        {
            NewSession();
            Give(script.Fill);
            Open(script.Screen);
            lines.Add("script " + script.Name);
            lines.Add("given " + script.Fill.Trim());
            lines.Add("open " + script.Screen);
            // What opening the screen itself did (CrateScreen.OnPushed sends
            // CrateFocusMessage). Kept on a line of its own so the states and
            // their pinned hashes are what they were.
            lines.Add("opened events=[" + string.Join("; ", Recorder.Events) + "]");
            Recorder.Events.Clear();
            lines.Add(State());
            foreach (string step in script.Steps)
            {
                lines.Add("step " + step);
                try { Act(step); lines.Add(State()); }
                catch (Exception error)
                {
                    lines.Add("state threw=" + error.GetType().Name + " " + Recorder.Quote(error.Message));
                    break;
                }
            }
        }
        File.WriteAllLines(Path.Combine(OutputDirectory, "interaction." + (Modern ? "modern" : "classic") + ".txt"), lines);
    }

    // ------------------------------------------------------------------ tables

    private static List<DictionaryEntry> Entries(IDictionary dictionary)
    {
        var list = new List<DictionaryEntry>();
        foreach (DictionaryEntry entry in dictionary) list.Add(entry);
        return list;
    }

    private static void DumpTables()
    {
        string generation = Modern ? "modern" : "classic";
        var sprites = new List<string>();
        foreach (string field in new[] { "_uiSprites", "_logoSprites" })
        {
            if (!R.Has(GameType, field)) continue;
            object manager = R.Get(GameObject, field);
            if (manager == null) continue;
            foreach (DictionaryEntry entry in Entries((IDictionary)R.Get(manager, "_sprites")).OrderBy(pair => (string)pair.Key, StringComparer.Ordinal))
            {
                var texture = (Texture2D)R.Get(entry.Value, "Texture");
                var source = (Rectangle)R.Get(entry.Value, "SourceRectangle");
                sprites.Add(field + " " + entry.Key + " " + source.X + " " + source.Y + " " + source.Width + " " + source.Height +
                            " " + texture.Width + " " + texture.Height + " " + texture.Format);
            }
        }
        File.WriteAllLines(Path.Combine(OutputDirectory, "sprites." + generation + ".txt"), sprites);

        var fonts = new List<string>();
        Type strings = T("DNA.CastleMinerZ.Globalization.Strings");
        var samples = new List<string> { "x", "0", "64", "200", "999", "Ag", " " };
        foreach (string name in new[] { "Craft", "Right_Click", "Split_stack", "Split_Items", "Wheel_Scroll", "Grab_or_place_items",
                                        "Components", "Crafting", "Inventory", "Crate", "Press", "Materials", "Tools", "Weapons", "Structures" })
        {
            PropertyInfo property = strings.GetProperty(name, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            if (property == null) continue;
            string value = (string)property.GetValue(null, null);
            fonts.Add("string " + name + " " + Recorder.Quote(value));
            samples.Add(value);
        }
        foreach (string field in new[] { "_smallFont", "_medFont", "_medLargeFont", "_largeFont", "_myriadSmall", "_myriadMed", "_myriadLarge", "_consoleFont", "_systemFont", "_nameTagFont" })
        {
            var font = (SpriteFont)R.Get(GameObject, field);
            var texture = (Texture2D)R.Get(font, "textureValue");
            fonts.Add("font " + field + " line=" + font.LineSpacing + " spacing=" + Recorder.F(font.Spacing) + " glyphs=" + font.Characters.Count +
                      " sheet=" + texture.Width + "x" + texture.Height + " default=" + (font.DefaultCharacter.HasValue ? ((int)font.DefaultCharacter.Value).ToString(Invariant) : "none"));
            foreach (string sample in samples)
            {
                Vector2 size;
                try { size = font.MeasureString(sample); } catch (ArgumentException) { continue; }
                fonts.Add("measure " + field + " " + Recorder.F(size.X) + " " + Recorder.F(size.Y) + " " + Recorder.Quote(sample));
            }
        }
        File.WriteAllLines(Path.Combine(OutputDirectory, "fonts." + generation + ".txt"), fonts);

        itemType = T("DNA.CastleMinerZ.Inventory.InventoryItem");
        var items = new List<string>();
        var all = (IDictionary)R.GetStatic(itemType, "AllItems");
        foreach (DictionaryEntry entry in Entries(all).OrderBy(pair => Convert.ToInt32(pair.Key, Invariant)))
        {
            object itemClass = entry.Value;
            var line = new StringBuilder("item " + Convert.ToInt32(entry.Key, Invariant) + " " + entry.Key);
            line.Append(" max=" + R.Get(itemClass, "MaxStackCount"));
            line.Append(" wear=" + ((float)R.Get(itemClass, "ItemSelfDamagePerUse") > 0f ? 1 : 0));
            line.Append(" name=" + Recorder.Quote((string)R.Get(itemClass, "Name") ?? ""));
            if (Modern) line.Append(" description=" + Recorder.Quote((string)R.Get(itemClass, "Description") ?? ""));
            else
            {
                line.Append(" description1=" + Recorder.Quote((string)R.Get(itemClass, "Description1") ?? ""));
                line.Append(" description2=" + Recorder.Quote((string)R.Get(itemClass, "Description2") ?? ""));
            }
            items.Add(line.ToString());
        }
        File.WriteAllLines(Path.Combine(OutputDirectory, "items." + generation + ".txt"), items);
    }
}
