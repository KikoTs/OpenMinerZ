// mod-world: the SDK's world generator tools (world_tools.hpp).

#include "world_tools.hpp"

#include "sdk_session.hpp"

#include "openminerz/world/custom_terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <thread>

namespace openminerz::mod_sdk
{
namespace
{

struct Rgb final
{
    double r{};
    double g{};
    double b{};
};

[[nodiscard]] Rgb retail_color(world::BiomeKind kind) noexcept
{
    switch (kind)
    {
    case world::BiomeKind::classic:
        return {96, 140, 70};
    case world::BiomeKind::lagoon:
        return {70, 150, 160};
    case world::BiomeKind::desert:
        return {222, 200, 140};
    case world::BiomeKind::mountain:
        return {132, 130, 136};
    case world::BiomeKind::arctic:
        return {232, 238, 245};
    case world::BiomeKind::descent:
        return {92, 70, 92};
    }
    return {128, 128, 128};
}

// A mod biome's map colour: its top surface block's (a mod block's texture
// mean when known), or a hue from its key when it has no surface.
[[nodiscard]] Rgb key_color(std::string_view key) noexcept;

[[nodiscard]] Rgb biome_color(const world::TerrainBiome &biome, const WorldMapRequest &request) noexcept
{
    if (biome.surface.empty())
        return key_color(biome.key);
    const world::TerrainBlock top = biome.surface.front().block;
    if (top.mod != 0U && top.mod <= request.palette_colors.size() && request.palette_colors[top.mod - 1U])
    {
        const auto &rgb = *request.palette_colors[top.mod - 1U];
        return {static_cast<double>(rgb[0]), static_cast<double>(rgb[1]), static_cast<double>(rgb[2])};
    }
    const world::BlockColor color = world::fallback_color(top.block);
    return {static_cast<double>(color.red), static_cast<double>(color.green), static_cast<double>(color.blue)};
}

[[nodiscard]] Rgb key_color(std::string_view key) noexcept
{
    std::uint32_t hash = 2166136261U;
    for (const char character : key)
    {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619U;
    }
    const double hue = static_cast<double>(hash % 360U) / 60.0;
    const double saturation = 0.5;
    const double value = 0.85;
    const double chroma = value * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
    Rgb out;
    switch (static_cast<int>(hue))
    {
    case 0:
        out = {chroma, x, 0};
        break;
    case 1:
        out = {x, chroma, 0};
        break;
    case 2:
        out = {0, chroma, x};
        break;
    case 3:
        out = {0, x, chroma};
        break;
    case 4:
        out = {x, 0, chroma};
        break;
    default:
        out = {chroma, 0, x};
        break;
    }
    const double m = value - chroma;
    return {(out.r + m) * 255.0, (out.g + m) * 255.0, (out.b + m) * 255.0};
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t size, std::uint32_t crc = 0U)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t n = 0U; n < 256U; ++n)
        {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            result[n] = c;
        }
        return result;
    }();
    crc = ~crc;
    for (std::size_t index = 0U; index < size; ++index)
        crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    return ~crc;
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void chunk(std::vector<std::uint8_t> &out, const char *type, const std::vector<std::uint8_t> &data)
{
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

} // namespace

bool write_png(const std::filesystem::path &path, int width, int height, const std::vector<std::uint8_t> &rgb)
{
    if (width <= 0 || height <= 0 ||
        rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U)
        return false;
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(width) * 3U + 1U) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0U); // no filter
        const auto row = rgb.begin() + static_cast<std::ptrdiff_t>(y) * width * 3;
        raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(width) * 3);
    }
    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : raw)
    {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    for (std::size_t offset = 0U; offset < raw.size(); offset += 65535U)
    {
        const std::size_t length = std::min<std::size_t>(65535U, raw.size() - offset);
        zlib.push_back(offset + length >= raw.size() ? 1U : 0U);
        zlib.push_back(static_cast<std::uint8_t>(length & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>((~length >> 8U) & 0xFFU));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    put32(zlib, (b << 16U) | a);
    std::vector<std::uint8_t> png{0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, static_cast<std::uint32_t>(width));
    put32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8U, 2U, 0U, 0U, 0U}); // 8-bit RGB
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    std::error_code ignored;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(output);
}

