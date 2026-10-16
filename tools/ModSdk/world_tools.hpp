#pragma once

// mod-world: the SDK's world generator tools.
//
//   check_world_generator   what `validate` adds for a mod that calls
//                           terrain.define: the world is built in the
//                           sandbox exactly as the game builds it, and its
//                           errors, unused biomes and out-of-range ground are
//                           reported.
//   render_world_map        `preview-map`: a top-down biome and height map of
//                           a generator, from the same native evaluator the
//                           game runs, so a 25,000-block layout can be seen
//                           without starting the game.

#include "validate.hpp"

#include "openminerz/world/world.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openminerz::mod_sdk
{

class Sandbox;

void check_world_generator(ValidationResult &result);

struct WorldMapRequest final
{
    int pixels{1024};          // width and height of the image
    double size{52000.0};      // blocks across
    double center_x{};
    double center_z{};
    // The colour of each mod block of the generator's palette (index - 1),
    // e.g. its texture's mean; {} where unknown (its fallback's colour).
    std::vector<std::optional<std::array<std::uint8_t, 3>>> palette_colors;
};

// The mean colours of the generator's mod blocks, from their textures in the
// sandbox's assets (what preview-map draws a mod biome's ground with).
[[nodiscard]] std::vector<std::optional<std::array<std::uint8_t, 3>>> palette_colors(
    Sandbox &sandbox);

struct WorldMap final
{
    int pixels{};
    std::vector<std::uint8_t> rgb; // pixels * pixels * 3, rows north (-Z) first
    // "key  #rrggbb  share%" lines, heaviest first.
    std::vector<std::string> legend;
};

// Renders the generator's map (retail terrain too, when no mod terrain is
// installed). Uses every core.
[[nodiscard]] WorldMap render_world_map(const world::WorldGenerator &generator,
                                        const WorldMapRequest &request);

// A plain 8-bit RGB PNG (stored deflate blocks: big, but no zlib needed).
bool write_png(const std::filesystem::path &path, int width, int height,
               const std::vector<std::uint8_t> &rgb);

} // namespace openminerz::mod_sdk
