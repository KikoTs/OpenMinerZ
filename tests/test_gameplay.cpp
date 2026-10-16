#include "openminerz/game_mode.hpp"
#include "openminerz/menu_model.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/player_progress.hpp"
#include "openminerz/world/encounters.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/dragon_brain.hpp"
#include "openminerz/world/explosive.hpp"
#include "openminerz/world_save.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace openminerz;
namespace
{
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void playtest_encounters_and_colors()
{
    using namespace world;
    // Repeated attempts while another encounter owns the slot do not advance
    // the durable index. Reaching 5 km still yields the pending undead boss.
    int completed = 3;
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        const auto pending = pending_distance_dragon(5000.0F, completed);
        check(pending && pending->kind == DragonKind::undead && pending->next_index == 5,
              "busy or delayed encounter lost the final dragon");
    }
    completed = pending_distance_dragon(5000.0F, completed)->next_index;
    check(!pending_distance_dragon(5000.0F, completed), "accepted encounter repeats every frame");
    check(pending_distance_dragon(2700, 0)->kind == DragonKind::ice &&
          pending_distance_dragon(1200, 0)->kind == DragonKind::sand &&
          pending_distance_dragon(800, 0)->kind == DragonKind::forest,
          "fast travel queued a dragon from an old biome");
    for (int weapon = 0; weapon < 5; ++weapon)
    {
        const auto red = projectile_tint(static_cast<InventoryItemId>(82 + weapon));
        const auto green = projectile_tint(static_cast<InventoryItemId>(87 + weapon));
        const auto gold = projectile_tint(static_cast<InventoryItemId>(92 + weapon));
        const auto blue = projectile_tint(static_cast<InventoryItemId>(97 + weapon));
        check(red[0] > red[1]*5 && green[1] > green[0]*5 &&
              gold[0] > gold[2]*5 && gold[1] > gold[2]*5 && blue[2] > blue[0]*5,
              "laser tier colour was lost to its shared display name");
    }
    for (int item : {62, 63, 64, 65, 66, 121})
    {
        const auto diamond = projectile_tint(static_cast<InventoryItemId>(item));
        check(diamond[0] == 0 && diamond[1] == 1 && diamond[2] == 1,
              "diamond firearm lost its cyan trail");
    }
    WorldRegion unloaded{0, 0, 1, 1};
    check(!cavern_dragon_approach(unloaded, {0,-45,0}, 0),
          "dragon approach admitted unloaded terrain");
    WorldRegion cavern{-6, -6, 12, 12};
    for (int z = -90; z <= 90; ++z)
        for (int x = -90; x <= 90; ++x)
            for (int y = -46; y <= -15; ++y)
                cavern.set_block(x, y, z, BlockType::empty);
    const auto approach = cavern_dragon_approach(cavern, {0,-45,0}, 0);
    check(approach && approach->y < 0 && approach->y > -40,
          "Hell dragon was placed above the cavern instead of in its airspace");
    // A ceiling too low for the body/wings must not admit a clipping dragon.
    for (int z = -90; z <= 90; ++z)
        for (int x = -90; x <= 90; ++x)
            for (int y = -38; y <= -15; ++y)
                cavern.set_block(x, y, z, BlockType::rock);
    check(!cavern_dragon_approach(cavern, {0,-45,0}, 0),
          "Hell dragon intersects a low ceiling");
}

PlayerInventory empty_inventory()
{
    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::modern);
    for (auto &s : inventory.storage()) s.reset();
    for (auto &t : inventory.trays()) for (auto &s : t) s.reset();
    return inventory;
}

std::uint64_t recipe_hash(InventoryProfile profile)
{
    const auto protocol = profile == InventoryProfile::classic ? net::ProtocolVersion::classic_v3
                                                              : net::ProtocolVersion::modern_v4;
    std::uint64_t hash = 14695981039346656037ULL;
    const auto word = [&](std::uint16_t n) {
        for (int shift : {0, 8}) { hash ^= (n >> shift) & 255U; hash *= 1099511628211ULL; }
    };
    const auto item = [&](InventoryItemId id) {
        std::int16_t wire{};
        check(net::inventory_item_to_wire(id, protocol, wire), "recipe item has no wire identity");
        word(static_cast<std::uint16_t>(wire));
    };
    const auto &recipes = crafting_recipes(profile);
    word(static_cast<std::uint16_t>(recipes.size()));
    for (const auto &recipe : recipes)
    {
        item(recipe.result); word(recipe.result_count);
        for (const auto &part : recipe.ingredients) { item(part.item); word(part.count); }
    }
    return hash;
}