WorldMap render_world_map(const world::WorldGenerator &generator, const WorldMapRequest &request)
{
    WorldMap map;
    map.pixels = std::clamp(request.pixels, 16, 8192);
    const int pixels = map.pixels;
    const double step = request.size / static_cast<double>(pixels);
    const auto &terrain = generator.custom_terrain();
    const int sea_level = terrain != nullptr ? terrain->definition().sea_level : 1;
    std::vector<float> heights(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixels));
    std::vector<Rgb> colors(heights.size());
    std::vector<std::int32_t> dominant(heights.size());
    const auto row = [&](int pz) {
        for (int px = 0; px < pixels; ++px)
        {
            const int x = static_cast<int>(std::floor(request.center_x + (px + 0.5 - pixels / 2.0) * step));
            const int z = static_cast<int>(std::floor(request.center_z + (pz + 0.5 - pixels / 2.0) * step));
            const std::size_t index = static_cast<std::size_t>(pz) * static_cast<std::size_t>(pixels) +
                                      static_cast<std::size_t>(px);
            if (terrain == nullptr)
            {
                const world::BiomeKind kind = generator.biome_at(x, z);
                colors[index] = retail_color(kind);
                dominant[index] = -1 - static_cast<int>(kind);
                heights[index] = static_cast<float>(generator.surface_column(x, z, false).top_local_y +
                                                    world::world_min_y);
                continue;
            }
            std::array<world::BiomeWeight, world::maximum_biome_weights> weights{};
            const std::size_t count = terrain->biome_weights(generator, x, z, weights);
            Rgb color;
            for (std::size_t entry = 0U; entry < count; ++entry)
            {
                const world::TerrainBiome &biome = terrain->definition().biomes[weights[entry].biome];
                const Rgb part = biome.retail == world::RetailReuse::world
                                     ? retail_color(generator.retail_biome_at(x, z))
                                 : biome.retail != world::RetailReuse::none ? retail_color(biome.like)
                                                                             : biome_color(biome, request);
                color.r += part.r * weights[entry].weight;
                color.g += part.g * weights[entry].weight;
                color.b += part.b * weights[entry].weight;
            }
            colors[index] = color;
            dominant[index] = weights[0].biome;
            heights[index] = static_cast<float>(terrain->surface_height(generator, x, z));
        }
    };
    const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1U, 32U);
    std::vector<std::thread> threads;
    for (unsigned worker = 0U; worker < workers; ++worker)
        threads.emplace_back([&, worker] {
            for (int pz = static_cast<int>(worker); pz < pixels; pz += static_cast<int>(workers))
                row(pz);
        });
    for (std::thread &thread : threads)
        thread.join();

    map.rgb.resize(heights.size() * 3U);
    const auto height_at = [&](int px, int pz) {
        px = std::clamp(px, 0, pixels - 1);
        pz = std::clamp(pz, 0, pixels - 1);
        return heights[static_cast<std::size_t>(pz) * static_cast<std::size_t>(pixels) + static_cast<std::size_t>(px)];
    };
    for (int pz = 0; pz < pixels; ++pz)
        for (int px = 0; px < pixels; ++px)
        {
            const std::size_t index = static_cast<std::size_t>(pz) * static_cast<std::size_t>(pixels) +
                                      static_cast<std::size_t>(px);
            // Light from the north-west, and a little brighter higher up.
            const double slope = ((height_at(px - 1, pz) - height_at(px + 1, pz)) +
                                  (height_at(px, pz - 1) - height_at(px, pz + 1))) /
                                 (2.0 * std::max(step, 1.0));
            const double shade = std::clamp(0.82 + slope * 0.6, 0.45, 1.25) *
                                 (0.8 + 0.2 * std::clamp((heights[index] + 20.0) / 80.0, 0.0, 1.0));
            Rgb color = colors[index];
            if (heights[index] < static_cast<float>(sea_level))
                color = {color.r * 0.45 + 40.0 * 0.55, color.g * 0.45 + 90.0 * 0.55, color.b * 0.45 + 160.0 * 0.55};
            if (heights[index] < static_cast<float>(world::world_min_y + 1))
                color = {20.0, 16.0, 24.0}; // nothing but the bottom (retail's void past the descent)
            map.rgb[index * 3U] = static_cast<std::uint8_t>(std::clamp(color.r * shade, 0.0, 255.0));
            map.rgb[index * 3U + 1U] = static_cast<std::uint8_t>(std::clamp(color.g * shade, 0.0, 255.0));
            map.rgb[index * 3U + 2U] = static_cast<std::uint8_t>(std::clamp(color.b * shade, 0.0, 255.0));
        }
    // The origin (black) and a new world's spawn (red).
    const auto mark = [&](double x, double z, int radius, std::array<std::uint8_t, 3> color) {
        const int cx = static_cast<int>(std::floor((x - request.center_x) / step + pixels / 2.0));
        const int cz = static_cast<int>(std::floor((z - request.center_z) / step + pixels / 2.0));
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const int px = cx + dx;
                const int pz = cz + dz;
                if (px < 0 || pz < 0 || px >= pixels || pz >= pixels || dx * dx + dz * dz > radius * radius)
                    continue;
                const std::size_t index = (static_cast<std::size_t>(pz) * static_cast<std::size_t>(pixels) +
                                           static_cast<std::size_t>(px)) * 3U;
                std::copy(color.begin(), color.end(), map.rgb.begin() + static_cast<std::ptrdiff_t>(index));
            }
    };
    mark(0.0, 0.0, 3, {0U, 0U, 0U});
    if (terrain != nullptr)
        mark(terrain->definition().spawn_x, terrain->definition().spawn_z, 2, {230U, 30U, 30U});

    // The legend: each biome's colour and share of the map.
    std::map<std::int32_t, std::size_t> counts;
    for (const std::int32_t biome : dominant)
        ++counts[biome];
    std::vector<std::pair<std::size_t, std::int32_t>> ordered;
    for (const auto &[biome, count] : counts)
        ordered.emplace_back(count, biome);
    std::sort(ordered.begin(), ordered.end(), std::greater<>{});
    for (const auto &[count, biome] : ordered)
    {
        std::string name;
        Rgb color;
        if (biome < 0)
        {
            const auto kind = static_cast<world::BiomeKind>(-1 - biome);
            name = "retail:" + std::string{world::retail_biome_name(kind)};
            color = retail_color(kind);
        }
        else
        {
            const world::TerrainBiome &entry = terrain->definition().biomes[static_cast<std::size_t>(biome)];
            name = entry.key;
            color = entry.retail == world::RetailReuse::none ? biome_color(entry, request) : retail_color(entry.like);
        }
        map.legend.push_back(std::format("{:<28} #{:02x}{:02x}{:02x}  {:5.1f}%", name,
                                         static_cast<int>(color.r), static_cast<int>(color.g),
                                         static_cast<int>(color.b),
                                         100.0 * static_cast<double>(count) / static_cast<double>(dominant.size())));
    }
    return map;
}

