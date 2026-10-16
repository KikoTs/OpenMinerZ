// What a player reads, held to the shipped CastleMiner Z builds.
//
// Three modes:
//   (no arguments)                       the tables against values recorded from
//                                        the owned 1.6.3.8 and 1.9.9.8 installs
//                                        (tests/presentation_parity_pinned.hpp);
//   --compare <facts> <classic|modern> [derived data dir]
//                                        the tables against a fresh run of
//                                        tools/OriginalClientInterop/
//                                        PresentationParityProbe, and, when a
//                                        content pack's DerivedData is given,
//                                        the imported sprite fonts against the
//                                        original SpriteFont's own measurements;
//   --write-facts <file> <classic|modern> [derived data dir]
//                                        this side's facts, for inspection.
//
// Both sides describe themselves with the same key=value facts. Every key the
// native side answers must be present in the reference with the same value,
// and every reference key in a family the native side claims to cover
// (strings, item text, block names, achievements, fonts) must be answered.

#include "openminerz/inventory.hpp"
#include "openminerz/player_progress.hpp"
#include "openminerz/retail_text.hpp"
#include "openminerz/sprite_font.hpp"
#include "openminerz/world/block.hpp"
#include "presentation_parity_pinned.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;

using Facts = std::map<std::string, std::string, std::less<>>;

int failures = 0;

void expect(bool value, std::string_view message)
{
    if (value)
        return;
    ++failures;
    std::cerr << "FAIL " << message << '\n';
}

std::string escape(std::string_view text)
{
    std::string result;
    for (const char c : text)
    {
        if (c == '\\')
            result += "\\\\";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\n')
            result += "\\n";
        else
            result += c;
    }
    return result;
}

std::uint32_t fnv1a(std::string_view text, std::uint32_t hash = 2166136261U)
{
    for (const unsigned char c : text)
        hash = (hash ^ c) * 16777619U;
    return hash;
}

// Shared with PresentationParityProbe (fontSamples).
constexpr std::array<std::string_view, 11> font_samples{
    "H",          "Hg",         "Stone PickAxe", "Click To Respawn",
    "Loading The World... ",    "AVATAR To.",    "1234567890",
    "Two\nLines", "tab\there",  "na\xC3\xAFve caf\xC3\xA9", "\xE2\x98\x83 snow",
};

std::string float_text(float value)
{
    // The probe writes float.ToString("R"): the shortest text that round-trips.
    return std::format("{}", value);
}

// InventoryItemIDs as each build numbers them: 1.9.9.8's 123..154 are the
// native enum's 256..287 (inventory.hpp).
InventoryItemId native_item(int wire) noexcept
{
    return static_cast<InventoryItemId>(wire >= 123 ? wire + 133 : wire);
}