void crafting_and_modes()
{
    // Independent hashes extracted from owned 1.6.3.8 / 1.9.9.8 CookBook data.
    check(recipe_hash(InventoryProfile::classic) == 0xef223f6035c37d14ULL, "Classic recipe data diverged from the owned executable");
    check(recipe_hash(InventoryProfile::modern) == 0x470fec876209a814ULL, "Modern recipe data diverged from the owned executable");
    const auto &book = crafting_recipes(InventoryProfile::modern);
    const auto station = std::find_if(book.begin(), book.end(), [](const auto &r) { return r.result == InventoryItemId::modern_teleport_station; });
    check(std::count_if(book.begin(), book.end(), [&](const auto &r) { return r.result == station->result; }) == 3, "station alternatives missing");
    auto inventory = empty_inventory();
    check(!inventory.recipe_discovered(*station) && !inventory.can_craft(*station), "empty inventory discovered/crafted a station");
    inventory.tray(1)[0] = InventoryStack{InventoryItemId::copper, 40};
    check(inventory.recipe_discovered(*station), "inactive Modern tray did not discover a recipe");
    for (const auto &part : station->ingredients)
        if (part.item != InventoryItemId::copper) (void)inventory.add_item(part.item, part.count);
    check(inventory.can_craft(*station), "ingredients across trays/storage cannot craft");
    check(inventory.craft(*station) == 0 && inventory.count(station->result) == 1 &&
              inventory.count(InventoryItemId::copper) == 0, "craft did not conserve ingredients and output");
    check(!has_unrestricted_resources(GameMode::exploration, true) &&
              has_unrestricted_resources(GameMode::creative, true) &&
              has_unrestricted_resources(GameMode::survival, true), "Exploration was conflated with Creative");
    ui::MenuModel menu;
    Settings settings;
    (void)menu.activate(settings);
    check(menu.screen() == ui::MenuScreen::game_profile, "solo skipped profile selection");
    menu.set_selection(1, settings);
    (void)menu.activate(settings);
    check(menu.entries().size() == 7, "Modern modes missing");
    menu.set_selection(4, settings);
    check(menu.activate(settings).command == ui::MenuCommand::select_mode_exploration &&
              menu.screen() == ui::MenuScreen::difficulty, "Exploration menu is not playable");
    (void)menu.back();
    menu.set_selection(5, settings);
    check(menu.activate(settings).command == ui::MenuCommand::select_mode_scavenger, "Scavenger menu is not playable");
    menu.set_gameplay_profile(false, false);
    menu.open(ui::MenuScreen::game_mode);
    check(menu.entries().size() == 5 && !menu.entries()[2].enabled, "Classic unlock policy missing");
    menu.set_gameplay_profile(true, false);
    check(menu.entries()[2].enabled, "Modern incorrectly inherited the Classic dragon lock");
    menu.open(ui::MenuScreen::pause);
    const auto a = std::find_if(menu.entries().begin(), menu.entries().end(), [](const auto &e) { return e.command == ui::MenuCommand::open_achievements; });
    menu.set_selection(std::size_t(a - menu.entries().begin()), settings);
    (void)menu.activate(settings);
    check(menu.screen() == ui::MenuScreen::achievements && menu.entries().size() == 31, "achievement menu incomplete");
    (void)menu.back();
    check(menu.screen() == ui::MenuScreen::pause, "achievements lost pause return context");
}

void navigation_and_saves()
{
    WorldSaveData save;
    save.inventory = empty_inventory();
    check(save.inventory.register_spawn_point({-3, -40, 6}), "could not register spawn");
    check(save.inventory.register_teleport_station({20, 4, -30}, "Home") &&
              save.inventory.register_teleport_station({4000, -52, 700}, "Hell"), "could not register stations");
    check(save.inventory.register_teleport_station({20, 4, -30}, "Base") &&
              save.inventory.teleport_stations().size() == 2, "rename duplicated a station");
    check(!save.inventory.register_teleport_station({0, 65, 0}, "Invalid"), "invalid travel coordinate accepted");
    save.encounters.carved_cells = 67543;
    save.encounters.hell_bosses_spawned = 3;
    save.encounters.hell_countdown = 17;
    save.encounters.populated_chunks.insert({-3, 5});
    std::ostringstream output;
    check(write_world_save(output, save), "travel/encounter save failed");
    WorldSaveData restored;
    std::istringstream input(output.str());
    check(read_world_save(input, restored) && restored.version == 9 && restored.inventory.spawn_point() &&
              restored.inventory.spawn_point()->waypoint == std::array<float, 3>{-3, -40, 6} &&
              restored.inventory.teleport_stations().size() == 2 &&
              restored.inventory.teleport_stations()[0].waypoint_name == "Base" &&
              restored.encounters.carved_cells == 67543 && restored.encounters.hell_countdown == 17 &&
              restored.encounters.populated_chunks == save.encounters.populated_chunks, "navigation/encounters lost on reload");
    net::PlayerInventoryPayload payload;
    check(net::player_inventory_to_payload(restored.inventory, net::ProtocolVersion::modern_v4, payload) &&
              payload.spawn_point && payload.spawn_point->item_id == 133 && payload.teleport_stations.size() == 2 &&
              payload.teleport_stations[0]->custom_name == "Base", "native travel state did not reach PINV");
    net::PayloadError error{};
    auto wire = net::encode_player_inventory_payload(payload, net::ProtocolVersion::modern_v4, error);
    net::PlayerInventoryPayload decoded;
    check(wire && net::decode_player_inventory_payload(*wire, net::ProtocolVersion::modern_v4, decoded, error) &&
              decoded.spawn_point == payload.spawn_point && decoded.teleport_stations == payload.teleport_stations,
          "travel PINV did not round-trip");
    restored.inventory.remove_travel_destination({20, 4, -30});
    restored.inventory.remove_travel_destination({-3, -40, 6});
    check(!restored.inventory.spawn_point() && restored.inventory.teleport_stations().size() == 1,
          "destroyed destination remained registered");
    restored.inventory.teleport_stations()[0].waypoint_name = "Bad\nName";
    std::ostringstream invalid;
    check(!write_world_save(invalid, restored) &&
              !net::player_inventory_to_payload(restored.inventory, net::ProtocolVersion::modern_v4, payload),
          "invalid travel metadata reached a save or multiplayer payload");
}

