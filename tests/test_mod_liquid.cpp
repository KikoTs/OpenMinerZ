// mod-liquid: liquid mod blocks (blocks.define{..., liquid = true};
// openminerz/world/liquid.hpp).
//
// The world side, on a pool of generated liquid in a small region: which
// cells are liquid (generated, placed over, edited away), the mesher (faces
// only toward what is neither liquid nor opaque, the surface flagged, the
// retail mesh untouched without a table), collision and swimming (a player
// sinks slowly, floats at the surface, climbs out at a bank, takes no fall
// damage landing in it), the crosshair and building into a liquid.
// The service side, through the runtime against a fake game: the registry
// flag on both halves and its fallback rule, the server's own liquid cells,
// players.get's in_liquid/underwater, item.used's target.liquid, walkers that
// path around a lake and float when thrown in, swimmers that find water
// without ai.set_water, surface spawns that never land on a lake.
// The host's authority: a swimmer is supported, a dive is no fall, building
// into a liquid is a placement and digging one is refused.

#include "modding_test_harness.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/net/session_authority.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/liquid.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using modding::Handle;
using modding::Side;
using modding::Value;
namespace gameplay = openminerz::modding::gameplay;

void expect(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error{std::string{message}};
}

// ------------------------------------------------------------- the world

// Three by three chunks around the origin: rock up to y 0 (its top at 1.0),
// and a pool of generated liquid - ice holding palette entry 1 - over x and
// z 4..11, y -3..0. A real ice block stands beside it at (12, 0, 6).
constexpr int pool_min = 4;
constexpr int pool_max = 11;
constexpr int pool_floor = -3;

void set_mod_cell(world::WorldRegion &region, int x, int y, int z, std::uint8_t palette)
{
    const int chunk_x = x >= 0 ? x / world::chunk_width : (x - world::chunk_width + 1) / world::chunk_width;
    const int chunk_z = z >= 0 ? z / world::chunk_depth : (z - world::chunk_depth + 1) / world::chunk_depth;
    world::Chunk &chunk = region.chunks()[static_cast<std::size_t>(
        (chunk_x - region.origin_chunk_x()) + (chunk_z - region.origin_chunk_z()) * region.chunks_x())];
    chunk.set_mod_cell(x - chunk_x * world::chunk_width, y - world::world_min_y,
                       z - chunk_z * world::chunk_depth, palette);
}

world::WorldRegion pool_world()
{
    world::WorldRegion region{-1, -1, 3, 3};
    for (int z = region.minimum_z(); z <= region.maximum_z(); ++z)
        for (int x = region.minimum_x(); x <= region.maximum_x(); ++x)
            for (int y = world::world_min_y; y <= 0; ++y)
                region.set_block(x, y, z, world::BlockType::rock);
    for (int z = pool_min; z <= pool_max; ++z)
        for (int x = pool_min; x <= pool_max; ++x)
            for (int y = pool_floor; y <= 0; ++y)
            {
                region.set_block(x, y, z, world::BlockType::ice);
                set_mod_cell(region, x, y, z, 1U);
            }
    region.set_block(12, 0, 6, world::BlockType::ice);
    return region;
}

std::shared_ptr<world::LiquidTable> water_table()
{
    auto table = std::make_shared<world::LiquidTable>();
    expect(table->add_kind({"test:water", world::BlockType::ice, {0.2F, 0.5F, 0.8F}}) == 1U, "one kind");
    table->set_palette(1U, 1U, world::BlockType::ice);
    return table;
}

// Installs a table for one scope (the process-wide state every query reads).
struct Liquids final
{
    explicit Liquids(std::shared_ptr<const world::LiquidTable> table)
    {
        world::set_liquids(std::move(table));
    }
    ~Liquids()
    {
        world::set_liquids(nullptr);
    }
    Liquids(const Liquids &) = delete;
    Liquids &operator=(const Liquids &) = delete;
};

std::vector<world::ChunkCoordinate> every_section(const world::WorldRegion &region)
{
    std::vector<world::ChunkCoordinate> selection;
    for (const world::Chunk &chunk : region.chunks())
        for (int section = 0; section < world::chunk_section_count; ++section)
            selection.push_back({chunk.chunk_x(), chunk.chunk_z(), section});
    return selection;
}

