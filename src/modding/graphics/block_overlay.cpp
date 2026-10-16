// Mod blocks on the terrain (openminerz/modding/graphics.hpp): the cells a
// client draws with a mod block's tiles, and the pass that gives a mesh
// section's faces those tiles after the retail mesher built it.

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <tuple>

namespace openminerz::modding::graphics
{
namespace
{

[[nodiscard]] int floor_divide(int value, int divisor) noexcept
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

// Cells are told apart exactly by the coordinates stored with them; the key
// only spreads them (24 bits of x and z are far more than a loaded window).
[[nodiscard]] std::uint64_t cell_key(int x, int y, int z) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFU) << 40U) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFU) << 16U) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFFFU));
}

[[nodiscard]] world::ChunkCoordinate section_of(int x, int y, int z) noexcept
{
    return {floor_divide(x, world::chunk_width), floor_divide(z, world::chunk_depth),
            std::clamp((y - world::world_min_y) / world::chunk_section_height, 0,
                       world::chunk_section_count - 1)};
}

[[nodiscard]] std::uint64_t section_key(world::ChunkCoordinate section) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(section.x)) << 32U) ^
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(section.z)) << 4U) ^
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(section.y) & 15U);
}

[[nodiscard]] bool same_faces(const ModBlockLook &a, const ModBlockLook &b) noexcept
{
    return a.fallback == b.fallback && a.tiles == b.tiles && a.liquid == b.liquid; // (mod-liquid)
}

} // namespace

std::size_t mod_block_face_slot(world::FaceDirection face) noexcept
{
    switch (face)
    {
    case world::FaceDirection::positive_y:
        return 0U;
    case world::FaceDirection::negative_y:
        return 2U;
    default:
        return 1U;
    }
}

void ModBlockOverlay::set(int x, int y, int z, const ModBlockLook &look)
{
    cells_.insert_or_assign(cell_key(x, y, z), Cell{x, y, z, look});
    sections_.insert(section_key(section_of(x, y, z)));
}

const ModBlockLook *ModBlockOverlay::find(int x, int y, int z,
                                          world::BlockType block) const noexcept
{
    if (cells_.empty())
        return nullptr;
    const auto found = cells_.find(cell_key(x, y, z));
    if (found == cells_.end() || found->second.x != x || found->second.y != y ||
        found->second.z != z || found->second.look.fallback != static_cast<std::uint8_t>(block))
        return nullptr;
    return &found->second.look;
}

std::uint8_t debris_tile(const ModBlockOverlay *overlay, int x, int y, int z,
                         world::BlockType block, world::FaceDirection face, bool mod_tiles) noexcept
{
    if (mod_tiles && overlay != nullptr)
        if (const ModBlockLook *look = overlay->find(x, y, z, block))
            return look->tiles[mod_block_face_slot(face)];
    return world::atlas_tile(block, face);
}

std::size_t ModBlockOverlay::apply(world::ChunkFaceBatch &batch) const
{
    if (cells_.empty() || !sections_.contains(section_key(batch.coordinate)))
        return 0U;
    std::size_t changed{};
    for (world::WorldFace &face : batch.faces)
    {
        const ModBlockLook *look = find(face.x, face.y, face.z, face.block);
        if (look == nullptr)
            continue;
        face.tile = look->tiles[mod_block_face_slot(face.direction)];
        ++changed;
    }
    return changed;
}

std::vector<world::ChunkCoordinate> ModBlockOverlay::changed_sections(const ModBlockOverlay *before,
                                                                      const ModBlockOverlay *after)
{
    std::vector<world::ChunkCoordinate> sections;
    const auto compare = [&](const ModBlockOverlay *from, const ModBlockOverlay *to) {
        if (from == nullptr)
            return;
        for (const auto &[key, cell] : from->cells_)
        {
            const Cell *other = nullptr;
            if (to != nullptr)
                if (const auto found = to->cells_.find(key); found != to->cells_.end())
                    other = &found->second;
            if (other != nullptr && other->x == cell.x && other->y == cell.y &&
                other->z == cell.z && same_faces(other->look, cell.look))
                continue;
            sections.push_back(section_of(cell.x, cell.y, cell.z));
        }
    };
    compare(before, after);
    compare(after, before);
    std::sort(sections.begin(), sections.end(), [](const auto &a, const auto &b) {
        return std::tie(a.x, a.z, a.y) < std::tie(b.x, b.z, b.y);
    });
    sections.erase(std::unique(sections.begin(), sections.end()), sections.end());
    return sections;
}

} // namespace openminerz::modding::graphics
