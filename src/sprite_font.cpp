#include "openminerz/sprite_font.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace openminerz
{
namespace
{

constexpr std::uint32_t font_magic = 0x465A4D4FU; // "OMZF"
// Version 1 stored one coverage byte per texel; version 2 stores the sheet as
// the premultiplied RGBA XNA samples. Both are read.
constexpr std::uint32_t font_version_coverage = 1U;
constexpr std::uint32_t font_version = 2U;
// Far beyond any shipped font (the Japanese satellite fonts have ~7000 glyphs
// on 2048x2048 sheets); bounds a damaged file before anything is allocated.
constexpr std::uint32_t max_glyphs = 65536U;
constexpr std::int32_t max_texture_side = 8192;

template <typename T> bool read(std::istream &stream, T &value)
{
    stream.read(reinterpret_cast<char *>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

} // namespace

std::string_view retail_font_name(RetailFont font) noexcept
{
    constexpr std::array<std::string_view, static_cast<std::size_t>(RetailFont::count)> names{
        "SmallFont",   "MedFont", "MedLargeFont", "LargeFont",    "ConsoleFont",
        "NameTagFont", "System",  "MyriadSmall",  "MyriadMedium", "MyriadLarge",
    };
    const auto index = static_cast<std::size_t>(font);
    return index < names.size() ? names[index] : std::string_view{};
}

std::u16string utf16_units(std::string_view text)
{
    std::u16string result;
    result.reserve(text.size());
    for (std::size_t index = 0U; index < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if (lead < 0x80U)
        {
            codepoint = lead;
            length = 1U;
        }
        else if ((lead & 0xE0U) == 0xC0U)
        {
            codepoint = lead & 0x1FU;
            length = 2U;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            codepoint = lead & 0x0FU;
            length = 3U;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            codepoint = lead & 0x07U;
            length = 4U;
        }
        bool valid = length != 0U && index + length <= text.size();
        for (std::size_t continuation = 1U; valid && continuation < length; ++continuation)
        {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            valid = (byte & 0xC0U) == 0x80U;
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if (!valid || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
        {
            result.push_back(static_cast<char16_t>(0xFFFDU));
            ++index;
            continue;
        }
        if (codepoint >= 0x10000U)
        {
            codepoint -= 0x10000U;
            result.push_back(static_cast<char16_t>(0xD800U + (codepoint >> 10U)));
            result.push_back(static_cast<char16_t>(0xDC00U + (codepoint & 0x3FFU)));
        }
        else
        {
            result.push_back(static_cast<char16_t>(codepoint));
        }
        index += length;
    }
    return result;
}

std::optional<SpriteFont> SpriteFont::load(const std::filesystem::path &path)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        return std::nullopt;
    std::uint32_t magic{};
    std::uint32_t version{};
    std::int32_t line_spacing{};
    float spacing{};
    std::int32_t default_character{};
    std::int32_t texture_width{};
    std::int32_t texture_height{};
    std::uint32_t glyph_count{};
    if (!read(stream, magic) || !read(stream, version) || magic != font_magic ||
        (version != font_version && version != font_version_coverage) ||
        !read(stream, line_spacing) || !read(stream, spacing) ||
        !read(stream, default_character) || !read(stream, texture_width) ||
        !read(stream, texture_height) || !read(stream, glyph_count))
        return std::nullopt;
    if (glyph_count == 0U || glyph_count > max_glyphs || texture_width <= 0 ||
        texture_height <= 0 || texture_width > max_texture_side ||
        texture_height > max_texture_side || line_spacing < 0 || line_spacing > 4096)
        return std::nullopt;

    SpriteFont font;
    font.line_spacing_ = line_spacing;
    font.spacing_ = spacing;
    if (default_character >= 0 && default_character <= 0xFFFF)
        font.default_character_ = static_cast<char16_t>(default_character);
    font.texture_width_ = texture_width;
    font.texture_height_ = texture_height;
    font.glyphs_.resize(glyph_count);
    for (SpriteFontGlyph &glyph : font.glyphs_)
    {
        std::uint32_t character{};
        std::array<std::int32_t, 8> rectangles{};
        std::array<float, 3> kerning{};
        if (!read(stream, character) || !read(stream, rectangles) || !read(stream, kerning) ||
            character > 0xFFFFU)
            return std::nullopt;
        glyph.character = static_cast<char16_t>(character);
        glyph.x = rectangles[0];
        glyph.y = rectangles[1];
        glyph.width = rectangles[2];
        glyph.height = rectangles[3];
        glyph.crop_x = rectangles[4];
        glyph.crop_y = rectangles[5];
        glyph.crop_width = rectangles[6];
        glyph.crop_height = rectangles[7];
        glyph.left = kerning[0];
        glyph.advance = kerning[1];
        glyph.right = kerning[2];
        // A rectangle outside the sheet would read past the coverage buffer.
        if (glyph.x < 0 || glyph.y < 0 || glyph.width < 0 || glyph.height < 0 ||
            glyph.x + glyph.width > texture_width || glyph.y + glyph.height > texture_height)
            return std::nullopt;
    }
    // SpriteFont.GetIndexForCharacter binary-searches the character map.
    if (!std::ranges::is_sorted(font.glyphs_, {}, &SpriteFontGlyph::character))
        return std::nullopt;
    const std::size_t texels =
        static_cast<std::size_t>(texture_width) * static_cast<std::size_t>(texture_height);
    font.pixels_.resize(texels * 4U);
    if (version == font_version_coverage)
    {
        // An import from before the sheet's colour was kept: treat coverage as
        // premultiplied white (exact wherever a texel's colour equals its alpha).
        std::vector<std::uint8_t> coverage(texels);
        stream.read(reinterpret_cast<char *>(coverage.data()),
                    static_cast<std::streamsize>(coverage.size()));
        for (std::size_t texel = 0U; texel < texels; ++texel)
            for (std::size_t channel = 0U; channel < 4U; ++channel)
                font.pixels_[texel * 4U + channel] = coverage[texel];
    }
    else
    {
        stream.read(reinterpret_cast<char *>(font.pixels_.data()),
                    static_cast<std::streamsize>(font.pixels_.size()));
    }
    if (!stream)
        return std::nullopt;
    return font;
}

const SpriteFontGlyph *SpriteFont::find(char16_t unit, std::uint32_t *index) const noexcept
{
    const auto locate = [this](char16_t wanted) -> const SpriteFontGlyph * {
        const auto found = std::ranges::lower_bound(glyphs_, wanted, {},
                                                    &SpriteFontGlyph::character);
        return found != glyphs_.end() && found->character == wanted ? &*found : nullptr;
    };
    const SpriteFontGlyph *glyph = locate(unit);
    if (glyph == nullptr && default_character_)
        glyph = locate(*default_character_);
    if (glyph != nullptr && index != nullptr)
        *index = static_cast<std::uint32_t>(glyph - glyphs_.data());
    return glyph;
}

int SpriteFont::cap_height() const noexcept
{
    const SpriteFontGlyph *glyph = find(u'H', nullptr);
    return glyph != nullptr ? glyph->height : 0;
}

int SpriteFont::cap_top() const noexcept
{
    const SpriteFontGlyph *glyph = find(u'H', nullptr);
    return glyph != nullptr ? glyph->crop_y : 0;
}

bool SpriteFont::covers(std::string_view text) const
{
    for (const char16_t unit : utf16_units(text))
    {
        if (unit == u'\r' || unit == u'\n')
            continue;
        const auto found =
            std::ranges::lower_bound(glyphs_, unit, {}, &SpriteFontGlyph::character);
        if (found == glyphs_.end() || found->character != unit)
            return false;
    }
    return true;
}

SpriteFontExtent SpriteFont::measure(std::string_view text) const
{
    // SpriteFont.InternalMeasure, statement for statement.
    const std::u16string units = utf16_units(text);
    if (units.empty())
        return {};
    float x = 0.0F;
    float y = static_cast<float>(line_spacing_);
    float widest = 0.0F;
    int lines = 0;
    float trailing = 0.0F;
    bool first = true;
    for (const char16_t unit : units)
    {
        if (unit == u'\r')
            continue;
        if (unit == u'\n')
        {
            x += std::max(trailing, 0.0F);
            trailing = 0.0F;
            widest = std::max(x, widest);
            x = 0.0F;
            y = static_cast<float>(line_spacing_);
            first = true;
            ++lines;
            continue;
        }
        const SpriteFontGlyph *glyph = find(unit, nullptr);
        if (glyph == nullptr)
            continue;
        float left = glyph->left;
        if (first)
            left = std::max(left, 0.0F);
        else
            x += spacing_ + trailing;
        x += left + glyph->advance;
        trailing = glyph->right;
        y = std::max(y, static_cast<float>(glyph->crop_height));
        first = false;
    }
    x += std::max(trailing, 0.0F);
    y += static_cast<float>(lines * line_spacing_);
    return {std::max(x, widest), y};
}

void SpriteFont::layout(std::string_view text, std::vector<SpriteFontPlacement> &placements) const
{
    // SpriteFont.InternalDraw without rotation or flipping.
    float x = 0.0F;
    float y = 0.0F;
    bool first = true;
    for (const char16_t unit : utf16_units(text))
    {
        if (unit == u'\r')
            continue;
        if (unit == u'\n')
        {
            first = true;
            x = 0.0F;
            y += static_cast<float>(line_spacing_);
            continue;
        }
        std::uint32_t index{};
        const SpriteFontGlyph *glyph = find(unit, &index);
        if (glyph == nullptr)
            continue;
        float left = glyph->left;
        if (first)
            left = std::max(left, 0.0F);
        else
            x += spacing_;
        x += left;
        placements.push_back({index, x + static_cast<float>(glyph->crop_x),
                              y + static_cast<float>(glyph->crop_y)});
        first = false;
        x += glyph->advance + glyph->right;
    }
}

std::vector<std::string> SpriteFont::wrap(std::string_view text, float width, float scale) const
{
    std::vector<std::string> lines;
    std::string line;
    const auto flush = [&] {
        lines.push_back(line);
        line.clear();
    };
    std::size_t start = 0U;
    while (start <= text.size())
    {
        const std::size_t newline = std::min(text.find('\n', start), text.size());
        std::string_view paragraph = text.substr(start, newline - start);
        while (!paragraph.empty())
        {
            const std::size_t space = paragraph.find(' ');
            const std::string_view word =
                paragraph.substr(0U, space == std::string_view::npos ? paragraph.size() : space);
            paragraph.remove_prefix(space == std::string_view::npos ? paragraph.size()
                                                                    : space + 1U);
            std::string candidate = line.empty() ? std::string{word} : line + ' ' + std::string{word};
            if (!line.empty() && measure(candidate).width * scale > width)
            {
                flush();
                candidate = std::string{word};
            }
            line = std::move(candidate);
        }
        flush();
        if (newline >= text.size())
            break;
        start = newline + 1U;
    }
    return lines;
}

std::uint32_t SpriteFont::table_hash() const noexcept
{
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](std::uint32_t value) { hash = (hash ^ value) * 16777619U; };
    const auto bits = [](float value) {
        std::uint32_t result{};
        std::memcpy(&result, &value, sizeof(result));
        return result;
    };
    for (const SpriteFontGlyph &glyph : glyphs_)
    {
        mix(glyph.character);
        for (const int value : {glyph.x, glyph.y, glyph.width, glyph.height, glyph.crop_x,
                                glyph.crop_y, glyph.crop_width, glyph.crop_height})
            mix(static_cast<std::uint32_t>(value));
        mix(bits(glyph.left));
        mix(bits(glyph.advance));
        mix(bits(glyph.right));
    }
    return hash;
}

} // namespace openminerz