Facts native_facts(RetailGeneration generation, const std::filesystem::path &derived)
{
    Facts facts;
    const bool classic = generation == RetailGeneration::classic;
    const InventoryProfile profile = classic ? InventoryProfile::classic : InventoryProfile::modern;

    // Strings. DNA.Common's shared words follow the game's own table.
    std::uint32_t string_count = 0U;
    for (std::size_t index = 0U; index < retail_string_count; ++index)
    {
        const auto id = static_cast<RetailString>(index);
        if (!retail_text_exists(id, generation))
            continue;
        const bool common = id >= RetailString::Common_Address_not_available;
        facts[std::string{common ? "resource.DNA.CommonResources." : "string."} +
              std::string{retail_string_key(id)}] = escape(retail_text(id, generation));
        if (!common)
            ++string_count;
    }
    std::uint32_t hash = 2166136261U;
    for (const auto &[key, value] : facts)
        if (key.starts_with("string."))
            hash = fnv1a(key + "=" + value + "\n", hash);
    facts["string.count"] = std::to_string(string_count);
    facts["string.hash"] = std::format("{:08x}", hash);

    // Items.
    int item_count = 0;
    for (int wire = 0; wire <= 154; ++wire)
    {
        const InventoryItemId item = native_item(wire);
        if ((classic && wire > 122) || !item_registered(item, profile))
            continue;
        ++item_count;
        const ItemText text = item_text(item, profile);
        const std::string key = std::format("item.{}.", wire);
        facts[key + "name"] = escape(text.name);
        if (classic)
        {
            facts[key + "description1"] = escape(text.description1);
            facts[key + "description2"] = escape(text.description2);
        }
        else
        {
            facts[key + "description"] = escape(text.description1);
        }
    }
    facts["item.count"] = std::to_string(item_count);

    // Blocks: 1.6.3.8 has ids 0..44, 1.9.9.8 0..93.
    const int block_count = classic ? 45 : 94;
    for (int block = 0; block < block_count; ++block)
        facts[std::format("block.{}.name", block)] =
            escape(world::block_name(static_cast<world::BlockType>(block)));
    facts["block.count"] = std::to_string(block_count);

    // Achievements.
    const auto achievements = achievement_definitions();
    for (std::size_t index = 0U; index < achievements.size(); ++index)
    {
        const std::string key = std::format("achievement.{}.", index);
        facts[key + "api"] = escape(achievements[index].id);
        facts[key + "name"] = escape(achievements[index].name);
        facts[key + "how"] = escape(achievements[index].description);
    }
    facts["achievement.count"] = std::to_string(achievements.size());

    // Fonts, when a content pack has been imported.
    if (!derived.empty())
    {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(RetailFont::count); ++index)
        {
            const std::string_view name = retail_font_name(static_cast<RetailFont>(index));
            const auto font = SpriteFont::load(derived / std::format("font_{}.omfnt", name));
            if (!font)
                continue;
            const std::string key = std::format("font.{}.", name);
            facts[key + "line_spacing"] = std::to_string(font->line_spacing());
            facts[key + "spacing"] = float_text(font->spacing());
            facts[key + "glyphs"] = std::to_string(font->glyphs().size());
            facts[key + "default"] = std::to_string(
                font->default_character() ? static_cast<int>(*font->default_character()) : -1);
            facts[key + "first"] =
                std::to_string(static_cast<int>(font->glyphs().front().character));
            facts[key + "last"] = std::to_string(static_cast<int>(font->glyphs().back().character));
            facts[key + "cap_height"] = std::to_string(font->cap_height());
            facts[key + "cap_top"] = std::to_string(font->cap_top());
            facts[key + "table_hash"] = std::format("{:08x}", font->table_hash());
            for (std::size_t sample = 0U; sample < font_samples.size(); ++sample)
            {
                const SpriteFontExtent size = font->measure(font_samples[sample]);
                facts[std::format("{}measure.{}", key, sample)] =
                    float_text(size.width) + "x" + float_text(size.height);
            }
        }
    }
    return facts;
}

// The reference key families the native side must answer in full.
bool covered(std::string_view key, bool fonts)
{
    if (key.starts_with("string.") || key.starts_with("resource.DNA.CommonResources.") ||
        key.starts_with("block.") || key.starts_with("achievement."))
        return !key.ends_with(".enum") && !key.ends_with(".class");
    if (key.starts_with("item."))
        return !key.ends_with(".enum");
    if (fonts && key.starts_with("font."))
    {
        // Only the ten fonts CastleMinerZGame.LoadContent loads are imported;
        // the Twitch overlay and controller-glyph fonts are not game text.
        const std::string_view rest = key.substr(5U);
        const std::string_view name = rest.substr(0U, rest.find('.'));
        bool imported = false;
        for (std::size_t index = 0U; index < static_cast<std::size_t>(RetailFont::count); ++index)
            imported |= retail_font_name(static_cast<RetailFont>(index)) == name;
        return imported && !key.ends_with(".folder") && !key.ends_with(".texture");
    }
    return false;
}

