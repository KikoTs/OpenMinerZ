#pragma once

#include <SDL3/SDL.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openminerz
{

enum class TextAlign
{
    left,
    center,
    right
};

struct TextExtent final
{
    float width{};
    float height{};
};

class FontRenderer final
{
  public:
    FontRenderer(SDL_Renderer *renderer, const std::filesystem::path &font_path);
    ~FontRenderer();

    FontRenderer(const FontRenderer &) = delete;
    FontRenderer &operator=(const FontRenderer &) = delete;

    [[nodiscard]] TextExtent measure(std::string_view text, int pixel_size);
    void draw(std::string_view text, float x, float y, int pixel_size, SDL_Color color,
              TextAlign alignment = TextAlign::left, bool outline = false);
    void clear_cache();

  private:
    struct CachedText final
    {
        SDL_Texture *texture{};
        int width{};
        int height{};
    };

    [[nodiscard]] CachedText &get(std::string_view text, int pixel_size);
    [[nodiscard]] CachedText rasterize(std::string_view text, int pixel_size);

    SDL_Renderer *renderer_{};
    FT_Library library_{};
    FT_Face face_{};
    std::unordered_map<std::string, CachedText> cache_;
};

[[nodiscard]] std::filesystem::path find_default_font();

} // namespace openminerz
