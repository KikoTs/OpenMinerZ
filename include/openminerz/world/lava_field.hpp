#pragma once

// Where the lava is, as the Enhanced ("Rebirth") presentation's lighting, haze
// and embers see it: a small world-anchored map of every column that shows
// lava, with a full chain of averaged levels.
//
// PRESENTATION ONLY. It is built from the faces the mesher already emitted for
// drawing and is never read by the simulation, the voxel light field or the
// network. The Original ("Classic") presentation never samples it.
//
// WHY A MAP. The voxel light field floods fifteen cells from an emitter. The
// lava under Hell is a lake hundreds of blocks wide, twenty and more blocks
// below the cavern roof it should light. Sampling this map at a level whose
// texel is about as wide as the surface is high above the lava is the cosine
// kernel of an area light, h^2 / (pi (h^2 + d^2)^2), to within a box filter:
// near the lake a wall sees only the lava in front of it, the roof sees the
// whole lake, and a lone block of lava fades with the square of the distance
// because its share of the footprint does.
//
// TWO LAYERS, because one height per column cannot hold a cave's lava pocket
// and the lake far below it: `deep` is lava whose surface is under
// lava_shallow_floor (the Hell lake), `shallow` everything above (pockets in
// cave walls, lava met at the surface).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace openminerz::world
{

// One column that shows lava: where it is and the height of its surface.
struct LavaColumn final
{
    std::int32_t x{};
    std::int32_t z{};
    std::int16_t top{};

    bool operator==(const LavaColumn &) const noexcept = default;
};

// Side of the map in blocks. Sixteen chunks: the map is addressed by world
// position modulo this, so re-centring it never moves a texel.
inline constexpr int lava_field_extent = 256;
inline constexpr int lava_field_chunk = 16;
inline constexpr int lava_field_slots = lava_field_extent / lava_field_chunk;
// 256, 128 .. 1.
inline constexpr int lava_field_levels = 9;
// Heights are stored as a share of this range above this floor.
inline constexpr float lava_field_height_floor = -64.0F;
inline constexpr float lava_field_height_range = 128.0F;
// A lava surface at or above this is `shallow`.
inline constexpr int lava_shallow_floor = -48;
// Beyond this distance from the window's centre the map fades to nothing, so
// the wrap-around never shows.
inline constexpr float lava_field_fade_start = 104.0F;
inline constexpr float lava_field_fade_width = 16.0F;

class LavaField final
{
  public:
    // Chunk x, chunk z, vertical mesh section.
    using SectionKey = std::tuple<int, int, int>;

    // What the GPU copy holds per texel: coverage and coverage x height share.
    struct Texel final
    {
        float deep{};
        float deep_height{};
        float shallow{};
        float shallow_height{};
    };

    struct Sample final
    {
        // Share of the footprint that is lava, 0..1.
        float deep{};
        float shallow{};
        // World height of that lava's surface (meaningless at zero coverage).
        float deep_top{};
        float shallow_top{};
    };

    LavaField()
    {
        int side = lava_field_extent;
        for (auto &level : levels_)
        {
            level.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), Texel{});
            side = std::max(1, side / 2);
        }
        slot_chunk_.fill({unassigned, unassigned});
        recenter(0, 0);
        // Nothing is on the GPU yet, and nothing needs to be: the map is empty.
        dirty_.fill(false);
        any_dirty_ = false;
    }

    void clear()
    {
        sections_.clear();
        for (int slot = 0; slot < lava_field_slots * lava_field_slots; ++slot)
            if (slot_filled_[static_cast<std::size_t>(slot)])
                rasterize(slot);
    }

    // Replaces what one mesh section shows; an empty list removes it. Columns
    // outside the section's own chunk are ignored.
    void set_section(const SectionKey &key, std::vector<LavaColumn> columns)
    {
        const auto found = sections_.find(key);
        if (columns.empty())
        {
            if (found == sections_.end())
                return;
            sections_.erase(found);
        }
        else if (found != sections_.end())
        {
            if (found->second == columns)
                return;
            found->second = std::move(columns);
        }
        else
        {
            sections_.emplace(key, std::move(columns));
        }
        const int slot = slot_of(std::get<0>(key), std::get<1>(key));
        if (holds(slot, std::get<0>(key), std::get<1>(key)))
            rasterize(slot);
    }

    // Drops every section outside the inclusive chunk rectangle.
    void retain(int minimum_chunk_x, int minimum_chunk_z, int maximum_chunk_x, int maximum_chunk_z)
    {
        std::array<bool, lava_field_slots * lava_field_slots> redraw{};
        bool any{};
        for (auto section = sections_.begin(); section != sections_.end();)
        {
            const int chunk_x = std::get<0>(section->first);
            const int chunk_z = std::get<1>(section->first);
            if (chunk_x >= minimum_chunk_x && chunk_x <= maximum_chunk_x &&
                chunk_z >= minimum_chunk_z && chunk_z <= maximum_chunk_z)
            {
                ++section;
                continue;
            }
            section = sections_.erase(section);
            const int slot = slot_of(chunk_x, chunk_z);
            if (holds(slot, chunk_x, chunk_z))
            {
                redraw[static_cast<std::size_t>(slot)] = true;
                any = true;
            }
        }
        if (!any)
            return;
        for (int slot = 0; slot < lava_field_slots * lava_field_slots; ++slot)
            if (redraw[static_cast<std::size_t>(slot)])
                rasterize(slot);
    }

    // Moves the sixteen-chunk window so that it is centred on this chunk. Only
    // the chunks that entered or left are redrawn.
    void recenter(int chunk_x, int chunk_z)
    {
        center_chunk_x_ = chunk_x;
        center_chunk_z_ = chunk_z;
        const int first_x = chunk_x - lava_field_slots / 2;
        const int first_z = chunk_z - lava_field_slots / 2;
        for (int offset_z = 0; offset_z < lava_field_slots; ++offset_z)
            for (int offset_x = 0; offset_x < lava_field_slots; ++offset_x)
            {
                const int wanted_x = first_x + offset_x;
                const int wanted_z = first_z + offset_z;
                const int slot = slot_of(wanted_x, wanted_z);
                if (holds(slot, wanted_x, wanted_z))
                    continue;
                slot_chunk_[static_cast<std::size_t>(slot)] = {wanted_x, wanted_z};
                rasterize(slot);
            }
    }

    [[nodiscard]] float center_x() const noexcept
    {
        return (static_cast<float>(center_chunk_x_) + 0.5F) * static_cast<float>(lava_field_chunk);
    }

    [[nodiscard]] float center_z() const noexcept
    {
        return (static_cast<float>(center_chunk_z_) + 0.5F) * static_cast<float>(lava_field_chunk);
    }

    // 1 inside the window, fading to 0 before its edge (the shaders' mirror).
    [[nodiscard]] float window(float x, float z) const noexcept
    {
        const float away = std::max(std::abs(x - center_x()), std::abs(z - center_z()));
        return std::clamp((lava_field_fade_start + lava_field_fade_width - away) /
                              lava_field_fade_width,
                          0.0F, 1.0F);
    }

    // Bilinear read of one level (rounded), windowed like the shaders' read.
    [[nodiscard]] Sample sample(float x, float z, float level) const noexcept
    {
        const int index = std::clamp(static_cast<int>(std::lround(level)), 0, lava_field_levels - 1);
        const int side = lava_field_extent >> index;
        const float scale = static_cast<float>(side) / static_cast<float>(lava_field_extent);
        const float u = x * scale - 0.5F;
        const float v = z * scale - 0.5F;
        const float floor_u = std::floor(u);
        const float floor_v = std::floor(v);
        const float fraction_u = u - floor_u;
        const float fraction_v = v - floor_v;
        const auto wrap = [side](float value) {
            const int integer = static_cast<int>(value);
            return ((integer % side) + side) % side;
        };
        const int x0 = wrap(floor_u);
        const int z0 = wrap(floor_v);
        const int x1 = (x0 + 1) % side;
        const int z1 = (z0 + 1) % side;
        const auto &texels = levels_[static_cast<std::size_t>(index)];
        const auto at = [&](int texel_x, int texel_z) -> const Texel & {
            return texels[static_cast<std::size_t>(texel_z) * static_cast<std::size_t>(side) +
                          static_cast<std::size_t>(texel_x)];
        };
        const auto mix = [&](float Texel::*channel) {
            const float top = std::lerp(at(x0, z0).*channel, at(x1, z0).*channel, fraction_u);
            const float bottom = std::lerp(at(x0, z1).*channel, at(x1, z1).*channel, fraction_u);
            return std::lerp(top, bottom, fraction_v);
        };
        const float deep = mix(&Texel::deep);
        const float deep_height = mix(&Texel::deep_height);
        const float shallow = mix(&Texel::shallow);
        const float shallow_height = mix(&Texel::shallow_height);
        const float weight = window(x, z);
        Sample result;
        result.deep = deep * weight;
        result.shallow = shallow * weight;
        result.deep_top = height_of(deep, deep_height);
        result.shallow_top = height_of(shallow, shallow_height);
        return result;
    }

    // Any lava at all in the window (the 1x1 level).
    [[nodiscard]] bool empty() const noexcept
    {
        const Texel &whole = levels_.back().front();
        return whole.deep <= 0.0F && whole.shallow <= 0.0F;
    }

    [[nodiscard]] const std::vector<Texel> &level(int index) const noexcept
    {
        return levels_[static_cast<std::size_t>(std::clamp(index, 0, lava_field_levels - 1))];
    }

    // Chunk slots (x, z in 0..15) whose texels changed since the last call. A
    // slot is the 16x16 block at (x * 16, z * 16) of level 0 and its share of
    // every coarser level.
    [[nodiscard]] std::vector<std::array<int, 2>> take_dirty_slots()
    {
        std::vector<std::array<int, 2>> slots;
        if (!any_dirty_)
            return slots;
        for (int slot = 0; slot < lava_field_slots * lava_field_slots; ++slot)
            if (dirty_[static_cast<std::size_t>(slot)])
                slots.push_back({slot % lava_field_slots, slot / lava_field_slots});
        dirty_.fill(false);
        any_dirty_ = false;
        return slots;
    }

    [[nodiscard]] std::size_t section_count() const noexcept
    {
        return sections_.size();
    }

    [[nodiscard]] static float height_share(float top) noexcept
    {
        return std::clamp((top - lava_field_height_floor) / lava_field_height_range, 0.0F, 1.0F);
    }

  private:
    static constexpr int unassigned = std::numeric_limits<int>::min();

    [[nodiscard]] static int wrap_slot(int chunk) noexcept
    {
        return ((chunk % lava_field_slots) + lava_field_slots) % lava_field_slots;
    }

    [[nodiscard]] static int slot_of(int chunk_x, int chunk_z) noexcept
    {
        return wrap_slot(chunk_z) * lava_field_slots + wrap_slot(chunk_x);
    }

    [[nodiscard]] bool holds(int slot, int chunk_x, int chunk_z) const noexcept
    {
        const auto &held = slot_chunk_[static_cast<std::size_t>(slot)];
        return held[0] == chunk_x && held[1] == chunk_z;
    }

    [[nodiscard]] static float height_of(float coverage, float premultiplied) noexcept
    {
        if (coverage <= 1.0e-6F)
            return lava_field_height_floor;
        return premultiplied / coverage * lava_field_height_range + lava_field_height_floor;
    }

    using SectionMap = std::map<SectionKey, std::vector<LavaColumn>>;

    [[nodiscard]] std::pair<SectionMap::const_iterator, SectionMap::const_iterator>
    sections_of(int chunk_x, int chunk_z) const
    {
        return {sections_.lower_bound(SectionKey{chunk_x, chunk_z, unassigned}),
                sections_.upper_bound(
                    SectionKey{chunk_x, chunk_z, std::numeric_limits<int>::max()})};
    }

    // Redraws one chunk's block of level 0 from every section that shows lava
    // there, then its share of the coarser levels.
    void rasterize(int slot)
    {
        const auto &held = slot_chunk_[static_cast<std::size_t>(slot)];
        const int slot_x = slot % lava_field_slots;
        const int slot_z = slot / lava_field_slots;
        std::array<Texel, lava_field_chunk * lava_field_chunk> block{};
        bool filled{};
        if (held[0] != unassigned)
        {
            const auto [first, last] = sections_of(held[0], held[1]);
            for (auto section = first; section != last; ++section)
                for (const LavaColumn &column : section->second)
                {
                    const int local_x = column.x - held[0] * lava_field_chunk;
                    const int local_z = column.z - held[1] * lava_field_chunk;
                    if (local_x < 0 || local_x >= lava_field_chunk || local_z < 0 ||
                        local_z >= lava_field_chunk)
                        continue;
                    Texel &texel =
                        block[static_cast<std::size_t>(local_z * lava_field_chunk + local_x)];
                    const float share = height_share(static_cast<float>(column.top));
                    if (column.top >= lava_shallow_floor)
                    {
                        texel.shallow_height =
                            texel.shallow > 0.0F ? std::max(texel.shallow_height, share) : share;
                        texel.shallow = 1.0F;
                    }
                    else
                    {
                        texel.deep_height =
                            texel.deep > 0.0F ? std::max(texel.deep_height, share) : share;
                        texel.deep = 1.0F;
                    }
                    filled = true;
                }
        }
        const std::size_t index = static_cast<std::size_t>(slot);
        if (!filled && !slot_filled_[index])
            return;
        slot_filled_[index] = filled;
        dirty_[index] = true;
        any_dirty_ = true;

        auto &base = levels_[0];
        for (int z = 0; z < lava_field_chunk; ++z)
            for (int x = 0; x < lava_field_chunk; ++x)
                base[static_cast<std::size_t>(slot_z * lava_field_chunk + z) * lava_field_extent +
                     static_cast<std::size_t>(slot_x * lava_field_chunk + x)] =
                    block[static_cast<std::size_t>(z * lava_field_chunk + x)];
        // Levels 1-4 cover the slot with 8, 4, 2 and 1 texels a side; from
        // level 5 on one texel holds several slots.
        for (int level = 1; level < lava_field_levels; ++level)
        {
            const int side = lava_field_extent >> level;
            const int span = std::max(1, lava_field_chunk >> level);
            const int origin_x = (slot_x * lava_field_chunk) >> level;
            const int origin_z = (slot_z * lava_field_chunk) >> level;
            const auto &finer = levels_[static_cast<std::size_t>(level - 1)];
            const int finer_side = side * 2;
            auto &coarser = levels_[static_cast<std::size_t>(level)];
            for (int z = origin_z; z < origin_z + span; ++z)
                for (int x = origin_x; x < origin_x + span; ++x)
                {
                    Texel total;
                    for (int tap = 0; tap < 4; ++tap)
                    {
                        const Texel &source =
                            finer[static_cast<std::size_t>(z * 2 + (tap >> 1)) *
                                      static_cast<std::size_t>(finer_side) +
                                  static_cast<std::size_t>(x * 2 + (tap & 1))];
                        total.deep += source.deep;
                        total.deep_height += source.deep_height;
                        total.shallow += source.shallow;
                        total.shallow_height += source.shallow_height;
                    }
                    coarser[static_cast<std::size_t>(z) * static_cast<std::size_t>(side) +
                            static_cast<std::size_t>(x)] = {total.deep * 0.25F,
                                                            total.deep_height * 0.25F,
                                                            total.shallow * 0.25F,
                                                            total.shallow_height * 0.25F};
                }
        }
    }

    SectionMap sections_;
    std::array<std::vector<Texel>, lava_field_levels> levels_;
    std::array<std::array<int, 2>, lava_field_slots * lava_field_slots> slot_chunk_{};
    std::array<bool, lava_field_slots * lava_field_slots> slot_filled_{};
    std::array<bool, lava_field_slots * lava_field_slots> dirty_{};
    bool any_dirty_{};
    int center_chunk_x_{};
    int center_chunk_z_{};
};

} // namespace openminerz::world
