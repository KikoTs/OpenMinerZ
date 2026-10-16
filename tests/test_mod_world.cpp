// mod-world: custom world generation for server and core mods.
//
// The retail generator is pinned first: golden hashes of chunks, far-terrain
// columns, tree sites and biomes at the distances the game cares about (the
// origin, the ring transitions, the 4,400-block fold, 10k, 25k, 40k and past
// the 46,340-block overflow), recorded from the generator before mod terrain
// existed, so "no mod generator" provably still means byte-identical terrain.
//
// Then the mod generators themselves: the native evaluator (layouts, blends,
// the mirror rule, retail reuse, refused definitions), the chunk's mod layer,
// determinism on any number of threads, far columns against the chunks they
// summarize, the `terrain` / `noise` script libraries, the identity of
// generated mod blocks in gameplay, a host and a joined client (over the real
// admission and mod channel of LoopbackSession, the joiner holding only the
// client-visible files) generating the same chunks, mod blocks and far
// columns from the Frontier example, the world's generator record, big
// distances, and ms per chunk.

#include "modding_test_harness.hpp"

#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/net.hpp"
#include "openminerz/modding/worldgen.hpp"
#include "openminerz/net/host_session.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/world/custom_terrain.hpp"
#include "openminerz/world/far_terrain.hpp"
#include "openminerz/world/world.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/world_save.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
namespace gameplay = openminerz::modding::gameplay;
namespace worldgen = openminerz::modding::worldgen;

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        ++failures;
        std::cout << "  FAIL: " << message << '\n';
    }
}

[[nodiscard]] std::optional<std::string> environment_value(const char *name)
{
#if defined(_MSC_VER)
    char *value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string text{value};
    std::free(value);
    return text;
#else
    const char *value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
#endif
}

[[nodiscard]] bool environment_set(const char *name)
{
#if defined(_MSC_VER)
    char *value{};
    std::size_t length{};
    const bool set = _dupenv_s(&value, &length, name) == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}

struct Fnv final
{
    std::uint64_t value{1469598103934665603ULL};
    void add(std::uint64_t word) noexcept
    {
        for (int shift = 0; shift < 64; shift += 8)
        {
            value ^= (word >> shift) & 0xFFU;
            value *= 1099511628211ULL;
        }
    }
};

// Block positions (x, z) the retail golden covers: the origin, the classic /
// lagoon blend, each band, the descent, the fold at 4,400, the mirrored ring,
// 10k, 25k, 40k, and past the 46,340-block overflow, in several directions.
constexpr std::array<std::array<int, 2>, 16> golden_positions{{
    {0, 0},
    {250, 40},
    {-640, 300},
    {1300, -900},
    {2000, 600},
    {-2700, -400},
    {3300, 100},
    {4400, 0},
    {4650, -1200},
    {-7000, 5200},
    {10000, 0},
    {0, -25000},
    {17700, 17700},
    {40000, 0},
    {-28000, -28000},
    {46500, 3000},
}};

struct RetailGolden final
{
    std::uint64_t chunks{};
    std::uint64_t columns{};
};

RetailGolden retail_golden(const world::WorldGenerator &generator)
{
    Fnv chunks;
    Fnv columns;
    std::vector<world::TreeSite> trees;
    for (const auto &[x, z] : golden_positions)
    {
        const int chunk_x = x >= 0 ? x / 16 : (x - 15) / 16;
        const int chunk_z = z >= 0 ? z / 16 : (z - 15) / 16;
        chunks.add(generator.generate_chunk(chunk_x, chunk_z).hash());
        for (int step = 0; step < 16; ++step)
        {
            const world::SurfaceColumn surface = generator.surface_column(x + step * 3, z - step);
            columns.add(static_cast<std::uint64_t>(surface.top_local_y) |
                        static_cast<std::uint64_t>(surface.top) << 16U |
                        static_cast<std::uint64_t>(static_cast<std::uint16_t>(surface.under_local_y)) << 24U |
                        static_cast<std::uint64_t>(static_cast<std::uint16_t>(surface.lower_local_y)) << 40U |
                        static_cast<std::uint64_t>(surface.lower) << 56U);
            columns.add(static_cast<std::uint64_t>(generator.biome_at(x + step * 3, z - step)));
        }
        std::array<world::BlockType, world::chunk_height> cells{};
        generator.generate_column(x + 1, z + 2, cells);
        for (const world::BlockType block : cells)
            columns.add(static_cast<std::uint64_t>(block));
        trees.clear();
        generator.tree_sites(chunk_x, chunk_z, trees);
        generator.tree_sites(chunk_x + 1, chunk_z, trees, true);
        for (const world::TreeSite &tree : trees)
            columns.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(tree.x)) ^
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(tree.z)) << 32U ^
                        static_cast<std::uint64_t>(tree.ground_local_y) << 7U ^
                        static_cast<std::uint64_t>(tree.trunk_height) << 21U);
    }
    return {chunks.value, columns.value};
}

struct GoldenCase final
{
    std::int32_t seed{};
    world::TerrainProfile profile{};
    GameMode mode{};
    RetailGolden expected;
};

// Recorded from the retail generator on 2026-10-14, before mod terrain was
// added (OPENMINERZ_PRINT_GOLDEN=1 prints the current values).
constexpr std::array<GoldenCase, 3> golden_cases{{
    {1337, world::TerrainProfile::classic, GameMode::endurance,
     {0xA728A7F30AB3601FULL, 0xFE74AFD97712ED75ULL}},
    {-20240917, world::TerrainProfile::modern, GameMode::survival,
     {0x800D75D3FFADB36DULL, 0x3FFF2765E6B652B9ULL}},
    {424242, world::TerrainProfile::modern, GameMode::scavenger,
     {0x79AC218C69ECFFE1ULL, 0x6181D01D20AB2DDBULL}},
}};

void retail_terrain_matches_its_golden_hashes()
{
    const bool print = environment_set("OPENMINERZ_PRINT_GOLDEN");
    for (const GoldenCase &test : golden_cases)
    {
        const world::WorldGenerator generator{test.seed, test.profile, test.mode};
        const RetailGolden actual = retail_golden(generator);
        if (print)
            std::cout << std::format("    {{{}, ..., {{0x{:016X}ULL, 0x{:016X}ULL}}}},\n", test.seed,
                                     actual.chunks, actual.columns);
        expect(actual.chunks == test.expected.chunks,
               std::format("retail chunks of seed {} changed: 0x{:016X}", test.seed, actual.chunks));
        expect(actual.columns == test.expected.columns,
               std::format("retail far columns / biomes / trees of seed {} changed: 0x{:016X}",
                           test.seed, actual.columns));
    }
}

// --- No generator installed: the retail passes, untouched --------------------

void without_a_generator_every_function_is_retail()
{
    world::WorldGenerator generator{555, world::TerrainProfile::modern, GameMode::survival};
    expect(generator.custom_terrain() == nullptr, "a new generator has no mod terrain");
    for (const auto &[x, z] : golden_positions)
    {
        const int chunk_x = x >= 0 ? x / 16 : (x - 15) / 16;
        const int chunk_z = z >= 0 ? z / 16 : (z - 15) / 16;
        const world::Chunk chunk = generator.generate_chunk(chunk_x, chunk_z);
        expect(chunk.hash() == generator.retail_chunk(chunk_x, chunk_z).hash() && !chunk.has_mod_cells(),
               "generate_chunk is retail_chunk without a mod generator");
        expect(generator.surface_column(x, z) == generator.retail_surface_column(x, z) &&
                   generator.biome_at(x, z) == generator.retail_biome_at(x, z),
               "far columns and biomes are retail without a mod generator");
    }
}

// --- The chunk's mod layer -----------------------------------------------------

void a_chunks_mod_layer_follows_its_edits()
{
    world::Chunk chunk{3, -2};
    expect(!chunk.has_mod_cells() && chunk.mod_hash() == 0U, "a retail chunk has no mod layer");
    chunk.set_mod_cell(1, 70, 1, 0U);
    expect(!chunk.has_mod_cells(), "clearing a cell of no layer allocates nothing");
    const std::uint64_t retail_hash = chunk.hash();
    chunk.set_block(1, 70, 1, world::BlockType::sand);
    chunk.set_mod_cell(1, 70, 1, 3U);
    expect(chunk.mod_cell(1, 70, 1) == 3U && chunk.has_mod_cells(), "a cell holds its palette index");
    world::Chunk snapshot = chunk; // what a mesh worker holds
    chunk.set_block(1, 70, 1, world::BlockType::sand);
    expect(chunk.mod_cell(1, 70, 1) == 3U, "writing the same block keeps the identity");
    chunk.set_block(1, 70, 1, world::BlockType::empty);
    expect(chunk.mod_cell(1, 70, 1) == 0U && chunk.previous_mod_cell(1, 70, 1) == 3U,
           "a dug cell loses its identity and remembers it");
    expect(snapshot.mod_cell(1, 70, 1) == 3U, "a snapshot keeps its own copy (copy on write)");
    chunk.set_block(1, 70, 1, world::BlockType::sand);
    expect(chunk.mod_cell(1, 70, 1) == 0U, "a retail block put back is retail");
    chunk.set_block(1, 70, 1, world::BlockType::rock);
    expect(chunk.previous_mod_cell(1, 70, 1) == 0U,
           "after the next change, what it held before is the retail block");
    expect(retail_hash != chunk.hash() && chunk.mod_hash() != snapshot.mod_hash(),
           "the mod layer has a hash of its own");
}

