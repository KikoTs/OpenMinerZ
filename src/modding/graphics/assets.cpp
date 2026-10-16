// Asset packs: the layered stack of mod assets/ folders, bounded file reads,
// image decoding, and the overrides the content loaders consult
// (openminerz/modding/graphics.hpp).

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

// stb_image, private to this file: every symbol is static, so another module
// that compiles its own copy cannot collide with this one.
#if defined(_MSC_VER)
// C4505 (unused static functions) is reported at the end of the file, after
// the pop below, so it is off for this whole file.
#pragma warning(disable : 4505)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4456 4457 4459 4701 4702 4996 6001 6262 6385 6386)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace openminerz::modding::graphics
{
namespace
{

constexpr std::size_t max_name_length = 200U;
constexpr int max_name_depth = 8;

bool name_character(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.' ||
           value == '/';
}

bool has_extension(std::string_view name, std::span<const std::string_view> extensions)
{
    for (const std::string_view extension : extensions)
        if (name.size() > extension.size() && name.ends_with(extension))
            return true;
    return false;
}

// "@mod/rest" -> {"mod", "rest"}; anything else -> {"", name}.
std::pair<std::string_view, std::string_view> split_pinned(std::string_view name)
{
    if (!name.starts_with('@'))
        return {{}, name};
    const std::size_t slash = name.find('/');
    if (slash == std::string_view::npos)
        return {name.substr(1U), {}};
    return {name.substr(1U, slash - 1U), name.substr(slash + 1U)};
}

std::mutex installed_mutex;
std::shared_ptr<const PresentationAssets> installed_assets;

} // namespace

bool valid_asset_name(std::string_view name, std::string *reason)
{
    const auto refuse = [&](const char *why) {
        if (reason != nullptr)
            *reason = why;
        return false;
    };
    if (name.empty())
        return refuse("empty asset name");
    if (name.size() > max_name_length)
        return refuse("asset name too long");
    if (name.front() == '/')
        return refuse("asset names are relative to the mod's assets folder");
    int depth = 0;
    std::size_t start = 0U;
    while (start <= name.size())
    {
        const std::size_t end = std::min(name.find('/', start), name.size());
        const std::string_view part = name.substr(start, end - start);
        if (part.empty())
            return refuse("empty path component");
        if (part == "." || part == "..")
            return refuse("'.' and '..' are not allowed in asset names");
        // (Polish) Hidden files are not part of a mod (net::is_ignored_mod_entry).
        if (part.front() == '.')
            return refuse("names starting with '.' are hidden files the game ignores");
        for (const char value : part)
            if (!name_character(value))
                return refuse("asset names use only letters, digits, '_', '-', '.' and '/'");
        if (++depth > max_name_depth)
            return refuse("asset name nested too deeply");
        start = end + 1U;
    }
    return true;
}

std::optional<std::filesystem::path> contained_path(const std::filesystem::path &root,
                                                    std::string_view name)
{
    if (!valid_asset_name(name))
        return std::nullopt;
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(
        canonical_root / std::filesystem::path{std::string{name}}, error);
    if (error)
        return std::nullopt;
    // Lexically inside the root after links were followed.
    const auto relative = candidate.lexically_relative(canonical_root);
    if (relative.empty() || relative.is_absolute())
        return std::nullopt;
    const auto first = relative.begin();
    if (first == relative.end() || *first == "..")
        return std::nullopt;
    return candidate;
}

void AssetStack::set_sources(std::vector<AssetSource> sources)
{
    sources_.clear();
    std::set<std::string> seen;
    for (AssetSource &source : sources)
    {
        std::error_code error;
        if (source.mod_id.empty() || !std::filesystem::is_directory(source.root, error))
            continue;
        // A mod listed twice keeps its later (higher) position.
        if (!seen.insert(source.mod_id).second)
            std::erase_if(sources_,
                          [&](const AssetSource &other) { return other.mod_id == source.mod_id; });
        sources_.push_back(std::move(source));
    }
    ++generation_;
}

void AssetStack::clear() noexcept
{
    if (!sources_.empty())
        ++generation_;
    sources_.clear();
}

std::optional<AssetStack::Resolved> AssetStack::resolve(
    std::string_view name, std::span<const std::string_view> extensions) const
{
    const auto [pinned, relative] = split_pinned(name);
    if (!valid_asset_name(relative))
        return std::nullopt;
    std::vector<std::string> candidates;
    if (extensions.empty() || has_extension(relative, extensions))
        candidates.emplace_back(relative);
    if (!has_extension(relative, extensions))
        for (const std::string_view extension : extensions)
            candidates.push_back(std::string{relative} + std::string{extension});
    for (auto source = sources_.rbegin(); source != sources_.rend(); ++source)
    {
        if (!pinned.empty() && source->mod_id != pinned)
            continue;
        for (const std::string &candidate : candidates)
        {
            const auto path = contained_path(source->root, candidate);
            std::error_code error;
            if (path && std::filesystem::is_regular_file(*path, error))
                return Resolved{*path, source->mod_id, candidate};
        }
    }
    return std::nullopt;
}

std::vector<AssetStack::Resolved> AssetStack::list(
    std::string_view folder, std::span<const std::string_view> extensions) const
{
    std::map<std::string, Resolved> found;
    if (!valid_asset_name(folder))
        return {};
    for (const AssetSource &source : sources_)
    {
        const auto directory = contained_path(source.root, folder);
        std::error_code error;
        if (!directory || !std::filesystem::is_directory(*directory, error))
            continue;
        std::size_t visited = 0U;
        for (std::filesystem::directory_iterator entry{*directory, error}, end;
             !error && entry != end && visited < 4096U; entry.increment(error), ++visited)
        {
            if (!entry->is_regular_file(error))
                continue;
            // Only plain ASCII names can be asset names; anything else is
            // skipped before a narrowing conversion could throw.
            const std::wstring wide = entry->path().filename().wstring();
            if (wide.empty() || !std::all_of(wide.begin(), wide.end(), [](wchar_t value) {
                    return value > 0 && value < 128;
                }))
                continue;
            std::string file;
            file.reserve(wide.size());
            for (const wchar_t value : wide)
                file.push_back(static_cast<char>(value));
            const std::string name = std::string{folder} + "/" + file;
            if (!valid_asset_name(name) || !has_extension(file, extensions))
                continue;
            // Later sources replace earlier ones.
            found.insert_or_assign(name, Resolved{entry->path(), source.mod_id, name});
        }
    }
    std::vector<Resolved> result;
    result.reserve(found.size());
    for (auto &[name, resolved] : found)
        result.push_back(std::move(resolved));
    return result;
}

std::optional<std::vector<std::uint8_t>> read_file_bounded(const std::filesystem::path &path,
                                                           std::size_t max_bytes,
                                                           std::string *error)
{
    const auto fail = [&](std::string message) -> std::optional<std::vector<std::uint8_t>> {
        if (error != nullptr)
            *error = std::move(message);
        return std::nullopt;
    };
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error)
        return fail("cannot read " + path.filename().string());
    if (size > max_bytes)
        return fail(path.filename().string() + " is larger than " +
                    std::to_string(max_bytes / (1024U * 1024U)) + " MB");
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        return fail("cannot open " + path.filename().string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream && !bytes.empty())
        return fail("short read of " + path.filename().string());
    return bytes;
}

