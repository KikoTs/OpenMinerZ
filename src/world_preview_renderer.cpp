#include "world_preview_renderer.hpp"

#include "avatar_viewmodel.hpp"
#include "depth_voxel_renderer.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/dynamic_radiance.hpp"
#include "static_model_asset.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <numbers>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace openminerz
{
namespace
{

int floor_divide(int value, int divisor) noexcept
{
    return value >= 0 ? value / divisor : (value - divisor + 1) / divisor;
}

int active_span_for(int view_distance) noexcept
{
    const int visible_radius = (std::clamp(view_distance, 4, 32) + 3) / 4;
    return std::max(10, (visible_radius + 2) * 2);
}

float visibility_for(int view_distance) noexcept
{
    return static_cast<float>(std::clamp(view_distance * 4, 32, 128));
}

world::BiomeKind preview_biome(int index) noexcept
{
    constexpr std::array biomes{world::BiomeKind::classic, world::BiomeKind::lagoon,
                                world::BiomeKind::desert,  world::BiomeKind::mountain,
                                world::BiomeKind::arctic,  world::BiomeKind::descent};
    return biomes[static_cast<std::size_t>(
        std::clamp(index, 0, static_cast<int>(biomes.size()) - 1))];
}

int preview_biome_center_chunk(world::BiomeKind biome) noexcept
{
    switch (biome)
    {
    case world::BiomeKind::classic:
        return 0;
    case world::BiomeKind::lagoon:
        return 37;
    case world::BiomeKind::desert:
        return 75;
    case world::BiomeKind::mountain:
        return 118;
    case world::BiomeKind::arctic:
        return 162;
    case world::BiomeKind::descent:
        return 212;
    }
    return 0;
}

int preview_biome_origin(world::BiomeKind biome, int span) noexcept
{
    return preview_biome_center_chunk(biome) - span / 2;
}

SDL_Color to_color(world::SkyColor color, float multiplier = 1.0F, Uint8 alpha = 255U)
{
    const auto channel = [multiplier](float value) {
        return static_cast<Uint8>(std::clamp(value * multiplier * 255.0F, 0.0F, 255.0F));
    };
    return {channel(color.red), channel(color.green), channel(color.blue), alpha};
}

// The CPU twin of the composite shader's tone curves. The sky is drawn with
// SDL behind the tone-mapped world target, so it has to be exposed and
// compressed by the same numbers or the horizon splits in two.
float tone_curve_channel(float value, int curve)
{
    switch (curve)
    {
    case 1: // Extended Reinhard, white point 4.
    {
        constexpr float white = 4.0F;
        return std::clamp(value * (1.0F + value / (white * white)) / (1.0F + value), 0.0F, 1.0F);
    }
    case 2: // Hable filmic.
    {
        const auto hable = [](float x) {
            constexpr float a = 0.15F;
            constexpr float b = 0.50F;
            constexpr float c = 0.10F;
            constexpr float d = 0.20F;
            constexpr float e = 0.02F;
            constexpr float f = 0.30F;
            return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
        };
        constexpr float white = 11.2F;
        return std::clamp(hable(value * 2.0F) / hable(white), 0.0F, 1.0F);
    }
    case 3: // ACES (Narkowicz fit).
    {
        constexpr float a = 2.51F;
        constexpr float b = 0.03F;
        constexpr float c = 2.43F;
        constexpr float d = 0.59F;
        constexpr float e = 0.14F;
        return std::clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0F, 1.0F);
    }
    default:
        break;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

// CPU twin of the shader's fog-driven white balance.
std::array<float, 3> fog_white_balance(world::SkyColor fog, float strength,
                                       float maximum_boost = 4.0F)
{
    std::array<float, 3> tint{1.0F / (fog.red + 0.5F), 1.0F / (fog.green + 0.5F),
                              1.0F / (fog.blue + 0.5F)};
    for (float &channel : tint)
        channel += (1.0F - channel) * 0.2F;
    const float smallest = std::max(std::min({tint[0], tint[1], tint[2]}), 0.0001F);
    const float amount = std::clamp(strength, 0.0F, 1.0F);
    for (float &channel : tint)
        channel = std::min(1.0F + (channel / smallest - 1.0F) * amount, maximum_boost);
    return tint;
}

world::SkyColor graded_sky_color(world::SkyColor color, float exposure, int curve,
                                 const std::array<float, 3> &tint)
{
    return {tone_curve_channel(color.red * tint[0] * exposure, curve),
            tone_curve_channel(color.green * tint[1] * exposure, curve),
            tone_curve_channel(color.blue * tint[2] * exposure, curve)};
}

SDL_Color mix(SDL_Color left, SDL_Color right, float amount)
{
    amount = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [amount](Uint8 a, Uint8 b) {
        return static_cast<Uint8>(static_cast<float>(a) +
                                  (static_cast<float>(b) - static_cast<float>(a)) * amount);
    };
    return {channel(left.r, right.r), channel(left.g, right.g), channel(left.b, right.b),
            channel(left.a, right.a)};
}

SDL_FColor to_float_color(SDL_Color color) noexcept
{
    constexpr float scale = 1.0F / 255.0F;
    return {color.r * scale, color.g * scale, color.b * scale, color.a * scale};
}

void fill(SDL_Renderer *renderer, const SDL_FRect &rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

std::filesystem::path derived_atlas_path()
{
    return original_derived_data_path() / "terrain_diffuse.bmp";
}

struct Projection final
{
    float center_x{};
    float center_y{};
    float tile_width{};
    float tile_height{};
    float block_height{};
    float world_center_x{};
    float world_center_z{};

    [[nodiscard]] SDL_FPoint point(float x, float y, float z) const noexcept
    {
        const float relative_x = x - world_center_x;
        const float relative_z = z - world_center_z;
        return {center_x + (relative_x - relative_z) * tile_width * 0.5F,
                center_y + (relative_x + relative_z) * tile_height * 0.5F - y * block_height};
    }
};

SDL_Color face_color(const world::WorldFace &face, const world::SkyState &sky, bool textured,
                     bool ambient_occlusion, float shadow_strength)
{
    world::Vec3 normal{};
    switch (face.direction)
    {
    case world::FaceDirection::positive_x:
        normal = {1.0F, 0.0F, 0.0F};
        break;
    case world::FaceDirection::negative_z:
        normal = {0.0F, 0.0F, -1.0F};
        break;
    case world::FaceDirection::negative_x:
        normal = {-1.0F, 0.0F, 0.0F};
        break;
    case world::FaceDirection::positive_z:
        normal = {0.0F, 0.0F, 1.0F};
        break;
    case world::FaceDirection::positive_y:
        normal = {0.0F, 1.0F, 0.0F};
        break;
    case world::FaceDirection::negative_y:
        normal = {0.0F, -1.0F, 0.0F};
        break;
    }
    const float ao = ambient_occlusion
                         ? static_cast<float>(std::accumulate(face.ambient_occlusion.begin(),
                                                              face.ambient_occlusion.end(), 0)) /
                               (4.0F * 255.0F)
                         : 1.0F;
    const float sky_light =
        static_cast<float>(std::accumulate(face.sky_light.begin(), face.sky_light.end(), 0)) /
        (4.0F * 255.0F);
    const float torch_light =
        static_cast<float>(std::accumulate(face.block_light.begin(), face.block_light.end(), 0)) /
        (4.0F * 255.0F);
    const float sun_facing =
        std::clamp(normal.x * sky.sun_x + normal.y * sky.sun_y + normal.z * sky.sun_z, 0.0F, 1.0F);
    const float directional = 1.0F + ((0.52F + sun_facing * 0.48F) - 1.0F) *
                                         std::clamp(shadow_strength, 0.0F, 1.0F) * sky_light;
    const float smooth_sky = sky_light * sky_light * (3.0F - 2.0F * sky_light);
    const float cave_light = 0.05F + 0.95F * smooth_sky;
    const float face_light = directional * cave_light * ao;
    // BlockTerrain.TorchColor warm add, suppressed where daylight already covers it.
    const float torch_gain = torch_light * (1.0F - sky_light * sky.daylight * 0.85F) * ao;
    const float torch_red = torch_gain;
    const float torch_green = torch_gain * (235.0F / 255.0F);
    const float torch_blue = torch_gain * (190.0F / 255.0F);
    const auto illumination = [](float ambient, float direct) {
        return std::min(1.0F, (ambient * 0.35F + direct * 0.65F) * 1.2F);
    };
    if (face.emission != 0U)
    {
        return {255U, 255U, 255U, 255U};
    }
    if (textured)
    {
        const auto channel = [](float lit) {
            return static_cast<Uint8>(std::clamp(lit, 0.0F, 1.0F) * 255.0F);
        };
        return {
            channel(face_light * illumination(sky.ambient.red, sky.direct.red) + torch_red),
            channel(face_light * illumination(sky.ambient.green, sky.direct.green) + torch_green),
            channel(face_light * illumination(sky.ambient.blue, sky.direct.blue) + torch_blue),
            255U};
    }
    const auto base = world::fallback_color(face.block);
    const float warm = 0.72F + sky.direct.red * 0.28F;
    return {static_cast<Uint8>(std::clamp(base.red * face_light *
                                              illumination(sky.ambient.red, sky.direct.red) * warm,
                                          0.0F, 255.0F)),
            static_cast<Uint8>(std::clamp(base.green * face_light *
                                              illumination(sky.ambient.green, sky.direct.green),
                                          0.0F, 255.0F)),
            static_cast<Uint8>(
                std::clamp(base.blue * face_light * illumination(sky.ambient.blue, sky.direct.blue),
                           0.0F, 255.0F)),
            255U};
}

float local_emissive_light(const world::WorldRegion &region, world::Vec3 sample_position) noexcept
{
    // Match the fifteen-level voxel-light reach closely enough for dynamic
    // actors and the first-person rig without rebuilding the mesh LightMap on
    // the render thread. The former five-cell scan missed a lava floor only six
    // cells below the player's head, leaving hands and enemies black in hell.
    constexpr int horizontal_radius = 10;
    constexpr int below_radius = 10;
    constexpr int above_radius = 6;
    constexpr float light_radius = 10.0F;
    constexpr float light_radius_squared = light_radius * light_radius;
    const int center_x = static_cast<int>(std::floor(sample_position.x));
    const int center_y = static_cast<int>(std::floor(sample_position.y));
    const int center_z = static_cast<int>(std::floor(sample_position.z));
    float result{};
    for (int y = center_y - below_radius; y <= center_y + above_radius && result < 0.999F; ++y)
        for (int z = center_z - horizontal_radius; z <= center_z + horizontal_radius; ++z)
            for (int x = center_x - horizontal_radius; x <= center_x + horizontal_radius; ++x)
            {
                if (!region.contains(x, y, z))
                    continue;
                const std::uint8_t emission = world::self_illumination(region.block(x, y, z));
                if (emission == 0U)
                    continue;
                const float dx = static_cast<float>(x) + 0.5F - sample_position.x;
                const float dy = static_cast<float>(y) + 0.5F - sample_position.y;
                const float dz = static_cast<float>(z) + 0.5F - sample_position.z;
                const float distance_squared = dx * dx + dy * dy + dz * dz;
                if (distance_squared >= light_radius_squared)
                    continue;
                const float source = static_cast<float>(emission) / 15.0F;
                result =
                    std::max(result, source * (1.0F - std::sqrt(distance_squared) / light_radius));
            }
    return std::clamp(result, 0.0F, 1.0F);
}

float local_actor_sky_light(const world::WorldRegion &region,
                            world::Vec3 sample_position) noexcept
{
    // BlockTerrain.GetEnemyLighting samples an interpolated light field at the
    // actor rather than assigning a binary outdoor/cave value. Approximate the
    // same soft edge from nearby vertical sky columns: a player under a ledge
    // now receives lateral daylight while a deep enclosed room reaches zero.
    struct Probe final
    {
        int x{};
        int z{};
        float weight{};
    };
    constexpr std::array<Probe, 13> probes{{
        {0, 0, 4.0F},
        {1, 0, 1.5F}, {-1, 0, 1.5F}, {0, 1, 1.5F}, {0, -1, 1.5F},
        {1, 1, 0.8F}, {1, -1, 0.8F}, {-1, 1, 0.8F}, {-1, -1, 0.8F},
        {2, 0, 0.35F}, {-2, 0, 0.35F}, {0, 2, 0.35F}, {0, -2, 0.35F},
    }};
    const int center_x = static_cast<int>(std::floor(sample_position.x));
    const int center_z = static_cast<int>(std::floor(sample_position.z));
    const int start_y = static_cast<int>(std::floor(sample_position.y));
    const int top_y = world::world_min_y + world::chunk_height;
    float open_weight{};
    float total_weight{};
    for (const Probe &probe : probes)
    {
        const int x = center_x + probe.x;
        const int z = center_z + probe.z;
        if (!region.contains(x, start_y, z))
            continue;
        total_weight += probe.weight;
        bool open = true;
        for (int y = start_y + 1; y < top_y; ++y)
        {
            if (region.contains(x, y, z) && world::occludes(region.block(x, y, z)))
            {
                open = false;
                break;
            }
        }
        if (open)
            open_weight += probe.weight;
    }
    return total_weight > 0.0F ? std::clamp(open_weight / total_weight, 0.0F, 1.0F) : 0.0F;
}

} // namespace

WorldPreviewRenderer::WorldPreviewRenderer(SDL_Renderer *renderer, std::int32_t seed,
                                           int view_distance, int initial_biome,
                                           world::TerrainProfile terrain_profile,
                                           GameMode game_mode)
    : renderer_(renderer), generator_(seed, terrain_profile, game_mode),
      // Bring up only the four-by-four spawn window synchronously. The configured view window
      // is generated on the worker and its mesh sections are admitted frame by frame.
      region_(preview_biome_origin(preview_biome(initial_biome), 4), -2, 4, 4),
      active_span_(active_span_for(view_distance)),
      visibility_distance_(visibility_for(view_distance))
{
    const world::BiomeKind launch_biome = preview_biome(initial_biome);
    rebuild_region(preview_biome_origin(launch_biome, 4), -2, launch_biome);
    stats_.seed = seed;
    load_original_atlas();
    const auto derived = original_derived_data_path();
    pickup_pickaxe_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pickaxe.omsm");
    pickup_compass_ = std::make_unique<StaticModelAsset>(renderer_, derived / "compass.omsm");
    pickup_pistol_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pistol.omsm");
    pickup_knife_ = std::make_unique<StaticModelAsset>(renderer_, derived / "knife.omsm");
    pickup_ammo_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ammo.omsm");
    pickup_torch_ = std::make_unique<StaticModelAsset>(renderer_, derived / "torch.omsm");
    pickup_assault_rifle_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ak.omsm");
    pickup_shotgun_ = std::make_unique<StaticModelAsset>(renderer_, derived / "shotgun.omsm");
    pickup_rifle_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rifle.omsm");
    pickup_smg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "smg.omsm");
    pickup_lmg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "lmg.omsm");
    pickup_launcher_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rpg.omsm");
    pickup_space_assault_rifle_ =
        std::make_unique<StaticModelAsset>(renderer_, derived / "space_ak.omsm");
    pickup_space_pistol_ =
        std::make_unique<StaticModelAsset>(renderer_, derived / "space_pistol.omsm");
    pickup_space_shotgun_ =
        std::make_unique<StaticModelAsset>(renderer_, derived / "space_shotgun.omsm");
    pickup_space_rifle_ =
        std::make_unique<StaticModelAsset>(renderer_, derived / "space_rifle.omsm");
    pickup_space_smg_ =
        std::make_unique<StaticModelAsset>(renderer_, derived / "space_smg.omsm");
    pickup_ore_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ore.omsm");
    pickup_gems_ = std::make_unique<StaticModelAsset>(renderer_, derived / "gems.omsm");
    pickup_bars_ = std::make_unique<StaticModelAsset>(renderer_, derived / "bars.omsm");
    door_wood_ = std::make_unique<StaticModelAsset>(renderer_, derived / "door_wood.omsm");
    door_iron_ = std::make_unique<StaticModelAsset>(renderer_, derived / "door_iron.omsm");
    door_diamond_ = std::make_unique<StaticModelAsset>(renderer_, derived / "door_diamond.omsm");
    door_tech_ = std::make_unique<StaticModelAsset>(renderer_, derived / "door_tech.omsm");
    fireball_model_ = std::make_unique<StaticModelAsset>(renderer_, derived / "fireball.omsm");
    rocket_model_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rocket.omsm");
    avatar_viewmodel_ = std::make_unique<AvatarViewmodel>(renderer_, default_avatar_path());
    if (!avatar_viewmodel_->loaded())
    {
        // The newest live export can be mid-write or a newer format than the
        // reader supports; the staged DerivedData copy is known good and must
        // never leave the player without hands.
        const auto staged = derived / "avatar.omavatar";
        std::error_code probe_error;
        if (std::filesystem::exists(staged, probe_error))
        {
            std::cerr << "Falling back to the staged avatar shell: " << staged << '\n';
            avatar_viewmodel_ = std::make_unique<AvatarViewmodel>(renderer_, staged);
        }
    }
    enemy_models_ = std::make_unique<EnemyModelLibrary>(renderer_, derived);
    // Untextured white surface for projectile streaks (tracers/arrows).
    {
        SDL_Surface *white = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
        if (white != nullptr)
        {
            SDL_FillSurfaceRect(white, nullptr, SDL_MapSurfaceRGBA(white, 255U, 255U, 255U, 255U));
            white_texture_ = SDL_CreateTextureFromSurface(renderer_, white);
            SDL_DestroySurface(white);
        }
    }
    stats_.original_avatar_loaded = avatar_viewmodel_->loaded();
    stats_.original_avatar_world_animations_loaded = avatar_viewmodel_->world_animation_ready();
    stats_.avatar_animation_clip_count = avatar_viewmodel_->animation_clip_count();
    stats_.avatar_hand_triangles = avatar_viewmodel_->triangle_count();
    if (stats_.original_avatar_loaded && !stats_.original_avatar_world_animations_loaded)
        std::cerr << "Avatar loaded without the complete Stand/Walk/Run/Faint world animation "
                     "set; using the animated native fallback for remote players\n";
    if (atlas_ != nullptr && normal_spec_atlas_ != nullptr && metal_light_atlas_ != nullptr)
    {
        depth_renderer_ = std::make_unique<DepthVoxelRenderer>(
            renderer_, atlas_, normal_spec_atlas_, metal_light_atlas_);
        stats_.depth_buffered = depth_renderer_->available();
        if (!stats_.depth_buffered)
        {
            std::cerr << "Depth renderer unavailable: " << depth_renderer_->failure_reason()
                      << '\n';
        }
        if (stats_.depth_buffered)
        {
            depth_renderer_->rebuild_chunks(mesh_batches_, region_.minimum_x(), region_.minimum_z(),
                                            region_.maximum_x(), region_.maximum_z());
            stats_.depth_buffered = depth_renderer_->available();
        }
    }
    if (active_span_ > region_.chunks_x())
    {
        world::WorldRegion current = region_;
        const world::WorldGenerator generator = generator_;
        const int target_span = active_span_;
        const int target_origin_x = preview_biome_origin(launch_biome, target_span);
        stream_job_ = std::async(std::launch::async, [generator, current = std::move(current),
                                                      target_origin_x, target_span]() mutable {
            return generator.stream_region(current, target_origin_x, -target_span / 2, target_span,
                                           target_span);
        });
        stream_job_active_ = true;
        pending_biome_ = launch_biome;
        stats_.stream_pending = true;
    }
}

std::size_t WorldPreviewRenderer::BlockCoordinateHash::operator()(
    const BlockCoordinate &coordinate) const noexcept
{
    std::size_t result = std::hash<int>{}(coordinate.x);
    result ^= std::hash<int>{}(coordinate.y) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
    result ^= std::hash<int>{}(coordinate.z) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
    return result;
}

std::size_t WorldPreviewRenderer::ChunkCoordinateHash::operator()(
    world::ChunkCoordinate coordinate) const noexcept
{
    std::size_t result = std::hash<int>{}(coordinate.x);
    result ^= std::hash<int>{}(coordinate.z) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
    result ^= std::hash<int>{}(coordinate.y) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
    return result;
}

void WorldPreviewRenderer::rebuild_region(int origin_chunk_x, int origin_chunk_z,
                                          world::BiomeKind biome)
{
    discard_pending_stream();
    auto next = stats_.chunk_count == 0U
                    ? generator_.generate_region(origin_chunk_x, origin_chunk_z, region_.chunks_x(),
                                                 region_.chunks_z())
                    : generator_.stream_region(region_, origin_chunk_x, origin_chunk_z);
    install_region(std::move(next), biome);
}

void WorldPreviewRenderer::install_region(world::WorldRegion next, world::BiomeKind biome)
{
    std::vector<world::ChunkCoordinate> previous;
    previous.reserve(mesh_batches_.size());
    for (const auto &batch : mesh_batches_)
    {
        previous.push_back(batch.coordinate);
    }
    region_ = std::move(next);
    apply_saved_edits();
    if (previous.empty())
    {
        rebuild_mesh();
    }
    else
    {
        struct CoordinateHash final
        {
            std::size_t operator()(world::ChunkCoordinate coordinate) const noexcept
            {
                std::size_t result = std::hash<int>{}(coordinate.x);
                result ^=
                    std::hash<int>{}(coordinate.z) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
                result ^=
                    std::hash<int>{}(coordinate.y) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
                return result;
            }
        };
        const std::unordered_set<world::ChunkCoordinate, CoordinateHash> previous_set{
            previous.begin(), previous.end()};
        std::vector<world::ChunkCoordinate> current;
        current.reserve(region_.chunks().size() *
                        static_cast<std::size_t>(world::chunk_section_count));
        for (const auto &chunk : region_.chunks())
        {
            for (int section_y = 0; section_y < world::chunk_section_count; ++section_y)
                current.push_back({chunk.chunk_x(), chunk.chunk_z(), section_y});
        }
        const std::unordered_set<world::ChunkCoordinate, CoordinateHash> current_set{
            current.begin(), current.end()};
        std::vector<world::ChunkCoordinate> changed;
        const auto add_neighborhood = [&](world::ChunkCoordinate center) {
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const world::ChunkCoordinate coordinate{center.x + dx, center.z + dz, center.y};
                    if (current_set.contains(coordinate))
                    {
                        changed.push_back(coordinate);
                    }
                }
            }
        };
        for (const auto coordinate : current)
        {
            if (!previous_set.contains(coordinate))
            {
                add_neighborhood(coordinate);
            }
        }
        for (const auto coordinate : previous)
        {
            if (!current_set.contains(coordinate))
            {
                add_neighborhood(coordinate);
            }
        }
        std::erase_if(mesh_batches_, [&](const world::ChunkFaceBatch &batch) {
            return !current_set.contains(batch.coordinate);
        });
        reindex_mesh_batches();
        if (depth_renderer_ != nullptr && depth_renderer_->available())
        {
            depth_renderer_->retain_chunks(region_.origin_chunk_x(), region_.origin_chunk_z(),
                                           region_.origin_chunk_x() + region_.chunks_x() - 1,
                                           region_.origin_chunk_z() + region_.chunks_z() - 1);
            depth_renderer_->set_world_bounds(region_.minimum_x(), region_.minimum_z(),
                                              region_.maximum_x(), region_.maximum_z());
        }
        queue_chunk_rebuilds(std::move(changed));
    }
    biome_ = biome;
    stats_.chunk_count = region_.chunks().size();
}

void WorldPreviewRenderer::discard_pending_stream()
{
    pending_mesh_rebuilds_.clear();
    defer_mesh_rebuild_once_ = false;
    discard_pending_mesh();
    if (!stream_job_active_)
    {
        stats_.stream_pending = false;
        return;
    }
    stream_job_.wait();
    (void)stream_job_.get();
    stream_job_active_ = false;
    stats_.stream_pending = false;
}

void WorldPreviewRenderer::discard_pending_mesh()
{
    if (!mesh_job_active_)
    {
        active_mesh_rebuilds_.clear();
        return;
    }
    mesh_job_.wait();
    (void)mesh_job_.get();
    mesh_job_active_ = false;
    active_mesh_rebuilds_.clear();
}

void WorldPreviewRenderer::apply_saved_edits()
{
    for (const auto &[coordinate, block] : saved_edits_)
    {
        if (region_.contains(coordinate.x, coordinate.y, coordinate.z))
        {
            region_.set_block(coordinate.x, coordinate.y, coordinate.z, block);
        }
    }
}

void WorldPreviewRenderer::rebuild_mesh()
{
    discard_pending_mesh();
    pending_mesh_rebuilds_.clear();
    defer_mesh_rebuild_once_ = false;
    mesh_batches_ = region_.visible_faces_by_chunk();
    reindex_mesh_batches();
    std::size_t exposed_count{};
    for (const auto &batch : mesh_batches_)
    {
        exposed_count += batch.faces.size();
    }
    faces_.clear();
    faces_.reserve(exposed_count);
    for (const auto &batch : mesh_batches_)
    {
        for (const auto &face : batch.faces)
        {
            faces_.push_back({face, (face.x + face.z) * 256 + face.y});
        }
    }
    std::stable_sort(
        faces_.begin(), faces_.end(), [](const RenderFace &left, const RenderFace &right) {
            if (left.depth != right.depth)
            {
                return left.depth < right.depth;
            }
            return static_cast<int>(left.face.direction) < static_cast<int>(right.face.direction);
        });
    stats_.visible_face_count = exposed_count;
    faces_dirty_ = false;
    if (depth_renderer_ != nullptr && depth_renderer_->available())
    {
        depth_renderer_->rebuild_chunks(mesh_batches_, region_.minimum_x(), region_.minimum_z(),
                                        region_.maximum_x(), region_.maximum_z());
        stats_.depth_buffered = depth_renderer_->available();
    }
}

void WorldPreviewRenderer::rebuild_block_edit(int world_x, int world_y, int world_z)
{
    rebuild_block_edits({{world_x, world_y, world_z}});
}

void WorldPreviewRenderer::rebuild_block_edits(const std::vector<std::array<int, 3>> &edits)
{
    // Flood-filled lighting reaches fifteen cells; Rebirth then traces a six-cell
    // primary bounce, a three-cell secondary bounce, and one surface-light sample.
    // Remesh every section in the union of the complete 25-cell influence volumes.
    // A blast or an enemy dig wave enters the queue once, so overlapping volumes
    // never cause repeated sorts or repeated whole-world light bakes.
    constexpr int light_reach = 25;
    std::unordered_set<world::ChunkCoordinate, ChunkCoordinateHash> selected;
    selected.reserve(edits.size() * 64U);
    for (const auto &edit : edits)
    {
        const int minimum_chunk_x = floor_divide(edit[0] - light_reach, world::chunk_width);
        const int maximum_chunk_x = floor_divide(edit[0] + light_reach, world::chunk_width);
        const int minimum_chunk_z = floor_divide(edit[2] - light_reach, world::chunk_depth);
        const int maximum_chunk_z = floor_divide(edit[2] + light_reach, world::chunk_depth);
        const int local_y = edit[1] - world::world_min_y;
        const int maximum_section =
            std::clamp((local_y + light_reach) / world::chunk_section_height, 0,
                       world::chunk_section_count - 1);
        for (int chunk_z = minimum_chunk_z; chunk_z <= maximum_chunk_z; ++chunk_z)
            for (int chunk_x = minimum_chunk_x; chunk_x <= maximum_chunk_x; ++chunk_x)
                for (int section_y = 0; section_y <= maximum_section; ++section_y)
                    selected.insert({chunk_x, chunk_z, section_y});
    }
    std::vector<world::ChunkCoordinate> selection{selected.begin(), selected.end()};
    queue_chunk_rebuilds(std::move(selection));
}

void WorldPreviewRenderer::queue_chunk_rebuilds(std::vector<world::ChunkCoordinate> selection)
{
    std::sort(selection.begin(), selection.end(), [](const auto &left, const auto &right) {
        if (left.z != right.z)
            return left.z < right.z;
        if (left.x != right.x)
            return left.x < right.x;
        return left.y < right.y;
    });
    selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    std::unordered_set<world::ChunkCoordinate, ChunkCoordinateHash> queued{
        pending_mesh_rebuilds_.begin(), pending_mesh_rebuilds_.end()};
    queued.reserve(queued.size() + selection.size());
    bool added{};
    for (const auto coordinate : selection)
    {
        if (coordinate.x < region_.origin_chunk_x() ||
            coordinate.x >= region_.origin_chunk_x() + region_.chunks_x() ||
            coordinate.z < region_.origin_chunk_z() ||
            coordinate.z >= region_.origin_chunk_z() + region_.chunks_z() || coordinate.y < 0 ||
            coordinate.y >= world::chunk_section_count)
        {
            continue;
        }
        if (queued.insert(coordinate).second)
        {
            pending_mesh_rebuilds_.push_back(coordinate);
            added = true;
        }
    }
    const int center_twice_x = region_.origin_chunk_x() * 2 + region_.chunks_x() - 1;
    const int center_twice_z = region_.origin_chunk_z() * 2 + region_.chunks_z() - 1;
    std::stable_sort(pending_mesh_rebuilds_.begin(), pending_mesh_rebuilds_.end(),
                     [&](const auto &left, const auto &right) {
                         const int left_x = left.x * 2 - center_twice_x;
                         const int left_z = left.z * 2 - center_twice_z;
                         const int right_x = right.x * 2 - center_twice_x;
                         const int right_z = right.z * 2 - center_twice_z;
                         const int left_distance = left_x * left_x + left_z * left_z;
                         const int right_distance = right_x * right_x + right_z * right_z;
                         if (left_distance != right_distance)
                             return left_distance < right_distance;
                         if (left.x != right.x)
                             return left.x < right.x;
                         if (left.z != right.z)
                             return left.z < right.z;
                         const int left_height = std::abs(left.y - 5);
                         const int right_height = std::abs(right.y - 5);
                         return left_height != right_height ? left_height < right_height
                                                            : left.y < right.y;
                     });
    defer_mesh_rebuild_once_ = defer_mesh_rebuild_once_ || added;
    stats_.stream_pending =
        stream_job_active_ || mesh_job_active_ || !pending_mesh_rebuilds_.empty();
}