// --- A definition built in C++ ------------------------------------------------

struct DefinitionBuilder final
{
    world::TerrainDefinition definition;

    world::NoiseRef node(world::NoiseNode node)
    {
        definition.nodes.push_back(std::move(node));
        return static_cast<world::NoiseRef>(definition.nodes.size() - 1U);
    }
    world::NoiseRef constant(double value)
    {
        world::NoiseNode result;
        result.value = value;
        return node(result);
    }
    world::NoiseRef fbm(double scale, int octaves, std::uint32_t seed, bool three_d = false)
    {
        world::NoiseNode result;
        result.op = world::NoiseOp::fbm;
        result.scale = scale;
        result.octaves = octaves;
        result.seed = seed;
        result.three_d = three_d;
        return node(result);
    }
    world::NoiseRef combine(world::NoiseOp op, std::vector<world::NoiseRef> inputs)
    {
        world::NoiseNode result;
        result.op = op;
        result.inputs = std::move(inputs);
        return node(result);
    }
    std::uint8_t palette(std::string key, world::BlockType fallback)
    {
        definition.palette.push_back(std::move(key));
        definition.palette_fallback.push_back(fallback);
        return static_cast<std::uint8_t>(definition.palette.size());
    }
};

// Hills with overhangs, caves, ores and trees of mod blocks; flats with a
// fluid under the sea; retail terrain around the origin; climate patches in
// the outer band. Bands: retail to 600, hills to 3000, then a noise layout to
// 9000, mirrored beyond.
world::TerrainDefinition sample_definition(bool retail_core = true)
{
    DefinitionBuilder builder;
    world::TerrainDefinition &definition = builder.definition;
    definition.mod_id = "sample";
    const std::uint8_t bark = builder.palette("sample:bark", world::BlockType::log);
    const std::uint8_t moss = builder.palette("sample:moss", world::BlockType::dirt);
    const std::uint8_t crystal = builder.palette("sample:crystal", world::BlockType::diamond_ore);

    world::TerrainBiome hills;
    hills.key = "sample:hills";
    hills.height = builder.combine(
        world::NoiseOp::add,
        {builder.constant(16.0),
         builder.combine(world::NoiseOp::multiply, {builder.fbm(180.0, 5, 1), builder.constant(18.0)})});
    hills.density = builder.fbm(22.0, 2, 2, true);
    hills.density_strength = 6.0;
    hills.surface = {{{world::BlockType::grass}, 1}, {{world::BlockType::dirt, moss}, 3}};
    hills.ores = {{{world::BlockType::diamond_ore, crystal}, {}, 0.5, 2, 10, -60, 30, 7U}};
    world::TerrainTree redwood;
    redwood.shape = world::TreeShape::cone;
    redwood.chance = 0.02;
    redwood.height_min = 12;
    redwood.height_max = 20;
    redwood.radius = 3;
    redwood.thick = true;
    redwood.trunk = {world::BlockType::log, bark};
    world::TerrainTree oak;
    oak.chance = 0.01;
    hills.trees = {redwood, oak};
    definition.biomes.push_back(hills);

    world::TerrainBiome flats;
    flats.key = "sample:flats";
    flats.like = world::BiomeKind::desert;
    flats.height = builder.combine(
        world::NoiseOp::add,
        {builder.constant(1.5),
         builder.combine(world::NoiseOp::multiply, {builder.fbm(300.0, 3, 3), builder.constant(3.0)})});
    flats.surface = {{{world::BlockType::sand}, 2}};
    flats.fluid = {world::BlockType::ice};
    flats.caves.enabled = false;
    world::TerrainTree pillar;
    pillar.shape = world::TreeShape::column;
    pillar.chance = 0.004;
    pillar.trunk = {world::BlockType::rock};
    pillar.on = {{world::BlockType::sand}};
    flats.trees = {pillar};
    definition.biomes.push_back(flats);

    world::TerrainBiome retail;
    retail.key = "retail:world";
    retail.retail = world::RetailReuse::world;
    definition.biomes.push_back(retail);

    world::TerrainLayout root;
    root.warp = builder.combine(world::NoiseOp::multiply, {builder.fbm(600.0, 2, 4), builder.constant(80.0)});
    if (retail_core)
        root.bands.push_back({600.0, 0.0, 2, -1});
    root.bands.push_back({3000.0, 120.0, 0, -1});
    root.bands.push_back({9000.0, 160.0, -1, 1});
    world::TerrainLayout climate;
    climate.input = world::LayoutInput::noise;
    climate.select = builder.fbm(700.0, 2, 5);
    climate.bands = {{-0.1, 0.0, 1, -1}, {0.0, 0.1, 0, -1}};
    definition.layouts = {root, climate};
    return definition;
}

std::shared_ptr<const world::CustomTerrain> sample_terrain(bool retail_core = true)
{
    return std::make_shared<const world::CustomTerrain>(sample_definition(retail_core));
}

void a_broken_definition_is_refused_with_a_reason()
{
    const auto refused = [](world::TerrainDefinition definition, std::string_view why) {
        try
        {
            const world::CustomTerrain terrain{std::move(definition)};
            expect(false, std::format("accepted: {}", why));
        }
        catch (const std::invalid_argument &error)
        {
            expect(std::string_view{error.what()}.starts_with("world generator: "),
                   std::format("{}: a readable reason ({})", why, error.what()));
        }
    };
    world::TerrainDefinition empty = sample_definition();
    empty.biomes.clear();
    refused(empty, "no biomes");
    world::TerrainDefinition dangling = sample_definition();
    dangling.biomes[0].height = 9999U;
    refused(dangling, "a dangling expression");
    world::TerrainDefinition cyclic = sample_definition();
    cyclic.nodes[1].inputs = {5U};
    refused(cyclic, "an expression that names a later one");
    world::TerrainDefinition loop = sample_definition();
    loop.layouts[1].bands[0] = {0.0, 0.0, -1, 0};
    refused(loop, "a layout that nests its parent");
    world::TerrainDefinition wrong_fallback = sample_definition();
    wrong_fallback.biomes[0].filler = {world::BlockType::rock, 1U};
    refused(wrong_fallback, "a mod block that does not hold its fallback");
    world::TerrainDefinition no_height = sample_definition();
    no_height.biomes[1].height = world::no_noise;
    refused(no_height, "a biome without a height");
    world::TerrainDefinition unordered = sample_definition();
    unordered.layouts[0].bands[1].to = 100.0;
    refused(unordered, "bands out of order");
}

