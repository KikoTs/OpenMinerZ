#include "mod_surfaces.hpp"

#include <cstring>
#include <iostream>
#include <map>
#include <mutex>

namespace openminerz
{
namespace
{

std::string_view kind_folder(ContentKind kind) noexcept
{
    switch (kind)
    {
    case ContentKind::entities:
        return "entities";
    case ContentKind::sky:
        return "sky";
    case ContentKind::particles:
        return "particles";
    case ContentKind::ui:
        return "ui";
    }
    return "ui";
}

// The installed packs' tile overrides, decoded once per installed stack.
std::shared_ptr<const modding::graphics::TileOverrides> installed_tiles()
{
    static std::mutex mutex;
    static std::weak_ptr<const modding::graphics::PresentationAssets> key;
    static std::shared_ptr<const modding::graphics::TileOverrides> cached;
    const auto assets = modding::graphics::presentation_assets();
    if (assets == nullptr)
        return nullptr;
    const std::lock_guard lock{mutex};
    if (cached == nullptr || key.lock() != assets)
    {
        std::vector<std::string> warnings;
        cached = std::make_shared<const modding::graphics::TileOverrides>(
            modding::graphics::collect_tile_overrides(assets->stack, &warnings));
        for (const std::string &warning : warnings)
            std::cerr << "Texture pack: " << warning << '\n';
        key = assets;
    }
    return cached;
}

} // namespace

SDL_Surface *surface_from_image(const modding::graphics::Image &image)
{
    if (image.empty())
        return nullptr;
    SDL_Surface *surface = SDL_CreateSurface(image.width, image.height, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr)
        return nullptr;
    for (int y = 0; y < image.height; ++y)
        std::memcpy(
            static_cast<std::uint8_t *>(surface->pixels) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(surface->pitch),
            &image.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 4U],
            static_cast<std::size_t>(image.width) * 4U);
    return surface;
}

SDL_Surface *load_content_surface(const std::filesystem::path &path, ContentKind kind)
{
    const std::filesystem::path replacement =
        modding::graphics::derived_override(kind_folder(kind), path.filename().string());
    if (!replacement.empty())
    {
        std::string error;
        if (auto image = modding::graphics::load_image(replacement, &error))
        {
            if (kind == ContentKind::ui)
                for (std::size_t texel = 0U; texel + 3U < image->rgba.size(); texel += 4U)
                {
                    const unsigned alpha = image->rgba[texel + 3U];
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        image->rgba[texel + channel] = static_cast<std::uint8_t>(
                            (image->rgba[texel + channel] * alpha + 127U) / 255U);
                }
            if (SDL_Surface *surface = surface_from_image(*image))
                return surface;
        }
        std::cerr << "Texture pack: " << replacement.filename().string() << ": " << error
                  << " (using the content pack's)\n";
    }
    return SDL_LoadBMP(path.string().c_str());
}

SDL_Surface *apply_atlas_overrides(SDL_Surface *surface, AtlasLayer layer)
{
    if (surface == nullptr)
        return nullptr;
    const auto tiles = installed_tiles();
    if (tiles == nullptr || tiles->empty())
        return surface;
    std::map<int, modding::graphics::Image> writes;
    switch (layer)
    {
    case AtlasLayer::diffuse:
        writes = tiles->diffuse;
        break;
    case AtlasLayer::normal_spec:
    case AtlasLayer::metal_light: {
        const auto &own =
            layer == AtlasLayer::normal_spec ? tiles->normal_spec : tiles->metal_light;
        const auto neutral = layer == AtlasLayer::normal_spec
                                 ? modding::graphics::flat_normal_spec
                                 : modding::graphics::matte_metal_light;
        writes = own;
        // A replaced tile without maps of its own is drawn flat and matte
        // rather than with the bumps of the tile it replaced.
        for (const auto &[tile, image] : tiles->diffuse)
        {
            (void)image;
            if (writes.contains(tile))
                continue;
            modding::graphics::Image flat;
            flat.width = 1;
            flat.height = 1;
            flat.rgba.assign(neutral.begin(), neutral.end());
            writes.emplace(tile, std::move(flat));
        }
        break;
    }
    }
    if (writes.empty())
        return surface;
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (rgba == nullptr)
        return surface;
    SDL_DestroySurface(surface);
    modding::graphics::Image atlas;
    atlas.width = rgba->w;
    atlas.height = rgba->h;
    atlas.rgba.resize(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h) * 4U);
    for (int y = 0; y < rgba->h; ++y)
        std::memcpy(
            &atlas.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->w) * 4U],
            static_cast<const std::uint8_t *>(rgba->pixels) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->pitch),
            static_cast<std::size_t>(rgba->w) * 4U);
    modding::graphics::apply_tile_overrides(atlas, writes, 8);
    for (int y = 0; y < rgba->h; ++y)
        std::memcpy(
            static_cast<std::uint8_t *>(rgba->pixels) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->pitch),
            &atlas.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->w) * 4U],
            static_cast<std::size_t>(rgba->w) * 4U);
    return rgba;
}

std::vector<modding::graphics::Image> mod_block_tile_images()
{
    std::vector<modding::graphics::Image> images;
    const auto assets = modding::graphics::presentation_assets();
    if (assets == nullptr || assets->block_tiles.textures().empty())
        return images;
    static constexpr std::array<std::string_view, 3> extensions{".png", ".bmp", ".tga"};
    for (const std::string &texture : assets->block_tiles.textures())
    {
        modding::graphics::Image image;
        std::string error;
        if (const auto found = assets->stack.resolve(texture, extensions))
        {
            if (auto loaded = modding::graphics::load_image(found->path, &error, {1024, 8U << 20U}))
                image = std::move(*loaded);
        }
        else
            error = "not found";
        if (image.empty())
            std::cerr << "Mod block texture " << texture << ": " << error << '\n';
        images.push_back(std::move(image)); // an empty slot draws magenta
    }
    return images;
}

} // namespace openminerz