void compare(const Facts &reference, const Facts &native, bool fonts, std::string_view label)
{
    const int before = failures;
    int compared = 0;
    // The pinned reference holds the string table as a count and a hash; a
    // probe run holds every entry and no hash.
    const bool hashed = reference.contains("string.hash");
    for (const auto &[key, value] : native)
    {
        const bool entry = (key.starts_with("string.") && key != "string.count" &&
                            key != "string.hash") ||
                           key.starts_with("resource.");
        if (hashed ? entry : key == "string.hash")
            continue;
        const auto found = reference.find(key);
        if (found == reference.end())
        {
            expect(false, std::format("{}: {} is not in the original (ours: \"{}\")", label, key,
                                      value));
            continue;
        }
        ++compared;
        expect(found->second == value, std::format("{}: {} original \"{}\" ours \"{}\"", label,
                                                   key, found->second, value));
    }
    for (const auto &[key, value] : reference)
        if (covered(key, fonts) && !native.contains(key))
            expect(false, std::format("{}: {} = \"{}\" is not answered", label, key, value));
    std::cout << label << ": " << compared << " facts compared, " << failures - before
              << " mismatches\n";
}

Facts read_facts(const std::filesystem::path &path)
{
    Facts facts;
    std::ifstream file{path, std::ios::binary};
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t split = line.find('=');
        if (split != std::string::npos && split != 0U)
            facts[line.substr(0U, split)] = line.substr(split + 1U);
    }
    return facts;
}

template <std::size_t Size>
Facts pinned_facts(const std::array<presentation_pinned::Fact, Size> &table)
{
    Facts facts;
    for (const presentation_pinned::Fact &fact : table)
        // The pinned literals hold the text itself; facts are compared escaped.
        facts[std::string{fact.key}] = escape(fact.value);
    return facts;
}