void the_layout_places_blends_and_mirrors_bands()
{
    // Without the warp, so distances are exact.
    world::TerrainDefinition definition = sample_definition();
    definition.layouts[0].warp = world::no_noise;
    const world::CustomTerrain terrain{std::move(definition)};
    world::WorldGenerator generator{2026};
    const auto biome = [&](int x, int z) { return terrain.biome_at(generator, x, z); };
    expect(biome(100, 100) == 2U && biome(-400, 0) == 2U, "inside 600 the retail world");
    expect(biome(1500, 0) == 0U && biome(0, -2500) == 0U, "600..3000 the hills");
    std::array<world::BiomeWeight, world::maximum_biome_weights> weights{};
    const std::size_t count = terrain.biome_weights(generator, 600, 0, weights);
    float total{};
    for (std::size_t index = 0U; index < count; ++index)
        total += weights[index].weight;
    expect(count >= 2U && std::abs(total - 1.0F) < 1e-4F && weights[0].weight >= weights[1].weight,
           "on a band edge two biomes blend with weights adding up to 1, heaviest first");
    expect(terrain.biome_weights(generator, 2700, 0, weights) == 1U,
           "outside the blend one biome");
    // Mirror past 9000: 9000 + d looks like 9000 - d, and 18000 + d like d
    // (in the distance bands; the climate band samples its own field).
    for (const int d : {6500, 8000, 8700})
        expect(biome(9000 + d, 0) == biome(9000 - d, 0),
               std::format("the layout mirrors at 9000 (d = {})", d));
    for (const int d : {300, 1500, 2600})
        expect(biome(18000 + d, 0) == biome(d, 0),
               std::format("the second ring starts over (d = {})", d));
    // Far beyond every band and fold, still a biome and no NaN.
    expect(biome(1'000'000, -1'000'000) <= 2U && biome(25'000, 0) <= 2U, "far away is still somewhere");
    // The retail biome the engine assumes follows the reused retail terrain.
    expect(terrain.retail_kind_at(generator, 450, 0) == generator.retail_biome_at(450, 0) &&
               terrain.retail_kind_at(generator, 1500, 0) == world::BiomeKind::classic,
           "retail_kind_at is retail's own biome inside the reused band, the biome's like elsewhere");
}

[[nodiscard]] int floor_chunk(int block) noexcept
{
    return block >= 0 ? block / 16 : (block - 15) / 16;
}

void every_thread_count_generates_the_same_chunks()
{
    const auto terrain = sample_terrain();
    for (const world::TerrainProfile profile : {world::TerrainProfile::classic, world::TerrainProfile::modern})
    {
        world::WorldGenerator generator{-77, profile, GameMode::survival};
        generator.set_custom_terrain(terrain);
        // A second generator with its own compiled copy of the definition.
        world::WorldGenerator other{-77, profile, GameMode::survival};
        other.set_custom_terrain(std::make_shared<const world::CustomTerrain>(sample_definition()));
        expect(other.custom_terrain()->digest() == terrain->digest(), "the same definition has the same digest");
        std::size_t mod_chunks{};
        for (const auto &[x, z] : std::initializer_list<std::array<int, 2>>{
                 {0, 0}, {590, 40}, {1700, -300}, {2990, 200}, {6000, 0}, {-8800, 1000}, {25000, 0}})
        {
            const int origin_x = floor_chunk(x) - 5;
            const int origin_z = floor_chunk(z) - 5;
            // 100 missing columns: the parallel path of stream_region (up to 8 workers).
            const world::WorldRegion parallel = generator.stream_region(
                world::WorldRegion{origin_x + 10000, origin_z, 1, 1}, origin_x, origin_z, 10, 10);
            for (const world::Chunk &chunk : parallel.chunks())
            {
                const world::Chunk alone = other.generate_chunk(chunk.chunk_x(), chunk.chunk_z());
                expect(chunk.hash() == alone.hash() && chunk.mod_hash() == alone.mod_hash(),
                       std::format("chunk {},{} is the same on any thread and from any copy",
                                   chunk.chunk_x(), chunk.chunk_z()));
                mod_chunks += chunk.has_mod_cells() ? 1U : 0U;
            }
        }
        expect(mod_chunks > 50U, std::format("generated chunks hold mod blocks ({})", mod_chunks));
    }
}

void far_columns_match_the_chunks()
{
    world::WorldGenerator generator{424242, world::TerrainProfile::classic, GameMode::endurance};
    generator.set_custom_terrain(sample_terrain());
    std::size_t compared{};
    std::size_t overhangs{};
    for (const auto &[x, z] : std::initializer_list<std::array<int, 2>>{
             {0, 0}, {600, 0}, {1200, 300}, {-2000, 1400}, {5000, 5000}, {25000, 100}, {40000, -64}})
    {
        const world::Chunk chunk = generator.generate_chunk(floor_chunk(x), floor_chunk(z));
        for (int local_z = 0; local_z < 16; local_z += 3)
            for (int local_x = 0; local_x < 16; local_x += 3)
            {
                const int wx = chunk.chunk_x() * 16 + local_x;
                const int wz = chunk.chunk_z() * 16 + local_z;
                std::array<world::BlockType, world::chunk_height> cells{};
                std::array<std::uint8_t, world::chunk_height> mods{};
                std::span<std::uint8_t, world::chunk_height> mod_span{mods};
                generator.custom_terrain()->generate_column(generator, wx, wz, cells, &mod_span);
                std::array<world::BlockType, world::chunk_height> plain{};
                generator.generate_column(wx, wz, plain);
                bool same = plain == cells;
                int top = -1;
                for (int y = 0; y < world::chunk_height; ++y)
                {
                    const world::BlockType expected = cells[static_cast<std::size_t>(y)];
                    if (expected != world::BlockType::empty)
                    {
                        top = y;
                        // Trees only fill cells the column left empty.
                        same = same && chunk.block(local_x, y, local_z) == expected &&
                               chunk.mod_cell(local_x, y, local_z) == mods[static_cast<std::size_t>(y)];
                    }
                }
                expect(same, std::format("column {},{} is the chunk's column (trees aside)", wx, wz));
                const world::SurfaceColumn surface = generator.surface_column(wx, wz);
                expect(surface.top_local_y == top &&
                           (top < 0 || surface.top == cells[static_cast<std::size_t>(top)]),
                       std::format("far column {},{}: top {} {} vs {}", wx, wz, surface.top_local_y,
                                   static_cast<int>(surface.top), top));
                if (surface.overhangs())
                {
                    ++overhangs;
                    expect(surface.lower_local_y < surface.under_local_y &&
                               cells[static_cast<std::size_t>(surface.under_local_y - 1)] ==
                                   world::BlockType::empty,
                           "an overhang has air under its span");
                }
                ++compared;
            }
        std::vector<world::TreeSite> trees;
        generator.tree_sites(chunk.chunk_x(), chunk.chunk_z(), trees);
        for (const world::TreeSite &tree : trees)
            expect(chunk.block(tree.x - chunk.chunk_x() * 16, tree.ground_local_y + 1,
                               tree.z - chunk.chunk_z() * 16) != world::BlockType::empty,
                   "a far tree site has a trunk in the chunk");
    }
    expect(compared > 200U, "enough columns compared");
    std::cout << std::format("    {} columns compared, {} with an overhang\n", compared, overhangs);
}

void reused_retail_terrain_is_retail_exactly()
{
    const auto terrain = sample_terrain();
    for (const world::TerrainProfile profile : {world::TerrainProfile::classic, world::TerrainProfile::modern})
    {
        world::WorldGenerator generator{97531, profile, GameMode::scavenger};
        generator.set_custom_terrain(terrain);
        for (int chunk_z = -12; chunk_z <= 12; chunk_z += 6)
            for (int chunk_x = -12; chunk_x <= 12; chunk_x += 6)
            {
                // Well inside the reused 600 blocks (the warp moves it by 80).
                const world::Chunk custom = generator.generate_chunk(chunk_x, chunk_z);
                const world::Chunk retail = generator.retail_chunk(chunk_x, chunk_z);
                const auto *custom_sites = custom.generation_sites();
                const auto *retail_sites = retail.generation_sites();
                expect(custom.hash() == retail.hash() && !custom.has_mod_cells() &&
                           (custom_sites == nullptr) == (retail_sites == nullptr) &&
                           (custom_sites == nullptr ||
                            (custom_sites->carved_count == retail_sites->carved_count &&
                             custom_sites->caves.size() == retail_sites->caves.size() &&
                             custom_sites->hell_floor.size() == retail_sites->hell_floor.size())),
                       std::format("chunk {},{} inside the retail band is retail's own, encounter sites "
                                   "included",
                                   chunk_x, chunk_z));
                expect(generator.surface_column(chunk_x * 16 + 5, chunk_z * 16 + 9) ==
                           generator.retail_surface_column(chunk_x * 16 + 5, chunk_z * 16 + 9),
                       "far columns there are retail's too");
            }
    }
    // A single retail biome reused: its own surface pass at full strength.
    DefinitionBuilder builder;
    world::TerrainBiome desert;
    desert.key = "retail:desert";
    desert.retail = world::RetailReuse::desert;
    builder.definition.biomes.push_back(desert);
    builder.definition.layouts.push_back({});
    builder.definition.layouts.back().bands.push_back({0.0, 0.0, 0, -1});
    world::WorldGenerator generator{31337};
    generator.set_custom_terrain(std::make_shared<const world::CustomTerrain>(std::move(builder.definition)));
    std::array<world::BlockType, world::chunk_height> cells{};
    std::array<world::BlockType, world::chunk_height> expected{};
    generator.generate_column(20000, -4000, cells);
    generator.retail_biome_column(world::BiomeKind::desert, 20000, -4000, expected);
    expect(cells == expected && generator.biome_at(20000, -4000) == world::BiomeKind::desert,
           "retail:desert 20,000 blocks out is the desert column");
}

void the_digest_is_the_generator_not_its_presentation()
{
    const world::TerrainDefinition definition = sample_definition();
    world::TerrainDefinition renamed = definition;
    renamed.biomes[0].name = "Something Else";
    renamed.mod_version = "9.9.9";
    renamed.biomes[0].like = world::BiomeKind::arctic;
    world::TerrainDefinition changed = definition;
    changed.biomes[0].ores[0].chance = 0.51;
    const world::CustomTerrain a{definition};
    const world::CustomTerrain b{renamed};
    const world::CustomTerrain c{changed};
    expect(a.digest() == b.digest(), "names, versions and the presentation biome are not in the digest");
    expect(a.digest() != c.digest() && a.digest().size() == 64U, "anything that shapes a block is");
}

// --- Scripts: the terrain and noise libraries ------------------------------------

// The game as the gameplay service sees it, with its terrain from a real
// WorldGenerator (so biome() and generated_block() answer like the app's).
class FakeGame final : public gameplay::GameplayBackend
{
  public:
    world::WorldGenerator generator{20261014};
    std::uint8_t local_gid{};
    std::string local_name{"Host"};
    std::map<gameplay::BlockPos, std::uint8_t> edits;
    std::map<std::pair<int, int>, world::Chunk> chunks;

    world::Chunk &chunk(int x, int z)
    {
        const std::pair key{floor_chunk(x), floor_chunk(z)};
        auto found = chunks.find(key);
        if (found == chunks.end())
            found = chunks.emplace(key, generator.generate_chunk(key.first, key.second)).first;
        return found->second;
    }

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return generator.seed();
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return true;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        return local_gid;
    }
    void players(std::vector<gameplay::PlayerSnapshot> &out) const override
    {
        out.clear();
        out.push_back({local_gid, local_name, true, true, {8.5, 30.0, -7.5}, 0.0F, 100.0F});
    }
    void local_set_health(float) override
    {
    }
    void local_damage(float, modding::Vec3, gameplay::DamageCause) override
    {
    }
    void local_teleport(modding::Vec3) override
    {
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
        const world::Chunk &column = const_cast<FakeGame *>(this)->chunk(cell.x, cell.z);
        return static_cast<std::uint8_t>(column.block(cell.x - column.chunk_x() * 16, cell.y + 64,
                                                      cell.z - column.chunk_z() * 16));
    }
    bool set_block(gameplay::BlockPos cell, std::uint8_t value) override
    {
        edits[cell] = value;
        world::Chunk &column = chunk(cell.x, cell.z);
        column.set_block(cell.x - column.chunk_x() * 16, cell.y + 64, cell.z - column.chunk_z() * 16,
                         static_cast<world::BlockType>(value));
        return true;
    }
    [[nodiscard]] int biome(int x, int z) const override
    {
        const auto &terrain = generator.custom_terrain();
        if (terrain == nullptr)
            return static_cast<int>(generator.biome_at(x, z));
        const world::TerrainBiome &biome = terrain->definition().biomes[terrain->biome_at(generator, x, z)];
        return biome.retail != world::RetailReuse::none ? static_cast<int>(generator.biome_at(x, z))
                                                        : world::biome_id(biome.key);
    }
    [[nodiscard]] std::string generated_block(gameplay::BlockPos cell, bool before_change) const override
    {
        const auto &terrain = generator.custom_terrain();
        if (terrain == nullptr)
            return {};
        const world::Chunk &column = const_cast<FakeGame *>(this)->chunk(cell.x, cell.z);
        const int lx = cell.x - column.chunk_x() * 16;
        const int lz = cell.z - column.chunk_z() * 16;
        const std::uint8_t palette = before_change ? column.previous_mod_cell(lx, cell.y + 64, lz)
                                                   : column.mod_cell(lx, cell.y + 64, lz);
        return palette == 0U ? std::string{} : terrain->definition().palette[palette - 1U];
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(gameplay::BlockPos) const override
    {
        return std::array<float, 2>{1.0F, 0.0F};
    }
    bool set_crate_slot(gameplay::BlockPos, std::size_t, std::optional<gameplay::StackView>) override
    {
        return false;
    }
    bool spawn_pickup(modding::Vec3 at, gameplay::StackView stack) override
    {
        pickups.emplace_back(at, stack);
        return true;
    }
    std::vector<std::pair<modding::Vec3, gameplay::StackView>> pickups;
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
};

// The app's WorldgenBackend (app_mod_world.inc): block fallbacks from the
// side's gameplay definitions.
class FakeWorldgen final : public worldgen::WorldgenBackend
{
  public:
    explicit FakeWorldgen(FakeGame &game) : game_{game}
    {
    }
    gameplay::GameplayService *gameplay{};

    [[nodiscard]] std::optional<std::uint8_t> block_fallback(modding::Side side,
                                                             std::string_view key) const override
    {
        return gameplay != nullptr ? gameplay->block_fallback(side, key) : std::nullopt;
    }
    [[nodiscard]] const world::WorldGenerator *generator() const override
    {
        return &game_.generator;
    }
    [[nodiscard]] std::int32_t world_seed() const override
    {
        return game_.generator.seed();
    }

  private:
    FakeGame &game_;
};

// The frontier example's files: all of them for a host, the client-visible
// part (shared/, client/) for a joined player, as the mod download sends.
modding::ModSource frontier_mod(bool client_visible_only)
{
    modding::ModSource source;
    source.id = "frontier";
    source.version = "1.0.0";
    source.scope = net::ModScope::server;
    const std::filesystem::path root = std::filesystem::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods" / "frontier";
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root})
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".luau")
            continue;
        std::string relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (client_visible_only && !net::is_client_visible_mod_path(relative))
            continue;
        std::ifstream input{entry.path(), std::ios::binary};
        source.files[relative] = std::string{std::istreambuf_iterator<char>{input}, {}};
    }
    return source;
}

