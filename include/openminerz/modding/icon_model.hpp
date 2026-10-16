#pragma once

// Modding, visuals area: a mod item's inventory icon as a small solid object,
// for the places the game shows an item in 3D - a dropped pickup and the item
// in the first-person hand. Every texel the icon covers becomes a voxel one
// texel square and `thickness` deep, coloured like the texel, with walls only
// where the icon ends - the extruded-sprite look of a pixel-art game.
//
// PRESENTATION ONLY: built on the client from the mod's icon file; nothing
// here reaches the simulation, the wire or a save.

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openminerz::modding::graphics
{

struct IconModelVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    // Straight (not premultiplied) linear-ish colour of the texel, alpha 1.
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

// A triangle list. The icon spans x in [-0.5, 0.5] (left to right) and y in
// [-0.5, 0.5] (bottom to top) and is centred on z = 0, front face towards +z.
struct IconModel final
{
    std::vector<IconModelVertex> vertices;
    int resolution{};
    float thickness{};

    [[nodiscard]] bool empty() const noexcept
    {
        return vertices.empty();
    }
};

// Texels at least half opaque are solid. The icon is reduced to at most
// `resolution` texels a side first (alpha-weighted, so a transparent border
// does not darken the edge colours). Empty for an empty or fully clear icon.
[[nodiscard]] inline IconModel icon_model(const Image &icon, int resolution = 32,
                                          float thickness = 1.0F / 16.0F)
{
    IconModel model;
    if (icon.empty())
        return model;
    const int side = std::clamp(std::min(std::max(icon.width, icon.height), resolution), 1, 256);
    model.resolution = side;
    model.thickness = thickness;
    // Alpha-weighted box reduction to side x side.
    std::vector<std::array<float, 4>> texels(static_cast<std::size_t>(side) * side);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
        {
            const int x0 = x * icon.width / side;
            const int x1 = std::max(x0 + 1, (x + 1) * icon.width / side);
            const int y0 = y * icon.height / side;
            const int y1 = std::max(y0 + 1, (y + 1) * icon.height / side);
            float red{};
            float green{};
            float blue{};
            float alpha{};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                {
                    const std::size_t at =
                        (static_cast<std::size_t>(sy) * static_cast<std::size_t>(icon.width) +
                         static_cast<std::size_t>(sx)) *
                        4U;
                    const float weight = static_cast<float>(icon.rgba[at + 3U]) / 255.0F;
                    red += static_cast<float>(icon.rgba[at]) * weight;
                    green += static_cast<float>(icon.rgba[at + 1U]) * weight;
                    blue += static_cast<float>(icon.rgba[at + 2U]) * weight;
                    alpha += weight;
                }
            const auto count = static_cast<float>((x1 - x0) * (y1 - y0));
            std::array<float, 4> &texel = texels[static_cast<std::size_t>(y) * side + x];
            texel[3] = alpha / count;
            if (alpha > 0.0F)
            {
                texel[0] = red / alpha / 255.0F;
                texel[1] = green / alpha / 255.0F;
                texel[2] = blue / alpha / 255.0F;
            }
        }
    const auto solid = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < side && y < side &&
               texels[static_cast<std::size_t>(y) * side + x][3] >= 0.5F;
    };
    const float cell = 1.0F / static_cast<float>(side);
    const float front = thickness * 0.5F;
    // Corner a of a quad and its two edges, wound counter-clockwise seen
    // along -normal (from outside).
    const auto quad = [&](std::array<float, 3> a, std::array<float, 3> edge_u,
                          std::array<float, 3> edge_v, std::array<float, 3> normal,
                          const std::array<float, 4> &color) {
        const auto at = [&](float u, float v) {
            return std::array<float, 3>{a[0] + edge_u[0] * u + edge_v[0] * v,
                                        a[1] + edge_u[1] * u + edge_v[1] * v,
                                        a[2] + edge_u[2] * u + edge_v[2] * v};
        };
        const std::array<std::array<float, 3>, 4> corners{at(0.0F, 0.0F), at(1.0F, 0.0F),
                                                          at(1.0F, 1.0F), at(0.0F, 1.0F)};
        for (const std::size_t corner : {0U, 1U, 2U, 0U, 2U, 3U})
            model.vertices.push_back({corners[corner], normal, color});
    };
    for (int y = 0; y < side; ++y)
    {
        // Image rows run top to bottom; the model's y runs up.
        const float top = 0.5F - static_cast<float>(y) * cell;
        const float bottom = top - cell;
        int x = 0;
        while (x < side)
        {
            if (!solid(x, y))
            {
                ++x;
                continue;
            }
            // One quad per run of identical colour on the two broad faces.
            const std::array<float, 4> &texel = texels[static_cast<std::size_t>(y) * side + x];
            int end = x + 1;
            while (end < side && solid(end, y) &&
                   texels[static_cast<std::size_t>(y) * side + end] == texel)
                ++end;
            const std::array<float, 4> color{texel[0], texel[1], texel[2], 1.0F};
            const float left = -0.5F + static_cast<float>(x) * cell;
            const float right = -0.5F + static_cast<float>(end) * cell;
            const float width = right - left;
            quad({left, bottom, front}, {width, 0.0F, 0.0F}, {0.0F, cell, 0.0F},
                 {0.0F, 0.0F, 1.0F}, color);
            quad({right, bottom, -front}, {-width, 0.0F, 0.0F}, {0.0F, cell, 0.0F},
                 {0.0F, 0.0F, -1.0F}, color);
            x = end;
        }
        // The walls where the icon ends, a little darker like a cut edge.
        for (int column = 0; column < side; ++column)
        {
            if (!solid(column, y))
                continue;
            const std::array<float, 4> &texel = texels[static_cast<std::size_t>(y) * side + column];
            const std::array<float, 4> color{texel[0] * 0.8F, texel[1] * 0.8F, texel[2] * 0.8F,
                                             1.0F};
            const float left = -0.5F + static_cast<float>(column) * cell;
            const float right = left + cell;
            if (!solid(column - 1, y))
                quad({left, bottom, -front}, {0.0F, 0.0F, thickness}, {0.0F, cell, 0.0F},
                     {-1.0F, 0.0F, 0.0F}, color);
            if (!solid(column + 1, y))
                quad({right, bottom, front}, {0.0F, 0.0F, -thickness}, {0.0F, cell, 0.0F},
                     {1.0F, 0.0F, 0.0F}, color);
            if (!solid(column, y - 1))
                quad({left, top, front}, {cell, 0.0F, 0.0F}, {0.0F, 0.0F, -thickness},
                     {0.0F, 1.0F, 0.0F}, color);
            if (!solid(column, y + 1))
                quad({left, bottom, -front}, {cell, 0.0F, 0.0F}, {0.0F, 0.0F, thickness},
                     {0.0F, -1.0F, 0.0F}, color);
        }
    }
    return model;
}

} // namespace openminerz::modding::graphics