// --- Images ----------------------------------------------------------------

std::optional<Image> decode_image(std::span<const std::uint8_t> bytes, std::string *error,
                                  const ImageLimits &limits)
{
    const auto fail = [&](std::string message) -> std::optional<Image> {
        if (error != nullptr)
            *error = std::move(message);
        return std::nullopt;
    };
    if (bytes.empty())
        return fail("empty image");
    if (bytes.size() > limits.max_file_bytes || bytes.size() > 0x7FFFFFFFU)
        return fail("image file too large");
    int width{};
    int height{};
    int components{};
    const int length = static_cast<int>(bytes.size());
    // The header first: a huge declared size is refused before any allocation.
    if (stbi_info_from_memory(bytes.data(), length, &width, &height, &components) == 0)
        return fail(std::string{"unsupported or corrupt image ("} + stbi_failure_reason() + ")");
    if (width <= 0 || height <= 0 || width > limits.max_side || height > limits.max_side)
        return fail("image is " + std::to_string(width) + "x" + std::to_string(height) +
                    "; the limit is " + std::to_string(limits.max_side) + " per side");
    stbi_uc *pixels = stbi_load_from_memory(bytes.data(), length, &width, &height, &components, 4);
    if (pixels == nullptr)
        return fail(std::string{"image decode failed ("} + stbi_failure_reason() + ")");
    Image image;
    image.width = width;
    image.height = height;
    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    image.rgba.assign(pixels, pixels + count);
    stbi_image_free(pixels);
    return image;
}