void achievements()
{
    PlayerProgress p;
    p.observe(GameMode::survival, 0.5, 5000, -50, 0, 1.39F, 1.41F, InventoryItemId::pistol);
    p.record_enemy_kill(GameMode::survival, 0, InventoryItemId::pistol);
    p.record_craft(GameMode::survival, InventoryItemId::wood_block, 20);
    check(p.value(ProgressMetric::kills) == 0 && p.value(ProgressMetric::distance) == 0 &&
              p.value(ProgressMetric::crafted) == 0 && p.value(ProgressMetric::days) == 0,
          "Endurance milestones counted another mode");
    p.observe(GameMode::endurance, 0.5, 300, -41, 400, 1.39F, 1.41F, InventoryItemId::pistol);
    check(p.value(ProgressMetric::distance) == 500 && p.value(ProgressMetric::depth) == 41 &&
              p.value(ProgressMetric::days) == 1, "travel/depth/day thresholds incorrect");
    p.record_enemy_kill(GameMode::endurance, 51, InventoryItemId::modern_precision_laser);
    p.record_enemy_kill(GameMode::endurance, 50, InventoryItemId::tnt);
    p.record_enemy_kill(GameMode::endurance, 1, InventoryItemId::grenade);
    p.record_craft(GameMode::endurance, InventoryItemId::tnt, 1);
    p.record_dragon_kill(GameMode::endurance, 4, false, InventoryItemId::guided_rocket_launcher);
    check(p.value(ProgressMetric::undead_dragon) == 1 && p.value(ProgressMetric::guided_dragon) == 0 &&
              p.value(ProgressMetric::kills) == 3, "party milestone stole the killer's weapon credit");
    p.record_dragon_kill(GameMode::endurance, 0, true, InventoryItemId::guided_rocket_launcher);
    check(p.value(ProgressMetric::guided_dragon) == 1 && p.value(ProgressMetric::kills) == 4, "guided dragon kill credit missing");
    const auto earned = p.unlock_earned(1720000000);
    check(!earned.empty() && p.unlock_earned(1720000001).empty(), "achievement unlock is not idempotent");
    for (int second = 0; second < 360000; ++second)
        p.observe(GameMode::endurance, 1, 5000, -41, 0, 1.5F, 1.5F, InventoryItemId::pistol);
    for (int day = 1; day <= 365; ++day)
        p.observe(GameMode::endurance, 0.02, 0, 0, 0, float(day) + 0.39F, float(day) + 0.41F, InventoryItemId::pistol);
    for (int count = 0; count < 1000; ++count)
    {
        p.record_craft(GameMode::endurance, InventoryItemId::wood_block, 1);
        p.record_enemy_kill(GameMode::endurance, 0, InventoryItemId::pistol);
    }
    p.add(ProgressMetric::alien);
    (void)p.unlock_earned(1720000002);
    check(p.unlocked_count() == achievement_count, "not all 31 achievements are earnable");
    // 1.6.3.8 CraftingUIScreen: `while (CanCraft) Craft();` and only then
    // `itemStats.Crafted++` (and TotalItemsCrafted++ in Endurance), so a
    // Shift-click is one craft however many it made. 1.9.9.8 Tier2Item
    // increments inside the loop. (Read from both shipped exes.)
    check(stock_crafted_count(false, 40U) == 1U && stock_crafted_count(false, 1U) == 1U &&
              stock_crafted_count(false, 0U) == 0U,
          "a 1.6.3.8 Shift-craft counts as more than one craft");
    check(stock_crafted_count(true, 40U) == 40U && stock_crafted_count(true, 1U) == 1U,
          "a 1.9.9.8 Shift-craft does not count each craft");
    const auto data = serialize_player_progress(p);
    const auto loaded = parse_player_progress(data);
    check(loaded && loaded->unlocked_at == p.unlocked_at && loaded->items == p.items &&
              loaded->enemy_kills == p.enemy_kills, "achievement history or per-item statistics were lost");
    auto corrupt = data; corrupt[corrupt.size() - 2] = 'X';
    check(!parse_player_progress(corrupt) && !parse_player_progress(data.substr(0, data.size() - 30)),
          "corrupt/truncated player progress was accepted");
    const auto path = std::filesystem::temp_directory_path() /
                      ("omz-progress-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(save_player_progress(path, p) && save_player_progress(path, p), "progress checkpoint write failed");
    { std::ofstream broken(path); broken << "broken"; }
    check(load_player_progress(path).unlocked_count() == 31, "progress backup recovery failed");
    std::filesystem::remove(path);
    auto backup = path; backup += ".bak"; std::filesystem::remove(backup);
}

void encounters_and_harvest()
{
    using namespace world;
    check(explosive_breaks_block(ExplosiveKind::harvest, BlockType::diamond_ore, 0, 0, 0) &&
              !explosive_breaks_block(ExplosiveKind::laser, BlockType::diamond_ore, 0, 0, 0) &&
              !explosive_breaks_block(ExplosiveKind::harvest, BlockType::diamond_ore, 1, 0, 0),
          "harvest laser footprint/resource behavior wrong");
    check(!explosive_breaks_block(ExplosiveKind::harvest, BlockType::blood_stone, 0, 0, 0) &&
              !explosive_breaks_block(ExplosiveKind::harvest, BlockType::bedrock, 0, 0, 0) &&
              harvested_block(BlockType::iron_ore) && !harvested_block(BlockType::rock),
          "harvest destroyed protected rock or manufactured ordinary terrain drops");
    WorldRegion region(0, 0, 1, 1);
    auto &chunk = region.chunks()[0];
    for (int i = 1; i <= 2000; ++i) chunk.record_cave_site(1, -30, 1, i == 2000);
    chunk.record_hell_site(2, -55, 2);
    WorldEncounterState state;
    state.hell_countdown = 1;
    const auto edits = populate_chunk_encounters(chunk, region, 23, GameMode::survival, state);
    check(edits.size() == 2 && edits.back()[3] == int(BlockType::boss_spawn_off) &&
              state.carved_cells == 2000 && state.hell_bosses_spawned == 1 && state.hell_countdown == 8000,
          "cave or Hell progression was not populated");
    check(populate_chunk_encounters(chunk, region, 23, GameMode::survival, state).empty() &&
              state.carved_cells == 2000, "revisiting a chunk duplicated encounter progression");
    WorldEncounterState capped; capped.hell_bosses_spawned = 50; capped.hell_countdown = 0;
    const auto cap_edits = populate_chunk_encounters(chunk, region, 23, GameMode::survival, capped);
    check(std::none_of(cap_edits.begin(), cap_edits.end(), [](const auto &e) { return e[3] == int(BlockType::boss_spawn_off); }), "Hell boss cap ignored");
    WorldEncounterState endurance; endurance.hell_countdown = 1;
    (void)populate_chunk_encounters(chunk, region, 23, GameMode::endurance, endurance);
    check(endurance.hell_bosses_spawned == 0, "Modern Hell boss system leaked into Endurance");
    auto generated = WorldGenerator(23, TerrainProfile::modern, GameMode::scavenger).generate_region(0, 0, 4, 4);
    WorldEncounterState rich, survival;
    std::size_t rich_count{}, survival_count{};
    for (const auto &c : generated.chunks())
    {
        rich_count += populate_chunk_encounters(c, generated, 23, GameMode::scavenger, rich).size();
        survival_count += populate_chunk_encounters(c, generated, 23, GameMode::survival, survival).size();
    }
    check(rich_count > survival_count && rich.carved_cells > 0, "Scavenger rates do not affect actual generated caves");
    check(!WorldGenerator(23).generate_chunk(0, 0).generation_sites(), "Modern encounter metadata leaked into Classic terrain");
}

// Destroying a block only re-bakes the sections WorldRegion::edited_sections
// names. If that set were ever too small the world would keep stale lighting or
// stale geometry, so prove that rebuilding exactly that set reproduces, face for
// face, what rebuilding the whole region produces.
void edit_rebuilds_match_a_full_rebuild()
{
    using namespace world;
    const auto bake = [](const WorldRegion &region, const std::vector<ChunkCoordinate> &sections) {
        auto batches = region.visible_faces_by_chunk(sections);
        std::sort(batches.begin(), batches.end(), [](const auto &l, const auto &r) {
            return std::tie(l.coordinate.z, l.coordinate.x, l.coordinate.y) <
                   std::tie(r.coordinate.z, r.coordinate.x, r.coordinate.y);
        });
        return batches;
    };
    const auto identical = [](const ChunkFaceBatch &left, const ChunkFaceBatch &right) {
        if (left.faces.size() != right.faces.size()) return false;
        for (std::size_t i = 0; i < left.faces.size(); ++i)
        {
            const WorldFace &a = left.faces[i];
            const WorldFace &b = right.faces[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.direction != b.direction ||
                a.block != b.block || a.tile != b.tile || a.sky_light != b.sky_light ||
                a.block_light != b.block_light || a.lava_light != b.lava_light ||
                a.ambient_occlusion != b.ambient_occlusion ||
                a.indirect_light != b.indirect_light ||
                a.block_light_lobe != b.block_light_lobe || a.emission != b.emission)
                return false;
        }
        return true;
    };

    WorldGenerator generator(4242, TerrainProfile::modern, GameMode::survival);
    auto region = generator.generate_region(0, 0, 6, 6);
    // A surface cell under open sky, a cell with solid rock above it, and a
    // pair that straddle a section boundary: the three shapes the selection
    // rule distinguishes.
    std::vector<std::array<int, 3>> edits;
    const int probe_x = region.minimum_x() + 40;
    const int probe_z = region.minimum_z() + 40;
    for (int y = world_min_y + chunk_height - 1; y > world_min_y; --y)
        if (region.block(probe_x, y, probe_z) != BlockType::empty)
        {
            edits.push_back({probe_x, y, probe_z});                 // open to the sky
            edits.push_back({probe_x, y - 6, probe_z});             // buried
            edits.push_back({probe_x + 1, (y / chunk_section_height) * chunk_section_height,
                             probe_z + 1});                         // on a section boundary
            break;
        }
    check(edits.size() == 3, "the test world had no terrain column to edit");

    const auto selection = region.edited_sections(edits);
    const auto geometry = WorldRegion::edited_geometry_sections(edits);
    check(!selection.empty() && !geometry.empty(), "an edit selected nothing to rebuild");
    // The two tiers the renderer queues must together be the whole invalidation
    // set, or the fast tier would show a hole the slow tier never finishes.
    for (const ChunkCoordinate &section : geometry)
        check(std::find_if(selection.begin(), selection.end(), [&](const ChunkCoordinate &c) {
                  return c.x == section.x && c.z == section.z && c.y == section.y;
              }) != selection.end(),
              "a section holding an edit was missing from the light-influence set");

    std::vector<BlockType> previous_blocks;
    for (const auto &edit : edits)
    {
        previous_blocks.push_back(region.block(edit[0], edit[1], edit[2]));
        region.set_block(edit[0], edit[1], edit[2], BlockType::empty);
    }

    const auto incremental = bake(region, selection);
    const auto full = bake(region, selection);
    const auto reference = region.visible_faces_by_chunk();
    check(incremental.size() == full.size(), "an incremental bake was not deterministic");
    std::size_t compared{};
    for (const auto &batch : incremental)
    {
        const auto whole = std::find_if(reference.begin(), reference.end(), [&](const auto &r) {
            return r.coordinate.x == batch.coordinate.x && r.coordinate.z == batch.coordinate.z &&
                   r.coordinate.y == batch.coordinate.y;
        });
        check(whole != reference.end(), "an edited section is not part of the region");
        check(identical(batch, *whole),
              "a section rebuilt from the edit selection differs from a full rebuild");
        ++compared;
    }
    check(compared == incremental.size() && compared > 0, "no edited section was compared");

    // The measured apron is what the renderer actually queues. It is computed
    // from the edited region (as the renderer does) and must still name every
    // section the edit changed, while naming far fewer than the geometric rule.
    const auto measured = region.light_changed_sections(edits);
    const auto measured_exact = region.light_changed_sections(edits, &previous_blocks);
    check(!measured.empty() && !measured_exact.empty(),
          "the light-change probe selected nothing to rebuild");
    check(measured_exact.size() <= measured.size(),
          "knowing the previous block made the measured light set larger");
    for (const ChunkCoordinate &section : geometry)
        check(std::find_if(measured.begin(), measured.end(), [&](const ChunkCoordinate &c) {
                  return c.x == section.x && c.z == section.z && c.y == section.y;
              }) != measured.end(),
              "a section holding an edit was missing from the measured light set");

    // Every section either selection leaves out must be untouched by the edit,
    // which is what makes skipping it safe.
    const auto before = generator.generate_region(0, 0, 6, 6).visible_faces_by_chunk();
    const auto after = region.visible_faces_by_chunk();
    check(before.size() == after.size(), "a block edit changed the section list");
    std::size_t changed_sections{};
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        if (identical(before[i], after[i]))
            continue;
        ++changed_sections;
        const ChunkCoordinate &changed = after[i].coordinate;
        const auto names = [&](const std::vector<ChunkCoordinate> &set) {
            return std::find_if(set.begin(), set.end(), [&](const ChunkCoordinate &c) {
                       return c.x == changed.x && c.z == changed.z && c.y == changed.y;
                   }) != set.end();
        };
        check(names(selection), "a section changed by the edit was not scheduled for a rebuild");
        check(names(measured), "a section changed by the edit was missing from the measured set");
        check(names(measured_exact),
              "a section changed by the edit was missing from the exact measured set");
    }
    check(changed_sections > 0, "the test edit changed no section at all");
    check(measured.size() <= selection.size(),
          "the measured light set was larger than the geometric apron");
    // The whole point of measuring: the geometric rule queues an apron many
    // times larger than the sections an edit really changes.
    check(measured.size() * 2U < selection.size(),
          "the measured light set did not shrink the geometric apron");
    for (const auto &probe : {measured, measured_exact})
    {
        for (const auto &batch : bake(region, probe))
        {
            const auto whole = std::find_if(after.begin(), after.end(), [&](const auto &r) {
                return r.coordinate.x == batch.coordinate.x &&
                       r.coordinate.z == batch.coordinate.z && r.coordinate.y == batch.coordinate.y;
            });
            check(whole != after.end(), "a measured section is not part of the region");
            check(identical(batch, *whole),
                  "a section rebuilt from the measured selection differs from a full rebuild");
        }
    }
}

