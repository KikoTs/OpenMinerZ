// World edits for scripts: budgeted edit jobs, the mod-block overlay,
// schematics (structures, dungeons), raycasts and the world-generation pass.
//
// Every change goes through GameplayBackend::set_block, which is the same
// authoritative path a player's own edit takes. That is the whole sync story
// for mod structures: the host's chunk snapshot, AlterBlockMessage and the
// world save all carry plain retail blocks, so a joining or stock peer gets
// them as ordinary modified-chunk data and never runs mod generation itself.
// A mod block is its retail fallback in all of those; its identity is the
// overlay below, kept in the sidecar and replicated over the mod channel.

#include "internal.hpp"

#include "openminerz/world/block.hpp"
#include "openminerz/world/custom_terrain.hpp" // mod-world

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr int lowest_y = -64;
constexpr int highest_y = 63;
constexpr int schematic_axis_limit = 256;
constexpr std::size_t schematic_cell_limit = 1U << 20U;

[[nodiscard]] int floor_div(int value, int divisor) noexcept
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

[[nodiscard]] std::uint32_t mix(std::uint32_t value) noexcept
{
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    return value;
}

} // namespace

// ------------------------------------------------------------ schematics

bool parse_schematic(std::string_view text, Schematic &schematic, std::string &error)
{
    schematic = {};
    std::map<char, std::uint16_t> palette;
    std::vector<std::vector<std::string>> layers;
    bool origin_set{};
    std::size_t line_number{};
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1U);
        const auto fail = [&](std::string_view why) {
            error = std::format("line {}: {}", line_number, why);
            return false;
        };
        if (line.starts_with("#"))
        {
            // A row may start with '#' too, once a layer is open and '#' is
            // in the palette.
            if (layers.empty() || !palette.contains('#'))
                continue;
        }
        if (line.starts_with("origin "))
        {
            std::array<int, 3> axes{};
            std::string_view rest = line.substr(7U);
            for (int &axis : axes)
            {
                while (!rest.empty() && rest.front() == ' ')
                    rest.remove_prefix(1U);
                const auto [end_of_number, failure] =
                    std::from_chars(rest.data(), rest.data() + rest.size(), axis);
                if (failure != std::errc{} || std::abs(axis) > schematic_axis_limit)
                    return fail("origin needs three integers");
                rest.remove_prefix(static_cast<std::size_t>(end_of_number - rest.data()));
            }
            schematic.origin = {axes[0], axes[1], axes[2]};
            origin_set = true;
            continue;
        }
        if (line.starts_with("palette "))
        {
            if (line.size() < 11U || line[9] != ' ')
                return fail("palette needs a character and a block: palette # rock");
            const char key = line[8];
            if (key == ' ')
                return fail("a space always means 'leave the cell as it is'");
            std::string_view block = line.substr(10U);
            while (!block.empty() && block.back() == ' ')
                block.remove_suffix(1U);
            if (block.empty() || block.size() > 128U)
                return fail("palette needs a block name");
            if (palette.contains(key))
                return fail("that palette character is already used");
            if (schematic.palette.size() >= 64U)
                return fail("a palette holds at most 64 entries");
            schematic.palette.emplace_back(block);
            palette[key] = static_cast<std::uint16_t>(schematic.palette.size());
            continue;
        }
        if (line == "layer")
        {
            if (layers.size() >= static_cast<std::size_t>(schematic_axis_limit))
                return fail("too many layers");
            layers.emplace_back();
            continue;
        }
        if (layers.empty())
        {
            if (line.find_first_not_of(' ') == std::string_view::npos)
                continue;
            return fail("rows come after a 'layer' line");
        }
        if (line.size() > static_cast<std::size_t>(schematic_axis_limit) ||
            layers.back().size() >= static_cast<std::size_t>(schematic_axis_limit))
            return fail("a layer is at most 256 x 256");
        for (const char character : line)
            if (character != ' ' && !palette.contains(character))
                return fail(std::format("'{}' is not in the palette", character));
        layers.back().emplace_back(line);
    }
    if (layers.empty())
    {
        error = "a schematic needs at least one layer";
        return false;
    }
    schematic.size_y = static_cast<int>(layers.size());
    for (const auto &layer : layers)
    {
        schematic.size_z = std::max(schematic.size_z, static_cast<int>(layer.size()));
        for (const std::string &row : layer)
            schematic.size_x = std::max(schematic.size_x, static_cast<int>(row.size()));
    }
    if (schematic.size_x == 0 || schematic.size_z == 0)
    {
        error = "a schematic needs at least one cell";
        return false;
    }
    const std::size_t cells = static_cast<std::size_t>(schematic.size_x) *
                              static_cast<std::size_t>(schematic.size_y) *
                              static_cast<std::size_t>(schematic.size_z);
    if (cells > schematic_cell_limit)
    {
        error = "a schematic holds at most 1048576 cells";
        return false;
    }
    schematic.cells.assign(cells, 0U);
    for (int y = 0; y < schematic.size_y; ++y)
        for (int z = 0; z < static_cast<int>(layers[static_cast<std::size_t>(y)].size()); ++z)
        {
            const std::string &row = layers[static_cast<std::size_t>(y)][static_cast<std::size_t>(z)];
            for (int x = 0; x < static_cast<int>(row.size()); ++x)
                if (row[static_cast<std::size_t>(x)] != ' ')
                    schematic.cells[(static_cast<std::size_t>(y) * schematic.size_z +
                                     static_cast<std::size_t>(z)) *
                                        schematic.size_x +
                                    static_cast<std::size_t>(x)] =
                        palette[row[static_cast<std::size_t>(x)]];
        }
    if (!origin_set)
        schematic.origin = {0, 0, 0};
    return true;
}