std::vector<std::optional<std::array<std::uint8_t, 3>>> palette_colors(Sandbox &sandbox)
{
    std::vector<std::optional<std::array<std::uint8_t, 3>>> colors;
    if (sandbox.terrain() == nullptr)
        return colors;
    const world::TerrainDefinition &definition = sandbox.terrain()->definition();
    colors.resize(definition.palette.size());
    const auto &types = sandbox.gameplay().client_block_types();
    static constexpr std::array<std::string_view, 4> extensions{".png", ".bmp", ".tga", ".jpg"};
    for (std::size_t index = 0U; index < definition.palette.size(); ++index)
    {
        const auto type = std::find_if(types.begin(), types.end(), [&](const auto &entry) {
            return entry.key == definition.palette[index];
        });
        if (type == types.end())
            continue;
        const std::string &name = !type->faces[0].empty() ? type->faces[0] : type->texture;
        const auto resolved = sandbox.graphics().assets().resolve(name, extensions);
        if (!resolved)
            continue;
        const auto image = modding::graphics::load_image(resolved->path);
        if (!image || image->empty())
            continue;
        double sum[3]{};
        double weight{};
        for (std::size_t texel = 0U; texel + 3U < image->rgba.size(); texel += 4U)
        {
            const double alpha = image->rgba[texel + 3U] / 255.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                sum[channel] += image->rgba[texel + channel] * alpha;
            weight += alpha;
        }
        if (weight > 0.0)
            colors[index] = std::array{static_cast<std::uint8_t>(sum[0] / weight),
                                       static_cast<std::uint8_t>(sum[1] / weight),
                                       static_cast<std::uint8_t>(sum[2] / weight)};
    }
    return colors;
}