std::uint64_t digest(const std::vector<world::ChunkFaceBatch> &batches)
{
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t index = 0U; index < size; ++index)
            hash = (hash ^ bytes[index]) * 0x100000001B3ULL;
    };
    for (const world::ChunkFaceBatch &batch : batches)
        for (const world::WorldFace &face : batch.faces)
        {
            const std::array<int, 6> header{face.x, face.y, face.z, static_cast<int>(face.direction),
                                            static_cast<int>(face.block), face.liquid};
            mix(header.data(), sizeof(header));
            mix(face.sky_light.data(), 4U);
            mix(face.ambient_occlusion.data(), 4U);
        }
    return hash;
}

// ------------------------------------------------------- the world side

void cells_are_liquid_while_they_hold_their_fallback()
{
    world::WorldRegion region = pool_world();
    expect(!world::liquid_at(region, 6, 0, 6), "nothing is liquid without a table");
    const auto table = water_table();
    expect(table->kind_at(region, 6, 0, 6) == 1U && table->kind_at(region, 6, pool_floor, 6) == 1U,
           "the generated pool is liquid");
    expect(table->kind_at(region, 6, pool_floor - 1, 6) == 0U, "the rock under it is not");
    expect(table->kind_at(region, 12, 0, 6) == 0U, "real ice beside it is not");
    {
        Liquids installed{table};
        expect(world::liquid_at(region, 6, 0, 6) && !world::cell_blocks_player(region, 6, 0, 6),
               "an installed table makes the pool open");
        expect(world::cell_blocks_player(region, 12, 0, 6), "the real ice still stops a player");
        const std::optional<float> surface = world::liquid_surface(region, 6.5F, -2.5F, 6.5F);
        expect(surface.has_value() && std::abs(*surface - (1.0F - world::liquid_surface_drop)) < 1e-5F,
               "the surface is the top of the column, lowered");
        expect(std::abs(world::liquid_depth(region, 6.5F, 0.0F, 6.5F) - 0.875F) < 1e-5F,
               "depth is measured from it");
        expect(world::liquid_depth(region, 6.5F, 0.95F, 6.5F) == 0.0F, "above it is dry");
    }
    expect(world::active_liquids() == nullptr, "the scope took it out again");
    world::set_liquids(std::make_shared<world::LiquidTable>());
    expect(world::active_liquids() == nullptr, "an empty table is no table");

    // A non-liquid mod block placed over the pool hides it; a liquid placed
    // over real ice makes that a liquid.
    auto placed = water_table();
    placed->set_placed(6, 0, 6, 0U, world::BlockType::ice);
    placed->set_placed(12, 0, 6, 1U, world::BlockType::ice);
    expect(placed->kind_at(region, 6, 0, 6) == 0U && placed->kind_at(region, 12, 0, 6) == 1U,
           "placed blocks decide their cells");
    expect(!placed->same_as(*table) && world::LiquidTable::changed_placed(table.get(), placed.get()).size() == 2U,
           "two placed cells differ");
    expect(!world::LiquidTable::generated_differ(table.get(), placed.get()), "the generated ones do not");
    // An edit ends the identity, and putting the fallback back does not
    // bring it back.
    region.set_block(7, 0, 7, world::BlockType::rock);
    expect(table->kind_at(region, 7, 0, 7) == 0U, "a cell built over is no liquid");
    region.set_block(7, 0, 7, world::BlockType::ice);
    expect(table->kind_at(region, 7, 0, 7) == 0U, "nor is plain ice put back there");
}