BlockPos schematic_cell(const Schematic &schematic, BlockPos anchor, int x, int y, int z,
                        int quarter_turns) noexcept
{
    int relative_x = x - schematic.origin.x;
    int relative_z = z - schematic.origin.z;
    const int turns = ((quarter_turns % 4) + 4) % 4;
    for (int turn = 0; turn < turns; ++turn)
    {
        // +X -> +Z -> -X -> -Z
        const int rotated_x = -relative_z;
        relative_z = relative_x;
        relative_x = rotated_x;
    }
    return {anchor.x + relative_x, anchor.y + (y - schematic.origin.y), anchor.z + relative_z};
}

Schematic GameplayService::Impl::schematic_from_table(const Value &table) const
{
    // Re-expressed as the text form, so both share one parser and its limits.
    std::string text;
    if (const Value *origin = table.field("origin"); origin != nullptr && !origin->is_nil())
    {
        const BlockPos cell = to_cell(*origin, "origin");
        text += std::format("origin {} {} {}\n", cell.x, cell.y, cell.z);
    }
    const Value *palette = table.field("palette");
    if (palette == nullptr || palette->type() != ValueType::map)
        throw ScriptError{"a structure needs palette = {[\"#\"] = \"rock\", ...}"};
    for (const auto &[key, block] : palette->as_map())
    {
        if (key.size() != 1U)
            throw ScriptError{std::format("palette key '{}' must be one character", key)};
        text += std::format("palette {} {}\n", key, block.as_string());
    }
    const Value *layers = table.field("layers");
    if (layers == nullptr || layers->type() != ValueType::array)
        throw ScriptError{"a structure needs layers = {{\"###\", ...}, ...} (bottom layer first)"};
    for (const Value &layer : layers->as_array())
    {
        text += "layer\n";
        for (const Value &row : layer.as_array())
        {
            text += row.as_string();
            text += '\n';
        }
    }
    Schematic schematic;
    std::string error;
    if (!parse_schematic(text, schematic, error))
        throw ScriptError{std::format("structure: {}", error)};
    return schematic;
}

// --------------------------------------------------------------- cells

std::optional<std::uint8_t> GameplayService::Impl::block_at(BlockPos cell) const
{
    if (cell.y < lowest_y || cell.y > highest_y)
        return std::nullopt;
    return backend.block(cell);
}

std::uint16_t GameplayService::Impl::mod_block_at(BlockPos cell, std::uint8_t current)
{
    const auto found = overlay.find(cell);
    if (found == overlay.end())
        return generated_block_at(cell, current, false); // mod-world
    const BlockDef *definition = server_registry.block(found->second);
    if (definition == nullptr || definition->fallback != current)
    {
        // Something replaced the fallback behind our back (a dig, a blast).
        forget_overlay(cell);
        return 0U;
    }
    return found->second;
}

