#pragma once

// (mod-liquid) Liquids: the cells that hold a mod block defined with
// `liquid = true` (blocks.define, openminerz/modding/gameplay.hpp).
//
// The world holds such a cell's retail fallback - what the save, stock
// messages and a peer without the mod see - so a cell is liquid while it still
// holds that fallback AND its mod identity is a liquid: generated there by the
// session's world generator (Chunk::mod_cell) or placed there since (the
// gameplay overlay). A liquid stops nothing (players, enemies, pickups, mod
// creatures), is swum in like the Lagoon's water (PlayerSimulation), lets the
// crosshair through, and is drawn translucent with its faces only against
// what is not liquid (WorldRegion::visible_faces_by_chunk).
//
// Liquids exist only in sessions whose server mods define one. With no table
// installed (the default, and every session without one) each function below
// returns at its first test and nothing anywhere changes.

#include "openminerz/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openminerz::world
{

// A liquid's surface lies this far below the top of its highest cell, drawn
// and swum alike.
inline constexpr float liquid_surface_drop = 0.125F;

// What a peer knows of its session's liquids. Built by the app from the mod
// block registry, the world generator's palette and the overlay of placed mod
// blocks; immutable once installed, so mesh workers may read a snapshot while
// the game thread installs the next.
class LiquidTable final
{
  public:
    struct Kind final
    {
        std::string key; // "mod:name"
        BlockType fallback{BlockType::ice};
        // The texture's mean colour (display-referred, 0..1): the tint and
        // fog of the view from inside it.
        std::array<float, 3> color{0.16F, 0.36F, 0.52F};
    };

    // Adds a liquid; returns its number (1-based), or 0 once 255 are in.
    std::uint8_t add_kind(Kind kind);
    // Generated mod blocks: palette entry `index` (1-based, Chunk::mod_cell)
    // is liquid `kind` (0: a mod block that is not a liquid) on `fallback`.
    void set_palette(std::size_t index, std::uint8_t kind, BlockType fallback);
    // A mod block placed in a cell after generation; `kind` 0 is one that is
    // not a liquid, which hides what was generated there.
    void set_placed(int x, int y, int z, std::uint8_t kind, BlockType fallback);

    [[nodiscard]] bool empty() const noexcept
    {
        return kinds_.empty();
    }
    [[nodiscard]] const std::vector<Kind> &kinds() const noexcept
    {
        return kinds_;
    }
    [[nodiscard]] const Kind *kind(std::uint8_t number) const noexcept;
    // Whether some liquid falls back to `block`: a cell that holds anything
    // else is never liquid.
    [[nodiscard]] bool falls_back_to(BlockType block) const noexcept
    {
        return fallbacks_[static_cast<std::size_t>(block)];
    }
    // The liquid in a cell of `region` (its number), 0 for none.
    [[nodiscard]] std::uint8_t kind_at(const WorldRegion &region, int x, int y, int z) const noexcept;
    // The same for a cell known some other way: the block it holds and its
    // Chunk::mod_cell.
    [[nodiscard]] std::uint8_t kind_of(int x, int y, int z, BlockType block,
                                       std::uint8_t palette) const noexcept;
    [[nodiscard]] bool liquid(const WorldRegion &region, int x, int y, int z) const noexcept
    {
        return kind_at(region, x, y, z) != 0U;
    }
    // Whether two tables answer every cell alike.
    [[nodiscard]] bool same_as(const LiquidTable &other) const noexcept;
    // Whether they differ in more than placed cells (the kinds, or what the
    // generator's palette is): every generated liquid may have changed.
    [[nodiscard]] static bool generated_differ(const LiquidTable *before,
                                              const LiquidTable *after) noexcept;
    // The placed cells whose answer differs between two tables (either may
    // be null).
    [[nodiscard]] static std::vector<std::array<int, 3>> changed_placed(const LiquidTable *before,
                                                                        const LiquidTable *after);

  private:
    struct Placed final
    {
        int x{};
        int y{};
        int z{};
        std::uint8_t kind{};
        BlockType fallback{};
        bool operator==(const Placed &) const noexcept = default;
    };
    std::array<bool, 256> fallbacks_{};
    std::vector<std::uint8_t> palette_;
    std::vector<BlockType> palette_fallback_;
    std::unordered_map<std::uint64_t, Placed> placed_;
    std::vector<Kind> kinds_;
};

// The session's liquids, process-wide like set_surface_lava_passable: every
// collision query of the simulation asks them. Null (the default) or an empty
// table: nothing is liquid. Installed and read by the game thread; liquids()
// hands any thread a snapshot.
void set_liquids(std::shared_ptr<const LiquidTable> table);
[[nodiscard]] std::shared_ptr<const LiquidTable> liquids();
[[nodiscard]] const LiquidTable *active_liquids() noexcept;

// The installed table's answer for a cell (0 / false without one).
[[nodiscard]] std::uint8_t liquid_kind_at(const WorldRegion &region, int x, int y, int z) noexcept;
[[nodiscard]] bool liquid_at(const WorldRegion &region, int x, int y, int z) noexcept;
// blocks_player for a cell: the block's own answer, except that a liquid
// stops nothing.
[[nodiscard]] bool cell_blocks_player(const WorldRegion &region, int x, int y, int z) noexcept;
// The surface of the liquid a point is in: the top of the column of liquid
// cells it stands in, less liquid_surface_drop. nullopt outside a liquid.
[[nodiscard]] std::optional<float> liquid_surface(const WorldRegion &region, float x, float y,
                                                  float z) noexcept;
// How far below that surface the point is (0 outside a liquid, or above it).
[[nodiscard]] float liquid_depth(const WorldRegion &region, float x, float y, float z) noexcept;

} // namespace openminerz::world
