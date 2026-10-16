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

int FontRenderer::pixel_size_for_cap_height(int cap_height)
{
    // Hinted outlines grow in whole pixels, so walk up to the first size whose
    // capital H reaches the requested height.
    for (int pixel_size = 8; pixel_size <= 256; ++pixel_size)
    {
        if (FT_Set_Pixel_Sizes(face_, 0U, static_cast<FT_UInt>(pixel_size)) != 0 ||
            FT_Load_Char(face_, 'H', FT_LOAD_DEFAULT) != 0)
            continue;
        if (static_cast<int>((face_->glyph->metrics.height + 32) >> 6) >= cap_height)
            return pixel_size;
    }
    return std::clamp(cap_height * 10 / 7, 8, 256);
}

int FontRenderer::baseline(int pixel_size)
{
    pixel_size = std::clamp(pixel_size, 8, 256);
    if (FT_Set_Pixel_Sizes(face_, 0U, static_cast<FT_UInt>(pixel_size)) != 0)
        return pixel_size;
    // rasterize() places glyph origins two pixels below the texture's top.
    return 2 + static_cast<int>(face_->size->metrics.ascender >> 6);
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

RetailFonts::RetailFonts(SDL_Renderer *renderer, const std::filesystem::path &derived_data)
    : renderer_(renderer)
{
    if (renderer_ == nullptr)
        return;
    for (std::size_t index = 0U; index < fonts_.size(); ++index)
    {
        const std::string_view name = retail_font_name(static_cast<RetailFont>(index));
        auto font = SpriteFont::load(derived_data / ("font_" + std::string{name} + ".omfnt"));
        if (!font)
            continue;
        // The sheet exactly as shipped: premultiplied RGBA, blended the way
        // SpriteBatch's default BlendState.AlphaBlend does (the source colour
        // is added, the destination attenuated by the source alpha). A
        // straight-alpha blend would square the coverage at glyph edges.
        SDL_Surface *surface =
            SDL_CreateSurface(font->texture_width(), font->texture_height(), SDL_PIXELFORMAT_RGBA32);
        if (surface == nullptr)
            continue;
        const auto &pixels = font->pixels();
        const std::size_t row_bytes = static_cast<std::size_t>(surface->w) * 4U;
        for (int y = 0; y < surface->h; ++y)
            std::memcpy(static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch,
                        pixels.data() + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (texture == nullptr)
            continue;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        // SpriteBatch's default sampler is LinearClamp: unscaled text on whole
        // pixels is exact, scaled text is filtered as the original's is.
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        fonts_[index].emplace(Loaded{std::move(*font), texture, {}, {}});
    }
}

RetailFonts::~RetailFonts()
{
    for (auto &loaded : fonts_)
    {
        if (!loaded)
            continue;
        if (loaded->texture != nullptr)
            SDL_DestroyTexture(loaded->texture);
        for (SDL_Texture *enlarged : loaded->enlarged)
            if (enlarged != nullptr)
                SDL_DestroyTexture(enlarged);
    }
}

// A glyph sheet `factor` times larger with its edges drawn firm again.
//
// The sheet holds antialiased coverage (premultiplied white, so coverage is the
// alpha). Enlarging it with a Catmull-Rom filter keeps each contour where it
// is and smooth; the coverage ramp across an edge is then `factor` texels wide,
// and a smoothstep around one half narrows it back to about one. The result is
// what the same lettering would look like rasterised at the larger size, give
// or take the corners the small bitmap never recorded.
SDL_Texture *RetailFonts::sheet_for(Loaded &loaded, float scale)
{
    if (!sharp_magnification_ || scale <= 1.25F)
        return loaded.texture;
    const std::size_t level = scale > 2.25F ? 1U : 0U;
    if (loaded.enlarged[level] != nullptr)
        return loaded.enlarged[level];
    if (loaded.enlarged_tried[level])
        // The smaller copy if the larger one could not be made, else XNA's.
        return level == 1U && loaded.enlarged[0] != nullptr ? loaded.enlarged[0] : loaded.texture;
    loaded.enlarged_tried[level] = true;

    const int factor = level == 0U ? 2 : 4;
    const int source_width = loaded.font.texture_width();
    const int source_height = loaded.font.texture_height();
    const int width = source_width * factor;
    const int height = source_height * factor;
    if (width > 4096 || height > 4096)
        return loaded.texture;
    const auto &pixels = loaded.font.pixels();
    const auto coverage = [&](int x, int y) {
        x = std::clamp(x, 0, source_width - 1);
        y = std::clamp(y, 0, source_height - 1);
        return static_cast<float>(
                   pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(source_width) +
                           static_cast<std::size_t>(x)) *
                              4U +
                          3U]) /
               255.0F;
    };
    // Catmull-Rom weights for the `factor` sub-positions of one source texel.
    std::vector<std::array<float, 4>> weights(static_cast<std::size_t>(factor));
    for (int phase = 0; phase < factor; ++phase)
    {
        const float t = (static_cast<float>(phase) + 0.5F) / static_cast<float>(factor) + 0.5F;
        const float f = t - std::floor(t);
        weights[static_cast<std::size_t>(phase)] = {
            ((-f + 2.0F) * f - 1.0F) * f * 0.5F, ((3.0F * f - 5.0F) * f * f + 2.0F) * 0.5F,
            ((-3.0F * f + 4.0F) * f + 1.0F) * f * 0.5F, (f - 1.0F) * f * f * 0.5F};
    }
    // Which source texel the four taps start at, for an output index.
    const auto first_tap = [factor](int index) {
        const float position = (static_cast<float>(index) + 0.5F) / static_cast<float>(factor) - 0.5F;
        return static_cast<int>(std::floor(position)) - 1;
    };
    // Rows first (width grows), then columns.
    std::vector<float> wide(static_cast<std::size_t>(width) * static_cast<std::size_t>(source_height));
    for (int y = 0; y < source_height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const int tap = first_tap(x);
            const auto &weight = weights[static_cast<std::size_t>(x % factor)];
            wide[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] =
                coverage(tap, y) * weight[0] + coverage(tap + 1, y) * weight[1] +
                coverage(tap + 2, y) * weight[2] + coverage(tap + 3, y) * weight[3];
        }
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr)
        return loaded.texture;
    // Half the width of the coverage band kept soft: about three quarters of an
    // output texel on either side of the contour.
    const float band = 0.75F / static_cast<float>(factor);
    for (int y = 0; y < height; ++y)
    {
        const int tap = first_tap(y);
        const auto &weight = weights[static_cast<std::size_t>(y % factor)];
        std::array<const float *, 4> rows{};
        for (int offset = 0; offset < 4; ++offset)
            rows[static_cast<std::size_t>(offset)] =
                wide.data() + static_cast<std::size_t>(std::clamp(tap + offset, 0, source_height - 1)) *
                                  static_cast<std::size_t>(width);
        auto *target = static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < width; ++x)
        {
            const float value = rows[0][x] * weight[0] + rows[1][x] * weight[1] +
                                rows[2][x] * weight[2] + rows[3][x] * weight[3];
            const float edge = std::clamp((value - (0.5F - band)) / (2.0F * band), 0.0F, 1.0F);
            const auto alpha =
                static_cast<std::uint8_t>(std::lround(edge * edge * (3.0F - 2.0F * edge) * 255.0F));
            // Premultiplied white, like the sheet it was made from.
            target[x * 4 + 0] = alpha;
            target[x * 4 + 1] = alpha;
            target[x * 4 + 2] = alpha;
            target[x * 4 + 3] = alpha;
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (texture == nullptr)
        return loaded.texture;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    loaded.enlarged[level] = texture;
    return texture;
}