void WorldPreviewRenderer::pump_chunk_rebuilds(std::size_t budget)
{
    if (defer_mesh_rebuild_once_)
    {
        defer_mesh_rebuild_once_ = false;
        stats_.stream_pending =
            stream_job_active_ || mesh_job_active_ || !pending_mesh_rebuilds_.empty();
        return;
    }

    if (mesh_job_active_)
    {
        if (mesh_job_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        {
            stats_.stream_pending = true;
            return;
        }
        install_chunk_batches(mesh_job_.get());
        mesh_job_active_ = false;
        active_mesh_rebuilds_.clear();
    }

    // Once generation has produced the newer resident window, let the stream
    // installer supersede/re-filter the old queue before launching another
    // edit worker. Otherwise a busy destruction queue can continuously put a
    // fresh worker in front of an already-ready stream.
    if (stream_job_active_ &&
        stream_job_.wait_for(std::chrono::seconds{0}) == std::future_status::ready)
    {
        stats_.stream_pending = true;
        return;
    }

    if (!pending_mesh_rebuilds_.empty())
    {
        const std::size_t count =
            std::min(std::max<std::size_t>(budget, 1U), pending_mesh_rebuilds_.size());
        std::vector<world::ChunkCoordinate> selection;
        selection.reserve(count);
        selection.insert(selection.end(), pending_mesh_rebuilds_.begin(),
                         pending_mesh_rebuilds_.begin() + static_cast<std::ptrdiff_t>(count));
        pending_mesh_rebuilds_.erase(pending_mesh_rebuilds_.begin(),
                                     pending_mesh_rebuilds_.begin() +
                                         static_cast<std::ptrdiff_t>(count));
        world::WorldRegion snapshot = region_;
        active_mesh_rebuilds_ = selection;
        mesh_job_ = std::async(std::launch::async, [snapshot = std::move(snapshot),
                                                    selection = std::move(selection)]() mutable {
            return snapshot.visible_faces_by_chunk(selection);
        });
        mesh_job_active_ = true;
    }
    stats_.stream_pending =
        stream_job_active_ || mesh_job_active_ || !pending_mesh_rebuilds_.empty();
}

void WorldPreviewRenderer::install_chunk_batches(std::vector<world::ChunkFaceBatch> rebuilt)
{
    for (auto &batch : rebuilt)
    {
        const auto indexed = mesh_batch_indices_.find(batch.coordinate);
        std::size_t batch_index{};
        std::size_t previous_face_count{};
        if (indexed == mesh_batch_indices_.end())
        {
            batch_index = mesh_batches_.size();
            mesh_batches_.push_back(std::move(batch));
            mesh_batch_indices_.insert_or_assign(mesh_batches_.back().coordinate, batch_index);
        }
        else
        {
            batch_index = indexed->second;
            previous_face_count = mesh_batches_[batch_index].faces.size();
            mesh_batches_[batch_index] = std::move(batch);
        }
        const auto &installed = mesh_batches_[batch_index];
        stats_.visible_face_count =
            stats_.visible_face_count - previous_face_count + installed.faces.size();
        if (depth_renderer_ != nullptr && depth_renderer_->available())
        {
            depth_renderer_->replace_chunk(installed);
        }
    }
    faces_dirty_ = true;
    stats_.depth_buffered = depth_renderer_ != nullptr && depth_renderer_->available();
}

void WorldPreviewRenderer::reindex_mesh_batches()
{
    mesh_batch_indices_.clear();
    mesh_batch_indices_.reserve(mesh_batches_.size());
    std::size_t face_count{};
    for (std::size_t index = 0U; index < mesh_batches_.size(); ++index)
    {
        mesh_batch_indices_.insert_or_assign(mesh_batches_[index].coordinate, index);
        face_count += mesh_batches_[index].faces.size();
    }
    stats_.visible_face_count = face_count;
}

void WorldPreviewRenderer::refresh_flat_faces()
{
    if (!faces_dirty_)
    {
        return;
    }
    faces_.clear();
    faces_.reserve(stats_.visible_face_count);
    for (const auto &batch : mesh_batches_)
    {
        for (const auto &face : batch.faces)
        {
            faces_.push_back({face, (face.x + face.z) * 256 + face.y});
        }
    }
    std::stable_sort(
        faces_.begin(), faces_.end(), [](const RenderFace &left, const RenderFace &right) {
            if (left.depth != right.depth)
            {
                return left.depth < right.depth;
            }
            return static_cast<int>(left.face.direction) < static_cast<int>(right.face.direction);
        });
    faces_dirty_ = false;
}

bool WorldPreviewRenderer::finish_stream_if_ready()
{
    if (!stream_job_active_ ||
        stream_job_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
    {
        return false;
    }
    // A completed stream supersedes meshes produced from the old resident
    // window, but its edit coordinates still matter. Preserve and re-filter
    // them against the new window after installation instead of letting a long
    // local-edit queue stall movement indefinitely.
    std::vector<world::ChunkCoordinate> interrupted_rebuilds = std::move(pending_mesh_rebuilds_);
    if (mesh_job_active_)
    {
        if (mesh_job_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        {
            pending_mesh_rebuilds_ = std::move(interrupted_rebuilds);
            return false;
        }
        (void)mesh_job_.get();
        mesh_job_active_ = false;
        interrupted_rebuilds.insert(interrupted_rebuilds.end(), active_mesh_rebuilds_.begin(),
                                    active_mesh_rebuilds_.end());
        active_mesh_rebuilds_.clear();
    }
    defer_mesh_rebuild_once_ = false;
    const auto install_started = std::chrono::steady_clock::now();
    auto next = stream_job_.get();
    stream_job_active_ = false;
    install_region(std::move(next), pending_biome_);
    queue_chunk_rebuilds(std::move(interrupted_rebuilds));
    stats_.stream_pending = mesh_job_active_ || !pending_mesh_rebuilds_.empty();
    stats_.last_stream_milliseconds =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - install_started)
            .count();
    ++stats_.stream_serial;
    return true;
}

bool WorldPreviewRenderer::preload_step(std::size_t mesh_budget)
{
    const bool installed = finish_stream_if_ready();
    pump_chunk_rebuilds(std::max<std::size_t>(mesh_budget, 1U));
    return installed;
}

bool WorldPreviewRenderer::preload_complete() const noexcept
{
    const std::size_t expected_chunks =
        static_cast<std::size_t>(active_span_) * static_cast<std::size_t>(active_span_);
    return !stream_job_active_ && !mesh_job_active_ && pending_mesh_rebuilds_.empty() &&
           region_.chunks().size() == expected_chunks;
}

float WorldPreviewRenderer::preload_progress() const noexcept
{
    const std::size_t expected_sections = static_cast<std::size_t>(active_span_) *
                                          static_cast<std::size_t>(active_span_) *
                                          static_cast<std::size_t>(world::chunk_section_count);
    if (expected_sections == 0U || preload_complete())
        return 1.0F;
    const float meshed = static_cast<float>(std::min(mesh_batches_.size(), expected_sections)) /
                         static_cast<float>(expected_sections);
    // Generation runs as one immutable worker result. Keep the loading bar alive while that
    // result is pending, then let actual admitted mesh sections drive it to completion.
    return std::clamp(stream_job_active_ ? std::max(0.08F, meshed * 0.35F) : meshed, 0.0F, 0.99F);
}

WorldPreviewRenderer::~WorldPreviewRenderer()
{
    discard_pending_stream();
    avatar_viewmodel_.reset();
    pickup_pickaxe_.reset();
    pickup_compass_.reset();
    pickup_pistol_.reset();
    pickup_knife_.reset();
    pickup_ammo_.reset();
    pickup_torch_.reset();
    pickup_assault_rifle_.reset();
    pickup_shotgun_.reset();
    pickup_rifle_.reset();
    pickup_smg_.reset();
    pickup_lmg_.reset();
    pickup_launcher_.reset();
    pickup_space_assault_rifle_.reset();
    pickup_space_pistol_.reset();
    pickup_space_shotgun_.reset();
    pickup_space_rifle_.reset();
    pickup_space_smg_.reset();
    pickup_ore_.reset();
    pickup_gems_.reset();
    pickup_bars_.reset();
    depth_renderer_.reset();
    SDL_DestroyTexture(atlas_);
    SDL_DestroyTexture(normal_spec_atlas_);
    SDL_DestroyTexture(metal_light_atlas_);
    SDL_DestroyTexture(block_cracks_);
    SDL_DestroyTexture(block_selector_);
    SDL_DestroyTexture(mining_smoke_);
    SDL_DestroyTexture(mining_sparks_);
    SDL_DestroyTexture(mining_rocks_);
    SDL_DestroyTexture(white_texture_);
    SDL_DestroyTexture(torch_fire_texture_);
    SDL_DestroyTexture(torch_smoke_texture_);
    SDL_DestroyTexture(flash_texture_);
    SDL_DestroyTexture(ice_flash_texture_);
    SDL_DestroyTexture(ice_puff_texture_);
    for (SkyCube *cube : {&clear_sky_, &dawn_sky_, &sunset_sky_, &night_sky_})
    {
        for (SDL_Texture *texture : *cube)
        {
            SDL_DestroyTexture(texture);
        }
    }
}

const WorldPreviewStats &WorldPreviewRenderer::stats() const noexcept
{
    return stats_;
}

const world::WorldRegion &WorldPreviewRenderer::region() const noexcept
{
    return region_;
}

world::WorldRegion &WorldPreviewRenderer::region() noexcept
{
    return region_;
}

bool WorldPreviewRenderer::stream_around(const world::PlayerState &player)
{
    // Face extraction runs from an immutable snapshot. Group the eight vertical
    // sections of one horizontal chunk into each worker so its shared light map
    // is computed once, while GPU installation remains a bounded per-frame slice.
    if (finish_stream_if_ready())
        return true;
    pump_chunk_rebuilds(static_cast<std::size_t>(world::chunk_section_count));
    const int player_x = static_cast<int>(std::floor(player.position.x));
    const int player_z = static_cast<int>(std::floor(player.position.z));
    const int player_chunk_x = floor_divide(player_x, world::chunk_width);
    const int player_chunk_z = floor_divide(player_z, world::chunk_depth);
    const int desired_origin_x = player_chunk_x - region_.chunks_x() / 2;
    const int desired_origin_z = player_chunk_z - region_.chunks_z() / 2;

    if (stream_job_active_)
    {
        if (!finish_stream_if_ready())
            return false;
        return true;
    }

    if (desired_origin_x == region_.origin_chunk_x() &&
        desired_origin_z == region_.origin_chunk_z())
    {
        return false;
    }
    world::WorldRegion current = region_;
    const world::WorldGenerator generator = generator_;
    pending_biome_ = generator_.biome_at(player_x, player_z);
    stream_job_ = std::async(std::launch::async, [generator, current = std::move(current),
                                                  desired_origin_x, desired_origin_z]() mutable {
        return generator.stream_region(current, desired_origin_x, desired_origin_z);
    });
    stream_job_active_ = true;
    stats_.stream_pending = true;
    return false;
}

void WorldPreviewRenderer::recenter_on(const world::PlayerState &player)
{
    const int player_chunk_x =
        floor_divide(static_cast<int>(std::floor(player.position.x)), world::chunk_width);
    const int player_chunk_z =
        floor_divide(static_cast<int>(std::floor(player.position.z)), world::chunk_depth);
    const int target_origin_x = player_chunk_x - active_span_ / 2;
    const int target_origin_z = player_chunk_z - active_span_ / 2;
    const world::BiomeKind biome =
        generator_.biome_at(static_cast<int>(std::floor(player.position.x)),
                            static_cast<int>(std::floor(player.position.z)));
    // A load or very early teleport can cancel the constructor's expanded-view
    // stream while region_ is still the four-chunk bootstrap window. Always
    // recenter at the configured active span so its loading screen can finish.
    if (region_.chunks_x() != active_span_ || region_.chunks_z() != active_span_)
    {
        discard_pending_stream();
        auto next = generator_.stream_region(region_, target_origin_x, target_origin_z,
                                             active_span_, active_span_);
        install_region(std::move(next), biome);
        return;
    }
    rebuild_region(target_origin_x, target_origin_z, biome);
}

void WorldPreviewRenderer::set_view_distance(int view_distance)
{
    discard_pending_stream();
    visibility_distance_ = visibility_for(view_distance);
    const int next_span = active_span_for(view_distance);
    if (next_span == active_span_)
    {
        return;
    }
    const int center_chunk_x = floor_divide(center_x(), world::chunk_width);
    const int center_chunk_z = floor_divide(center_z(), world::chunk_depth);
    active_span_ = next_span;
    world::WorldRegion current = region_;
    const world::WorldGenerator generator = generator_;
    const int target_origin_x = center_chunk_x - next_span / 2;
    const int target_origin_z = center_chunk_z - next_span / 2;
    stream_job_ =
        std::async(std::launch::async, [generator, current = std::move(current), target_origin_x,
                                        target_origin_z, next_span]() mutable {
            return generator.stream_region(current, target_origin_x, target_origin_z, next_span,
                                           next_span);
        });
    stream_job_active_ = true;
    pending_biome_ = biome_;
    stats_.stream_pending = true;
}

void WorldPreviewRenderer::remember_edit(int world_x, int world_y, int world_z,
                                         world::BlockType block)
{
    saved_edits_.insert_or_assign({world_x, world_y, world_z}, block);
}

std::optional<world::BlockType> WorldPreviewRenderer::remembered_block(int world_x, int world_y,
                                                                       int world_z) const noexcept
{
    const auto found = saved_edits_.find({world_x, world_y, world_z});
    return found == saved_edits_.end() ? std::nullopt
                                       : std::optional<world::BlockType>{found->second};
}

std::optional<world::BlockType> WorldPreviewRenderer::resolved_block(int world_x, int world_y,
                                                                     int world_z) const
{
    if (world_y < world::world_min_y ||
        world_y >= world::world_min_y + world::chunk_height)
        return std::nullopt;
    if (region_.contains(world_x, world_y, world_z))
        return region_.block(world_x, world_y, world_z);
    if (const auto edit = remembered_block(world_x, world_y, world_z); edit.has_value())
        return edit;

    const int chunk_x = floor_divide(world_x, world::chunk_width);
    const int chunk_z = floor_divide(world_z, world::chunk_depth);
    const world::ChunkCoordinate key{chunk_x, chunk_z, 0};
    auto found = authority_baselines_.find(key);
    if (found == authority_baselines_.end())
    {
        constexpr std::size_t maximum_authority_baselines = 64U;
        if (authority_baselines_.size() >= maximum_authority_baselines)
            authority_baselines_.erase(authority_baselines_.begin());
        found = authority_baselines_.emplace(key, generator_.generate_chunk(chunk_x, chunk_z)).first;
    }
    const int local_x = world_x - chunk_x * world::chunk_width;
    const int local_z = world_z - chunk_z * world::chunk_depth;
    return found->second.block(local_x, world_y - world::world_min_y, local_z);
}

std::vector<std::array<int, 4>> WorldPreviewRenderer::exported_edits() const
{
    std::vector<std::array<int, 4>> edits;
    edits.reserve(saved_edits_.size());
    for (const auto &[coordinate, block] : saved_edits_)
    {
        edits.push_back({coordinate.x, coordinate.y, coordinate.z, static_cast<int>(block)});
    }
    return edits;
}

void WorldPreviewRenderer::import_edits(const std::vector<std::array<int, 4>> &edits)
{
    for (const auto &edit : edits)
    {
        saved_edits_.insert_or_assign({edit[0], edit[1], edit[2]},
                                      static_cast<world::BlockType>(edit[3]));
    }
    apply_saved_edits();
    rebuild_mesh();
}

void WorldPreviewRenderer::replace_chunk_edits(
    int corner_x, int corner_z, const std::vector<std::array<int, 4>> &edits)
{
    const int chunk_x = floor_divide(corner_x, world::chunk_width);
    const int chunk_z = floor_divide(corner_z, world::chunk_depth);
    const int maximum_x = corner_x + world::chunk_width;
    const int maximum_z = corner_z + world::chunk_depth;
    std::erase_if(saved_edits_, [&](const auto &entry) {
        const BlockCoordinate &coordinate = entry.first;
        return coordinate.x >= corner_x && coordinate.x < maximum_x &&
               coordinate.z >= corner_z && coordinate.z < maximum_z;
    });
    for (const auto &edit : edits)
        saved_edits_.insert_or_assign({edit[0], edit[1], edit[2]},
                                      static_cast<world::BlockType>(edit[3]));

    const int region_x = chunk_x - region_.origin_chunk_x();
    const int region_z = chunk_z - region_.origin_chunk_z();
    if (region_x < 0 || region_x >= region_.chunks_x() || region_z < 0 ||
        region_z >= region_.chunks_z())
        return;

    // A worker may still be meshing the old voxel snapshot. Retire it before
    // replacing the resident column so it cannot install stale faces later.
    discard_pending_mesh();
    const std::size_t chunk_index =
        static_cast<std::size_t>(region_x + region_.chunks_x() * region_z);
    region_.chunks()[chunk_index] = generator_.generate_chunk(chunk_x, chunk_z);
    for (const auto &edit : edits)
        region_.set_block(edit[0], edit[1], edit[2],
                          static_cast<world::BlockType>(edit[3]));

    // A high-Y edit selects every vertical mesh section after the lighting
    // influence expansion, while also remeshing neighbouring chunk borders.
    rebuild_block_edits({{corner_x, world::world_min_y + world::chunk_height - 1, corner_z}});
}

void WorldPreviewRenderer::spawn_mining_impact(const world::BlockBreakUpdate &impact)
{
    // Preserve the original 54-particle impact burst while bounding accumulated work when
    // several impacts overlap. The oldest particles are visually the least significant.
    constexpr std::size_t retained_before_burst = 162U;
    if (mining_particles_.size() > retained_before_burst)
    {
        mining_particles_.erase(
            mining_particles_.begin(),
            mining_particles_.begin() +
                static_cast<std::ptrdiff_t>(mining_particles_.size() - retained_before_burst));
    }
    const auto random_unit = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
                   static_cast<float>(0x00FFFFFFU) * 2.0F -
               1.0F;
    };
    world::FaceDirection face = world::FaceDirection::positive_y;
    if (impact.normal_x > 0)
        face = world::FaceDirection::positive_x;
    else if (impact.normal_x < 0)
        face = world::FaceDirection::negative_x;
    else if (impact.normal_z > 0)
        face = world::FaceDirection::positive_z;
    else if (impact.normal_z < 0)
        face = world::FaceDirection::negative_z;
    else if (impact.normal_y < 0)
        face = world::FaceDirection::negative_y;

    const world::Vec3 normal{static_cast<float>(impact.normal_x),
                             static_cast<float>(impact.normal_y),
                             static_cast<float>(impact.normal_z)};
    const auto random_range = [&](float minimum, float maximum) {
        return minimum + (random_unit() * 0.5F + 0.5F) * (maximum - minimum);
    };
    const auto horizontal_velocity = [&](float minimum, float maximum) {
        const float angle = random_range(0.0F, std::numbers::pi_v<float> * 2.0F);
        const float speed = random_range(minimum, maximum);
        return world::Vec3{std::cos(angle) * speed, 0.0F, std::sin(angle) * speed};
    };
    const auto emit = [&](ParticleKind kind, int count) {
        for (int index = 0; index < count; ++index)
        {
            MiningParticle particle;
            particle.position = {impact.intersection.x + normal.x * 0.035F,
                                 impact.intersection.y + normal.y * 0.035F,
                                 impact.intersection.z + normal.z * 0.035F};
            particle.position.x += random_unit() * 0.055F;
            particle.position.y += random_unit() * 0.055F;
            particle.position.z += random_unit() * 0.055F;
            particle.block = impact.block;
            particle.tile = world::atlas_tile(impact.block, face);
            particle.kind = kind;
            if (kind == ParticleKind::rock)
            {
                particle.velocity = horizontal_velocity(1.0F, 6.0F);
                particle.velocity.x += normal.x * 2.0F;
                particle.velocity.y = random_range(1.0F, 6.0F) + normal.y * 2.0F;
                particle.velocity.z += normal.z * 2.0F;
                particle.lifetime = 1.0F;
                particle.size = random_range(0.025F, 0.09F);
                particle.end_size = random_range(0.04F, 0.11F);
                particle.rotation = random_range(0.0F, 360.0F);
                particle.angular_velocity = random_range(0.0F, 20.0F);
            }
            else if (kind == ParticleKind::spark)
            {
                particle.velocity = horizontal_velocity(1.0F, 5.0F);
                particle.velocity.x += normal.x * 2.0F;
                particle.velocity.y = random_range(1.0F, 5.0F) + normal.y * 2.0F;
                particle.velocity.z += normal.z * 2.0F;
                particle.lifetime = std::max(0.08F, 1.0F + random_unit());
                particle.size = random_range(0.006F, 0.025F);
                particle.end_size = random_range(0.02F, 0.07F);
            }
            else if (kind == ParticleKind::mining_dust)
            {
                // Dust expands away from the struck plane instead of stacking
                // four giant quads under the crosshair. Its colour comes from
                // the mined material, making sand, snow, bloodstone and rock
                // impacts read differently without requiring new textures.
                const world::BlockColor material = world::fallback_color(impact.block);
                particle.position.x += normal.x * 0.09F;
                particle.position.y += normal.y * 0.09F;
                particle.position.z += normal.z * 0.09F;
                particle.velocity = horizontal_velocity(0.12F, 0.55F);
                particle.velocity.x += normal.x * random_range(0.28F, 0.62F);
                particle.velocity.y =
                    random_range(0.18F, 0.68F) + normal.y * random_range(0.20F, 0.48F);
                particle.velocity.z += normal.z * random_range(0.28F, 0.62F);
                particle.lifetime = random_range(1.25F, 1.65F);
                particle.size = random_range(0.16F, 0.25F);
                particle.end_size = random_range(1.05F, 1.65F);
                particle.rotation = random_range(0.0F, 360.0F);
                particle.angular_velocity = random_range(-0.55F, 0.55F) * 57.29578F;
                particle.opacity = random_range(0.80F, 0.96F);
                particle.tint = {0.40F + 0.50F * static_cast<float>(material.red) / 255.0F,
                                 0.40F + 0.50F * static_cast<float>(material.green) / 255.0F,
                                 0.40F + 0.50F * static_cast<float>(material.blue) / 255.0F};
            }
            mining_particles_.push_back(particle);
        }
    };
    // The source used the same 25 rocks + 25 sparks + smoke recipe for every
    // voxel. Keep that energy for ores and metal, but let material decide the
    // mix: sand/snow become powder, timber becomes chips, and plain stone gets
    // a restrained spark accent instead of a shower of glowing embers.
    const auto scaled_count = [&](int original_count) {
        if (original_count <= 0)
            return 0;
        return std::max(1, static_cast<int>(std::lround(static_cast<float>(original_count) *
                                                        particle_density_)));
    };
    const bool powder =
        impact.block == world::BlockType::dirt || impact.block == world::BlockType::grass ||
        impact.block == world::BlockType::sand || impact.block == world::BlockType::snow ||
        impact.block == world::BlockType::leaves || impact.block == world::BlockType::slime;
    const bool timber = impact.block == world::BlockType::log ||
                        impact.block == world::BlockType::wood ||
                        impact.block == world::BlockType::crate ||
                        world::is_lower_door(impact.block) || world::is_upper_door(impact.block);
    const bool metal_or_ore = (impact.block >= world::BlockType::gold_ore &&
                               impact.block <= world::BlockType::diamond_ore) ||
                              (impact.block >= world::BlockType::iron_wall &&
                               impact.block <= world::BlockType::diamond_wall);
    emit(ParticleKind::rock, scaled_count(powder ? 8 : timber ? 14 : metal_or_ore ? 18 : 16));
    emit(ParticleKind::spark, scaled_count(powder || timber                         ? 0
                                           : metal_or_ore                           ? 20
                                           : impact.block == world::BlockType::rock ? 8
                                                                                    : 6));
    emit(ParticleKind::mining_dust, scaled_count(powder ? 7 : timber ? 5 : 6));
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::set_particle_density(float density) noexcept
{
    particle_density_ = std::clamp(density, 0.20F, 1.50F);
}

void WorldPreviewRenderer::set_grading(int tone_curve, bool auto_exposure,
                                       float exposure_compensation, float color_grading) noexcept
{
    if (depth_renderer_ != nullptr)
        depth_renderer_->set_grading(tone_curve, auto_exposure, exposure_compensation,
                                     color_grading);
}

void WorldPreviewRenderer::spawn_impact_smoke(world::Vec3 position,
                                              const std::array<float, 3> &tint)
{
    const auto random_unit = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
                   static_cast<float>(0x00FFFFFFU) * 2.0F -
               1.0F;
    };
    // A hit gets three separated Smoke2 puffs at default density: enough mass to
    // read as an impact, still far below an explosion cloud. Soft depth fading
    // handles intersections; opacity is no longer crushed merely for being near
    // the player.
    const int count = std::max(1, static_cast<int>(std::lround(3.0F * particle_density_)));
    for (int index = 0; index < count; ++index)
    {
        const float angle = (random_unit() + 1.0F) * std::numbers::pi_v<float>;
        const float horizontal = 0.5F + (random_unit() + 1.0F) * 0.25F;
        MiningParticle particle;
        particle.kind = ParticleKind::impact_smoke;
        particle.position = {position.x + std::cos(angle) * 0.06F,
                             position.y + (random_unit() + 1.0F) * 0.035F,
                             position.z + std::sin(angle) * 0.06F};
        particle.velocity = {std::cos(angle) * horizontal * 0.72F,
                             0.18F + (random_unit() + 1.0F) * 0.22F,
                             std::sin(angle) * horizontal * 0.72F};
        particle.lifetime = 1.05F + (random_unit() + 1.0F) * 0.10F;
        particle.size = 0.13F + (random_unit() + 1.0F) * 0.035F;
        particle.end_size = 0.78F + (random_unit() + 1.0F) * 0.16F;
        particle.rotation = (random_unit() + 1.0F) * 180.0F;
        particle.angular_velocity = random_unit() * 1.5F * 57.29578F;
        particle.opacity = 0.78F + (random_unit() + 1.0F) * 0.08F;
        particle.tint = {tint[0] * 0.72F, tint[1] * 0.72F, tint[2] * 0.70F};
        mining_particles_.push_back(particle);
    }
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::spawn_tracer_impact(const world::BlockBreakUpdate &impact)
{
    constexpr std::size_t retained_before_burst = 162U;
    if (mining_particles_.size() > retained_before_burst)
    {
        mining_particles_.erase(
            mining_particles_.begin(),
            mining_particles_.begin() +
                static_cast<std::ptrdiff_t>(mining_particles_.size() - retained_before_burst));
    }
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    const world::Vec3 normal{static_cast<float>(impact.normal_x),
                             static_cast<float>(impact.normal_y),
                             static_cast<float>(impact.normal_z)};
    const int spark_count = std::max(1, static_cast<int>(std::lround(25.0F * particle_density_)));
    for (int index = 0; index < spark_count; ++index)
    {
        const float angle = random01() * std::numbers::pi_v<float> * 2.0F;
        const float horizontal = 1.0F + random01() * 4.0F;
        MiningParticle particle;
        particle.kind = ParticleKind::spark;
        particle.position = {impact.intersection.x + normal.x * 0.035F,
                             impact.intersection.y + normal.y * 0.035F,
                             impact.intersection.z + normal.z * 0.035F};
        particle.velocity = {std::cos(angle) * horizontal + normal.x * 2.0F,
                             1.0F + random01() * 4.0F + normal.y * 2.0F,
                             std::sin(angle) * horizontal + normal.z * 2.0F};
        particle.lifetime = std::max(0.08F, random01() * 2.0F);
        particle.size = 0.01F + random01() * 0.04F;
        particle.end_size = 0.05F + random01() * 0.15F;
        mining_particles_.push_back(particle);
    }
    spawn_impact_smoke(impact.intersection, {1.0F, 1.0F, 1.0F});
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::spawn_blaster_impact(const world::BlockBreakUpdate &impact)
{
    constexpr std::size_t retained_before_burst = 162U;
    if (mining_particles_.size() > retained_before_burst)
    {
        mining_particles_.erase(
            mining_particles_.begin(),
            mining_particles_.begin() +
                static_cast<std::ptrdiff_t>(mining_particles_.size() - retained_before_burst));
    }
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    const world::Vec3 normal{static_cast<float>(impact.normal_x),
                             static_cast<float>(impact.normal_y),
                             static_cast<float>(impact.normal_z)};

    // BlasterFlash: five particles, 1s life, +/-1 velocity, full size 1 ->
    // 3..5, and BlasterShot applies LocalScale=.01 to this emitter only.
    for (int index = 0; index < 5; ++index)
    {
        MiningParticle particle;
        particle.kind = ParticleKind::flash;
        particle.position = impact.intersection;
        particle.velocity = {random01() * 2.0F - 1.0F, random01() * 2.0F - 1.0F,
                             random01() * 2.0F - 1.0F};
        particle.lifetime = 1.0F;
        particle.size = 0.005F;
        particle.end_size = (3.0F + random01() * 2.0F) * 0.005F;
        particle.rotation = random01() * 360.0F;
        mining_particles_.push_back(particle);
    }

    const int spark_count = std::max(1, static_cast<int>(std::lround(25.0F * particle_density_)));
    for (int index = 0; index < spark_count; ++index)
    {
        const float angle = random01() * std::numbers::pi_v<float> * 2.0F;
        const float horizontal = 1.0F + random01() * 4.0F;
        MiningParticle particle;
        particle.kind = ParticleKind::spark;
        particle.position = {impact.intersection.x + normal.x * 0.035F,
                             impact.intersection.y + normal.y * 0.035F,
                             impact.intersection.z + normal.z * 0.035F};
        particle.velocity = {std::cos(angle) * horizontal + normal.x * 2.0F,
                             1.0F + random01() * 4.0F + normal.y * 2.0F,
                             std::sin(angle) * horizontal + normal.z * 2.0F};
        particle.lifetime = std::max(0.08F, random01() * 2.0F);
        particle.size = 0.01F + random01() * 0.04F;
        particle.end_size = 0.05F + random01() * 0.15F;
        mining_particles_.push_back(particle);
    }
    spawn_impact_smoke(impact.intersection, {1.0F, 1.0F, 1.0F});
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::spawn_flash_effect(world::Vec3 position, std::array<float, 3> tint)
{
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    // FlashEffect: 100/s for 0.05s = 5 particles, life 1s, velocity +/-1 on
    // every axis, size 3-6 growing to 20-50 (halved to billboard half-extents).
    for (int index = 0; index < 5; ++index)
    {
        MiningParticle particle;
        particle.kind = tint[2] > tint[0] + 0.12F ? ParticleKind::ice_flash : ParticleKind::flash;
        particle.position = position;
        particle.velocity = {random01() * 2.0F - 1.0F, random01() * 2.0F - 1.0F,
                             random01() * 2.0F - 1.0F};
        particle.lifetime = 1.0F;
        particle.size = (3.0F + random01() * 3.0F) * 0.5F;
        particle.end_size = (20.0F + random01() * 30.0F) * 0.5F;
        particle.rotation = random01() * 360.0F;
        particle.tint = tint;
        mining_particles_.push_back(particle);
    }
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::spawn_fireball_explosion(world::Vec3 position, bool ice,
                                                    bool rock_chunks)
{
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    // Detonate: FlashEffect + FirePuff + BigSmokePuff + BigRockBlast, one each.
    const std::array<float, 3> fire_tint =
        ice ? std::array<float, 3>{0.65F, 0.85F, 1.0F} : std::array<float, 3>{1.0F, 1.0F, 1.0F};
    spawn_flash_effect(position, fire_tint);
    // FirePuff: 50/s for 2 seconds, life 3s (variation 1), rises 0-3.
    EffectEmitter fire_puff;
    fire_puff.kind = ice ? ParticleKind::ice_soft : ParticleKind::fire_soft;
    fire_puff.position = position;
    fire_puff.rate = 50.0F * particle_density_;
    fire_puff.time_left = 2.0F;
    fire_puff.life_base = 3.0F;
    fire_puff.life_variation = 1.0F;
    fire_puff.horizontal_min = 0.0F;
    fire_puff.horizontal_max = 2.0F;
    fire_puff.vertical_min = 0.0F;
    fire_puff.vertical_max = 3.0F;
    fire_puff.start_min = 0.0F;
    fire_puff.start_max = 1.0F;
    fire_puff.end_min = 0.5F;
    fire_puff.end_max = 7.5F;
    fire_puff.rotate_min = -0.5F * 57.29578F;
    fire_puff.rotate_max = 0.5F * 57.29578F;
    fire_puff.tint = fire_tint;
    effect_emitters_.push_back(fire_puff);
    // BigSmokePuff: 10/s for 3 seconds, life 5s (variation 1), drifts 0-5.
    EffectEmitter big_smoke;
    big_smoke.kind = ParticleKind::smoke;
    big_smoke.position = position;
    big_smoke.rate = 10.0F * particle_density_;
    big_smoke.time_left = 3.0F;
    big_smoke.life_base = 5.0F;
    big_smoke.life_variation = 1.0F;
    big_smoke.horizontal_min = 0.0F;
    big_smoke.horizontal_max = 5.0F;
    big_smoke.vertical_min = 0.0F;
    big_smoke.vertical_max = 5.0F;
    big_smoke.start_min = 2.5F;
    big_smoke.start_max = 5.0F;
    big_smoke.end_min = 10.0F;
    big_smoke.end_max = 25.0F;
    effect_emitters_.push_back(big_smoke);
    // BigRockBlast: 2000/s for 0.05s, shrapnel at 1-60 units/s under gravity,
    // warm 255,200,128..white tint. Scaled by the particle-density setting.
    const int rocks =
        rock_chunks ? std::max(8, static_cast<int>(std::lround(60.0F * particle_density_))) : 0;
    for (int index = 0; index < rocks; ++index)
    {
        MiningParticle particle;
        particle.kind = ParticleKind::blast_rock;
        particle.position = position;
        const float angle = random01() * 6.2831853F;
        const float horizontal = 1.0F + random01() * 59.0F;
        particle.velocity = {std::cos(angle) * horizontal, 1.0F + random01() * 59.0F,
                             std::sin(angle) * horizontal};
        particle.lifetime = 1.0F;
        particle.size = (0.5F + random01() * 1.5F) * 0.5F;
        particle.end_size = particle.size;
        particle.rotation = random01() * 360.0F;
        particle.angular_velocity = random01() * 20.0F * 57.29578F;
        const float mix = random01();
        particle.tint = {1.0F, (200.0F + 55.0F * mix) / 255.0F, (128.0F + 127.0F * mix) / 255.0F};
        mining_particles_.push_back(particle);
    }
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::update_effect_emitters(float delta)
{
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    for (EffectEmitter &emitter : effect_emitters_)
    {
        const float emit_delta = std::min(delta, emitter.time_left);
        emitter.time_left -= delta;
        emitter.accumulator += emitter.rate * std::max(0.0F, emit_delta);
        while (emitter.accumulator >= 1.0F)
        {
            emitter.accumulator -= 1.0F;
            MiningParticle particle;
            particle.kind = emitter.kind;
            particle.position = emitter.position;
            const float angle = random01() * 6.2831853F;
            const float horizontal = emitter.horizontal_min +
                                     random01() * (emitter.horizontal_max - emitter.horizontal_min);
            particle.velocity = {std::cos(angle) * horizontal,
                                 emitter.vertical_min +
                                     random01() * (emitter.vertical_max - emitter.vertical_min),
                                 std::sin(angle) * horizontal};
            particle.lifetime =
                std::max(0.1F, emitter.life_base * (1.0F - emitter.life_variation * random01()));
            particle.size =
                emitter.start_min + random01() * (emitter.start_max - emitter.start_min);
            particle.end_size = emitter.end_min + random01() * (emitter.end_max - emitter.end_min);
            particle.rotation = random01() * 360.0F;
            particle.angular_velocity =
                emitter.rotate_min + random01() * (emitter.rotate_max - emitter.rotate_min);
            particle.tint = emitter.tint;
            mining_particles_.push_back(particle);
        }
    }
    std::erase_if(effect_emitters_,
                  [](const EffectEmitter &emitter) { return emitter.time_left <= 0.0F; });
}

void WorldPreviewRenderer::emit_fireball_trails(float delta)
{
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    for (auto &[id, state] : fireball_trails_)
        state.seen = false;
    for (const FireballVisual &fireball : fireball_visuals_)
    {
        FireballTrailState &state = fireball_trails_[fireball.id];
        state.seen = true;
        const std::array<float, 3> fire_tint = fireball.ice
                                                   ? std::array<float, 3>{0.65F, 0.85F, 1.0F}
                                                   : std::array<float, 3>{1.0F, 1.0F, 1.0F};
        // FireTrail: 50/s, life 0-1s, shrinks from up to 7.5 to nothing.
        state.fire += 50.0F * particle_density_ * delta;
        while (state.fire >= 1.0F)
        {
            state.fire -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::fire_soft;
            particle.position = fireball.position;
            particle.lifetime = std::max(0.1F, 1.0F * (1.0F - random01()));
            particle.size = (1.0F + random01() * 14.0F) * 0.5F * 0.35F;
            particle.end_size = 0.0F;
            particle.rotation = random01() * 360.0F;
            particle.angular_velocity = (random01() * 4.0F - 2.0F) * 57.29578F;
            particle.tint = fire_tint;
            mining_particles_.push_back(particle);
        }
        // SmokeTrail: 50/s, life 0-3s, grows to ~7.
        state.smoke += 50.0F * particle_density_ * delta;
        while (state.smoke >= 1.0F)
        {
            state.smoke -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::smoke;
            particle.position = fireball.position;
            particle.lifetime = std::max(0.15F, 3.0F * (1.0F - random01()));
            particle.size = random01() * 1.5F * 0.35F;
            particle.end_size = (6.5F + random01() * 1.0F) * 0.35F;
            particle.rotation = random01() * 360.0F;
            particle.tint = {0.75F, 0.75F, 0.75F};
            mining_particles_.push_back(particle);
        }
        // FireGlow + FireBallEffect are LocalSpace: they ride the fireball, so
        // the particles inherit its velocity.
        state.glow += 10.0F * particle_density_ * delta;
        while (state.glow >= 1.0F)
        {
            state.glow -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::flash;
            particle.position = fireball.position;
            particle.velocity = fireball.velocity;
            particle.lifetime = 1.0F;
            particle.size = (6.0F + random01() * 6.0F) * 0.5F * 0.35F;
            particle.end_size = (10.0F + random01() * 20.0F) * 0.5F * 0.35F;
            particle.rotation = random01() * 360.0F;
            particle.tint = fire_tint;
            mining_particles_.push_back(particle);
        }
        state.ball += 10.0F * particle_density_ * delta;
        while (state.ball >= 1.0F)
        {
            state.ball -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::fire_soft;
            particle.position = fireball.position;
            particle.velocity = fireball.velocity;
            particle.lifetime = 1.0F;
            particle.size = (3.0F + random01() * 2.0F) * 0.5F * 0.35F;
            particle.end_size = (5.0F + random01() * 15.0F) * 0.5F * 0.35F;
            particle.rotation = random01() * 360.0F;
            particle.angular_velocity = (random01() * 4.0F - 2.0F) * 57.29578F;
            particle.tint = fire_tint;
            mining_particles_.push_back(particle);
        }
    }
    std::erase_if(fireball_trails_, [](const auto &entry) { return !entry.second.seen; });
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::emit_rocket_and_fuse_trails(float delta)
{
    const auto random01 = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
               static_cast<float>(0x00FFFFFFU);
    };
    for (auto &[id, state] : rocket_trails_)
        state.seen = false;
    for (const RocketVisual &rocket : rocket_visuals_)
    {
        RocketTrailState &state = rocket_trails_[rocket.id];
        state.seen = true;
        const world::Vec3 trail_position{rocket.position.x - rocket.direction.x * 0.25F,
                                         rocket.position.y - rocket.direction.y * 0.25F,
                                         rocket.position.z - rocket.direction.z * 0.25F};
        // RocketFireTrail: 200/s, 0.2 s maximum life, 0.5..1 -> 0..0.2.
        state.fire += 200.0F * particle_density_ * delta;
        while (state.fire >= 1.0F)
        {
            state.fire -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::fire_soft;
            particle.position = trail_position;
            particle.velocity = {-rocket.direction.x * 2.0F, -rocket.direction.y * 2.0F,
                                 -rocket.direction.z * 2.0F};
            particle.lifetime = std::max(0.02F, 0.2F * (1.0F - random01()));
            particle.size = (0.5F + random01() * 0.5F) * 0.5F;
            particle.end_size = random01() * 0.2F * 0.5F;
            particle.rotation = random01() * 360.0F;
            particle.angular_velocity = (random01() * 4.0F - 2.0F) * 57.29578F;
            mining_particles_.push_back(particle);
        }
        // RocketSmokeTrail: 50/s, up to three seconds, 0..2 -> 3..15.
        state.smoke += 50.0F * particle_density_ * delta;
        while (state.smoke >= 1.0F)
        {
            state.smoke -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::smoke;
            particle.position = trail_position;
            particle.velocity = {-rocket.direction.x * 0.5F, -rocket.direction.y * 0.5F,
                                 -rocket.direction.z * 0.5F};
            particle.lifetime = std::max(0.15F, 3.0F * (1.0F - random01()));
            particle.size = random01() * 2.0F * 0.35F;
            particle.end_size = (3.0F + random01() * 12.0F) * 0.35F;
            particle.rotation = random01() * 360.0F;
            particle.angular_velocity = random01() * 0.5F * 57.29578F;
            mining_particles_.push_back(particle);
        }
    }
    std::erase_if(rocket_trails_, [](const auto &entry) { return !entry.second.seen; });

    for (auto &[id, state] : fuse_trails_)
        state.seen = false;
    for (const ExplosiveFuseVisual &fuse : explosive_fuse_visuals_)
    {
        FuseTrailState &state = fuse_trails_[fuse.id];
        state.seen = true;
        // ExplosiveFlashEntity carries TorchSmoke one block above the charge.
        state.smoke += 10.0F * particle_density_ * delta;
        while (state.smoke >= 1.0F)
        {
            state.smoke -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::smoke;
            particle.position = {static_cast<float>(fuse.x) + 0.5F,
                                 static_cast<float>(fuse.y) + 1.0F,
                                 static_cast<float>(fuse.z) + 0.5F};
            const float angle = random01() * 6.2831853F;
            const float horizontal = random01() * 0.2F;
            particle.velocity = {std::cos(angle) * horizontal, 0.5F + random01(),
                                 std::sin(angle) * horizontal};
            particle.lifetime = 2.0F;
            particle.size = 0.1F + random01() * 0.3F;
            particle.end_size = 0.5F + random01() * 0.5F;
            particle.rotation = random01() * 360.0F;
            mining_particles_.push_back(particle);
        }
    }
    std::erase_if(fuse_trails_, [](const auto &entry) { return !entry.second.seen; });
    stats_.mining_particle_count = mining_particles_.size();
}

void WorldPreviewRenderer::spawn_block_pickup(const world::BlockBreakUpdate &mined)
{
    const InventoryItemId drop = mined_item(mined.block);
    if (drop == InventoryItemId::bare_hands)
    {
        return;
    }
    const auto random_unit = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
                   static_cast<float>(0x00FFFFFFU) * 2.0F -
               1.0F;
    };
    world::Vec3 direction{random_unit() * 0.5F, 0.1F, random_unit() * 0.5F};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length > 0.0001F)
    {
        direction.x = direction.x / length * 1.5F;
        direction.y = direction.y / length * 1.5F;
        direction.z = direction.z / length * 1.5F;
    }
    BlockPickup pickup;
    pickup.position = {static_cast<float>(mined.x) + 0.5F, static_cast<float>(mined.y) + 0.5F,
                       static_cast<float>(mined.z) + 0.5F};
    pickup.velocity = direction;
    pickup.item = drop;
    pickup.block = item_block(drop);
    pickup.rounds_in_clip = drop == InventoryItemId::pistol ? 8U : 0U;
    pickup.bounce_phase = (random_unit() + 1.0F) * 1.57079632679F;
    if (!register_spawned_network_pickup(pickup, false))
        return;
    block_pickups_.push_back(pickup);
    stats_.block_pickup_count = block_pickups_.size();
}

void WorldPreviewRenderer::set_enemies(const std::vector<world::Enemy> *enemies) noexcept
{
    visible_enemies_ = enemies;
}

void WorldPreviewRenderer::set_remote_players(
    const std::vector<RemotePlayerVisual> *players) noexcept
{
    visible_remote_players_ = players;
}

void WorldPreviewRenderer::set_dragon(const world::Dragon *dragon) noexcept
{
    visible_dragon_ = dragon;
}

void WorldPreviewRenderer::set_doors(const WorldSaveDoorMap *doors) noexcept
{
    visible_doors_ = doors;
}

void WorldPreviewRenderer::set_projectiles(std::vector<ProjectileVisual> projectiles) noexcept
{
    projectile_visuals_ = std::move(projectiles);
}

void WorldPreviewRenderer::set_rockets(std::vector<RocketVisual> rockets) noexcept
{
    rocket_visuals_ = std::move(rockets);
}

void WorldPreviewRenderer::set_explosive_fuses(std::vector<ExplosiveFuseVisual> fuses) noexcept
{
    explosive_fuse_visuals_ = std::move(fuses);
}

void WorldPreviewRenderer::set_fireball_visuals(std::vector<FireballVisual> fireballs) noexcept
{
    fireball_visuals_ = std::move(fireballs);
}

EnemyModelLibrary *WorldPreviewRenderer::enemy_models() noexcept
{
    return enemy_models_.get();
}

void WorldPreviewRenderer::set_network_pickups_enabled(bool enabled,
                                                       std::int32_t local_spawner_id) noexcept
{
    network_pickups_enabled_ = enabled;
    local_pickup_spawner_id_ = enabled ? local_spawner_id : -1;
    if (!enabled)
    {
        spawned_network_pickups_.clear();
        for (BlockPickup &pickup : block_pickups_)
        {
            pickup.pickup_id.reset();
            pickup.spawner_id = -1;
            pickup.request_pending = false;
        }
    }
}

bool WorldPreviewRenderer::register_spawned_network_pickup(BlockPickup &pickup, bool dropped)
{
    if (!network_pickups_enabled_)
        return true;
    if (local_pickup_spawner_id_ < 0 || local_pickup_spawner_id_ > 255 ||
        block_pickups_.size() >= 1000U)
        return false;
    if (next_local_pickup_id_ == std::numeric_limits<std::int32_t>::max())
        next_local_pickup_id_ = 0;
    while (std::ranges::any_of(block_pickups_, [&](const BlockPickup &existing) {
        return existing.pickup_id == next_local_pickup_id_ &&
               existing.spawner_id == local_pickup_spawner_id_;
    }))
    {
        if (next_local_pickup_id_ == std::numeric_limits<std::int32_t>::max())
            next_local_pickup_id_ = 0;
        else
            ++next_local_pickup_id_;
    }
    pickup.pickup_id = next_local_pickup_id_++;
    pickup.spawner_id = local_pickup_spawner_id_;
    InventoryStack stack{pickup.item, pickup.count};
    stack.rounds_in_clip = pickup.rounds_in_clip;
    stack.durability = pickup.durability;
    stack.waypoint = pickup.waypoint;
    stack.waypoint_name = pickup.waypoint_name;
    stack.waypoint_valid = pickup.waypoint_valid;
    spawned_network_pickups_.push_back({std::move(stack), pickup.position, pickup.velocity,
                                        *pickup.pickup_id, dropped, pickup.display_on_pickup});
    return true;
}

std::vector<SpawnedNetworkPickup> WorldPreviewRenderer::drain_spawned_network_pickups(
    std::int32_t spawner_id)
{
    for (BlockPickup &pickup : block_pickups_)
        if (pickup.pickup_id.has_value() && pickup.spawner_id < 0)
            pickup.spawner_id = spawner_id;
    std::vector<SpawnedNetworkPickup> result = std::move(spawned_network_pickups_);
    spawned_network_pickups_.clear();
    return result;
}

bool WorldPreviewRenderer::spawn_network_pickup(const InventoryStack &stack, world::Vec3 position,
                                                world::Vec3 velocity, std::int32_t pickup_id,
                                                std::int32_t spawner_id, bool dropped,
                                                bool display_on_pickup)
{
    if (!stack.valid() || pickup_id < 0 || spawner_id < 0 || spawner_id > 255 ||
        block_pickups_.size() >= 1000U ||
        std::ranges::any_of(block_pickups_, [&](const BlockPickup &pickup) {
            return pickup.pickup_id == pickup_id && pickup.spawner_id == spawner_id;
        }))
        return false;
    BlockPickup pickup;
    pickup.position = position;
    pickup.velocity = velocity;
    pickup.item = stack.item;
    pickup.block = stack.block;
    pickup.count = stack.count;
    pickup.rounds_in_clip = stack.rounds_in_clip;
    pickup.durability = stack.durability;
    pickup.waypoint = stack.waypoint;
    pickup.waypoint_name = stack.waypoint_name;
    pickup.waypoint_valid = stack.waypoint_valid;
    pickup.pickup_id = pickup_id;
    pickup.spawner_id = spawner_id;
    pickup.ready_for_pickup = !dropped || spawner_id != local_pickup_spawner_id_;
    pickup.display_on_pickup = display_on_pickup;
    pickup.bounce_phase = 1.57079632679F;
    block_pickups_.push_back(std::move(pickup));
    stats_.block_pickup_count = block_pickups_.size();
    return true;
}

std::optional<CollectedBlockStack> WorldPreviewRenderer::find_network_pickup(
    std::int32_t pickup_id, std::int32_t spawner_id) const
{
    const auto found = std::ranges::find_if(block_pickups_, [&](const BlockPickup &pickup) {
        return pickup.pickup_id == pickup_id && pickup.spawner_id == spawner_id;
    });
    if (found == block_pickups_.end())
        return std::nullopt;
    InventoryStack stack{found->item, found->count};
    stack.rounds_in_clip = found->rounds_in_clip;
    stack.durability = found->durability;
    stack.waypoint = found->waypoint;
    stack.waypoint_name = found->waypoint_name;
    stack.waypoint_valid = found->waypoint_valid;
    return CollectedBlockStack{std::move(stack), found->position, found->pickup_id,
                               found->spawner_id, found->display_on_pickup};
}

std::optional<CollectedBlockStack> WorldPreviewRenderer::consume_network_pickup(
    std::int32_t pickup_id, std::int32_t spawner_id)
{
    const auto found = std::ranges::find_if(block_pickups_, [&](const BlockPickup &pickup) {
        return pickup.pickup_id == pickup_id && pickup.spawner_id == spawner_id;
    });
    if (found == block_pickups_.end())
        return std::nullopt;
    std::optional<CollectedBlockStack> result = find_network_pickup(pickup_id, spawner_id);
    if (!result.has_value())
        return std::nullopt;
    block_pickups_.erase(found);
    stats_.block_pickup_count = block_pickups_.size();
    return result;
}

void WorldPreviewRenderer::clear_network_pickup_request(std::int32_t pickup_id,
                                                        std::int32_t spawner_id) noexcept
{
    const auto found = std::ranges::find_if(block_pickups_, [&](const BlockPickup &pickup) {
        return pickup.pickup_id == pickup_id && pickup.spawner_id == spawner_id;
    });
    if (found != block_pickups_.end())
        found->request_pending = false;
}

void WorldPreviewRenderer::spawn_dropped_stack_at(const InventoryStack &stack, world::Vec3 position,
                                                  float upward_speed)
{
    if (!stack.valid())
        return;
    const auto random_unit = [&]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0x00FFFFFFU) /
                   static_cast<float>(0x00FFFFFFU) * 2.0F -
               1.0F;
    };
    world::Vec3 direction{random_unit() * 0.5F, 0.1F, random_unit() * 0.5F};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length > 0.0001F)
    {
        direction.x = direction.x / length * upward_speed;
        direction.y = direction.y / length * upward_speed;
        direction.z = direction.z / length * upward_speed;
    }
    BlockPickup pickup;
    pickup.position = position;
    pickup.velocity = direction;
    pickup.item = stack.item;
    pickup.block = stack.block;
    pickup.count = stack.count;
    pickup.rounds_in_clip = stack.rounds_in_clip;
    pickup.durability = stack.durability;
    pickup.waypoint = stack.waypoint;
    pickup.waypoint_name = stack.waypoint_name;
    pickup.waypoint_valid = stack.waypoint_valid;
    pickup.ready_for_pickup = false;
    pickup.bounce_phase = 1.57079632679F;
    if (!register_spawned_network_pickup(pickup, false))
        return;
    block_pickups_.push_back(pickup);
    stats_.block_pickup_count = block_pickups_.size();
}

void WorldPreviewRenderer::spawn_created_stack_at(const InventoryStack &stack, world::Vec3 position)
{
    if (!stack.valid())
        return;
    BlockPickup pickup;
    pickup.position = position;
    pickup.item = stack.item;
    pickup.block = stack.block;
    pickup.count = stack.count;
    pickup.rounds_in_clip = stack.rounds_in_clip;
    pickup.durability = stack.durability;
    pickup.waypoint = stack.waypoint;
    pickup.waypoint_name = stack.waypoint_name;
    pickup.waypoint_valid = stack.waypoint_valid;
    pickup.ready_for_pickup = false;
    pickup.display_on_pickup = false;
    pickup.bounce_phase = 1.57079632679F;
    if (!register_spawned_network_pickup(pickup, false))
        return;
    block_pickups_.push_back(std::move(pickup));
    stats_.block_pickup_count = block_pickups_.size();
}

void WorldPreviewRenderer::spawn_loot_stack_at(const InventoryStack &stack, world::Vec3 position,
                                               world::Vec3 velocity)
{
    if (!stack.valid())
        return;
    BlockPickup pickup;
    pickup.position = position;
    pickup.velocity = velocity;
    pickup.item = stack.item;
    pickup.block = stack.block;
    pickup.count = stack.count;
    pickup.rounds_in_clip = stack.rounds_in_clip;
    pickup.durability = stack.durability;
    pickup.waypoint = stack.waypoint;
    pickup.waypoint_name = stack.waypoint_name;
    pickup.waypoint_valid = stack.waypoint_valid;
    pickup.ready_for_pickup = false;
    pickup.display_on_pickup = true;
    pickup.bounce_phase = 1.57079632679F;
    if (!register_spawned_network_pickup(pickup, false))
        return;
    block_pickups_.push_back(std::move(pickup));
    stats_.block_pickup_count = block_pickups_.size();
}

void WorldPreviewRenderer::spawn_dropped_stack(const InventoryStack &stack,
                                               const world::PlayerState &player)
{
    if (!stack.valid())
        return;
    BlockPickup pickup;
    pickup.position = {player.position.x, player.position.y + 1.0F, player.position.z};
    effect_random_state_ ^= effect_random_state_ << 13U;
    effect_random_state_ ^= effect_random_state_ >> 17U;
    effect_random_state_ ^= effect_random_state_ << 5U;
    const float random_zero_to_one =
        static_cast<float>(effect_random_state_ & 0x00FFFFFFU) / static_cast<float>(0x00FFFFFFU);
    const float lateral = -0.12F + random_zero_to_one * 0.25F;
    world::Vec3 direction{std::sin(player.yaw) - std::cos(player.yaw) * lateral, 0.1F,
                          -std::cos(player.yaw) - std::sin(player.yaw) * lateral};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length > 0.0001F)
    {
        direction.x = direction.x / length * 4.0F;
        direction.y = direction.y / length * 4.0F;
        direction.z = direction.z / length * 4.0F;
    }
    pickup.velocity = direction;
    pickup.item = stack.item;
    pickup.block = stack.block;
    pickup.count = stack.count;
    pickup.rounds_in_clip = stack.rounds_in_clip;
    pickup.durability = stack.durability;
    pickup.waypoint = stack.waypoint;
    pickup.waypoint_name = stack.waypoint_name;
    pickup.waypoint_valid = stack.waypoint_valid;
    pickup.ready_for_pickup = false;
    pickup.bounce_phase = 1.57079632679F;
    if (!register_spawned_network_pickup(pickup, true))
        return;
    block_pickups_.push_back(pickup);
    stats_.block_pickup_count = block_pickups_.size();
}

