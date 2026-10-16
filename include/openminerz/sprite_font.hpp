#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz
{

// An XNA SpriteFont as CastleMiner Z ships it, imported into the local content
// pack by tools/ContentBridge (DerivedData/font_<Name>.omfnt). Retail text is
// not "a UI font at a similar size": it is these bitmaps, placed by XNA's
// SpriteFont.MeasureString / DrawString arithmetic, which this class
// reproduces bit for bit (tests/test_presentation_parity.cpp measures the same
// strings through the original SpriteFont).
//
// Ten fonts exist in both builds, at different sizes per build:
//
//   name           1.6.3.8 line/cap   1.9.9.8 line/cap   used for
//   SmallFont          19 / 11            21 / 14        item counts, small labels
//   MedFont            29 / 18            32 / 22        HUD text, block label, lists
//   MedLargeFont       39 / 23            40 / 26        death prompt, descriptions
//   LargeFont          58 / 36            64 / 42        menu items and titles
//   ConsoleFont        25 / 14            25 / 14        chat / broadcast console
//   NameTagFont        25 / 14            25 / 15        player nametags
//   System             28 / 16            28 / 17        achievement pop-up
//   MyriadSmall        20 / 11            20 / 11        chat entry, small controls
//   MyriadMedium       31 / 18            31 / 19        dialogs, buttons
//   MyriadLarge        61 / 36            61 / 37        dialog titles
//
// Every one of them has 222-224 glyphs (Latin-1 plus a few punctuation marks)
// and the default character '*': text is processed one UTF-16 code unit at a
// time, and a unit the font lacks is drawn as an asterisk. Nothing is ever
// substituted from another font.
struct SpriteFontGlyph final
{
    char16_t character{};
    // Source rectangle in the font texture.
    int x{};
    int y{};
    int width{};
    int height{};
    // Cropping: where that rectangle sits inside the character cell.
    int crop_x{};
    int crop_y{};
    int crop_width{};
    int crop_height{};
    // Kerning triple: left side bearing, glyph width, right side bearing.
    float left{};
    float advance{};
    float right{};
};

struct SpriteFontExtent final
{
    float width{};
    float height{};
};

// One glyph of a laid-out string: which glyph, and where the top-left corner of
// its texture rectangle goes relative to the position handed to DrawString
// (before scaling).
struct SpriteFontPlacement final
{
    std::uint32_t glyph{};
    float x{};
    float y{};
};

enum class RetailFont : std::uint8_t
{
    small,
    medium,
    medium_large,
    large,
    console,
    name_tag,
    system,
    myriad_small,
    myriad_medium,
    myriad_large,
    count
};
// The asset name ("MedLargeFont"); the pack file is font_<name>.omfnt.
[[nodiscard]] std::string_view retail_font_name(RetailFont font) noexcept;

class SpriteFont final
{
  public:
    // Returns nothing when the file is absent or not a font export.
    [[nodiscard]] static std::optional<SpriteFont> load(const std::filesystem::path &path);

    [[nodiscard]] int line_spacing() const noexcept { return line_spacing_; }
    [[nodiscard]] float spacing() const noexcept { return spacing_; }
    [[nodiscard]] std::optional<char16_t> default_character() const noexcept
    {
        return default_character_;
    }
    [[nodiscard]] const std::vector<SpriteFontGlyph> &glyphs() const noexcept { return glyphs_; }
    [[nodiscard]] int texture_width() const noexcept { return texture_width_; }
    [[nodiscard]] int texture_height() const noexcept { return texture_height_; }
    // The sheet as XNA samples it: four bytes per texel, row by row,
    // PREMULTIPLIED RGBA. SpriteBatch blends result = texel * tint +
    // destination * (1 - texel.a * tint.a); a renderer must use that blend
    // (SDL_BLENDMODE_BLEND_PREMULTIPLIED) and a premultiplied tint. The sheets
    // are DXT3, so a texel's colour is not simply its alpha.
    [[nodiscard]] const std::vector<std::uint8_t> &pixels() const noexcept { return pixels_; }

    // Height of a capital letter and how far below the cell's top it starts.
    [[nodiscard]] int cap_height() const noexcept;
    [[nodiscard]] int cap_top() const noexcept;

    // SpriteFont.MeasureString. `text` is UTF-8; it is measured as the UTF-16
    // the original holds. A font without a default character cannot place a
    // unit it lacks (XNA throws); such units are skipped here.
    [[nodiscard]] SpriteFontExtent measure(std::string_view text) const;
    // SpriteFont.DrawString's glyph positions, appended to `placements`.
    void layout(std::string_view text, std::vector<SpriteFontPlacement> &placements) const;
    // True when every unit of `text` has its own glyph (nothing becomes '*').
    [[nodiscard]] bool covers(std::string_view text) const;
    // TextRegionElement-style wrapping: break at spaces so no line is wider
    // than `width` pixels at `scale`; explicit '\n' always breaks.
    [[nodiscard]] std::vector<std::string> wrap(std::string_view text, float width,
                                                float scale = 1.0F) const;
    // FNV-1a over the glyph table, in the order and form the probe hashes the
    // original's, so an import can be checked against the shipped font.
    [[nodiscard]] std::uint32_t table_hash() const noexcept;

  private:
    [[nodiscard]] const SpriteFontGlyph *find(char16_t unit, std::uint32_t *index) const noexcept;

    int line_spacing_{};
    float spacing_{};
    std::optional<char16_t> default_character_;
    int texture_width_{};
    int texture_height_{};
    std::vector<SpriteFontGlyph> glyphs_;
    std::vector<std::uint8_t> pixels_;
};

// UTF-8 to the UTF-16 code units the original's strings hold. Malformed input
// becomes U+FFFD, which no shipped font has.
[[nodiscard]] std::u16string utf16_units(std::string_view text);

} // namespace openminerz