void check_world_generator(ValidationResult &result)
{
    if (!result.ok())
        return;
    // Only a mod whose shared scripts define a world.
    bool defines{};
    const std::filesystem::path shared = result.mod.root / "shared";
    std::error_code error_code;
    if (std::filesystem::is_directory(shared, error_code))
        for (const auto &entry : std::filesystem::recursive_directory_iterator{shared, error_code})
        {
            if (!entry.is_regular_file() ||
                (entry.path().extension() != ".luau" && entry.path().extension() != ".lua"))
                continue;
            std::ifstream input{entry.path(), std::ios::binary};
            const std::string text{std::istreambuf_iterator<char>{input}, {}};
            defines = defines || text.find("terrain.define") != std::string::npos;
        }
    if (!defines)
        return;
    std::vector<Diagnostic> &out = result.diagnostics;
    if (result.mod.descriptor.scope == net::ModScope::client)
    {
        out.push_back({Diagnostic::Level::error, "", 0, 0,
                       "terrain.define needs a server or core mod: a client mod can never change the terrain"});
        return;
    }
    Sandbox sandbox;
    std::string error;
    const std::vector<net::LoadedMod> mods{result.mod};
    if (!sandbox.start(mods, error))
    {
        out.push_back({Diagnostic::Level::warning, "", 0, 0,
                       "the world could not be built in the sandbox: " + error});
        return;
    }
    const auto &terrain = sandbox.terrain();
    if (terrain == nullptr)
    {
        for (const modding::LogEntry &entry : sandbox.log())
            if (entry.level == modding::LogLevel::error)
                out.push_back({Diagnostic::Level::error, "", 0, 0, "world generator: " + entry.message});
        if (std::none_of(out.begin(), out.end(), [](const Diagnostic &diagnostic) {
                return diagnostic.level == Diagnostic::Level::error;
            }))
            out.push_back({Diagnostic::Level::error, "", 0, 0, "the world generator was not built"});
        return;
    }
    const world::TerrainDefinition &definition = terrain->definition();
    // Biomes that no band places.
    std::set<std::string> placed;
    for (const world::TerrainBiome &biome : definition.biomes)
        placed.insert(biome.key);
    if (const modding::Value biomes = sandbox.runtime().run(modding::Side::server, result.mod.descriptor.id,
                                                            "validate", "return terrain.biomes()")
                                          .value;
        biomes.type() == modding::ValueType::array)
        for (const modding::Value &key : biomes.as_array())
            if (key.type() == modding::ValueType::string && !placed.contains(key.as_string()))
                out.push_back({Diagnostic::Level::warning, "", 0, 0,
                               std::format("biome '{}' is defined but no layout band places it", key.as_string())});
    // Ground that leaves the world: sampled over the layout's first period.
    const world::WorldGenerator &generator = sandbox.terrain_generator();
    std::map<std::uint16_t, std::pair<int, int>> range;
    for (int ring = 0; ring <= 60; ++ring)
        for (int spoke = 0; spoke < 24; ++spoke)
        {
            const double radius = ring * 500.0;
            const double angle = spoke * 0.2617993877991494;
            const int x = static_cast<int>(radius * std::cos(angle));
            const int z = static_cast<int>(radius * std::sin(angle));
            const std::uint16_t biome = terrain->biome_at(generator, x, z);
            const int height = terrain->surface_height(generator, x, z);
            auto [entry, inserted] = range.try_emplace(biome, height, height);
            if (!inserted)
                entry->second = {std::min(entry->second.first, height), std::max(entry->second.second, height)};
        }
    for (const auto &[biome, heights] : range)
    {
        const world::TerrainBiome &entry = definition.biomes[biome];
        if (entry.retail != world::RetailReuse::none)
            continue;
        if (heights.second >= world::world_min_y + world::chunk_height - 2)
            out.push_back({Diagnostic::Level::warning, "", 0, 0,
                           std::format("biome '{}' reaches the top of the world (y {}): its ground is cut flat",
                                       entry.key, heights.second)});
        if (heights.first <= world::world_min_y + 2)
            out.push_back({Diagnostic::Level::warning, "", 0, 0,
                           std::format("biome '{}' reaches the bottom of the world (y {})", entry.key,
                                       heights.first)});
    }
}

} // namespace openminerz::mod_sdk
