#pragma once

#include <SDL3/SDL.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "openminerz/sprite_font.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    // For text that has to match a bitmap font's size rather than a design
    // size: the pixel size whose capital letters are `cap_height` pixels tall,
    // and how far below the `y` passed to draw() that size's baseline sits.
    [[nodiscard]] int pixel_size_for_cap_height(int cap_height);
    [[nodiscard]] int baseline(int pixel_size);

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

// The ten XNA sprite fonts of one shipped build, as imported into a content
// pack (DerivedData/font_<Name>.omfnt, see tools/ContentBridge). Text drawn
// through this class is the original's: its glyph bitmaps at their authored
// size, placed by SpriteFont's own arithmetic, with '*' for anything the font
// lacks. A pack imported before fonts were exported simply has none; callers
// test font() and fall back to FontRenderer.
//
//   const float scale = ...;   // 1 in 1.6.3.8; Screen.Adjuster.ScaleFactor in 1.9.9.8
//   fonts.draw_outlined(RetailFont::medium, "Dirt", x, y, white, black, 1, scale);
class RetailFonts final
{
  public:
    RetailFonts(SDL_Renderer *renderer, const std::filesystem::path &derived_data);
    ~RetailFonts();

    RetailFonts(const RetailFonts &) = delete;
    RetailFonts &operator=(const RetailFonts &) = delete;

    // Null when the pack has no import of that font.
    [[nodiscard]] const SpriteFont *font(RetailFont id) const noexcept;
    // True when all ten fonts are present.
    [[nodiscard]] bool complete() const noexcept;
    // SpriteFont.MeasureString(text) * scale.
    [[nodiscard]] TextExtent measure(RetailFont id, std::string_view text,
                                     float scale = 1.0F) const;
    // SpriteBatch.DrawString(font, text, {x, y}, color, 0, Vector2.Zero, scale, ...):
    // {x, y} is the top-left corner of the text cell.
    void draw(RetailFont id, std::string_view text, float x, float y, SDL_Color color,
              float scale = 1.0F);
    // DrawingTools.DrawOutlinedText: four diagonal copies in the outline colour,
    // offset by outline_width * scale, then the text.
    void draw_outlined(RetailFont id, std::string_view text, float x, float y, SDL_Color color,
                       SDL_Color outline, int outline_width, float scale = 1.0F);

    // Magnified lettering. XNA enlarges a sprite font by stretching its glyph
    // bitmaps, which is how the originals look above 720p: soft. While this is
    // on, a draw at a scale above 1.25 samples a copy of the sheet enlarged and
    // re-edged ahead of time (two or four times the size, made on first use),
    // so large menu lettering keeps a firm outline at 1080p and 4K. Positions,
    // sizes and measurements do not change. Off - the default, and what the
    // HUD and the original's screens are drawn with - is XNA's own result.
    void set_sharp_magnification(bool enabled) noexcept
    {
        sharp_magnification_ = enabled;
    }
    // One line set as a title: every row of a glyph is shifted right by `slant`
    // times its height above the baseline, and the fill runs from `top` at the
    // top of the line to `bottom` at its foot. {x, y} is the cell's top-left
    // corner, as for draw().
    void draw_slanted(RetailFont id, std::string_view text, float x, float y, SDL_Color top,
                      SDL_Color bottom, float scale, float slant);

  private:
    struct Loaded final
    {
        SpriteFont font;
        SDL_Texture *texture{};
        // The sheet at 2x and 4x for set_sharp_magnification(); null until a
        // draw needs it, and left null if it could not be made.
        std::array<SDL_Texture *, 2> enlarged{};
        std::array<bool, 2> enlarged_tried{};
    };
    // The texture a draw at `scale` samples.
    [[nodiscard]] SDL_Texture *sheet_for(Loaded &loaded, float scale);

    bool sharp_magnification_{};
    SDL_Renderer *renderer_{};
    std::array<std::optional<Loaded>, static_cast<std::size_t>(RetailFont::count)> fonts_;
    std::vector<SpriteFontPlacement> placements_;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

} // namespace openminerz
