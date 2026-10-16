// Block-for-block parity of the generated world against retail CastleMiner Z.
//
// Retail never streams base terrain. BlockTerrain.DoThreadedComputeBlocks runs
// the local CastleMinerZBuilder for the seed carried by WorldInfoMessage and
// only then applies the host's delta list, so a cell this generator places
// differently is a permanent disagreement between an OpenMinerZ peer and a
// stock peer, everywhere on the map and for the whole session.
//
// Modes:
//   (no arguments)                        check the pinned hashes below; needs
//                                         no game install.
//   --write-request <profile> <file> [quick|full|soak<N>]
//                                         write the chunk plan the original
//                                         TerrainParityProbe is asked to build
//                                         (soak<N>: N random chunks per world).
//   --compare <profile> <request> <retail.bin> [report]
//                                         diff our generator against the
//                                         original builder's output.
//   --pin <profile> <request> <retail.bin>
//                                         print the hashes of the ORIGINAL
//                                         output, for the table below.
//   --dump <profile> <request> <ours.bin> write our own blocks in the probe's
//                                         layout.
// <profile> is `classic` (1.6.3.8, protocol 3) or `modern` (1.9.9.8, protocol 4).
#include "openminerz/game_mode.hpp"
#include "openminerz/world/encounters.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using openminerz::GameMode;
using openminerz::world::BlockType;
using openminerz::world::Chunk;
using openminerz::world::TerrainProfile;
using openminerz::world::WorldGenerator;

constexpr int chunk_cells = 16 * 128 * 16;

struct PlannedWorld final
{
    std::int32_t seed{};
    int game_mode{};
    std::vector<std::array<int, 2>> chunks;
};

// One chunk in the original block array's own order: y + 128 * (x + 16 * z).
// Cells the generator never wrote are Empty, as they are after the original
// ReplaceRegion(NumberOfBlocks -> Empty).
void chunk_bytes(const Chunk &chunk, std::uint8_t *out)
{
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (int y = 0; y < 128; ++y)
            {
                const BlockType type = chunk.block(x, y, z);
                out[y + 128 * (x + 16 * z)] =
                    type == BlockType::uninitialized ? std::uint8_t{0}
                                                     : static_cast<std::uint8_t>(type);
            }
}