// mod-world: a mod block a world generator put in the cell (every peer
// derives those itself; they are never in the overlay). `changed`: the cell
// was just dug or blown up, and `current` is what it held.
std::uint16_t GameplayService::Impl::generated_block_at(BlockPos cell, std::uint8_t current,
                                                        bool changed) const
{
    const std::string key = backend.generated_block(cell, changed);
    if (key.empty())
        return 0U;
    const BlockDef *definition = server_registry.block(key);
    return definition != nullptr && definition->fallback == current ? definition->id
                                                                    : std::uint16_t{0U};
}

void GameplayService::Impl::forget_overlay(BlockPos cell)
{
    if (overlay.erase(cell) != 0U)
        overlay_outbox.emplace_back(cell, std::uint16_t{0U});
}

bool GameplayService::Impl::set_cell(BlockPos cell, std::uint8_t block, std::uint16_t mod_block)
{
    if (cell.y < lowest_y || cell.y > highest_y || std::abs(cell.x) > 1'000'000 ||
        std::abs(cell.z) > 1'000'000)
        return false;
    const std::optional<std::uint8_t> current = backend.block(cell);
    if (!current.has_value())
        return false;
    const auto marked = overlay.find(cell);
    const std::uint16_t current_mod = marked != overlay.end() ? marked->second : 0U;
    if (*current == block && current_mod == mod_block)
        return false;
    if (*current != block && !backend.set_block(cell, block))
        return false;
    if (mod_block != 0U)
    {
        if (overlay.size() >= limits.max_overlay_cells && marked == overlay.end())
            log({}, LogLevel::warning, "gameplay: the mod block overlay is full");
        else
        {
            overlay[cell] = mod_block;
            overlay_outbox.emplace_back(cell, mod_block);
        }
    }
    else if (marked != overlay.end())
        forget_overlay(cell);
    return true;
}

Value GameplayService::Impl::overlay_value(
    const std::vector<std::pair<BlockPos, std::uint16_t>> &cells) const
{
    Array list;
    list.reserve(cells.size());
    for (const auto &[cell, id] : cells)
    {
        const BlockDef *definition = server_registry.block(id);
        list.push_back(make_map({{"x", cell.x},
                                 {"y", cell.y},
                                 {"z", cell.z},
                                 {"b", definition != nullptr ? definition->key : std::string{}}}));
    }
    return make_map({{"t", "overlay"}, {"cells", Value{std::move(list)}}});
}

void GameplayService::Impl::flush_overlay()
{
    if (overlay_outbox.empty())
        return;
    for (std::size_t offset{}; offset < overlay_outbox.size(); offset += 256U)
    {
        const std::vector<std::pair<BlockPos, std::uint16_t>> part{
            overlay_outbox.begin() + static_cast<std::ptrdiff_t>(offset),
            overlay_outbox.begin() +
                static_cast<std::ptrdiff_t>(std::min(overlay_outbox.size(), offset + 256U))};
        out().to_all(overlay_value(part), true);
    }
    overlay_outbox.clear();
}

void GameplayService::Impl::sweep_overlay()
{
    // A slow round-robin check that every marked cell still holds its
    // fallback; edits this service never heard about end the identity.
    std::size_t checked{};
    auto cursor = overlay.lower_bound(overlay_cursor);
    while (checked < 512U && !overlay.empty())
    {
        if (cursor == overlay.end())
            cursor = overlay.begin();
        const BlockPos cell = cursor->first;
        const std::uint16_t id = cursor->second;
        ++cursor;
        ++checked;
        const BlockDef *definition = server_registry.block(id);
        const auto current = backend.block(cell);
        if (definition == nullptr || !current.has_value() || *current != definition->fallback)
        {
            forget_overlay(cell);
            cursor = overlay.upper_bound(cell);
        }
        if (checked >= overlay.size())
            break;
    }
    overlay_cursor = cursor == overlay.end() ? BlockPos{std::numeric_limits<int>::min(), 0, 0}
                                             : cursor->first;
}

// ---------------------------------------------------------------- jobs

