#pragma once

// Content-pack loads that a mod may override (openminerz/modding/graphics.hpp,
// ASSET FOLDERS). Every texture the game reads from DerivedData goes through
// one of these; with no pack installed they read exactly the file they always
// read, as SDL_LoadBMP did.

#include "openminerz/modding/graphics.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string_view>

namespace openminerz
{

enum class ContentKind : std::uint8_t
{
    // Opaque or straight-alpha art: creature skins, sky faces, particles.
    entities,
    sky,
    particles,
    // XNA sprites, premultiplied: a mod's straight-alpha PNG is premultiplied
    // on load so it blends like the retail art it replaces.
    ui,
};

// `path` (a DerivedData BMP), or the PNG/BMP/TGA a mod puts in its place.
// The caller owns the surface (SDL_DestroySurface); nullptr when neither loads.
[[nodiscard]] SDL_Surface *load_content_surface(const std::filesystem::path &path,
                                                ContentKind kind);

// An RGBA32 surface holding `image` (nullptr when empty).
[[nodiscard]] SDL_Surface *surface_from_image(const modding::graphics::Image &image);

// Which of the three terrain atlases a surface is.
enum class AtlasLayer : std::uint8_t
{
    diffuse,
    normal_spec,
    metal_light,
};

// Writes the installed packs' tile overrides into a loaded 8x8 atlas surface
// (converting it to RGBA32 when there is anything to write). Returns the
// surface to use, which may be a new one; `surface` is then destroyed.
[[nodiscard]] SDL_Surface *apply_atlas_overrides(SDL_Surface *surface, AtlasLayer layer);

// The diffuse images of the installed mod block tiles, in slice order
// (BlockTextureTable), resolved through the asset stack; empty without any.
[[nodiscard]] std::vector<modding::graphics::Image> mod_block_tile_images();

} // namespace openminerz