std::uint64_t fnv1a(const std::uint8_t *data, std::size_t size,
                    std::uint64_t hash = 1469598103934665603ULL) noexcept
{
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// The radial band a column belongs to, by the original builder's own distance
// fold (double square root narrowed to float, 4400-block mirror).
float folded_radius(int x, int z) noexcept
{
    float radius = static_cast<float>(std::sqrt(static_cast<double>(
        static_cast<std::int64_t>(x) * x + static_cast<std::int64_t>(z) * z)));
    int bands{};
    while (radius > 4400.0F)
    {
        radius -= 4400.0F;
        ++bands;
    }
    return (bands & 1) != 0 ? 4400.0F - radius : radius;
}

constexpr std::array<std::string_view, 11> band_names{
    "classic 0-200",        "classic>lagoon 200-300", "lagoon 300-900",
    "lagoon>desert 900-1k", "desert 1000-1600",       "desert>mountain 16-1700",
    "mountain 1700-2300",   "mountain>arctic 23-2400", "arctic 2400-3000",
    "descent 3000-3600",    "hell 3600-4400",
};

int band_of(int x, int z) noexcept
{
    const float radius = folded_radius(x, z);
    constexpr std::array<float, 10> limits{200.0F,  300.0F,  900.0F,  1000.0F, 1600.0F,
                                           1700.0F, 2300.0F, 2400.0F, 3000.0F, 3600.0F};
    for (std::size_t index = 0; index < limits.size(); ++index)
        if (radius < limits[index])
            return static_cast<int>(index);
    return 10;
}

constexpr std::array<std::string_view, 5> depth_names{
    "bedrock y0-3", "hell y4-31", "deep ore y32-49", "caves y50-65", "surface y66-127"};

int depth_of(int local_y) noexcept
{
    if (local_y < 4)
        return 0;
    if (local_y < 32)
        return 1;
    if (local_y < 50)
        return 2;
    if (local_y < 66)
        return 3;
    return 4;
}

TerrainProfile parse_profile(std::string_view text)
{
    if (text == "classic")
        return TerrainProfile::classic;
    if (text == "modern")
        return TerrainProfile::modern;
    std::cerr << "unknown profile " << text << '\n';
    std::exit(2);
}

std::vector<PlannedWorld> read_request(const std::string &path)
{
    std::ifstream input{path};
    if (!input)
    {
        std::cerr << "cannot read " << path << '\n';
        std::exit(2);
    }
    std::vector<PlannedWorld> worlds;
    std::string line;
    while (std::getline(input, line))
    {
        if (const auto comment = line.find('#'); comment != std::string::npos)
            line.resize(comment);
        std::istringstream tokens{line};
        std::string first;
        if (!(tokens >> first))
            continue;
        if (first == "world")
        {
            PlannedWorld world;
            tokens >> world.seed >> world.game_mode;
            worlds.push_back(std::move(world));
            continue;
        }
        int chunk_z{};
        tokens >> chunk_z;
        worlds.back().chunks.push_back({std::stoi(first), chunk_z});
    }
    return worlds;
}

void add_ray(PlannedWorld &world, double degrees, int from, int to, int step)
{
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    for (int radius = from; radius <= to; radius += step)
    {
        const int chunk_x = static_cast<int>(std::floor(radius * std::cos(radians) / 16.0));
        const int chunk_z = static_cast<int>(std::floor(radius * std::sin(radians) / 16.0));
        const std::array<int, 2> chunk{chunk_x, chunk_z};
        if (std::find(world.chunks.begin(), world.chunks.end(), chunk) == world.chunks.end())
            world.chunks.push_back(chunk);
    }
}

void add_square(PlannedWorld &world, int min_chunk, int max_chunk)
{
    for (int chunk_z = min_chunk; chunk_z <= max_chunk; ++chunk_z)
        for (int chunk_x = min_chunk; chunk_x <= max_chunk; ++chunk_x)
            world.chunks.push_back({chunk_x, chunk_z});
}

// The comparison plan. Every radial band and every transition between bands is
// crossed on several headings (all four sign quadrants, so negative coordinates
// and their chunk seams are in), the mirrored second 4400-block ring is
// reached, and a few chunks sit where 32-bit and float coordinate arithmetic
// stops being exact.
std::vector<PlannedWorld> make_plan(TerrainProfile profile, bool full)
{
    std::vector<PlannedWorld> plan;
    const auto world = [&](std::int32_t seed, int mode, bool dense) {
        PlannedWorld next;
        next.seed = seed;
        next.game_mode = mode;
        add_square(next, -4, 3);
        if (dense)
        {
            add_ray(next, 0.0, 0, 4500, 16);
            add_ray(next, 225.0, 0, 4500, 16);
            add_ray(next, 101.0, 0, 4500, 16);
            add_ray(next, 297.0, 0, 4500, 16);
            add_ray(next, 163.0, 4500, 9100, 64);
            add_ray(next, 45.0, 4500, 9100, 64);
            // x = 65536: the origin tower's 32-bit squared distance wraps to 0.
            for (const auto &far : std::array<std::array<int, 2>, 10>{{
                     {4096, 0},
                     {-4096, 0},
                     {4096, 4096},
                     {0, -4096},
                     {2896, 2896},
                     {6250, -4375},
                     {65536, 5},
                     {-1048576, 262144},
                     {134217727, 0},
                     {-134217728, -134217728},
                 }})
                next.chunks.push_back(far);
        }
        else
        {
            add_ray(next, 13.0, 0, 4500, full ? 32 : 96);
            add_ray(next, 148.0, 0, 4500, full ? 32 : 96);
            add_ray(next, 260.0, 0, 4500, full ? 32 : 96);
        }
        plan.push_back(std::move(next));
    };
    if (profile == TerrainProfile::classic)
    {
        world(1, 0, full);
        world(839880689, 0, true); // WorldInfo.CreateNewWorld's trial-mode seed
        if (full)
        {
            world(0, 0, false);
            world(-1, 0, false);
            world(12345, 0, false);
            world(1337, 0, false);
            world(2147483647, 0, false);
            world(-2147483647 - 1, 0, false);
        }
        else
        {
            world(-1, 0, false);
            world(-2147483647 - 1, 0, false);
        }
    }
    else
    {
        // Loot blocks depend on the game mode in Modern.
        world(839880689, 0, true);
        world(1, 5, full);
        world(12345, 1, false);
        world(-77, 4, false);
        world(1337, 3, false);
        world(2024, 2, false);
    }
    return plan;
}

// A soak plan: uniformly random chunks out to 9000 blocks, from a fixed
// generator so a failure can be reproduced. The rounding differences this suite
// guards against are roughly one-in-ten-million events per cell, so the
// structured plan alone would not prove much about them.
std::vector<PlannedWorld> make_soak_plan(TerrainProfile profile, int chunks_per_world)
{
    std::vector<PlannedWorld> plan;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    };
    const std::vector<std::pair<std::int32_t, int>> worlds =
        profile == TerrainProfile::classic
            ? std::vector<std::pair<std::int32_t, int>>{
                  {1, 0}, {839880689, 0}, {987654321, 0}, {-424242, 0}}
            : std::vector<std::pair<std::int32_t, int>>{
                  {839880689, 5}, {987654321, 1}, {-424242, 4}, {31337, 0}};
    for (const auto &[seed, mode] : worlds)
    {
        PlannedWorld world;
        world.seed = seed;
        world.game_mode = mode;
        for (int index = 0; index < chunks_per_world; ++index)
        {
            const int chunk_x = static_cast<int>(next() % 1126U) - 563;
            const int chunk_z = static_cast<int>(next() % 1126U) - 563;
            world.chunks.push_back({chunk_x, chunk_z});
        }
        plan.push_back(std::move(world));
    }
    return plan;
}