// What the tables are FOR: the words a screen composes from them.
void check_composition()
{
    using G = RetailGeneration;
    using S = RetailString;
    namespace message = retail_message;
    expect(retail_text(S::Click_To_Respawn, G::classic) == "Click To Respawn" &&
               retail_text(S::Waiting_For_Host_To_Restart, G::modern) ==
                   "Waiting For Host To Restart",
           "death prompts");
    // 1.9.9.8-only entries answer in a Classic presentation too.
    expect(!retail_text_exists(S::Sprint, G::classic) && retail_text(S::Sprint, G::classic) == "Sprint",
           "an entry only 1.9.9.8 has is named by 1.9.9.8");
    expect(retail_text(S::Controller_Sensitivity, G::classic) == "Controller Sensitivity" &&
               retail_text(S::Controller_Sensitivity, G::modern) == "Sensitivity" &&
               retail_text(S::Erase_Storage, G::classic) == "Erase Storage" &&
               retail_text(S::Erase_Storage, G::modern) == "Erase Saves",
           "entries the two builds word differently");
    expect(find_retail_string("Loading_The_World____") == S::Loading_The_World &&
               retail_string_key(S::Loading_The_World) == "Loading_The_World____" &&
               !find_retail_string("No_Such_Key"),
           "lookup by shipped resource key");
    expect(message::loading_world(G::classic) == "Loading The World... Please Wait..." &&
               message::connecting(G::modern) == "Connecting... Please Wait...",
           "load screen captions");
    expect(message::player_line("Steve", S::Has_Fallen, G::classic) == "Steve Has Fallen" &&
               message::player_line("Steve", S::Has_Respawned, G::modern) == "Steve Has Respawned" &&
               message::player_line("Steve", S::has_been_kicked_by_the_host, G::modern) ==
                   "Steve has been kicked by the host",
           "broadcast lines");
    expect(message::player_joined("Steve", G::classic) == "Player Joined: Steve" &&
               message::player_left("Steve", G::modern) == "Player Left: Steve",
           "join and leave lines");
    expect(message::dragon_killed("Steve", "Undead Dragon", G::classic) ==
               "Steve Has Killed The Undead Dragon",
           "dragon kill line");
    // 1.6.3.8 really omits the space; 1.9.9.8 added it.
    expect(message::achievement_earned("Steve", "Tinkerer", G::classic) ==
                   "SteveHas earned 'Tinkerer'" &&
               message::achievement_earned("Steve", "Tinkerer", G::modern) ==
                   "Steve Has earned 'Tinkerer'",
           "achievement broadcast");
    expect(message::respawn_countdown(17, G::classic) == "Respawn In: 17" &&
               message::distance_traveled(412, G::classic) == "Distance Traveled: 412" &&
               message::in_days(1, G::classic) == " In 1 Day" &&
               message::in_days(3, G::modern) == " In 3 Days",
           "death summary lines");
    expect(message::chat_line("Steve", "hello") == "Steve: hello", "chat line");
    expect(retail_game_mode_name(GameMode::dragon_endurance, G::classic) == "Dragon Endurance" &&
               retail_game_mode_name(GameMode::scavenger, G::classic) == "Scavenger" &&
               retail_difficulty_name(GameDifficulty::hard, G::modern) == "Normal" &&
               retail_difficulty_name(GameDifficulty::no_enemies, G::classic) == "No Enemies" &&
               retail_game_mode_description(GameMode::creative, G::classic) ==
                   "Build structures with unlimited resources. You can play with or without enemies" &&
               retail_game_mode_description(GameMode::creative, G::modern)
                       .find("Fly Mode is enabled!") != std::string_view::npos,
           "game mode and difficulty names");

    // Item text as each build shows it.
    const ItemText classic_stick = item_text(InventoryItemId::stick, InventoryProfile::classic);
    const ItemText modern_stick = item_text(InventoryItemId::stick, InventoryProfile::modern);
    expect(classic_stick.name == "Wood Stick" &&
               classic_stick.description1 == "Use this to make various items" &&
               classic_stick.description2 == "Such as a pickaxe or a torch" &&
               modern_stick.name == "Wood Stick" &&
               modern_stick.description1 ==
                   "Use this to make various items. Such as a pickaxe or a torch" &&
               modern_stick.description2.empty(),
           "two description lines in 1.6.3.8, one joined line in 1.9.9.8");
    expect(item_name(InventoryItemId::pump_shotgun) == "Shotgun" &&
               item_name(InventoryItemId::gold) == "Gold" &&
               item_name(InventoryItemId::copper_space_smg) == "Laser Sub Machine Gun" &&
               item_name(InventoryItemId::guided_rocket_launcher) == "Anti Dragon Guided Missile" &&
               item_name(InventoryItemId::modern_tech_door) == "Strong Door" &&
               item_name(InventoryItemId::modern_lantern_fancy_block) == "Lantern" &&
               item_name(InventoryItemId::fired_rocket).empty(),
           "retail item names");
    // A 1.9.9.8-only item under a Classic presentation is described by 1.9.9.8.
    expect(item_text(InventoryItemId::modern_sticky_grenade, InventoryProfile::classic).name ==
                   "Sticky Grenade" &&
               item_text(InventoryItemId::modern_sticky_grenade, InventoryProfile::classic)
                       .description1 == "Sticks to terrain and zombies!",
           "an item only 1.9.9.8 has is described by 1.9.9.8");
    expect(item_description(InventoryItemId::iron) == "Used to craft items. Made from Iron ore" &&
               item_description(InventoryItemId::stone_pickaxe) ==
                   "Used for breaking certain stones and ores",
           "single-line descriptions");
    // Every item either build registers has a name the screens can draw;
    // nothing falls back to a placeholder.
    for (int value = 0; value < static_cast<int>(InventoryItemId::item_count); ++value)
    {
        const auto item = static_cast<InventoryItemId>(value);
        if (!valid_inventory_item_id(item))
            continue;
        const bool shot = item == InventoryItemId::fired_rocket ||
                          item == InventoryItemId::fired_guided_rocket;
        expect(shot || !item_name(item).empty(), std::format("item {} has a name", value));
    }
    expect(world::block_name(world::BlockType::coal_ore) == "Coal" &&
               world::block_name(world::BlockType::diamond_ore) == "Diamonds" &&
               world::block_name(world::BlockType::deep_lava) == "Lava" &&
               world::block_name(world::BlockType::golden_wall) == "Golden Wall" &&
               world::block_name(world::BlockType::enemy_spawn_off) ==
                   "Monster Spawner (Right click to activate)" &&
               world::block_name(world::BlockType::boss_spawn_on) == "Active Spawner!",
           "aimed-block label names");
    const auto achievements = achievement_definitions();
    expect(achievements[1].name == "Veteren MinerZ" &&
               achievements[1].description == "Play Online For 10 Hours" &&
               achievements[12].description == "Survive for 1 Days" &&
               achievements[21].description == "Kill 1 Enemies",
           "achievement text keeps the original's spelling and grammar");
}