// A windowless model of one held gun, stepped exactly like the game's fixed
// tick: timers, shoulder, reload clip, then one input sample. It uses only the
// pure rules the game uses (gun_trigger_step, gun_reload_step, gun_reload_cycle),
// so what it proves about restarts and interrupts holds for the real weapon.
struct GunModel final
{
    static constexpr float tick = 1.0F / 60.0F;
    const GunStats *stats{};
    std::uint16_t rounds{};
    std::uint32_t reserve{};
    GunTriggerState trigger{};
    GunReloadClip clip{};
    float cooldown{};
    float fire_animation{};
    float fire_animation_seconds{0.15F};
    float aim{};
    bool shoulder_held{};
    bool trigger_held{};
    bool lock{true};
    int shots{};
    int clips_started{};
    int clips_cancelled{};
    int clips_completed{};

    GunModel(InventoryItemId item, std::uint16_t loaded, std::uint32_t spare)
        : stats(gun_stats(item)), rounds(loaded), reserve(spare)
    {
        check(stats != nullptr, "gun model built from a non-gun item");
    }

    void sample(bool use_pressed, bool reload_pressed)
    {
        GunTriggerInput input;
        input.use_pressed = use_pressed;
        input.use_held = trigger_held;
        input.reload_pressed = reload_pressed;
        input.cooldown_expired = cooldown <= 0.0F;
        input.fire_permitted = lock;
        const auto decision = gun_trigger_step(*stats, trigger, rounds, reserve, input);
        trigger = decision.state;
        if (decision.fire)
        {
            --rounds;
            ++shots;
            cooldown = stats->fire_cooldown_seconds;
            fire_animation = fire_animation_seconds;
        }
    }
    void press() { trigger_held = true; sample(true, false); }
    void release() { trigger_held = false; }
    void reload_key() { sample(false, true); }