// TorchCloud's per-face placement: offset from the block centre plus a tilt.
// Ground torches stand upright, wall torches lean 45 degrees away from their
// wall, ceiling torches hang flipped.
struct TorchPlacement final
{
    float offset_x{};
    float offset_y{};
    float offset_z{};
    bool tilt_about_x{};
    float tilt{};
};

[[nodiscard]] TorchPlacement torch_placement(world::BlockType variant) noexcept
{
    constexpr float quarter_pi = 0.7853982F;
    constexpr float pi_value = 3.1415927F;
    switch (variant)
    {
    case world::BlockType::torch_pos_x:
        return {0.5F, -0.25F, 0.0F, false, quarter_pi};
    case world::BlockType::torch_neg_x:
        return {-0.5F, -0.25F, 0.0F, false, -quarter_pi};
    case world::BlockType::torch_pos_z:
        return {0.0F, -0.25F, 0.5F, true, -quarter_pi};
    case world::BlockType::torch_neg_z:
        return {0.0F, -0.25F, -0.5F, true, quarter_pi};
    case world::BlockType::torch_neg_y:
        return {0.0F, 0.5F, 0.0F, false, pi_value};
    default:
        return {0.0F, -0.5F, 0.0F, false, 0.0F};
    }
}

// Rotates a local point by the placement tilt (row-vector convention shared
// with the entity pass).
[[nodiscard]] world::Vec3 torch_rotate(const TorchPlacement &placement, float x, float y,
                                       float z) noexcept
{
    const float c = std::cos(placement.tilt);
    const float s = std::sin(placement.tilt);
    if (placement.tilt_about_x)
    {
        return {x, y * c - z * s, y * s + z * c};
    }
    return {x * c - y * s, x * s + y * c, z};
}

void WorldPreviewRenderer::update_torch_props(float delta, world::Vec3 player_position)
{
    // Rescan the neighbourhood for placed torches twice a second; block edits
    // land within that window.
    torch_scan_timer_ -= delta;
    if (torch_scan_timer_ <= 0.0F)
    {
        torch_scan_timer_ = 0.5F;
        std::vector<TorchInstance> fresh;
        const int center_x = static_cast<int>(std::floor(player_position.x));
        const int center_y = static_cast<int>(std::floor(player_position.y));
        const int center_z = static_cast<int>(std::floor(player_position.z));
        for (int y = center_y - 14; y <= center_y + 14; ++y)
            for (int z = center_z - 32; z <= center_z + 32; ++z)
                for (int x = center_x - 32; x <= center_x + 32; ++x)
                {
                    if (!region_.contains(x, y, z))
                        continue;
                    const world::BlockType block = region_.block(x, y, z);
                    if (block < world::BlockType::torch || block > world::BlockType::torch_neg_y)
                        continue;
                    TorchInstance instance;
                    instance.x = x;
                    instance.y = y;
                    instance.z = z;
                    instance.variant = block;
                    // Newly discovered torches ignite with an instant burst;
                    // known ones keep their emission accumulators.
                    instance.fire_accumulator = 3.0F;
                    instance.smoke_accumulator = 1.0F;
                    for (const TorchInstance &previous : torch_cache_)
                        if (previous.x == x && previous.y == y && previous.z == z)
                        {
                            instance.fire_accumulator = previous.fire_accumulator;
                            instance.smoke_accumulator = previous.smoke_accumulator;
                            break;
                        }
                    fresh.push_back(instance);
                }
        torch_cache_ = std::move(fresh);
    }
    // TorchFire: 10/s, 1 s life, rises 0.2-0.5, grows 0.1-0.2 -> 0.5-1.
    // TorchSmoke: 5/s, 2 s life, rises 0.5-1, grows 0.1-0.5 -> 0.5-1.
    const auto random01 = [this]() {
        effect_random_state_ ^= effect_random_state_ << 13U;
        effect_random_state_ ^= effect_random_state_ >> 17U;
        effect_random_state_ ^= effect_random_state_ << 5U;
        return static_cast<float>(effect_random_state_ & 0xFFFFFFU) / 16777215.0F;
    };
    for (TorchInstance &torch : torch_cache_)
    {
        const float dx = static_cast<float>(torch.x) + 0.5F - player_position.x;
        const float dz = static_cast<float>(torch.z) + 0.5F - player_position.z;
        if (dx * dx + dz * dz > 48.0F * 48.0F)
            continue;
        const TorchPlacement placement = torch_placement(torch.variant);
        const world::Vec3 flame_local = torch_rotate(placement, 0.0F, 0.55F, 0.0F);
        const world::Vec3 flame{
            static_cast<float>(torch.x) + 0.5F + placement.offset_x + flame_local.x,
            static_cast<float>(torch.y) + 0.5F + placement.offset_y + flame_local.y,
            static_cast<float>(torch.z) + 0.5F + placement.offset_z + flame_local.z};
        torch.fire_accumulator += 10.0F * delta;
        while (torch.fire_accumulator >= 1.0F)
        {
            torch.fire_accumulator -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::torch_fire;
            particle.position = flame;
            const float angle = random01() * 6.2831853F;
            const float horizontal = random01() * 0.1F;
            particle.velocity = {std::cos(angle) * horizontal, 0.2F + random01() * 0.3F,
                                 std::sin(angle) * horizontal};
            particle.lifetime = 1.0F;
            particle.size = 0.1F + random01() * 0.1F;
            particle.end_size = 0.5F + random01() * 0.5F;
            particle.rotation = random01() * 6.2831853F;
            particle.angular_velocity = random01() * 2.0F - 1.0F;
            // Burst spawns (fresh torches) stagger ages so the flame appears
            // already burning instead of growing from nothing.
            particle.age = std::min(0.85F, torch.fire_accumulator * 0.3F);
            mining_particles_.push_back(particle);
        }
        torch.smoke_accumulator += 5.0F * delta;
        while (torch.smoke_accumulator >= 1.0F)
        {
            torch.smoke_accumulator -= 1.0F;
            MiningParticle particle;
            particle.kind = ParticleKind::torch_smoke;
            particle.position = flame;
            const float angle = random01() * 6.2831853F;
            const float horizontal = random01() * 0.1F;
            particle.velocity = {std::cos(angle) * horizontal, 0.5F + random01() * 0.5F,
                                 std::sin(angle) * horizontal};
            particle.lifetime = 2.0F;
            particle.size = 0.1F + random01() * 0.4F;
            particle.end_size = 0.5F + random01() * 0.5F;
            mining_particles_.push_back(particle);
        }
    }
}