const SpriteFont *RetailFonts::font(RetailFont id) const noexcept
{
    const auto index = static_cast<std::size_t>(id);
    return index < fonts_.size() && fonts_[index] ? &fonts_[index]->font : nullptr;
}

bool RetailFonts::complete() const noexcept
{
    return std::ranges::all_of(fonts_, [](const auto &loaded) { return loaded.has_value(); });
}

TextExtent RetailFonts::measure(RetailFont id, std::string_view text, float scale) const
{
    const SpriteFont *loaded = font(id);
    if (loaded == nullptr)
        return {};
    const SpriteFontExtent extent = loaded->measure(text);
    return {extent.width * scale, extent.height * scale};
}

void RetailFonts::draw(RetailFont id, std::string_view text, float x, float y, SDL_Color color,
                       float scale)
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= fonts_.size() || !fonts_[index] || color.a == 0U || text.empty())
        return;
    Loaded &loaded = *fonts_[index];
    placements_.clear();
    loaded.font.layout(text, placements_);
    if (placements_.empty())
        return;
    vertices_.clear();
    indices_.clear();
    // `color` is a straight colour and opacity; the tint SpriteBatch multiplies
    // a premultiplied texel by is that colour premultiplied.
    const float opacity = static_cast<float>(color.a) / 255.0F;
    const SDL_FColor tint{static_cast<float>(color.r) / 255.0F * opacity,
                          static_cast<float>(color.g) / 255.0F * opacity,
                          static_cast<float>(color.b) / 255.0F * opacity, opacity};
    const float sheet_width = static_cast<float>(loaded.font.texture_width());
    const float sheet_height = static_cast<float>(loaded.font.texture_height());
    const auto &glyphs = loaded.font.glyphs();
    for (const SpriteFontPlacement &placement : placements_)
    {
        const SpriteFontGlyph &glyph = glyphs[placement.glyph];
        if (glyph.width == 0 || glyph.height == 0)
            continue;
        const float left = x + placement.x * scale;
        const float top = y + placement.y * scale;
        const float right = left + static_cast<float>(glyph.width) * scale;
        const float bottom = top + static_cast<float>(glyph.height) * scale;
        const float u0 = static_cast<float>(glyph.x) / sheet_width;
        const float v0 = static_cast<float>(glyph.y) / sheet_height;
        const float u1 = static_cast<float>(glyph.x + glyph.width) / sheet_width;
        const float v1 = static_cast<float>(glyph.y + glyph.height) / sheet_height;
        const int base = static_cast<int>(vertices_.size());
        vertices_.push_back({{left, top}, tint, {u0, v0}});
        vertices_.push_back({{right, top}, tint, {u1, v0}});
        vertices_.push_back({{right, bottom}, tint, {u1, v1}});
        vertices_.push_back({{left, bottom}, tint, {u0, v1}});
        for (const int offset : {0, 1, 2, 0, 2, 3})
            indices_.push_back(base + offset);
    }
    if (!vertices_.empty())
        SDL_RenderGeometry(renderer_, sheet_for(loaded, scale), vertices_.data(),
                           static_cast<int>(vertices_.size()), indices_.data(),
                           static_cast<int>(indices_.size()));
}