int write_request(TerrainProfile profile, const std::string &path, const std::string &kind)
{
    const std::vector<PlannedWorld> plan =
        kind.rfind("soak", 0) == 0
            ? make_soak_plan(profile, kind.size() > 4 ? std::stoi(kind.substr(4)) : 4000)
            : make_plan(profile, kind == "full");
    std::ofstream output{path, std::ios::trunc};
    std::size_t chunks{};
    for (const PlannedWorld &world : plan)
    {
        output << "world " << world.seed << ' ' << world.game_mode << '\n';
        for (const auto &chunk : world.chunks)
            output << chunk[0] << ' ' << chunk[1] << '\n';
        chunks += world.chunks.size();
    }
    std::cout << "wrote " << plan.size() << " worlds, " << chunks << " chunks to " << path << '\n';
    return output ? 0 : 1;
}

std::vector<std::uint8_t> read_binary(const std::string &path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
    {
        std::cerr << "cannot read " << path << '\n';
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

struct Tally final
{
    std::uint64_t cells{};
    std::uint64_t different{};
};

// Modern's CaveBiome and HellFloorBiome also place loot blocks and spawners,
// and those are NOT a function of seed and coordinate even in retail: CaveBiome
// counts carved cells in an instance field that runs across every chunk the
// builder has produced so far (so the result depends on chunk build order),
// picks the spawner type with the global MathTools.RandomInt, and HellFloorBiome
// counts hell-floor columns the same way. Two stock Modern clients in one
// session therefore already disagree about these cells. They are verified here
// by replaying the original counters over the native generator's recorded cave
// and hell-floor sites in the probe's chunk order: every such block must appear
// in exactly the predicted cell, and only the three-way spawner type may vary.
constexpr std::uint8_t no_overlay = 0xFF;
constexpr std::uint8_t any_enemy_spawner = 0xFE;
constexpr std::uint8_t loot_block = 69;
constexpr std::uint8_t lucky_loot_block = 70;
constexpr std::uint8_t enemy_spawn_off = 73;
constexpr std::uint8_t enemy_spawn_rare_off = 75;
constexpr std::uint8_t alien_spawn_off = 81;
constexpr std::uint8_t boss_spawn_off = 85;

struct StatefulReplay final
{
    openminerz::world::CaveEncounterRates rates{};
    std::uint64_t carved{};
    std::uint32_t boss_countdown{};
    std::uint32_t bosses_spawned{};
    std::uint32_t max_bosses{50};

    explicit StatefulReplay(GameMode mode)
        : rates(openminerz::world::cave_encounter_rates(mode)),
          boss_countdown(openminerz::world::hell_boss_interval(0))
    {
    }

    // Fills `cave` with the block CaveBiome leaves in a freshly carved cell and
    // `boss` with the cells HellFloorBiome.CheckForBossSpawns overwrites.
    void chunk(const Chunk &chunk, std::array<std::uint8_t, chunk_cells> &cave,
               std::array<std::uint8_t, chunk_cells> &boss)
    {
        cave.fill(no_overlay);
        boss.fill(no_overlay);
        const openminerz::world::ChunkGenerationSites *sites = chunk.generation_sites();
        if (sites == nullptr)
            return;
        const auto index_of = [&](const std::array<int, 3> &cell) {
            const int x = cell[0] - chunk.chunk_x() * 16;
            const int y = cell[1] - openminerz::world::world_min_y;
            const int z = cell[2] - chunk.chunk_z() * 16;
            return static_cast<std::size_t>(y + 128 * (x + 16 * z));
        };
        for (const openminerz::world::CaveGenerationSite &site : sites->caves)
        {
            const std::uint64_t count = carved + site.carved_index;
            std::uint8_t block = no_overlay;
            if (count % rates.loot == 0)
                block = loot_block;
            if (count % rates.enemy == 0)
                block = any_enemy_spawner;
            if (count % rates.lucky_loot == 0)
                block = lucky_loot_block;
            cave[index_of(site.cell)] = block;
        }
        carved += sites->carved_count;
        for (const std::array<int, 3> &cell : sites->hell_floor)
        {
            if (bosses_spawned >= max_bosses)
            {
                boss_countdown = 0;
                continue;
            }
            if (boss_countdown == 0)
                continue;
            if (--boss_countdown == 0)
            {
                ++bosses_spawned;
                boss_countdown = openminerz::world::hell_boss_interval(bosses_spawned);
                boss[index_of(cell)] = boss_spawn_off;
            }
        }
    }
};

int compare(TerrainProfile profile, const std::string &request_path, const std::string &retail_path,
            const std::string &report_path)
{
    const std::vector<PlannedWorld> plan = read_request(request_path);
    const std::vector<std::uint8_t> retail = read_binary(retail_path);
    std::size_t planned{};
    for (const PlannedWorld &world : plan)
        planned += world.chunks.size();
    if (retail.size() != planned * chunk_cells)
    {
        std::cerr << "retail dump holds " << retail.size() / chunk_cells << " chunks, plan has "
                  << planned << '\n';
        return 2;
    }
    std::array<std::array<Tally, 5>, 11> table{};
    std::map<std::pair<int, int>, std::uint64_t> confusion;
    std::vector<std::string> examples;
    std::vector<std::string> chunk_lines;
    std::uint64_t different_chunks{};
    std::array<std::uint8_t, chunk_cells> ours{};
    std::size_t offset{};
    std::array<std::uint8_t, chunk_cells> cave_overlay{};
    std::array<std::uint8_t, chunk_cells> boss_overlay{};
    std::uint64_t stateful_cells{};
    for (const PlannedWorld &world : plan)
    {
        const WorldGenerator generator{world.seed, profile, static_cast<GameMode>(world.game_mode)};
        StatefulReplay replay{static_cast<GameMode>(world.game_mode)};
        for (const auto &coordinate : world.chunks)
        {
            const Chunk chunk = generator.generate_chunk(coordinate[0], coordinate[1]);
            chunk_bytes(chunk, ours.data());
            if (profile == TerrainProfile::modern)
            {
                // What the original's counters put on top of the deterministic
                // terrain, in the order its passes run: the cave block only
                // survives where no later pass overwrote the carved cell, and
                // the hell floor's boss spawner replaces whatever it lands on
                // short of the origin tower.
                replay.chunk(chunk, cave_overlay, boss_overlay);
                for (std::size_t index = 0; index < ours.size(); ++index)
                {
                    if (boss_overlay[index] != no_overlay && ours[index] != 14 && ours[index] != 5)
                    {
                        ours[index] = boss_overlay[index];
                        ++stateful_cells;
                    }
                    else if (cave_overlay[index] != no_overlay && ours[index] == 0)
                    {
                        ours[index] = cave_overlay[index];
                        ++stateful_cells;
                    }
                }
            }
            const std::uint8_t *theirs = retail.data() + offset;
            offset += chunk_cells;
            // The spawner type is the one thing retail draws from a global RNG.
            const auto same = [](std::uint8_t expected, std::uint8_t actual) {
                return expected == actual ||
                       (expected == any_enemy_spawner &&
                        (actual == enemy_spawn_off || actual == enemy_spawn_rare_off ||
                         actual == alien_spawn_off));
            };
            bool chunk_differs{};
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                {
                    // 64-bit so the far-coordinate chunks classify sanely.
                    const std::int64_t wide_x = static_cast<std::int64_t>(coordinate[0]) * 16 + x;
                    const std::int64_t wide_z = static_cast<std::int64_t>(coordinate[1]) * 16 + z;
                    const int band = band_of(static_cast<int>(wide_x), static_cast<int>(wide_z));
                    for (int y = 0; y < 128; ++y)
                    {
                        const std::size_t index = static_cast<std::size_t>(y + 128 * (x + 16 * z));
                        Tally &tally = table[static_cast<std::size_t>(band)]
                                            [static_cast<std::size_t>(depth_of(y))];
                        ++tally.cells;
                        if (same(ours[index], theirs[index]))
                            continue;
                        ++tally.different;
                        chunk_differs = true;
                        ++confusion[{ours[index], theirs[index]}];
                        if (examples.size() < 40)
                        {
                            std::ostringstream text;
                            text << "seed " << world.seed << " mode " << world.game_mode << " x "
                                 << wide_x << " y " << y << " z " << wide_z << " ours "
                                 << static_cast<int>(ours[index]) << " retail "
                                 << static_cast<int>(theirs[index]) << " band "
                                 << band_names[static_cast<std::size_t>(band)];
                            examples.push_back(text.str());
                        }
                    }
                }
            if (chunk_differs)
            {
                ++different_chunks;
                std::uint64_t cells{};
                for (std::size_t index = 0; index < ours.size(); ++index)
                    cells += same(ours[index], theirs[index]) ? 0U : 1U;
                if (chunk_lines.size() < 64)
                {
                    std::ostringstream text;
                    text << "seed " << world.seed << " mode " << world.game_mode << " chunk "
                         << coordinate[0] << ',' << coordinate[1] << " : " << cells << " cells";
                    chunk_lines.push_back(text.str());
                }
            }
        }
    }

    std::ostringstream report;
    std::uint64_t total_cells{};
    std::uint64_t total_different{};
    report << "profile " << (profile == TerrainProfile::classic ? "classic" : "modern") << ", "
           << plan.size() << " worlds, " << planned << " chunks\n";
    report << "percentage of cells that differ from retail (band x depth)\n";
    char line[256];
    std::snprintf(line, sizeof line, "%-26s", "band");
    report << line;
    for (const std::string_view depth : depth_names)
    {
        std::snprintf(line, sizeof line, " %16.*s", static_cast<int>(depth.size()), depth.data());
        report << line;
    }
    report << "            all\n";
    for (std::size_t band = 0; band < table.size(); ++band)
    {
        std::snprintf(line, sizeof line, "%-26.*s", static_cast<int>(band_names[band].size()),
                      band_names[band].data());
        report << line;
        Tally row;
        for (const Tally &tally : table[band])
        {
            row.cells += tally.cells;
            row.different += tally.different;
            std::snprintf(line, sizeof line, " %15.4f%%",
                          tally.cells == 0
                              ? 0.0
                              : 100.0 * static_cast<double>(tally.different) /
                                    static_cast<double>(tally.cells));
            report << line;
        }
        std::snprintf(line, sizeof line, " %13.4f%%  (%llu of %llu)\n",
                      row.cells == 0 ? 0.0
                                     : 100.0 * static_cast<double>(row.different) /
                                           static_cast<double>(row.cells),
                      static_cast<unsigned long long>(row.different),
                      static_cast<unsigned long long>(row.cells));
        report << line;
        total_cells += row.cells;
        total_different += row.different;
    }
    std::snprintf(line, sizeof line, "TOTAL %llu of %llu cells differ (%.6f%%) in %llu of %llu chunks\n",
                  static_cast<unsigned long long>(total_different),
                  static_cast<unsigned long long>(total_cells),
                  total_cells == 0 ? 0.0
                                   : 100.0 * static_cast<double>(total_different) /
                                         static_cast<double>(total_cells),
                  static_cast<unsigned long long>(different_chunks),
                  static_cast<unsigned long long>(planned));
    report << line;
    if (profile == TerrainProfile::modern)
        report << stateful_cells
               << " counter-placed loot/spawner cells replayed from the generator's cave and "
                  "hell-floor sites (position exact; spawner type is random in retail)\n";
    if (!confusion.empty())
    {
        std::vector<std::pair<std::uint64_t, std::pair<int, int>>> ranked;
        for (const auto &[pair, count] : confusion)
            ranked.push_back({count, pair});
        std::sort(ranked.rbegin(), ranked.rend());
        report << "most frequent (ours -> retail) block ids:\n";
        for (std::size_t index = 0; index < ranked.size() && index < 24; ++index)
            report << "  " << ranked[index].second.first << " -> " << ranked[index].second.second
                   << " : " << ranked[index].first << '\n';
        report << "differing chunks:\n";
        for (const std::string &chunk_line : chunk_lines)
            report << "  " << chunk_line << '\n';
        report << "first differing cells:\n";
        for (const std::string &example : examples)
            report << "  " << example << '\n';
    }
    std::cout << report.str();
    if (!report_path.empty())
    {
        std::ofstream output{report_path, std::ios::trunc};
        output << report.str();
    }
    return total_different == 0 ? 0 : 1;
}

int dump(TerrainProfile profile, const std::string &request_path, const std::string &output_path)
{
    const std::vector<PlannedWorld> plan = read_request(request_path);
    std::ofstream output{output_path, std::ios::binary | std::ios::trunc};
    std::array<std::uint8_t, chunk_cells> bytes{};
    for (const PlannedWorld &world : plan)
    {
        const WorldGenerator generator{world.seed, profile, static_cast<GameMode>(world.game_mode)};
        for (const auto &coordinate : world.chunks)
        {
            chunk_bytes(generator.generate_chunk(coordinate[0], coordinate[1]), bytes.data());
            output.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
    }
    return output ? 0 : 1;
}

// ---- pinned hashes -------------------------------------------------------
// FNV-1a over the ORIGINAL builder's output for a fixed chunk list per world,
// produced by `--pin` from the retail dump. They hold on a machine without the
// game, so a change that moves a single generated block fails the plain suite.
struct Pin final
{
    TerrainProfile profile;
    std::int32_t seed;
    int game_mode;
    std::uint64_t hash;
};

// The chunk list every pin covers: the spawn area and origin tower, every band
// and transition along two headings, the mirrored ring and a far chunk.
std::vector<std::array<int, 2>> pinned_chunks()
{
    PlannedWorld world;
    add_square(world, -2, 1);
    add_ray(world, 37.0, 0, 4500, 112);
    add_ray(world, 211.0, 56, 4500, 112);
    add_ray(world, 118.0, 4500, 8900, 400);
    world.chunks.push_back({4096, 0});
    world.chunks.push_back({-1048576, 262144});
    // Chunks in which a generator that rounds each step to float (instead of
    // following retail's x87 evaluation) is known to place a different block
    // for at least one pinned seed: the descent lerp that lands on an integer
    // along z = 0, a cave cell at the -0.35 threshold in the spawn area, the
    // lagoon overhang threshold inside a transition. Without these the pins
    // could all survive someone "tidying" a cast in world.cpp.
    for (const auto &sensitive : std::array<std::array<int, 2>, 8>{{
             {188, 0},
             {189, 0},
             {190, 0},
             {191, 0},
             {3, -1},
             {60, -119},
             {60, 13},
             {-23, -129},
         }})
        world.chunks.push_back(sensitive);
    return world.chunks;
}

std::uint64_t world_hash(TerrainProfile profile, std::int32_t seed, int game_mode)
{
    const WorldGenerator generator{seed, profile, static_cast<GameMode>(game_mode)};
    std::array<std::uint8_t, chunk_cells> bytes{};
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &coordinate : pinned_chunks())
    {
        chunk_bytes(generator.generate_chunk(coordinate[0], coordinate[1]), bytes.data());
        hash = fnv1a(bytes.data(), bytes.size(), hash);
    }
    return hash;
}

const std::vector<Pin> &pins()
{
    static const std::vector<Pin> table{
        // PINS-BEGIN (printed by --pin after the original builder's dump for
        // these chunks compared clean: Classic 1.6.3.8 and Modern 1.9.9.8).
        // Seeds 1 and -1 share a hash because System.Random seeds from
        // Math.Abs(seed); retail generates the same world for both.
        {TerrainProfile::classic, 1, 0, 0x450c3760e9edc52fULL},
        {TerrainProfile::classic, 839880689, 0, 0x01784a6d844e3beaULL},
        {TerrainProfile::classic, -1, 0, 0x450c3760e9edc52fULL},
        {TerrainProfile::classic, 12345, 0, 0x506df4f8b5a803a5ULL},
        {TerrainProfile::classic, -2147483647 - 1, 0, 0x49ccf39076f0dcfbULL},
        {TerrainProfile::modern, 839880689, 0, 0x2b72e04d79e48875ULL},
        {TerrainProfile::modern, 1, 5, 0x89a18ed3cd58f31cULL},
        {TerrainProfile::modern, 12345, 1, 0x567314a27c1ece5bULL},
        {TerrainProfile::modern, -77, 4, 0x77161bb8fe1a9847ULL},
        {TerrainProfile::modern, 1337, 3, 0xfa60aef69e857c1eULL},
        {TerrainProfile::modern, 2024, 2, 0xa53fdbb03ae0b59eULL},
        // PINS-END
    };
    return table;
}

std::vector<std::pair<std::int32_t, int>> pinned_worlds(TerrainProfile profile)
{
    if (profile == TerrainProfile::classic)
        return {{1, 0}, {839880689, 0}, {-1, 0}, {12345, 0}, {-2147483647 - 1, 0}};
    return {{839880689, 0}, {1, 5}, {12345, 1}, {-77, 4}, {1337, 3}, {2024, 2}};
}

int write_pin_request(TerrainProfile profile, const std::string &path)
{
    std::ofstream output{path, std::ios::trunc};
    for (const auto &[seed, mode] : pinned_worlds(profile))
    {
        output << "world " << seed << ' ' << mode << '\n';
        for (const auto &chunk : pinned_chunks())
            output << chunk[0] << ' ' << chunk[1] << '\n';
    }
    return output ? 0 : 1;
}

// Prints table rows for the pinned worlds, but only once the ORIGINAL builder's
// dump for exactly those chunks has compared clean -- a pin is a retail fact,
// never a snapshot of whatever the native generator happens to produce. The
// hash is taken over the native bytes, which that comparison has just shown to
// be the original's (in Modern, minus the counter-placed blocks whose spawner
// type retail randomises and which therefore cannot be pinned at all).
int print_pins(TerrainProfile profile, const std::string &request_path,
               const std::string &retail_path)
{
    if (compare(profile, request_path, retail_path, "") != 0)
    {
        std::cerr << "refusing to pin: the native generator does not match this retail dump\n";
        return 1;
    }
    const std::vector<PlannedWorld> plan = read_request(request_path);
    const std::vector<std::array<int, 2>> expected = pinned_chunks();
    for (const PlannedWorld &world : plan)
    {
        if (world.chunks != expected)
        {
            std::cerr << "refusing to pin: request is not the --write-pin-request plan\n";
            return 2;
        }
        std::printf("        {TerrainProfile::%s, %s, %d, 0x%016llxULL},\n",
                    profile == TerrainProfile::classic ? "classic" : "modern",
                    world.seed == -2147483647 - 1 ? "-2147483647 - 1"
                                                  : std::to_string(world.seed).c_str(),
                    world.game_mode,
                    static_cast<unsigned long long>(
                        world_hash(profile, world.seed, world.game_mode)));
    }
    return 0;
}

int check_pins()
{
    int failures{};
    for (const Pin &pin : pins())
    {
        const std::uint64_t ours = world_hash(pin.profile, pin.seed, pin.game_mode);
        const bool ok = ours == pin.hash;
        if (!ok)
            ++failures;
        std::printf("%s %s seed %d mode %d retail %016llx ours %016llx\n", ok ? "PASS" : "FAIL",
                    pin.profile == TerrainProfile::classic ? "classic" : "modern", pin.seed,
                    pin.game_mode, static_cast<unsigned long long>(pin.hash),
                    static_cast<unsigned long long>(ours));
    }
    if (pins().empty())
    {
        std::puts("FAIL no retail terrain hashes are pinned");
        return 1;
    }
    std::printf("%s: %zu retail terrain hashes\n", failures == 0 ? "PASS" : "FAIL", pins().size());
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    if (args.empty())
        return check_pins();
    if (args[0] == "--write-request" && args.size() >= 3)
        return write_request(parse_profile(args[1]), args[2], args.size() > 3 ? args[3] : "quick");
    if (args[0] == "--write-pin-request" && args.size() == 3)
        return write_pin_request(parse_profile(args[1]), args[2]);
    if (args[0] == "--compare" && args.size() >= 4)
        return compare(parse_profile(args[1]), args[2], args[3], args.size() > 4 ? args[4] : "");
    if (args[0] == "--pin" && args.size() == 4)
        return print_pins(parse_profile(args[1]), args[2], args[3]);
    if (args[0] == "--dump" && args.size() == 4)
        return dump(parse_profile(args[1]), args[2], args[3]);
    std::cerr << "usage: see the header of tests/test_terrain_parity.cpp\n";
    return 2;
}