// (wave 5) One script of an example mod (the wilderness pack's land is its
// shared/terrain.luau; its other scripts need the whole game's API).
modding::ModSource example_script(std::string_view id, std::string_view relative)
{
    modding::ModSource source;
    source.id = std::string{id};
    source.version = "1.0.0";
    source.scope = net::ModScope::server;
    const std::filesystem::path path =
        std::filesystem::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods" / std::string{id} / std::string{relative};
    std::ifstream input{path, std::ios::binary};
    source.files[std::string{relative}] = std::string{std::istreambuf_iterator<char>{input}, {}};
    return source;
}

void scripts_define_a_world_natively()
{
    modding::testing::Harness harness;
    FakeGame game;
    FakeWorldgen backend{game};
    auto gameplay_service = modding::register_gameplay_api(harness.registry(), harness.host(), game);
    backend.gameplay = gameplay_service.get();
    auto service = worldgen::register_worldgen_api(harness.registry(), harness.host(), backend);
    const auto result = harness.run(R"(
        blocks.define("moss", { fallback = "dirt", texture = "moss.png" })
        local hills = noise.add(14, noise.mul(noise.fbm({ scale = 200, octaves = 4, seed = 3 }), 12))
        terrain.biome("hills", { height = hills, surface = { "grass", { block = "moss", depth = 3 } },
            trees = { { shape = "round", chance = 0.02, radius = 2 } } })
        terrain.biome("flats", { like = "desert", height = 3, surface = { { block = "sand", depth = 2 } },
            fluid = "ice", caves = false })
        terrain.define({ spawn = { x = 40, z = 40 }, layout = {
            bands = { { to = 500, biome = "retail" }, { to = 4000, biome = "hills", blend = 100 },
                      { to = 9000, biome = "flats", blend = 100 } } } })
        return noise.sample(hills, 10, 20)
    )", modding::Side::server);
    expect(result.ok, "the definition runs: " + result.error);
    expect(result.value.is_number() && result.value.as_number() > 0.0 && result.value.as_number() < 30.0,
           "noise.sample evaluates natively");
    std::string error;
    const auto terrain = service->build(modding::Side::server, error);
    expect(terrain != nullptr && error.empty(), "the world builds: " + error);
    if (terrain == nullptr)
        return;
    const world::TerrainDefinition &definition = terrain->definition();
    expect(definition.palette == std::vector<std::string>{"test:moss"} &&
               definition.palette_fallback == std::vector<world::BlockType>{world::BlockType::dirt},
           "mod blocks become palette entries with their fallback");
    expect(definition.biomes.size() == 3U && definition.biomes[1].key == "test:hills" &&
               definition.spawn_x == 40 && definition.biomes[0].retail == world::RetailReuse::world,
           "biomes, retail reuse and the spawn arrive");
    expect(world::biome_name(world::biome_id("test:hills")) == "test:hills", "mod biomes get ids");
    // The sample is exactly what the generator sees.
    game.generator.set_custom_terrain(terrain);
    service->set_active(terrain);
    const auto again = harness.run(R"(
        local hills = noise.add(14, noise.mul(noise.fbm({ scale = 200, octaves = 4, seed = 3 }), 12))
        local info = terrain.info(1500, 0)
        return { noise.sample(hills, 1500, 0), info.height, info.biome, world.biome(1500, 0), terrain.active() }
    )", modding::Side::server);
    expect(again.ok, again.error);
    if (again.ok)
    {
        const modding::Array &values = again.value.as_array();
        expect(static_cast<int>(std::floor(values[0].as_number())) == values[1].as_integer(),
               "noise.sample of the height is the ground terrain.info reports");
        expect(values[2] == modding::Value{"test:hills"} && values[3] == modding::Value{"test:hills"} &&
                   values[4] == modding::Value{true},
               "terrain.info and world.biome name the mod biome");
    }
    // A creature may spawn in a mod biome.
    const auto creature = harness.run(R"(
        creatures.define("crab", { behaviour = "passive", spawn = { biomes = { "test:hills", "desert" } } })
        return world.biome(300, 300)
    )", modding::Side::server);
    expect(creature.ok && creature.value == modding::Value{"lagoon"},
           "spawn biomes accept mod keys; the reused retail band names retail's biome: " + creature.error);
}

void a_client_mod_cannot_shape_the_world()
{
    modding::testing::Harness harness;
    FakeGame game;
    FakeWorldgen backend{game};
    auto service = worldgen::register_worldgen_api(harness.registry(), harness.host(), backend);
    const auto refused = harness.run(R"(terrain.define({ layout = { bands = { { biome = "retail" } } } }))",
                                     modding::Side::client, modding::testing::client_mod_capabilities(), "skin");
    expect(!refused.ok && refused.error.find("server or core mod") != std::string::npos,
           "terrain.define from a client mod is refused: " + refused.error);
    const auto allowed = harness.run(R"(return noise.fbm({ scale = 30 }).op)", modding::Side::client,
                                     modding::testing::client_mod_capabilities(), "skin");
    expect(allowed.ok && allowed.value == modding::Value{"fbm"}, "noise expressions are plain data anyone can build");
    std::string error;
    expect(service->build(modding::Side::client, error) == nullptr && error.empty(),
           "no definition, no generator");
}