std::uint64_t GameplayService::Impl::queue_job(std::string_view mod, std::vector<EditCell> cells,
                                               Callback on_done)
{
    if (cells.size() > limits.edit_cells_per_call)
        throw ScriptError{std::format("an edit may touch at most {} cells", limits.edit_cells_per_call)};
    if (edit_jobs.size() >= 256U)
        throw ScriptError{"too many world edits are queued; wait for them to finish"};
    EditJob job;
    job.id = next_job_id++;
    job.mod = std::string{mod};
    job.cells = std::move(cells);
    job.on_done = on_done;
    edit_jobs.push_back(std::move(job));
    // Small edits happen at once, so a script can read back what it wrote.
    if (edit_jobs.size() == 1U && edit_jobs.front().cells.size() <= 64U)
        run_jobs();
    return next_job_id - 1U;
}

void GameplayService::Impl::run_jobs()
{
    std::size_t changed_budget = limits.edits_per_tick;
    std::size_t examined_budget = limits.edits_per_tick * 8U;
    while (!edit_jobs.empty() && changed_budget > 0U && examined_budget > 0U)
    {
        EditJob &job = edit_jobs.front();
        while (job.next < job.cells.size() && changed_budget > 0U && examined_budget > 0U)
        {
            const EditCell &edit = job.cells[job.next++];
            --examined_budget;
            if (edit.conditional)
            {
                const auto current = block_at(edit.cell);
                if (!current.has_value() || *current != edit.from_block)
                    continue;
                const auto marked = overlay.find(edit.cell);
                const std::uint16_t current_mod = marked != overlay.end() ? marked->second : 0U;
                if (current_mod != edit.from_mod_block)
                    continue;
            }
            if (set_cell(edit.cell, edit.block, edit.mod_block))
            {
                ++job.changed;
                --changed_budget;
            }
        }
        if (job.next < job.cells.size())
            break;
        const Callback done = job.on_done;
        const std::uint64_t id = job.id;
        const std::size_t changed = job.changed;
        const std::string mod = job.mod;
        edit_jobs.pop_front();
        if (done.valid())
        {
            const std::array<Value, 2> call{Value{static_cast<std::int64_t>(id)},
                                            Value{static_cast<std::int64_t>(changed)}};
            static_cast<void>(host.invoke(done, call));
            host.release(done);
        }
        emit(Side::server, "world.edit_done",
             make_map({{"job", static_cast<std::int64_t>(id)}, {"changed", static_cast<std::int64_t>(changed)},
                       {"mod", mod}}));
    }
}

std::uint64_t GameplayService::Impl::place_structure(std::string_view mod,
                                                     const Schematic &schematic, BlockPos at,
                                                     int quarter_turns, bool include_air,
                                                     Callback on_done)
{
    std::vector<BlockSpec> palette;
    palette.reserve(schematic.palette.size());
    for (const std::string &entry : schematic.palette)
        palette.push_back(resolve_block(Value{entry}, mod, "structure palette"));
    std::vector<EditCell> cells;
    for (int y = 0; y < schematic.size_y; ++y)
        for (int z = 0; z < schematic.size_z; ++z)
            for (int x = 0; x < schematic.size_x; ++x)
            {
                const std::uint16_t index =
                    schematic.cells[(static_cast<std::size_t>(y) * schematic.size_z +
                                     static_cast<std::size_t>(z)) *
                                        schematic.size_x +
                                    static_cast<std::size_t>(x)];
                if (index == 0U)
                    continue;
                const BlockSpec &spec = palette[index - 1U];
                if (spec.block == 0U && !include_air)
                    continue;
                EditCell edit;
                edit.cell = schematic_cell(schematic, at, x, y, z, quarter_turns);
                edit.block = spec.block;
                edit.mod_block = spec.mod_block;
                cells.push_back(edit);
            }
    return queue_job(mod, std::move(cells), on_done);
}

// ---------------------------------------------------------------- queries

