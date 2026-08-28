#include "font_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace openminerz
{
namespace
{

std::vector<std::uint32_t> decode_utf8(std::string_view text)
{
    std::vector<std::uint32_t> result;
    result.reserve(text.size());
    for (std::size_t index = 0U; index < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if ((lead & 0x80U) == 0U)
        {
            codepoint = lead;
            length = 1U;
        }
        else if ((lead & 0xE0U) == 0xC0U && index + 1U < text.size())
        {
            codepoint = lead & 0x1FU;
            length = 2U;
        }
        else if ((lead & 0xF0U) == 0xE0U && index + 2U < text.size())
        {
            codepoint = lead & 0x0FU;
            length = 3U;
        }
        else if ((lead & 0xF8U) == 0xF0U && index + 3U < text.size())
        {
            codepoint = lead & 0x07U;
            length = 4U;
        }
        else
        {
            result.push_back(0xFFFDU);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t continuation = 1U; continuation < length; ++continuation)
        {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            if ((byte & 0xC0U) != 0x80U)
            {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        result.push_back(valid ? codepoint : 0xFFFDU);
        index += valid ? length : 1U;
    }
    return result;
}

std::string cache_key(std::string_view text, int pixel_size)
{
    return std::to_string(pixel_size) + ':' + std::string{text};
}

} // namespace

FontRenderer::FontRenderer(SDL_Renderer *renderer, const std::filesystem::path &font_path)
    : renderer_(renderer)
{
    if (renderer_ == nullptr)
    {
        throw std::invalid_argument{"font renderer needs a valid SDL renderer"};
    }
    if (FT_Init_FreeType(&library_) != 0)
    {
        throw std::runtime_error{"FreeType initialization failed"};
    }
    const std::string path = font_path.string();
    if (FT_New_Face(library_, path.c_str(), 0, &face_) != 0)
    {
        FT_Done_FreeType(library_);
        library_ = nullptr;
        throw std::runtime_error{"could not load font: " + path};
    }
}

FontRenderer::~FontRenderer()
{
    clear_cache();
    if (face_ != nullptr)
    {
        FT_Done_Face(face_);
    }
    if (library_ != nullptr)
    {
        FT_Done_FreeType(library_);
    }
}

TextExtent FontRenderer::measure(std::string_view text, int pixel_size)
{
    const auto &cached = get(text, pixel_size);
    return {static_cast<float>(cached.width), static_cast<float>(cached.height)};
}

void FontRenderer::draw(std::string_view text, float x, float y, int pixel_size, SDL_Color color,
                        TextAlign alignment, bool outline)
{
    auto &cached = get(text, pixel_size);
    float left = x;
    if (alignment == TextAlign::center)
    {
        left -= static_cast<float>(cached.width) * 0.5F;
    }
    else if (alignment == TextAlign::right)
    {
        left -= static_cast<float>(cached.width);
    }
    const SDL_FRect destination{left, y, static_cast<float>(cached.width),
                                static_cast<float>(cached.height)};

    if (outline && color.a > 0U)
    {
        const float offset = std::max(1.0F, std::round(static_cast<float>(pixel_size) * 0.035F));
        SDL_SetTextureColorMod(cached.texture, 4U, 6U, 8U);
        SDL_SetTextureAlphaMod(cached.texture, color.a);
        constexpr std::array<std::pair<float, float>, 8U> directions{{
            {-1.0F, -1.0F},
            {0.0F, -1.0F},
            {1.0F, -1.0F},
            {-1.0F, 0.0F},
            {1.0F, 0.0F},
            {-1.0F, 1.0F},
            {0.0F, 1.0F},
            {1.0F, 1.0F},
        }};
        for (const auto &[dx, dy] : directions)
        {
            SDL_FRect shadow = destination;
            shadow.x += dx * offset;
            shadow.y += dy * offset;
            SDL_RenderTexture(renderer_, cached.texture, nullptr, &shadow);
        }
    }

    SDL_SetTextureColorMod(cached.texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(cached.texture, color.a);
    SDL_RenderTexture(renderer_, cached.texture, nullptr, &destination);
}

void FontRenderer::clear_cache()
{
    for (auto &[key, cached] : cache_)
    {
        static_cast<void>(key);
        SDL_DestroyTexture(cached.texture);
    }
    cache_.clear();
}

FontRenderer::CachedText &FontRenderer::get(std::string_view text, int pixel_size)
{
    pixel_size = std::clamp(pixel_size, 8, 256);
    const std::string key = cache_key(text, pixel_size);
    const auto found = cache_.find(key);
    if (found != cache_.end())
    {
        return found->second;
    }
    return cache_.emplace(key, rasterize(text, pixel_size)).first->second;
}

FontRenderer::CachedText FontRenderer::rasterize(std::string_view text, int pixel_size)
{
    if (FT_Set_Pixel_Sizes(face_, 0U, static_cast<FT_UInt>(pixel_size)) != 0)
    {
        throw std::runtime_error{"FreeType rejected the requested font size"};
    }

    const auto codepoints = decode_utf8(text);
    const int ascender = static_cast<int>(face_->size->metrics.ascender >> 6);
    const int line_height = std::max(1, static_cast<int>(face_->size->metrics.height >> 6));
    int width = 2;
    FT_UInt previous{};
    for (const std::uint32_t codepoint : codepoints)
    {
        const FT_UInt glyph = FT_Get_Char_Index(face_, codepoint);
        if (previous != 0U && glyph != 0U && FT_HAS_KERNING(face_))
        {
            FT_Vector kerning{};
            FT_Get_Kerning(face_, previous, glyph, FT_KERNING_DEFAULT, &kerning);
            width += static_cast<int>(kerning.x >> 6);
        }
        if (FT_Load_Glyph(face_, glyph, FT_LOAD_DEFAULT) == 0)
        {
            width += static_cast<int>(face_->glyph->advance.x >> 6);
        }
        previous = glyph;
    }
    width = std::max(width + 2, 2);
    const int height = line_height + 4;

    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr)
    {
        throw std::runtime_error{"SDL could not allocate a text surface"};
    }
    std::memset(surface->pixels, 0, static_cast<std::size_t>(surface->pitch * surface->h));

    int pen_x = 1;
    previous = 0U;
    auto *destination = static_cast<std::uint8_t *>(surface->pixels);
    for (const std::uint32_t codepoint : codepoints)
    {
        const FT_UInt glyph = FT_Get_Char_Index(face_, codepoint);
        if (previous != 0U && glyph != 0U && FT_HAS_KERNING(face_))
        {
            FT_Vector kerning{};
            FT_Get_Kerning(face_, previous, glyph, FT_KERNING_DEFAULT, &kerning);
            pen_x += static_cast<int>(kerning.x >> 6);
        }
        if (FT_Load_Glyph(face_, glyph, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) != 0)
        {
            previous = glyph;
            continue;
        }

        const FT_Bitmap &bitmap = face_->glyph->bitmap;
        const int origin_x = pen_x + face_->glyph->bitmap_left;
        const int origin_y = 2 + ascender - face_->glyph->bitmap_top;
        for (unsigned int row = 0U; row < bitmap.rows; ++row)
        {
            const int y = origin_y + static_cast<int>(row);
            if (y < 0 || y >= height)
            {
                continue;
            }
            const auto *source_row =
                bitmap.buffer + (bitmap.pitch >= 0
                                     ? static_cast<int>(row) * bitmap.pitch
                                     : static_cast<int>(bitmap.rows - 1U - row) * -bitmap.pitch);
            for (unsigned int column = 0U; column < bitmap.width; ++column)
            {
                const int x = origin_x + static_cast<int>(column);
                if (x < 0 || x >= width)
                {
                    continue;
                }
                const std::size_t offset = static_cast<std::size_t>(y * surface->pitch + x * 4);
                const std::uint8_t alpha = source_row[column];
                destination[offset] = 255U;
                destination[offset + 1U] = 255U;
                destination[offset + 2U] = 255U;
                destination[offset + 3U] = std::max(destination[offset + 3U], alpha);
            }
        }
        pen_x += static_cast<int>(face_->glyph->advance.x >> 6);
        previous = glyph;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (texture == nullptr)
    {
        throw std::runtime_error{"SDL could not upload a text texture"};
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return {texture, width, height};
}

std::filesystem::path find_default_font()
{
    constexpr std::array candidates{
        "C:/Windows/Fonts/bahnschrift.ttf",
        "C:/Windows/Fonts/seguisb.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    for (const char *candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }
    throw std::runtime_error{"no supported system UI font was found"};
}

} // namespace openminerz