void the_mesher_draws_liquids_toward_what_is_open()
{
    const world::WorldRegion region = pool_world();
    const auto table = water_table();
    const auto selection = every_section(region);
    const auto liquid_mesh = region.visible_faces_by_chunk(selection, table.get());
    const auto retail_mesh = region.visible_faces_by_chunk(selection);
    std::size_t surface_faces{};
    std::size_t other_liquid_faces{};
    bool toward_the_ice{};
    bool ice_faces_the_pool{};
    bool floor_shows{};
    for (const world::ChunkFaceBatch &batch : liquid_mesh)
        for (const world::WorldFace &face : batch.faces)
        {
            if (face.liquid == 2U && face.direction == world::FaceDirection::positive_y && face.y == 0)
                ++surface_faces;
            else if (face.liquid == 2U && face.x == 11 && face.y == 0 && face.z == 6 &&
                     face.direction == world::FaceDirection::positive_x)
                toward_the_ice = true; // ice lets light through: the liquid shows there
            else if (face.liquid != 0U)
                ++other_liquid_faces;
            if (face.x == 12 && face.y == 0 && face.z == 6 && face.direction == world::FaceDirection::negative_x)
                ice_faces_the_pool = face.liquid == 0U;
            if (face.x == 6 && face.y == pool_floor - 1 && face.z == 6 &&
                face.direction == world::FaceDirection::positive_y)
                floor_shows = true;
        }
    const int width = pool_max - pool_min + 1;
    expect(surface_faces == static_cast<std::size_t>(width * width), "the pool shows its whole surface");
    expect(other_liquid_faces == 0U && toward_the_ice, "and nothing toward rock or itself");
    expect(ice_faces_the_pool, "real ice beside it shows its face toward the liquid");
    expect(floor_shows, "the floor under the liquid is drawn");
    bool retail_ice_face{};
    for (const world::ChunkFaceBatch &batch : retail_mesh)
        for (const world::WorldFace &face : batch.faces)
        {
            expect(face.liquid == 0U, "the retail mesher flags nothing");
            if (face.x == 12 && face.y == 0 && face.z == 6 && face.direction == world::FaceDirection::negative_x)
                retail_ice_face = true;
        }
    expect(!retail_ice_face, "retail culls ice against ice, as before");
    const world::LiquidTable empty;
    expect(digest(region.visible_faces_by_chunk(selection, &empty)) == digest(retail_mesh),
           "an empty table meshes exactly the retail way");
}

void players_swim_in_liquids_and_climb_out()
{
    const world::WorldRegion region = pool_world();
    const auto drop = [&](float from_y, float seconds, bool liquids) {
        std::optional<Liquids> installed;
        if (liquids)
            installed.emplace(water_table());
        world::PlayerSimulation player;
        player.state().position = {8.5F, from_y, 8.5F};
        bool hard_landing{};
        for (int tick = 0; tick < static_cast<int>(seconds * 60.0F); ++tick)
        {
            player.update(region, {}, 1.0F / 60.0F, true);
            if (player.landing_speed_change() > 15.0F && !player.touched_liquid())
                hard_landing = true;
        }
        return std::pair{player.state(), hard_landing};
    };
    const auto [dry, dry_hard] = drop(6.0F, 4.0F, false);
    expect(dry.grounded && std::abs(dry.position.y - 1.0F) < 0.01F && !dry_hard,
           "without liquids the pool is ice to stand on");
    const auto [wet, wet_hard] = drop(6.0F, 4.0F, true);
    expect(!wet.grounded && wet.position.y < 0.6F && wet.position.y > -0.6F,
           std::format("a swimmer floats with its feet under the surface (y {:.2f})", wet.position.y));
    const auto [dive, dive_hard] = drop(40.0F, 6.0F, true);
    expect(!dive_hard && dive.position.y > -2.0F, "a dive into the pool from 40 m is no hard landing");
    const auto [crash, crash_hard] = drop(40.0F, 6.0F, false);
    expect(crash_hard, "the same fall onto ice is one");

    // Out at the bank: swim east holding jump, without auto-climb.
    Liquids installed{water_table()};
    world::PlayerSimulation swimmer;
    swimmer.state().position = {10.5F, 0.05F, 8.5F};
    swimmer.state().yaw = std::numbers::pi_v<float> * 0.5F;
    bool climbed{};
    for (int tick = 0; tick < 4 * 60 && !climbed; ++tick)
    {
        swimmer.update(region, {0.0F, 1.0F, true}, 1.0F / 60.0F, false);
        climbed = swimmer.state().position.x > 12.4F && swimmer.state().position.y >= 0.99F;
    }
    expect(climbed, std::format("jump against the bank climbs out (x {:.2f} y {:.2f})", swimmer.state().position.x,
                                swimmer.state().position.y));
}