void a_bad_world_names_what_is_wrong()
{
    const auto failure = [](std::string_view source) {
        modding::testing::Harness harness;
        FakeGame game;
        FakeWorldgen backend{game};
        auto gameplay_service = modding::register_gameplay_api(harness.registry(), harness.host(), game);
        backend.gameplay = gameplay_service.get();
        auto service = worldgen::register_worldgen_api(harness.registry(), harness.host(), backend);
        const auto result = harness.run(source, modding::Side::server);
        if (!result.ok)
            return result.error;
        std::string error;
        static_cast<void>(service->build(modding::Side::server, error));
        return error;
    };
    expect(failure(R"(terrain.biome("a", { height = 1, surface = { "nope_block" } })
                      terrain.define({ layout = { bands = { { biome = "a" } } } }))")
                   .find("unknown block 'nope_block'") != std::string::npos,
           "an unknown block is named");
    expect(failure(R"(terrain.define({ layout = { bands = { { biome = "missing" } } } }))")
                   .find("no terrain.biome defines") != std::string::npos,
           "an unknown biome is named");
    expect(failure(R"(terrain.biome("a", { height = { op = "fbm", sclae = 3 } }))").find("unknown field 'sclae'") !=
               std::string::npos,
           "a hand-made expression is checked");
    expect(failure(R"(terrain.biome("a", { height = 1, colour = "red" }))").find("unknown field 'colour'") !=
               std::string::npos,
           "a misspelt field is refused");
    expect(failure(R"(noise.curve(noise.x(), { { 1, 0 } }))").find("2 to 64 points") != std::string::npos,
           "a curve needs points");
    expect(failure(R"(terrain.define({ layout = { bands = { { to = 5, biome = "retail" }, { to = 2, biome = "retail" },
                      { biome = "retail" } } } }))")
                   .find("increase") != std::string::npos,
           "bands must increase");
}

// --- Host and joined client agree --------------------------------------------------

// One machine's stack: runtime, gameplay, the world generator service and the
// mod channel, wired as the app wires them.
struct Machine final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    FakeWorldgen backend{game};
    std::unique_ptr<gameplay::GameplayService> gameplay;
    std::unique_ptr<worldgen::WorldgenService> worldgen_service;
    std::unique_ptr<modding::ModNetServer> server;
    std::unique_ptr<modding::ModNetClient> client;
    double now{};

    Machine()
    {
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        backend.gameplay = gameplay.get();
        worldgen_service = worldgen::register_worldgen_api(runtime.registry(), runtime, backend);
    }
    ~Machine()
    {
        runtime.stop_all();
    }
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;
};

net::ModManifest frontier_manifest()
{
    net::ModManifest manifest;
    manifest.protocol = net::ProtocolVersion::classic_v3;
    net::ModDigest digest{};
    digest.fill(0x5AU);
    manifest.mods.push_back({"frontier", "1.0.0", digest, net::ModScope::server});
    return manifest;
}

bool admit_peer(net::LoopbackSession &session, const net::HostSessionSettings &settings, net::PeerId peer,
                const net::ModManifest &manifest)
{
    net::PeerAdmissionRequest request;
    request.peer = peer;
    request.properties = net::make_session_properties(settings, false, 1, 1, false);
    request.advertised_extension = true;
    const net::AdmissionDecision decision = session.connect(request);
    if (!decision.accepted())
        return false;
    net::ExtensionMessage hello;
    hello.kind = net::ExtensionMessageKind::client_hello;
    hello.nonce = 0x9000U + peer;
    hello.manifest = manifest;
    static_cast<void>(session.submit_from_peer(peer, net::encode_extension_message(hello),
                                               net::MessageDeliveryPolicy::reliable_ordered,
                                               net::openminerz_control_channel));
    static_cast<void>(session.receive_for_peer(peer));
    return session.admission().status(peer) == net::AdmissionStatus::admitted;
}

void host_and_joined_client_generate_the_same_world()
{
    const net::ModManifest manifest = frontier_manifest();
    net::HostSessionSettings settings;
    settings.server_name = "Frontier";
    settings.mod_manifest = manifest;
    net::LoopbackSession wire{settings, false, 1, 1, false};

    // The host: server and client VMs with every file of the mod.
    Machine host;
    host.server = std::make_unique<modding::ModNetServer>(manifest);
    host.client = std::make_unique<modding::ModNetClient>(manifest);
    const modding::Handle host_player = host.gameplay->admit_player(0U, "Host", true);
    expect(host.server->add_peer(modding::local_mod_peer, host_player, 0.0), "the host's channel opens");
    expect(host.runtime.start(modding::Side::server, {frontier_mod(false)}, {}) &&
               host.runtime.start(modding::Side::client, {frontier_mod(false)}, {}),
           "the host runs Frontier on both VMs");
    std::string error;
    const auto host_terrain = host.worldgen_service->build(modding::Side::server, error);
    expect(host_terrain != nullptr, "the host builds Frontier's world: " + error);
    const auto host_client_terrain = host.worldgen_service->build(modding::Side::client, error);
    expect(host_client_terrain != nullptr && host_terrain != nullptr &&
               host_client_terrain->digest() == host_terrain->digest(),
           "the host's two VMs define the same world");
    if (host_terrain == nullptr)
        return;
    expect(host.worldgen_service->world_mod(modding::Side::server).first == "frontier", "the world names its mod");
    // What the app publishes for its clients.
    const modding::Value published{modding::Map{{"mod", modding::Value{"frontier"}},
                                                {"version", modding::Value{"1.0.0"}},
                                                {"digest", modding::Value{host_terrain->digest()}}}};
    host.server->set(modding::StoreRef::world(), "", worldgen::terrain_channel_key, published,
                     modding::ReplicatedKeyOptions{false, modding::ReplicationPriority::critical});

    // A joined player: only the client-visible files, only the client VM.
    expect(admit_peer(wire, settings, 42U, manifest), "the joiner is admitted");
    const modding::Handle joiner = host.gameplay->admit_player(3U, "Joiner", false);
    expect(host.server->add_peer(42U, joiner, 0.0), "the joiner's channel opens");
    Machine guest;
    guest.client = std::make_unique<modding::ModNetClient>(manifest);
    const modding::ModSource visible = frontier_mod(true);
    expect(!visible.files.contains("server/landmarks.luau") && visible.files.contains("shared/world.luau"),
           "the server script stays on the host");
    expect(guest.runtime.start(modding::Side::client, {visible}, {}), "the joiner runs Frontier's client VM");
    const auto guest_terrain = guest.worldgen_service->build(modding::Side::client, error);
    expect(guest_terrain != nullptr, "the joiner builds the world from what it downloaded: " + error);
    if (guest_terrain == nullptr)
        return;

    // The channel carries the host's choice.
    for (int round = 0; round < 64; ++round)
    {
        host.now += 0.05;
        guest.now += 0.05;
        host.server->update(host.now);
        guest.client->set_ready(guest.now);
        guest.client->update(guest.now);
        for (const auto &frame : guest.client->take_sends())
            static_cast<void>(wire.submit_from_peer(42U, frame, net::MessageDeliveryPolicy::reliable_ordered,
                                                    net::openminerz_control_channel));
        for (modding::ModNetSend &send : host.server->take_sends())
            if (send.peer == 42U)
                static_cast<void>(wire.submit_from_host(send.peer, send.frame,
                                                        net::MessageDeliveryPolicy::reliable_ordered,
                                                        net::openminerz_control_channel));
        while (auto datagram = wire.receive_for_host())
            host.server->receive(datagram->sender, datagram->payload, host.now);
        while (auto datagram = wire.receive_for_peer(42U))
            guest.client->receive(datagram->payload, guest.now);
    }
    const modding::Value *received =
        guest.client->get(modding::StoreRef::world(), "", worldgen::terrain_channel_key);
    expect(received != nullptr && received->field("digest") != nullptr &&
               received->field("digest")->as_string() == guest_terrain->digest(),
           "the host's world digest reaches the joiner and matches its own");

    // Same chunks, mod blocks and far columns on both, whatever the threads.
    world::WorldGenerator on_host{-1234567, world::TerrainProfile::classic, GameMode::survival};
    on_host.set_custom_terrain(host_terrain);
    world::WorldGenerator on_guest{-1234567, world::TerrainProfile::classic, GameMode::survival};
    on_guest.set_custom_terrain(guest_terrain);
    std::size_t chunks{};
    std::size_t mod_chunks{};
    for (const int distance : {0, 700, 3000, 6500, 9000, 13000, 16000, 22000, 25000, 30000, 40000})
    {
        const int origin = floor_chunk(distance) - 3;
        const world::WorldRegion host_region =
            on_host.stream_region(world::WorldRegion{origin + 5000, 0, 1, 1}, origin, -3, 8, 8);
        for (const world::Chunk &chunk : host_region.chunks())
        {
            const world::Chunk theirs = on_guest.generate_chunk(chunk.chunk_x(), chunk.chunk_z());
            expect(chunk.hash() == theirs.hash() && chunk.mod_hash() == theirs.mod_hash(),
                   std::format("chunk {},{} agrees between host and joiner", chunk.chunk_x(), chunk.chunk_z()));
            ++chunks;
            mod_chunks += chunk.has_mod_cells() ? 1U : 0U;
        }
        for (int step = 0; step < 64; ++step)
        {
            const int x = distance + step * 13 - 300;
            const int z = step * 29 - 900;
            expect(on_host.surface_column(x, z, false) == on_guest.surface_column(x, z, false) &&
                       on_host.biome_at(x, z) == on_guest.biome_at(x, z),
                   "far columns agree");
        }
    }
    expect(mod_chunks > chunks / 3U, std::format("Frontier's chunks hold mod blocks ({} of {})", mod_chunks, chunks));
    std::cout << std::format("    {} chunks agree ({} with mod blocks), digest {}\n", chunks, mod_chunks,
                             host_terrain->digest().substr(0U, 16U));
}