    void step()
    {
        cooldown = std::max(0.0F, cooldown - tick);
        fire_animation = std::max(0.0F, fire_animation - tick);
        const bool firing = fire_animation > 0.0F;
        if (trigger.reloading && aim > 0.0F)
        {
            if (!firing) aim = std::max(0.0F, aim - tick / 0.15F);
        }
        else
            aim = std::clamp(aim + (shoulder_held && !trigger.reloading ? tick : -tick) / 0.15F, 0.0F, 1.0F);
        auto reload = gun_reload_step(clip, trigger.reloading, aim <= 0.0F, firing, stats->reload_seconds, tick);
        if (reload.event == GunReloadEvent::completed)
        {
            ++clips_completed;
            const auto cycle = gun_reload_cycle(*stats, rounds, reserve);
            rounds = static_cast<std::uint16_t>(rounds + cycle.loaded);
            reserve -= cycle.loaded;
            trigger.reloading = cycle.continue_reloading;
            reload = gun_reload_step({}, trigger.reloading, aim <= 0.0F, firing, stats->reload_seconds, 0.0F);
        }
        clip = reload.clip;
        if (reload.event == GunReloadEvent::started) ++clips_started;
        if (reload.event == GunReloadEvent::cancelled) ++clips_cancelled;
        if (trigger_held && stats->automatic) sample(false, false);
    }
    // Ticks until the reload request is gone, giving up after twenty seconds.
    int run_until_idle(int click_every = 0)
    {
        int ticks{};
        for (; (trigger.reloading || clip.active) && ticks < 1200; ++ticks)
        {
            if (click_every > 0 && ticks % click_every == 0) { release(); press(); }
            step();
        }
        return ticks;
    }
};