void the_crosshair_and_building_pass_through_liquids()
{
    world::WorldRegion region = pool_world();
    // A rock under the water, within reach of someone on the bank.
    region.set_block(10, -1, 8, world::BlockType::rock);
    world::PlayerSimulation player;
    player.state().position = {12.5F, 1.001F, 8.5F};
    player.state().yaw = -std::numbers::pi_v<float> * 0.5F; // west, over the pool
    player.state().pitch = -0.9F;
    player.refresh_target(region);
    expect(player.state().target.has_value() && player.state().target->block == world::BlockType::ice,
           "without liquids the pool's top is what the crosshair finds");
    Liquids installed{water_table()};
    player.refresh_target(region);
    const auto target = player.state().target;
    expect(target.has_value() && target->block == world::BlockType::rock,
           "with them it passes through to the rock beyond");
    expect(world::liquid_at(region, target->place_x, target->place_y, target->place_z),
           "and builds into the liquid before it");
    player.state().selected_block = world::BlockType::ice;
    expect(!player.place_target(region), "the liquid's own fallback cannot be placed into it");
    player.state().selected_block = world::BlockType::rock;
    const int x = target->place_x;
    const int y = target->place_y;
    const int z = target->place_z;
    expect(player.place_target(region), "a block replaces the liquid");
    expect(region.block(x, y, z) == world::BlockType::rock && !world::liquid_at(region, x, y, z),
           "the cell is rock now");
    expect(!world::PlayerSimulation::touches_solid(region, {6.5F, -2.0F, 6.5F}),
           "a body in the liquid touches nothing");
    expect(world::PlayerSimulation::raycast_blocking(region, {6.5F, 3.0F, 6.5F}, {0.0F, -1.0F, 0.0F}, 10.0F)
               .has_value(),
           "a shot down through it strikes the floor");
}

// -------------------------------------------------------- the service

// A flat world (rock below y 1) with edits and generated mod blocks, this
// process's player, and what the service asked of it.
class FakeGame final : public gameplay::GameplayBackend
{
  public:
    modding::Vec3 position{0.5, 1.0, 0.5};
    std::map<gameplay::BlockPos, std::uint8_t> edits;
    std::map<gameplay::BlockPos, std::string> generated; // cell -> mod key
    std::function<bool(gameplay::BlockPos)> generated_lake; // a whole generated lake
    std::optional<gameplay::LocalView> view;
    gameplay::GameplayService *service{};

    [[nodiscard]] std::optional<gameplay::LocalView> local_view() const override
    {
        return view;
    }
    [[nodiscard]] std::int32_t world_seed() const override
    {
        return 7;
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return false;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        return std::uint8_t{0U};
    }
    void players(std::vector<gameplay::PlayerSnapshot> &out) const override
    {
        out.clear();
        out.push_back({0U, "Host", true, true, position, 0.0F, 100.0F});
        out.insert(out.end(), remotes.begin(), remotes.end());
    }
    std::vector<gameplay::PlayerSnapshot> remotes;
    void local_set_health(float) override
    {
    }
    void local_damage(float, modding::Vec3, gameplay::DamageCause) override
    {
    }
    void local_teleport(modding::Vec3 feet) override
    {
        position = feet;
    }
    std::uint16_t local_give(gameplay::StackView, bool) override
    {
        return 0U;
    }
    std::uint32_t local_take(std::uint16_t, std::uint32_t, std::uint32_t) override
    {
        return 0U;
    }
    [[nodiscard]] std::uint32_t local_count(std::uint16_t, std::uint32_t) const override
    {
        return 0U;
    }
    void local_clear_inventory() override
    {
    }
    [[nodiscard]] std::optional<gameplay::StackView> local_held() const override
    {
        return std::nullopt;
    }
    void local_slots(std::vector<std::pair<std::uint16_t, gameplay::StackView>> &out) const override
    {
        out.clear();
    }
    bool local_tag_slot(std::uint16_t, std::uint16_t, std::uint32_t) override
    {
        return false;
    }
    void local_set_speed(float) override
    {
    }
    [[nodiscard]] std::optional<std::uint8_t> block(gameplay::BlockPos cell) const override
    {
        if (cell.y < -64 || cell.y > 63)
            return std::nullopt;
        if (const auto found = edits.find(cell); found != edits.end())
            return found->second;
        if (generated.contains(cell) || (generated_lake && generated_lake(cell)))
            return static_cast<std::uint8_t>(world::BlockType::ice);
        return std::uint8_t{cell.y < -8 ? 14U : cell.y < 1 ? 6U : 0U};
    }
    bool set_block(gameplay::BlockPos cell, std::uint8_t value) override
    {
        edits[cell] = value;
        generated.erase(cell);
        return true;
    }
    [[nodiscard]] std::string generated_block(gameplay::BlockPos cell, bool) const override
    {
        if (edits.contains(cell))
            return {};
        if (const auto found = generated.find(cell); found != generated.end())
            return found->second;
        return generated_lake && generated_lake(cell) ? std::string{"test:water"} : std::string{};
    }
    [[nodiscard]] int biome(int, int) const override
    {
        return 0;
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(gameplay::BlockPos) const override
    {
        return std::array<float, 2>{1.0F, 0.0F};
    }
    bool set_crate_slot(gameplay::BlockPos, std::size_t, std::optional<gameplay::StackView>) override
    {
        return false;
    }
    bool spawn_pickup(modding::Vec3, gameplay::StackView) override
    {
        return true;
    }
    [[nodiscard]] double day() const override
    {
        return 0.5;
    }
    void set_time_of_day(float) override
    {
    }
    bool spawn_enemy(int, modding::Vec3) override
    {
        return false;
    }
    void chat(std::optional<std::uint8_t>, std::string_view) override
    {
    }
    [[nodiscard]] std::filesystem::path sidecar_directory() const override
    {
        return {};
    }

    // A pool of generated water: x0..x1, z0..z1, from y0 up to y 0.
    void pool(int x0, int x1, int z0, int z1, int y0)
    {
        for (int y = y0; y <= 0; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    generated[{x, y, z}] = "test:water";
    }
};

constexpr std::string_view definitions = R"(
blocks.define("water", { name = "Water", fallback = "ice", liquid = true })
blocks.define("slab", { fallback = "ice" })
creatures.define("runner", {
    health = 20, speed = 2, run_speed = 4.5, hostile_to = { "players" },
    senses = { sight = 40, fov = 360 }, attacks = { bite = { reach = 1.6, damage = 1, cooldown = 1, windup = 0.4 } },
})
creatures.define("eel", {
    health = 6, speed = 2, run_speed = 3, size = { 0.6, 0.4 }, movement = "swim",
    hostile_to = { "players" }, senses = { sight = 24, fov = 360 }, attacks = { bite = { reach = 1.5, damage = 1 } },
})
creatures.define("cow", { health = 10, speed = 2, behaviour = "passive" })
items.define("bottle", { fallback = "torch", use = function(player, key, target)
    require("shared/state").target = target
end })
)";

modding::ModSource liquid_mod(std::string_view server = {})
{
    modding::ModSource source;
    source.id = "test";
    source.scope = net::ModScope::server;
    source.files["shared/state.luau"] = "return {}";
    source.files["shared/defs.luau"] = std::string{definitions};
    if (!server.empty())
        source.files["server/main.luau"] = std::string{server};
    return source;
}

struct Offline final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    std::unique_ptr<gameplay::GameplayService> gameplay;