std::optional<Image> load_image(const std::filesystem::path &path, std::string *error,
                                const ImageLimits &limits)
{
    const auto bytes = read_file_bounded(path, limits.max_file_bytes, error);
    if (!bytes)
        return std::nullopt;
    auto image = decode_image(*bytes, error, limits);
    if (!image && error != nullptr)
        *error = path.filename().string() + ": " + *error;
    return image;
}

Image resample(const Image &source, int width, int height)
{
    Image result;
    if (source.empty() || width <= 0 || height <= 0)
        return result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    if (width == source.width && height == source.height)
    {
        result.rgba = source.rgba;
        return result;
    }
    const auto texel = [&](int x, int y, int channel) {
        x = std::clamp(x, 0, source.width - 1);
        y = std::clamp(y, 0, source.height - 1);
        return static_cast<float>(
            source.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) +
                         static_cast<std::size_t>(x)) *
                            4U +
                        static_cast<std::size_t>(channel)]);
    };
    const float scale_x = static_cast<float>(source.width) / static_cast<float>(width);
    const float scale_y = static_cast<float>(source.height) / static_cast<float>(height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            std::array<float, 4> value{};
            if (scale_x >= 1.0F && scale_y >= 1.0F)
            {
                // Box: the mean of every source texel the target texel covers
                // (alpha-weighted, so transparent edges do not darken colour).
                const int x0 = static_cast<int>(std::floor(static_cast<float>(x) * scale_x));
                const int y0 = static_cast<int>(std::floor(static_cast<float>(y) * scale_y));
                const int x1 = std::max(
                    x0 + 1, static_cast<int>(std::floor(static_cast<float>(x + 1) * scale_x)));
                const int y1 = std::max(
                    y0 + 1, static_cast<int>(std::floor(static_cast<float>(y + 1) * scale_y)));
                float weight{};
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                    {
                        const float alpha = texel(sx, sy, 3);
                        const float share = (alpha + 1.0F) / 256.0F;
                        for (int channel = 0; channel < 3; ++channel)
                            value[static_cast<std::size_t>(channel)] +=
                                texel(sx, sy, channel) * share;
                        value[3] += alpha;
                        weight += share;
                    }
                const float count = static_cast<float>((x1 - x0) * (y1 - y0));
                for (int channel = 0; channel < 3; ++channel)
                    value[static_cast<std::size_t>(channel)] /= std::max(weight, 1.0e-6F);
                value[3] /= count;
            }
            else
            {
                const float fx = (static_cast<float>(x) + 0.5F) * scale_x - 0.5F;
                const float fy = (static_cast<float>(y) + 0.5F) * scale_y - 0.5F;
                const int ix = static_cast<int>(std::floor(fx));
                const int iy = static_cast<int>(std::floor(fy));
                const float tx = fx - static_cast<float>(ix);
                const float ty = fy - static_cast<float>(iy);
                for (int channel = 0; channel < 4; ++channel)
                {
                    const float top =
                        texel(ix, iy, channel) * (1.0F - tx) + texel(ix + 1, iy, channel) * tx;
                    const float bottom = texel(ix, iy + 1, channel) * (1.0F - tx) +
                                         texel(ix + 1, iy + 1, channel) * tx;
                    value[static_cast<std::size_t>(channel)] = top * (1.0F - ty) + bottom * ty;
                }
            }
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) *
                                   4U;
            for (std::size_t channel = 0U; channel < 4U; ++channel)
                result.rgba[at + channel] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(value[channel]), 0L, 255L));
        }
    return result;
}

void blit(Image &target, const Image &source, int x, int y)
{
    if (target.empty() || source.empty())
        return;
    for (int row = 0; row < source.height; ++row)
    {
        const int ty = y + row;
        if (ty < 0 || ty >= target.height)
            continue;
        const int first = std::max(0, -x);
        const int last = std::min(source.width, target.width - x);
        if (last <= first)
            continue;
        std::memcpy(
            &target.rgba[(static_cast<std::size_t>(ty) * static_cast<std::size_t>(target.width) +
                          static_cast<std::size_t>(x + first)) *
                         4U],
            &source.rgba[(static_cast<std::size_t>(row) * static_cast<std::size_t>(source.width) +
                          static_cast<std::size_t>(first)) *
                         4U],
            static_cast<std::size_t>(last - first) * 4U);
    }
}