void generated_mod_blocks_have_their_identity_in_gameplay()
{
    modding::testing::Harness harness;
    FakeGame game;
    FakeWorldgen backend{game};
    auto gameplay_service = modding::register_gameplay_api(harness.registry(), harness.host(), game);
    backend.gameplay = gameplay_service.get();
    auto service = worldgen::register_worldgen_api(harness.registry(), harness.host(), backend);
    const auto result = harness.run(R"(
        blocks.define("moss", { fallback = "dirt", drops = "stick" })
        terrain.biome("mossy", { height = 10, surface = { { block = "moss", depth = 4 } }, caves = false })
        terrain.define({ origin_tower = false, layout = { bands = { { biome = "mossy" } } } })
        broken = {}
        events.on("block.broken", function(e) table.insert(broken, e.block) end)
    )", modding::Side::server);
    expect(result.ok, result.error);
    std::string error;
    const auto terrain = service->build(modding::Side::server, error);
    expect(terrain != nullptr, error);
    if (terrain == nullptr)
        return;
    game.generator.set_custom_terrain(terrain);
    gameplay_service->start_server();
    const auto read = harness.run(R"(return world.get_block(vector.create(5, 10, 5)))", modding::Side::server);
    expect(read.ok && read.value == modding::Value{"test:moss"},
           "world.get_block names a generated mod block: " + read.error + " / " +
               (read.value.type() == modding::ValueType::string ? read.value.as_string() : std::string{}));
    // A dig: the game empties the cell first, then asks about drops and tells
    // the scripts (app_mod_gameplay.inc's order).
    static_cast<void>(game.set_block({5, 10, 5}, 0U));
    expect(!gameplay_service->local_block_drops({5, 10, 5}, 1U) && !game.pickups.empty(),
           "the mod block's own drops replace the fallback's");
    gameplay_service->local_block_broken({5, 10, 5}, 1U);
    const auto broken = harness.run(R"(return broken[1])", modding::Side::server);
    expect(broken.ok && broken.value == modding::Value{"test:moss"}, "block.broken names it after the dig");
    // Put a retail dirt block back and dig again: that one is retail.
    static_cast<void>(game.set_block({5, 10, 5}, 1U));
    static_cast<void>(game.set_block({5, 10, 5}, 0U));
    gameplay_service->local_block_broken({5, 10, 5}, 1U);
    const auto second = harness.run(R"(return broken[2])", modding::Side::server);
    expect(second.ok && second.value == modding::Value{"dirt"}, "a re-placed retail block is retail");
}

// --- The world's record of its generator ----------------------------------------------

void a_world_records_its_generator()
{
    // OPENMINERZ_WORLD_FIXTURE=<folder> also leaves a small saved world there
    // (no record) for checking the game's open/refuse/warn path by hand.
    if (const std::optional<std::string> fixture = environment_value("OPENMINERZ_WORLD_FIXTURE"))
    {
        WorldSaveData save;
        save.seed = 20261014;
        save.owner = "mod-world fixture";
        save.has_player = true;
        save.player_position = {3200.5F, 40.0F, 40.5F};
        save.inventory.set_profile(InventoryProfile::classic);
        const std::filesystem::path path = std::filesystem::path{*fixture} / "Frontier Fixture.omw";
        std::filesystem::create_directories(path.parent_path());
        expect(write_world_save(path, save), "the fixture world is written");
        std::cout << "    fixture world: " << path.string() << '\n';
    }
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "openminerz-mod-world-record";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    expect(!worldgen::read_generator_record(folder).has_value(), "a retail world has no record");
    const worldgen::GeneratorRecord record{"frontier", "1.0.0", std::string(64U, 'a')};
    expect(worldgen::write_generator_record(folder, record), "the record is written");
    const auto read = worldgen::read_generator_record(folder);
    expect(read.has_value() && *read == record, "and read back");
    using worldgen::GeneratorVerdict;
    const std::optional<worldgen::GeneratorRecord> none;
    worldgen::GeneratorRecord changed = record;
    changed.digest = std::string(64U, 'b');
    changed.mod_version = "1.1.0";
    worldgen::GeneratorRecord other = record;
    other.mod_id = "elsewhere";
    expect(worldgen::judge_generator(none, none, false) == GeneratorVerdict::retail &&
               worldgen::judge_generator(none, record, true) == GeneratorVerdict::use &&
               worldgen::judge_generator(none, record, false) == GeneratorVerdict::keep_retail &&
               worldgen::judge_generator(record, record, false) == GeneratorVerdict::use &&
               worldgen::judge_generator(record, changed, false) == GeneratorVerdict::changed &&
               worldgen::judge_generator(record, none, false) == GeneratorVerdict::missing &&
               worldgen::judge_generator(record, other, false) == GeneratorVerdict::missing,
           "opening a world judges its generator");
    const std::string missing = worldgen::generator_message(GeneratorVerdict::missing, record, none);
    expect(missing.find("'frontier'") != std::string::npos && missing.find("Install") != std::string::npos,
           "refusing names the mod: " + missing);
    const std::string warning = worldgen::generator_message(GeneratorVerdict::changed, record, changed);
    expect(warning.find("1.0.0 -> 1.1.0") != std::string::npos, "the warning names the versions: " + warning);
    // A damaged record still refuses (rather than regenerating the world).
    {
        std::ofstream output{folder / worldgen::generator_record_file, std::ios::trunc};
        output << "garbage";
    }
    std::string error;
    const auto damaged = worldgen::read_generator_record(folder, &error);
    expect(damaged.has_value() && damaged->mod_id == "?" && !error.empty() &&
               worldgen::judge_generator(damaged, record, false) == GeneratorVerdict::missing,
           "a damaged record is refused");
    std::filesystem::remove_all(folder, ignored);
}

// --- Big distances ----------------------------------------------------------------------

void big_distances_stay_exact()
{
    world::WorldGenerator generator{8080, world::TerrainProfile::modern, GameMode::survival};
    generator.set_custom_terrain(sample_terrain(false));
    world::WorldGenerator retail{8080, world::TerrainProfile::modern, GameMode::survival};
    for (const int distance : {0, 4400, 10000, 25000, 40000, 46500, 100000, 1000000})
    {
        // Generation: the custom terrain has no overflow pillars (retail
        // squares the coordinates in 32 bits, see world.cpp).
        const int chunk_x = floor_chunk(distance);
        const world::Chunk chunk = generator.generate_chunk(chunk_x, 0);
        int bedrock_column_tops{};
        for (int local_x = 0; local_x < 16; ++local_x)
            bedrock_column_tops += chunk.block(local_x, 100, 12) == world::BlockType::bedrock ? 1 : 0;
        expect(bedrock_column_tops == 0, std::format("no bedrock pillars at {}", distance));
        // Region keys, cells and edits.
        world::WorldRegion region{chunk_x - 1, -1, 3, 3};
        region.chunks()[4] = chunk;
        const int x = distance + 7;
        expect(region.contains(x, 10, 9) && region.block(x, 10, 9) == chunk.block(7, 74, 9),
               std::format("a region at {} finds its cells", distance));
        region.set_block(x, 10, 9, world::BlockType::torch);
        expect(region.block(x, 10, 9) == world::BlockType::torch, "and edits them");
        // Save coordinates: edits are integers.
        WorldSaveData save;
        save.seed = 8080;
        save.has_player = true;
        save.player_position = {static_cast<float>(distance) + 0.5F, 20.0F, -static_cast<float>(distance) - 0.5F};
        save.edits.push_back({x, 10, -x, 31});
        std::ostringstream stream;
        expect(write_world_save(stream, save), "the save is written");
        WorldSaveData loaded;
        std::istringstream input{stream.str()};
        expect(read_world_save(input, loaded) && loaded.edits == save.edits &&
                   loaded.player_position.x == save.player_position.x &&
                   loaded.player_position.z == save.player_position.z,
               std::format("save coordinates at {} survive", distance));
        // The stock position on the wire is a float Vector3.
        net::PlayerUpdatePayload update;
        update.local_position = {static_cast<float>(distance) + 0.25F, 33.5F, static_cast<float>(-distance) + 0.75F};
        net::PayloadError payload_error{};
        const auto bytes = net::encode_player_update_payload(update, net::ProtocolVersion::modern_v4, payload_error);
        net::PlayerUpdatePayload decoded;
        expect(bytes.has_value() &&
                   net::decode_player_update_payload(*bytes, net::ProtocolVersion::modern_v4, decoded,
                                                     payload_error) &&
                   decoded.local_position.x == update.local_position.x &&
                   decoded.local_position.z == update.local_position.z,
               std::format("a player position at {} crosses the wire exactly", distance));
        // How finely a float can place things there (the physics step of a
        // walking player is about 0.07 blocks per tick).
        const float here = static_cast<float>(distance) + 0.5F;
        const float step = std::nextafter(here, 2.0F * here + 1.0F) - here;
        std::cout << std::format("    {:>8} blocks: float spacing {:.6f} blocks ({:.1f} steps per walking tick)\n",
                                 distance, step, 0.07F / step);
    }
    // Retail's own far generation is untouched (it IS the golden).
    expect(retail.generate_chunk(floor_chunk(46500), 0).hash() == retail.retail_chunk(floor_chunk(46500), 0).hash(),
           "retail keeps its far-away pillars");
}