    explicit Offline(std::string_view server = {})
    {
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        game.service = gameplay.get();
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
        for (const Side side : {Side::server, Side::client})
        {
            std::vector<modding::ModSource> mods{liquid_mod(server)};
            expect(runtime.start(side, std::move(mods), {}), "the mod's VM starts");
        }
        gameplay->start_server();
        gameplay->start_client();
        for (const modding::ModStatus &status : runtime.status())
            expect(status.state == modding::ModState::running, std::format("{} runs: {}", status.id, status.error));
    }
    ~Offline()
    {
        runtime.stop_all();
    }
    Offline(const Offline &) = delete;
    Offline &operator=(const Offline &) = delete;

    void step(double seconds = 1.0 / 60.0)
    {
        runtime.tick(seconds);
        gameplay->tick(static_cast<float>(seconds));
        runtime.frame(seconds);
    }
    void run_for(double seconds, const std::function<bool()> &each = {})
    {
        for (int index = 0; index < static_cast<int>(std::ceil(seconds * 60.0)); ++index)
        {
            step();
            if (each && each())
                return;
        }
    }
    Value probe(std::string_view expression, Side side = Side::server)
    {
        const modding::RunResult result = runtime.run(side, "test", "probe", std::format("return {}", expression));
        expect(result.ok, std::format("probe '{}': {}", expression, result.error));
        return result.value;
    }
    void run(std::string_view source)
    {
        const modding::RunResult result = runtime.run(Side::server, "test", "chunk", source);
        expect(result.ok, std::format("chunk failed: {}", result.error));
    }
    Handle spawn(std::string_view kind, modding::Vec3 at)
    {
        const Value value = probe(std::format("entities.spawn('{}', vector.create({}, {}, {}))", kind, at.x, at.y, at.z));
        expect(value.type() == modding::ValueType::handle, std::format("{} spawned", kind));
        return value.as_handle();
    }
    [[nodiscard]] std::optional<modding::Vec3> where(Handle entity)
    {
        const Value value = probe(std::format(
            "(function() for _, e in entities.list() do if e.id == {} then return entities.get(e).position end end "
            "return nil end)()",
            entity.id));
        return value.is_nil() ? std::nullopt : std::optional{value.as_vec3()};
    }
    [[nodiscard]] bool field_true(std::string_view expression)
    {
        return probe(expression) == Value{true};
    }
};