void check_fonts_without_a_pack()
{
    expect(retail_font_name(RetailFont::medium_large) == "MedLargeFont" &&
               retail_font_name(RetailFont::name_tag) == "NameTagFont",
           "font asset names");
    expect(!SpriteFont::load("does-not-exist.omfnt"), "a missing font is reported, not invented");
    // UTF-16 units, as the original's strings hold them.
    expect(utf16_units("a\xC3\xA9").size() == 2U && utf16_units("\xF0\x9F\x98\x80").size() == 2U &&
               utf16_units("\xFF") == std::u16string(1U, static_cast<char16_t>(0xFFFDU)),
           "UTF-8 is measured as the UTF-16 the original holds");
}

RetailGeneration generation_argument(std::string_view text)
{
    if (text == "classic")
        return RetailGeneration::classic;
    if (text == "modern")
        return RetailGeneration::modern;
    throw std::runtime_error{"generation must be classic or modern"};
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const std::vector<std::string_view> arguments{argv + 1, argv + argc};
        if (arguments.size() >= 3U && arguments[0] == "--write-facts")
        {
            const Facts facts = native_facts(
                generation_argument(arguments[2]),
                arguments.size() > 3U ? std::filesystem::path{arguments[3]} : std::filesystem::path{});
            std::ofstream file{std::filesystem::path{arguments[1]}, std::ios::binary};
            for (const auto &[key, value] : facts)
                file << key << '=' << value << '\n';
            return file ? 0 : 2;
        }
        if (arguments.size() >= 3U && arguments[0] == "--compare")
        {
            const RetailGeneration generation = generation_argument(arguments[2]);
            const std::filesystem::path derived =
                arguments.size() > 3U ? std::filesystem::path{arguments[3]} : std::filesystem::path{};
            const Facts reference = read_facts(std::filesystem::path{arguments[1]});
            if (reference.empty())
                throw std::runtime_error{"no facts in " + std::string{arguments[1]}};
            bool fonts = false;
            if (!derived.empty())
            {
                fonts = SpriteFont::load(derived / "font_MedFont.omfnt").has_value();
                expect(fonts, "the content pack has no imported sprite fonts (rerun the content import)");
            }
            compare(reference, native_facts(generation, fonts ? derived : std::filesystem::path{}),
                    fonts, arguments[2]);
            return failures == 0 ? 0 : 1;
        }

        compare(pinned_facts(presentation_pinned::classic),
                native_facts(RetailGeneration::classic, {}), false, "classic 1.6.3.8 (pinned)");
        compare(pinned_facts(presentation_pinned::modern),
                native_facts(RetailGeneration::modern, {}), false, "modern 1.9.9.8 (pinned)");
        check_composition();
        check_fonts_without_a_pack();
        if (failures != 0)
        {
            std::cerr << failures << " presentation parity failures\n";
            return 1;
        }
        std::cout << "presentation parity tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "presentation parity test error: " << error.what() << '\n';
        return 2;
    }
}