void trigger_decisions_match_the_original()
{
    const GunStats &pistol = *gun_stats(InventoryItemId::pistol);          // semi, 8 rounds
    const GunStats &assault = *gun_stats(InventoryItemId::assault_rifle);  // automatic, 30 rounds
    const GunStats &guided = *gun_stats(InventoryItemId::guided_rocket_launcher);
    const auto step = [](const GunStats &gun, GunTriggerState state, std::uint16_t rounds,
                         std::uint32_t reserve, GunTriggerInput input) {
        return gun_trigger_step(gun, state, rounds, reserve, input);
    };
    const GunTriggerInput pressed{true, true, false, true, true};
    const GunTriggerInput held{false, true, false, true, true};
    const GunTriggerInput reload{false, false, true, true, true};
    const GunTriggerInput reload_while_held{false, true, true, true, true};
    GunTriggerInput pressed_in_cooldown = pressed; pressed_in_cooldown.cooldown_expired = false;

    // Trigger modes and the cooldown gate.
    check(step(pistol, {}, 8, 0, pressed).fire && !step(pistol, {}, 8, 0, held).fire,
          "semi-automatic trigger did not require a fresh pull");
    check(step(assault, {}, 30, 0, held).fire && step(assault, {}, 30, 0, pressed).fire,
          "automatic trigger did not fire while held");
    check(!step(pistol, {}, 8, 0, pressed_in_cooldown).fire, "a shot ignored the fire cooldown");

    // Empty guns: reserve asks for a reload, no reserve does nothing at all.
    auto empty = step(pistol, {}, 0, 16, pressed);
    check(!empty.fire && empty.state.reloading, "empty trigger pull did not request the reload");
    auto dry = step(pistol, {}, 0, 0, pressed);
    check(!dry.fire && !dry.state.reloading && !dry.state.released, "dry trigger pull did something");
    check(!step(assault, {}, 0, 0, held).state.reloading, "held dry automatic requested a reload");

    // The last round out of a clip chains straight into the automatic reload.
    check(step(pistol, {}, 1, 16, pressed).fire && step(pistol, {}, 1, 16, pressed).state.reloading &&
              !step(pistol, {}, 1, 0, pressed).state.reloading && !step(pistol, {}, 2, 16, pressed).state.reloading,
          "automatic reload after the last round is wrong");

    // Reload key: needs space in the clip and reserve; records whether the trigger was up.
    check(!step(pistol, {}, 8, 16, reload).state.reloading && !step(pistol, {}, 3, 0, reload).state.reloading,
          "reload accepted with a full clip or no reserve");
    check(step(pistol, {}, 3, 16, reload).state.reloading && step(pistol, {}, 3, 16, reload).state.released &&
              step(pistol, {}, 3, 16, reload_while_held).state.reloading &&
              !step(pistol, {true, true}, 3, 16, reload_while_held).state.released,
          "reload key did not latch the trigger-released state");
    check(step(pistol, {true, false}, 3, 16, reload).state.reloading, "reload pressed during a reload cancelled it");

    // A trigger pull during a reload.
    auto locked = step(pistol, {true, false}, 3, 16, pressed);
    check(!locked.fire && locked.state.reloading, "a never-hand-reloaded gun let a pull interrupt its reload");
    auto interrupt = step(pistol, {true, true}, 3, 16, pressed);
    check(interrupt.fire && !interrupt.state.reloading, "a pull could not interrupt a reload for a loaded round");
    auto still_empty = step(pistol, {true, true}, 0, 16, pressed);
    check(!still_empty.fire && still_empty.state.reloading,
          "a pull on an empty gun dropped the reload it was waiting for");
    // Original quirk, kept: inside the fire cooldown the withdrawn request is not restored.
    check(!step(pistol, {true, true}, 0, 16, pressed_in_cooldown).state.reloading,
          "cooldown-time pull no longer matches GunInventoryItem.ProcessInput");
    check(!step(assault, {true, true}, 0, 16, held).fire && step(assault, {true, true}, 0, 16, held).state.reloading,
          "holding an automatic trigger disturbed its reload");

    // The guided launcher needs its lock; without one the pull does nothing.
    GunTriggerInput no_lock = pressed; no_lock.fire_permitted = false;
    check(!step(guided, {}, 1, 4, no_lock).fire && !step(guided, {}, 1, 4, no_lock).state.reloading &&
              step(guided, {}, 1, 4, pressed).fire, "guided launcher lock gate is wrong");
}

void a_reload_can_never_be_restarted_by_clicking()
{
    // The reported bug: empty magazine, reserve ammunition, click during the reload.
    for (const InventoryItemId item : {InventoryItemId::pistol, InventoryItemId::assault_rifle,
                                       InventoryItemId::bolt_action_rifle, InventoryItemId::smg,
                                       InventoryItemId::lmg, InventoryItemId::iron_space_assault_rifle,
                                       InventoryItemId::copper_space_pistol, InventoryItemId::gold_space_bolt_action_rifle,
                                       InventoryItemId::diamond_space_smg, InventoryItemId::modern_laser_drill})
    {
        for (const bool hand_reloaded_before : {false, true})
        {
            GunModel quiet{item, 1, 500};
            quiet.trigger.released = hand_reloaded_before;
            quiet.press(); quiet.release();   // last round; the automatic reload is requested
            check(quiet.shots == 1 && quiet.trigger.reloading, "last round did not start the reload");
            const int undisturbed = quiet.run_until_idle();
            check(quiet.rounds == quiet.stats->clip_capacity && quiet.clips_started == 1, "undisturbed reload failed");

            for (const int click_every : {1, 2, 3, 7, 20})
            {
                GunModel spammed{item, 1, 500};
                spammed.trigger.released = hand_reloaded_before;
                spammed.press(); spammed.release();
                // Let the shot's own fire cooldown pass first; the original's
                // inside-the-cooldown quirk is covered separately below.
                int settle{};
                while (spammed.cooldown > 0.0F && settle < 200) { spammed.step(); ++settle; }
                const int ticks = settle + spammed.run_until_idle(click_every);
                check(spammed.clips_started == 1 && spammed.clips_cancelled == 0 && spammed.clips_completed == 1,
                      "clicking during the reload restarted or cancelled it");
                check(ticks == undisturbed, "clicking during the reload changed how long it takes");
                // (An automatic may already have fired the first round of the fresh clip.)
                check(spammed.rounds + (spammed.shots - 1) == spammed.stats->clip_capacity && spammed.shots <= 2,
                      "clicked reload did not fill the clip exactly once");
            }
        }
    }

    // The one interruption GunInventoryItem.ProcessInput really has: the pull
    // clears Reloading before the cooldown gate, so a pull taken *inside* the
    // fire cooldown is never restored by the block below it. The request comes
    // back on the first pull after the cooldown ends. It is deliberately kept,
    // and the point is that it is bounded: however long the player keeps
    // clicking, the clip starts at most once more and always finishes.
    for (const InventoryItemId item : {InventoryItemId::gold_space_bolt_action_rifle,
                                       InventoryItemId::pistol})
    {
        GunModel quirk{item, 1, 40};
        quirk.trigger.released = true;      // hand-reloaded before, so pulls may interrupt
        quirk.press(); quirk.release();
        check(quirk.trigger.reloading, "last round did not request the reload");
        quirk.press();                      // pulled inside the fire cooldown
        check(!quirk.trigger.reloading, "the documented cooldown-window withdrawal is gone");
        int ticks{};
        while (quirk.clips_completed == 0 && ticks < 900)
        {
            quirk.release(); quirk.press();     // and keep clicking, every single tick
            quirk.step();
            ++ticks;
        }
        check(ticks < 900 && quirk.clips_started == 1 && quirk.clips_completed == 1,
              "clicking inside the fire cooldown left the reload stuck or looping");
        check(quirk.rounds == quirk.stats->clip_capacity,
              "the magazine was not filled exactly once after the cooldown-window withdrawal");
    }

    // Pressing Reload over and over is just as harmless.
    GunModel keys{InventoryItemId::assault_rifle, 4, 90};
    keys.reload_key();
    int reload_ticks{};
    for (; (keys.trigger.reloading || keys.clip.active) && reload_ticks < 600; ++reload_ticks)
    {
        keys.reload_key();
        keys.step();
    }
    check(keys.clips_started == 1 && keys.clips_cancelled == 0 && keys.rounds == 30 && keys.reserve == 64 &&
              std::abs(reload_ticks * GunModel::tick - keys.stats->reload_seconds) < 0.05F,
          "repeated reload presses restarted or stretched the reload");
    keys.reload_key();
    check(!keys.trigger.reloading, "reload accepted on a full clip");
}