[[nodiscard]] bool in_pool(const FakeGame &game, modding::Vec3 at)
{
    const gameplay::BlockPos feet{static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y + 0.05)),
                                  static_cast<int>(std::floor(at.z))};
    return game.generated_block(feet, false) == "test:water";
}

void the_registry_carries_the_flag_and_checks_the_fallback()
{
    Offline world;
    expect(world.field_true("blocks.get('water').liquid") && !world.field_true("blocks.get('slab').liquid"),
           "blocks.get says which block is a liquid");
    world.run_for(0.2);
    const auto &types = world.gameplay->client_block_types();
    const auto water = std::ranges::find(types, std::string{"test:water"}, &gameplay::ClientBlockType::key);
    expect(water != types.end() && water->liquid, "the client registry has it");
    expect(world.gameplay->client_defined_block("test:water")->liquid, "and so does the client VM's own definition");
    const modding::RunResult refused =
        world.runtime.run(Side::server, "test", "bad", "blocks.define('mud', { fallback = 'rock', liquid = true })");
    expect(!refused.ok && refused.error.find("light passes") != std::string::npos,
           "a liquid on an opaque fallback is refused: " + refused.error);
}

void the_server_knows_its_liquids_and_who_is_in_them()
{
    Offline world;
    world.game.pool(4, 11, -4, 4, -3);
    // A liquid placed by a script, and a non-liquid mod block over the pool.
    world.run("world.set_block(vector.create(20, 1, 0), 'water')");
    world.run("world.set_block(vector.create(4, 0, 0), 'slab')");
    // Where the player is, as the server's next tick sees it: {in_liquid, underwater}.
    const auto at = [&](modding::Vec3 feet) {
        world.game.position = feet;
        world.step();
        return std::pair{world.field_true("players.get(players.list()[1]).in_liquid"),
                         world.field_true("players.get(players.list()[1]).underwater")};
    };
    expect(at({6.5, -2.0, 0.5}) == std::pair{true, true}, "a player on the pool's floor is in it, eyes and all");
    expect(at({6.5, 0.5, 0.5}) == std::pair{true, false}, "one swimming at the surface has dry eyes");
    expect(at({0.5, 1.0, 0.5}) == std::pair{false, false}, "and one ashore is dry");
    expect(at({20.5, 1.0, 0.5}).first, "the placed liquid counts too");
    expect(!at({4.5, 0.2, 0.5}).first, "the slab over the pool does not");

    // item.used: the first liquid on the look, even with no block aimed at.
    world.run_for(0.1);
    gameplay::LocalView view;
    view.eye = {2.5, 2.4, 0.5};
    view.look = {0.7071, -0.7071, 0.0};
    view.liquid = gameplay::BlockPos{4, 0, 0}; // the slab: not a liquid
    world.game.view = view;
    const auto &items = world.gameplay->client_item_types();
    const auto found = std::ranges::find(items, std::string{"test:bottle"}, &gameplay::ClientItemType::key);
    expect(found != items.end(), "the bottle is in the registry");
    const std::uint32_t bottle = found->id;
    expect(!world.gameplay->local_item_using({static_cast<std::uint16_t>(InventoryItemId::torch), 1U, bottle}),
           "the bottle's use runs");
    expect(world.probe("require('shared/state').target.liquid") == Value{},
           "a cell that is no liquid is not named");
    view.liquid = gameplay::BlockPos{5, 0, 0};
    world.game.view = view;
    world.run_for(1.0);
    static_cast<void>(world.gameplay->local_item_using({static_cast<std::uint16_t>(InventoryItemId::torch), 1U, bottle}));
    expect(world.probe("require('shared/state').target.liquid.block") == Value{"test:water"},
           "the liquid on the look reaches the use callback");

    // A guest's use: its own word on the liquid when the host agrees, the
    // host's look along its ray when the word is far off, nothing for a cell
    // that holds no liquid.
    world.game.remotes.push_back({5U, "Guest", false, true, {3.5, 1.0, 2.5}, 0.0F, 100.0F});
    const Handle guest = world.gameplay->admit_player(5U, "Guest", false);
    world.gameplay->remote_player_joined(5U, "Guest");
    expect(guest.valid(), "the guest is admitted");
    world.run_for(0.2);
    const auto guest_use = [&](modding::Vec3 liquid, modding::Vec3 look) {
        world.run("require('shared/state').target = nil");
        world.gameplay->receive_from_client(
            5U, modding::Value{modding::Map{{"t", Value{"use"}},
                                            {"key", Value{"test:bottle"}},
                                            {"look", Value{look}},
                                            {"liquid", Value{liquid}}}});
        world.run_for(1.0);
        return world.probe("(require('shared/state').target or {}).liquid");
    };
    const Value named = guest_use({5.0, 0.0, 0.0}, {0.7071, -0.7071, 0.0});
    expect(named.type() == modding::ValueType::map && named.field("block") != nullptr &&
               *named.field("block") == Value{"test:water"},
           "the guest's own liquid cell is believed when it is one");
    const Value faraway = guest_use({90.0, 0.0, 0.0}, {0.7071, -0.7071, 0.0});
    expect(faraway.type() == modding::ValueType::map && faraway.field("position") != nullptr &&
               faraway.field("position")->as_vec3().x < 8.0,
           "a cell out of its reach is replaced by the host's own look");
    expect(guest_use({1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}).is_nil(), "rock is no liquid, whatever the guest says");
}

