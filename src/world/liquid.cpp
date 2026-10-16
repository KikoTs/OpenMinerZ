// (mod-liquid) Liquids: see openminerz/world/liquid.hpp.

#include "openminerz/world/liquid.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace openminerz::world
{
namespace
{

[[nodiscard]] std::uint64_t pack_cell(int x, int y, int z) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) ^
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x00FFFFFFU) << 8U) ^
           static_cast<std::uint64_t>(static_cast<std::uint8_t>(y - world_min_y));
}

// The installed table: owned here, read through `current` by the game thread
// without a lock (it is also the only writer), and copied out under the lock
// by any other thread.
std::mutex installed_mutex;
std::shared_ptr<const LiquidTable> installed_table;
std::atomic<const LiquidTable *> current{nullptr};

} // namespace

std::uint8_t LiquidTable::add_kind(Kind kind)
{
    if (kinds_.size() >= 255U)
        return 0U;
    fallbacks_[static_cast<std::size_t>(kind.fallback)] = true;
    kinds_.push_back(std::move(kind));
    return static_cast<std::uint8_t>(kinds_.size());
}

void LiquidTable::set_palette(std::size_t index, std::uint8_t kind, BlockType fallback)
{
    if (index == 0U || index > 255U)
        return;
    if (palette_.size() < index)
    {
        palette_.resize(index, 0U);
        palette_fallback_.resize(index, BlockType::empty);
    }
    palette_[index - 1U] = kind <= kinds_.size() ? kind : std::uint8_t{0U};
    palette_fallback_[index - 1U] = fallback;
}

void LiquidTable::set_placed(int x, int y, int z, std::uint8_t kind, BlockType fallback)
{
    placed_.insert_or_assign(
        pack_cell(x, y, z), Placed{x, y, z, kind <= kinds_.size() ? kind : std::uint8_t{0U}, fallback});
}

const LiquidTable::Kind *LiquidTable::kind(std::uint8_t number) const noexcept
{
    return number >= 1U && number <= kinds_.size() ? &kinds_[number - 1U] : nullptr;
}

std::uint8_t LiquidTable::kind_at(const WorldRegion &region, int x, int y, int z) const noexcept
{
    if (kinds_.empty() || !region.contains(x, y, z))
        return 0U;
    const BlockType block = region.block(x, y, z);
    // The one test almost every cell stops at.
    if (!fallbacks_[static_cast<std::size_t>(block)])
        return 0U;
    return kind_of(x, y, z, block, region.mod_cell(x, y, z));
}

std::uint8_t LiquidTable::kind_of(int x, int y, int z, BlockType block,
                                  std::uint8_t palette) const noexcept
{
    if (!fallbacks_[static_cast<std::size_t>(block)])
        return 0U;
    if (!placed_.empty())
    {
        const auto placed = placed_.find(pack_cell(x, y, z));
        if (placed != placed_.end() && placed->second.x == x && placed->second.y == y &&
            placed->second.z == z)
            return placed->second.fallback == block ? placed->second.kind : std::uint8_t{0U};
    }
    if (palette == 0U || palette > palette_.size() || palette_fallback_[palette - 1U] != block)
        return 0U;
    return palette_[palette - 1U];
}

bool LiquidTable::same_as(const LiquidTable &other) const noexcept
{
    if (kinds_.size() != other.kinds_.size() || palette_ != other.palette_ ||
        palette_fallback_ != other.palette_fallback_ || placed_ != other.placed_)
        return false;
    for (std::size_t index = 0U; index < kinds_.size(); ++index)
        if (kinds_[index].key != other.kinds_[index].key ||
            kinds_[index].fallback != other.kinds_[index].fallback ||
            kinds_[index].color != other.kinds_[index].color)
            return false;
    return true;
}

bool LiquidTable::generated_differ(const LiquidTable *before, const LiquidTable *after) noexcept
{
    const bool had = before != nullptr && !before->empty();
    const bool has = after != nullptr && !after->empty();
    if (had != has)
        return true;
    if (!had)
        return false;
    if (before->kinds_.size() != after->kinds_.size() || before->palette_ != after->palette_ ||
        before->palette_fallback_ != after->palette_fallback_)
        return true;
    for (std::size_t index = 0U; index < before->kinds_.size(); ++index)
        if (before->kinds_[index].fallback != after->kinds_[index].fallback)
            return true;
    return false;
}

std::vector<std::array<int, 3>> LiquidTable::changed_placed(const LiquidTable *before,
                                                            const LiquidTable *after)
{
    std::vector<std::array<int, 3>> cells;
    const auto compare = [&](const LiquidTable *from, const LiquidTable *to) {
        if (from == nullptr)
            return;
        for (const auto &[key, cell] : from->placed_)
        {
            if (to != nullptr)
                if (const auto found = to->placed_.find(key);
                    found != to->placed_.end() && found->second == cell)
                    continue;
            cells.push_back({cell.x, cell.y, cell.z});
        }
    };
    compare(before, after);
    compare(after, before);
    return cells;
}

void set_liquids(std::shared_ptr<const LiquidTable> table)
{
    if (table != nullptr && table->empty())
        table.reset();
    const std::lock_guard lock{installed_mutex};
    current.store(table.get(), std::memory_order_release);
    installed_table = std::move(table);
}

std::shared_ptr<const LiquidTable> liquids()
{
    const std::lock_guard lock{installed_mutex};
    return installed_table;
}

const LiquidTable *active_liquids() noexcept
{
    return current.load(std::memory_order_acquire);
}

std::uint8_t liquid_kind_at(const WorldRegion &region, int x, int y, int z) noexcept
{
    const LiquidTable *table = active_liquids();
    return table != nullptr ? table->kind_at(region, x, y, z) : std::uint8_t{0U};
}

bool liquid_at(const WorldRegion &region, int x, int y, int z) noexcept
{
    return liquid_kind_at(region, x, y, z) != 0U;
}

bool cell_blocks_player(const WorldRegion &region, int x, int y, int z) noexcept
{
    return blocks_player(region.block(x, y, z)) && !liquid_at(region, x, y, z);
}

std::optional<float> liquid_surface(const WorldRegion &region, float x, float y, float z) noexcept
{
    const LiquidTable *table = active_liquids();
    if (table == nullptr || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return std::nullopt;
    const int cell_x = static_cast<int>(std::floor(x));
    const int cell_z = static_cast<int>(std::floor(z));
    int top = static_cast<int>(std::floor(y));
    if (!table->liquid(region, cell_x, top, cell_z))
        return std::nullopt;
    while (top + 1 < world_min_y + chunk_height && table->liquid(region, cell_x, top + 1, cell_z))
        ++top;
    return static_cast<float>(top + 1) - liquid_surface_drop;
}

float liquid_depth(const WorldRegion &region, float x, float y, float z) noexcept
{
    const std::optional<float> surface = liquid_surface(region, x, y, z);
    return surface.has_value() ? std::max(*surface - y, 0.0F) : 0.0F;
}

} // namespace openminerz::world