void RetailFonts::draw_outlined(RetailFont id, std::string_view text, float x, float y,
                                SDL_Color color, SDL_Color outline, int outline_width, float scale)
{
    const float offset = static_cast<float>(outline_width) * scale;
    draw(id, text, x + offset, y + offset, outline, scale);
    draw(id, text, x - offset, y - offset, outline, scale);
    draw(id, text, x - offset, y + offset, outline, scale);
    draw(id, text, x + offset, y - offset, outline, scale);
    draw(id, text, x, y, color, scale);
}

void RetailFonts::draw_slanted(RetailFont id, std::string_view text, float x, float y,
                               SDL_Color top, SDL_Color bottom, float scale, float slant)
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= fonts_.size() || !fonts_[index] || text.empty())
        return;
    Loaded &loaded = *fonts_[index];
    placements_.clear();
    loaded.font.layout(text, placements_);
    if (placements_.empty())
        return;
    vertices_.clear();
    indices_.clear();
    // The gradient spans the capitals; the slant pivots on the baseline, so
    // the foot of every letter stays where draw() would have put it.
    const float cap_top = static_cast<float>(loaded.font.cap_top());
    const float cap_height = static_cast<float>(std::max(loaded.font.cap_height(), 1));
    const float baseline = y + (cap_top + cap_height) * scale;
    const auto premultiplied = [](SDL_Color color) {
        const float opacity = static_cast<float>(color.a) / 255.0F;
        return SDL_FColor{static_cast<float>(color.r) / 255.0F * opacity,
                          static_cast<float>(color.g) / 255.0F * opacity,
                          static_cast<float>(color.b) / 255.0F * opacity, opacity};
    };
    const SDL_FColor first = premultiplied(top);
    const SDL_FColor last = premultiplied(bottom);
    const auto tint_at = [&](float row) {
        const float amount = std::clamp((row - (y + cap_top * scale)) / (cap_height * scale), 0.0F, 1.0F);
        return SDL_FColor{first.r + (last.r - first.r) * amount, first.g + (last.g - first.g) * amount,
                          first.b + (last.b - first.b) * amount, first.a + (last.a - first.a) * amount};
    };
    const float sheet_width = static_cast<float>(loaded.font.texture_width());
    const float sheet_height = static_cast<float>(loaded.font.texture_height());
    const auto &glyphs = loaded.font.glyphs();
    for (const SpriteFontPlacement &placement : placements_)
    {
        const SpriteFontGlyph &glyph = glyphs[placement.glyph];
        if (glyph.width == 0 || glyph.height == 0)
            continue;
        const float left = x + placement.x * scale;
        const float upper = y + placement.y * scale;
        const float right = left + static_cast<float>(glyph.width) * scale;
        const float lower = upper + static_cast<float>(glyph.height) * scale;
        const float upper_shift = (baseline - upper) * slant;
        const float lower_shift = (baseline - lower) * slant;
        const float u0 = static_cast<float>(glyph.x) / sheet_width;
        const float v0 = static_cast<float>(glyph.y) / sheet_height;
        const float u1 = static_cast<float>(glyph.x + glyph.width) / sheet_width;
        const float v1 = static_cast<float>(glyph.y + glyph.height) / sheet_height;
        const SDL_FColor upper_tint = tint_at(upper);
        const SDL_FColor lower_tint = tint_at(lower);
        const int base = static_cast<int>(vertices_.size());
        vertices_.push_back({{left + upper_shift, upper}, upper_tint, {u0, v0}});
        vertices_.push_back({{right + upper_shift, upper}, upper_tint, {u1, v0}});
        vertices_.push_back({{right + lower_shift, lower}, lower_tint, {u1, v1}});
        vertices_.push_back({{left + lower_shift, lower}, lower_tint, {u0, v1}});
        for (const int offset : {0, 1, 2, 0, 2, 3})
            indices_.push_back(base + offset);
    }
    if (!vertices_.empty())
        SDL_RenderGeometry(renderer_, sheet_for(loaded, scale), vertices_.data(),
                           static_cast<int>(vertices_.size()), indices_.data(),
                           static_cast<int>(indices_.size()));
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