std::optional<std::pair<BlockPos, BlockPos>> GameplayService::Impl::raycast(Vec3 origin,
                                                                            Vec3 direction,
                                                                            float reach) const
{
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                     direction.z * direction.z);
    if (!(length > 1e-9) || !(reach > 0.0F))
        return std::nullopt;
    const std::array<double, 3> d{direction.x / length, direction.y / length, direction.z / length};
    const std::array<double, 3> o{origin.x, origin.y, origin.z};
    std::array<int, 3> cell{static_cast<int>(std::floor(o[0])), static_cast<int>(std::floor(o[1])),
                            static_cast<int>(std::floor(o[2]))};
    std::array<int, 3> step{};
    std::array<double, 3> next{};
    std::array<double, 3> delta{};
    for (std::size_t axis = 0U; axis < 3U; ++axis)
    {
        step[axis] = d[axis] > 0.0 ? 1 : d[axis] < 0.0 ? -1 : 0;
        delta[axis] = step[axis] != 0 ? std::abs(1.0 / d[axis]) : 1e30;
        const double boundary = step[axis] > 0 ? std::floor(o[axis]) + 1.0 : std::floor(o[axis]);
        next[axis] = step[axis] != 0 ? (boundary - o[axis]) / d[axis] : 1e30;
    }
    std::array<int, 3> previous = cell;
    double travelled{};
    for (int steps = 0; steps < 1024 && travelled <= reach; ++steps)
    {
        const BlockPos here{cell[0], cell[1], cell[2]};
        if (const auto block = block_at(here); block.has_value() && *block != 0U && steps > 0)
            return std::pair{here, BlockPos{previous[0], previous[1], previous[2]}};
        previous = cell;
        const std::size_t axis = next[0] < next[1] ? (next[0] < next[2] ? 0U : 2U)
                                                   : (next[1] < next[2] ? 1U : 2U);
        travelled = next[axis];
        next[axis] += delta[axis];
        cell[axis] += step[axis];
    }
    return std::nullopt;
}

std::optional<int> GameplayService::Impl::surface_y(int x, int z) const
{
    for (int y = highest_y; y >= lowest_y; --y)
    {
        const auto block = block_at({x, y, z});
        if (!block.has_value())
            return std::nullopt;
        if (world::blocks_player(static_cast<world::BlockType>(*block)))
            return y;
    }
    return std::nullopt;
}

// ------------------------------------------------------- world generation

void GameplayService::Impl::queue_worldgen()
{
    if (!worldgen_enabled)
        return;
    const int radius = std::clamp(limits.worldgen_radius_chunks, 1, 32);
    for (const PlayerRecord &player : players)
    {
        if (!player.present)
            continue;
        const int centre_x = floor_div(static_cast<int>(std::floor(player.position.x)), 16);
        const int centre_z = floor_div(static_cast<int>(std::floor(player.position.z)), 16);
        // Nearest rings first, so what a player walks toward is ready first.
        for (int ring = 0; ring <= radius; ++ring)
            for (int dz = -ring; dz <= ring; ++dz)
                for (int dx = -ring; dx <= ring; ++dx)
                {
                    if (std::max(std::abs(dx), std::abs(dz)) != ring)
                        continue;
                    const std::pair chunk{centre_x + dx, centre_z + dz};
                    if (decorated_chunks.contains(chunk) || worldgen_queued.contains(chunk) ||
                        worldgen_queue.size() >= 8192U)
                        continue;
                    worldgen_queue.push_back(chunk);
                    worldgen_queued.insert(chunk);
                }
    }
}

void GameplayService::Impl::run_worldgen()
{
    for (std::size_t done{}; done < limits.worldgen_chunks_per_tick && !worldgen_queue.empty(); ++done)
    {
        const auto [chunk_x, chunk_z] = worldgen_queue.front();
        worldgen_queue.pop_front();
        worldgen_queued.erase({chunk_x, chunk_z});
        if (!decorated_chunks.insert({chunk_x, chunk_z}).second)
            continue;
        // The chunk's own deterministic seed: the same world and mods
        // decorate the same way, which makes a replay or a reset repeatable.
        // Sync never depends on it: the result travels as block edits.
        const std::uint32_t seed =
            mix(static_cast<std::uint32_t>(backend.world_seed()) ^
                mix(static_cast<std::uint32_t>(chunk_x) * 73856093U ^
                    static_cast<std::uint32_t>(chunk_z) * 19349663U));
        static constexpr std::array<std::string_view, 6> biomes{"classic", "lagoon", "desert",
                                                                "mountain", "arctic", "descent"};
        const int biome = backend.biome(chunk_x * 16 + 8, chunk_z * 16 + 8);
        emit(Side::server, "worldgen.chunk",
             make_map({{"cx", chunk_x},
                       {"cz", chunk_z},
                       {"x", chunk_x * 16},
                       {"z", chunk_z * 16},
                       {"seed", static_cast<std::int64_t>(seed)},
                       {"biome", world::biome_name(biome)}})); // mod-world
    }
}

} // namespace openminerz::modding::gameplay