// --- The Wilderness example's land (wave 5) ---------------------------------------------------

std::shared_ptr<const world::CustomTerrain> wilderness_terrain(Machine &machine)
{
    if (!machine.runtime.start(modding::Side::server,
                               {example_script("wilderness", "shared/terrain.luau")}, {}))
        return nullptr;
    std::string error;
    auto terrain = machine.worldgen_service->build(modding::Side::server, error);
    expect(terrain != nullptr, "the wilderness land builds: " + error);
    return terrain;
}

// Fluid cells of the chunks in [cx0, cx1] x [cz0, cz1] with air under them or
// beside them: water hanging over a cave (nothing flows, so it would stay).
int hanging_fluid(const world::WorldGenerator &generator, int sea_level, int cx0, int cz0, int cx1, int cz1)
{
    std::map<std::pair<int, int>, world::Chunk> chunks;
    const auto chunk_at = [&](int cx, int cz) -> const world::Chunk & {
        auto found = chunks.find({cx, cz});
        if (found == chunks.end())
            found = chunks.emplace(std::pair{cx, cz}, generator.generate_chunk(cx, cz)).first;
        return found->second;
    };
    const auto block = [&](int x, int local_y, int z) {
        const world::Chunk &chunk = chunk_at(floor_chunk(x), floor_chunk(z));
        return chunk.block(x - floor_chunk(x) * world::chunk_width, local_y, z - floor_chunk(z) * world::chunk_depth);
    };
    int hanging{};
    const int surface = sea_level - world::world_min_y;
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const world::Chunk &chunk = chunk_at(cx, cz);
            for (int local_z = 0; local_z < world::chunk_depth; ++local_z)
                for (int local_x = 0; local_x < world::chunk_width; ++local_x)
                    for (int local_y = 1; local_y <= surface; ++local_y)
                    {
                        if (chunk.block(local_x, local_y, local_z) != world::BlockType::ice ||
                            chunk.mod_cell(local_x, local_y, local_z) == 0U)
                            continue;
                        const int x = cx * world::chunk_width + local_x;
                        const int z = cz * world::chunk_depth + local_z;
                        if (block(x, local_y - 1, z) == world::BlockType::empty ||
                            block(x + 1, local_y, z) == world::BlockType::empty ||
                            block(x - 1, local_y, z) == world::BlockType::empty ||
                            block(x, local_y, z + 1) == world::BlockType::empty ||
                            block(x, local_y, z - 1) == world::BlockType::empty)
                            ++hanging;
                    }
        }
    return hanging;
}

void wilderness_mountains_replace_the_retail_ring()
{
    Machine machine;
    const auto terrain = wilderness_terrain(machine);
    if (terrain == nullptr)
        return;
    const auto &definition = terrain->definition();
    const auto palette_index = [&](std::string_view key) {
        for (std::size_t index = 0U; index < definition.palette.size(); ++index)
            if (definition.palette[index] == key)
                return static_cast<std::uint8_t>(index + 1U);
        return std::uint8_t{};
    };
    const std::uint8_t water = palette_index("wilderness:water");
    const std::uint8_t spruce = palette_index("wilderness:spruce_log");
    expect(water != 0U && spruce != 0U, "the land names its water and spruce blocks");
    for (const std::int32_t seed : {20261014, -77, 424242})
    {
        const world::WorldGenerator retail{seed, world::TerrainProfile::modern, GameMode::survival};
        world::WorldGenerator generator{seed, world::TerrainProfile::modern, GameMode::survival};
        generator.set_custom_terrain(terrain);
        // The spawn and everything but the mountain ring is retail's own.
        expect(generator.surface_column(definition.spawn_x, definition.spawn_z, false) ==
                   retail.surface_column(definition.spawn_x, definition.spawn_z, false),
               std::format("seed {}: the spawn stands on retail ground", seed));
        for (const auto &[x, z] : {std::array{150, 40}, std::array{-600, 120}, std::array{40, 1150},
                                   std::array{2930, -60}, std::array{-200, -3500}, std::array{0, 4400}})
            expect(generator.generate_chunk(floor_chunk(x), floor_chunk(z)).hash() ==
                       retail.generate_chunk(floor_chunk(x), floor_chunk(z)).hash(),
                   std::format("seed {}: retail terrain at {}, {}", seed, x, z));
        // The ring: tall peaks under the top of the world, lakes of water.
        int highest = world::world_min_y;
        int lake_columns{};
        std::optional<std::array<int, 2>> lake;
        std::map<std::string, int> biomes;
        for (int z = 1650; z <= 2350; z += 6)
            for (int x = -700; x <= 700; x += 6)
            {
                const int height = terrain->surface_height(generator, x, z);
                highest = std::max(highest, height);
                if (height < definition.sea_level - 1)
                {
                    ++lake_columns;
                    lake = lake.value_or(std::array{x, z});
                }
                ++biomes[definition.biomes[terrain->biome_at(generator, x, z)].key];
            }
        expect(highest >= 55 && highest <= 61,
               std::format("seed {}: the peaks reach Y {} (55-61 wanted)", seed, highest));
        expect(lake_columns > 200, std::format("seed {}: {} lake columns", seed, lake_columns));
        for (const char *key : {"wilderness:jagged_peaks", "wilderness:stony_slopes", "wilderness:pine_forest",
                                "wilderness:alpine_meadow", "wilderness:lake_valley"})
            expect(biomes[key] > 20, std::format("seed {}: {} covers {} samples", seed, key, biomes[key]));
        if (lake.has_value())
        {
            const auto [x, z] = *lake;
            const world::Chunk chunk = generator.generate_chunk(floor_chunk(x), floor_chunk(z));
            const int local_x = x - floor_chunk(x) * world::chunk_width;
            const int local_z = z - floor_chunk(z) * world::chunk_depth;
            const int surface = definition.sea_level - world::world_min_y;
            expect(chunk.block(local_x, surface, local_z) == world::BlockType::ice &&
                       chunk.mod_cell(local_x, surface, local_z) == water,
                   std::format("seed {}: the lake at {}, {} is wilderness:water up to the sea level", seed, x, z));
            expect(chunk.block(local_x, surface + 1, local_z) == world::BlockType::empty,
                   std::format("seed {}: and air above it", seed));
            // Caves keep away from the lakes: no water hangs over or beside air.
            const int hanging = hanging_fluid(generator, definition.sea_level, floor_chunk(x) - 3,
                                              floor_chunk(z) - 3, floor_chunk(x) + 3, floor_chunk(z) + 3);
            expect(hanging == 0, std::format("seed {}: {} water cells around {}, {} hang over air", seed,
                                             hanging, x, z));
        }
        if (seed == 20261014)
        {
            // Two big lakes of this seed (the second one where caves ran under it).
            const int big = hanging_fluid(generator, definition.sea_level, floor_chunk(-420), floor_chunk(2040),
                                          floor_chunk(-300), floor_chunk(2180)) +
                            hanging_fluid(generator, definition.sea_level, floor_chunk(1740), floor_chunk(-340),
                                          floor_chunk(1910), floor_chunk(-170));
            expect(big == 0, std::format("seed {}: {} water cells of the big lakes hang over air", seed, big));
        }
        // Spruces: cones of their own blocks in the far landscape too.
        std::vector<world::TreeSite> sites;
        for (int chunk_z = floor_chunk(1800); chunk_z <= floor_chunk(2200); chunk_z += 3)
            for (int chunk_x = floor_chunk(-500); chunk_x <= floor_chunk(500); chunk_x += 3)
                generator.tree_sites(chunk_x, chunk_z, sites, true);
        const auto cones = std::count_if(sites.begin(), sites.end(), [&](const world::TreeSite &site) {
            return site.crown == world::TreeCrown::cone && site.trunk == world::BlockType::log &&
                   site.trunk_mod == spruce && site.crown_radius >= 2U;
        });
        expect(cones > 50, std::format("seed {}: {} of {} far tree sites are spruce cones", seed, cones,
                                       sites.size()));
    }
    // Retail's own tree sites keep the retail crown.
    const world::WorldGenerator retail{20261014, world::TerrainProfile::modern, GameMode::survival};
    std::vector<world::TreeSite> sites;
    for (int chunk_x = 0; chunk_x < 6; ++chunk_x)
        retail.tree_sites(chunk_x, 2, sites, true);
    expect(!sites.empty() && std::all_of(sites.begin(), sites.end(), [](const world::TreeSite &site) {
               return site.crown == world::TreeCrown::retail && site.trunk_mod == 0U && site.leaves_mod == 0U;
           }),
           "retail tree sites keep retail's crown");
}