void walkers_go_around_lakes_and_float_when_thrown_in()
{
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.pool(4, 9, -6, 6, -3);
        world.run("events.on('entity.attacking', function() return false end)");
        const Handle runner = world.spawn("runner", {13.5, 1.0, 0.5});
        bool wet{};
        world.run_for(25.0, [&] {
            const auto at = world.where(runner);
            wet = wet || (at.has_value() && in_pool(world.game, *at));
            return at.has_value() && std::hypot(at->x - 0.5, at->z - 0.5) < 2.5;
        });
        const auto at = world.where(runner);
        expect(!wet, "the walker never set foot in the lake");
        expect(at.has_value() && std::hypot(at->x - 0.5, at->z - 0.5) < 2.5, "it found its way around it");
    }
    {
        Offline world;
        world.game.position = {30.5, 1.0, 0.5};
        world.game.pool(4, 9, -6, 6, -3);
        const Handle cow = world.spawn("cow", {6.5, -2.9, 0.5});
        double highest{-10.0};
        world.run_for(4.0, [&] {
            if (const auto at = world.where(cow))
                highest = std::max(highest, at->y);
            return false;
        });
        expect(highest > -0.5, std::format("a cow on the lake bed floats up (highest {:.2f})", highest));
    }
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.pool(2, 13, -6, 6, -3);
        const Handle eel = world.spawn("eel", {10.5, -2.0, 0.5});
        double highest{-10.0};
        world.run_for(12.0, [&] {
            if (const auto at = world.where(eel))
                highest = std::max(highest, at->y);
            return false;
        });
        const auto at = world.where(eel);
        expect(at.has_value() && at->x < 6.0, "a swimmer found its way through the liquid without ai.set_water");
        expect(highest < 1.0, "and never left it");
    }
}

void nothing_spawns_on_a_lake()
{
    constexpr std::string_view spawner = R"(
        creatures.define("duckling", { health = 2, speed = 1, spawn = { chance = 1, max = 6, surface = true } })
    )";
    {
        Offline world{spawner};
        // Every column around the player is a lake (y -2..0) but its own.
        world.game.generated_lake = [](gameplay::BlockPos cell) {
            return cell.y >= -2 && cell.y <= 0 && (std::abs(cell.x) > 2 || std::abs(cell.z) > 2);
        };
        world.run_for(15.0);
        expect(world.probe("#entities.list()") == Value{std::int64_t{0}}, "no creature lands on the water");
    }
    {
        Offline world{spawner};
        world.run_for(15.0);
        expect(world.probe("#entities.list()").as_integer() > 0, "on dry land they do spawn");
    }
}

// --------------------------------------------------------- the authority

class FakeAuthorityWorld final : public net::AuthorityWorld
{
  public:
    [[nodiscard]] std::optional<world::BlockType> block(int x, int y, int z) const override
    {
        if (y < -64 || y > 127)
            return std::nullopt;
        if (liquid(x, y, z))
            return world::BlockType::ice;
        return y < 1 ? world::BlockType::rock : world::BlockType::empty;
    }
    [[nodiscard]] std::optional<net::AuthorityEnemy> enemy(std::uint8_t, std::uint32_t) const override
    {
        return std::nullopt;
    }
    [[nodiscard]] float blast_line_of_sight(world::Vec3, world::Vec3, world::DragonKind) const override
    {
        return 1.0F;
    }
    [[nodiscard]] bool clear_line(world::Vec3, world::Vec3) const override
    {
        return true;
    }
    // A lake over x 10..40, from y -10 up to y 0.
    [[nodiscard]] bool liquid(int x, int y, int) const override
    {
        return x >= 10 && x <= 40 && y >= -10 && y <= 0;
    }
};