// --- Content-pack overrides ------------------------------------------------

TileOverrides collect_tile_overrides(const AssetStack &stack, std::vector<std::string> *warnings)
{
    TileOverrides overrides;
    if (stack.empty())
        return overrides;
    static constexpr std::array<std::string_view, 3> extensions{".png", ".bmp", ".tga"};
    for (const AssetStack::Resolved &file : stack.list("textures/blocks", extensions))
    {
        std::string stem = std::filesystem::path{file.name}.stem().string();
        std::map<int, Image> *target = &overrides.diffuse;
        if (stem.ends_with("_n"))
        {
            target = &overrides.normal_spec;
            stem.resize(stem.size() - 2U);
        }
        else if (stem.ends_with("_m"))
        {
            target = &overrides.metal_light;
            stem.resize(stem.size() - 2U);
        }
        if (stem.empty() || stem.size() > 2U ||
            !std::all_of(stem.begin(), stem.end(),
                         [](char value) { return value >= '0' && value <= '9'; }))
        {
            if (warnings != nullptr)
                warnings->push_back(file.name + ": block textures are named by atlas tile (0..63)");
            continue;
        }
        const int tile = std::stoi(stem);
        if (tile < 0 || tile >= 64)
        {
            if (warnings != nullptr)
                warnings->push_back(file.name + ": tile out of range (0..63)");
            continue;
        }
        std::string error;
        auto image = load_image(file.path, &error, {1024, 8U * 1024U * 1024U});
        if (!image)
        {
            if (warnings != nullptr)
                warnings->push_back(file.mod_id + "/" + file.name + ": " + error);
            continue;
        }
        target->insert_or_assign(tile, std::move(*image));
    }
    return overrides;
}

void apply_tile_overrides(Image &atlas, const std::map<int, Image> &tiles, int columns)
{
    if (atlas.empty() || columns <= 0)
        return;
    const int tile_width = atlas.width / columns;
    const int tile_height = atlas.height / columns;
    if (tile_width <= 0 || tile_height <= 0)
        return;
    for (const auto &[tile, image] : tiles)
    {
        if (tile < 0 || tile >= columns * columns || image.empty())
            continue;
        blit(atlas, resample(image, tile_width, tile_height), (tile % columns) * tile_width,
             (tile / columns) * tile_height);
    }
}

std::optional<std::uint8_t> BlockTextureTable::tile_for(std::string_view texture)
{
    if (const auto existing = find(texture))
        return existing;
    if (textures_.size() >= static_cast<std::size_t>(max_mod_tiles) || texture.empty())
        return std::nullopt;
    textures_.emplace_back(texture);
    return static_cast<std::uint8_t>(first_mod_tile + static_cast<int>(textures_.size()) - 1);
}

std::optional<std::uint8_t> BlockTextureTable::find(std::string_view texture) const
{
    for (std::size_t index = 0U; index < textures_.size(); ++index)
        if (textures_[index] == texture)
            return static_cast<std::uint8_t>(first_mod_tile + static_cast<int>(index));
    return std::nullopt;
}

void BlockTextureTable::clear() noexcept
{
    textures_.clear();
}

void install_presentation_assets(std::shared_ptr<const PresentationAssets> assets)
{
    const std::lock_guard lock{installed_mutex};
    installed_assets = std::move(assets);
}

std::shared_ptr<const PresentationAssets> presentation_assets()
{
    const std::lock_guard lock{installed_mutex};
    return installed_assets;
}

std::filesystem::path derived_override(std::string_view kind, std::string_view file_name)
{
    const auto assets = presentation_assets();
    if (assets == nullptr || assets->stack.empty())
        return {};
    const std::string stem = std::filesystem::path{std::string{file_name}}.stem().string();
    static constexpr std::array<std::string_view, 3> extensions{".png", ".bmp", ".tga"};
    const auto found =
        assets->stack.resolve("textures/" + std::string{kind} + "/" + stem, extensions);
    return found ? found->path : std::filesystem::path{};
}

std::filesystem::path cue_override(std::string_view cue)
{
    const auto assets = presentation_assets();
    if (assets == nullptr || assets->stack.empty())
        return {};
    static constexpr std::array<std::string_view, 2> extensions{".ogg", ".wav"};
    const auto found = assets->stack.resolve("sounds/cues/" + std::string{cue}, extensions);
    return found ? found->path : std::filesystem::path{};
}

} // namespace openminerz::modding::graphics