std::vector<CollectedBlockStack> WorldPreviewRenderer::update_effects(float delta_seconds,
                                                                      world::Vec3 player_position)
{
    const float delta = std::clamp(delta_seconds, 0.0F, 0.1F);
    for (auto &particle : mining_particles_)
    {
        particle.age += delta;
        particle.position.x += particle.velocity.x * delta;
        particle.position.y += particle.velocity.y * delta;
        particle.position.z += particle.velocity.z * delta;
        particle.rotation += particle.angular_velocity * delta;
        if (particle.kind == ParticleKind::rock)
            particle.velocity.y -= 10.0F * delta;
        else if (particle.kind == ParticleKind::spark)
            particle.velocity.y -= 9.0F * delta;
        else if (particle.kind == ParticleKind::blast_rock)
        {
            // BigRockBlast: gravity -10 with VelocityEnd 0.75 drag.
            particle.velocity.y -= 10.0F * delta;
            particle.velocity.x *= std::pow(0.75F, delta);
            particle.velocity.z *= std::pow(0.75F, delta);
        }
        else if (particle.kind == ParticleKind::flash || particle.kind == ParticleKind::ice_flash ||
                 particle.kind == ParticleKind::fire_soft ||
                 particle.kind == ParticleKind::ice_soft)
        {
            // Flash/fire recipes keep their launch velocity (VelocityEnd 0).
        }
        else
        {
            particle.velocity.x *= std::pow(0.3F, delta);
            particle.velocity.z *= std::pow(0.3F, delta);
        }
    }
    std::erase_if(mining_particles_,
                  [](const MiningParticle &particle) { return particle.age >= particle.lifetime; });
    update_effect_emitters(delta);
    emit_fireball_trails(delta);
    emit_rocket_and_fuse_trails(delta);
    update_torch_props(delta, player_position);

    std::vector<CollectedBlockStack> collected;
    const auto pickup_collides = [&](world::Vec3 center) {
        constexpr float radius = 0.3F;
        constexpr float inset = 0.0001F;
        const int minimum_x = static_cast<int>(std::floor(center.x - radius + inset));
        const int maximum_x = static_cast<int>(std::floor(center.x + radius - inset));
        const int minimum_y = static_cast<int>(std::floor(center.y - radius + inset));
        const int maximum_y = static_cast<int>(std::floor(center.y + radius - inset));
        const int minimum_z = static_cast<int>(std::floor(center.z - radius + inset));
        const int maximum_z = static_cast<int>(std::floor(center.z + radius - inset));
        for (int z = minimum_z; z <= maximum_z; ++z)
            for (int y = minimum_y; y <= maximum_y; ++y)
                for (int x = minimum_x; x <= maximum_x; ++x)
                    if (world::blocks_player(region_.block(x, y, z)))
                        return true;
        return false;
    };
    const auto pickup_axis = [](world::Vec3 &point, int axis) -> float & {
        if (axis == 0)
            return point.x;
        if (axis == 1)
            return point.y;
        return point.z;
    };
    const auto move_pickup_axis = [&](world::Vec3 &position, float amount, int axis) {
        if (std::abs(amount) <= 0.000001F)
            return false;
        world::Vec3 candidate = position;
        pickup_axis(candidate, axis) += amount;
        if (!pickup_collides(candidate))
        {
            position = candidate;
            return false;
        }
        // BasicPhysics performs a swept AABB trace and slides along the collision plane. A short
        // binary contact solve is equivalent at the fixed 60 Hz pickup speeds and avoids wall
        // tunnelling without coupling pickup motion to the player's much taller collider.
        float clear_fraction{};
        float blocked_fraction = 1.0F;
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            const float middle = (clear_fraction + blocked_fraction) * 0.5F;
            candidate = position;
            pickup_axis(candidate, axis) += amount * middle;
            if (pickup_collides(candidate))
                blocked_fraction = middle;
            else
                clear_fraction = middle;
        }
        pickup_axis(position, axis) += amount * clear_fraction;
        return true;
    };
    for (auto &pickup : block_pickups_)
    {
        pickup.age += delta;
        pickup.rotation += delta;
        if (pickup.flying_to_player)
        {
            const world::Vec3 to_player{player_position.x - pickup.position.x,
                                        player_position.y - pickup.position.y,
                                        player_position.z - pickup.position.z};
            const float distance_squared =
                to_player.x * to_player.x + to_player.y * to_player.y + to_player.z * to_player.z;
            if (distance_squared < 0.5F)
            {
                pickup.time_left = 0.0F;
                continue;
            }
            pickup.fly_speed = std::min(100.0F, pickup.fly_speed + 20.0F * delta);
            const float distance = std::sqrt(distance_squared);
            const float travel = pickup.fly_speed * delta;
            if (travel * travel > distance_squared)
            {
                pickup.time_left = 0.0F;
                continue;
            }
            pickup.position.x += to_player.x / distance * travel;
            pickup.position.y += to_player.y / distance * travel;
            pickup.position.z += to_player.z / distance * travel;
            continue;
        }
        pickup.time_left -= delta;
        pickup.bounce_phase += delta * 1.5F;
        pickup.velocity.y += world::PlayerSimulation::gravity * 0.25F * delta;
        world::Vec3 next = pickup.position;
        const bool hit_x = move_pickup_axis(next, pickup.velocity.x * delta, 0);
        const bool hit_y = move_pickup_axis(next, pickup.velocity.y * delta, 1);
        const bool hit_z = move_pickup_axis(next, pickup.velocity.z * delta, 2);
        const bool on_ground = hit_y && pickup.velocity.y < 0.0F;
        if (hit_x)
            pickup.velocity.x = 0.0F;
        if (hit_y)
            pickup.velocity.y = 0.0F;
        if (hit_z)
            pickup.velocity.z = 0.0F;
        pickup.velocity.x *= on_ground ? 0.9F : 0.99F;
        pickup.velocity.z *= on_ground ? 0.9F : 0.99F;
        if (std::abs(pickup.velocity.x) < 0.1F)
            pickup.velocity.x = 0.0F;
        if (std::abs(pickup.velocity.z) < 0.1F)
            pickup.velocity.z = 0.0F;
        pickup.position = next;
        const float dx = pickup.position.x - player_position.x;
        const float dy = pickup.position.y - (player_position.y + 1.0F);
        const float dz = pickup.position.z - player_position.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (!pickup.ready_for_pickup && distance_squared > 4.0F)
            pickup.ready_for_pickup = true;
        if (pickup.ready_for_pickup && !pickup.request_pending && distance_squared < 4.0F)
        {
            InventoryStack stack{pickup.item, pickup.count};
            stack.rounds_in_clip = pickup.rounds_in_clip;
            stack.durability = pickup.durability;
            stack.waypoint = pickup.waypoint;
            stack.waypoint_name = pickup.waypoint_name;
            stack.waypoint_valid = pickup.waypoint_valid;
            collected.push_back({stack, pickup.position, pickup.pickup_id, pickup.spawner_id,
                                 pickup.display_on_pickup});
            if (pickup.pickup_id.has_value())
            {
                pickup.request_pending = true;
                continue;
            }
            pickup.position.y += std::sin(pickup.bounce_phase) * 0.1F + 0.1F;
            pickup.velocity = {};
            pickup.flying_to_player = true;
            pickup.fly_speed = 0.0F;
        }
    }
    std::erase_if(block_pickups_,
                  [](const BlockPickup &pickup) { return pickup.time_left <= 0.0F; });
    stats_.mining_particle_count = mining_particles_.size();
    stats_.block_pickup_count = block_pickups_.size();
    return collected;
}

void WorldPreviewRenderer::cycle_biome(int direction)
{
    constexpr std::array biomes{world::BiomeKind::classic, world::BiomeKind::lagoon,
                                world::BiomeKind::desert,  world::BiomeKind::mountain,
                                world::BiomeKind::arctic,  world::BiomeKind::descent};
    auto iterator = std::find(biomes.begin(), biomes.end(), biome_);
    int index = iterator == biomes.end() ? 0 : static_cast<int>(iterator - biomes.begin());
    index = (index + (direction < 0 ? -1 : 1) + static_cast<int>(biomes.size())) %
            static_cast<int>(biomes.size());
    set_biome(index);
}

// Install a small playable window immediately, then generate the full active
// window on the worker. The old path stopped after the four-by-four rebuild,
// leaving preload_complete() waiting forever for active_span_^2 chunks.
void WorldPreviewRenderer::set_biome(int index)
{
    const world::BiomeKind selected = preview_biome(index);
    discard_pending_stream();
    auto immediate = generator_.generate_region(preview_biome_origin(selected, 4), -2, 4, 4);
    install_region(std::move(immediate), selected);

    world::WorldRegion current = region_;
    const world::WorldGenerator generator = generator_;
    const int target_span = active_span_;
    const int target_origin_x = preview_biome_origin(selected, target_span);
    stream_job_ = std::async(std::launch::async, [generator, current = std::move(current),
                                                  target_origin_x, target_span]() mutable {
        return generator.stream_region(current, target_origin_x, -target_span / 2, target_span,
                                       target_span);
    });
    stream_job_active_ = true;
    pending_biome_ = selected;
    stats_.stream_pending = true;
}

std::string_view WorldPreviewRenderer::biome_name() const noexcept
{
    switch (biome_)
    {
    case world::BiomeKind::classic:
        return "CLASSIC";
    case world::BiomeKind::lagoon:
        return "LAGOON";
    case world::BiomeKind::desert:
        return "DESERT";
    case world::BiomeKind::mountain:
        return "MOUNTAIN";
    case world::BiomeKind::arctic:
        return "ARCTIC";
    case world::BiomeKind::descent:
        return "DESCENT";
    }
    return "UNKNOWN";
}

world::BiomeKind WorldPreviewRenderer::biome() const noexcept
{
    return biome_;
}

int WorldPreviewRenderer::center_x() const noexcept
{
    return (region_.minimum_x() + region_.maximum_x() + 1) / 2;
}

int WorldPreviewRenderer::center_z() const noexcept
{
    return (region_.minimum_z() + region_.maximum_z() + 1) / 2;
}

