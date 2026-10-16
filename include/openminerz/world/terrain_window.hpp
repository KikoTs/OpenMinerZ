#pragma once

namespace openminerz::world
{

// The block window of the shipped game's BlockTerrain (identical in 1.6.3.8
// and 1.9.9.8): 24 x 24 chunks of 16 blocks that follow the LOCAL player.
// Everything a stock peer simulates lives inside it. `RegionIsLoaded` is the
// test several entities apply to themselves, most importantly
// BaseZombie.OnUpdate, whose first statement is
//
//     if (!BlockTerrain.Instance.RegionIsLoaded(LocalPosition)) { Remove(); return; }
//
// so an enemy is dropped, silently and on that peer only, when it leaves the
// window around THAT peer's own player. There is no distance-to-target rule.
//
// This models the window's position, including the two quirks that decide
// where its edges are:
//   * BlockTerrain.SetWorldMin snaps with C# integer division, which truncates
//     toward zero, not toward minus infinity;
//   * BlockTerrain.TryShiftTerrain keeps the player's chunk at index 11 or 12
//     of 24 and only scrolls when it leaves that pair, so the window lags the
//     player by up to one chunk in the negative direction.
// The scroll is treated as immediate; the original spreads it over a few
// frames on a worker thread.
class StockTerrainWindow final
{
  public:
    static constexpr int chunk_size = 16;
    static constexpr int chunks = 24;
    static constexpr int blocks = chunk_size * chunks; // 384

    // BlockTerrain.Init / Teleport: SetWorldMin(center - 192). In the original
    // this is a full reset: every chunk is thrown away and rebuilt around the
    // new centre, and until a chunk has geometry again RegionIsLoaded is false
    // inside it. `resets()` counts these so that whatever the original drops
    // during one (every BaseZombie, on its next update) can be dropped here.
    constexpr void center_on(float eye_x, float eye_z) noexcept
    {
        min_x_ = (floor_to_int(eye_x) - blocks / 2) / chunk_size * chunk_size;
        min_z_ = (floor_to_int(eye_z) - blocks / 2) / chunk_size * chunk_size;
        centered_ = true;
        ++resets_;
    }

    [[nodiscard]] constexpr unsigned resets() const noexcept
    {
        return resets_;
    }
    [[nodiscard]] constexpr bool centered() const noexcept
    {
        return centered_;
    }

    // Once per frame with the local player's position: CenterOn(eye, true)
    // followed by TryShiftTerrain.
    constexpr void follow(float eye_x, float eye_z) noexcept
    {
        if (!centered_)
        {
            center_on(eye_x, eye_z);
            return;
        }
        const int index_x = chunk_index(eye_x, min_x_);
        const int index_z = chunk_index(eye_z, min_z_);
        if (index_x < 0 || index_x >= chunks || index_z < 0 || index_z >= chunks)
        {
            // CenterOn: the eye left the window altogether (a teleport).
            center_on(eye_x, eye_z);
            return;
        }
        min_x_ += shift_for(index_x) * chunk_size;
        min_z_ += shift_for(index_z) * chunk_size;
    }

    // BlockTerrain.RegionIsLoaded's index test. GetChunkVectorIndex divides
    // the block offset by 16 with truncation, so the fifteen blocks just below
    // the window's minimum edge still map to chunk 0 and count as loaded.
    [[nodiscard]] constexpr bool contains(float x, float z) const noexcept
    {
        const int index_x = chunk_index(x, min_x_);
        const int index_z = chunk_index(z, min_z_);
        return index_x >= 0 && index_x < chunks && index_z >= 0 && index_z < chunks;
    }

    [[nodiscard]] constexpr int minimum_x() const noexcept
    {
        return min_x_;
    }
    [[nodiscard]] constexpr int minimum_z() const noexcept
    {
        return min_z_;
    }

  private:
    [[nodiscard]] static constexpr int floor_to_int(float value) noexcept
    {
        const int truncated = static_cast<int>(value);
        return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
    }

    // (int)Math.Floor(position) - _worldMin, then `/= 16` on an int.
    [[nodiscard]] static constexpr int chunk_index(float position, int minimum) noexcept
    {
        return (floor_to_int(position) - minimum) / chunk_size;
    }

    // TryShiftTerrain: `d = index - 12; if (d < 0) d++;` and scroll by d.
    [[nodiscard]] static constexpr int shift_for(int index) noexcept
    {
        int shift = index - chunks / 2;
        if (shift < 0)
            ++shift;
        return shift;
    }

    int min_x_{-blocks / 2};
    int min_z_{-blocks / 2};
    bool centered_{};
    unsigned resets_{};
};

// A square of chunk columns: [origin, origin + span) on both axes.
struct ChunkWindow final
{
    int origin_x{};
    int origin_z{};
    int span{};

    [[nodiscard]] constexpr bool contains(int chunk_x, int chunk_z) const noexcept
    {
        return chunk_x >= origin_x && chunk_x < origin_x + span && chunk_z >= origin_z &&
               chunk_z < origin_z + span;
    }
    constexpr bool operator==(const ChunkWindow &) const noexcept = default;
};

// A client keeps two windows around its player, and only one of them may
// depend on a graphics setting.
//
// RESIDENT: the columns whose blocks exist -- for collision, enemy AI, pickups,
// projectiles and every RegionIsLoaded test. This is the original's window,
// for EVERY view distance: a peer that held less would drop an enemy that a
// stock player's game is still simulating. `view_distance` is a parameter only
// so that its irrelevance can be stated and tested.
[[nodiscard]] constexpr ChunkWindow resident_chunk_window(const StockTerrainWindow &window,
                                                          [[maybe_unused]] int view_distance) noexcept
{
    // The window minimum is always a multiple of the chunk size.
    return {window.minimum_x() / StockTerrainWindow::chunk_size,
            window.minimum_z() / StockTerrainWindow::chunk_size, StockTerrainWindow::chunks};
}

// MESHED: the columns that are lit, baked and drawn. The View distance setting
// (4 to 32, in units of four blocks) chooses 10 to 20 columns across, centred
// on the player's own chunk; it never exceeds the resident window.
[[nodiscard]] constexpr int meshed_span_chunks(int view_distance) noexcept
{
    const int clamped = view_distance < 4 ? 4 : view_distance > 32 ? 32 : view_distance;
    const int visible_radius = (clamped + 3) / 4;
    const int span = (visible_radius + 2) * 2;
    return span < 10 ? 10 : span;
}

[[nodiscard]] constexpr ChunkWindow meshed_chunk_window_of_span(int player_chunk_x,
                                                                int player_chunk_z,
                                                                int span) noexcept
{
    return {player_chunk_x - span / 2, player_chunk_z - span / 2, span};
}

[[nodiscard]] constexpr ChunkWindow meshed_chunk_window(int player_chunk_x, int player_chunk_z,
                                                        int view_distance) noexcept
{
    return meshed_chunk_window_of_span(player_chunk_x, player_chunk_z,
                                       meshed_span_chunks(view_distance));
}

} // namespace openminerz::world