// (fix, wave 5) The far landscape layers a column as the biome does: the peaks' one
// cell of snow over stone is a stone cliff with a white rim from afar too, not
// retail's four cells of snow down every step (which drew a grey "castle wall"
// with a white band above it along the snow line).
void the_far_landscape_layers_columns_like_the_land()
{
    Machine machine;
    const auto terrain = wilderness_terrain(machine);
    if (terrain == nullptr)
        return;
    const auto &profiles = terrain->far_profiles();
    const world::FarSurfaceProfile thin_snow{{world::BlockType::snow}, {world::BlockType::rock}, 1U};
    const auto snow = std::find(profiles.begin(), profiles.end(), thin_snow);
    expect(snow != profiles.end(), "the peaks' surface is a profile of one cell of snow over rock");
    expect(profiles.size() + terrain->definition().palette.size() <=
               static_cast<std::size_t>(world::far_mod_block_count),
           "profiles and palette share the far block bytes without meeting");
    if (snow == profiles.end())
        return;
    const auto snow_profile = static_cast<std::uint8_t>(snow - profiles.begin() + 1);
    world::WorldGenerator generator{20261014, world::TerrainProfile::modern, GameMode::survival};
    generator.set_custom_terrain(terrain);
    int snowy{};
    int stone_under{};
    for (int z = 1700; z <= 2300; z += 7)
        for (int x = -400; x <= 400; x += 7)
        {
            const world::SurfaceColumn surface = generator.surface_column(x, z, false);
            if (surface.top_profile == 0U)
                continue;
            expect(surface.top_profile <= profiles.size() &&
                       profiles[surface.top_profile - 1U].top.block == surface.top &&
                       profiles[surface.top_profile - 1U].top.mod == surface.top_mod,
                   std::format("the profile at {}, {} has the column's top block", x, z));
            if (surface.top_profile != snow_profile || surface.top_local_y < 1)
                continue;
            ++snowy;
            std::array<world::BlockType, world::chunk_height> cells{};
            generator.generate_column(x, z, cells);
            stone_under += cells[static_cast<std::size_t>(surface.top_local_y - 1)] != world::BlockType::snow;
        }
    expect(snowy > 50, std::format("{} snowy peak columns sampled", snowy));
    expect(stone_under * 10 >= snowy * 9,
           std::format("{} of {} snowy columns have no second cell of snow", stone_under, snowy));
    // A far tile of the peaks stores the profile's byte; retail's never names one.
    const world::FarTerrainSettings settings{2048, 1, 256};
    const world::FarSurfaceProfile *found = nullptr;
    for (int tile_z = 1700 / 64; tile_z <= 2300 / 64 && found == nullptr; ++tile_z)
        for (int tile_x = -6; tile_x <= 6 && found == nullptr; ++tile_x)
        {
            const world::FarTileSummary summary = world::summarize_far_tile(generator, settings, {0, tile_x, tile_z});
            if (std::find(summary.block.begin(), summary.block.end(), world::far_profile_byte(snow_profile)) !=
                summary.block.end())
                found = &*snow;
        }
    expect(found != nullptr, "a far tile of the peaks names the thin snow profile");
    const world::WorldGenerator retail{20261014, world::TerrainProfile::modern, GameMode::survival};
    for (const auto &key : {world::FarTileKey{0, 0, 0}, world::FarTileKey{0, 30, 4}, world::FarTileKey{2, -3, 9}})
    {
        const world::FarTileSummary summary = world::summarize_far_tile(retail, settings, key);
        expect(std::all_of(summary.block.begin(), summary.block.end(),
                           [](std::uint8_t block) { return block < world::far_mod_block_base; }),
               "retail far tiles hold retail blocks only");
    }
}

// --- Performance (RelWithDebInfo / Release) -----------------------------------------------

void milliseconds_per_chunk()
{
    const auto measure = [](const world::WorldGenerator &generator, int distance) {
        const auto started = std::chrono::steady_clock::now();
        constexpr int count = 48;
        std::uint64_t sink{};
        for (int index = 0; index < count; ++index)
            sink ^= generator.generate_chunk(floor_chunk(distance) + index % 8, index / 8).hash();
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        static_cast<void>(sink);
        return elapsed / count;
    };
    const world::WorldGenerator retail{20261014, world::TerrainProfile::modern, GameMode::survival};
    world::WorldGenerator sample{20261014, world::TerrainProfile::modern, GameMode::survival};
    sample.set_custom_terrain(sample_terrain());
    std::cout << "    ms/chunk (one thread):\n";
    for (const int distance : {150, 1500, 4000, 25000})
        std::cout << std::format("      at {:>5}: retail {:.2f}, sample generator {:.2f}\n", distance,
                                 measure(retail, distance), measure(sample, distance));
    // Frontier itself, built from its scripts.
    Machine machine;
    if (machine.runtime.start(modding::Side::server, {frontier_mod(false)}, {}))
    {
        std::string error;
        if (const auto frontier = machine.worldgen_service->build(modding::Side::server, error))
        {
            world::WorldGenerator generator{20261014, world::TerrainProfile::modern, GameMode::survival};
            generator.set_custom_terrain(frontier);
            for (const int distance : {300, 3000, 9000, 16000, 23000})
                std::cout << std::format("      Frontier at {:>5}: {:.2f}\n", distance, measure(generator, distance));
            // Far terrain: one 67 x 67 summary (a far tile) per sample.
            const auto started = std::chrono::steady_clock::now();
            for (int z = 0; z < 67; ++z)
                for (int x = 0; x < 67; ++x)
                    static_cast<void>(generator.surface_column(16000 + x * 4, z * 4, false));
            std::cout << std::format("      Frontier far tile (4489 columns): {:.1f} ms\n",
                                     std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
        }
    }
    // (wave 5) The Wilderness example: retail around it, its mountains in the ring.
    Machine wilderness;
    if (const auto terrain = wilderness_terrain(wilderness))
    {
        world::WorldGenerator generator{20261014, world::TerrainProfile::modern, GameMode::survival};
        generator.set_custom_terrain(terrain);
        for (const int distance : {300, 1500, 1800, 2000, 2200, 2900, 6800})
            std::cout << std::format("      Wilderness at {:>5}: {:.2f} (retail {:.2f})\n", distance,
                                     measure(generator, distance), measure(retail, distance));
        const auto started = std::chrono::steady_clock::now();
        for (int z = 0; z < 67; ++z)
            for (int x = 0; x < 67; ++x)
                static_cast<void>(generator.surface_column(x * 4 - 134, 1900 + z * 4, false));
        std::cout << std::format("      Wilderness far tile (4489 columns): {:.1f} ms\n",
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
    }
}

struct TestCase final
{
    const char *name;
    void (*run)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"retail_terrain_matches_its_golden_hashes", retail_terrain_matches_its_golden_hashes},
        {"without_a_generator_every_function_is_retail", without_a_generator_every_function_is_retail},
        {"a_chunks_mod_layer_follows_its_edits", a_chunks_mod_layer_follows_its_edits},
        {"a_broken_definition_is_refused_with_a_reason", a_broken_definition_is_refused_with_a_reason},
        {"the_layout_places_blends_and_mirrors_bands", the_layout_places_blends_and_mirrors_bands},
        {"every_thread_count_generates_the_same_chunks", every_thread_count_generates_the_same_chunks},
        {"far_columns_match_the_chunks", far_columns_match_the_chunks},
        {"reused_retail_terrain_is_retail_exactly", reused_retail_terrain_is_retail_exactly},
        {"the_digest_is_the_generator_not_its_presentation", the_digest_is_the_generator_not_its_presentation},
        {"scripts_define_a_world_natively", scripts_define_a_world_natively},
        {"a_client_mod_cannot_shape_the_world", a_client_mod_cannot_shape_the_world},
        {"a_bad_world_names_what_is_wrong", a_bad_world_names_what_is_wrong},
        {"generated_mod_blocks_have_their_identity_in_gameplay", generated_mod_blocks_have_their_identity_in_gameplay},
        {"host_and_joined_client_generate_the_same_world", host_and_joined_client_generate_the_same_world},
        {"a_world_records_its_generator", a_world_records_its_generator},
        {"big_distances_stay_exact", big_distances_stay_exact},
        {"wilderness_mountains_replace_the_retail_ring", wilderness_mountains_replace_the_retail_ring},
        {"the_far_landscape_layers_columns_like_the_land", the_far_landscape_layers_columns_like_the_land},
        {"milliseconds_per_chunk", milliseconds_per_chunk},
    };
    for (const TestCase &test : tests)
    {
        const int before = failures;
        try
        {
            test.run();
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "  FAIL: exception: " << error.what() << '\n';
        }
        std::cout << (failures == before ? "[PASS] " : "[FAIL] ") << test.name << '\n';
    }
    std::cout << (failures == 0 ? "all mod-world checks passed\n" : "mod-world checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