void WorldPreviewRenderer::load_original_atlas()
{
    stats_.atlas_path = derived_atlas_path();
    SDL_Surface *surface = SDL_LoadBMP(stats_.atlas_path.string().c_str());
    if (surface == nullptr)
    {
        return;
    }
    atlas_ = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (atlas_ != nullptr)
    {
        SDL_SetTextureScaleMode(atlas_, SDL_SCALEMODE_NEAREST);
        stats_.original_atlas_loaded = true;
    }

    const auto load_material_atlas = [this](std::string_view name) {
        const auto path = stats_.atlas_path.parent_path() / std::string{name};
        SDL_Surface *material_surface = SDL_LoadBMP(path.string().c_str());
        if (material_surface == nullptr)
        {
            return static_cast<SDL_Texture *>(nullptr);
        }
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, material_surface);
        SDL_DestroySurface(material_surface);
        if (texture != nullptr)
        {
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    };
    normal_spec_atlas_ = load_material_atlas("terrain_normal_spec.bmp");
    metal_light_atlas_ = load_material_atlas("terrain_metal_light.bmp");
    block_cracks_ = load_material_atlas("block_cracks.bmp");
    if (block_cracks_ != nullptr)
    {
        SDL_SetTextureScaleMode(block_cracks_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(block_cracks_, SDL_BLENDMODE_MUL);
    }
    block_selector_ = load_material_atlas("selector_0.bmp");
    if (block_selector_ != nullptr)
    {
        SDL_SetTextureScaleMode(block_selector_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(block_selector_, SDL_BLENDMODE_BLEND);
    }
    mining_smoke_ = load_material_atlas("mining_smoke.bmp");
    if (mining_smoke_ != nullptr)
    {
        SDL_SetTextureScaleMode(mining_smoke_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(mining_smoke_, SDL_BLENDMODE_BLEND);
    }
    mining_sparks_ = load_material_atlas("mining_sparks.bmp");
    if (mining_sparks_ != nullptr)
    {
        SDL_SetTextureScaleMode(mining_sparks_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(mining_sparks_, SDL_BLENDMODE_ADD);
    }
    mining_rocks_ = load_material_atlas("mining_rocks.bmp");
    if (mining_rocks_ != nullptr)
    {
        SDL_SetTextureScaleMode(mining_rocks_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(mining_rocks_, SDL_BLENDMODE_BLEND);
    }
    // TorchCloud's Fire/Smoke sprites: additive fire sells the glow, alpha
    // smoke drifts above it.
    torch_fire_texture_ = load_material_atlas("torch_fire.bmp");
    if (torch_fire_texture_ != nullptr)
    {
        SDL_SetTextureScaleMode(torch_fire_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(torch_fire_texture_, SDL_BLENDMODE_ADD);
    }
    flash_texture_ = load_material_atlas("flash_effect.bmp");
    if (flash_texture_ != nullptr)
    {
        SDL_SetTextureScaleMode(flash_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(flash_texture_, SDL_BLENDMODE_ADD);
    }
    ice_flash_texture_ = load_material_atlas("ice_flash.bmp");
    if (ice_flash_texture_ != nullptr)
    {
        SDL_SetTextureScaleMode(ice_flash_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(ice_flash_texture_, SDL_BLENDMODE_ADD);
    }
    ice_puff_texture_ = load_material_atlas("ice_puff.bmp");
    if (ice_puff_texture_ != nullptr)
    {
        SDL_SetTextureScaleMode(ice_puff_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(ice_puff_texture_, SDL_BLENDMODE_BLEND);
    }
    torch_smoke_texture_ = load_material_atlas("torch_smoke.bmp");
    if (torch_smoke_texture_ != nullptr)
    {
        SDL_SetTextureScaleMode(torch_smoke_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(torch_smoke_texture_, SDL_BLENDMODE_BLEND);
    }

    const auto load_sky = [this](std::string_view name) {
        const auto path = stats_.atlas_path.parent_path() / std::string{name};
        SDL_Surface *sky_surface = SDL_LoadBMP(path.string().c_str());
        if (sky_surface == nullptr)
        {
            return static_cast<SDL_Texture *>(nullptr);
        }
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, sky_surface);
        SDL_DestroySurface(sky_surface);
        if (texture != nullptr)
        {
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        }
        return texture;
    };
    constexpr std::array face_names{"negativez", "positivex", "positivez",
                                    "negativex", "positivey", "negativey"};
    const auto load_cube = [&](std::string_view period, SkyCube &cube) {
        for (std::size_t index = 0; index < cube.size(); ++index)
        {
            cube[index] = load_sky(std::format("sky_{}_{}.bmp", period, face_names[index]));
        }
    };
    load_cube("clear", clear_sky_);
    load_cube("dawn", dawn_sky_);
    load_cube("sunset", sunset_sky_);
    load_cube("night", night_sky_);
    const auto complete = [](const SkyCube &cube) {
        return std::all_of(cube.begin(), cube.end(),
                           [](SDL_Texture *texture) { return texture != nullptr; });
    };
    stats_.original_skies_loaded = complete(clear_sky_) && complete(dawn_sky_) &&
                                   complete(sunset_sky_) && complete(night_sky_);
}

void WorldPreviewRenderer::draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
                                const world::PlayerState &player, float vertical_fov_degrees,
                                float elapsed_seconds, float render_scale, float camera_bob,
                                float screen_shake, float viewmodel_fov_degrees,
                                bool rebirth_presentation, bool reduced_motion, bool inspector_view,
                                bool ambient_occlusion, float shadow_strength)
{
    world::PlayerState render_player = player;
    render_player.clock_time_of_day = sky.time_of_day;
    render_player.position.y += player.step_view_offset;
    const float horizontal_speed =
        std::sqrt(player.velocity.x * player.velocity.x + player.velocity.z * player.velocity.z);
    const float motion_amount =
        !inspector_view && !reduced_motion && player.grounded
            ? std::clamp(horizontal_speed / world::PlayerSimulation::movement_speed, 0.0F, 1.0F) *
                  std::clamp(camera_bob, 0.0F, 1.0F)
            : 0.0F;
    if (motion_amount > 0.0F)
    {
        const float phase = elapsed_seconds * 9.6F;
        const float lateral = std::cos(phase) * 0.018F * motion_amount;
        render_player.position.x += std::cos(player.yaw) * lateral;
        render_player.position.z += std::sin(player.yaw) * lateral;
        render_player.position.y += std::sin(phase * 2.0F) * 0.025F * motion_amount;
        render_player.pitch += std::sin(phase * 2.0F) * 0.0025F * motion_amount;
    }
    if (!inspector_view && !reduced_motion && player.using_tool && screen_shake > 0.0F)
    {
        const float progress = std::clamp(player.tool_swing_progress, 0.0F, 1.0F);
        const float envelope = std::sin(progress * std::numbers::pi_v<float>);
        const float strength = std::clamp(screen_shake, 0.0F, 1.0F) * envelope;
        const bool pistol = player.selected_item_id == 51U;
        render_player.pitch -= strength * (pistol ? 0.014F : 0.0035F);
        render_player.yaw += std::sin(progress * std::numbers::pi_v<float> * 2.0F) * strength *
                             (pistol ? 0.0045F : 0.0015F);
    }
    draw_sky(
        metrics, sky, elapsed_seconds, reduced_motion, inspector_view ? 0.0F : render_player.yaw,
        inspector_view ? -0.08F : render_player.pitch, vertical_fov_degrees, rebirth_presentation);
    if (inspector_view)
    {
        draw_inspector(metrics, sky, ambient_occlusion, shadow_strength);
    }
    else
    {
        const bool expected_depth_renderer = stats_.depth_buffered;
        const bool water_enabled = rebirth_presentation && biome_ == world::BiomeKind::lagoon;
        // A zero-delta pass only refreshes the torch cache when its timer has
        // lapsed — keeps props correct even when the simulation is paused.
        update_torch_props(0.0F, render_player.position);
        if (depth_renderer_ != nullptr && enemy_models_ != nullptr &&
            ((visible_enemies_ != nullptr && !visible_enemies_->empty()) ||
             (visible_remote_players_ != nullptr && !visible_remote_players_->empty()) ||
             visible_dragon_ != nullptr || !projectile_visuals_.empty() ||
             !rocket_visuals_.empty() || !explosive_fuse_visuals_.empty() ||
             !fireball_visuals_.empty() || !torch_cache_.empty() ||
             (visible_doors_ != nullptr && !visible_doors_->empty()) ||
             !mining_particles_.empty() || !block_pickups_.empty()))
        {
            // CPU-skin the live enemies into the depth pass. Light factors refresh a
            // few enemies per frame; the cache keeps the rest stable between samples.
            EntityBatch batch;
            const std::array<float, 3> sun_color{
                std::min(1.0F, (sky.ambient.red * 0.35F + sky.direct.red * 0.65F) * 1.2F),
                std::min(1.0F, (sky.ambient.green * 0.35F + sky.direct.green * 0.65F) * 1.2F),
                std::min(1.0F, (sky.ambient.blue * 0.35F + sky.direct.blue * 0.65F) * 1.2F)};
            if (visible_dragon_ != nullptr)
                enemy_models_->append_dragon(batch, *visible_dragon_, sun_color);
            std::size_t refresh_budget = 4U;
            const std::size_t enemy_count =
                visible_enemies_ != nullptr ? visible_enemies_->size() : 0U;
            for (std::size_t index = 0; index < enemy_count; ++index)
            {
                const world::Enemy &enemy = (*visible_enemies_)[index];
                auto &sample = enemy_light_cache_[enemy.id];
                const bool wants_refresh =
                    (index + enemy_light_cursor_) %
                        std::max<std::size_t>(1U, visible_enemies_->size() /
                                                      std::max<std::size_t>(refresh_budget, 1U)) ==
                    0U;
                if (wants_refresh && refresh_budget > 0U)
                {
                    --refresh_budget;
                    const int cell_x = static_cast<int>(std::floor(enemy.position.x));
                    const int cell_z = static_cast<int>(std::floor(enemy.position.z));
                    const int head_y = static_cast<int>(std::floor(enemy.position.y + 1.5F));
                    bool sky_clear = true;
                    for (int y = head_y + 1; y < world::world_min_y + world::chunk_height; ++y)
                        if (region_.contains(cell_x, y, cell_z) &&
                            world::occludes(region_.block(cell_x, y, cell_z)))
                        {
                            sky_clear = false;
                            break;
                        }
                    sample.sun = sky_clear ? 1.0F : 0.2F;
                    sample.torch = local_emissive_light(
                        region_, {enemy.position.x, enemy.position.y + 1.0F, enemy.position.z});
                }
                enemy_models_->append_enemy(batch, enemy, sample.sun, sample.torch, sun_color,
                                            sky.daylight);
            }
            ++enemy_light_cursor_;
            if (visible_remote_players_ != nullptr && !visible_remote_players_->empty())
            {
                if (avatar_viewmodel_ != nullptr && avatar_viewmodel_->world_animation_ready())
                {
                    for (const RemotePlayerVisual &player_visual : *visible_remote_players_)
                    {
                        const float sun = local_actor_sky_light(
                            region_, {player_visual.position.x,
                                      player_visual.position.y + 1.2F,
                                      player_visual.position.z});
                        const float torch = local_emissive_light(
                            region_, {player_visual.position.x,
                                      player_visual.position.y + 1.0F,
                                      player_visual.position.z});
                        avatar_viewmodel_->append_world_player(
                            batch, player_visual.gid, player_visual.carried_item,
                            player_visual.position, player_visual.yaw,
                            player_visual.torso_pitch, player_visual.animation_seconds,
                            player_visual.movement_x, player_visual.movement_y,
                            player_visual.animation_mode, player_visual.using_tool,
                            player_visual.dead, player_visual.shouldering,
                            player_visual.reloading, player_visual.throwing_grenade,
                            player_visual.ready_to_throw_grenade,
                            sun, torch, atlas_);
                    }
                }
                else if (white_texture_ != nullptr)
                {
                EntitySpan player_span;
                player_span.texture = white_texture_;
                player_span.first = static_cast<std::uint32_t>(batch.vertices.size());
                const auto append_box = [&](world::Vec3 center, world::Vec3 half, float yaw,
                                            const std::array<float, 3> &color) {
                    const float cosine = std::cos(yaw);
                    const float sine = std::sin(yaw);
                    const auto point = [&](float x, float y, float z) {
                        return world::Vec3{center.x + x * cosine + z * sine, center.y + y,
                                           center.z - x * sine + z * cosine};
                    };
                    const std::array<world::Vec3, 8> corners{
                        point(-half.x, -half.y, -half.z), point(-half.x, half.y, -half.z),
                        point(half.x, half.y, -half.z),   point(half.x, -half.y, -half.z),
                        point(-half.x, -half.y, half.z),  point(-half.x, half.y, half.z),
                        point(half.x, half.y, half.z),    point(half.x, -half.y, half.z)};
                    struct BoxFace final
                    {
                        std::array<std::uint8_t, 6> indices;
                        world::Vec3 normal;
                        float shade{};
                    };
                    static constexpr std::array<BoxFace, 6> faces{{
                        {{{3, 2, 6, 3, 6, 7}}, {1.0F, 0.0F, 0.0F}, 0.88F},
                        {{{4, 5, 1, 4, 1, 0}}, {-1.0F, 0.0F, 0.0F}, 0.76F},
                        {{{1, 5, 6, 1, 6, 2}}, {0.0F, 1.0F, 0.0F}, 1.0F},
                        {{{4, 0, 3, 4, 3, 7}}, {0.0F, -1.0F, 0.0F}, 0.62F},
                        {{{7, 6, 5, 7, 5, 4}}, {0.0F, 0.0F, 1.0F}, 0.84F},
                        {{{0, 1, 2, 0, 2, 3}}, {0.0F, 0.0F, -1.0F}, 0.72F},
                    }};
                    for (const BoxFace &face : faces)
                    {
                        const float normal_x = face.normal.x * cosine + face.normal.z * sine;
                        const float normal_z = -face.normal.x * sine + face.normal.z * cosine;
                        for (const std::uint8_t index : face.indices)
                        {
                            const world::Vec3 &corner = corners[index];
                            EntityVertex vertex;
                            vertex.x = corner.x;
                            vertex.y = corner.y;
                            vertex.z = corner.z;
                            vertex.u = 0.5F;
                            vertex.v = 0.5F;
                            vertex.red = color[0] * face.shade;
                            vertex.green = color[1] * face.shade;
                            vertex.blue = color[2] * face.shade;
                            vertex.normal_x = normal_x;
                            vertex.normal_y = face.normal.y;
                            vertex.normal_z = normal_z;
                            vertex.sky_light = 1.0F;
                            batch.vertices.push_back(vertex);
                        }
                    }
                };
                static constexpr std::array<std::array<float, 3>, 6> player_colors{{
                    {0.22F, 0.68F, 0.94F},
                    {0.94F, 0.43F, 0.31F},
                    {0.39F, 0.82F, 0.48F},
                    {0.83F, 0.58F, 0.94F},
                    {0.95F, 0.77F, 0.28F},
                    {0.28F, 0.83F, 0.78F},
                }};
                for (const RemotePlayerVisual &player_visual : *visible_remote_players_)
                {
                    const auto &color = player_colors[player_visual.gid % player_colors.size()];
                    const float lowered = player_visual.dead ? -0.75F : 0.0F;
                    const world::Vec3 feet{player_visual.position.x,
                                           player_visual.position.y + lowered,
                                           player_visual.position.z};
                    const float movement =
                        std::clamp(std::max({std::abs(player_visual.movement_x),
                                             std::abs(player_visual.movement_y),
                                             std::hypot(player_visual.velocity.x,
                                                        player_visual.velocity.z) /
                                                 6.0F}),
                                   0.0F, 1.0F);
                    const float gait_speed = movement >= 0.8F ? 11.0F : 7.0F;
                    const float gait = player_visual.dead
                                           ? 0.0F
                                           : std::sin(player_visual.animation_seconds * gait_speed) *
                                                 movement;
                    const float stride = gait * 0.18F;
                    const float lift = std::abs(gait) * 0.055F;
                    const float right_x = std::cos(player_visual.yaw);
                    const float right_z = -std::sin(player_visual.yaw);
                    const float forward_x = std::sin(player_visual.yaw);
                    const float forward_z = std::cos(player_visual.yaw);
                    append_box({feet.x - right_x * 0.12F + forward_x * stride,
                                feet.y + 0.34F + (gait > 0.0F ? lift : 0.0F),
                                feet.z - right_z * 0.12F + forward_z * stride},
                               {0.12F, 0.34F, 0.13F}, player_visual.yaw,
                               {color[0] * 0.55F, color[1] * 0.55F, color[2] * 0.55F});
                    append_box({feet.x + right_x * 0.12F - forward_x * stride,
                                feet.y + 0.34F + (gait < 0.0F ? lift : 0.0F),
                                feet.z + right_z * 0.12F - forward_z * stride},
                               {0.12F, 0.34F, 0.13F}, player_visual.yaw,
                               {color[0] * 0.62F, color[1] * 0.62F, color[2] * 0.62F});
                    append_box({feet.x, feet.y + 1.02F, feet.z}, {0.31F, 0.36F, 0.18F},
                               player_visual.yaw, color);
                    const float tool_raise = player_visual.using_tool ? 0.20F : 0.0F;
                    append_box({feet.x - right_x * 0.41F - forward_x * stride,
                                feet.y + 1.03F + tool_raise,
                                feet.z - right_z * 0.41F - forward_z * stride},
                               {0.10F, 0.33F, 0.11F}, player_visual.yaw,
                               {color[0] * 0.78F, color[1] * 0.78F, color[2] * 0.78F});
                    append_box({feet.x + right_x * 0.41F + forward_x * stride,
                                feet.y + 1.03F,
                                feet.z + right_z * 0.41F + forward_z * stride},
                               {0.10F, 0.33F, 0.11F}, player_visual.yaw,
                               {color[0] * 0.82F, color[1] * 0.82F, color[2] * 0.82F});
                    append_box({feet.x, feet.y + 1.55F, feet.z}, {0.22F, 0.22F, 0.22F},
                               player_visual.yaw, {0.88F, 0.68F, 0.54F});
                }
                player_span.count =
                    static_cast<std::uint32_t>(batch.vertices.size()) - player_span.first;
                if (player_span.count > 0U)
                    batch.spans.push_back(player_span);
                }
            }
            // Projectile streaks: the original tracer bipyramid — a sharp head
            // point, a three-vertex ring a tenth of the way back, and a tail
            // point that fades dark.
            if (!projectile_visuals_.empty() && white_texture_ != nullptr)
            {
                EntitySpan span;
                span.texture = white_texture_;
                span.first = static_cast<std::uint32_t>(batch.vertices.size());
                for (const ProjectileVisual &projectile : projectile_visuals_)
                {
                    const float dx = projectile.head.x - projectile.tail.x;
                    const float dy = projectile.head.y - projectile.tail.y;
                    const float dz = projectile.head.z - projectile.tail.z;
                    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (length < 0.0005F)
                        continue;
                    const std::array<float, 3> axis{dx / length, dy / length, dz / length};
                    const std::array<float, 3> helper =
                        std::abs(axis[1]) < 0.9F ? std::array<float, 3>{0.0F, 1.0F, 0.0F}
                                                 : std::array<float, 3>{1.0F, 0.0F, 0.0F};
                    std::array<float, 3> side1{axis[1] * helper[2] - axis[2] * helper[1],
                                               axis[2] * helper[0] - axis[0] * helper[2],
                                               axis[0] * helper[1] - axis[1] * helper[0]};
                    const float side1_length =
                        std::sqrt(side1[0] * side1[0] + side1[1] * side1[1] + side1[2] * side1[2]);
                    for (float &component : side1)
                        component /= std::max(side1_length, 0.0001F);
                    const std::array<float, 3> side2{axis[1] * side1[2] - axis[2] * side1[1],
                                                     axis[2] * side1[0] - axis[0] * side1[2],
                                                     axis[0] * side1[1] - axis[1] * side1[0]};
                    const float ring_x = projectile.head.x - dx * 0.1F;
                    const float ring_y = projectile.head.y - dy * 0.1F;
                    const float ring_z = projectile.head.z - dz * 0.1F;
                    const float radius = projectile.width;
                    const std::array<std::array<float, 3>, 5> points{{
                        {projectile.head.x, projectile.head.y, projectile.head.z},
                        {ring_x + side1[0] * radius, ring_y + side1[1] * radius,
                         ring_z + side1[2] * radius},
                        {ring_x - side1[0] * radius * 0.5F - side2[0] * radius * 0.866F,
                         ring_y - side1[1] * radius * 0.5F - side2[1] * radius * 0.866F,
                         ring_z - side1[2] * radius * 0.5F - side2[2] * radius * 0.866F},
                        {ring_x - side1[0] * radius * 0.5F + side2[0] * radius * 0.866F,
                         ring_y - side1[1] * radius * 0.5F + side2[1] * radius * 0.866F,
                         ring_z - side1[2] * radius * 0.5F + side2[2] * radius * 0.866F},
                        {projectile.tail.x, projectile.tail.y, projectile.tail.z},
                    }};
                    // Original TracerManager index pattern.
                    static constexpr std::array<int, 18> spear_indices{1, 0, 2, 3, 0, 1, 2, 0, 3,
                                                                       1, 4, 3, 2, 4, 1, 3, 4, 2};
                    for (const int point : spear_indices)
                    {
                        const auto &position = points[static_cast<std::size_t>(point)];
                        const float shade = point == 4 ? 0.25F : 1.0F;
                        batch.vertices.push_back({position[0], position[1], position[2], 0.5F, 0.5F,
                                                  projectile.color[0] * shade,
                                                  projectile.color[1] * shade,
                                                  projectile.color[2] * shade});
                    }
                }
                span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                if (span.count > 0U)
                    batch.spans.push_back(span);
            }
            // RocketEntity uses the authored RPGGrenade model, aligned to its
            // flight direction and rolled at 0.5 * speed radians per second.
            if (!rocket_visuals_.empty() && rocket_model_ != nullptr && rocket_model_->loaded())
            {
                for (const RocketVisual &visual : rocket_visuals_)
                {
                    world::Vec3 forward = visual.direction;
                    const float forward_length = std::sqrt(
                        forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
                    if (forward_length < 0.0001F)
                        continue;
                    forward = {forward.x / forward_length, forward.y / forward_length,
                               forward.z / forward_length};
                    const world::Vec3 helper = std::abs(forward.y) < 0.95F
                                                   ? world::Vec3{0.0F, 1.0F, 0.0F}
                                                   : world::Vec3{1.0F, 0.0F, 0.0F};
                    world::Vec3 right{helper.y * forward.z - helper.z * forward.y,
                                      helper.z * forward.x - helper.x * forward.z,
                                      helper.x * forward.y - helper.y * forward.x};
                    const float right_length =
                        std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
                    right = {right.x / right_length, right.y / right_length,
                             right.z / right_length};
                    const world::Vec3 up{forward.y * right.z - forward.z * right.y,
                                         forward.z * right.x - forward.x * right.z,
                                         forward.x * right.y - forward.y * right.x};
                    const float spin_cos = std::cos(visual.spin);
                    const float spin_sin = std::sin(visual.spin);
                    for (const StaticModelPart &part : rocket_model_->parts())
                    {
                        EntitySpan span;
                        span.texture = part.texture;
                        span.first = static_cast<std::uint32_t>(batch.vertices.size());
                        for (const std::uint32_t index : part.indices)
                        {
                            const StaticModelVertex &source = part.vertices[index];
                            const auto &m = part.transform;
                            const std::array<float, 3> baked{
                                source.position[0] * m[0] + source.position[1] * m[4] +
                                    source.position[2] * m[8] + m[12],
                                source.position[0] * m[1] + source.position[1] * m[5] +
                                    source.position[2] * m[9] + m[13],
                                source.position[0] * m[2] + source.position[1] * m[6] +
                                    source.position[2] * m[10] + m[14]};
                            // RPGGrenade is authored lengthwise on X. The
                            // child's -90-degree yaw maps that axis onto the
                            // parent's -Z (forward) axis before the parent
                            // CreateWorld transform aligns it to flight.
                            const float local_x = -baked[0];
                            const float unspun_y = baked[1];
                            const float unspun_z = -baked[2];
                            const float local_y = unspun_y * spin_cos - unspun_z * spin_sin;
                            const float local_z = unspun_y * spin_sin + unspun_z * spin_cos;
                            EntityVertex vertex;
                            vertex.x = visual.position.x + forward.x * local_x + up.x * local_y +
                                       right.x * local_z;
                            vertex.y = visual.position.y + forward.y * local_x + up.y * local_y +
                                       right.y * local_z;
                            vertex.z = visual.position.z + forward.z * local_x + up.z * local_y +
                                       right.z * local_z;
                            vertex.u = source.uv[0];
                            vertex.v = source.uv[1];
                            vertex.red = source.color[0] * part.diffuse[0] * 0.85F;
                            vertex.green = source.color[1] * part.diffuse[1] * 0.85F;
                            vertex.blue = source.color[2] * part.diffuse[2] * 0.85F;
                            vertex.alpha = source.color[3];
                            batch.vertices.push_back(vertex);
                        }
                        span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                        if (span.count > 0U)
                            batch.spans.push_back(span);
                    }
                }
            }
            // ExplosiveFlashEntity: a depth-read additive half-alpha red box
            // pulsing every 0.25 s, twice as fast during the final second.
            if (!explosive_fuse_visuals_.empty() && white_texture_ != nullptr)
            {
                static constexpr std::array<int, 36> cube_indices{
                    0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                    3, 7, 6, 3, 6, 2, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3};
                for (const ExplosiveFuseVisual &fuse : explosive_fuse_visuals_)
                {
                    if (!fuse.flash)
                        continue;
                    const float x0 = static_cast<float>(fuse.x) - 0.008F;
                    const float y0 = static_cast<float>(fuse.y) - 0.008F;
                    const float z0 = static_cast<float>(fuse.z) - 0.008F;
                    const float x1 = static_cast<float>(fuse.x + 1) + 0.008F;
                    const float y1 = static_cast<float>(fuse.y + 1) + 0.008F;
                    const float z1 = static_cast<float>(fuse.z + 1) + 0.008F;
                    const std::array<world::Vec3, 8> corners{
                        world::Vec3{x0, y0, z0}, world::Vec3{x1, y0, z0}, world::Vec3{x1, y1, z0},
                        world::Vec3{x0, y1, z0}, world::Vec3{x0, y0, z1}, world::Vec3{x1, y0, z1},
                        world::Vec3{x1, y1, z1}, world::Vec3{x0, y1, z1}};
                    EntitySpan span;
                    span.texture = white_texture_;
                    span.blend = EntityBlend::additive;
                    span.first = static_cast<std::uint32_t>(batch.vertices.size());
                    for (const int index : cube_indices)
                    {
                        const world::Vec3 point = corners[static_cast<std::size_t>(index)];
                        batch.vertices.push_back(
                            {point.x, point.y, point.z, 0.5F, 0.5F, 1.0F, 0.0F, 0.0F, 0.5F});
                    }
                    span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                    batch.spans.push_back(span);
                }
            }
            // The dragon's fireballs: the original FireBall model tumbling
            // about its local X at 3 rad/s, fullbright (FireballModelEntity
            // disables all lighting), scaled up so the blaze reads at range.
            if (!fireball_visuals_.empty() && fireball_model_ != nullptr &&
                fireball_model_->loaded())
            {
                for (const FireballVisual &visual : fireball_visuals_)
                {
                    const float spin_cos = std::cos(visual.spin);
                    const float spin_sin = std::sin(visual.spin);
                    constexpr float fireball_scale = 1.6F;
                    for (const StaticModelPart &part : fireball_model_->parts())
                    {
                        EntitySpan span;
                        span.texture = part.texture;
                        span.first = static_cast<std::uint32_t>(batch.vertices.size());
                        for (const std::uint32_t index : part.indices)
                        {
                            const StaticModelVertex &source = part.vertices[index];
                            const auto &m = part.transform;
                            const std::array<float, 3> baked{
                                source.position[0] * m[0] + source.position[1] * m[4] +
                                    source.position[2] * m[8] + m[12],
                                source.position[0] * m[1] + source.position[1] * m[5] +
                                    source.position[2] * m[9] + m[13],
                                source.position[0] * m[2] + source.position[1] * m[6] +
                                    source.position[2] * m[10] + m[14]};
                            // Tumble about local X, then scale and translate.
                            const float local_y = baked[1] * spin_cos - baked[2] * spin_sin;
                            const float local_z = baked[1] * spin_sin + baked[2] * spin_cos;
                            EntityVertex vertex;
                            vertex.x = visual.position.x + baked[0] * fireball_scale;
                            vertex.y = visual.position.y + local_y * fireball_scale;
                            vertex.z = visual.position.z + local_z * fireball_scale;
                            vertex.u = source.uv[0];
                            vertex.v = source.uv[1];
                            if (visual.ice)
                            {
                                vertex.red = 0.65F;
                                vertex.green = 0.85F;
                                vertex.blue = 1.0F;
                            }
                            batch.vertices.push_back(vertex);
                        }
                        span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                        if (span.count > 0U)
                            batch.spans.push_back(span);
                    }
                }
            }
            // Placed torch props: the real torch model at every cached torch
            // block, tilted per TorchCloud's per-face table, with the
            // original's flat 0.75 ambient.
            if (!torch_cache_.empty() && pickup_torch_ != nullptr && pickup_torch_->loaded())
            {
                for (const TorchInstance &torch : torch_cache_)
                {
                    const float torch_dx =
                        static_cast<float>(torch.x) + 0.5F - render_player.position.x;
                    const float torch_dz =
                        static_cast<float>(torch.z) + 0.5F - render_player.position.z;
                    if (torch_dx * torch_dx + torch_dz * torch_dz > 48.0F * 48.0F)
                        continue;
                    const TorchPlacement placement = torch_placement(torch.variant);
                    const world::Vec3 base{static_cast<float>(torch.x) + 0.5F + placement.offset_x,
                                           static_cast<float>(torch.y) + 0.5F + placement.offset_y,
                                           static_cast<float>(torch.z) + 0.5F + placement.offset_z};
                    for (const StaticModelPart &part : pickup_torch_->parts())
                    {
                        EntitySpan span;
                        span.texture = part.texture;
                        span.first = static_cast<std::uint32_t>(batch.vertices.size());
                        for (const std::uint32_t index : part.indices)
                        {
                            const StaticModelVertex &source = part.vertices[index];
                            const auto &m = part.transform;
                            const std::array<float, 3> local{
                                source.position[0] * m[0] + source.position[1] * m[4] +
                                    source.position[2] * m[8] + m[12],
                                source.position[0] * m[1] + source.position[1] * m[5] +
                                    source.position[2] * m[9] + m[13],
                                source.position[0] * m[2] + source.position[1] * m[6] +
                                    source.position[2] * m[10] + m[14]};
                            const world::Vec3 rotated =
                                torch_rotate(placement, local[0], local[1], local[2]);
                            EntityVertex vertex;
                            vertex.x = base.x + rotated.x;
                            vertex.y = base.y + rotated.y;
                            vertex.z = base.z + rotated.z;
                            vertex.u = source.uv[0];
                            vertex.v = source.uv[1];
                            vertex.red = 0.75F;
                            vertex.green = 0.75F;
                            vertex.blue = 0.75F;
                            batch.vertices.push_back(vertex);
                        }
                        span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                        if (span.count > 0U)
                            batch.spans.push_back(span);
                    }
                }
            }
            append_door_models(batch, render_player, sky);
            // Particles and pickups join the depth-tested batch. Soft spans
            // (alpha/additive) draw after all opaque geometry with read-only
            // depth, so nothing shows through zombies, terrain, or props.
            {
                const float sine_yaw = std::sin(render_player.yaw);
                const float cosine_yaw = std::cos(render_player.yaw);
                const float sine_pitch = std::sin(render_player.pitch);
                const float cosine_pitch = std::cos(render_player.pitch);
                const world::Vec3 camera_right{cosine_yaw, 0.0F, sine_yaw};
                const world::Vec3 camera_up{-sine_yaw * sine_pitch, cosine_pitch,
                                            cosine_yaw * sine_pitch};
                const bool soft_particle_depth =
                    SDL_getenv("OPENMINERZ_DISABLE_SOFT_PARTICLES") == nullptr;
                const bool hdr_vfx =
                    rebirth_presentation && SDL_getenv("OPENMINERZ_DISABLE_HDR_VFX") == nullptr;
                const auto append_billboard = [&](world::Vec3 center, float world_size,
                                                  float rotation_degrees,
                                                  const std::array<float, 4> &color) {
                    constexpr float pi = 3.14159265358979323846F;
                    const float radians = rotation_degrees * pi / 180.0F;
                    const float cosine = std::cos(radians);
                    const float sine = std::sin(radians);
                    const auto corner = [&](float local_x, float local_y, float u, float v) {
                        const float right_amount = (local_x * cosine - local_y * sine) * world_size;
                        const float up_amount = -(local_x * sine + local_y * cosine) * world_size;
                        EntityVertex vertex;
                        vertex.x =
                            center.x + camera_right.x * right_amount + camera_up.x * up_amount;
                        vertex.y =
                            center.y + camera_right.y * right_amount + camera_up.y * up_amount;
                        vertex.z =
                            center.z + camera_right.z * right_amount + camera_up.z * up_amount;
                        vertex.u = u;
                        vertex.v = v;
                        vertex.red = color[0];
                        vertex.green = color[1];
                        vertex.blue = color[2];
                        vertex.alpha = color[3];
                        // Negative light-data Y is an explicit soft-particle
                        // marker. Face decals retain zero and therefore stay
                        // tightly attached instead of fading off their block.
                        vertex.torch_light = soft_particle_depth ? -1.0F : 0.0F;
                        return vertex;
                    };
                    const EntityVertex top_left = corner(-1.0F, 1.0F, 0.0F, 1.0F);
                    const EntityVertex top_right = corner(1.0F, 1.0F, 1.0F, 1.0F);
                    const EntityVertex bottom_right = corner(1.0F, -1.0F, 1.0F, 0.0F);
                    const EntityVertex bottom_left = corner(-1.0F, -1.0F, 0.0F, 0.0F);
                    batch.vertices.push_back(top_left);
                    batch.vertices.push_back(top_right);
                    batch.vertices.push_back(bottom_right);
                    batch.vertices.push_back(top_left);
                    batch.vertices.push_back(bottom_right);
                    batch.vertices.push_back(bottom_left);
                };
                const auto particle_span = [&](SDL_Texture *texture, ParticleKind kind,
                                               EntityBlend blend) {
                    if (texture == nullptr)
                        return;
                    EntitySpan span;
                    span.texture = texture;
                    span.blend = blend;
                    span.first = static_cast<std::uint32_t>(batch.vertices.size());
                    for (const MiningParticle &particle : mining_particles_)
                    {
                        if (particle.kind != kind)
                            continue;
                        const float remaining =
                            std::clamp(1.0F - particle.age / particle.lifetime, 0.0F, 1.0F);
                        const float progress =
                            std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
                        const float grown_size =
                            particle.size + (particle.end_size - particle.size) * progress;
                        std::array<float, 4> color{1.0F, 1.0F, 1.0F, remaining};
                        float size = grown_size;
                        switch (kind)
                        {
                        case ParticleKind::rock:
                            color = {1.0F, 0.88F, 0.66F, remaining};
                            size = particle.size;
                            break;
                        case ParticleKind::spark:
                            color = {1.0F, 0.82F, 0.42F, remaining};
                            size = particle.size;
                            break;
                        case ParticleKind::torch_smoke:
                            color = {0.62F, 0.62F, 0.62F, remaining * 0.8F};
                            break;
                        case ParticleKind::mining_dust:
                        case ParticleKind::impact_smoke: {
                            const float dx = particle.position.x - render_player.position.x;
                            const float dy =
                                particle.position.y -
                                (render_player.position.y + world::PlayerSimulation::eye_height);
                            const float dz = particle.position.z - render_player.position.z;
                            const float eye_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                            const float distance_weight =
                                std::clamp((eye_distance - 1.0F) / 7.0F, 0.0F, 1.0F);
                            const bool mining_dust = kind == ParticleKind::mining_dust;
                            const float visibility = mining_dust
                                                         ? std::lerp(0.62F, 0.84F, distance_weight)
                                                         : std::lerp(0.52F, 0.72F, distance_weight);
                            const float fade = remaining * (0.36F + 0.64F * remaining);
                            color = {particle.tint[0], particle.tint[1], particle.tint[2],
                                     fade * visibility * particle.opacity};
                            size *= std::lerp(mining_dust ? 0.78F : 0.74F, 1.0F, distance_weight);
                            break;
                        }
                        case ParticleKind::smoke:
                        case ParticleKind::flash:
                        case ParticleKind::ice_flash:
                        case ParticleKind::fire_soft:
                        case ParticleKind::ice_soft:
                        case ParticleKind::blast_rock:
                            color = {particle.tint[0], particle.tint[1], particle.tint[2],
                                     remaining};
                            break;
                        default:
                            break;
                        }
                        if (hdr_vfx)
                        {
                            float emissive_gain = 1.0F;
                            if (kind == ParticleKind::spark)
                                emissive_gain = 1.50F;
                            else if (kind == ParticleKind::torch_fire)
                                emissive_gain = 2.05F;
                            else if (kind == ParticleKind::flash || kind == ParticleKind::ice_flash)
                                emissive_gain = 2.20F;
                            else if (kind == ParticleKind::fire_soft ||
                                     kind == ParticleKind::ice_soft)
                                emissive_gain = 1.55F;
                            color[0] *= emissive_gain;
                            color[1] *= emissive_gain;
                            color[2] *= emissive_gain;
                        }
                        append_billboard(particle.position, size, particle.rotation, color);
                    }
                    span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
                    if (span.count > 0U)
                        batch.spans.push_back(span);
                };
                // Opaque pickups first (cubes + item models), then the soft spans.
                if (atlas_ != nullptr)
                {
                    EntitySpan cube_span;
                    cube_span.texture = atlas_;
                    cube_span.first = static_cast<std::uint32_t>(batch.vertices.size());
                    for (const BlockPickup &pickup : block_pickups_)
                    {
                        if (pickup.block == world::BlockType::empty)
                            continue;
                        if (!pickup.flying_to_player && pickup.time_left <= 5.0F &&
                            std::fmod(pickup.age * 8.0F, 1.0F) >= 0.5F)
                            continue;
                        world::Vec3 center = pickup.position;
                        if (!pickup.flying_to_player)
                            center.y += std::sin(pickup.bounce_phase) * 0.1F + 0.1F;
                        constexpr float half_size = 0.10F;
                        const float cosine = std::cos(pickup.rotation);
                        const float sine = std::sin(pickup.rotation);
                        const auto rotate_y = [&](float local_x, float local_y, float local_z) {
                            return world::Vec3{center.x + local_x * cosine + local_z * sine,
                                               center.y + local_y,
                                               center.z - local_x * sine + local_z * cosine};
                        };
                        struct PickupFace final
                        {
                            world::FaceDirection direction{};
                            float light{};
                        };
                        static constexpr std::array<PickupFace, 6> faces{{
                            {world::FaceDirection::positive_x, 0.88F},
                            {world::FaceDirection::negative_x, 0.76F},
                            {world::FaceDirection::positive_y, 1.0F},
                            {world::FaceDirection::negative_y, 0.62F},
                            {world::FaceDirection::positive_z, 0.84F},
                            {world::FaceDirection::negative_z, 0.72F},
                        }};
                        for (const PickupFace &face : faces)
                        {
                            const std::uint8_t tile =
                                world::atlas_tile(pickup.block, face.direction);
                            const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
                            const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
                            const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
                            const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
                            const auto uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
                            // Cube corners for this face in the same order the
                            // software path used (see draw_pickup_cube's tables).
                            const auto face_corners = [&]() -> std::array<world::Vec3, 4> {
                                const float h = half_size;
                                switch (face.direction)
                                {
                                case world::FaceDirection::positive_x:
                                    return {rotate_y(h, -h, -h), rotate_y(h, h, -h),
                                            rotate_y(h, h, h), rotate_y(h, -h, h)};
                                case world::FaceDirection::negative_x:
                                    return {rotate_y(-h, -h, h), rotate_y(-h, h, h),
                                            rotate_y(-h, h, -h), rotate_y(-h, -h, -h)};
                                case world::FaceDirection::positive_y:
                                    return {rotate_y(-h, h, -h), rotate_y(-h, h, h),
                                            rotate_y(h, h, h), rotate_y(h, h, -h)};
                                case world::FaceDirection::negative_y:
                                    return {rotate_y(-h, -h, h), rotate_y(-h, -h, -h),
                                            rotate_y(h, -h, -h), rotate_y(h, -h, h)};
                                case world::FaceDirection::positive_z:
                                    return {rotate_y(h, -h, h), rotate_y(h, h, h),
                                            rotate_y(-h, h, h), rotate_y(-h, -h, h)};
                                default:
                                    return {rotate_y(-h, -h, -h), rotate_y(-h, h, -h),
                                            rotate_y(h, h, -h), rotate_y(h, -h, -h)};
                                }
                            }();
                            static constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
                            for (const int index : order)
                            {
                                const auto corner = static_cast<std::size_t>(index);
                                EntityVertex vertex;
                                vertex.x = face_corners[corner].x;
                                vertex.y = face_corners[corner].y;
                                vertex.z = face_corners[corner].z;
                                vertex.u = uv[corner][0];
                                vertex.v = uv[corner][1];
                                vertex.red = face.light;
                                vertex.green = face.light;
                                vertex.blue = face.light;
                                batch.vertices.push_back(vertex);
                            }
                        }
                    }
                    cube_span.count =
                        static_cast<std::uint32_t>(batch.vertices.size()) - cube_span.first;
                    if (cube_span.count > 0U)
                        batch.spans.push_back(cube_span);
                }
                append_target_decals(batch, render_player, elapsed_seconds);
                append_pickup_models(batch);
                particle_span(mining_rocks_, ParticleKind::rock, EntityBlend::alpha);
                particle_span(mining_rocks_, ParticleKind::blast_rock, EntityBlend::alpha);
                particle_span(mining_smoke_, ParticleKind::mining_dust, EntityBlend::alpha);
                particle_span(mining_smoke_, ParticleKind::impact_smoke, EntityBlend::alpha);
                particle_span(mining_smoke_, ParticleKind::smoke, EntityBlend::alpha);
                particle_span(torch_smoke_texture_, ParticleKind::torch_smoke, EntityBlend::alpha);
                particle_span(torch_fire_texture_, ParticleKind::fire_soft, EntityBlend::additive);
                particle_span(ice_puff_texture_ != nullptr ? ice_puff_texture_
                                                           : torch_fire_texture_,
                              ParticleKind::ice_soft, EntityBlend::alpha);
                particle_span(mining_sparks_, ParticleKind::spark, EntityBlend::additive);
                particle_span(torch_fire_texture_, ParticleKind::torch_fire, EntityBlend::additive);
                particle_span(flash_texture_, ParticleKind::flash, EntityBlend::additive);
                particle_span(ice_flash_texture_ != nullptr ? ice_flash_texture_ : flash_texture_,
                              ParticleKind::ice_flash, EntityBlend::additive);
            }
            if (SDL_getenv("OPENMINERZ_ENEMY_DEBUG") != nullptr)
            {
                float min_x{1e9F}, min_y{1e9F}, min_z{1e9F};
                float max_x{-1e9F}, max_y{-1e9F}, max_z{-1e9F};
                for (const EntityVertex &vertex : batch.vertices)
                {
                    min_x = std::min(min_x, vertex.x);
                    min_y = std::min(min_y, vertex.y);
                    min_z = std::min(min_z, vertex.z);
                    max_x = std::max(max_x, vertex.x);
                    max_y = std::max(max_y, vertex.y);
                    max_z = std::max(max_z, vertex.z);
                }
                SDL_Log("enemy batch: %zu enemies, %zu vertices, %zu spans, bounds "
                        "(%.1f %.1f %.1f)-(%.1f %.1f %.1f) player (%.1f %.1f %.1f)",
                        enemy_count, batch.vertices.size(), batch.spans.size(), min_x, min_y, min_z,
                        max_x, max_y, max_z, render_player.position.x, render_player.position.y,
                        render_player.position.z);
            }
            depth_renderer_->submit_entities(std::move(batch));
        }
        else if (depth_renderer_ != nullptr)
        {
            depth_renderer_->submit_entities({});
        }
        if (depth_renderer_ != nullptr)
        {
            const bool dynamic_radiance_enabled =
                rebirth_presentation &&
                SDL_getenv("OPENMINERZ_DISABLE_DYNAMIC_RADIANCE") == nullptr;
            const world::Vec3 volume_center{render_player.position.x,
                                            render_player.position.y +
                                                world::PlayerSimulation::eye_height,
                                            render_player.position.z};

            std::vector<world::DynamicRadianceSource> radiance_sources;
            radiance_sources.reserve(world::dynamic_radiance_max_sources);
            if (dynamic_radiance_enabled && render_player.selected_item_id == 14U)
            {
                constexpr std::array<float, 3> held_torch_linear{
                    1.0F, (235.0F / 255.0F) * (235.0F / 255.0F),
                    (190.0F / 255.0F) * (190.0F / 255.0F)};
                radiance_sources.push_back({volume_center, held_torch_linear,
                                            static_cast<float>(world::dynamic_radiance_radius),
                                            1.0F});
            }
            const char *radiance_capture = SDL_getenv("OPENMINERZ_DYNAMIC_RADIANCE_CAPTURE");
            if (dynamic_radiance_enabled && radiance_capture != nullptr &&
                std::string_view{radiance_capture} == "stress")
            {
                constexpr std::array<float, 3> fire_linear{1.0F, 0.38F, 0.08F};
                constexpr std::array<float, 3> ice_linear{0.22F, 0.65F, 1.0F};
                for (std::size_t index = 0U; index < world::dynamic_radiance_max_sources; ++index)
                {
                    const float angle = static_cast<float>(index) *
                                        (2.0F * std::numbers::pi_v<float> /
                                         static_cast<float>(world::dynamic_radiance_max_sources));
                    radiance_sources.push_back(
                        {{volume_center.x + std::cos(angle) * 2.5F, volume_center.y,
                          volume_center.z + std::sin(angle) * 2.5F},
                         index % 2U == 0U ? fire_linear : ice_linear,
                         6.5F,
                         1.05F,
                         1.0F});
                }
            }

            struct TransientRadianceCandidate final
            {
                world::DynamicRadianceSource source{};
                float distance_squared{};
                int priority{};
            };
            std::vector<TransientRadianceCandidate> transient_sources;
            transient_sources.reserve(fireball_visuals_.size() + rocket_visuals_.size() + 4U);
            const auto distance_to_player_squared = [&](world::Vec3 position) {
                const float x = position.x - volume_center.x;
                const float y = position.y - volume_center.y;
                const float z = position.z - volume_center.z;
                return x * x + y * y + z * z;
            };
            if (dynamic_radiance_enabled)
            {
                for (const FireballVisual &fireball : fireball_visuals_)
                {
                    constexpr std::array<float, 3> fire_linear{1.0F, 0.38F, 0.08F};
                    constexpr std::array<float, 3> ice_linear{0.22F, 0.65F, 1.0F};
                    constexpr float radius = 6.5F;
                    const float distance_squared = distance_to_player_squared(fireball.position);
                    const float reach = static_cast<float>(world::dynamic_radiance_radius) + radius;
                    if (distance_squared <= reach * reach)
                    {
                        transient_sources.push_back(
                            {{fireball.position, fireball.ice ? ice_linear : fire_linear, radius,
                              1.05F, 1.0F},
                             distance_squared,
                             300});
                    }
                }

                // FlashEffect emits five coincident particles. Treat the large
                // explosion recipe as one source and merge its drifting copies;
                // tiny blaster flashes and fireball glow remain visual-only.
                for (const MiningParticle &particle : mining_particles_)
                {
                    if ((particle.kind != ParticleKind::flash &&
                         particle.kind != ParticleKind::ice_flash) ||
                        particle.end_size < 8.0F)
                        continue;
                    const float fade =
                        world::dynamic_radiance_lifetime_fade(particle.age, particle.lifetime);
                    if (fade <= 0.001F)
                        continue;
                    constexpr std::array<float, 3> fire_blast_linear{1.0F, 0.48F, 0.12F};
                    constexpr std::array<float, 3> ice_blast_linear{0.25F, 0.70F, 1.0F};
                    const bool ice = particle.tint[2] > particle.tint[0] + 0.12F;
                    constexpr float radius = 9.0F;
                    const float distance_squared = distance_to_player_squared(particle.position);
                    const float reach = static_cast<float>(world::dynamic_radiance_radius) + radius;
                    if (distance_squared > reach * reach)
                        continue;

                    auto duplicate = std::find_if(
                        transient_sources.begin(), transient_sources.end(),
                        [&](const TransientRadianceCandidate &candidate) {
                            const float x = candidate.source.position.x - particle.position.x;
                            const float y = candidate.source.position.y - particle.position.y;
                            const float z = candidate.source.position.z - particle.position.z;
                            return candidate.source.radius >= 8.5F && x * x + y * y + z * z < 4.0F;
                        });
                    const float intensity = 1.8F * fade;
                    if (duplicate != transient_sources.end())
                    {
                        duplicate->source.intensity =
                            std::max(duplicate->source.intensity, intensity);
                        duplicate->distance_squared =
                            std::min(duplicate->distance_squared, distance_squared);
                        duplicate->priority = std::max(duplicate->priority, 400);
                    }
                    else
                    {
                        transient_sources.push_back(
                            {{particle.position, ice ? ice_blast_linear : fire_blast_linear, radius,
                              intensity, 1.0F},
                             distance_squared,
                             400});
                    }
                }

                // RocketFireTrail is short-lived but continuously re-emitted at
                // the motor nozzle while the authoritative projectile exists.
                for (const RocketVisual &rocket : rocket_visuals_)
                {
                    constexpr std::array<float, 3> exhaust_linear{1.0F, 0.32F, 0.055F};
                    constexpr float radius = 4.5F;
                    const world::Vec3 exhaust{rocket.position.x - rocket.direction.x * 0.25F,
                                              rocket.position.y - rocket.direction.y * 0.25F,
                                              rocket.position.z - rocket.direction.z * 0.25F};
                    const float distance_squared = distance_to_player_squared(exhaust);
                    const float reach = static_cast<float>(world::dynamic_radiance_radius) + radius;
                    if (distance_squared <= reach * reach)
                    {
                        transient_sources.push_back({{exhaust, exhaust_linear, radius, 0.72F, 1.0F},
                                                     distance_squared,
                                                     200});
                    }
                }

                std::stable_sort(transient_sources.begin(), transient_sources.end(),
                                 [](const TransientRadianceCandidate &left,
                                    const TransientRadianceCandidate &right) {
                                     if (left.priority != right.priority)
                                         return left.priority > right.priority;
                                     return left.distance_squared < right.distance_squared;
                                 });
                for (const TransientRadianceCandidate &candidate : transient_sources)
                {
                    if (radiance_sources.size() >= world::dynamic_radiance_max_sources)
                        break;
                    radiance_sources.push_back(candidate.source);
                }
            }
            const auto radiance_build_begin = std::chrono::steady_clock::now();
            auto dynamic_radiance = world::build_dynamic_radiance(
                region_, volume_center,
                std::span<const world::DynamicRadianceSource>{radiance_sources});
            if (SDL_getenv("OPENMINERZ_DYNAMIC_RADIANCE_CAPTURE") != nullptr)
            {
                const double build_milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              radiance_build_begin)
                        .count();
                static std::size_t capture_samples{};
                static double capture_total_milliseconds{};
                static double capture_max_milliseconds{};
                ++capture_samples;
                capture_total_milliseconds += build_milliseconds;
                capture_max_milliseconds = std::max(capture_max_milliseconds, build_milliseconds);
                if (capture_samples == 1U || capture_samples % 300U == 0U)
                {
                    SDL_Log("dynamic radiance capture: %zu sources, active=%d, samples=%zu, "
                            "build avg=%.3f ms max=%.3f ms",
                            radiance_sources.size(), dynamic_radiance.active ? 1 : 0,
                            capture_samples,
                            capture_total_milliseconds / static_cast<double>(capture_samples),
                            capture_max_milliseconds);
                }
            }
            depth_renderer_->submit_dynamic_radiance(std::move(dynamic_radiance));
        }
        const bool depth_rendered =
            depth_renderer_ != nullptr &&
            depth_renderer_->draw(metrics, sky, render_player, vertical_fov_degrees, render_scale,
                                  elapsed_seconds, visibility_distance_, biome_, water_enabled,
                                  rebirth_presentation, ambient_occlusion, shadow_strength);
        if (!depth_rendered)
        {
            if (expected_depth_renderer && depth_renderer_ != nullptr)
            {
                std::cerr << "Depth renderer draw failed: " << depth_renderer_->failure_reason()
                          << '\n';
            }
            stats_.depth_buffered = false;
            draw_first_person(metrics, sky, render_player, vertical_fov_degrees, ambient_occlusion,
                              shadow_strength);
        }
        // The depth path draws target decals, particles and pickups inside the
        // entity batch. SDL overlays only backstop the non-D3D fallback.
        if (!depth_rendered)
        {
            draw_target_outline(metrics, render_player, vertical_fov_degrees, elapsed_seconds);
            draw_mining_effects(metrics, render_player, vertical_fov_degrees, elapsed_seconds);
        }
        const InventoryItemId held_item =
            static_cast<InventoryItemId>(render_player.selected_item_id);
        const bool scoped_weapon = held_item == InventoryItemId::rocket_launcher ||
                                   held_item == InventoryItemId::guided_rocket_launcher ||
                                   held_item == InventoryItemId::iron_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::copper_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::gold_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::diamond_space_bolt_action_rifle;
        const bool scoped_view =
            scoped_weapon && (render_player.viewmodel_cue == world::ViewmodelCue::shoulder_idle ||
                              render_player.viewmodel_cue == world::ViewmodelCue::shoulder_use);
        if (avatar_viewmodel_ != nullptr && !scoped_view &&
            SDL_getenv("OPENMINERZ_DIAGNOSTIC_HIDE_VIEWMODEL") == nullptr)
        {
            // GetAvatarColor: sample sun/torch light at the player and light
            // the arms with ambient + directional sun (pow(sun, 30) means only
            // open sky gets the direct term), sun direction in camera space.
            const int eye_x = static_cast<int>(std::floor(render_player.position.x));
            const int eye_z = static_cast<int>(std::floor(render_player.position.z));
            const int head_y = static_cast<int>(
                std::floor(render_player.position.y + world::PlayerSimulation::eye_height));
            bool sky_clear = true;
            for (int y = head_y + 1; y < world::world_min_y + world::chunk_height; ++y)
                if (region_.contains(eye_x, y, eye_z) &&
                    world::occludes(region_.block(eye_x, y, eye_z)))
                {
                    sky_clear = false;
                    break;
                }
            const float sun_amount = sky_clear ? 1.0F : 0.2F;
            float torch_amount = local_emissive_light(region_, {render_player.position.x,
                                                                render_player.position.y + 1.0F,
                                                                render_player.position.z});
            // The terrain shader already treats an equipped torch as a local
            // light. Feed the same source to the separate first-person pass so
            // the torch and hands do not remain black inside the cave they light.
            if (render_player.selected_item_id == 14U)
                torch_amount = 1.0F;
            constexpr std::array<float, 3> torch_color{255.0F / 255.0F, 235.0F / 255.0F,
                                                       190.0F / 255.0F};
            float torch_mix = torch_amount * (1.0F - sun_amount * sky.direct.green);
            // A held flame is close to the camera but not a one-unit white
            // ambient dome. Match the world shader's perceived local exposure
            // without bleaching the sleeve and hand to paper white.
            if (render_player.selected_item_id == 14U)
                torch_mix *= 0.72F;
            const float hell_viewmodel =
                rebirth_presentation && SDL_getenv("OPENMINERZ_LEGACY_HELL_ATMOSPHERE") == nullptr
                    ? std::clamp((-32.0F - (render_player.position.y +
                                            world::PlayerSimulation::eye_height)) /
                                     8.0F,
                                 0.0F, 1.0F)
                    : 0.0F;
            // The first-person rig is a separate pass and cannot sample the
            // terrain's directional probe. Give it the same restrained furnace
            // floor so hands and the held tool remain identifiable in Hell.
            constexpr std::array<float, 3> hell_rig_ambient{0.080F, 0.014F, 0.004F};
            const std::array<float, 3> ambient{
                std::min(1.0F, sky.ambient.red * sun_amount + torch_color[0] * torch_mix +
                                   hell_rig_ambient[0] * hell_viewmodel),
                std::min(1.0F, sky.ambient.green * sun_amount + torch_color[1] * torch_mix +
                                   hell_rig_ambient[1] * hell_viewmodel),
                std::min(1.0F, sky.ambient.blue * sun_amount + torch_color[2] * torch_mix +
                                   hell_rig_ambient[2] * hell_viewmodel)};
            const float direct_gain = std::pow(sun_amount, 30.0F);
            const std::array<float, 3> directional{sky.direct.red * direct_gain,
                                                   sky.direct.green * direct_gain,
                                                   sky.direct.blue * direct_gain};
            const float sine_yaw = std::sin(render_player.yaw);
            const float cosine_yaw = std::cos(render_player.yaw);
            const float sine_pitch = std::sin(render_player.pitch);
            const float cosine_pitch = std::cos(render_player.pitch);
            const std::array<float, 3> sun_world{sky.sun_x, sky.sun_y, sky.sun_z};
            const std::array<float, 3> sun_camera{
                sun_world[0] * cosine_yaw + sun_world[2] * sine_yaw,
                sun_world[0] * (-sine_yaw * sine_pitch) + sun_world[1] * cosine_pitch +
                    sun_world[2] * (cosine_yaw * sine_pitch),
                sun_world[0] * (sine_yaw * cosine_pitch) + sun_world[1] * sine_pitch +
                    sun_world[2] * (-cosine_yaw * cosine_pitch)};
            world::SkyColor viewmodel_fog = sky.fog;
            if (rebirth_presentation)
            {
                constexpr float horizon_mix = 0.72F;
                viewmodel_fog = {std::lerp(sky.fog.red, sky.horizon.red, horizon_mix),
                                 std::lerp(sky.fog.green, sky.horizon.green, horizon_mix),
                                 std::lerp(sky.fog.blue, sky.horizon.blue, horizon_mix)};
                viewmodel_fog = {viewmodel_fog.red * viewmodel_fog.red,
                                 viewmodel_fog.green * viewmodel_fog.green,
                                 viewmodel_fog.blue * viewmodel_fog.blue};
            }
            const DisplayGrading grading =
                depth_renderer_ != nullptr ? depth_renderer_->display_grading() : DisplayGrading{};
            avatar_viewmodel_->set_environment_light(
                ambient, directional, sun_camera, rebirth_presentation, torch_mix, grading.exposure,
                grading.tone_curve, grading.color_grading,
                {viewmodel_fog.red, viewmodel_fog.green, viewmodel_fog.blue});
            avatar_viewmodel_->draw(metrics, render_player, elapsed_seconds, reduced_motion,
                                    viewmodel_fov_degrees, atlas_);
        }
        const float eye_y = render_player.position.y + world::PlayerSimulation::eye_height;
        if (water_enabled && eye_y < world::water_level)
        {
            fill(renderer_,
                 {0.0F, 0.0F, static_cast<float>(metrics.output_width),
                  static_cast<float>(metrics.output_height)},
                 {4U, 42U, 58U, 76U});
            if (!reduced_motion)
            {
                for (int band = 0; band < 5; ++band)
                {
                    const float wave = std::sin(elapsed_seconds * 0.7F + band * 1.9F);
                    const float y = (0.18F + static_cast<float>(band) * 0.17F) *
                                        static_cast<float>(metrics.output_height) +
                                    wave * metrics.px(9.0F);
                    fill(renderer_,
                         {0.0F, y, static_cast<float>(metrics.output_width),
                          std::max(1.0F, metrics.px(2.0F))},
                         {116U, 205U, 214U, 17U});
                }
            }
        }
    }
}

void WorldPreviewRenderer::draw_sky(const ui::UiMetrics &metrics, const world::SkyState &sky,
                                    float elapsed_seconds, bool reduced_motion, float yaw,
                                    float pitch, float vertical_fov_degrees,
                                    bool rebirth_presentation)
{
    constexpr int bands = 36;
    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    // Match the world's exposure and tone curve so the sky and the terrain
    // agree at the horizon.
    const DisplayGrading grading =
        depth_renderer_ != nullptr ? depth_renderer_->display_grading() : DisplayGrading{};
    world::SkyColor presentation_fog = sky.fog;
    if (rebirth_presentation)
    {
        constexpr float horizon_mix = 0.72F;
        presentation_fog = {std::lerp(sky.fog.red, sky.horizon.red, horizon_mix),
                            std::lerp(sky.fog.green, sky.horizon.green, horizon_mix),
                            std::lerp(sky.fog.blue, sky.horizon.blue, horizon_mix)};
    }
    const std::array<float, 3> tint = fog_white_balance(presentation_fog, grading.color_grading,
                                                        rebirth_presentation ? 1.25F : 4.0F);
    // Sky textures are authored display-referred images. Feeding the terrain's
    // eye-adaptation multiplier into them crushed the already-dark cloud plate
    // into near-black at noon. Rebirth grades the sky independently, like a
    // modern HDR pipeline, while Classic retains the recovered behavior.
    const float storminess =
        rebirth_presentation ? world::biome_atmosphere(biome_).storminess : 0.0F;
    const float sky_exposure = rebirth_presentation ? std::lerp(0.92F, 1.18F, sky.daylight) *
                                                          std::lerp(1.0F, 0.86F, storminess)
                                                    : grading.exposure;
    const SDL_Color zenith =
        to_color(graded_sky_color(sky.zenith, sky_exposure, grading.tone_curve, tint));
    const SDL_Color horizon =
        to_color(graded_sky_color(sky.horizon, sky_exposure, grading.tone_curve, tint));
    // The cube-map skybox carries its own colours, so it gets the scalar the
    // tone curve applies at a representative sky brightness.
    constexpr float sky_reference = 0.72F;
    const float sky_scale =
        tone_curve_channel(sky_reference * sky_exposure, grading.tone_curve) / sky_reference;
    for (int band = 0; band < bands; ++band)
    {
        const float amount = static_cast<float>(band) / static_cast<float>(bands - 1);
        const float eased = amount * amount * (3.0F - 2.0F * amount);
        fill(renderer_, {0.0F, height * amount, width, height / bands + 2.0F},
             mix(zenith, horizon, eased));
    }

    if (stats_.original_skies_loaded)
    {
        const SkyCube *first = &night_sky_;
        const SkyCube *second = &night_sky_;
        float amount{};
        const float hours = sky.time_of_day * 24.0F;
        const int hour = static_cast<int>(hours);
        const float fraction = hours - static_cast<float>(hour);
        if (hour == 6)
        {
            second = &dawn_sky_;
            amount = fraction;
        }
        else if (hour == 7)
        {
            first = &dawn_sky_;
            second = &dawn_sky_;
        }
        else if (hour == 8)
        {
            first = &dawn_sky_;
            second = &clear_sky_;
            amount = fraction;
        }
        else if (hour >= 9 && hour <= 17)
        {
            first = &clear_sky_;
            second = &clear_sky_;
        }
        else if (hour == 18)
        {
            first = &clear_sky_;
            second = &sunset_sky_;
            amount = fraction;
        }
        else if (hour == 19)
        {
            first = &sunset_sky_;
            second = &sunset_sky_;
        }
        else if (hour == 20)
        {
            first = &sunset_sky_;
            second = &night_sky_;
            amount = fraction;
        }
        constexpr float pi = 3.14159265358979323846F;
        const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
        const float tangent_vertical = std::tan(vertical_fov * 0.5F);
        const float tangent_horizontal = tangent_vertical * width / std::max(height, 1.0F);
        const float sine_yaw = std::sin(yaw);
        const float cosine_yaw = std::cos(yaw);
        const float sine_pitch = std::sin(pitch);
        const float cosine_pitch = std::cos(pitch);
        const world::Vec3 camera_right{cosine_yaw, 0.0F, sine_yaw};
        const world::Vec3 camera_up{-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch};
        const world::Vec3 camera_forward{sine_yaw * cosine_pitch, sine_pitch,
                                         -cosine_yaw * cosine_pitch};
        const auto cube_direction = [](std::size_t face, float u, float v) {
            switch (face)
            {
            case 0U:
                return world::Vec3{-u, -v, -1.0F};
            case 1U:
                return world::Vec3{1.0F, -v, -u};
            case 2U:
                return world::Vec3{u, -v, 1.0F};
            case 3U:
                return world::Vec3{-1.0F, -v, u};
            case 4U:
                return world::Vec3{u, 1.0F, v};
            default:
                return world::Vec3{u, -1.0F, -v};
            }
        };
        struct SkyProjection final
        {
            SDL_FPoint point{};
            float depth{};
        };
        const auto project_sky = [&](world::Vec3 direction) {
            const float camera_x = direction.x * camera_right.x + direction.y * camera_right.y +
                                   direction.z * camera_right.z;
            const float camera_y =
                direction.x * camera_up.x + direction.y * camera_up.y + direction.z * camera_up.z;
            const float camera_z = direction.x * camera_forward.x + direction.y * camera_forward.y +
                                   direction.z * camera_forward.z;
            return SkyProjection{{width * 0.5F + camera_x / std::max(camera_z, 0.0001F) * width *
                                                     0.5F / tangent_horizontal,
                                  height * 0.5F - camera_y / std::max(camera_z, 0.0001F) * height *
                                                      0.5F / tangent_vertical},
                                 camera_z};
        };
        const auto render_cube = [&](const SkyCube &cube, float alpha) {
            const int segments =
                std::clamp(static_cast<int>(std::ceil(std::max(width, height) / 96.0F)), 16, 48);
            for (std::size_t face = 0U; face < cube.size(); ++face)
            {
                std::vector<SDL_Vertex> vertices;
                vertices.reserve(static_cast<std::size_t>(segments * segments * 6));
                for (int row = 0; row < segments; ++row)
                {
                    const float v0 =
                        -1.0F + 2.0F * static_cast<float>(row) / static_cast<float>(segments);
                    const float v1 =
                        -1.0F + 2.0F * static_cast<float>(row + 1) / static_cast<float>(segments);
                    for (int column = 0; column < segments; ++column)
                    {
                        const float u0 = -1.0F + 2.0F * static_cast<float>(column) /
                                                     static_cast<float>(segments);
                        const float u1 = -1.0F + 2.0F * static_cast<float>(column + 1) /
                                                     static_cast<float>(segments);
                        const std::array projections{
                            project_sky(cube_direction(face, u0, v0)),
                            project_sky(cube_direction(face, u1, v0)),
                            project_sky(cube_direction(face, u1, v1)),
                            project_sky(cube_direction(face, u0, v1)),
                        };
                        if (std::any_of(projections.begin(), projections.end(),
                                        [](const SkyProjection &projection) {
                                            return projection.depth <= 0.025F;
                                        }))
                        {
                            continue;
                        }
                        const std::array texture_coordinates{
                            SDL_FPoint{u0 * 0.5F + 0.5F, v0 * 0.5F + 0.5F},
                            SDL_FPoint{u1 * 0.5F + 0.5F, v0 * 0.5F + 0.5F},
                            SDL_FPoint{u1 * 0.5F + 0.5F, v1 * 0.5F + 0.5F},
                            SDL_FPoint{u0 * 0.5F + 0.5F, v1 * 0.5F + 0.5F},
                        };
                        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
                        for (const int index : order)
                        {
                            const auto vertex = static_cast<std::size_t>(index);
                            vertices.push_back({projections[vertex].point,
                                                // SDL's geometry path does not reliably consume
                                                // texture alpha modulation on every renderer
                                                // backend. Carry the authored-cloud blend in the
                                                // vertex alpha that RenderGeometry always uses.
                                                {sky_scale, sky_scale, sky_scale, alpha},
                                                texture_coordinates[vertex]});
                        }
                    }
                }
                if (vertices.empty())
                {
                    continue;
                }
                SDL_RenderGeometry(renderer_, cube[face], vertices.data(),
                                   static_cast<int>(vertices.size()), nullptr, 0);
            }
        };
        // The Modern clear cube is intentionally a very dark storm panorama, not
        // an HDR atmosphere. Treating it as a 74%-opaque replacement at noon
        // crushed the physical Rebirth zenith/horizon gradient back to charcoal
        // and made every daylight frame feel underexposed. In Rebirth it is a
        // cloud/detail layer: strong enough to preserve the authored formation at
        // night, but transparent enough in daylight for the solar atmosphere to
        // carry the frame's energy. Classic keeps the installed cube unchanged.
        // The diagnostic switch restores the previous overlay for matched stills.
        const bool legacy_sky_overlay = SDL_getenv("OPENMINERZ_LEGACY_SKY_OVERLAY") != nullptr;
        const float authored_cloud_alpha =
            !rebirth_presentation || legacy_sky_overlay
                ? (rebirth_presentation ? std::lerp(0.92F, 0.74F, sky.daylight) : 1.0F)
                : std::lerp(std::lerp(0.54F, 0.18F, sky.daylight),
                            std::lerp(0.94F, 0.78F, sky.daylight), storminess);
        if (second != first && amount > 0.0F)
        {
            // Two ordinary alpha draws can exactly represent
            // background*(1-a) + first*a*(1-t) + second*a*t when the first
            // pass is solved for the attenuation introduced by the second.
            // Drawing both plates at `a` left old-period clouds behind during
            // dawn/sunset transitions and made the sky muddy at mid-blend.
            const float second_alpha = amount * authored_cloud_alpha;
            const float first_alpha =
                authored_cloud_alpha * (1.0F - amount) / std::max(1.0F - second_alpha, 0.0001F);
            if (first_alpha > 0.0001F)
                render_cube(*first, std::clamp(first_alpha, 0.0F, 1.0F));
            render_cube(*second, std::clamp(second_alpha, 0.0F, 1.0F));
        }
        else
        {
            render_cube(*first, authored_cloud_alpha);
        }
        fill(renderer_, {0.0F, 0.0F, width, height},
             {to_color(presentation_fog).r, to_color(presentation_fog).g,
              to_color(presentation_fog).b, rebirth_presentation ? 18U : 28U});
    }

    // The voxel pass becomes completely fog-coloured before its bounded chunk window ends.
    // Put the same colour behind it in a world-horizontal belt so an unloaded edge never reads
    // as a hole into the skybox.  Reconstructing each row's world elevation keeps this belt on
    // the horizon while pitching instead of pinning a dark gradient to the screen.
    {
        constexpr float pi = 3.14159265358979323846F;
        const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
        const float focal_length = height * 0.5F / std::tan(vertical_fov * 0.5F);
        const float sine_pitch = std::sin(pitch);
        const float cosine_pitch = std::cos(pitch);
        const SDL_Color fog_color = to_color(presentation_fog);
        const auto fog_amount_at = [&](float y) {
            const float camera_y = (height * 0.5F - y) / focal_length;
            const float world_y = cosine_pitch * camera_y + sine_pitch;
            const float inverse_length = 1.0F / std::sqrt(1.0F + camera_y * camera_y);
            const float elevation_sine = world_y * inverse_length;
            float amount{};
            if (elevation_sine < -0.22F)
                amount = std::clamp((elevation_sine + 0.55F) / 0.33F, 0.0F, 1.0F);
            else if (elevation_sine > 0.08F)
                amount = std::clamp((0.36F - elevation_sine) / 0.28F, 0.0F, 1.0F);
            else
                amount = 1.0F;
            return amount * amount * (3.0F - 2.0F * amount);
        };

        // Render one continuous interpolated strip. The former stack of
        // overlapping translucent rectangles accumulated alpha at every
        // four-pixel boundary and produced the conspicuous horizontal
        // "scanlines" around distant mountains.
        constexpr int fog_segments = 96;
        std::vector<SDL_Vertex> fog_vertices;
        fog_vertices.reserve(static_cast<std::size_t>(fog_segments) * 6U);
        const auto fog_vertex = [&](float x, float y, float amount) {
            SDL_FColor color = to_float_color(fog_color);
            color.a = std::clamp(amount, 0.0F, 1.0F);
            return SDL_Vertex{{x, y}, color, {0.0F, 0.0F}};
        };
        for (int segment = 0; segment < fog_segments; ++segment)
        {
            const float y0 = height * static_cast<float>(segment) / fog_segments;
            const float y1 = height * static_cast<float>(segment + 1) / fog_segments;
            const float amount0 = fog_amount_at(y0);
            const float amount1 = fog_amount_at(y1);
            if (amount0 <= 0.002F && amount1 <= 0.002F)
                continue;
            const SDL_Vertex top_left = fog_vertex(0.0F, y0, amount0);
            const SDL_Vertex top_right = fog_vertex(width, y0, amount0);
            const SDL_Vertex bottom_left = fog_vertex(0.0F, y1, amount1);
            const SDL_Vertex bottom_right = fog_vertex(width, y1, amount1);
            fog_vertices.insert(fog_vertices.end(), {top_left, top_right, bottom_right, top_left,
                                                     bottom_right, bottom_left});
        }
        if (!fog_vertices.empty())
            SDL_RenderGeometry(renderer_, nullptr, fog_vertices.data(),
                               static_cast<int>(fog_vertices.size()), nullptr, 0);
    }

    const float night_amount = std::clamp(1.0F - sky.daylight * 1.25F, 0.0F, 1.0F);
    for (int index = 0; index < (stats_.original_skies_loaded ? 0 : 72); ++index)
    {
        const float x = std::fmod(static_cast<float>(index * 977), width);
        const float y = std::fmod(static_cast<float>(index * index * 37), height * 0.58F);
        const float twinkle =
            reduced_motion ? 0.75F : 0.62F + std::sin(elapsed_seconds * 1.8F + index) * 0.28F;
        const Uint8 alpha = static_cast<Uint8>(255.0F * night_amount * twinkle);
        const float size = metrics.px(index % 11 == 0 ? 2.2F : 1.1F);
        fill(renderer_, {x, y, size, size}, {220U, 229U, 239U, alpha});
    }

    constexpr float pi = 3.14159265358979323846F;
    const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
    const float sine_yaw = std::sin(yaw);
    const float cosine_yaw = std::cos(yaw);
    const float sine_pitch = std::sin(pitch);
    const float cosine_pitch = std::cos(pitch);
    const world::Vec3 camera_right{cosine_yaw, 0.0F, sine_yaw};
    const world::Vec3 camera_up{-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch};
    const world::Vec3 camera_forward{sine_yaw * cosine_pitch, sine_pitch,
                                     -cosine_yaw * cosine_pitch};
    const world::Vec3 sun_direction{sky.sun_x, sky.sun_y, sky.sun_z};
    const float sun_depth = sun_direction.x * camera_forward.x +
                            sun_direction.y * camera_forward.y + sun_direction.z * camera_forward.z;
    const float sun_camera_x = sun_direction.x * camera_right.x + sun_direction.y * camera_right.y +
                               sun_direction.z * camera_right.z;
    const float sun_camera_y = sun_direction.x * camera_up.x + sun_direction.y * camera_up.y +
                               sun_direction.z * camera_up.z;
    const float tangent_vertical = std::tan(vertical_fov * 0.5F);
    const float tangent_horizontal = tangent_vertical * width / std::max(height, 1.0F);
    const float sun_x = width * 0.5F + sun_camera_x / std::max(sun_depth, 0.0001F) * width * 0.5F /
                                           tangent_horizontal;
    const float sun_y = height * 0.5F - sun_camera_y / std::max(sun_depth, 0.0001F) * height *
                                            0.5F / tangent_vertical;
    const float sun_radius = metrics.px(30.0F);
    const SDL_Color sun_color = sky.daylight > 0.42F ? SDL_Color{255U, 224U, 154U, 225U}
                                                     : SDL_Color{196U, 210U, 232U, 190U};
    for (int slice = -10; slice <= 10 && !stats_.original_skies_loaded && sun_depth > 0.0F; ++slice)
    {
        const float ratio = static_cast<float>(slice) / 10.0F;
        const float half_width = sun_radius * std::sqrt(std::max(0.0F, 1.0F - ratio * ratio));
        fill(renderer_,
             {sun_x - half_width, sun_y + ratio * sun_radius, half_width * 2.0F,
              std::max(1.0F, sun_radius / 10.0F)},
             sun_color);
    }

    const float motion = reduced_motion ? 0.0F : elapsed_seconds;
    for (int cloud = 0; cloud < (stats_.original_skies_loaded ? 0 : 7); ++cloud)
    {
        const float cloud_width = metrics.px(160.0F + static_cast<float>((cloud * 53) % 170));
        const float wrap = width + cloud_width * 2.0F;
        const float x = std::fmod(static_cast<float>(cloud * 331) + motion * (4.0F + cloud), wrap) -
                        cloud_width;
        const float y = metrics.px(115.0F + static_cast<float>(cloud % 3) * 74.0F);
        fill(renderer_, {x, y, cloud_width, metrics.px(18.0F)},
             {220U, 225U, 226U, static_cast<Uint8>(18.0F + sky.daylight * 24.0F)});
        fill(
            renderer_,
            {x + cloud_width * 0.2F, y - metrics.px(11.0F), cloud_width * 0.48F, metrics.px(14.0F)},
            {220U, 225U, 226U, static_cast<Uint8>(14.0F + sky.daylight * 20.0F)});
    }
    if (sky.lightning)
    {
        // CastleMinerSky replaces its cube-map blend with pure white. This
        // executes before terrain, so the flash illuminates only visible sky
        // rather than washing the whole screen or brightening cave geometry.
        fill(renderer_, {0.0F, 0.0F, width, height}, {255U, 255U, 255U, 255U});
    }
}

void WorldPreviewRenderer::draw_inspector(const ui::UiMetrics &metrics, const world::SkyState &sky,
                                          bool ambient_occlusion, float shadow_strength)
{
    refresh_flat_faces();
    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    const float tile_width = std::clamp(width / 86.0F, metrics.px(13.0F), metrics.px(25.0F));
    const Projection projection{width * 0.5F,
                                height * 0.60F,
                                tile_width,
                                tile_width * 0.5F,
                                tile_width * 0.36F,
                                (region_.minimum_x() + region_.maximum_x() + 1) * 0.5F,
                                (region_.minimum_z() + region_.maximum_z() + 1) * 0.5F};
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(faces_.size() * 6U);
    for (const RenderFace &render_face : faces_)
    {
        const auto &face = render_face.face;
        if ((face.direction != world::FaceDirection::positive_y &&
             face.direction != world::FaceDirection::positive_x &&
             face.direction != world::FaceDirection::positive_z) ||
            face.y < -8)
        {
            continue;
        }
        const float x = static_cast<float>(face.x);
        const float y = static_cast<float>(face.y);
        const float z = static_cast<float>(face.z);
        std::array<SDL_FPoint, 4> points{};
        if (face.direction == world::FaceDirection::positive_y)
        {
            points = {projection.point(x, y + 1.0F, z), projection.point(x + 1.0F, y + 1.0F, z),
                      projection.point(x + 1.0F, y + 1.0F, z + 1.0F),
                      projection.point(x, y + 1.0F, z + 1.0F)};
        }
        else if (face.direction == world::FaceDirection::positive_x)
        {
            points = {projection.point(x + 1.0F, y, z), projection.point(x + 1.0F, y + 1.0F, z),
                      projection.point(x + 1.0F, y + 1.0F, z + 1.0F),
                      projection.point(x + 1.0F, y, z + 1.0F)};
        }
        else
        {
            points = {projection.point(x, y, z + 1.0F), projection.point(x + 1.0F, y, z + 1.0F),
                      projection.point(x + 1.0F, y + 1.0F, z + 1.0F),
                      projection.point(x, y + 1.0F, z + 1.0F)};
        }
        const int tile_x = face.tile & 7;
        const int tile_y = face.tile >> 3;
        constexpr float inset = 0.0012F;
        const float u0 = static_cast<float>(tile_x) / 8.0F + inset;
        const float v0 = static_cast<float>(tile_y) / 8.0F + inset;
        const float u1 = static_cast<float>(tile_x + 1) / 8.0F - inset;
        const float v1 = static_cast<float>(tile_y + 1) / 8.0F - inset;
        const SDL_Color color =
            face_color(face, sky, atlas_ != nullptr, ambient_occlusion, shadow_strength);
        const auto face_uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
        const std::array<SDL_FPoint, 4> texture_coordinates{{
            {face_uv[0][0], face_uv[0][1]},
            {face_uv[1][0], face_uv[1][1]},
            {face_uv[2][0], face_uv[2][1]},
            {face_uv[3][0], face_uv[3][1]},
        }};
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        for (const int index : order)
        {
            vertices.push_back({points[static_cast<std::size_t>(index)], to_float_color(color),
                                texture_coordinates[static_cast<std::size_t>(index)]});
        }
    }
    SDL_RenderGeometry(renderer_, atlas_, vertices.data(), static_cast<int>(vertices.size()),
                       nullptr, 0);

    const SDL_Color fog = to_color(sky.fog, 0.64F, 48U);
    fill(renderer_, {0.0F, height * 0.91F, width, height * 0.09F}, fog);
}

void WorldPreviewRenderer::draw_first_person(const ui::UiMetrics &metrics,
                                             const world::SkyState &sky,
                                             const world::PlayerState &player,
                                             float vertical_fov_degrees, bool ambient_occlusion,
                                             float shadow_strength)
{
    refresh_flat_faces();
    struct ProjectedFace final
    {
        std::array<SDL_FPoint, 4> points{};
        std::array<SDL_FPoint, 4> texture_coordinates{};
        SDL_FColor color{};
        float depth{};
    };

    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    constexpr float pi = 3.14159265358979323846F;
    const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
    const float focal_length = height * 0.5F / std::tan(vertical_fov * 0.5F);
    const float sine_yaw = std::sin(player.yaw);
    const float cosine_yaw = std::cos(player.yaw);
    const float sine_pitch = std::sin(player.pitch);
    const float cosine_pitch = std::cos(player.pitch);
    const world::Vec3 eye{player.position.x,
                          player.position.y + world::PlayerSimulation::eye_height,
                          player.position.z};

    const auto to_camera = [&](world::Vec3 point) {
        const float dx = point.x - eye.x;
        const float dy = point.y - eye.y;
        const float dz = point.z - eye.z;
        const float camera_x = dx * cosine_yaw + dz * sine_yaw;
        const float level_depth = dx * sine_yaw - dz * cosine_yaw;
        return world::Vec3{camera_x, dy * cosine_pitch - level_depth * sine_pitch,
                           level_depth * cosine_pitch + dy * sine_pitch};
    };
    const auto project = [&](world::Vec3 point) -> std::optional<SDL_FPoint> {
        const world::Vec3 camera = to_camera(point);
        if (camera.z <= 0.06F)
        {
            return std::nullopt;
        }
        return SDL_FPoint{width * 0.5F + camera.x * focal_length / camera.z,
                          height * 0.5F - camera.y * focal_length / camera.z};
    };

    std::vector<ProjectedFace> projected;
    projected.reserve(faces_.size() / 3U);
    for (const RenderFace &render_face : faces_)
    {
        const auto &face = render_face.face;
        const float x = static_cast<float>(face.x);
        const float y = static_cast<float>(face.y);
        const float z = static_cast<float>(face.z);
        world::Vec3 normal{};
        std::array<world::Vec3, 4> corners{};
        switch (face.direction)
        {
        case world::FaceDirection::positive_x:
            normal = {1.0F, 0.0F, 0.0F};
            corners = {{{x + 1.0F, y, z},
                        {x + 1.0F, y + 1.0F, z},
                        {x + 1.0F, y + 1.0F, z + 1.0F},
                        {x + 1.0F, y, z + 1.0F}}};
            break;
        case world::FaceDirection::negative_z:
            normal = {0.0F, 0.0F, -1.0F};
            corners = {{{x + 1.0F, y, z}, {x, y, z}, {x, y + 1.0F, z}, {x + 1.0F, y + 1.0F, z}}};
            break;
        case world::FaceDirection::negative_x:
            normal = {-1.0F, 0.0F, 0.0F};
            corners = {{{x, y, z + 1.0F}, {x, y + 1.0F, z + 1.0F}, {x, y + 1.0F, z}, {x, y, z}}};
            break;
        case world::FaceDirection::positive_z:
            normal = {0.0F, 0.0F, 1.0F};
            corners = {{{x, y, z + 1.0F},
                        {x + 1.0F, y, z + 1.0F},
                        {x + 1.0F, y + 1.0F, z + 1.0F},
                        {x, y + 1.0F, z + 1.0F}}};
            break;
        case world::FaceDirection::positive_y:
            normal = {0.0F, 1.0F, 0.0F};
            corners = {{{x, y + 1.0F, z},
                        {x + 1.0F, y + 1.0F, z},
                        {x + 1.0F, y + 1.0F, z + 1.0F},
                        {x, y + 1.0F, z + 1.0F}}};
            break;
        case world::FaceDirection::negative_y:
            normal = {0.0F, -1.0F, 0.0F};
            corners = {{{x, y, z + 1.0F}, {x + 1.0F, y, z + 1.0F}, {x + 1.0F, y, z}, {x, y, z}}};
            break;
        }
        const world::Vec3 center{x + 0.5F, y + 0.5F, z + 0.5F};
        const float camera_facing = normal.x * (eye.x - center.x) + normal.y * (eye.y - center.y) +
                                    normal.z * (eye.z - center.z);
        if (camera_facing <= 0.0F)
        {
            continue;
        }
        const float dx = center.x - eye.x;
        const float dy = center.y - eye.y;
        const float dz = center.z - eye.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > 68.0F * 68.0F)
        {
            continue;
        }
        ProjectedFace output;
        bool clipped{};
        float depth{};
        for (std::size_t index = 0U; index < corners.size(); ++index)
        {
            const auto point = project(corners[index]);
            if (!point.has_value())
            {
                clipped = true;
                break;
            }
            output.points[index] = *point;
            depth += to_camera(corners[index]).z;
        }
        if (clipped)
        {
            continue;
        }
        const bool left = std::all_of(output.points.begin(), output.points.end(),
                                      [](SDL_FPoint point) { return point.x < -2.0F; });
        const bool right =
            std::all_of(output.points.begin(), output.points.end(),
                        [width](SDL_FPoint point) { return point.x > width + 2.0F; });
        const bool above = std::all_of(output.points.begin(), output.points.end(),
                                       [](SDL_FPoint point) { return point.y < -2.0F; });
        const bool below =
            std::all_of(output.points.begin(), output.points.end(),
                        [height](SDL_FPoint point) { return point.y > height + 2.0F; });
        if (left || right || above || below)
        {
            continue;
        }
        const int tile_x = face.tile & 7;
        const int tile_y = face.tile >> 3;
        constexpr float inset = 0.0012F;
        const float u0 = static_cast<float>(tile_x) / 8.0F + inset;
        const float v0 = static_cast<float>(tile_y) / 8.0F + inset;
        const float u1 = static_cast<float>(tile_x + 1) / 8.0F - inset;
        const float v1 = static_cast<float>(tile_y + 1) / 8.0F - inset;
        const auto face_uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
        output.texture_coordinates = {{{face_uv[0][0], face_uv[0][1]},
                                       {face_uv[1][0], face_uv[1][1]},
                                       {face_uv[2][0], face_uv[2][1]},
                                       {face_uv[3][0], face_uv[3][1]}}};
        SDL_Color color =
            face_color(face, sky, atlas_ != nullptr, ambient_occlusion, shadow_strength);
        const float distance = std::sqrt(dx * dx + dz * dz);
        const float fog_end = std::clamp(visibility_distance_ - 10.0F, 24.0F, 118.0F);
        const float fog_start = std::min(24.0F, fog_end - 8.0F);
        const float fog_amount =
            std::clamp((distance - fog_start) / std::max(fog_end - fog_start, 1.0F), 0.0F, 1.0F);
        color = mix(color, to_color(sky.fog), fog_amount);
        output.color = to_float_color(color);
        output.depth = depth * 0.25F;
        projected.push_back(output);
    }
    std::sort(projected.begin(), projected.end(),
              [](const ProjectedFace &left, const ProjectedFace &right) {
                  return left.depth > right.depth;
              });
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(projected.size() * 6U);
    constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
    for (const ProjectedFace &face : projected)
    {
        for (const int index : order)
        {
            vertices.push_back({face.points[static_cast<std::size_t>(index)], face.color,
                                face.texture_coordinates[static_cast<std::size_t>(index)]});
        }
    }
    if (!vertices.empty())
    {
        SDL_RenderGeometry(renderer_, atlas_, vertices.data(), static_cast<int>(vertices.size()),
                           nullptr, 0);
    }
}

WorldPreviewRenderer::PickupModelChoice WorldPreviewRenderer::pickup_model_choice(
    InventoryItemId item) const
{
    constexpr float pi = 3.14159265358979323846F;
    PickupModelChoice choice;
    const auto rgb = [](int red, int green, int blue) {
        constexpr float scale = 1.0F / 255.0F;
        return std::array<float, 3>{red * scale, green * scale, blue * scale};
    };
    choice.primary = item_material_tint(item);
    switch (item)
    {
    case InventoryItemId::stone_pickaxe:
        choice.model = pickup_pickaxe_.get();
        choice.pitch = -pi * 0.5F;
        choice.primary = rgb(169, 169, 169);
        break;
    case InventoryItemId::compass:
        choice.model = pickup_compass_.get();
        choice.scale = 2.0F;
        break;
    case InventoryItemId::pistol:
    case InventoryItemId::gold_pistol:
    case InventoryItemId::diamond_pistol:
    case InventoryItemId::blood_stone_pistol:
        choice.model = pickup_pistol_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::knife:
        choice.model = pickup_knife_.get();
        choice.pitch = -pi * 0.5F;
        choice.primary = rgb(128, 128, 128);
        break;
    case InventoryItemId::bullets:
        choice.model = pickup_ammo_.get();
        choice.scale = 1.5F; // ModelInventoryItemClass's Brass pickup transform.
        choice.primary = rgb(169, 169, 169);
        choice.secondary = rgb(234, 227, 150);
        break;
    case InventoryItemId::torch:
        choice.model = pickup_torch_.get();
        choice.scale = 0.7F;
        break;
    case InventoryItemId::assault_rifle:
    case InventoryItemId::gold_assault_rifle:
    case InventoryItemId::diamond_assault_rifle:
    case InventoryItemId::blood_stone_assault_rifle:
        choice.model = pickup_assault_rifle_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::pump_shotgun:
    case InventoryItemId::gold_pump_shotgun:
    case InventoryItemId::diamond_pump_shotgun:
    case InventoryItemId::blood_stone_pump_shotgun:
        choice.model = pickup_shotgun_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::bolt_action_rifle:
    case InventoryItemId::gold_bolt_action_rifle:
    case InventoryItemId::diamond_bolt_action_rifle:
    case InventoryItemId::blood_stone_bolt_action_rifle:
        choice.model = pickup_rifle_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::smg:
    case InventoryItemId::gold_smg:
    case InventoryItemId::diamond_smg:
    case InventoryItemId::blood_stone_smg:
        choice.model = pickup_smg_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::lmg:
    case InventoryItemId::gold_lmg:
    case InventoryItemId::diamond_lmg:
    case InventoryItemId::blood_stone_lmg:
        choice.model = pickup_lmg_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::rocket_launcher:
    case InventoryItemId::guided_rocket_launcher:
    case InventoryItemId::modern_basic_grenade_launcher:
    case InventoryItemId::modern_advanced_grenade_launcher:
        choice.model = pickup_launcher_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::modern_laser_drill:
    case InventoryItemId::modern_precision_laser:
    case InventoryItemId::modern_multi_laser:
        choice.model = pickup_space_assault_rifle_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::iron_space_assault_rifle:
    case InventoryItemId::copper_space_assault_rifle:
    case InventoryItemId::gold_space_assault_rifle:
    case InventoryItemId::diamond_space_assault_rifle:
        choice.model = pickup_space_assault_rifle_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::iron_space_pistol:
    case InventoryItemId::copper_space_pistol:
    case InventoryItemId::gold_space_pistol:
    case InventoryItemId::diamond_space_pistol:
        choice.model = pickup_space_pistol_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::iron_space_pump_shotgun:
    case InventoryItemId::copper_space_pump_shotgun:
    case InventoryItemId::gold_space_pump_shotgun:
    case InventoryItemId::diamond_space_pump_shotgun:
        choice.model = pickup_space_shotgun_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::iron_space_bolt_action_rifle:
    case InventoryItemId::copper_space_bolt_action_rifle:
    case InventoryItemId::gold_space_bolt_action_rifle:
    case InventoryItemId::diamond_space_bolt_action_rifle:
        choice.model = pickup_space_rifle_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::iron_space_smg:
    case InventoryItemId::copper_space_smg:
    case InventoryItemId::gold_space_smg:
    case InventoryItemId::diamond_space_smg:
        choice.model = pickup_space_smg_.get();
        choice.pitch = -pi * 0.5F;
        break;
    case InventoryItemId::coal:
        choice.model = pickup_ore_.get();
        choice.primary = rgb(0, 0, 0);
        choice.secondary = rgb(0, 0, 0);
        break;
    case InventoryItemId::copper_ore:
        choice.model = pickup_ore_.get();
        choice.primary = rgb(99, 146, 131);
        break;
    case InventoryItemId::iron_ore:
        choice.model = pickup_ore_.get();
        choice.primary = rgb(183, 65, 14);
        break;
    case InventoryItemId::gold_ore:
        choice.model = pickup_ore_.get();
        choice.primary = rgb(255, 215, 0);
        break;
    case InventoryItemId::diamond:
        choice.model = pickup_gems_.get();
        choice.primary = rgb(0, 255, 255);
        break;
    case InventoryItemId::iron:
        choice.model = pickup_bars_.get();
        choice.primary = rgb(128, 128, 128);
        break;
    case InventoryItemId::copper:
        choice.model = pickup_bars_.get();
        choice.primary = rgb(184, 115, 51);
        break;
    case InventoryItemId::gold:
        choice.model = pickup_bars_.get();
        choice.primary = rgb(255, 215, 0);
        break;
    default:
        break;
    }
    return choice;
}

void WorldPreviewRenderer::append_target_decals(EntityBatch &batch,
                                                const world::PlayerState &player,
                                                float elapsed_seconds)
{
    if (!player.target.has_value())
        return;

    const world::Vec3 normal{static_cast<float>(player.target->normal_x),
                             static_cast<float>(player.target->normal_y),
                             static_cast<float>(player.target->normal_z)};
    if (normal.x == 0.0F && normal.y == 0.0F && normal.z == 0.0F)
        return;

    // Exact face-local basis used by the original Selector model.
    world::Vec3 axis_u{};
    world::Vec3 axis_v{};
    if (normal.x > 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {0.0F, 1.0F, 0.0F};
    }
    else if (normal.x < 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {0.0F, -1.0F, 0.0F};
    }
    else if (normal.z < 0.0F)
    {
        axis_u = {0.0F, -1.0F, 0.0F};
        axis_v = {1.0F, 0.0F, 0.0F};
    }
    else if (normal.z > 0.0F)
    {
        axis_u = {0.0F, -1.0F, 0.0F};
        axis_v = {-1.0F, 0.0F, 0.0F};
    }
    else if (normal.y > 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {-1.0F, 0.0F, 0.0F};
    }
    else
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {1.0F, 0.0F, 0.0F};
    }

    const world::Vec3 block_center{static_cast<float>(player.target->x) + 0.5F,
                                   static_cast<float>(player.target->y) + 0.5F,
                                   static_cast<float>(player.target->z) + 0.5F};
    const auto append_quad = [&](SDL_Texture *texture, EntityBlend blend, float face_offset,
                                 float u0, float u1, const std::array<float, 4> &color) {
        if (texture == nullptr)
            return;
        const world::Vec3 center{block_center.x + normal.x * face_offset,
                                 block_center.y + normal.y * face_offset,
                                 block_center.z + normal.z * face_offset};
        const auto point = [&](float u, float v) {
            return world::Vec3{center.x + axis_u.x * u + axis_v.x * v,
                               center.y + axis_u.y * u + axis_v.y * v,
                               center.z + axis_u.z * u + axis_v.z * v};
        };
        const std::array positions{point(-0.5F, -0.5F), point(0.5F, -0.5F), point(0.5F, 0.5F),
                                   point(-0.5F, 0.5F)};
        const std::array<std::array<float, 2>, 4> uv{
            {{u0, 1.0F}, {u1, 1.0F}, {u1, 0.0F}, {u0, 0.0F}}};
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        EntitySpan span;
        span.texture = texture;
        span.blend = blend;
        span.first = static_cast<std::uint32_t>(batch.vertices.size());
        for (const int source_index : order)
        {
            const auto index = static_cast<std::size_t>(source_index);
            const world::Vec3 &position = positions[index];
            EntityVertex vertex;
            vertex.x = position.x;
            vertex.y = position.y;
            vertex.z = position.z;
            vertex.u = uv[index][0];
            vertex.v = uv[index][1];
            vertex.red = color[0];
            vertex.green = color[1];
            vertex.blue = color[2];
            vertex.alpha = color[3];
            batch.vertices.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
        batch.spans.push_back(span);
    };

    if (player.break_progress > 0.1F)
    {
        const int stage = std::clamp(static_cast<int>(player.break_progress * 4.0F), 0, 3);
        const float u0 = static_cast<float>(stage) * 0.25F;
        append_quad(block_cracks_, EntityBlend::multiply, 0.503F, u0, u0 + 0.25F,
                    {1.0F, 1.0F, 1.0F, 1.0F});
    }

    const float scroll = std::fmod(std::max(0.0F, elapsed_seconds) / 5.0F, 1.0F);
    const float pulse = 0.88F + std::sin(scroll * 6.28318530718F) * 0.12F;
    append_quad(block_selector_, EntityBlend::alpha, 0.51F, 0.0F, 1.0F,
                {pulse, pulse, pulse, 1.0F});
}

void WorldPreviewRenderer::append_door_models(EntityBatch &batch, const world::PlayerState &player,
                                              const world::SkyState &sky)
{
    if (visible_doors_ == nullptr)
        return;

    const auto model_for = [&](std::uint8_t model_name) -> StaticModelAsset * {
        switch (model_name)
        {
        case 1U:
            return door_wood_.get();
        case 2U:
            return door_iron_.get();
        case 3U:
            return door_diamond_.get();
        case 4U:
            return door_tech_.get();
        default:
            return nullptr;
        }
    };
    const auto wrap_uv = [](float coordinate) {
        float wrapped = coordinate - std::floor(coordinate);
        if (coordinate > 0.0F && std::abs(wrapped) < 0.00001F)
            wrapped = 1.0F;
        return wrapped;
    };
    constexpr float maximum_distance_squared = 48.0F * 48.0F;
    constexpr float color_scale = 1.0F / 255.0F;
    for (const auto &[location, model_name] : *visible_doors_)
    {
        const int x = location[0];
        const int y = location[1];
        const int z = location[2];
        if (!region_.contains(x, y, z))
            continue;
        const float dx = static_cast<float>(x) + 0.5F - player.position.x;
        const float dz = static_cast<float>(z) + 0.5F - player.position.z;
        if (dx * dx + dz * dz > maximum_distance_squared)
            continue;

        const world::BlockType block = region_.block(x, y, z);
        const std::optional<world::DoorModelTransform> transform =
            world::door_model_transform(block);
        StaticModelAsset *model = model_for(model_name);
        if (!transform.has_value() || model == nullptr || !model->loaded())
            continue;
        // WorldInfo.SetDoor must agree with the voxel family. Refuse corrupt
        // save data rather than silently drawing a wood model over a strong
        // door (or vice versa).
        if ((model_name == 1U) == world::is_strong_door(block))
            continue;

        bool sky_clear = true;
        for (int sample_y = y + 2; sample_y < world::world_min_y + world::chunk_height; ++sample_y)
            if (region_.contains(x, sample_y, z) && world::occludes(region_.block(x, sample_y, z)))
            {
                sky_clear = false;
                break;
            }
        const float sky_light = sky_clear ? 1.0F : 0.2F;
        const float torch_light = local_emissive_light(region_, {static_cast<float>(x) + 0.5F,
                                                                 static_cast<float>(y) + 1.0F,
                                                                 static_cast<float>(z) + 0.5F});
        const std::array<float, 3> classic_light{
            std::clamp((sky.ambient.red * 0.55F + sky.direct.red * 0.45F) *
                               (0.25F + sky_light * 0.75F) +
                           torch_light * 0.70F,
                       0.12F, 1.0F),
            std::clamp((sky.ambient.green * 0.55F + sky.direct.green * 0.45F) *
                               (0.25F + sky_light * 0.75F) +
                           torch_light * 0.42F,
                       0.12F, 1.0F),
            std::clamp((sky.ambient.blue * 0.55F + sky.direct.blue * 0.45F) *
                               (0.25F + sky_light * 0.75F) +
                           torch_light * 0.22F,
                       0.12F, 1.0F)};
        const float yaw_cosine = std::cos(transform->yaw_radians);
        const float yaw_sine = std::sin(transform->yaw_radians);
        const world::Vec3 base{static_cast<float>(x) + transform->offset_x,
                               static_cast<float>(y) + transform->offset_y,
                               static_cast<float>(z) + transform->offset_z};
        for (const StaticModelPart &part : model->parts())
        {
            EntitySpan span;
            span.texture = part.texture;
            span.first = static_cast<std::uint32_t>(batch.vertices.size());
            for (const std::uint32_t index : part.indices)
            {
                const StaticModelVertex &source = part.vertices[index];
                const auto &matrix = part.transform;
                const std::array<float, 3> point{
                    source.position[0] * matrix[0] + source.position[1] * matrix[4] +
                        source.position[2] * matrix[8] + matrix[12],
                    source.position[0] * matrix[1] + source.position[1] * matrix[5] +
                        source.position[2] * matrix[9] + matrix[13],
                    source.position[0] * matrix[2] + source.position[1] * matrix[6] +
                        source.position[2] * matrix[10] + matrix[14]};
                std::array<float, 3> normal{
                    source.normal[0] * matrix[0] + source.normal[1] * matrix[4] +
                        source.normal[2] * matrix[8],
                    source.normal[0] * matrix[1] + source.normal[1] * matrix[5] +
                        source.normal[2] * matrix[9],
                    source.normal[0] * matrix[2] + source.normal[1] * matrix[6] +
                        source.normal[2] * matrix[10]};
                const float normal_length = std::sqrt(
                    normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (normal_length > 0.00001F)
                    for (float &component : normal)
                        component /= normal_length;
                const float rotated_x = point[0] * yaw_cosine + point[2] * yaw_sine;
                const float rotated_z = -point[0] * yaw_sine + point[2] * yaw_cosine;
                const float normal_x = normal[0] * yaw_cosine + normal[2] * yaw_sine;
                const float normal_z = -normal[0] * yaw_sine + normal[2] * yaw_cosine;
                EntityVertex vertex;
                vertex.x = base.x + rotated_x;
                vertex.y = base.y + point[1];
                vertex.z = base.z + rotated_z;
                vertex.u = wrap_uv(source.uv[0]);
                vertex.v = wrap_uv(source.uv[1]);
                vertex.red = part.diffuse[0] * source.color[0] * color_scale * classic_light[0];
                vertex.green = part.diffuse[1] * source.color[1] * color_scale * classic_light[1];
                vertex.blue = part.diffuse[2] * source.color[2] * color_scale * classic_light[2];
                vertex.normal_x = normal_x;
                vertex.normal_y = normal[1];
                vertex.normal_z = normal_z;
                vertex.sky_light = sky_light;
                vertex.torch_light = torch_light;
                batch.vertices.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
            if (span.count > 0U)
                batch.spans.push_back(span);
        }
    }
}

void WorldPreviewRenderer::append_pickup_models(EntityBatch &batch)
{
    for (const BlockPickup &pickup : block_pickups_)
    {
        if (pickup.block != world::BlockType::empty)
            continue;
        if (!pickup.flying_to_player && pickup.time_left <= 5.0F &&
            std::fmod(pickup.age * 8.0F, 1.0F) >= 0.5F)
            continue;
        const PickupModelChoice choice = pickup_model_choice(pickup.item);
        if (choice.model == nullptr || !choice.model->loaded())
            continue;
        world::Vec3 center = pickup.position;
        if (!pickup.flying_to_player)
            center.y += std::sin(pickup.bounce_phase) * 0.1F + 0.1F;
        const float pitch_cosine = std::cos(choice.pitch);
        const float pitch_sine = std::sin(choice.pitch);
        // PickupEntity rotates its display child around Vector3.Down at 1 rad/s.
        const float spin_cosine = std::cos(-pickup.rotation);
        const float spin_sine = std::sin(-pickup.rotation);
        const auto wrap_uv = [](float coordinate) {
            float wrapped = coordinate - std::floor(coordinate);
            if (coordinate > 0.0F && std::abs(wrapped) < 0.00001F)
                wrapped = 1.0F;
            return wrapped;
        };
        for (const StaticModelPart &part : choice.model->parts())
        {
            EntitySpan span;
            span.texture = part.texture;
            span.first = static_cast<std::uint32_t>(batch.vertices.size());
            for (const std::uint32_t index : part.indices)
            {
                const StaticModelVertex &source = part.vertices[index];
                const auto &m = part.transform;
                std::array<float, 3> point{source.position[0] * m[0] + source.position[1] * m[4] +
                                               source.position[2] * m[8] + m[12],
                                           source.position[0] * m[1] + source.position[1] * m[5] +
                                               source.position[2] * m[9] + m[13],
                                           source.position[0] * m[2] + source.position[1] * m[6] +
                                               source.position[2] * m[10] + m[14]};
                for (float &component : point)
                    component *= choice.scale;
                const float pitched_y = point[1] * pitch_cosine - point[2] * pitch_sine;
                const float pitched_z = point[1] * pitch_sine + point[2] * pitch_cosine;
                point[1] = pitched_y;
                point[2] = pitched_z;
                const float spun_x = point[0] * spin_cosine + point[2] * spin_sine;
                const float spun_z = -point[0] * spin_sine + point[2] * spin_cosine;
                const std::array<float, 3> part_tint =
                    (part.material_flags & 2U) != 0U
                        ? choice.secondary
                        : ((part.material_flags & 1U) != 0U
                               ? choice.primary
                               : std::array<float, 3>{1.0F, 1.0F, 1.0F});
                const float light = std::clamp(
                    0.68F + source.normal[1] * 0.18F - source.normal[2] * 0.10F, 0.40F, 1.0F);
                constexpr float color_scale = 1.0F / 255.0F;
                EntityVertex vertex;
                vertex.x = center.x + spun_x;
                vertex.y = center.y + point[1];
                vertex.z = center.z + spun_z;
                vertex.u = wrap_uv(source.uv[0]);
                vertex.v = wrap_uv(source.uv[1]);
                vertex.red = part.diffuse[0] * source.color[0] * color_scale * part_tint[0] * light;
                vertex.green =
                    part.diffuse[1] * source.color[1] * color_scale * part_tint[1] * light;
                vertex.blue =
                    part.diffuse[2] * source.color[2] * color_scale * part_tint[2] * light;
                batch.vertices.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
            if (span.count > 0U)
                batch.spans.push_back(span);
        }
    }
}

void WorldPreviewRenderer::draw_mining_effects(const ui::UiMetrics &metrics,
                                               const world::PlayerState &player,
                                               float vertical_fov_degrees, float elapsed_seconds)
{
    constexpr float pi = 3.14159265358979323846F;
    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
    const float focal_length = height * 0.5F / std::tan(vertical_fov * 0.5F);
    const float sine_yaw = std::sin(player.yaw);
    const float cosine_yaw = std::cos(player.yaw);
    const float sine_pitch = std::sin(player.pitch);
    const float cosine_pitch = std::cos(player.pitch);
    const world::Vec3 eye{player.position.x,
                          player.position.y + world::PlayerSimulation::eye_height,
                          player.position.z};
    struct ScreenPoint final
    {
        SDL_FPoint point{};
        float depth{};
    };
    const auto project = [&](world::Vec3 point) -> std::optional<ScreenPoint> {
        const float dx = point.x - eye.x;
        const float dy = point.y - eye.y;
        const float dz = point.z - eye.z;
        const float camera_x = dx * cosine_yaw + dz * sine_yaw;
        const float level_depth = dx * sine_yaw - dz * cosine_yaw;
        const float camera_y = dy * cosine_pitch - level_depth * sine_pitch;
        const float camera_z = level_depth * cosine_pitch + dy * sine_pitch;
        if (camera_z <= 0.06F)
            return std::nullopt;
        return ScreenPoint{{width * 0.5F + camera_x * focal_length / camera_z,
                            height * 0.5F - camera_y * focal_length / camera_z},
                           camera_z};
    };
    const auto unobstructed_margin = [&](world::Vec3 point, float margin) {
        world::Vec3 direction{point.x - eye.x, point.y - eye.y, point.z - eye.z};
        const float distance = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                         direction.z * direction.z);
        if (distance <= margin + 0.02F)
            return true;
        direction.x /= distance;
        direction.y /= distance;
        direction.z /= distance;
        return !world::PlayerSimulation::raycast(region_, eye, direction,
                                                 std::max(0.0F, distance - margin))
                    .has_value();
    };
    const auto unobstructed = [&](world::Vec3 point) { return unobstructed_margin(point, 0.10F); };

    std::vector<SDL_Vertex> rock_particle_vertices;
    std::vector<SDL_Vertex> spark_particle_vertices;
    std::vector<SDL_Vertex> smoke_particle_vertices;
    std::vector<SDL_Vertex> torch_fire_vertices;
    std::vector<SDL_Vertex> torch_smoke_vertices;
    rock_particle_vertices.reserve(mining_particles_.size() * 6U);
    spark_particle_vertices.reserve(mining_particles_.size() * 6U);
    smoke_particle_vertices.reserve(mining_particles_.size() * 24U);
    const auto world_texture_quad = [&](SDL_Texture *texture, world::Vec3 center, float world_size,
                                        float rotation, SDL_FColor color,
                                        std::vector<SDL_Vertex> &destination,
                                        float obstruction_margin = 0.10F) {
        if (texture == nullptr)
            return;
        const float radians = rotation * pi / 180.0F;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const world::Vec3 camera_right{cosine_yaw, 0.0F, sine_yaw};
        const world::Vec3 camera_up{-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch};
        const auto billboard_point = [&](float local_x, float local_y) {
            const float right_amount = (local_x * cosine - local_y * sine) * world_size;
            const float up_amount = -(local_x * sine + local_y * cosine) * world_size;
            return world::Vec3{center.x + camera_right.x * right_amount + camera_up.x * up_amount,
                               center.y + camera_right.y * right_amount + camera_up.y * up_amount,
                               center.z + camera_right.z * right_amount + camera_up.z * up_amount};
        };
        // Smoke grows to several metres in the original emitter. Subdivide large billboards so
        // terrain rays clip individual cells instead of letting one clear center draw over a wall.
        const int subdivisions = world_size >= 0.75F ? 4 : (world_size >= 0.3F ? 2 : 1);
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        for (int cell_y = 0; cell_y < subdivisions; ++cell_y)
        {
            for (int cell_x = 0; cell_x < subdivisions; ++cell_x)
            {
                const float x0 = -1.0F + 2.0F * static_cast<float>(cell_x) / subdivisions;
                const float x1 = -1.0F + 2.0F * static_cast<float>(cell_x + 1) / subdivisions;
                const float y0 = -1.0F + 2.0F * static_cast<float>(cell_y) / subdivisions;
                const float y1 = -1.0F + 2.0F * static_cast<float>(cell_y + 1) / subdivisions;
                if (!unobstructed_margin(billboard_point((x0 + x1) * 0.5F, (y0 + y1) * 0.5F),
                                         obstruction_margin))
                    continue;
                const std::array<world::Vec3, 4> corners{
                    billboard_point(x0, y1), billboard_point(x1, y1), billboard_point(x1, y0),
                    billboard_point(x0, y0)};
                std::array<std::optional<ScreenPoint>, 4> screen{};
                std::transform(corners.begin(), corners.end(), screen.begin(), project);
                if (std::any_of(screen.begin(), screen.end(),
                                [](const auto &point) { return !point.has_value(); }))
                    continue;
                const std::array<SDL_FPoint, 4> uv{
                    SDL_FPoint{(x0 + 1.0F) * 0.5F, (y1 + 1.0F) * 0.5F},
                    SDL_FPoint{(x1 + 1.0F) * 0.5F, (y1 + 1.0F) * 0.5F},
                    SDL_FPoint{(x1 + 1.0F) * 0.5F, (y0 + 1.0F) * 0.5F},
                    SDL_FPoint{(x0 + 1.0F) * 0.5F, (y0 + 1.0F) * 0.5F}};
                std::array<SDL_Vertex, 6> vertices{};
                for (std::size_t index = 0; index < vertices.size(); ++index)
                {
                    const auto corner = static_cast<std::size_t>(order[index]);
                    vertices[index] = {screen[corner]->point, color, uv[corner]};
                }
                destination.insert(destination.end(), vertices.begin(), vertices.end());
            }
        }
    };

    const auto draw_pickup_cube = [&](const BlockPickup &pickup, world::Vec3 center) {
        if (pickup.block == world::BlockType::empty || atlas_ == nullptr || !unobstructed(center))
            return;
        constexpr float half_size = 0.10F;
        const float cosine = std::cos(pickup.rotation);
        const float sine = std::sin(pickup.rotation);
        const auto rotate_y = [&](world::Vec3 local) {
            return world::Vec3{center.x + local.x * cosine + local.z * sine, center.y + local.y,
                               center.z - local.x * sine + local.z * cosine};
        };
        std::array<world::Vec3, 8> corners{};
        std::array<std::optional<ScreenPoint>, 8> projected{};
        for (std::size_t index = 0U; index < corners.size(); ++index)
        {
            const world::Vec3 local{(index & 1U) != 0U ? half_size : -half_size,
                                    (index & 2U) != 0U ? half_size : -half_size,
                                    (index & 4U) != 0U ? half_size : -half_size};
            corners[index] = rotate_y(local);
            projected[index] = project(corners[index]);
        }
        struct CubeFace final
        {
            std::array<std::size_t, 4> corners{};
            world::Vec3 local_normal{};
            world::FaceDirection direction{};
            float light{};
            float depth{};
        };
        std::array<CubeFace, 6> faces{{
            {{{1U, 3U, 7U, 5U}}, {1.0F, 0.0F, 0.0F}, world::FaceDirection::positive_x, 0.88F},
            {{{4U, 6U, 2U, 0U}}, {-1.0F, 0.0F, 0.0F}, world::FaceDirection::negative_x, 0.76F},
            {{{2U, 6U, 7U, 3U}}, {0.0F, 1.0F, 0.0F}, world::FaceDirection::positive_y, 1.0F},
            {{{4U, 0U, 1U, 5U}}, {0.0F, -1.0F, 0.0F}, world::FaceDirection::negative_y, 0.62F},
            {{{5U, 7U, 6U, 4U}}, {0.0F, 0.0F, 1.0F}, world::FaceDirection::positive_z, 0.84F},
            {{{0U, 2U, 3U, 1U}}, {0.0F, 0.0F, -1.0F}, world::FaceDirection::negative_z, 0.72F},
        }};
        std::vector<CubeFace> visible_faces;
        visible_faces.reserve(3U);
        for (auto face : faces)
        {
            const world::Vec3 normal{face.local_normal.x * cosine + face.local_normal.z * sine,
                                     face.local_normal.y,
                                     -face.local_normal.x * sine + face.local_normal.z * cosine};
            const world::Vec3 to_eye{eye.x - center.x, eye.y - center.y, eye.z - center.z};
            if (normal.x * to_eye.x + normal.y * to_eye.y + normal.z * to_eye.z <= 0.0F)
                continue;
            bool clipped{};
            for (const std::size_t corner : face.corners)
            {
                if (!projected[corner].has_value())
                {
                    clipped = true;
                    break;
                }
                face.depth += projected[corner]->depth * 0.25F;
            }
            world::Vec3 face_center{};
            for (const std::size_t corner : face.corners)
            {
                face_center.x += corners[corner].x * 0.25F;
                face_center.y += corners[corner].y * 0.25F;
                face_center.z += corners[corner].z * 0.25F;
            }
            if (!clipped && unobstructed(face_center))
                visible_faces.push_back(face);
        }
        std::sort(
            visible_faces.begin(), visible_faces.end(),
            [](const CubeFace &left, const CubeFace &right) { return left.depth > right.depth; });
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        for (const CubeFace &face : visible_faces)
        {
            const std::uint8_t tile = world::atlas_tile(pickup.block, face.direction);
            const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
            const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
            const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
            const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
            const auto uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
            std::array<SDL_Vertex, 6> vertices{};
            for (std::size_t index = 0U; index < vertices.size(); ++index)
            {
                const std::size_t face_corner = static_cast<std::size_t>(order[index]);
                const std::size_t cube_corner = face.corners[face_corner];
                vertices[index] = {projected[cube_corner]->point,
                                   {face.light, face.light, face.light, 1.0F},
                                   {uv[face_corner][0], uv[face_corner][1]}};
            }
            SDL_RenderGeometry(renderer_, atlas_, vertices.data(),
                               static_cast<int>(vertices.size()), nullptr, 0);
        }
    };

    const auto draw_pickup_item = [&](const BlockPickup &pickup, world::Vec3 center) {
        if (pickup.block != world::BlockType::empty || !unobstructed(center))
            return;

        const PickupModelChoice choice = pickup_model_choice(pickup.item);
        StaticModelAsset *model = choice.model;
        const float model_scale = choice.scale;
        const float pickup_pitch = choice.pitch;
        const std::array<float, 3> primary_tint = choice.primary;
        const std::array<float, 3> secondary_tint = choice.secondary;
        if (model != nullptr && model->loaded())
        {
            struct ModelTriangle final
            {
                float depth{};
                SDL_Texture *texture{};
                std::array<SDL_Vertex, 3> vertices{};
            };
            const float pitch_cosine = std::cos(pickup_pitch);
            const float pitch_sine = std::sin(pickup_pitch);
            // PickupEntity rotates its display child around Vector3.Down at one radian/second.
            const float spin_cosine = std::cos(-pickup.rotation);
            const float spin_sine = std::sin(-pickup.rotation);
            const auto static_transform = [](const std::array<float, 3> &point,
                                             const StaticModelMatrix &matrix) {
                return std::array<float, 3>{
                    point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8] + matrix[12],
                    point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9] + matrix[13],
                    point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] +
                        matrix[14]};
            };
            const auto pickup_transform = [&](std::array<float, 3> point) {
                for (float &component : point)
                    component *= model_scale;
                const float pitched_y = point[1] * pitch_cosine - point[2] * pitch_sine;
                const float pitched_z = point[1] * pitch_sine + point[2] * pitch_cosine;
                point[1] = pitched_y;
                point[2] = pitched_z;
                const float spun_x = point[0] * spin_cosine + point[2] * spin_sine;
                const float spun_z = -point[0] * spin_sine + point[2] * spin_cosine;
                return world::Vec3{center.x + spun_x, center.y + point[1], center.z + spun_z};
            };
            const auto wrap_uv = [](float coordinate) {
                float wrapped = coordinate - std::floor(coordinate);
                if (coordinate > 0.0F && std::abs(wrapped) < 0.00001F)
                    wrapped = 1.0F;
                return wrapped;
            };
            std::vector<ModelTriangle> triangles;
            for (const auto &part : model->parts())
            {
                triangles.reserve(triangles.size() + part.indices.size() / 3U);
                for (std::size_t triangle = 0U; triangle < part.indices.size(); triangle += 3U)
                {
                    ModelTriangle output;
                    output.texture = part.texture;
                    std::array<world::Vec3, 3> points{};
                    bool visible = true;
                    for (std::size_t corner = 0U; corner < 3U; ++corner)
                    {
                        const auto &source = part.vertices[part.indices[triangle + corner]];
                        points[corner] =
                            pickup_transform(static_transform(source.position, part.transform));
                        const auto screen_point = project(points[corner]);
                        visible = visible && screen_point.has_value();
                        if (!screen_point.has_value())
                            continue;
                        const std::array<float, 3> part_tint =
                            (part.material_flags & 2U) != 0U
                                ? secondary_tint
                                : ((part.material_flags & 1U) != 0U
                                       ? primary_tint
                                       : std::array<float, 3>{1.0F, 1.0F, 1.0F});
                        const float light =
                            std::clamp(0.68F + source.normal[1] * 0.18F - source.normal[2] * 0.10F,
                                       0.40F, 1.0F);
                        constexpr float color_scale = 1.0F / 255.0F;
                        output.vertices[corner] = {
                            screen_point->point,
                            {part.diffuse[0] * source.color[0] * color_scale * part_tint[0] * light,
                             part.diffuse[1] * source.color[1] * color_scale * part_tint[1] * light,
                             part.diffuse[2] * source.color[2] * color_scale * part_tint[2] * light,
                             source.color[3] * color_scale},
                            {wrap_uv(source.uv[0]), wrap_uv(source.uv[1])}};
                        output.depth += screen_point->depth;
                    }
                    if (!visible)
                        continue;
                    const world::Vec3 triangle_center{
                        (points[0].x + points[1].x + points[2].x) / 3.0F,
                        (points[0].y + points[1].y + points[2].y) / 3.0F,
                        (points[0].z + points[1].z + points[2].z) / 3.0F};
                    if (!unobstructed(triangle_center))
                        continue;
                    const SDL_FPoint a = output.vertices[0].position;
                    const SDL_FPoint b = output.vertices[1].position;
                    const SDL_FPoint c = output.vertices[2].position;
                    const float signed_area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    if (signed_area < -0.001F)
                        triangles.push_back(output);
                }
            }
            std::stable_sort(triangles.begin(), triangles.end(),
                             [](const ModelTriangle &left, const ModelTriangle &right) {
                                 return left.depth > right.depth;
                             });
            std::vector<SDL_Vertex> run;
            run.reserve(triangles.size() * 3U);
            SDL_Texture *run_texture{};
            bool run_started{};
            const auto flush_run = [&]() {
                if (!run.empty())
                    SDL_RenderGeometry(renderer_, run_texture, run.data(),
                                       static_cast<int>(run.size()), nullptr, 0);
                run.clear();
            };
            for (const auto &triangle : triangles)
            {
                if (run_started && triangle.texture != run_texture)
                    flush_run();
                run_texture = triangle.texture;
                run_started = true;
                run.insert(run.end(), triangle.vertices.begin(), triangle.vertices.end());
            }
            flush_run();
            return;
        }

        // Items whose original pickup model has not been exported yet retain a small neutral
        // marker instead of silently disappearing.
        const auto screen = project(center);
        if (!screen.has_value())
            return;
        const float radius = std::clamp(focal_length * 0.15F / screen->depth, 2.0F, 54.0F);
        SDL_FColor color{0.78F, 0.79F, 0.76F, 1.0F};
        switch (pickup.item)
        {
        case InventoryItemId::bullets:
            color = {0.82F, 0.59F, 0.19F, 1.0F};
            break;
        case InventoryItemId::compass:
            color = {0.76F, 0.61F, 0.18F, 1.0F};
            break;
        case InventoryItemId::pistol:
            color = {0.39F, 0.42F, 0.43F, 1.0F};
            break;
        case InventoryItemId::knife:
            color = {0.76F, 0.79F, 0.80F, 1.0F};
            break;
        case InventoryItemId::coal:
            color = {0.16F, 0.16F, 0.17F, 1.0F};
            break;
        case InventoryItemId::copper_ore:
            color = {0.71F, 0.38F, 0.20F, 1.0F};
            break;
        case InventoryItemId::iron_ore:
            color = {0.63F, 0.55F, 0.48F, 1.0F};
            break;
        case InventoryItemId::gold_ore:
            color = {0.90F, 0.70F, 0.16F, 1.0F};
            break;
        case InventoryItemId::diamond:
            color = {0.24F, 0.86F, 0.91F, 1.0F};
            break;
        default:
            break;
        }
        const float spin = pickup.rotation;
        const float cosine = std::cos(spin);
        const float sine = std::sin(spin);
        constexpr std::array<SDL_FPoint, 4> local{SDL_FPoint{0.0F, -1.0F}, SDL_FPoint{0.72F, 0.0F},
                                                  SDL_FPoint{0.0F, 1.0F}, SDL_FPoint{-0.72F, 0.0F}};
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        std::array<SDL_Vertex, 6> vertices{};
        for (std::size_t index = 0; index < vertices.size(); ++index)
        {
            const SDL_FPoint point = local[static_cast<std::size_t>(order[index])];
            vertices[index] = {{screen->point.x + (point.x * cosine - point.y * sine) * radius,
                                screen->point.y + (point.x * sine + point.y * cosine) * radius},
                               color,
                               {0.0F, 0.0F}};
        }
        SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                           nullptr, 0);
    };

    for (const auto &pickup : block_pickups_)
    {
        if (!pickup.flying_to_player && pickup.time_left <= 5.0F &&
            std::fmod(pickup.age * 8.0F, 1.0F) >= 0.5F)
            continue;
        world::Vec3 display = pickup.position;
        if (!pickup.flying_to_player)
            display.y += std::sin(pickup.bounce_phase) * 0.1F + 0.1F;
        draw_pickup_cube(pickup, display);
        draw_pickup_item(pickup, display);
    }
    for (const auto &particle : mining_particles_)
    {
        const auto screen = project(particle.position);
        if (!screen.has_value())
            continue;
        const float remaining = std::clamp(1.0F - particle.age / particle.lifetime, 0.0F, 1.0F);
        if (particle.kind == ParticleKind::rock)
        {
            world_texture_quad(mining_rocks_, particle.position, particle.size, particle.rotation,
                               {1.0F, 0.88F, 0.66F, remaining}, rock_particle_vertices);
        }
        else if (particle.kind == ParticleKind::spark)
        {
            world_texture_quad(mining_sparks_, particle.position, particle.size, particle.rotation,
                               {1.0F, 0.82F, 0.42F, remaining}, spark_particle_vertices);
        }
        else if (particle.kind == ParticleKind::torch_fire)
        {
            const float progress = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
            const float grown_size = particle.size + (particle.end_size - particle.size) * progress;
            // The flame lives inside the torch's own cell: the occlusion ray
            // needs enough margin not to hit the torch block itself.
            world_texture_quad(torch_fire_texture_, particle.position, grown_size,
                               particle.rotation, {1.0F, 1.0F, 1.0F, remaining},
                               torch_fire_vertices, 0.9F);
        }
        else if (particle.kind == ParticleKind::torch_smoke)
        {
            const float progress = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
            const float grown_size = particle.size + (particle.end_size - particle.size) * progress;
            world_texture_quad(torch_smoke_texture_, particle.position, grown_size,
                               particle.rotation, {0.62F, 0.62F, 0.62F, remaining * 0.8F},
                               torch_smoke_vertices, 0.9F);
        }
        else if (particle.kind == ParticleKind::mining_dust ||
                 particle.kind == ParticleKind::impact_smoke)
        {
            const float progress = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
            float grown_size = particle.size + (particle.end_size - particle.size) * progress;
            const float distance_weight = std::clamp((screen->depth - 1.0F) / 7.0F, 0.0F, 1.0F);
            const bool mining_dust = particle.kind == ParticleKind::mining_dust;
            grown_size *= std::lerp(mining_dust ? 0.78F : 0.74F, 1.0F, distance_weight);
            const float visibility = mining_dust ? std::lerp(0.62F, 0.84F, distance_weight)
                                                 : std::lerp(0.52F, 0.72F, distance_weight);
            const float fade = remaining * (0.36F + 0.64F * remaining);
            world_texture_quad(mining_smoke_, particle.position, grown_size, particle.rotation,
                               {particle.tint[0], particle.tint[1], particle.tint[2],
                                fade * visibility * particle.opacity},
                               smoke_particle_vertices);
        }
        else
        {
            const float progress = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
            const float grown_size = particle.size + (particle.end_size - particle.size) * progress;
            world_texture_quad(mining_smoke_, particle.position, grown_size, particle.rotation,
                               {particle.tint[0], particle.tint[1], particle.tint[2], remaining},
                               smoke_particle_vertices);
        }
    }
    const auto draw_particle_batch = [&](SDL_Texture *texture,
                                         const std::vector<SDL_Vertex> &vertices) {
        if (texture != nullptr && !vertices.empty())
            SDL_RenderGeometry(renderer_, texture, vertices.data(),
                               static_cast<int>(vertices.size()), nullptr, 0);
    };
    draw_particle_batch(mining_rocks_, rock_particle_vertices);
    draw_particle_batch(mining_sparks_, spark_particle_vertices);
    draw_particle_batch(mining_smoke_, smoke_particle_vertices);
    draw_particle_batch(torch_smoke_texture_, torch_smoke_vertices);
    draw_particle_batch(torch_fire_texture_, torch_fire_vertices);
    if (SDL_getenv("OPENMINERZ_TORCH_DEBUG") != nullptr)
    {
        SDL_Log("torch debug: cache=%zu particles=%zu fire_tex=%p fire_verts=%zu",
                torch_cache_.size(), mining_particles_.size(),
                static_cast<void *>(torch_fire_texture_), torch_fire_vertices.size());
        if (!mining_particles_.empty())
        {
            const MiningParticle &first = mining_particles_.front();
            SDL_Log("torch debug: first particle kind=%d pos=(%.2f %.2f %.2f) age=%.2f "
                    "life=%.2f size=%.2f",
                    static_cast<int>(first.kind), first.position.x, first.position.y,
                    first.position.z, first.age, first.lifetime, first.size);
        }
    }
    (void)elapsed_seconds;
}

void WorldPreviewRenderer::draw_target_outline(const ui::UiMetrics &metrics,
                                               const world::PlayerState &player,
                                               float vertical_fov_degrees, float elapsed_seconds)
{
    if (!player.target.has_value())
    {
        return;
    }
    constexpr float pi = 3.14159265358979323846F;
    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
    const float focal_length = height * 0.5F / std::tan(vertical_fov * 0.5F);
    const float sine_yaw = std::sin(player.yaw);
    const float cosine_yaw = std::cos(player.yaw);
    const float sine_pitch = std::sin(player.pitch);
    const float cosine_pitch = std::cos(player.pitch);
    const world::Vec3 eye{player.position.x,
                          player.position.y + world::PlayerSimulation::eye_height,
                          player.position.z};
    const auto project = [&](world::Vec3 point) -> std::optional<SDL_FPoint> {
        const float dx = point.x - eye.x;
        const float dy = point.y - eye.y;
        const float dz = point.z - eye.z;
        const float camera_x = dx * cosine_yaw + dz * sine_yaw;
        const float level_depth = dx * sine_yaw - dz * cosine_yaw;
        const float camera_y = dy * cosine_pitch - level_depth * sine_pitch;
        const float camera_z = level_depth * cosine_pitch + dy * sine_pitch;
        if (camera_z <= 0.06F)
        {
            return std::nullopt;
        }
        return SDL_FPoint{width * 0.5F + camera_x * focal_length / camera_z,
                          height * 0.5F - camera_y * focal_length / camera_z};
    };
    const float bx = static_cast<float>(player.target->x);
    const float by = static_cast<float>(player.target->y);
    const float bz = static_cast<float>(player.target->z);
    const world::Vec3 center{bx + 0.5F, by + 0.5F, bz + 0.5F};

    if (block_cracks_ != nullptr && player.break_progress > 0.1F)
    {
        struct CrackFace final
        {
            world::Vec3 normal;
            std::array<world::Vec3, 4> corners;
        };
        constexpr float offset = 0.003F;
        const std::array<CrackFace, 6> crack_faces{{
            {{1.0F, 0.0F, 0.0F},
             {{{bx + 1.0F + offset, by, bz},
               {bx + 1.0F + offset, by + 1.0F, bz},
               {bx + 1.0F + offset, by + 1.0F, bz + 1.0F},
               {bx + 1.0F + offset, by, bz + 1.0F}}}},
            {{-1.0F, 0.0F, 0.0F},
             {{{bx - offset, by, bz + 1.0F},
               {bx - offset, by + 1.0F, bz + 1.0F},
               {bx - offset, by + 1.0F, bz},
               {bx - offset, by, bz}}}},
            {{0.0F, 0.0F, 1.0F},
             {{{bx + 1.0F, by, bz + 1.0F + offset},
               {bx + 1.0F, by + 1.0F, bz + 1.0F + offset},
               {bx, by + 1.0F, bz + 1.0F + offset},
               {bx, by, bz + 1.0F + offset}}}},
            {{0.0F, 0.0F, -1.0F},
             {{{bx, by, bz - offset},
               {bx, by + 1.0F, bz - offset},
               {bx + 1.0F, by + 1.0F, bz - offset},
               {bx + 1.0F, by, bz - offset}}}},
            {{0.0F, 1.0F, 0.0F},
             {{{bx, by + 1.0F + offset, bz + 1.0F},
               {bx + 1.0F, by + 1.0F + offset, bz + 1.0F},
               {bx + 1.0F, by + 1.0F + offset, bz},
               {bx, by + 1.0F + offset, bz}}}},
            {{0.0F, -1.0F, 0.0F},
             {{{bx, by - offset, bz},
               {bx + 1.0F, by - offset, bz},
               {bx + 1.0F, by - offset, bz + 1.0F},
               {bx, by - offset, bz + 1.0F}}}},
        }};
        const int stage = std::clamp(static_cast<int>(player.break_progress * 4.0F), 0, 3);
        const float u0 = static_cast<float>(stage) * 0.25F;
        const float u1 = u0 + 0.25F;
        constexpr std::array<SDL_FPoint, 4> uv{
            {{0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}}};
        constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        for (const auto &face : crack_faces)
        {
            if (face.normal.x != static_cast<float>(player.target->normal_x) ||
                face.normal.y != static_cast<float>(player.target->normal_y) ||
                face.normal.z != static_cast<float>(player.target->normal_z))
            {
                continue;
            }
            const world::Vec3 to_eye{eye.x - center.x, eye.y - center.y, eye.z - center.z};
            if (face.normal.x * to_eye.x + face.normal.y * to_eye.y + face.normal.z * to_eye.z <=
                0.0F)
            {
                continue;
            }
            std::array<std::optional<SDL_FPoint>, 4> points{};
            std::transform(face.corners.begin(), face.corners.end(), points.begin(), project);
            if (std::any_of(points.begin(), points.end(),
                            [](const auto &point) { return !point.has_value(); }))
            {
                continue;
            }
            std::array<SDL_Vertex, 6> vertices{};
            for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex)
            {
                const int corner = order[vertex];
                const float texture_u = u0 + uv[static_cast<std::size_t>(corner)].x * (u1 - u0);
                vertices[vertex] = {*points[static_cast<std::size_t>(corner)],
                                    {1.0F, 1.0F, 1.0F, 1.0F},
                                    {texture_u, uv[static_cast<std::size_t>(corner)].y}};
            }
            SDL_RenderGeometry(renderer_, block_cracks_, vertices.data(),
                               static_cast<int>(vertices.size()), nullptr, 0);
        }
    }

    // Vanilla's Selector is face-local rather than a full wire cube. It sits 0.51 from the
    // block center and scrolls two layers in opposite directions over five seconds.
    const world::Vec3 normal{static_cast<float>(player.target->normal_x),
                             static_cast<float>(player.target->normal_y),
                             static_cast<float>(player.target->normal_z)};
    if (normal.x == 0.0F && normal.y == 0.0F && normal.z == 0.0F)
    {
        return;
    }
    // Exact CreateWorld(forward, up) face bases from InGameHUD. The Selector
    // model lies in local XZ, so these are local Right and Backward. Keeping
    // the signs matters: otherwise the animated gold frame rotates/mirrors as
    // the ray crosses block faces.
    world::Vec3 axis_u{};
    world::Vec3 axis_v{};
    if (normal.x > 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {0.0F, 1.0F, 0.0F};
    }
    else if (normal.x < 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {0.0F, -1.0F, 0.0F};
    }
    else if (normal.z < 0.0F)
    {
        axis_u = {0.0F, -1.0F, 0.0F};
        axis_v = {1.0F, 0.0F, 0.0F};
    }
    else if (normal.z > 0.0F)
    {
        axis_u = {0.0F, -1.0F, 0.0F};
        axis_v = {-1.0F, 0.0F, 0.0F};
    }
    else if (normal.y > 0.0F)
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {-1.0F, 0.0F, 0.0F};
    }
    else
    {
        axis_u = {0.0F, 0.0F, 1.0F};
        axis_v = {1.0F, 0.0F, 0.0F};
    }
    const world::Vec3 selector_center{center.x + normal.x * 0.51F, center.y + normal.y * 0.51F,
                                      center.z + normal.z * 0.51F};
    const auto corner = [&](float u, float v) {
        return world::Vec3{selector_center.x + axis_u.x * u + axis_v.x * v,
                           selector_center.y + axis_u.y * u + axis_v.y * v,
                           selector_center.z + axis_u.z * u + axis_v.z * v};
    };
    const std::array<world::Vec3, 4> selector_world{
        {corner(-0.5F, -0.5F), corner(0.5F, -0.5F), corner(0.5F, 0.5F), corner(-0.5F, 0.5F)}};
    std::array<std::optional<SDL_FPoint>, 4> selector{};
    std::transform(selector_world.begin(), selector_world.end(), selector.begin(), project);
    if (std::any_of(selector.begin(), selector.end(),
                    [](const auto &point) { return !point.has_value(); }))
    {
        return;
    }
    const float scroll = std::fmod(std::max(0.0F, elapsed_seconds) / 5.0F, 1.0F);
    if (block_selector_ != nullptr)
    {
        // selector_0 is already the complete gold frame used by the original
        // Selector model. Splitting the projected face at a moving UV seam made
        // that frame duplicate into stripes. Preserve the source texture once,
        // while the two original counter-scrolling meshes are represented by a
        // restrained five-second luminance pulse.
        const float pulse = 0.88F + std::sin(scroll * 6.28318530718F) * 0.12F;
        const SDL_FColor tint{pulse, pulse, pulse, 1.0F};
        const std::array<SDL_Vertex, 6> vertices{{
            {*selector[0], tint, {0.0F, 1.0F}},
            {*selector[1], tint, {1.0F, 1.0F}},
            {*selector[2], tint, {1.0F, 0.0F}},
            {*selector[0], tint, {0.0F, 1.0F}},
            {*selector[2], tint, {1.0F, 0.0F}},
            {*selector[3], tint, {0.0F, 0.0F}},
        }};
        SDL_RenderGeometry(renderer_, block_selector_, vertices.data(),
                           static_cast<int>(vertices.size()), nullptr, 0);
    }
    else
    {
        const auto draw_layer = [&](float phase, SDL_Color color) {
            SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
            constexpr int dashes = 12;
            constexpr float dash_length = 0.045F;
            for (std::size_t edge = 0; edge < selector.size(); ++edge)
            {
                const SDL_FPoint from = *selector[edge];
                const SDL_FPoint to = *selector[(edge + 1U) % selector.size()];
                for (int dash = 0; dash < dashes; ++dash)
                {
                    const float start = std::fmod(static_cast<float>(dash) / dashes + phase, 1.0F);
                    const float finish = std::min(1.0F, start + dash_length);
                    const SDL_FPoint line[]{
                        {from.x + (to.x - from.x) * start, from.y + (to.y - from.y) * start},
                        {from.x + (to.x - from.x) * finish, from.y + (to.y - from.y) * finish}};
                    SDL_RenderLines(renderer_, line, 2);
                }
            }
        };
        draw_layer(scroll, {239U, 248U, 248U, 225U});
        draw_layer(1.0F - scroll, {92U, 201U, 218U, 155U});
    }
}

} // namespace openminerz