void the_host_lets_players_swim_and_dive()
{
    net::AuthorityRules rules;
    rules.mode = GameMode::survival;
    rules.difficulty = GameDifficulty::hard;
    rules.strict = true;
    rules.spawn_point = {0.0F, 1.0F, 0.0F};
    net::AuthorityHost host{rules};
    FakeAuthorityWorld lake;
    host.set_world(&lake);
    double now{};
    host.add_player(1U, now);
    const auto move = [&](world::Vec3 at) {
        net::PlayerUpdatePayload update;
        update.local_position = {at.x, at.y, at.z};
        static_cast<void>(host.observe_player_update(1U, update, now));
        host.update(now);
        now += 1.0 / 20.0;
    };
    // The player stands on the shore, wades in and treads water at one
    // height for five seconds (with nothing under its feet but water).
    for (int step = 0; step < 20; ++step)
        move({5.5F, 1.0F, 0.5F});
    for (float x = 5.5F; x < 15.0F; x += 0.15F)
        move({x, std::max(0.2F, 1.0F - (x - 9.0F) * 0.2F), 0.5F});
    for (int step = 0; step < 100; ++step)
        move({15.5F, 0.2F, 0.5F});
    // Then, moved up by the host, it falls twenty metres into the lake.
    host.allow_teleport(1U, world::Vec3{25.5F, 21.0F, 0.5F}, now);
    move({25.5F, 21.0F, 0.5F});
    float y = 21.0F;
    float vy = 0.0F;
    while (y > -1.0F)
    {
        vy -= 20.0F / 20.0F;
        y = std::max(-1.0F, y + vy / 20.0F);
        move({25.5F, y, 0.5F});
    }
    for (int step = 0; step < 100; ++step)
        move({25.5F, -1.0F, 0.5F});
    const auto view = host.player(1U);
    expect(view.has_value() && view->violations == 0U && view->corrections == 0U,
           std::format("a swimmer and a diver are no cheats ({} violations, {} corrections)",
                       view.has_value() ? view->violations : 99U, view.has_value() ? view->corrections : 99U));
    expect(view->health >= 99.0F, "and a dive into water hurts nobody");

    // Building into the lake is a placement; digging the lake out is refused.
    move({12.5F, 0.5F, 0.5F});
    expect(host.observe_block_edit(1U, {13, 0, 0}, world::BlockType::ice, world::BlockType::rock, now) ==
               net::AuthorityVerdict::accept,
           "a block built into the liquid is accepted");
    expect(host.observe_block_edit(1U, {14, 0, 0}, world::BlockType::ice, world::BlockType::empty, now) ==
               net::AuthorityVerdict::refuse,
           "a liquid cannot be dug out");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"cells_are_liquid_while_they_hold_their_fallback", cells_are_liquid_while_they_hold_their_fallback},
        {"the_mesher_draws_liquids_toward_what_is_open", the_mesher_draws_liquids_toward_what_is_open},
        {"players_swim_in_liquids_and_climb_out", players_swim_in_liquids_and_climb_out},
        {"the_crosshair_and_building_pass_through_liquids", the_crosshair_and_building_pass_through_liquids},
        {"the_registry_carries_the_flag_and_checks_the_fallback",
         the_registry_carries_the_flag_and_checks_the_fallback},
        {"the_server_knows_its_liquids_and_who_is_in_them", the_server_knows_its_liquids_and_who_is_in_them},
        {"walkers_go_around_lakes_and_float_when_thrown_in", walkers_go_around_lakes_and_float_when_thrown_in},
        {"nothing_spawns_on_a_lake", nothing_spawns_on_a_lake},
        {"the_host_lets_players_swim_and_dive", the_host_lets_players_swim_and_dive},
    };
    int passed{};
    for (const auto &[name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[PASS] " << name << '\n';
            ++passed;
        }
        catch (const std::exception &error)
        {
            std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
        }
        world::set_liquids(nullptr);
    }
    std::cout << passed << " of " << tests.size() << " liquid checks passed\n";
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}