void reload_interrupts_shells_and_held_triggers()
{
    // A hand-started magazine reload can be abandoned for a round that is still loaded.
    GunModel pistol{InventoryItemId::pistol, 3, 16};
    pistol.reload_key();
    for (int i = 0; i < 20; ++i) pistol.step();
    check(pistol.clip.active && pistol.clips_started == 1, "manual reload clip did not start");
    pistol.press(); pistol.release(); pistol.step();
    check(pistol.shots == 1 && pistol.rounds == 2 && pistol.reserve == 16 && !pistol.clip.active &&
              pistol.clips_cancelled == 1 && !pistol.trigger.reloading,
          "interrupting a magazine reload fired nothing, or kept part of the reload");

    // The reload clip waits for the fire clip, and for the weapon to be lowered.
    GunModel rifle{InventoryItemId::bolt_action_rifle, 1, 8};
    rifle.fire_animation_seconds = 0.5F;
    rifle.shoulder_held = true;
    for (int i = 0; i < 20; ++i) rifle.step();
    check(rifle.aim >= 0.999F, "model did not shoulder");
    rifle.press(); rifle.release();
    for (int i = 0; i < 29; ++i) rifle.step();
    check(rifle.trigger.reloading && !rifle.clip.active && rifle.aim >= 0.999F,
          "reload began (or the weapon dropped) before the fire clip finished");
    for (int i = 0; i < 6; ++i) rifle.step();
    check(!rifle.clip.active && rifle.aim > 0.0F && rifle.aim < 1.0F, "reload began before the weapon was lowered");
    for (int i = 0; i < 12; ++i) rifle.step();
    check(rifle.clip.active && rifle.clips_started == 1, "reload never began after unshouldering");
    (void)rifle.run_until_idle();
    for (int i = 0; i < 20; ++i) rifle.step();
    check(rifle.rounds == 8 && rifle.aim >= 0.999F, "held shoulder did not come back up after the reload");

    // Shotguns: one shell per clip, looping while there is tube space and reserve.
    GunModel few{InventoryItemId::pump_shotgun, 0, 3};
    few.press(); few.release();
    (void)few.run_until_idle();
    check(few.rounds == 3 && few.reserve == 0 && few.clips_started == 3 && few.clips_completed == 3 && few.shots == 0,
          "three reserve shells did not load as three one-shell cycles");
    GunModel one{InventoryItemId::iron_space_pump_shotgun, 0, 1};
    one.press(); one.release();
    (void)one.run_until_idle();
    check(one.rounds == 1 && one.clips_started == 1, "a single reserve shell looped the reload");
    GunModel full{InventoryItemId::pump_shotgun, 4, 50};
    full.reload_key();
    (void)full.run_until_idle();
    check(full.rounds == 6 && full.reserve == 48 && full.clips_started == 2, "shell loop overfilled or underfilled the tube");

    // Shell loop interrupt: only once the gun has been reloaded by hand (gunReleased).
    GunModel fresh{InventoryItemId::pump_shotgun, 0, 50};
    fresh.press(); fresh.release();
    while (fresh.rounds < 2) fresh.step();
    fresh.press(); fresh.release();
    check(fresh.shots == 0 && fresh.trigger.reloading, "an untouched shotgun let a pull interrupt its first reload");
    (void)fresh.run_until_idle();
    check(fresh.rounds == 6 && fresh.clips_cancelled == 0, "ignored pull still disturbed the shell loop");

    GunModel pump{InventoryItemId::pump_shotgun, 2, 50};
    pump.fire_animation_seconds = 1.42F;
    pump.reload_key();   // by hand, trigger up: later pulls may interrupt
    while (pump.rounds < 4) pump.step();
    const int started_before = pump.clips_started;
    pump.press(); pump.release(); pump.step();
    check(pump.shots == 1 && pump.rounds == 3 && pump.clips_cancelled == 1 && !pump.clip.active && !pump.trigger.reloading,
          "a pull between shells did not fire the loaded shell and stop the loop");
    for (int i = 0; i < 200; ++i) pump.step();
    check(pump.clips_started == started_before && pump.rounds == 3, "an interrupted shell loop resumed by itself");

    // Holding an automatic trigger through the reload resumes fire; a semi-automatic waits for a new pull.
    GunModel automatic{InventoryItemId::assault_rifle, 1, 90};
    automatic.press();
    (void)automatic.run_until_idle();
    for (int i = 0; i < 30; ++i) automatic.step();
    check(automatic.shots > 1 && automatic.clips_started == 1, "held automatic trigger did not resume after its reload");
    GunModel semi{InventoryItemId::pistol, 1, 16};
    semi.press();
    (void)semi.run_until_idle();
    for (int i = 0; i < 30; ++i) semi.step();
    check(semi.shots == 1 && semi.rounds == 8, "held semi-automatic trigger fired without a new pull");

    // The clip rules on their own.
    check(gun_reload_step({}, true, false, false, 2.0F, 0.1F).event == GunReloadEvent::none &&
              gun_reload_step({}, true, true, true, 2.0F, 0.1F).event == GunReloadEvent::none &&
              gun_reload_step({}, false, true, false, 2.0F, 0.1F).event == GunReloadEvent::none,
          "reload clip started without a request, while shouldered, or during the fire clip");
    const auto begun = gun_reload_step({}, true, true, false, 2.0F, 0.1F);
    check(begun.event == GunReloadEvent::started && begun.clip.active && begun.clip.seconds_left == 2.0F,
          "reload clip did not start at its full duration");
    const auto running = gun_reload_step(begun.clip, true, false, true, 9.0F, 0.5F);
    check(running.event == GunReloadEvent::none && running.clip.active && std::abs(running.clip.seconds_left - 1.5F) < 0.0001F,
          "a running reload clip was reset or paused by shoulder/fire state");
    check(gun_reload_step(running.clip, false, true, false, 2.0F, 0.1F).event == GunReloadEvent::cancelled &&
              gun_reload_step({true, 0.05F}, true, true, false, 2.0F, 0.1F).event == GunReloadEvent::completed,
          "reload clip cancel/complete events are wrong");
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--write-runtime-fixture")
        {
            const std::filesystem::path directory{argv[2]};
            std::filesystem::create_directories(directory);
            for (auto mode : {GameMode::survival, GameMode::creative, GameMode::exploration, GameMode::scavenger})
            {
                WorldSaveData save;
                save.seed = 23;
                save.mode = mode;
                save.infinite_resources = mode == GameMode::creative || mode == GameMode::exploration;
                save.inventory.set_profile(InventoryProfile::modern);
                check(save.inventory.register_spawn_point({0, 6, 0}) &&
                      save.inventory.register_teleport_station({3, 6, 0}, "Home"), "fixture travel registration failed");
                save.edits = {{0, 6, 0, int(world::BlockType::spawn_point_basic)},
                              {3, 6, 0, int(world::BlockType::teleport_station)}};
                check(write_world_save(directory / (std::to_string(int(mode)) + ".omw"), save), "fixture save failed");
            }
            WorldSaveData boss;
            boss.seed = 23;
            boss.mode = GameMode::survival;
            boss.inventory.set_profile(InventoryProfile::modern);
            boss.has_player = true;
            boss.player_position = {5000.5F, -44.999F, 0.5F};
            boss.player_yaw = 0;
            boss.dragon_distance_index = 5; // old checkpoint skipped the boss
            boss.max_distance = 5000;
            for (int z = -2; z <= 2; ++z)
                for (int x = 4998; x <= 5002; ++x)
                {
                    boss.edits.push_back({x, -46, z, int(world::BlockType::rock)});
                    for (int y = -45; y <= -42; ++y)
                        boss.edits.push_back({x, y, z, int(world::BlockType::empty)});
                }
            check(write_world_save(directory / "hell-boss.omw", boss), "boss fixture save failed");
            PlayerProgress progress;
            progress.observe(GameMode::endurance, 1, 275, -25, 0, 1.39F, 1.41F, InventoryItemId::pistol);
            progress.record_craft(GameMode::endurance, InventoryItemId::wood_block, 43);
            progress.record_enemy_kill(GameMode::endurance, 0, InventoryItemId::pistol);
            (void)progress.unlock_earned(1788739200);
            check(save_player_progress(directory / "player.omprogress", progress), "fixture progress failed");
            std::cout << "Wrote isolated Modern gameplay fixtures\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--check-runtime-fixture")
        {
            const std::filesystem::path directory{argv[2]};
            for (auto mode : {GameMode::survival, GameMode::creative, GameMode::exploration, GameMode::scavenger})
            {
                const auto save = read_world_save(directory / (std::to_string(int(mode)) + ".omw"));
                check(save && save->version == 9 && save->mode == mode &&
                      save->inventory.profile() == InventoryProfile::modern &&
                      save->infinite_resources == (mode == GameMode::creative || mode == GameMode::exploration) &&
                      save->inventory.spawn_point() && save->inventory.teleport_stations().size() == 1 &&
                      save->inventory.teleport_stations()[0].waypoint_name == "Home" &&
                      save->encounters.carved_cells > 0 && !save->encounters.populated_chunks.empty(),
                      "runtime lost world rules/travel or did not populate encounters");
                std::cout << "Mode " << int(mode) << ": " << save->encounters.populated_chunks.size()
                          << " populated chunks, " << save->encounters.carved_cells << " carved cells\n";
            }
            return 0;
        }
        check(argc == 1, "unknown gameplay test argument");
        crafting_and_modes(); navigation_and_saves(); achievements(); encounters_and_harvest();
        playtest_encounters_and_colors();
        std::cout << "Gameplay progression, travel, encounters and all 31 achievements passed\n";
        trigger_decisions_match_the_original(); a_reload_can_never_be_restarted_by_clicking();
        reload_interrupts_shells_and_held_triggers();
        std::cout << "Weapon trigger, reload-clip and shell-loop rules passed\n";
        edit_rebuilds_match_a_full_rebuild();
        std::cout << "Block-edit rebuild selection matches a full rebuild\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
