#include "depth_voxel_renderer.hpp"

#include "openminerz/world/biome_atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace openminerz
{
namespace
{
constexpr std::size_t maximum_crater_masks = 32U;
}

class DepthVoxelRenderer::Impl final
{
  public:
    Impl(SDL_Renderer *renderer, SDL_Texture *atlas, SDL_Texture *normal_spec_atlas,
         SDL_Texture *metal_light_atlas)
        : renderer_(renderer)
    {
#if defined(_WIN32)
        const SDL_PropertiesID renderer_properties = SDL_GetRendererProperties(renderer_);
        auto *device = static_cast<ID3D11Device *>(SDL_GetPointerProperty(
            renderer_properties, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
        if (device == nullptr)
        {
            failure_reason_ = "SDL renderer is not exposing a D3D11 device";
            return;
        }
        device_ = device;
        ID3D11DeviceContext *context{};
        device_->GetImmediateContext(&context);
        context_.Attach(context);

        const SDL_PropertiesID texture_properties = SDL_GetTextureProperties(atlas);
        presentation_format_ = static_cast<SDL_PixelFormat>(SDL_GetNumberProperty(
            texture_properties, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN));
        auto *atlas_texture = static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
            texture_properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        if (atlas_texture == nullptr ||
            FAILED(device_->CreateShaderResourceView(atlas_texture, nullptr, &atlas_view_)))
        {
            failure_reason_ = "original atlas is not available as a D3D11 texture";
            return;
        }
        const auto create_material_view =
            [&](SDL_Texture *texture, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view) {
                if (texture == nullptr)
                {
                    return false;
                }
                const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
                auto *resource = static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
                    properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
                return resource != nullptr &&
                       SUCCEEDED(device_->CreateShaderResourceView(resource, nullptr, &view));
            };
        if (!create_material_view(normal_spec_atlas, normal_spec_view_) ||
            !create_material_view(metal_light_atlas, metal_light_view_))
        {
            failure_reason_ = "original normal/specular material atlases are unavailable";
            return;
        }
        // Repack each 8x8 atlas into a per-tile texture array with a full mip
        // chain: mip levels never bleed across neighbouring tiles, so distant
        // terrain filters cleanly instead of shimmering.
        const auto tile_source = [&](SDL_Texture *texture) -> ID3D11Texture2D * {
            const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
            return static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
                properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        };
        if (!build_tile_array(atlas_texture, atlas_array_view_) ||
            !build_tile_array(tile_source(normal_spec_atlas), normal_spec_array_view_) ||
            !build_tile_array(tile_source(metal_light_atlas), metal_light_array_view_))
        {
            failure_reason_ = "failed to build the mipmapped terrain tile arrays";
            return;
        }
        if (!create_pipeline())
        {
            return;
        }
        available_ = true;
#else
        (void)atlas;
        (void)normal_spec_atlas;
        (void)metal_light_atlas;
        failure_reason_ = "depth renderer currently requires Windows D3D11";
#endif
    }

    ~Impl()
    {
        SDL_DestroyTexture(target_texture_);
#if defined(_WIN32)
        target_view_.Reset();
        target_resource_.Reset();
        presentation_resource_.Reset();
        depth_view_.Reset();
        depth_texture_.Reset();
#endif
    }

    [[nodiscard]] bool available() const noexcept
    {
        return available_;
    }

    [[nodiscard]] std::string_view failure_reason() const noexcept
    {
        return failure_reason_;
    }

    void rebuild(const std::vector<world::WorldFace> &faces, int minimum_x, int minimum_z,
                 int maximum_x, int maximum_z)
    {
#if defined(_WIN32)
        if (!available_)
        {
            return;
        }
        crash_site_columns_.clear();
        track_crash_site_faces(std::tuple{0, 0, 0}, faces);
        std::vector<GpuVertex> vertices = build_vertices(faces);
        vertex_count_ = static_cast<UINT>(vertices.size());
        const float x0 = static_cast<float>(minimum_x);
        const float z0 = static_cast<float>(minimum_z);
        const float x1 = static_cast<float>(maximum_x + 1);
        const float z1 = static_cast<float>(maximum_z + 1);
        const std::array<GpuVertex, 6> water_vertices{{
            {x0, world::water_level, z0, x0 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z1, x1 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z0, x1 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x0, world::water_level, z0, x0 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x0, world::water_level, z1, x0 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z1, x1 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
        }};
        vertices.insert(vertices.end(), water_vertices.begin(), water_vertices.end());
        water_vertex_count_ = static_cast<UINT>(water_vertices.size());

        vertex_buffer_.Reset();
        if (vertices.empty())
        {
            return;
        }
        const D3D11_BUFFER_DESC description{static_cast<UINT>(vertices.size() * sizeof(GpuVertex)),
                                            D3D11_USAGE_IMMUTABLE,
                                            D3D11_BIND_VERTEX_BUFFER,
                                            0U,
                                            0U,
                                            sizeof(GpuVertex)};
        const D3D11_SUBRESOURCE_DATA data{vertices.data(), 0U, 0U};
        if (FAILED(device_->CreateBuffer(&description, &data, &vertex_buffer_)))
        {
            available_ = false;
            failure_reason_ = "failed to upload the depth-tested voxel mesh";
        }
#else
        (void)faces;
        (void)minimum_x;
        (void)minimum_z;
        (void)maximum_x;
        (void)maximum_z;
#endif
    }

    void rebuild_chunks(const std::vector<world::ChunkFaceBatch> &batches, int minimum_x,
                        int minimum_z, int maximum_x, int maximum_z)
    {
#if defined(_WIN32)
        if (!available_)
        {
            return;
        }
        vertex_buffer_.Reset();
        vertex_count_ = 0U;
        chunk_meshes_.clear();
        crash_site_columns_.clear();
        crater_masks_dirty_ = true;
        for (const auto &batch : batches)
        {
            replace_chunk(batch);
            if (!available_)
            {
                return;
            }
        }
        rebuild_water_buffer(minimum_x, minimum_z, maximum_x, maximum_z);
#else
        (void)batches;
        (void)minimum_x;
        (void)minimum_z;
        (void)maximum_x;
        (void)maximum_z;
#endif
    }

    void replace_chunk(const world::ChunkFaceBatch &batch)
    {
#if defined(_WIN32)
        if (!available_)
        {
            return;
        }
        const auto key = std::tuple{batch.coordinate.x, batch.coordinate.z, batch.coordinate.y};
        track_crash_site_faces(key, batch.faces);
        if (batch.faces.empty())
        {
            chunk_meshes_.erase(key);
            return;
        }
        const auto vertices = build_vertices(batch.faces);
        GpuChunkMesh mesh{};
        mesh.vertex_count = static_cast<UINT>(vertices.size());
        if (!create_vertex_buffer(vertices, mesh.vertex_buffer))
        {
            available_ = false;
            failure_reason_ = "failed to upload a chunk-local voxel mesh";
            return;
        }
        chunk_meshes_.insert_or_assign(key, std::move(mesh));
#else
        (void)batch;
#endif
    }

    void retain_chunks(int minimum_chunk_x, int minimum_chunk_z, int maximum_chunk_x,
                       int maximum_chunk_z)
    {
#if defined(_WIN32)
        std::erase_if(chunk_meshes_, [&](const auto &entry) {
            const auto &[chunk_x, chunk_z, section_y] = entry.first;
            (void)section_y;
            return chunk_x < minimum_chunk_x || chunk_x > maximum_chunk_x ||
                   chunk_z < minimum_chunk_z || chunk_z > maximum_chunk_z;
        });
        const std::size_t previous_crash_chunks = crash_site_columns_.size();
        std::erase_if(crash_site_columns_, [&](const auto &entry) {
            const auto &[chunk_x, chunk_z, section_y] = entry.first;
            (void)section_y;
            return chunk_x < minimum_chunk_x || chunk_x > maximum_chunk_x ||
                   chunk_z < minimum_chunk_z || chunk_z > maximum_chunk_z;
        });
        crater_masks_dirty_ =
            crater_masks_dirty_ || crash_site_columns_.size() != previous_crash_chunks;
#else
        (void)minimum_chunk_x;
        (void)minimum_chunk_z;
        (void)maximum_chunk_x;
        (void)maximum_chunk_z;
#endif
    }

    void submit_entities(EntityBatch batch)
    {
        entity_batch_ = std::move(batch);
    }

    void submit_dynamic_radiance(world::DynamicRadianceVolume volume)
    {
        dynamic_radiance_volume_ = std::move(volume);
    }

    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept
    {
        tone_curve_ = std::clamp(tone_curve, 0, 3);
        auto_exposure_enabled_ = auto_exposure;
        exposure_compensation_ = std::clamp(exposure_compensation, -2.0F, 2.0F);
        color_grading_ = std::clamp(color_grading, 0.0F, 1.5F);
    }

    // What the world was exposed by this frame, so the separately drawn sky
    // can match it exactly.
    [[nodiscard]] DisplayGrading display_grading() const noexcept
    {
        const float manual =
            (tone_curve_ == 0 ? 1.0F : 1.15F) * std::pow(2.0F, exposure_compensation_);
        const bool automatic = exposure_ready_ && auto_exposure_enabled_;
        return {manual * (automatic ? readback_exposure_ : 1.0F), tone_curve_, color_grading_};
    }

    void set_world_bounds(int minimum_x, int minimum_z, int maximum_x, int maximum_z)
    {
#if defined(_WIN32)
        if (available_)
        {
            rebuild_water_buffer(minimum_x, minimum_z, maximum_x, maximum_z);
        }
#else
        (void)minimum_x;
        (void)minimum_z;
        (void)maximum_x;
        (void)maximum_z;
#endif
    }

    [[nodiscard]] bool draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
                            const world::PlayerState &player, float vertical_fov_degrees,
                            float render_scale, float elapsed_seconds, float visibility_distance,
                            world::BiomeKind biome, bool water_enabled, bool enhanced_foliage,
                            bool ambient_occlusion, bool ore_glow, float shadow_strength)
    {
#if defined(_WIN32)
        if (!available_ || (vertex_buffer_ == nullptr && chunk_meshes_.empty()))
        {
            return false;
        }
        rebuild_crater_masks_if_needed();
        const float requested_scale = std::clamp(render_scale, 0.5F, 2.0F);
        const std::uint64_t requested_pixels =
            static_cast<std::uint64_t>(static_cast<double>(metrics.output_width) *
                                       metrics.output_height * requested_scale * requested_scale);
        constexpr std::uint64_t maximum_pixels = 32'000'000ULL;
        const float safe_scale =
            requested_pixels > maximum_pixels
                ? requested_scale * std::sqrt(static_cast<float>(maximum_pixels) /
                                              static_cast<float>(requested_pixels))
                : requested_scale;
        const int target_width =
            std::max(1, static_cast<int>(std::lround(metrics.output_width * safe_scale)));
        const int target_height =
            std::max(1, static_cast<int>(std::lround(metrics.output_height * safe_scale)));
        if (!ensure_target(target_width, target_height))
        {
            return false;
        }
        if (!SDL_FlushRenderer(renderer_))
        {
            failure_reason_ = SDL_GetError();
            return false;
        }
        const D3DStateBackup previous_state{context_.Get()};
        const bool dynamic_radiance_ready = upload_dynamic_radiance();
        constexpr float pi = 3.14159265358979323846F;
        const float vertical_fov = std::clamp(vertical_fov_degrees, 50.0F, 105.0F) * pi / 180.0F;
        const float aspect = static_cast<float>(target_width) / static_cast<float>(target_height);
        const float sine_yaw = std::sin(player.yaw);
        const float cosine_yaw = std::cos(player.yaw);
        const float sine_pitch = std::sin(player.pitch);
        const float cosine_pitch = std::cos(player.pitch);
        constexpr float near_plane = 0.06F;
        // The original's camera far plane is 1000 while terrain draws at
        // 64-192: the dragon approaches from 750 m as a crisp silhouette far
        // beyond the fog curtain.  Terrain chunks are still culled by the
        // visibility radius, so the wide clip costs nothing.
        constexpr float far_plane = 1000.0F;
        SceneConstants constants{};
        const bool hell_atmosphere_enabled =
            enhanced_foliage && SDL_getenv("OPENMINERZ_LEGACY_HELL_ATMOSPHERE") == nullptr;
        constants.eye = {player.position.x, player.position.y + world::PlayerSimulation::eye_height,
                         player.position.z, hell_atmosphere_enabled ? 1.0F : 0.0F};
        constants.right = {cosine_yaw, 0.0F, sine_yaw, 0.0F};
        constants.up = {-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch, 0.0F};
        constants.forward = {sine_yaw * cosine_pitch, sine_pitch, -cosine_yaw * cosine_pitch, 0.0F};
        const float y_scale = 1.0F / std::tan(vertical_fov * 0.5F);
        constants.projection = {y_scale / aspect, y_scale, far_plane / (far_plane - near_plane),
                                -near_plane * far_plane / (far_plane - near_plane)};
        // Keep terrain fog in the horizontal plane.  Using full 3-D eye distance made the
        // amount of fog on a fixed patch of ground change merely because the player looked up
        // or down.  Finish the fade before the streaming boundary so missing chunks can only
        // reveal the matching fog curtain drawn behind the world.
        const bool biome_atmosphere_enabled =
            enhanced_foliage && SDL_getenv("OPENMINERZ_DISABLE_BIOME_ATMOSPHERE") == nullptr;
        const world::BiomeAtmosphere biome_atmosphere =
            biome_atmosphere_enabled ? world::biome_atmosphere(biome) : world::BiomeAtmosphere{};
        const float fog_end = std::clamp(visibility_distance - 10.0F, 24.0F, 118.0F);
        // Fog is a band near the draw limit, not an atmosphere-wide soup: the
        // original's distance haze only bites in the last stretch before the
        // chunk edge (the desert used to drown in orange from 24 blocks out).
        const float fog_start = std::max(biome_atmosphere_enabled ? 18.0F : 24.0F,
                                         fog_end * biome_atmosphere.fog_start_ratio);
        // SunlightColor and AmbientSunColor stay separate all the way to the
        // pixel shader, exactly like the original terrain effect parameters.
        constants.light_and_fog_start = {sky.direct.red, sky.direct.green, sky.direct.blue,
                                         fog_start};
        // w selects the Rebirth foliage cutout. This is a presentation flag,
        // independent from the Lagoon-only enhanced water path below.
        constants.ambient_sun = {sky.ambient.red, sky.ambient.green, sky.ambient.blue,
                                 enhanced_foliage ? 1.0F : 0.0F};
        world::SkyColor presentation_fog = sky.fog;
        if (enhanced_foliage)
        {
            // Rebirth treats the horizon as illuminated atmosphere instead of
            // the original game's dark-grey cut-off curtain. Keep a little of
            // the authored fog hue, but lift it toward the sky horizon so distant
            // terrain recedes rather than looking as though the world ends.
            constexpr float horizon_mix = 0.72F;
            presentation_fog = {std::lerp(sky.fog.red, sky.horizon.red, horizon_mix),
                                std::lerp(sky.fog.green, sky.horizon.green, horizon_mix),
                                std::lerp(sky.fog.blue, sky.horizon.blue, horizon_mix)};
            // SkyState palette values are authored/display-referred. The HDR
            // world target is linear in Rebirth, so fog must cross the same
            // gamma-two boundary as albedo before it is mixed into terrain.
            presentation_fog = {presentation_fog.red * presentation_fog.red,
                                presentation_fog.green * presentation_fog.green,
                                presentation_fog.blue * presentation_fog.blue};
        }
        constants.fog_and_end = {presentation_fog.red, presentation_fog.green,
                                 presentation_fog.blue, fog_end};
        const bool eye_in_dry_crater =
            std::any_of(crater_masks_.begin(), crater_masks_.end(), [&](const auto &mask) {
                return mask[3] > 0.5F && std::hypot(player.position.x - mask[0],
                                                    player.position.z - mask[1]) < mask[2];
            });
        constants.water_and_time = {
            elapsed_seconds, world::water_level,
            water_enabled && !eye_in_dry_crater && constants.eye.y < world::water_level ? 1.0F
                                                                                        : 0.0F,
            sky.daylight};
        constants.sun_and_options = {sky.sun_x, sky.sun_y, sky.sun_z,
                                     ambient_occlusion ? 1.0F : 0.0F};
        const float quality_shadow_strength = std::clamp(shadow_strength, 0.0F, 1.0F);
        const bool sun_shadows_disabled = SDL_getenv("OPENMINERZ_DISABLE_SUN_SHADOWS") != nullptr;
        float solar_shadow_visibility = 1.0F;
        if (enhanced_foliage)
        {
            solar_shadow_visibility = std::clamp((sky.sun_y - 0.005F) / 0.10F, 0.0F, 1.0F);
            solar_shadow_visibility = solar_shadow_visibility * solar_shadow_visibility *
                                      (3.0F - 2.0F * solar_shadow_visibility);
        }
        const float effective_shadow_strength =
            sun_shadows_disabled ? 0.0F : quality_shadow_strength * solar_shadow_visibility;
        constants.render_options = {effective_shadow_strength, sky.specular.red, sky.specular.green,
                                    sky.specular.blue};
        // BlockTerrain.TorchColor (255, 235, 190); w lights the area around a held torch.
        constants.torch_and_eye = {1.0F, 235.0F / 255.0F, 190.0F / 255.0F,
                                   player.selected_item_id == 14U ? 1.0F : 0.0F};
        const bool point_shadow_ready =
            enhanced_foliage && constants.torch_and_eye.w > 0.0F &&
            SDL_getenv("OPENMINERZ_DISABLE_POINT_SHADOW") == nullptr &&
            point_shadow_vertex_shader_ != nullptr && shadow_pixel_shader_ != nullptr &&
            point_shadow_srv_ != nullptr && point_shadow_rasterizer_state_ != nullptr &&
            std::all_of(point_shadow_views_.begin(), point_shadow_views_.end(),
                        [](const auto &view) { return view != nullptr; });
        const float contact_shadow_mode =
            SDL_getenv("OPENMINERZ_DISABLE_CONTACT_SHADOWS") != nullptr ? -1.0F : 0.0F;

        // Three frustum-fitted sun cascades. A single player-centred square lets
        // off-camera mountains consume most of the map and cast one coarse slab
        // over the scene. Fitting each slice to what the camera can actually see
        // keeps contact shadows detailed while the far cascade remains stable.
        constexpr float shadow_resolution = 2048.0F;
        const float shadow_far = std::clamp(visibility_distance + 18.0F, 48.0F, 150.0F);
        const std::array<float, 3> shadow_splits{std::min(16.0F, shadow_far),
                                                 std::min(48.0F, shadow_far), shadow_far};
        const std::array<float, 3> shadow_starts{near_plane, shadow_splits[0], shadow_splits[1]};
        std::array<float, 3> shadow_draw_radii{};
        {
            const float sun_length =
                std::sqrt(sky.sun_x * sky.sun_x + sky.sun_y * sky.sun_y + sky.sun_z * sky.sun_z);
            const float inv_sun = sun_length > 0.0001F ? 1.0F / sun_length : 0.0F;
            const float dx = sky.sun_x * inv_sun;
            const float dy = sky.sun_y * inv_sun;
            const float dz = sky.sun_z * inv_sun;
            float rx = dz;
            constexpr float ry = 0.0F;
            float rz = -dx;
            const float right_length = std::sqrt(rx * rx + rz * rz);
            if (right_length > 0.0001F)
            {
                rx /= right_length;
                rz /= right_length;
            }
            else
            {
                rx = 1.0F;
                rz = 0.0F;
            }
            const float ux = dy * rz - dz * ry;
            const float uy = dz * rx - dx * rz;
            const float uz = dx * ry - dy * rx;
            const float tangent_y = std::tan(vertical_fov * 0.5F);
            const float tangent_x = tangent_y * aspect;

            struct CascadeTransform final
            {
                Float4 x;
                Float4 y;
                Float4 z;
            };
            std::array<CascadeTransform, 3> transforms{};
            for (std::size_t cascade = 0; cascade < transforms.size(); ++cascade)
            {
                const float slice_near = shadow_starts[cascade];
                const float slice_far = shadow_splits[cascade];
                float minimum_u = std::numeric_limits<float>::max();
                float maximum_u = std::numeric_limits<float>::lowest();
                float minimum_v = std::numeric_limits<float>::max();
                float maximum_v = std::numeric_limits<float>::lowest();
                float minimum_d = std::numeric_limits<float>::max();
                float maximum_d = std::numeric_limits<float>::lowest();
                for (const float depth : {slice_near, slice_far})
                {
                    const float half_x = depth * tangent_x;
                    const float half_y = depth * tangent_y;
                    for (const float side : {-1.0F, 1.0F})
                        for (const float vertical : {-1.0F, 1.0F})
                        {
                            const float wx = constants.eye.x + constants.forward.x * depth +
                                             constants.right.x * half_x * side +
                                             constants.up.x * half_y * vertical;
                            const float wy = constants.eye.y + constants.forward.y * depth +
                                             constants.right.y * half_x * side +
                                             constants.up.y * half_y * vertical;
                            const float wz = constants.eye.z + constants.forward.z * depth +
                                             constants.right.z * half_x * side +
                                             constants.up.z * half_y * vertical;
                            const float u = wx * rx + wy * ry + wz * rz;
                            const float v = wx * ux + wy * uy + wz * uz;
                            const float d = -(wx * dx + wy * dy + wz * dz);
                            minimum_u = std::min(minimum_u, u);
                            maximum_u = std::max(maximum_u, u);
                            minimum_v = std::min(minimum_v, v);
                            maximum_v = std::max(maximum_v, v);
                            minimum_d = std::min(minimum_d, d);
                            maximum_d = std::max(maximum_d, d);
                        }
                }

                // A square footprint prevents resolution pumping when the camera
                // turns. Padding grows with the cascade so filtered edges never
                // expose the border.
                const float padding = std::array{2.0F, 5.0F, 11.0F}[cascade];
                const float extent = std::max({(maximum_u - minimum_u) * 0.5F,
                                               (maximum_v - minimum_v) * 0.5F, 4.0F}) +
                                     padding;
                const float texel_world = (2.0F * extent) / shadow_resolution;
                float center_u = (minimum_u + maximum_u) * 0.5F;
                float center_v = (minimum_v + maximum_v) * 0.5F;
                center_u = std::floor(center_u / texel_world) * texel_world;
                center_v = std::floor(center_v / texel_world) * texel_world;

                // Leave enough depth in front of the visible slice for a nearby
                // ridge or tree to cast into it, without admitting an arbitrary
                // mountain from the other side of the streamed world.
                minimum_d -= std::array{38.0F, 64.0F, 104.0F}[cascade];
                maximum_d += 18.0F;
                const float depth_extent = std::max((maximum_d - minimum_d) * 0.5F, 24.0F);
                const float center_d = (minimum_d + maximum_d) * 0.5F;
                const float uv_scale = 1.0F / (2.0F * extent);
                const float depth_scale = 1.0F / (2.0F * depth_extent);
                transforms[cascade].x = {rx * uv_scale, ry * uv_scale, rz * uv_scale,
                                         0.5F - center_u * uv_scale};
                transforms[cascade].y = {-ux * uv_scale, -uy * uv_scale, -uz * uv_scale,
                                         0.5F + center_v * uv_scale};
                transforms[cascade].z = {-dx * depth_scale, -dy * depth_scale, -dz * depth_scale,
                                         0.5F - center_d * depth_scale};
                shadow_draw_radii[cascade] = slice_far + std::array{22.0F, 38.0F, 64.0F}[cascade];
            }
            constants.shadow_x = transforms[0].x;
            constants.shadow_y = transforms[0].y;
            constants.shadow_z = transforms[0].z;
            constants.shadow_x2 = transforms[1].x;
            constants.shadow_y2 = transforms[1].y;
            constants.shadow_z2 = transforms[1].z;
            constants.shadow_x3 = transforms[2].x;
            constants.shadow_y3 = transforms[2].y;
            constants.shadow_z3 = transforms[2].z;
            constants.shadow_cascades = {shadow_splits[0], shadow_splits[1], shadow_splits[2],
                                         contact_shadow_mode};
            const bool point_shadow_debug_output =
                SDL_getenv("OPENMINERZ_POINT_SHADOW_DEBUG_OUTPUT") != nullptr;
            constants.shadow_params = {
                point_shadow_debug_output ? -2.0F
                : sun_shadows_disabled    ? 0.0F
                                          : quality_shadow_strength,
                1.0F / shadow_resolution, solar_shadow_visibility,
                point_shadow_ready ? 1.0F / static_cast<float>(point_shadow_resolution) : 0.0F};
        }
        // Sun screen-space position for god rays / glow in the post pass.
        {
            const float cx = sky.sun_x * constants.right.x + sky.sun_y * constants.right.y +
                             sky.sun_z * constants.right.z;
            const float cy = sky.sun_x * constants.up.x + sky.sun_y * constants.up.y +
                             sky.sun_z * constants.up.z;
            const float cz = sky.sun_x * constants.forward.x + sky.sun_y * constants.forward.y +
                             sky.sun_z * constants.forward.z;
            const float safe_z = std::max(cz, 0.0001F);
            constants.post_sun = {0.5F + 0.5F * (cx / safe_z) * constants.projection.x,
                                  0.5F - 0.5F * (cy / safe_z) * constants.projection.y,
                                  enhanced_foliage && sky.sun_y <= -0.005F ? -1.0F : cz,
                                  sky.daylight};
        }
        constants.post_params = {0.85F, 0.42F, static_cast<float>(tone_curve_), elapsed_seconds};
        constants.target_size = {
            static_cast<float>(target_width), static_cast<float>(target_height),
            1.0F / static_cast<float>(target_width), 1.0F / static_cast<float>(target_height)};
        // The legacy curve keeps the old un-exposed look so the comparison is
        // honest; the real curves expect scene light around middle grey.
        const float base_exposure = tone_curve_ == 0 ? 1.0F : 1.15F;
        // z scales the bloom's quadratic weight rather than thresholding it.
        constants.grade_params = {base_exposure * std::pow(2.0F, exposure_compensation_),
                                  color_grading_, 0.62F,
                                  exposure_ready_ && auto_exposure_enabled_ ? 1.0F : 0.0F};
        constants.dynamic_radiance_origin = {static_cast<float>(dynamic_radiance_volume_.origin_x),
                                             static_cast<float>(dynamic_radiance_volume_.origin_y),
                                             static_cast<float>(dynamic_radiance_volume_.origin_z),
                                             dynamic_radiance_ready ? 1.0F : 0.0F};
        constants.dynamic_radiance_params = {
            static_cast<float>(world::dynamic_radiance_extent),
            1.0F / static_cast<float>(world::dynamic_radiance_extent), 0.82F,
            SDL_getenv("OPENMINERZ_CANOPY_SHADOW_DEBUG_OUTPUT") != nullptr ? 1.0F : 0.0F};
        const float atmosphere_presence = 0.35F + 0.65F * sky.daylight;
        constants.biome_tint = {
            biome_atmosphere.grade_multiplier.red, biome_atmosphere.grade_multiplier.green,
            biome_atmosphere.grade_multiplier.blue,
            biome_atmosphere_enabled ? biome_atmosphere.grade_strength * atmosphere_presence
                                     : 0.0F};
        constants.biome_params = {biome_atmosphere_enabled ? biome_atmosphere.saturation : 1.0F,
                                  ore_glow ? 1.0F : 0.0F, 0.0F, 0.0F};
        for (std::size_t index = 0U; index < crater_masks_.size(); ++index)
        {
            constants.crater_masks[index] = {crater_masks_[index][0], crater_masks_[index][1],
                                             crater_masks_[index][2], crater_masks_[index][3]};
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(scene_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
        {
            failure_reason_ = "failed to update the depth renderer camera";
            return false;
        }
        *static_cast<SceneConstants *>(mapped.pData) = constants;
        context_->Unmap(scene_buffer_.Get(), 0U);

        const UINT stride = sizeof(GpuVertex);
        const UINT offset = 0U;
        const auto draw_world_geometry = [&](float draw_radius) {
            if (chunk_meshes_.empty())
            {
                ID3D11Buffer *vertex_buffer = vertex_buffer_.Get();
                context_->IASetVertexBuffers(0U, 1U, &vertex_buffer, &stride, &offset);
                context_->Draw(vertex_count_, 0U);
                return;
            }
            const float draw_radius_squared = draw_radius * draw_radius;
            for (const auto &[coordinate, mesh] : chunk_meshes_)
            {
                const float chunk_center_x =
                    (static_cast<float>(std::get<0>(coordinate)) + 0.5F) * world::chunk_width;
                const float chunk_center_z =
                    (static_cast<float>(std::get<1>(coordinate)) + 0.5F) * world::chunk_depth;
                const float dx = chunk_center_x - player.position.x;
                const float dz = chunk_center_z - player.position.z;
                if (dx * dx + dz * dz > draw_radius_squared)
                {
                    continue;
                }
                ID3D11Buffer *chunk_buffer = mesh.vertex_buffer.Get();
                context_->IASetVertexBuffers(0U, 1U, &chunk_buffer, &stride, &offset);
                context_->Draw(mesh.vertex_count, 0U);
            }
        };
        const bool entity_buffer_ready =
            entity_ready_ && !entity_batch_.vertices.empty() && upload_entity_batch();
        const bool entity_shadow_casting =
            entity_buffer_ready && SDL_getenv("OPENMINERZ_DISABLE_ENTITY_SHADOWS") == nullptr;

        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer *scene_buffer = scene_buffer_.Get();
        context_->VSSetConstantBuffers(0U, 1U, &scene_buffer);
        context_->PSSetConstantBuffers(0U, 1U, &scene_buffer);
        ID3D11SamplerState *sampler = sampler_state_.Get();
        context_->PSSetSamplers(0U, 1U, &sampler);

        // 1) Depth-only shadow passes. Sunlight uses three fitted cascades;
        // the moving held torch uses a small six-face point map and therefore
        // remains available at night and underground.
        const bool sun_shadows_ready =
            std::all_of(shadow_views_.begin(), shadow_views_.end(),
                        [](const auto &view) { return view != nullptr; }) &&
            shadow_vertex_shader_ != nullptr && effective_shadow_strength > 0.0F;
        const bool canopy_shadow_transmission_ready =
            shadow_transmission_srv_ != nullptr &&
            std::all_of(shadow_transmission_views_.begin(), shadow_transmission_views_.end(),
                        [](const auto &view) { return view != nullptr; });
        if (sun_shadows_ready || point_shadow_ready)
        {
            context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
            context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
            context_->PSSetShader(shadow_pixel_shader_.Get(), nullptr, 0U);
            ID3D11ShaderResourceView *diffuse_only = atlas_array_view_.Get();
            context_->PSSetShaderResources(0U, 1U, &diffuse_only);
            // The transmission array is written beside the sun depth atlas.
            // Explicitly unbind its main-pass SRV before attaching any slice as
            // an RTV on the next frame.
            ID3D11ShaderResourceView *empty_shadow_transmission{};
            context_->PSSetShaderResources(8U, 1U, &empty_shadow_transmission);
            if (sun_shadows_ready)
            {
                const D3D11_VIEWPORT shadow_viewport{0.0F, 0.0F, 2048.0F, 2048.0F, 0.0F, 1.0F};
                context_->RSSetViewports(1U, &shadow_viewport);
                context_->RSSetState(shadow_rasterizer_state_.Get());
                context_->VSSetShader(shadow_vertex_shader_.Get(), nullptr, 0U);
                for (std::size_t cascade = 0; cascade < shadow_views_.size(); ++cascade)
                {
                    constants.shadow_cascades.w = static_cast<float>(cascade);
                    if (FAILED(context_->Map(scene_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                             &mapped)))
                    {
                        failure_reason_ = "failed to update the shadow cascade";
                        return false;
                    }
                    *static_cast<SceneConstants *>(mapped.pData) = constants;
                    context_->Unmap(scene_buffer_.Get(), 0U);
                    context_->ClearDepthStencilView(shadow_views_[cascade].Get(), D3D11_CLEAR_DEPTH,
                                                    1.0F, 0U);
                    ID3D11RenderTargetView *transmission_target =
                        canopy_shadow_transmission_ready ? shadow_transmission_views_[cascade].Get()
                                                         : nullptr;
                    if (transmission_target != nullptr)
                    {
                        constexpr std::array<float, 4> opaque_shadow{};
                        context_->ClearRenderTargetView(transmission_target, opaque_shadow.data());
                        context_->OMSetRenderTargets(1U, &transmission_target,
                                                     shadow_views_[cascade].Get());
                    }
                    else
                    {
                        context_->OMSetRenderTargets(0U, nullptr, shadow_views_[cascade].Get());
                    }
                    // Entity depth is submitted first. If an entity is the nearest
                    // caster, its untouched/cleared transmission texel stays opaque;
                    // nearer terrain or foliage then updates depth and material in
                    // one test. The depth result itself is order-independent.
                    if (entity_shadow_casting && entity_sun_shadow_vertex_shader_ != nullptr)
                        draw_entity_shadow_geometry(entity_sun_shadow_vertex_shader_.Get(),
                                                    shadow_vertex_shader_.Get());
                    draw_world_geometry(shadow_draw_radii[cascade]);
                }
            }

            if (point_shadow_ready)
            {
                ID3D11ShaderResourceView *empty_point_view{};
                context_->PSSetShaderResources(6U, 1U, &empty_point_view);
                const D3D11_VIEWPORT point_viewport{0.0F,
                                                    0.0F,
                                                    static_cast<float>(point_shadow_resolution),
                                                    static_cast<float>(point_shadow_resolution),
                                                    0.0F,
                                                    1.0F};
                context_->RSSetViewports(1U, &point_viewport);
                context_->RSSetState(point_shadow_rasterizer_state_.Get());
                context_->VSSetShader(point_shadow_vertex_shader_.Get(), nullptr, 0U);
                for (std::size_t face = 0; face < point_shadow_views_.size(); ++face)
                {
                    constants.shadow_cascades.w = static_cast<float>(face);
                    if (FAILED(context_->Map(scene_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                             &mapped)))
                    {
                        failure_reason_ = "failed to update the held-torch shadow face";
                        return false;
                    }
                    *static_cast<SceneConstants *>(mapped.pData) = constants;
                    context_->Unmap(scene_buffer_.Get(), 0U);
                    context_->ClearDepthStencilView(point_shadow_views_[face].Get(),
                                                    D3D11_CLEAR_DEPTH, 1.0F, 0U);
                    context_->OMSetRenderTargets(0U, nullptr, point_shadow_views_[face].Get());
                    // A nine-unit light can straddle a 16x16 chunk corner. The
                    // extra centre allowance includes every section capable of
                    // occluding it without traversing the distant stream.
                    draw_world_geometry(22.0F);
                    if (entity_shadow_casting && entity_point_shadow_vertex_shader_ != nullptr)
                        draw_entity_shadow_geometry(entity_point_shadow_vertex_shader_.Get(),
                                                    point_shadow_vertex_shader_.Get());
                }
            }

            // Restore the shared constants once after all depth slices. Main-pass
            // shaders use all transforms; the selector itself is depth-pass only.
            constants.shadow_cascades.w = contact_shadow_mode;
            if (FAILED(
                    context_->Map(scene_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            {
                failure_reason_ = "failed to restore the lit-scene constants";
                return false;
            }
            *static_cast<SceneConstants *>(mapped.pData) = constants;
            context_->Unmap(scene_buffer_.Get(), 0U);
        }

        // 2) Main lit pass into the world colour target.
        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        context_->ClearRenderTargetView(target_view_.Get(), transparent.data());
        context_->ClearDepthStencilView(depth_view_.Get(), D3D11_CLEAR_DEPTH, 1.0F, 0U);
        ID3D11RenderTargetView *render_target = target_view_.Get();
        context_->OMSetRenderTargets(1U, &render_target, depth_view_.Get());
        context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
        context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(target_width), static_cast<float>(target_height),
            0.0F, 1.0F};
        context_->RSSetViewports(1U, &viewport);
        context_->RSSetState(rasterizer_state_.Get());
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0U);
        const std::array material_views{
            atlas_array_view_.Get(),
            normal_spec_array_view_.Get(),
            metal_light_array_view_.Get(),
            sun_shadows_ready ? shadow_srv_.Get() : nullptr,
            static_cast<ID3D11ShaderResourceView *>(nullptr),
            static_cast<ID3D11ShaderResourceView *>(nullptr),
            point_shadow_ready ? point_shadow_srv_.Get() : nullptr,
            dynamic_radiance_ready ? dynamic_radiance_srv_.Get() : nullptr,
            canopy_shadow_transmission_ready &&
                    SDL_getenv("OPENMINERZ_DISABLE_CANOPY_SHADOW_TRANSMISSION") == nullptr
                ? shadow_transmission_srv_.Get()
                : nullptr};
        context_->PSSetShaderResources(0U, static_cast<UINT>(material_views.size()),
                                       material_views.data());
        ID3D11SamplerState *shadow_sampler = shadow_sampler_.Get();
        context_->PSSetSamplers(1U, 1U, &shadow_sampler);
        ID3D11SamplerState *aniso_sampler = aniso_sampler_state_.Get();
        context_->PSSetSamplers(2U, 1U, &aniso_sampler);
        draw_world_geometry(visibility_distance + 24.0F);

        if (entity_buffer_ready)
        {
            draw_entity_batch();
        }

        if (water_enabled)
        {
            // The water shader refracts the already-rendered scene, so snapshot
            // the colour target and expose the depth buffer read-only.
            context_->OMSetRenderTargets(0U, nullptr, nullptr);
            context_->CopyResource(scene_copy_resource_.Get(), target_resource_.Get());
            context_->OMSetRenderTargets(1U, &render_target, depth_view_readonly_.Get());
            context_->OMSetDepthStencilState(water_depth_state_.Get(), 0U);
            context_->OMSetBlendState(water_blend_state_.Get(), nullptr, 0xFFFFFFFFU);
            const std::array water_views{scene_copy_srv_.Get(), depth_srv_.Get()};
            context_->PSSetShaderResources(4U, static_cast<UINT>(water_views.size()),
                                           water_views.data());
            if (water_vertex_buffer_ != nullptr)
            {
                ID3D11Buffer *water_buffer = water_vertex_buffer_.Get();
                context_->IASetVertexBuffers(0U, 1U, &water_buffer, &stride, &offset);
                context_->Draw(water_vertex_count_, 0U);
            }
            else
            {
                context_->Draw(water_vertex_count_, vertex_count_);
            }
        }

        constexpr std::array<ID3D11ShaderResourceView *, 9> empty_views{};
        context_->PSSetShaderResources(0U, static_cast<UINT>(empty_views.size()),
                                       empty_views.data());
        context_->OMSetRenderTargets(0U, nullptr, nullptr);

        // 3) Post-processing: bloom bright/blur at quarter resolution, then the
        //    composite pass adds god rays and the sun glow.
        if (post_ready_)
        {
            context_->IASetInputLayout(nullptr);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(post_vertex_shader_.Get(), nullptr, 0U);
            context_->OMSetDepthStencilState(post_depth_state_.Get(), 0U);
            context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
            context_->RSSetState(rasterizer_state_.Get());

            // 3a) Measure the frame: log luminance at 64x64, mip-averaged to a
            //     single value, then blended into the adapted exposure.
            const bool auto_exposure = exposure_ready_ && auto_exposure_enabled_;
            if (auto_exposure)
            {
                const D3D11_VIEWPORT luminance_viewport{0.0F,
                                                        0.0F,
                                                        static_cast<float>(luminance_extent),
                                                        static_cast<float>(luminance_extent),
                                                        0.0F,
                                                        1.0F};
                context_->RSSetViewports(1U, &luminance_viewport);
                ID3D11RenderTargetView *luminance_target = luminance_rtv_.Get();
                context_->OMSetRenderTargets(1U, &luminance_target, nullptr);
                context_->PSSetShader(luminance_pixel_shader_.Get(), nullptr, 0U);
                ID3D11ShaderResourceView *scene_source = target_srv_.Get();
                context_->PSSetShaderResources(0U, 1U, &scene_source);
                context_->Draw(3U, 0U);
                context_->OMSetRenderTargets(0U, nullptr, nullptr);
                context_->GenerateMips(luminance_srv_.Get());

                // Frame delta and a "snap on the first frame" flag ride in the
                // spare blur constant buffer.
                D3D11_MAPPED_SUBRESOURCE adapt_mapped{};
                if (SUCCEEDED(context_->Map(blur_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                            &adapt_mapped)))
                {
                    const float frame_delta =
                        last_frame_seconds_ < 0.0F
                            ? 0.016F
                            : std::clamp(elapsed_seconds - last_frame_seconds_, 0.0F, 0.25F);
                    const Float4 adapt_constants{frame_delta, adapt_primed_ ? 0.0F : 1.0F, 0.0F,
                                                 0.0F};
                    *static_cast<Float4 *>(adapt_mapped.pData) = adapt_constants;
                    context_->Unmap(blur_buffer_.Get(), 0U);
                }
                ID3D11Buffer *adapt_buffer = blur_buffer_.Get();
                context_->PSSetConstantBuffers(1U, 1U, &adapt_buffer);
                const std::size_t next_adapt = 1U - adapt_index_;
                const D3D11_VIEWPORT adapt_viewport{0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};
                context_->RSSetViewports(1U, &adapt_viewport);
                ID3D11RenderTargetView *adapt_target = adapt_rtv_[next_adapt].Get();
                context_->OMSetRenderTargets(1U, &adapt_target, nullptr);
                context_->PSSetShader(adapt_pixel_shader_.Get(), nullptr, 0U);
                const std::array adapt_views{luminance_srv_.Get(), adapt_srv_[adapt_index_].Get()};
                context_->PSSetShaderResources(0U, static_cast<UINT>(adapt_views.size()),
                                               adapt_views.data());
                context_->Draw(3U, 0U);
                context_->OMSetRenderTargets(0U, nullptr, nullptr);
                context_->PSSetShaderResources(0U, static_cast<UINT>(empty_views.size()),
                                               empty_views.data());
                adapt_index_ = next_adapt;
                adapt_primed_ = true;
                // Non-blocking readback: whatever the CPU can see this frame
                // drives the sky, which is drawn outside this pipeline.
                context_->CopyResource(adapt_staging_.Get(), adapt_resource_[adapt_index_].Get());
                D3D11_MAPPED_SUBRESOURCE readback{};
                if (SUCCEEDED(context_->Map(adapt_staging_.Get(), 0U, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &readback)) &&
                    readback.pData != nullptr)
                {
                    const float adapted = *static_cast<const float *>(readback.pData);
                    context_->Unmap(adapt_staging_.Get(), 0U);
                    if (std::isfinite(adapted))
                    {
                        const float average = std::exp(adapted);
                        const float maximum_exposure = enhanced_foliage ? 1.0F : 1.414F;
                        readback_exposure_ = std::clamp(0.20F / std::max(average, 0.0008F), 0.354F,
                                                        maximum_exposure);
                    }
                }
            }
            last_frame_seconds_ = elapsed_seconds;
            ID3D11ShaderResourceView *adapted_view =
                auto_exposure ? adapt_srv_[adapt_index_].Get() : nullptr;

            const D3D11_VIEWPORT bloom_viewport{
                0.0F, 0.0F, static_cast<float>(bloom_width_), static_cast<float>(bloom_height_),
                0.0F, 1.0F};
            context_->RSSetViewports(1U, &bloom_viewport);
            ID3D11RenderTargetView *bloom_target = bloom_a_rtv_.Get();
            context_->OMSetRenderTargets(1U, &bloom_target, nullptr);
            context_->PSSetShader(bright_pixel_shader_.Get(), nullptr, 0U);
            const std::array bright_views{target_srv_.Get(), adapted_view};
            context_->PSSetShaderResources(0U, static_cast<UINT>(bright_views.size()),
                                           bright_views.data());
            context_->Draw(3U, 0U);

            const auto blur_pass = [&](ID3D11RenderTargetView *destination,
                                       ID3D11ShaderResourceView *source, float direction_x,
                                       float direction_y) {
                D3D11_MAPPED_SUBRESOURCE blur_mapped{};
                if (SUCCEEDED(context_->Map(blur_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                            &blur_mapped)))
                {
                    const Float4 blur_constants{direction_x / static_cast<float>(bloom_width_),
                                                direction_y / static_cast<float>(bloom_height_),
                                                0.0F, 0.0F};
                    *static_cast<Float4 *>(blur_mapped.pData) = blur_constants;
                    context_->Unmap(blur_buffer_.Get(), 0U);
                }
                ID3D11Buffer *blur_buffer = blur_buffer_.Get();
                context_->PSSetConstantBuffers(1U, 1U, &blur_buffer);
                context_->OMSetRenderTargets(1U, &destination, nullptr);
                context_->PSSetShader(blur_pixel_shader_.Get(), nullptr, 0U);
                context_->PSSetShaderResources(0U, 1U, &source);
                context_->Draw(3U, 0U);
                ID3D11ShaderResourceView *cleared = nullptr;
                context_->PSSetShaderResources(0U, 1U, &cleared);
                context_->OMSetRenderTargets(0U, nullptr, nullptr);
            };
            blur_pass(bloom_b_rtv_.Get(), bloom_a_srv_.Get(), 1.0F, 0.0F);
            blur_pass(bloom_a_rtv_.Get(), bloom_b_srv_.Get(), 0.0F, 1.0F);

            context_->RSSetViewports(1U, &viewport);
            ID3D11RenderTargetView *post_target = post_rtv_.Get();
            context_->OMSetRenderTargets(1U, &post_target, nullptr);
            context_->PSSetShader(composite_pixel_shader_.Get(), nullptr, 0U);
            const std::array composite_views{target_srv_.Get(), bloom_a_srv_.Get(), adapted_view};
            context_->PSSetShaderResources(0U, static_cast<UINT>(composite_views.size()),
                                           composite_views.data());
            context_->Draw(3U, 0U);
            context_->PSSetShaderResources(0U, static_cast<UINT>(empty_views.size()),
                                           empty_views.data());
            context_->OMSetRenderTargets(0U, nullptr, nullptr);
            context_->CopyResource(presentation_resource_.Get(), post_resource_.Get());
        }
        else
        {
            context_->CopyResource(presentation_resource_.Get(), target_resource_.Get());
        }
        previous_state.restore(context_.Get());
        // SDL's second external-rendering boundary invalidates its D3D11 state cache.
        if (!SDL_FlushRenderer(renderer_))
        {
            failure_reason_ =
                std::format("failed to invalidate SDL state after D3D11 ({})", SDL_GetError());
            return false;
        }
        const SDL_FRect destination{0.0F, 0.0F, static_cast<float>(metrics.output_width),
                                    static_cast<float>(metrics.output_height)};
        return SDL_RenderTexture(renderer_, target_texture_, nullptr, &destination);
#else
        (void)metrics;
        (void)sky;
        (void)player;
        (void)vertical_fov_degrees;
        (void)render_scale;
        (void)elapsed_seconds;
        (void)visibility_distance;
        (void)water_enabled;
        (void)enhanced_foliage;
        (void)ambient_occlusion;
        (void)ore_glow;
        (void)shadow_strength;
        return false;
#endif
    }

  private:
#if defined(_WIN32)
    struct GpuVertex final
    {
        float x{};
        float y{};
        float z{};
        float u{};
        float v{};
        float normal_x{};
        float normal_y{};
        float normal_z{};
        float ambient_occlusion{};
        float sky_light{};
        float emission{};
        float torch_light{};
        std::uint32_t material{};
        std::uint32_t indirect_light{};
        std::uint32_t block_light_lobe{};
    };
    static_assert(sizeof(GpuVertex) == 60U,
                  "packed material and lighting data must keep the voxel vertex at 60 bytes");

    // Foliage sways in the vertex shader; everything else stays rigid.
    [[nodiscard]] static float block_wave(world::BlockType type) noexcept
    {
        return type == world::BlockType::leaves ? 1.0F : 0.0F;
    }

    // Snow sparkles and ice reflects: per-vertex gloss lifts the authored material
    // response for those surfaces without touching the derived texture atlases.
    [[nodiscard]] static float block_gloss(world::BlockType type) noexcept
    {
        switch (type)
        {
        case world::BlockType::ice:
            return 0.85F;
        case world::BlockType::snow:
            return 0.55F;
        case world::BlockType::diamond_wall:
            return 0.6F;
        default:
            return 0.0F;
        }
    }

    [[nodiscard]] static bool is_torch_block(world::BlockType type) noexcept
    {
        return type >= world::BlockType::torch && type <= world::BlockType::torch_neg_y;
    }

    [[nodiscard]] static std::uint32_t pack_unorm8(
        const std::array<std::uint8_t, 4> &light) noexcept
    {
        return static_cast<std::uint32_t>(light[0]) | static_cast<std::uint32_t>(light[1]) << 8U |
               static_cast<std::uint32_t>(light[2]) << 16U |
               static_cast<std::uint32_t>(light[3]) << 24U;
    }

    [[nodiscard]] static std::uint32_t pack_material(float gloss, float wave,
                                                     std::uint8_t tile) noexcept
    {
        const auto unorm = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        return static_cast<std::uint32_t>(unorm(gloss)) |
               static_cast<std::uint32_t>(unorm(wave)) << 8U |
               static_cast<std::uint32_t>(tile) << 16U;
    }

    struct GpuChunkMesh final
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
        UINT vertex_count{};
    };

    [[nodiscard]] std::vector<GpuVertex> build_vertices(
        const std::vector<world::WorldFace> &faces) const
    {
        std::vector<GpuVertex> vertices;
        vertices.reserve(faces.size() * 6U);
        constexpr std::array<int, 6> triangle_order{0, 1, 2, 0, 2, 3};
        for (const world::WorldFace &face : faces)
        {
            // Torch blocks render as the real torch prop through the entity
            // pass now — never as voxel geometry.
            if (is_torch_block(face.block))
            {
                continue;
            }
            const float x = static_cast<float>(face.x);
            const float y = static_cast<float>(face.y);
            const float z = static_cast<float>(face.z);
            std::array<std::array<float, 3>, 4> corners{};
            switch (face.direction)
            {
            case world::FaceDirection::positive_x:
                corners = {{{{x + 1.0F, y, z}},
                            {{x + 1.0F, y + 1.0F, z}},
                            {{x + 1.0F, y + 1.0F, z + 1.0F}},
                            {{x + 1.0F, y, z + 1.0F}}}};
                break;
            case world::FaceDirection::negative_z:
                corners = {{{{x + 1.0F, y, z}},
                            {{x, y, z}},
                            {{x, y + 1.0F, z}},
                            {{x + 1.0F, y + 1.0F, z}}}};
                break;
            case world::FaceDirection::negative_x:
                corners = {{{{x, y, z + 1.0F}},
                            {{x, y + 1.0F, z + 1.0F}},
                            {{x, y + 1.0F, z}},
                            {{x, y, z}}}};
                break;
            case world::FaceDirection::positive_z:
                corners = {{{{x, y, z + 1.0F}},
                            {{x + 1.0F, y, z + 1.0F}},
                            {{x + 1.0F, y + 1.0F, z + 1.0F}},
                            {{x, y + 1.0F, z + 1.0F}}}};
                break;
            case world::FaceDirection::positive_y:
                corners = {{{{x, y + 1.0F, z}},
                            {{x + 1.0F, y + 1.0F, z}},
                            {{x + 1.0F, y + 1.0F, z + 1.0F}},
                            {{x, y + 1.0F, z + 1.0F}}}};
                break;
            case world::FaceDirection::negative_y:
                corners = {{{{x, y, z + 1.0F}},
                            {{x + 1.0F, y, z + 1.0F}},
                            {{x + 1.0F, y, z}},
                            {{x, y, z}}}};
                break;
            }

            // Per-tile array slices: local 0..1 coordinates, the slice index
            // rides its own vertex channel.  No inset needed — the array clamp
            // keeps neighbouring tiles from ever bleeding, at any mip.
            const auto texture_coordinates =
                world::atlas_uv_corners(face.direction, 0.0F, 0.0F, 1.0F, 1.0F);
            const auto normal = face_normal(face.direction);
            const float gloss = block_gloss(face.block);
            const float wave = block_wave(face.block);
            for (const int corner : triangle_order)
            {
                const auto index = static_cast<std::size_t>(corner);
                vertices.push_back({corners[index][0], corners[index][1], corners[index][2],
                                    texture_coordinates[index][0], texture_coordinates[index][1],
                                    normal[0], normal[1], normal[2],
                                    static_cast<float>(face.ambient_occlusion[index]) / 255.0F,
                                    static_cast<float>(face.sky_light[index]) / 255.0F,
                                    static_cast<float>(face.emission) / 255.0F,
                                    static_cast<float>(face.block_light[index]) / 255.0F,
                                    pack_material(gloss, wave, face.tile & 63U),
                                    pack_unorm8(face.indirect_light),
                                    pack_unorm8(face.block_light_lobe)});
            }
        }
        return vertices;
    }

    void track_crash_site_faces(const std::tuple<int, int, int> &key,
                                const std::vector<world::WorldFace> &faces)
    {
        std::vector<std::array<int, 2>> columns;
        for (const world::WorldFace &face : faces)
        {
            if (face.block == world::BlockType::space_rock ||
                face.block == world::BlockType::space_rock_inventory)
                columns.push_back({face.x, face.z});
        }
        std::sort(columns.begin(), columns.end());
        columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
        if (columns.empty())
            crash_site_columns_.erase(key);
        else
            crash_site_columns_.insert_or_assign(key, std::move(columns));
        crater_masks_dirty_ = true;
    }

    void rebuild_crater_masks_if_needed()
    {
        if (!crater_masks_dirty_)
            return;
        crater_masks_ = {};
        crater_masks_dirty_ = false;

        std::vector<std::array<int, 2>> columns;
        for (const auto &[key, chunk_columns] : crash_site_columns_)
        {
            (void)key;
            columns.insert(columns.end(), chunk_columns.begin(), chunk_columns.end());
        }
        std::sort(columns.begin(), columns.end());
        columns.erase(std::unique(columns.begin(), columns.end()), columns.end());

        struct CraterCluster final
        {
            std::vector<std::array<int, 2>> points;
            float sum_x{};
            float sum_z{};
        };
        std::vector<CraterCluster> clusters;
        constexpr float cluster_distance = 34.0F;
        for (const auto &column : columns)
        {
            CraterCluster *nearest{};
            float nearest_distance_squared = cluster_distance * cluster_distance;
            for (CraterCluster &cluster : clusters)
            {
                const float inverse_count = 1.0F / static_cast<float>(cluster.points.size());
                const float dx = static_cast<float>(column[0]) - cluster.sum_x * inverse_count;
                const float dz = static_cast<float>(column[1]) - cluster.sum_z * inverse_count;
                const float distance_squared = dx * dx + dz * dz;
                if (distance_squared < nearest_distance_squared)
                {
                    nearest = &cluster;
                    nearest_distance_squared = distance_squared;
                }
            }
            if (nearest == nullptr)
            {
                clusters.push_back(
                    {{column}, static_cast<float>(column[0]), static_cast<float>(column[1])});
            }
            else
            {
                nearest->points.push_back(column);
                nearest->sum_x += static_cast<float>(column[0]);
                nearest->sum_z += static_cast<float>(column[1]);
            }
        }
        std::sort(clusters.begin(), clusters.end(), [](const auto &left, const auto &right) {
            return left.points.size() > right.points.size();
        });
        const std::size_t mask_count = std::min(crater_masks_.size(), clusters.size());
        for (std::size_t index = 0; index < mask_count; ++index)
        {
            const CraterCluster &cluster = clusters[index];
            const float inverse_count = 1.0F / static_cast<float>(cluster.points.size());
            const float center_x = cluster.sum_x * inverse_count + 0.5F;
            const float center_z = cluster.sum_z * inverse_count + 0.5F;
            float radius = 0.0F;
            for (const auto &point : cluster.points)
            {
                radius =
                    std::max(radius, std::hypot(static_cast<float>(point[0]) + 0.5F - center_x,
                                                static_cast<float>(point[1]) + 0.5F - center_z));
            }
            // The authored impact rim extends beyond the exposed SpaceRock.
            // Leave a modest dry margin so the meteor reads as a crater rather
            // than an island sitting in the Lagoon plane.
            crater_masks_[index] = {center_x, center_z, std::clamp(radius + 6.0F, 8.0F, 30.0F),
                                    1.0F};
        }
    }

    [[nodiscard]] bool create_vertex_buffer(const std::vector<GpuVertex> &vertices,
                                            Microsoft::WRL::ComPtr<ID3D11Buffer> &destination) const
    {
        destination.Reset();
        if (vertices.empty())
        {
            return true;
        }
        const D3D11_BUFFER_DESC description{static_cast<UINT>(vertices.size() * sizeof(GpuVertex)),
                                            D3D11_USAGE_IMMUTABLE,
                                            D3D11_BIND_VERTEX_BUFFER,
                                            0U,
                                            0U,
                                            sizeof(GpuVertex)};
        const D3D11_SUBRESOURCE_DATA data{vertices.data(), 0U, 0U};
        return SUCCEEDED(device_->CreateBuffer(&description, &data, &destination));
    }

    void rebuild_water_buffer(int minimum_x, int minimum_z, int maximum_x, int maximum_z)
    {
        const float x0 = static_cast<float>(minimum_x);
        const float z0 = static_cast<float>(minimum_z);
        const float x1 = static_cast<float>(maximum_x + 1);
        const float z1 = static_cast<float>(maximum_z + 1);
        const std::vector<GpuVertex> vertices{{
            {x0, world::water_level, z0, x0 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z1, x1 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z0, x1 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x0, world::water_level, z0, x0 * 0.035F, z0 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x0, world::water_level, z1, x0 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
            {x1, world::water_level, z1, x1 * 0.035F, z1 * 0.035F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
             -1.0F},
        }};
        water_vertex_count_ = static_cast<UINT>(vertices.size());
        if (!create_vertex_buffer(vertices, water_vertex_buffer_))
        {
            available_ = false;
            failure_reason_ = "failed to upload the chunk-window water mesh";
        }
    }

    struct Float4 final
    {
        float x{};
        float y{};
        float z{};
        float w{};
    };

    struct alignas(16) SceneConstants final
    {
        // xyz = camera position; w enables the Rebirth Hell atmosphere. Keeping
        // this diagnostic switch in an otherwise-unused component lets matched
        // captures compare the complete legacy and modern deep-lighting paths.
        Float4 eye;
        Float4 right;
        Float4 up;
        Float4 forward;
        Float4 projection;
        Float4 light_and_fog_start;
        Float4 fog_and_end;
        Float4 water_and_time;
        Float4 sun_and_options;
        Float4 render_options;
        // Original BlockTerrain.TorchColor (255, 235, 190) in rgb; w carries the
        // held-torch strength that lights the player's surroundings.
        Float4 torch_and_eye;
        // LightColorPack.ambient for the current time of day (AmbientSun).
        Float4 ambient_sun;
        // Sun shadow map: world position folds into shadow-map UV+depth through
        // three dot products (xyz = axis, w = offset), so both the depth-only
        // pass and the lighting pass agree without a full matrix.
        Float4 shadow_x;
        Float4 shadow_y;
        Float4 shadow_z;
        // x = shadow strength, y = shadow texel size, z = solar-horizon
        // visibility (zero below the Rebirth horizon), w = reserved.
        Float4 shadow_params;
        // xy = sun position in screen UV, z = camera-forward sun depth
        // (positive when the sun is in front of the camera), w = daylight.
        Float4 post_sun;
        // x = bloom strength, y = god-ray strength, z = tone curve index,
        // w = time.
        Float4 post_params;
        // Render target extent: xy = pixels, zw = 1 / pixels.
        Float4 target_size;
        // x = manual exposure multiplier (2^EV), y = colour-grading strength,
        // z = bloom threshold in exposed space, w = auto-exposure enable.
        Float4 grade_params;
        // Cascades two and three retain the same dot-product transform as the
        // near map. shadow_cascades.xyz are camera-depth split distances;
        // shadow_cascades.w selects the slice while rendering the depth atlas.
        Float4 shadow_x2;
        Float4 shadow_y2;
        Float4 shadow_z2;
        Float4 shadow_x3;
        Float4 shadow_y3;
        Float4 shadow_z3;
        Float4 shadow_cascades;
        // xyz = integer world-cell origin, w = active. Params x = extent,
        // y = reciprocal extent, z = terrain gather strength.
        Float4 dynamic_radiance_origin;
        Float4 dynamic_radiance_params;
        // Display-referred biome grade. Tint w is blend strength; params x is
        // the target saturation and params y enables the presentation-only ore
        // glow. This stays after every lighting constant so existing
        // terrain/entity shader layouts retain their exact offsets.
        Float4 biome_tint;
        Float4 biome_params;
        // xy = crash-site centre, z = dry radius, w = active. Lagoon water is
        // clipped around authored meteor impacts instead of flooding them.
        std::array<Float4, maximum_crater_masks> crater_masks;
    };

    struct D3DStateBackup final
    {
        explicit D3DStateBackup(ID3D11DeviceContext *context)
        {
            context->OMGetRenderTargets(1U, render_target.ReleaseAndGetAddressOf(),
                                        depth_target.ReleaseAndGetAddressOf());
            context->OMGetDepthStencilState(depth_state.ReleaseAndGetAddressOf(), &stencil_ref);
            context->OMGetBlendState(blend_state.ReleaseAndGetAddressOf(), blend_factor.data(),
                                     &sample_mask);
            context->RSGetState(rasterizer_state.ReleaseAndGetAddressOf());
            viewport_count = static_cast<UINT>(viewports.size());
            context->RSGetViewports(&viewport_count, viewports.data());
            context->IAGetInputLayout(input_layout.ReleaseAndGetAddressOf());
            context->IAGetPrimitiveTopology(&topology);
            context->IAGetVertexBuffers(0U, 1U, vertex_buffer.ReleaseAndGetAddressOf(), &stride,
                                        &offset);
            UINT class_instance_count{};
            context->VSGetShader(vertex_shader.ReleaseAndGetAddressOf(), nullptr,
                                 &class_instance_count);
            class_instance_count = 0U;
            context->PSGetShader(pixel_shader.ReleaseAndGetAddressOf(), nullptr,
                                 &class_instance_count);
            context->VSGetConstantBuffers(0U, 1U, vertex_constants.ReleaseAndGetAddressOf());
            context->PSGetConstantBuffers(0U, 1U, pixel_constants.ReleaseAndGetAddressOf());
            std::array<ID3D11ShaderResourceView *, 9> resource_pointers{};
            context->PSGetShaderResources(0U, static_cast<UINT>(resource_pointers.size()),
                                          resource_pointers.data());
            for (std::size_t index = 0U; index < resource_pointers.size(); ++index)
            {
                pixel_resources[index].Attach(resource_pointers[index]);
            }
            context->PSGetSamplers(0U, 1U, pixel_sampler.ReleaseAndGetAddressOf());
        }

        void restore(ID3D11DeviceContext *context) const
        {
            ID3D11RenderTargetView *render_target_pointer = render_target.Get();
            context->OMSetRenderTargets(1U, &render_target_pointer, depth_target.Get());
            context->OMSetDepthStencilState(depth_state.Get(), stencil_ref);
            context->OMSetBlendState(blend_state.Get(), blend_factor.data(), sample_mask);
            context->RSSetState(rasterizer_state.Get());
            context->RSSetViewports(viewport_count, viewports.data());
            context->IASetInputLayout(input_layout.Get());
            context->IASetPrimitiveTopology(topology);
            ID3D11Buffer *vertex_buffer_pointer = vertex_buffer.Get();
            context->IASetVertexBuffers(0U, 1U, &vertex_buffer_pointer, &stride, &offset);
            context->VSSetShader(vertex_shader.Get(), nullptr, 0U);
            context->PSSetShader(pixel_shader.Get(), nullptr, 0U);
            ID3D11Buffer *vertex_constants_pointer = vertex_constants.Get();
            ID3D11Buffer *pixel_constants_pointer = pixel_constants.Get();
            context->VSSetConstantBuffers(0U, 1U, &vertex_constants_pointer);
            context->PSSetConstantBuffers(0U, 1U, &pixel_constants_pointer);
            const std::array pixel_resource_pointers{
                pixel_resources[0].Get(), pixel_resources[1].Get(), pixel_resources[2].Get(),
                pixel_resources[3].Get(), pixel_resources[4].Get(), pixel_resources[5].Get(),
                pixel_resources[6].Get(), pixel_resources[7].Get(), pixel_resources[8].Get()};
            context->PSSetShaderResources(0U, static_cast<UINT>(pixel_resource_pointers.size()),
                                          pixel_resource_pointers.data());
            ID3D11SamplerState *pixel_sampler_pointer = pixel_sampler.Get();
            context->PSSetSamplers(0U, 1U, &pixel_sampler_pointer);
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_target;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_constants;
        Microsoft::WRL::ComPtr<ID3D11Buffer> pixel_constants;
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 9> pixel_resources;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> pixel_sampler;
        std::array<float, 4> blend_factor{};
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
            viewports{};
        UINT viewport_count{};
        UINT sample_mask{};
        UINT stencil_ref{};
        UINT stride{};
        UINT offset{};
        D3D11_PRIMITIVE_TOPOLOGY topology{};
    };

    [[nodiscard]] static std::array<float, 3> face_normal(world::FaceDirection direction) noexcept
    {
        switch (direction)
        {
        case world::FaceDirection::positive_x:
            return {1.0F, 0.0F, 0.0F};
        case world::FaceDirection::negative_z:
            return {0.0F, 0.0F, -1.0F};
        case world::FaceDirection::negative_x:
            return {-1.0F, 0.0F, 0.0F};
        case world::FaceDirection::positive_z:
            return {0.0F, 0.0F, 1.0F};
        case world::FaceDirection::positive_y:
            return {0.0F, 1.0F, 0.0F};
        case world::FaceDirection::negative_y:
            return {0.0F, -1.0F, 0.0F};
        }
        return {0.0F, 1.0F, 0.0F};
    }

    // Copies each of the 64 atlas tiles into its own array slice and generates
    // a full mip chain per slice on the GPU.
    [[nodiscard]] bool build_tile_array(ID3D11Texture2D *source,
                                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view)
    {
        if (source == nullptr)
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC source_description{};
        source->GetDesc(&source_description);
        const UINT tile = source_description.Width / 8U;
        if (tile == 0U || source_description.Height / 8U != tile)
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC array_description{};
        array_description.Width = tile;
        array_description.Height = tile;
        array_description.MipLevels = 0U;
        array_description.ArraySize = 64U;
        array_description.Format = source_description.Format;
        array_description.SampleDesc.Count = 1U;
        array_description.Usage = D3D11_USAGE_DEFAULT;
        array_description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        array_description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> array_texture;
        if (FAILED(device_->CreateTexture2D(&array_description, nullptr, &array_texture)))
        {
            return false;
        }
        array_texture->GetDesc(&array_description);
        for (UINT slice = 0U; slice < 64U; ++slice)
        {
            const UINT tile_x = (slice & 7U) * tile;
            const UINT tile_y = (slice >> 3U) * tile;
            const D3D11_BOX box{tile_x, tile_y, 0U, tile_x + tile, tile_y + tile, 1U};
            context_->CopySubresourceRegion(
                array_texture.Get(), D3D11CalcSubresource(0U, slice, array_description.MipLevels),
                0U, 0U, 0U, source, 0U, &box);
        }
        if (FAILED(device_->CreateShaderResourceView(array_texture.Get(), nullptr, &view)))
        {
            return false;
        }
        context_->GenerateMips(view.Get());
        tile_array_textures_.push_back(std::move(array_texture));
        return true;
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(const char *source,
                                                                  const char *entry,
                                                                  const char *target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result =
            D3DCompile(source, std::char_traits<char>::length(source), nullptr, nullptr, nullptr,
                       entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0U, &shader, &errors);
        if (FAILED(result))
        {
            failure_reason_ =
                errors != nullptr
                    ? std::string{static_cast<const char *>(errors->GetBufferPointer()),
                                  errors->GetBufferSize()}
                    : std::format("D3DCompile failed with HRESULT 0x{:08X}",
                                  static_cast<unsigned int>(result));
            return {};
        }
        return shader;
    }

    [[nodiscard]] bool create_pipeline()
    {
        static constexpr char vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float ambient_occlusion : TEXCOORD1;
    float sky_light : TEXCOORD2;
    float emission : TEXCOORD3;
    float torch_light : TEXCOORD4;
    // r = gloss, g = foliage wave, b * 255 = texture-array slice.
    float4 material : TEXCOORD5;
    float4 indirect_light : TEXCOORD6;
    float4 block_light_lobe : TEXCOORD7;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float ambient_occlusion : TEXCOORD1;
    float sky_light : TEXCOORD2;
    float emission : TEXCOORD3;
    float3 world_position : TEXCOORD4;
    float torch_light : TEXCOORD5;
    float gloss : TEXCOORD6;
    float tile : TEXCOORD7;
    float4 indirect_light : TEXCOORD8;
    float4 block_light_lobe : TEXCOORD9;
};

float3 swayed_position(float3 position, float wave, float time)
{
    float phase = time * 1.15f + position.x * 0.9f + position.z * 0.8f + position.y * 0.35f;
    float2 sway = float2(sin(phase), cos(phase * 0.83f + 1.7f)) * 0.05f * wave;
    return position + float3(sway.x, sin(phase * 0.5f) * 0.02f * wave, sway.y);
}

PixelInput main(VertexInput input)
{
    PixelInput output;
    float3 world = swayed_position(input.position, input.material.g, WaterAndTime.x);
    float3 relative = world - Eye.xyz;
    float camera_x = dot(relative, CameraRight.xyz);
    float camera_y = dot(relative, CameraUp.xyz);
    float camera_z = dot(relative, CameraForward.xyz);
    output.position = float4(camera_x * Projection.x,
                             camera_y * Projection.y,
                             camera_z * Projection.z + Projection.w,
                             camera_z);
    output.uv = input.uv;
    output.normal = input.normal;
    output.ambient_occlusion = input.ambient_occlusion;
    output.sky_light = input.sky_light;
    output.emission = input.emission;
    output.world_position = world;
    output.torch_light = input.torch_light;
    output.gloss = input.material.r;
    output.tile = input.material.b * 255.0f;
    output.indirect_light = input.indirect_light;
    output.block_light_lobe = input.block_light_lobe;
    return output;
}
)hlsl";
        static constexpr char pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
    float4 DynamicRadianceOrigin;
    float4 DynamicRadianceParams;
    float4 BiomeTint;
    float4 BiomeParams;
    float4 CraterMasks[32];
};

Texture2DArray DiffuseAtlas : register(t0);
Texture2DArray NormalSpecAtlas : register(t1);
Texture2DArray MetalLightAtlas : register(t2);
Texture2DArray ShadowMap : register(t3);
Texture2D SceneCopy : register(t4);
Texture2D SceneDepth : register(t5);
Texture2DArray PointShadowMap : register(t6);
Texture3D<float4> DynamicRadiance : register(t7);
Texture2DArray ShadowTransmission : register(t8);
SamplerState AtlasSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState AnisoSampler : register(s2);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float ambient_occlusion : TEXCOORD1;
    float sky_light : TEXCOORD2;
    float emission : TEXCOORD3;
    float3 world_position : TEXCOORD4;
    float torch_light : TEXCOORD5;
    float gloss : TEXCOORD6;
    float tile : TEXCOORD7;
    float4 indirect_light : TEXCOORD8;
    float4 block_light_lobe : TEXCOORD9;
    bool front_face : SV_IsFrontFace;
};

float4 dynamic_radiance_cell(float3 world_position)
{
    if (DynamicRadianceOrigin.w < 0.5f)
        return 0.0f.xxxx;
    int extent = (int)(DynamicRadianceParams.x + 0.5f);
    int3 cell = int3(floor(world_position)) - int3(DynamicRadianceOrigin.xyz);
    if (any(cell < int3(0, 0, 0)) || any(cell >= int3(extent, extent, extent)))
        return 0.0f.xxxx;
    float4 packed = DynamicRadiance.Load(int4(cell, 0));
    return packed * packed;
}

float3 dynamic_radiance_seed(float3 world_position)
{
    return dynamic_radiance_cell(world_position).rgb;
}

float3 dynamic_radiance_gather(float3 world_position, float3 normal,
                               float3 tangent, float3 bitangent)
{
    if (DynamicRadianceOrigin.w < 0.5f)
        return 0.0f.xxx;
    float3 radiance = dynamic_radiance_seed(world_position + normal * 2.0f) * 0.18f;
    radiance += dynamic_radiance_seed(world_position + normal * 4.0f) * 0.14f;
    radiance += dynamic_radiance_seed(
        world_position + normalize(normal + tangent) * 3.0f) * 0.12f;
    radiance += dynamic_radiance_seed(
        world_position + normalize(normal - tangent) * 3.0f) * 0.12f;
    radiance += dynamic_radiance_seed(
        world_position + normalize(normal + bitangent) * 3.0f) * 0.12f;
    radiance += dynamic_radiance_seed(
        world_position + normalize(normal - bitangent) * 3.0f) * 0.12f;
    return radiance * DynamicRadianceParams.z;
}

void cascade_transform(int cascade, out float4 sx, out float4 sy, out float4 sz)
{
    if (cascade == 0)
    {
        sx = ShadowX;
        sy = ShadowY;
        sz = ShadowZ;
    }
    else if (cascade == 1)
    {
        sx = ShadowX2;
        sy = ShadowY2;
        sz = ShadowZ2;
    }
    else
    {
        sx = ShadowX3;
        sy = ShadowY3;
        sz = ShadowZ3;
    }
}

float cascade_visibility(int cascade, float3 world_position, float3 geometric_normal)
{
    float4 sx;
    float4 sy;
    float4 sz;
    cascade_transform(cascade, sx, sy, sz);
    float texel_world = ShadowParams.y / max(length(sx.xyz), 0.0001f);
    float3 sun_direction_n = normalize(SunAndOptions.xyz);
    float facing_sun = dot(geometric_normal, sun_direction_n);
    if (facing_sun <= 0.05f)
        return 1.0f;
    float slope = 1.0f - saturate(facing_sun);
    float3 biased = world_position + geometric_normal * texel_world * (1.15f + 2.4f * slope) +
                    sun_direction_n * texel_world * 0.35f;
    float2 uv = float2(dot(biased, sx.xyz) + sx.w, dot(biased, sy.xyz) + sy.w);
    float depth = dot(biased, sz.xyz) + sz.w;
    if (any(uv < 0.002f) || any(uv > 0.998f))
        return 1.0f;
    float texel = ShadowParams.y;
    float compare_bias = length(sz.xyz) * texel_world * (0.65f + slope * 1.35f);
    float receiver_depth = depth - compare_bias;

    // Diagnostic parity path for matched captures/benchmarks.
    if (ShadowCascades.w < -0.5f)
    {
        float legacy_visibility = 0.0f;
        float legacy_kernel = cascade == 0 ? 1.35f : (cascade == 1 ? 1.15f : 0.95f);
        [unroll]
        for (int row = -2; row <= 2; ++row)
        {
            [unroll]
            for (int column = -2; column <= 2; ++column)
            {
                float2 tap = uv + float2(column, row) * texel * legacy_kernel;
                float sample_visibility = ShadowMap.SampleCmpLevelZero(
                    ShadowSampler, float3(tap, (float)cascade), receiver_depth);
                float transmission = ShadowTransmission.SampleLevel(
                    AtlasSampler, float3(tap, (float)cascade), 0.0f).r;
                legacy_visibility += lerp(sample_visibility, 1.0f, transmission);
            }
        }
        return legacy_visibility / 25.0f;
    }

    // Small blocker search followed by a rotated Poisson PCF disk. Unlike a
    // fixed square blur, this approximates the finite angular size of the sun:
    // contact shadows remain crisp and the penumbra grows with the actual
    // light-space separation between caster and receiver. The rotation is
    // anchored to voxel coordinates, so it never swims with the camera.
    float blocker_depth = 0.0f;
    float blocker_count = 0.0f;
    static const float2 blocker_offsets[5] = {
        float2(0.0f, 0.0f), float2(-1.0f, 0.0f), float2(1.0f, 0.0f),
        float2(0.0f, -1.0f), float2(0.0f, 1.0f)
    };
    if (cascade < 2)
    {
        [unroll]
        for (int blocker_index = 0; blocker_index < 5; ++blocker_index)
        {
            float2 search_uv = clamp(uv + blocker_offsets[blocker_index] * texel * 2.0f,
                                     texel * 0.5f, 1.0f - texel * 0.5f);
            float stored = ShadowMap.SampleLevel(
                AtlasSampler, float3(search_uv, (float)cascade), 0.0f).r;
            if (stored < receiver_depth - compare_bias * 0.25f)
            {
                blocker_depth += stored;
                blocker_count += 1.0f;
            }
        }
    }
    float blocker_distance = 0.0f;
    if (blocker_count > 0.0f)
    {
        float average_blocker = blocker_depth / blocker_count;
        blocker_distance = max(0.0f, receiver_depth - average_blocker) /
                           max(length(sz.xyz), 0.00001f);
    }
    float filter_radius = blocker_count > 0.0f
        ? clamp(0.72f + blocker_distance * 0.0045f / max(texel_world, 0.0001f),
                0.72f, 3.25f)
        : (cascade == 2 ? 0.95f : 0.72f);

    static const float2 poisson[8] = {
        float2( 0.000f,  0.000f), float2(-0.326f, -0.406f),
        float2(-0.840f, -0.074f), float2(-0.696f,  0.457f),
        float2(-0.203f,  0.621f), float2( 0.962f, -0.195f),
        float2( 0.473f, -0.480f), float2( 0.519f,  0.767f)
    };
    float3 stable_cell = floor(world_position * 2.0f);
    float orientation = frac(dot(stable_cell, float3(0.1031f, 0.11369f, 0.13787f)));
    float visibility = 0.0f;
    [unroll]
    for (int sample_index = 0; sample_index < 8; ++sample_index)
    {
        float2 disk = poisson[sample_index];
        float2 rotated = orientation < 0.5f ? disk : disk.yx;
        rotated.x *= frac(orientation * 2.0f) < 0.5f ? -1.0f : 1.0f;
        rotated.y *= frac(orientation * 4.0f) < 0.5f ? -1.0f : 1.0f;
        float2 tap = clamp(uv + rotated * texel * filter_radius,
                           texel * 0.5f, 1.0f - texel * 0.5f);
        float sample_visibility = ShadowMap.SampleCmpLevelZero(
            ShadowSampler, float3(tap, (float)cascade), receiver_depth);
        float transmission = ShadowTransmission.SampleLevel(
            AtlasSampler, float3(tap, (float)cascade), 0.0f).r;
        visibility += lerp(sample_visibility, 1.0f, transmission);
    }
    return visibility / 8.0f;
}

float canopy_shadow_debug_value(float3 world_position, float3 geometric_normal)
{
    float camera_depth = max(dot(world_position - Eye.xyz, CameraForward.xyz), 0.0f);
    int cascade = camera_depth < ShadowCascades.x ? 0 :
                  (camera_depth < ShadowCascades.y ? 1 : 2);
    float4 sx;
    float4 sy;
    float4 sz;
    cascade_transform(cascade, sx, sy, sz);
    float texel_world = ShadowParams.y / max(length(sx.xyz), 0.0001f);
    float3 sun_direction = normalize(SunAndOptions.xyz);
    float slope = 1.0f - saturate(dot(geometric_normal, sun_direction));
    float3 biased = world_position + geometric_normal * texel_world * (1.15f + 2.4f * slope) +
                    sun_direction * texel_world * 0.35f;
    float2 uv = float2(dot(biased, sx.xyz) + sx.w, dot(biased, sy.xyz) + sy.w);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 0.0f;
    return ShadowTransmission.SampleLevel(
        AtlasSampler, float3(uv, (float)cascade), 0.0f).r;
}

// Percentage-closer filtered visibility of the sun at a world position.
float sun_shadow(float3 world_position, float3 geometric_normal, float distance_to_eye)
{
    if (ShadowParams.x <= 0.0f || ShadowParams.z <= 0.0f)
        return 1.0f;
    float camera_depth = max(dot(world_position - Eye.xyz, CameraForward.xyz), 0.0f);
    int cascade = camera_depth < ShadowCascades.x ? 0 :
                  (camera_depth < ShadowCascades.y ? 1 : 2);
    float visibility = cascade_visibility(cascade, world_position, geometric_normal);

    if (cascade < 2)
    {
        float split_start = cascade == 0 ? 0.0f : ShadowCascades.x;
        float split_end = cascade == 0 ? ShadowCascades.x : ShadowCascades.y;
        float transition = saturate((camera_depth - lerp(split_start, split_end, 0.86f)) /
                                    max((split_end - split_start) * 0.14f, 0.001f));
        if (transition > 0.0f)
            visibility = lerp(visibility,
                              cascade_visibility(cascade + 1, world_position, geometric_normal),
                              transition);
    }
    float far_fade = saturate((camera_depth - ShadowCascades.z * 0.88f) /
                              max(ShadowCascades.z * 0.12f, 0.001f));
    visibility = lerp(visibility, 1.0f, far_fade);
    // Cast shadows remove direct sun only. Their opacity is intentionally bounded
    // so a ridge cannot black out a valley that still sees the sky hemisphere.
    float shadow_opacity = lerp(0.18f, 0.43f, saturate(ShadowParams.x)) *
                           saturate(ShadowParams.z);
    return lerp(1.0f, visibility, shadow_opacity);
}

void point_shadow_face(float3 relative, out int face, out float2 projected,
                       out float forward)
{
    float3 axis = abs(relative);
    if (axis.x >= axis.y && axis.x >= axis.z)
    {
        if (relative.x >= 0.0f)
        {
            face = 0;
            projected = float2(-relative.z, relative.y);
            forward = relative.x;
        }
        else
        {
            face = 1;
            projected = float2(relative.z, relative.y);
            forward = -relative.x;
        }
    }
    else if (axis.y >= axis.z)
    {
        if (relative.y >= 0.0f)
        {
            face = 2;
            projected = float2(relative.x, -relative.z);
            forward = relative.y;
        }
        else
        {
            face = 3;
            projected = float2(relative.x, relative.z);
            forward = -relative.y;
        }
    }
    else if (relative.z >= 0.0f)
    {
        face = 4;
        projected = float2(relative.x, relative.y);
        forward = relative.z;
    }
    else
    {
        face = 5;
        projected = float2(-relative.x, relative.y);
        forward = -relative.z;
    }
}

// True point-light visibility from the eye-mounted torch. Six low-resolution
// depth faces cover the complete nine-unit light volume; this is geometric
// voxel occlusion, not a screen-space darkening mask.
float held_torch_shadow(float3 world_position, float3 geometric_normal)
{
    if (TorchAndEye.w <= 0.0f || AmbientSun.w <= 0.5f || ShadowParams.w <= 0.0f)
        return 1.0f;
    float3 relative = world_position - Eye.xyz;
    float range = length(relative);
    if (range <= 0.10f || range >= 9.0f)
        return 1.0f;
    int face;
    float2 projected;
    float forward;
    point_shadow_face(relative, face, projected, forward);
    const float near_plane = 0.08f;
    const float far_plane = 9.25f;
    if (forward <= near_plane)
        return 1.0f;
    float2 uv = float2(0.5f + 0.5f * projected.x / forward,
                       0.5f - 0.5f * projected.y / forward);
    float edge_guard = ShadowParams.w * 0.75f;
    uv = clamp(uv, edge_guard, 1.0f - edge_guard);
    float3 to_light = -relative / range;
    float slope = 1.0f - saturate(dot(geometric_normal, to_light));
    float biased_forward = max(near_plane, forward - (0.035f + slope * 0.070f));
    float compare_depth = far_plane / (far_plane - near_plane) -
                          near_plane * far_plane /
                              ((far_plane - near_plane) * biased_forward);
    if (ShadowParams.x < -1.5f)
    {
        float stored_depth = PointShadowMap.SampleLevel(
            AtlasSampler, float3(uv, (float)face), 0.0f).r;
        return saturate(0.5f + (compare_depth - stored_depth) * 40.0f);
    }
    float visibility = 0.0f;
    [unroll]
    for (int row = -1; row <= 1; ++row)
    {
        [unroll]
        for (int column = -1; column <= 1; ++column)
        {
            float2 tap = clamp(uv + float2(column, row) * ShadowParams.w * 1.15f,
                               ShadowParams.w * 0.5f,
                               1.0f - ShadowParams.w * 0.5f);
            visibility += PointShadowMap.SampleCmpLevelZero(
                ShadowSampler, float3(tap, (float)face), compare_depth);
        }
    }
    visibility /= 9.0f;
    // Preserve a small indirect floor while removing almost all blocked direct
    // energy; completely black torch shadows look cut out in a voxel cave.
    return lerp(0.12f, 1.0f, visibility);
}

float hash31(float3 value)
{
    value = frac(value * 0.1031f);
    value += dot(value, value.yzx + 33.33f);
    return frac((value.x + value.y) * value.z);
}

float4 main(PixelInput input) : SV_TARGET
{
    float distance_to_eye = distance(input.world_position, Eye.xyz);
    float horizontal_distance_to_eye = distance(input.world_position.xz, Eye.xz);
    float fog = saturate((horizontal_distance_to_eye - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    // Hell has suspended ash and heat haze even before the normal streaming-edge
    // curtain. It reveals depth as warm silhouettes instead of raising every
    // cave surface with a flat ambient term. Eye.w is zero for Classic and for
    // the matched legacy diagnostic path.
    float hell_depth = Eye.w * saturate((-32.0f - Eye.y) / 8.0f);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance_to_eye * 0.032f)) * 0.62f;
    fog = max(fog, hell_air);
    if (input.emission < -0.5f)
    {
        [unroll]
        for (int crater = 0; crater < 32; ++crater)
        {
            if (CraterMasks[crater].w > 0.5f)
                clip(distance(input.world_position.xz, CraterMasks[crater].xy) -
                     CraterMasks[crater].z);
        }
        // SEUS-style water: an animated normal field drives a screen-space
        // refraction of the already-rendered scene, a procedural sky
        // reflection, and a hard sun glint, mixed by fresnel.
        float time = WaterAndTime.x;
        float2 p = input.world_position.xz;
        float2 grad = float2(0.0f, 0.0f);
        grad += float2(0.9f, 0.36f) * cos(dot(p, float2(0.9f, 0.36f)) * 1.9f + time * 1.35f) * 0.045f;
        grad += float2(-0.42f, 0.91f) * cos(dot(p, float2(-0.42f, 0.91f)) * 3.1f - time * 1.1f) * 0.03f;
        grad += float2(0.65f, -0.76f) * cos(dot(p, float2(0.65f, -0.76f)) * 5.3f + time * 1.9f) * 0.02f;
        grad += float2(-0.94f, -0.34f) * cos(dot(p, float2(-0.94f, -0.34f)) * 9.7f - time * 2.6f) * 0.012f;
        float3 wave_normal = normalize(float3(-grad.x, 1.0f, -grad.y));
        float3 view_direction = normalize(Eye.xyz - input.world_position);
        bool under_water = WaterAndTime.z > 0.5f;
        float3 facing_normal = under_water ? -wave_normal : wave_normal;
        float fresnel = 0.02f + 0.98f * pow(1.0f - saturate(dot(facing_normal, view_direction)), 5.0f);

        float2 screen_uv = input.position.xy * TargetSize.zw;
        float distortion = 0.06f / max(1.0f, distance_to_eye * 0.22f);
        float2 refracted_uv = saturate(screen_uv + wave_normal.xz * distortion);
        float4 behind = SceneCopy.Sample(AtlasSampler, refracted_uv);
        float behind_depth = SceneDepth.Sample(AtlasSampler, refracted_uv).r;
        float behind_camera_z = Projection.w / min(behind_depth - Projection.z, -0.00001f);
        float thickness = max(0.0f, behind_camera_z - input.position.w);
        float3 absorption = exp(-thickness * float3(0.42f, 0.16f, 0.11f));
        float daylight = 0.35f + WaterAndTime.w * 0.65f;
        float3 deep_color = float3(0.015f, 0.09f, 0.14f) * daylight;
        float3 refraction = lerp(FogAndEnd.rgb, behind.rgb, behind.a);
        refraction = lerp(deep_color, refraction * absorption, exp(-thickness * 0.06f));

        float3 reflect_direction = reflect(-view_direction, wave_normal);
        float sky_band = saturate(reflect_direction.y);
        float3 sky_reflection = lerp(FogAndEnd.rgb,
                                     lerp(FogAndEnd.rgb, AmbientSun.rgb + float3(0.06f, 0.10f, 0.16f),
                                          0.6f) * daylight,
                                     sky_band);
        float3 sun_direction = normalize(SunAndOptions.xyz);
        float glint = pow(saturate(dot(reflect_direction, sun_direction)), 720.0f) * 2.6f +
                      pow(saturate(dot(reflect_direction, sun_direction)), 48.0f) * 0.22f;
        float shadow = sun_shadow(input.world_position, float3(0.0f, 1.0f, 0.0f), distance_to_eye);
        float3 reflection = sky_reflection + LightAndFogStart.rgb * glint * WaterAndTime.w * shadow;

        float3 water;
        if (under_water)
        {
            water = lerp(refraction, deep_color, 0.35f);
        }
        else
        {
            water = lerp(refraction, reflection, saturate(fresnel * 1.05f));
        }
        water = lerp(water, FogAndEnd.rgb, fog);
        return float4(water, 1.0f);
    }
    float3 tile_uv = float3(input.uv, input.tile);
    float4 texel = DiffuseAtlas.Sample(AnisoSampler, tile_uv);
    clip(texel.a - 0.20f);
    bool modern_lighting = AmbientSun.w > 0.5f;
    bool enhanced_leaf = AmbientSun.w > 0.5f && abs(input.tile - 17.0f) < 0.25f;
    float3 geometric_normal = normalize(input.normal);
    if (modern_lighting)
    {
        // A second, lower-frequency world-anchored read breaks the obvious
        // one-texture-per-cube repetition. Because the UV does not reset at
        // voxel boundaries, neighbouring faces of the same material form one
        // continuous patch instead of a checkerboard of cloned tiles.
        float2 world_uv = abs(geometric_normal.y) > 0.5f
                              ? input.world_position.xz
                              : (abs(geometric_normal.x) > 0.5f
                                     ? input.world_position.zy
                                     : input.world_position.xy);
        float2 material_offset = frac(float2(input.tile * 0.371f, input.tile * 0.613f));
        float4 macro_texel = DiffuseAtlas.Sample(
            AnisoSampler, float3(world_uv * 0.47f + material_offset, input.tile));
        float macro_blend = enhanced_leaf ? 0.28f : 0.12f;
        texel.rgb = lerp(texel.rgb, macro_texel.rgb, macro_blend);
    }
    if (enhanced_leaf)
    {
        // The source leaf sheet is fully opaque. Rebirth derives restrained,
        // stable pores from its own darkest detail so canopies have depth and
        // visible sky without dissolving into hashed noise at mip distance.
        // The 0.11 cutoff removes the genuinely dark gaps in the installed
        // tile; stacked blocks then build back a dense but visibly porous crown.
        // Sample the mask one-and-a-half mip levels sharper than the colour.
        // Ordinary color mips average the dark gaps away long before a leaf
        // block becomes small on screen, rebuilding a solid cube silhouette.
        float3 leaf_mask = DiffuseAtlas.SampleBias(AtlasSampler, tile_uv, -1.5f).rgb;
        float leaf_luma = dot(leaf_mask, float3(0.2126f, 0.7152f, 0.0722f));
        clip(leaf_luma - 0.11f);
        // Keep the authored chlorophyll values. A large albedo boost turns the
        // sun-facing canopy chalky once the indirect-light layer is present.
        texel.rgb *= 1.03f;
    }
    // Source diffuse sheets are display-referred. Rebirth follows the same
    // linear-light contract as OpenSpades' physical block path: decode the
    // albedo before any irradiance, fog, bloom, or tone-map operation. Gamma
    // two is a deliberate fast approximation and is paired with sqrt in the
    // final composite. Material data atlases stay unmodified/linear.
    if (modern_lighting)
        texel.rgb *= texel.rgb;
    if (enhanced_leaf && !input.front_face)
        geometric_normal = -geometric_normal;
    float3 tangent;
    float3 bitangent;
    if (abs(geometric_normal.y) > 0.5f)
    {
        tangent = float3(geometric_normal.y, 0.0f, 0.0f);
        bitangent = float3(0.0f, 0.0f, 1.0f);
    }
    else if (abs(geometric_normal.x) > 0.5f)
    {
        tangent = float3(0.0f, 0.0f, -geometric_normal.x);
        bitangent = float3(0.0f, 1.0f, 0.0f);
    }
    else
    {
        tangent = float3(geometric_normal.z, 0.0f, 0.0f);
        bitangent = float3(0.0f, 1.0f, 0.0f);
    }
    float4 normal_spec = NormalSpecAtlas.Sample(AnisoSampler, tile_uv);
    // The generated height normals supply small-scale depth, not rounded plastic
    // bevels. A restrained slope preserves the authored texel structure and lets
    // the geometric voxel faces remain legible at distance.
    // The source normal sheet was authored for the original very flat diffuse
    // effect. At modern HDR contrast, its full slope makes soil and stone look
    // wet/vacuum-formed. Keep enough micro-relief to catch grazing light while
    // letting the geometric voxel plane and diffuse texture remain dominant.
    float normal_strength = modern_lighting ? 0.34f : 0.72f;
    float2 tangent_xy = (normal_spec.rg * 2.0f - 1.0f) * normal_strength;
    float tangent_z = sqrt(saturate(1.0f - dot(tangent_xy, tangent_xy)));
    float3 surface_normal = normalize(tangent * tangent_xy.x + bitangent * tangent_xy.y +
                                      geometric_normal * tangent_z);
    float4 metal_light = MetalLightAtlas.Sample(AnisoSampler, tile_uv);
    float authored_gloss = max(saturate(metal_light.r), saturate(input.gloss));
    // Treat the authored red response primarily as smoothness. Dirt and grass
    // both carry about 0.30 in this channel; mapping that directly to highlight
    // intensity was the source of their wet-plastic sheen.
    float material_roughness = lerp(0.90f, 0.18f, authored_gloss);
    float3 view_direction = normalize(Eye.xyz - input.world_position);
    float ao = lerp(1.0f, input.ambient_occlusion, SunAndOptions.w);
    float sky_exposure = saturate(input.sky_light);
    if (enhanced_leaf)
    {
        // Leaf clusters transmit skylight and scatter it through their thin
        // surfaces. Avoid treating every inner crown face like cave rock.
        sky_exposure = max(sky_exposure, 0.45f);
        ao = lerp(ao, 1.0f, 0.35f);
    }
    float3 sun_direction = normalize(SunAndOptions.xyz);
    float raw_sun_facing = dot(surface_normal, sun_direction);
    float sun_facing = modern_lighting ? saturate((raw_sun_facing + 0.08f) / 1.08f)
                                       : saturate(raw_sun_facing);
    // Original terrain lighting model: the flood-filled sun value scales the
    // AmbientSun color LINEARLY (so shaded surface faces and shallow caves
    // stay readable), while the direct SunlightColor only reaches faces at
    // full sky exposure (the pow-30 gate) with normal-based shading.
    float placed_torch_level = saturate(input.torch_light);
    float held_glow = TorchAndEye.w *
                      pow(saturate(1.0f - distance_to_eye / 9.0f), 2.0f);
    float torch_level = max(placed_torch_level, held_glow);
    // AmbientSun is the original authored sky colour. The tone-mapped path also
    // needs a small daylight sky-irradiance gain: it is indirect light, so it
    // remains in cast shadow, but the sky-exposure field still removes it in
    // caves and under real cover.
    float sky_fill = 1.0f + WaterAndTime.w * 0.25f;
    // The CPU bake uses gamma-two companding so dark color bleed survives an
    // eight-bit vertex channel. Decode back to linear radiance before shading.
    float4 baked_indirect = input.indirect_light * input.indirect_light;
    float3 ambient_term;
    float material_ao = ao;
    if (modern_lighting)
    {
        // Directional hemisphere irradiance is the inexpensive bounce-light
        // layer missing from the old flat AmbientSun multiply. Upward surfaces
        // see the bright sky, vertical faces see the horizon, and down-facing
        // surfaces retain a subdued ground bounce. The baked voxel skylight still
        // closes this contribution under roofs and in caves.
        float hemisphere = saturate(surface_normal.y * 0.5f + 0.5f);
        if (enhanced_leaf)
            hemisphere = max(hemisphere, 0.64f);
        float3 ground_bounce = AmbientSun.rgb * 0.62f + FogAndEnd.rgb * 0.10f;
        float3 sky_irradiance = AmbientSun.rgb * 1.42f + FogAndEnd.rgb * 0.16f;
        float3 hemisphere_light = lerp(ground_bounce, sky_irradiance, hemisphere);
        ambient_term = hemisphere_light * sky_exposure * sky_fill;
        // The packed face probe carries deterministic local multi-bounce radiance. Unlike the
        // direct sky-exposure scalar it is evaluated over the face's outward
        // hemisphere, so grass, sand, snow and construction materials bleed a
        // restrained amount of their linear reflectance onto nearby surfaces and
    // around shallow corners. A sealed cave bakes zero here rather than being
        // lifted by a screen-space or global ambient approximation.
        float3 local_bounce_source = AmbientSun.rgb * 1.18f + FogAndEnd.rgb * 0.08f;
        ambient_term += local_bounce_source * baked_indirect.rgb * 0.30f * sky_fill;
        // Hell is a vast furnace rather than a sealed ordinary cave. A very low
        // directional underglow keeps the face immediately in front of the
        // player mineable even between visible lava pools. It strengthens with
        // depth and favours walls/ceilings facing the lava layer below; AO still
        // grounds creases, and the much stronger local term below remains tied
        // to transported emissive energy.
        float hell_lava_proximity = saturate((-38.0f - input.world_position.y) / 24.0f);
        float hell_upglow_direction =
            0.28f + 0.72f * saturate(0.58f - surface_normal.y * 0.45f);
        float hell_upglow_variation =
            0.90f + 0.10f * sin(input.world_position.x * 0.21f +
                                input.world_position.z * 0.17f);
        ambient_term += float3(0.26f, 0.038f, 0.007f) * hell_depth *
                        hell_lava_proximity * hell_upglow_direction *
                        hell_upglow_variation * material_ao;
        // The alpha probe is emissive/block-light radiance gathered over this
        // face's visible hemisphere. In Hell that mostly means lava. Colouring
        // only that transported energy gives nearby rock an orange furnace
        // bounce while sealed corners remain genuinely black.
        float hell_emissive_reach = saturate(baked_indirect.a * 1.65f +
                                              placed_torch_level * 0.20f);
        ambient_term += float3(0.38f, 0.052f, 0.008f) *
                        hell_depth * hell_emissive_reach;
        // The held light's directly visible surface seeds live in a tiny
        // player-centred 3-D clipmap. Gathering them in this face hemisphere
        // adds real moving color bounce without screen-space leakage.
        ambient_term += dynamic_radiance_gather(
            input.world_position, geometric_normal, tangent, bitangent) * material_ao;
        // Corner AO should ground contacts, not multiply the whole landscape
        // into soot. Large-scale cover is already represented by sky_exposure.
        material_ao = lerp(1.0f, ao, 0.68f);
        if (enhanced_leaf)
        {
            ambient_term *= 1.08f;
            material_ao = lerp(material_ao, 1.0f, 0.55f);
        }
    }
    else
    {
        ambient_term = AmbientSun.rgb * sky_exposure * sky_fill;
    }
    // TorchLight * y * (1 - x * SunlightColor.y): daylight swallows torches.
    // Rebirth keeps the three energy paths separate. Placed emitters follow the
    // packed gradient of the occlusion-safe voxel flood, the moving held torch
    // uses its actual surface-to-eye direction, and the baked bounce term remains
    // diffuse. The authored torch tint is display-referred, so square it before
    // adding it to the linear HDR target instead of bleaching caves pale grey.
    float local_torch_bounce = modern_lighting ? baked_indirect.a * 0.28f : 0.0f;
    float3 baked_torch_direction = input.block_light_lobe.rgb * 2.0f - 1.0f;
    baked_torch_direction *= rsqrt(max(dot(baked_torch_direction, baked_torch_direction), 0.0001f));
    float placed_facing = 0.35f + 0.90f * saturate(dot(surface_normal, baked_torch_direction));
    float placed_response = lerp(1.0f, placed_facing,
                                 saturate(input.block_light_lobe.a) * 0.85f);
    float held_response = modern_lighting
                              ? 0.30f + 1.00f * saturate(dot(surface_normal, view_direction))
                              : 1.0f;
    float held_visibility = modern_lighting
                                ? held_torch_shadow(input.world_position, geometric_normal)
                                : 1.0f;
    if (ShadowParams.x < -0.5f)
        return float4(held_visibility.xxx, 1.0f);
    float torch_energy = torch_level;
    if (modern_lighting)
        torch_energy = min(1.35f, placed_torch_level * placed_response +
                                      held_glow * held_response * held_visibility +
                                      local_torch_bounce);
    float3 torch_color = modern_lighting ? TorchAndEye.rgb * TorchAndEye.rgb : TorchAndEye.rgb;
    float3 torch_component = torch_color * torch_energy *
                             (1.0f - sky_exposure * LightAndFogStart.g);
    // Shadow quality controls the cast-shadow pass, never the material's
    // directional response. Coupling both made ULTRA dramatically darker
    // than LOW before any shadow-map lookup even happened.
    float face_shade;
    if (enhanced_leaf)
    {
        face_shade = 0.72f + abs(dot(surface_normal, sun_direction)) * 0.28f;
    }
    else if (modern_lighting)
    {
        // Oren-Nayar diffuse keeps rough soil/stone broad and matte. Unlike the
        // former +0.08 Lambert bias it contributes no direct energy on a face
        // turned away from the sun; the baked hemisphere term remains the fill.
        float n_dot_l = saturate(raw_sun_facing);
        float n_dot_v = saturate(dot(surface_normal, view_direction));
        float l_dot_v = dot(sun_direction, view_direction);
        float sigma = lerp(0.88f, 0.28f, authored_gloss);
        float sigma_squared = sigma * sigma;
        float oren_a = 1.0f - 0.5f * sigma_squared / (sigma_squared + 0.33f);
        float oren_b = 0.45f * sigma_squared / (sigma_squared + 0.09f);
        float azimuth = max(l_dot_v - n_dot_l * n_dot_v, 0.0f);
        float oren_scale = azimuth / max(max(n_dot_l, n_dot_v), 0.001f);
        face_shade = n_dot_l * (oren_a + oren_b * oren_scale);
    }
    else
    {
        face_shade = 0.80f + sun_facing * 0.20f;
    }
    float shadow = sun_shadow(input.world_position, geometric_normal, distance_to_eye);
    if (DynamicRadianceParams.w > 0.5f)
    {
        float transmission = canopy_shadow_debug_value(input.world_position,
                                                        geometric_normal);
        return float4(transmission.xxx, 1.0f);
    }
    float direct_gate = enhanced_leaf ? pow(sky_exposure, 4.0f) : pow(sky_exposure, 30.0f);
    float leaf_transmission = enhanced_leaf
                                  ? saturate(dot(-geometric_normal, sun_direction)) * 0.34f
                                  : 0.0f;
    float3 direct_term = LightAndFogStart.rgb * direct_gate *
                         (face_shade + leaf_transmission) * shadow;
    // The shadow map blocks only direct sun. AmbientSun is the sky/indirect
    // fill recovered from BlockTerrain and must remain available in a
    // mountain shadow; suppressing it is what made whole valleys go black.
    // RGB stores outgoing reflected radiance at visibility-tested solid cells;
    // alpha marks only transient fireball/explosion illumination. Sample just
    // inside this face so moving combat lights get a real direct response while
    // the held torch retains its existing shadowed direct path without doubling.
    // Feed it through the textured BRDF as irradiance; adding fallback block
    // colour after albedo would flatten a lit voxel into a plastic solid polygon.
    float4 transient_surface = dynamic_radiance_cell(
        input.world_position - geometric_normal * 0.06f);
    float3 transient_direct =
        transient_surface.rgb * step(0.5f, transient_surface.a) * 1.65f;
    float3 light_total = ambient_term + torch_component + direct_term + transient_direct;
    float3 lit = texel.rgb * light_total * material_ao;
    float3 half_direction = normalize(sun_direction + view_direction);
    if (modern_lighting)
    {
        // Cook-Torrance / GGX. Ordinary blocks remain dielectric (F0 2.5%);
        // only the strongest authored metal mask becomes tinted conductor F0.
        // Smoothness changes the lobe width instead of inventing more energy.
        float n_dot_l = saturate(dot(surface_normal, sun_direction));
        float n_dot_v = saturate(dot(surface_normal, view_direction));
        float n_dot_h = saturate(dot(surface_normal, half_direction));
        float v_dot_h = saturate(dot(view_direction, half_direction));
        float alpha = material_roughness * material_roughness;
        float alpha_squared = alpha * alpha;
        float denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0f) + 1.0f;
        float distribution = alpha_squared /
                             max(3.14159265f * denominator * denominator, 0.0001f);
        float geometry_k = (material_roughness + 1.0f);
        geometry_k = geometry_k * geometry_k * 0.125f;
        float geometry_v = n_dot_v / lerp(n_dot_v, 1.0f, geometry_k);
        float geometry_l = n_dot_l / lerp(n_dot_l, 1.0f, geometry_k);
        float metalness = smoothstep(0.82f, 0.98f, saturate(metal_light.r));
        float3 f0 = lerp(float3(0.025f, 0.025f, 0.025f), texel.rgb, metalness);
        float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - v_dot_h, 5.0f);
        float3 specular_brdf = fresnel * distribution * geometry_v * geometry_l /
                               max(4.0f * n_dot_v * n_dot_l, 0.001f);
        lit += RenderOptions.yzw * specular_brdf * n_dot_l * direct_gate * shadow *
               material_ao;

        // Individual grains of sun/torch-lit snow and sand act like sparse
        // microfacets. The field is anchored in world space, responds to the
        // actual light/view half-vector, and fades when a pixel covers several
        // grains, so it reads as material sparkle rather than screen noise.
        bool sand_grains = abs(input.tile - 3.0f) < 0.25f;
        bool snow_grains = abs(input.tile - 13.0f) < 0.25f;
        if (sand_grains || snow_grains)
        {
            const float grain_scale = 22.0f;
            float2 grain_coordinate =
                float2(dot(input.world_position, tangent),
                       dot(input.world_position, bitangent)) * grain_scale;
            float2 grain_cell_2d = floor(grain_coordinate);
            float face_seed = dot(abs(geometric_normal), float3(17.0f, 31.0f, 47.0f));
            float3 grain_cell = float3(grain_cell_2d, face_seed + input.tile * 3.7f);
            float2 grain_center =
                float2(hash31(grain_cell + 11.9f), hash31(grain_cell + 27.4f));
            float grain_distance = length(frac(grain_coordinate) - grain_center);
            float footprint = max(length(ddx(grain_coordinate)), length(ddy(grain_coordinate)));
            float point_profile = 1.0f - smoothstep(0.075f,
                                                    0.075f + max(footprint * 0.55f, 0.012f),
                                                    grain_distance);
            float grain_presence = point_profile *
                                    smoothstep(0.56f, 0.98f, hash31(grain_cell + 7.1f));
            float2 grain_slope =
                float2(hash31(grain_cell + 19.7f), hash31(grain_cell + 41.3f)) * 2.0f - 1.0f;
            float3 micro_normal = normalize(geometric_normal +
                                            tangent * grain_slope.x * 0.82f +
                                            bitangent * grain_slope.y * 0.82f);
            if (dot(micro_normal, geometric_normal) < 0.0f)
                micro_normal = -micro_normal;

            float resolution_fade = 1.0f - smoothstep(0.42f, 1.20f, footprint);
            float distance_fade = 1.0f - smoothstep(11.0f, 25.0f, distance_to_eye);
            float surface_weight = lerp(0.24f, 1.0f, saturate(geometric_normal.y));

            float3 sun_half = normalize(sun_direction + view_direction);
            float sun_glint = pow(saturate(dot(micro_normal, sun_half)), 180.0f) *
                              direct_gate * shadow * saturate(raw_sun_facing);

            float held_direct = held_glow * held_response * held_visibility;
            float placed_direct = placed_torch_level * placed_response;
            float3 local_light_direction = held_direct >= placed_direct
                                               ? view_direction
                                               : baked_torch_direction;
            float3 local_half = normalize(local_light_direction + view_direction);
            float local_glint = pow(saturate(dot(micro_normal, local_half)), 150.0f) *
                                max(held_direct, placed_direct) *
                                saturate(dot(geometric_normal, local_light_direction));

            float common = grain_presence * resolution_fade * distance_fade * surface_weight *
                           material_ao;
            float strength = snow_grains ? 1.65f : 0.52f;
            float3 sun_sparkle = RenderOptions.yzw * sun_glint;
            float3 local_sparkle = torch_color * local_glint;
            lit += max(sun_sparkle, local_sparkle) * common * strength;
        }
    }
    else
    {
        float highlight_power = lerp(18.0f, 112.0f, authored_gloss);
        float highlight_strength = lerp(0.025f, 0.68f, authored_gloss);
        float highlight = pow(saturate(dot(surface_normal, half_direction)), highlight_power) *
                          highlight_strength *
                          saturate(sky_exposure * shadow + torch_level * 0.6f) * material_ao;
        float3 specular_color =
            RenderOptions.yzw *
            lerp(float3(1.0f, 1.0f, 1.0f), texel.rgb, authored_gloss * 0.48f);
        lit += specular_color * highlight;
    }
    float authored_light = max(metal_light.g, metal_light.b);
    lit += texel.rgb * authored_light * 0.48f;
    // The modern material atlas already contains a precise smoothness mask for
    // the mineral fragments in tiles 6..10. Reusing it keeps the surrounding
    // rock dark instead of making the entire ore block self-illuminated. The
    // pulse is deliberately shallow: it gives exposed veins a readable glint
    // and a little bloom without turning them into gameplay light sources.
    if (BiomeParams.y > 0.5f && input.tile >= 5.5f && input.tile <= 10.5f)
    {
        float ore_mask = smoothstep(0.34f, 0.88f, saturate(metal_light.r));
        float3 ore_tint = input.tile < 6.5f
                              ? float3(1.00f, 0.46f, 0.08f)
                          : input.tile < 7.5f
                              ? float3(0.72f, 0.84f, 1.00f)
                          : input.tile < 8.5f
                              ? float3(1.00f, 0.28f, 0.07f)
                          : input.tile < 9.5f
                              ? float3(0.34f, 0.38f, 0.46f)
                              : float3(0.08f, 0.92f, 1.00f);
        float ore_strength = input.tile >= 8.5f && input.tile < 9.5f ? 0.14f : 0.34f;
        float pulse = 0.92f + 0.08f * sin(WaterAndTime.x * 1.45f +
                                         dot(input.world_position, float3(0.31f, 0.17f, 0.23f)));
        lit += ore_tint * ore_mask * ore_strength * pulse;
    }
    lit = lerp(lit, texel.rgb * 1.12f, saturate(input.emission));
    return float4(lerp(lit, FogAndEnd.rgb, fog), 1.0f);
}
)hlsl";
        const auto vertex_blob = compile_shader(vertex_source, "main", "vs_4_0");
        // Array-comparison sampling for the three shadow cascades needs the
        // native D3D11 shader model; ps_4_0 cannot lower a dynamic array slice.
        const auto pixel_blob = compile_shader(pixel_source, "main", "ps_5_0");
        if (vertex_blob == nullptr || pixel_blob == nullptr ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr,
                                              &pixel_shader_)))
        {
            if (failure_reason_.empty())
            {
                failure_reason_ = "failed to create the depth renderer shaders";
            }
            return false;
        }
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 10> input_elements{{
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 12U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 20U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 1U, DXGI_FORMAT_R32_FLOAT, 0U, 32U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 2U, DXGI_FORMAT_R32_FLOAT, 0U, 36U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 3U, DXGI_FORMAT_R32_FLOAT, 0U, 40U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 4U, DXGI_FORMAT_R32_FLOAT, 0U, 44U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 5U, DXGI_FORMAT_R8G8B8A8_UNORM, 0U, 48U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 6U, DXGI_FORMAT_R8G8B8A8_UNORM, 0U, 52U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 7U, DXGI_FORMAT_R8G8B8A8_UNORM, 0U, 56U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
        }};
        if (FAILED(device_->CreateInputLayout(
                input_elements.data(), static_cast<UINT>(input_elements.size()),
                vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), &input_layout_)))
        {
            failure_reason_ = "failed to create the voxel input layout";
            return false;
        }

        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = sizeof(SceneConstants);
        constant_description.Usage = D3D11_USAGE_DYNAMIC;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&constant_description, nullptr, &scene_buffer_)))
        {
            failure_reason_ = "failed to create the depth renderer scene buffer";
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depth_description{};
        depth_description.DepthEnable = TRUE;
        depth_description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_description.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device_->CreateDepthStencilState(&depth_description, &depth_state_)))
        {
            failure_reason_ = "failed to create the voxel depth state";
            return false;
        }
        depth_description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device_->CreateDepthStencilState(&depth_description, &water_depth_state_)))
        {
            failure_reason_ = "failed to create the water depth-read state";
            return false;
        }
        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rasterizer_description, &rasterizer_state_)))
        {
            failure_reason_ = "failed to create the voxel rasterizer state";
            return false;
        }
        D3D11_BLEND_DESC blend_description{};
        blend_description.RenderTarget[0].BlendEnable = FALSE;
        blend_description.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend_description, &blend_state_)))
        {
            failure_reason_ = "failed to create the voxel blend state";
            return false;
        }
        auto &water_blend = blend_description.RenderTarget[0];
        water_blend.BlendEnable = TRUE;
        water_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        water_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        water_blend.BlendOp = D3D11_BLEND_OP_ADD;
        water_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        water_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        water_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device_->CreateBlendState(&blend_description, &water_blend_state_)))
        {
            failure_reason_ = "failed to create the water blend state";
            return false;
        }
        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MinLOD = 0.0F;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler_description, &sampler_state_)))
        {
            failure_reason_ = "failed to create the atlas sampler";
            return false;
        }
        // Anisotropic sampling over the per-tile mip chains: grazing-angle
        // ground stays sharp without the shimmer of raw bilinear sampling.
        sampler_description.Filter = D3D11_FILTER_ANISOTROPIC;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.MaxAnisotropy = 8U;
        if (FAILED(device_->CreateSamplerState(&sampler_description, &aniso_sampler_state_)))
        {
            failure_reason_ = "failed to create the anisotropic atlas sampler";
            return false;
        }
        create_shadow_pipeline();
        create_post_pipeline();
        create_entity_pipeline();
        return true;
    }

    // Depth-only pass rendered from the sun.  Optional: when any resource is
    // missing the lit pass simply samples full visibility.
    void create_shadow_pipeline()
    {
        static constexpr char shadow_vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float ambient_occlusion : TEXCOORD1;
    float sky_light : TEXCOORD2;
    float emission : TEXCOORD3;
    float torch_light : TEXCOORD4;
    float4 material : TEXCOORD5;
    float4 indirect_light : TEXCOORD6;
    float4 block_light_lobe : TEXCOORD7;
};

struct ShadowPixelInput
{
    float4 position : SV_POSITION;
    float3 uv : TEXCOORD0;
    float wave : TEXCOORD1;
};

ShadowPixelInput main(VertexInput input)
{
    ShadowPixelInput output;
    float phase = WaterAndTime.x * 1.15f + input.position.x * 0.9f +
                  input.position.z * 0.8f + input.position.y * 0.35f;
    float2 sway = float2(sin(phase), cos(phase * 0.83f + 1.7f)) *
                  0.05f * input.material.g;
    float3 world = input.position + float3(sway.x, 0.0f, sway.y);
    float4 sx = ShadowX;
    float4 sy = ShadowY;
    float4 sz = ShadowZ;
    if (ShadowCascades.w > 1.5f)
    {
        sx = ShadowX3;
        sy = ShadowY3;
        sz = ShadowZ3;
    }
    else if (ShadowCascades.w > 0.5f)
    {
        sx = ShadowX2;
        sy = ShadowY2;
        sz = ShadowZ2;
    }
    float u = dot(world, sx.xyz) + sx.w;
    float v = dot(world, sy.xyz) + sy.w;
    float depth = dot(world, sz.xyz) + sz.w;
    output.position = float4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, depth, 1.0f);
    output.uv = float3(input.uv, input.material.b * 255.0f);
    output.wave = input.material.g;
    return output;
}
)hlsl";
        static constexpr char point_shadow_vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float ambient_occlusion : TEXCOORD1;
    float sky_light : TEXCOORD2;
    float emission : TEXCOORD3;
    float torch_light : TEXCOORD4;
    float4 material : TEXCOORD5;
    float4 indirect_light : TEXCOORD6;
    float4 block_light_lobe : TEXCOORD7;
};

struct ShadowPixelInput
{
    float4 position : SV_POSITION;
    float3 uv : TEXCOORD0;
    float wave : TEXCOORD1;
};

ShadowPixelInput main(VertexInput input)
{
    ShadowPixelInput output;
    float phase = WaterAndTime.x * 1.15f + input.position.x * 0.9f +
                  input.position.z * 0.8f + input.position.y * 0.35f;
    float2 sway = float2(sin(phase), cos(phase * 0.83f + 1.7f)) *
                  0.05f * input.material.g;
    float3 world = input.position + float3(sway.x, 0.0f, sway.y);
    float3 relative = world - Eye.xyz;
    float screen_x;
    float screen_y;
    float forward;
    int face = (int)(ShadowCascades.w + 0.5f);
    if (face == 0) // +X
    {
        screen_x = -relative.z;
        screen_y = relative.y;
        forward = relative.x;
    }
    else if (face == 1) // -X
    {
        screen_x = relative.z;
        screen_y = relative.y;
        forward = -relative.x;
    }
    else if (face == 2) // +Y
    {
        screen_x = relative.x;
        screen_y = -relative.z;
        forward = relative.y;
    }
    else if (face == 3) // -Y
    {
        screen_x = relative.x;
        screen_y = relative.z;
        forward = -relative.y;
    }
    else if (face == 4) // +Z
    {
        screen_x = relative.x;
        screen_y = relative.y;
        forward = relative.z;
    }
    else // -Z
    {
        screen_x = -relative.x;
        screen_y = relative.y;
        forward = -relative.z;
    }
    const float near_plane = 0.08f;
    const float far_plane = 9.25f;
    float depth = forward * far_plane / (far_plane - near_plane) -
                  near_plane * far_plane / (far_plane - near_plane);
    output.position = float4(screen_x, screen_y, depth, forward);
    output.uv = float3(input.uv, input.material.b * 255.0f);
    output.wave = input.material.g;
    return output;
}
)hlsl";
        static constexpr char shadow_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
};

Texture2DArray DiffuseAtlas : register(t0);
SamplerState AtlasSampler : register(s0);

struct ShadowPixelInput
{
    float4 position : SV_POSITION;
    float3 uv : TEXCOORD0;
    float wave : TEXCOORD1;
    bool front_face : SV_IsFrontFace;
};

float main(ShadowPixelInput input) : SV_TARGET
{
    float4 texel = DiffuseAtlas.Sample(AtlasSampler, input.uv);
    clip(texel.a - 0.20f);
    if (input.wave > 0.5f)
    {
        // Classic keeps the original no-leaf-shadow behavior. Rebirth uses
        // the exact same restrained texture cutout as the lit pass, creating
        // readable, dappled shade rather than an opaque voxel silhouette.
        clip(AmbientSun.w - 0.5f);
        clip(input.front_face ? 1.0f : -1.0f);
        float3 leaf_mask = DiffuseAtlas.SampleBias(AtlasSampler, input.uv, -1.5f).rgb;
        float leaf_luma = dot(leaf_mask, float3(0.2126f, 0.7152f, 0.0722f));
        // A more open shadow mask than the visible surface mask keeps several
        // stacked leaf blocks from rebuilding an opaque black canopy.
        clip(leaf_luma - 0.12f);
        // This channel describes the nearest sun caster, not screen-space
        // brightness. A surviving leaf texel transmits 58% of direct sunlight;
        // opaque terrain and entities remain zero. CPU skylight separately
        // carries the broader diffuse hemisphere through the canopy.
        return 0.58f;
    }
    return 0.0f;
}
)hlsl";
        const auto vertex_blob = compile_shader(shadow_vertex_source, "main", "vs_4_0");
        const auto point_vertex_blob = compile_shader(point_shadow_vertex_source, "main", "vs_4_0");
        const auto pixel_blob = compile_shader(shadow_pixel_source, "main", "ps_4_0");
        if (vertex_blob == nullptr || pixel_blob == nullptr ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &shadow_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr,
                                              &shadow_pixel_shader_)))
        {
            return;
        }
        if (point_vertex_blob != nullptr)
        {
            device_->CreateVertexShader(point_vertex_blob->GetBufferPointer(),
                                        point_vertex_blob->GetBufferSize(), nullptr,
                                        &point_shadow_vertex_shader_);
        }

        D3D11_TEXTURE2D_DESC shadow_description{};
        shadow_description.Width = 2048U;
        shadow_description.Height = 2048U;
        shadow_description.MipLevels = 1U;
        shadow_description.ArraySize = 3U;
        shadow_description.Format = DXGI_FORMAT_R32_TYPELESS;
        shadow_description.SampleDesc.Count = 1U;
        shadow_description.Usage = D3D11_USAGE_DEFAULT;
        shadow_description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&shadow_description, nullptr, &shadow_texture_)))
        {
            shadow_vertex_shader_.Reset();
            return;
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC shadow_view_description{};
        shadow_view_description.Format = DXGI_FORMAT_D32_FLOAT;
        shadow_view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        shadow_view_description.Texture2DArray.MipSlice = 0U;
        shadow_view_description.Texture2DArray.ArraySize = 1U;
        D3D11_SHADER_RESOURCE_VIEW_DESC shadow_resource_description{};
        shadow_resource_description.Format = DXGI_FORMAT_R32_FLOAT;
        shadow_resource_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        shadow_resource_description.Texture2DArray.MostDetailedMip = 0U;
        shadow_resource_description.Texture2DArray.MipLevels = 1U;
        shadow_resource_description.Texture2DArray.FirstArraySlice = 0U;
        shadow_resource_description.Texture2DArray.ArraySize = 3U;
        bool views_ready = true;
        for (std::size_t cascade = 0; cascade < shadow_views_.size(); ++cascade)
        {
            shadow_view_description.Texture2DArray.FirstArraySlice = static_cast<UINT>(cascade);
            views_ready = views_ready && SUCCEEDED(device_->CreateDepthStencilView(
                                             shadow_texture_.Get(), &shadow_view_description,
                                             &shadow_views_[cascade]));
        }
        if (!views_ready || FAILED(device_->CreateShaderResourceView(
                                shadow_texture_.Get(), &shadow_resource_description, &shadow_srv_)))
        {
            for (auto &view : shadow_views_)
                view.Reset();
            shadow_vertex_shader_.Reset();
            return;
        }

        // A compact material channel accompanies each D32 cascade. Depth still
        // chooses the nearest caster; R8 stores its bounded direct-light
        // transmission (zero for rock/entities, nonzero only for cut-out leaves).
        // This prevents stacked foliage from turning into a mountain-shaped
        // binary blocker without weakening real terrain shadows.
        D3D11_TEXTURE2D_DESC transmission_description{};
        transmission_description.Width = shadow_description.Width;
        transmission_description.Height = shadow_description.Height;
        transmission_description.MipLevels = 1U;
        transmission_description.ArraySize = shadow_description.ArraySize;
        transmission_description.Format = DXGI_FORMAT_R8_UNORM;
        transmission_description.SampleDesc.Count = 1U;
        transmission_description.Usage = D3D11_USAGE_DEFAULT;
        transmission_description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bool transmission_ready = SUCCEEDED(device_->CreateTexture2D(
            &transmission_description, nullptr, &shadow_transmission_texture_));
        D3D11_RENDER_TARGET_VIEW_DESC transmission_view_description{};
        transmission_view_description.Format = transmission_description.Format;
        transmission_view_description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        transmission_view_description.Texture2DArray.MipSlice = 0U;
        transmission_view_description.Texture2DArray.ArraySize = 1U;
        for (std::size_t cascade = 0;
             transmission_ready && cascade < shadow_transmission_views_.size(); ++cascade)
        {
            transmission_view_description.Texture2DArray.FirstArraySlice =
                static_cast<UINT>(cascade);
            transmission_ready = SUCCEEDED(device_->CreateRenderTargetView(
                shadow_transmission_texture_.Get(), &transmission_view_description,
                &shadow_transmission_views_[cascade]));
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC transmission_resource_description{};
        transmission_resource_description.Format = transmission_description.Format;
        transmission_resource_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        transmission_resource_description.Texture2DArray.MostDetailedMip = 0U;
        transmission_resource_description.Texture2DArray.MipLevels = 1U;
        transmission_resource_description.Texture2DArray.FirstArraySlice = 0U;
        transmission_resource_description.Texture2DArray.ArraySize =
            transmission_description.ArraySize;
        if (transmission_ready)
        {
            transmission_ready = SUCCEEDED(device_->CreateShaderResourceView(
                shadow_transmission_texture_.Get(), &transmission_resource_description,
                &shadow_transmission_srv_));
        }
        if (!transmission_ready)
        {
            shadow_transmission_texture_.Reset();
            shadow_transmission_srv_.Reset();
            for (auto &view : shadow_transmission_views_)
                view.Reset();
        }

        // A separate compact array avoids expanding the three 2048-square sun
        // slices into a wasteful nine-slice allocation. Six 384-square D32
        // faces cost about 3.4 MiB and cover the held torch's bounded range.
        if (point_shadow_vertex_shader_ != nullptr)
        {
            D3D11_TEXTURE2D_DESC point_description = shadow_description;
            point_description.Width = point_shadow_resolution;
            point_description.Height = point_shadow_resolution;
            point_description.ArraySize = 6U;
            bool point_ready = SUCCEEDED(
                device_->CreateTexture2D(&point_description, nullptr, &point_shadow_texture_));
            D3D11_DEPTH_STENCIL_VIEW_DESC point_view_description = shadow_view_description;
            for (std::size_t face = 0; point_ready && face < point_shadow_views_.size(); ++face)
            {
                point_view_description.Texture2DArray.FirstArraySlice = static_cast<UINT>(face);
                point_ready = SUCCEEDED(device_->CreateDepthStencilView(
                    point_shadow_texture_.Get(), &point_view_description,
                    &point_shadow_views_[face]));
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC point_resource_description =
                shadow_resource_description;
            point_resource_description.Texture2DArray.ArraySize = 6U;
            if (point_ready)
            {
                point_ready = SUCCEEDED(device_->CreateShaderResourceView(
                    point_shadow_texture_.Get(), &point_resource_description, &point_shadow_srv_));
            }
            if (!point_ready)
            {
                point_shadow_texture_.Reset();
                point_shadow_srv_.Reset();
                point_shadow_vertex_shader_.Reset();
                for (auto &view : point_shadow_views_)
                    view.Reset();
            }
        }
        D3D11_SAMPLER_DESC shadow_sampler_description{};
        shadow_sampler_description.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        shadow_sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        shadow_sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        shadow_sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        shadow_sampler_description.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        shadow_sampler_description.BorderColor[0] = 1.0F;
        shadow_sampler_description.BorderColor[1] = 1.0F;
        shadow_sampler_description.BorderColor[2] = 1.0F;
        shadow_sampler_description.BorderColor[3] = 1.0F;
        shadow_sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC shadow_rasterizer_description{};
        shadow_rasterizer_description.FillMode = D3D11_FILL_SOLID;
        shadow_rasterizer_description.CullMode = D3D11_CULL_NONE;
        shadow_rasterizer_description.DepthClipEnable = TRUE;
        shadow_rasterizer_description.DepthBias = 16;
        shadow_rasterizer_description.DepthBiasClamp = 0.003F;
        shadow_rasterizer_description.SlopeScaledDepthBias = 2.0F;
        if (FAILED(device_->CreateSamplerState(&shadow_sampler_description, &shadow_sampler_)) ||
            FAILED(device_->CreateRasterizerState(&shadow_rasterizer_description,
                                                  &shadow_rasterizer_state_)))
        {
            for (auto &view : shadow_views_)
                view.Reset();
            shadow_vertex_shader_.Reset();
        }
        D3D11_RASTERIZER_DESC point_rasterizer_description = shadow_rasterizer_description;
        point_rasterizer_description.DepthBias = 24;
        point_rasterizer_description.DepthBiasClamp = 0.0015F;
        point_rasterizer_description.SlopeScaledDepthBias = 1.5F;
        if (FAILED(device_->CreateRasterizerState(&point_rasterizer_description,
                                                  &point_shadow_rasterizer_state_)))
        {
            point_shadow_rasterizer_state_.Reset();
            point_shadow_vertex_shader_.Reset();
        }
        if (SDL_getenv("OPENMINERZ_GRAPHICS_TRACE") != nullptr)
        {
            const bool point_ready =
                point_shadow_vertex_shader_ != nullptr && point_shadow_srv_ != nullptr &&
                point_shadow_rasterizer_state_ != nullptr &&
                std::all_of(point_shadow_views_.begin(), point_shadow_views_.end(),
                            [](const auto &view) { return view != nullptr; });
            SDL_Log("Held-torch point shadow pipeline: %s%s%s%s",
                    point_ready ? "ready" : "unavailable",
                    !point_ready && !failure_reason_.empty() ? " (" : "",
                    !point_ready && !failure_reason_.empty() ? failure_reason_.c_str() : "",
                    !point_ready && !failure_reason_.empty() ? ")" : "");
            SDL_Log("Canopy sun-shadow transmission: %s",
                    shadow_transmission_srv_ != nullptr ? "ready" : "unavailable");
        }
    }

    // Fullscreen bloom / god-ray / sun-glow chain.  Also optional.
    void create_post_pipeline()
    {
        static constexpr char post_vertex_source[] = R"hlsl(
struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PostPixelInput main(uint vertex_id : SV_VertexID)
{
    PostPixelInput output;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}
)hlsl";
        // Scene luminance for auto exposure: a log-average is far more stable
        // than a linear one because a single bright sky pixel cannot drag the
        // whole frame dark.
        static constexpr char luminance_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
};

Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PostPixelInput input) : SV_TARGET
{
    float4 scene = Scene.Sample(LinearSampler, input.uv);
    // The sky is composited behind this target, so estimate it with the fog
    // colour where the world is transparent; otherwise a sky-heavy view would
    // read as pitch black and drive the exposure wide open.
    float3 color = scene.rgb + FogAndEnd.rgb * saturate(1.0f - scene.a);
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    return float4(log(max(luminance, 0.0002f)), 0.0f, 0.0f, 1.0f);
}
)hlsl";
        // Eye adaptation: the measured average chases the previous frame's
        // value, so walking out of a cave brightens over about a second
        // instead of snapping.
        static constexpr char adapt_pixel_source[] = R"hlsl(
cbuffer Blur : register(b1)
{
    float4 BlurDirection;
};

Texture2D Measured : register(t0);
Texture2D Previous : register(t1);
SamplerState LinearSampler : register(s0);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PostPixelInput input) : SV_TARGET
{
    float measured = Measured.SampleLevel(LinearSampler, float2(0.5f, 0.5f), 16.0f).r;
    float previous = Previous.SampleLevel(LinearSampler, float2(0.5f, 0.5f), 0.0f).r;
    // BlurDirection.x carries the frame delta; adapt faster when going dark
    // (pupil opening lags, closing is quick) like the reference renderers.
    float rate = (measured < previous) ? 2.8f : 1.2f;
    float blend = saturate(1.0f - exp(-max(BlurDirection.x, 0.0f) * rate));
    float adapted = previous + (measured - previous) * blend;
    if (BlurDirection.y > 0.5f)
    {
        adapted = measured;
    }
    return float4(adapted, 0.0f, 0.0f, 1.0f);
}
)hlsl";
        static constexpr char bright_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
};

Texture2D Scene : register(t0);
Texture2D Adapted : register(t1);
SamplerState LinearSampler : register(s0);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float scene_exposure()
{
    float manual = GradeParams.x;
    if (GradeParams.w < 0.5f)
    {
        return manual;
    }
    float average = exp(Adapted.SampleLevel(LinearSampler, float2(0.5f, 0.5f), 0.0f).r);
    // Middle-grey key over the measured average, clamped so a black cave or a
    // white desert cannot run away.
    float exposure = 0.20f / max(average, 0.0008f);
    // Deliberately asymmetric and NARROW: 2 EV total, from 2^-1.5 to 2^+0.5.
    // The job is to pull blown-out sand and snow DOWN, not to brighten dark
    // scenes up — a wide range would "fix" night by making it look like day,
    // which is exactly the mood the adaptation must not touch.
    // Rebirth's decoded albedo no longer needs the legacy +0.5 EV rescue. It
    // may close exposure in bright snow/desert scenes, but it never opens a
    // dark scene into the overexposed fluorescent look.
    float maximum = AmbientSun.w > 0.5f ? 1.0f : 1.414f;
    return clamp(exposure, 0.354f, maximum) * manual;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float3 color = Scene.Sample(LinearSampler, input.uv).rgb * scene_exposure();
    // No hard threshold. A cutoff makes an entire sunlit dune field sit just
    // above it across megapixels, so the whole biome becomes one bloom source
    // and glows into itself. Squaring is a soft weighting instead: midtones
    // contribute almost nothing, genuine highlights dominate, and there is no
    // edge for a flat bright surface to straddle.
    float weight = dot(color, float3(0.2126f, 0.7152f, 0.0722f)) * GradeParams.z;
    return float4(color * saturate(weight * weight), 1.0f);
}
)hlsl";
        static constexpr char blur_pixel_source[] = R"hlsl(
cbuffer Blur : register(b1)
{
    float4 BlurDirection;
};

Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PostPixelInput input) : SV_TARGET
{
    const float weights[5] = {0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f};
    float3 color = Scene.Sample(LinearSampler, input.uv).rgb * weights[0];
    [unroll]
    for (int tap = 1; tap < 5; ++tap)
    {
        float2 stride = BlurDirection.xy * tap * 1.5f;
        color += Scene.Sample(LinearSampler, input.uv + stride).rgb * weights[tap];
        color += Scene.Sample(LinearSampler, input.uv - stride).rgb * weights[tap];
    }
    return float4(color, 1.0f);
}
)hlsl";
        static constexpr char composite_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
    float4 DynamicRadianceOrigin;
    float4 DynamicRadianceParams;
    float4 BiomeTint;
    float4 BiomeParams;
};

Texture2D Scene : register(t0);
Texture2D Bloom : register(t1);
Texture2D Adapted : register(t2);
SamplerState LinearSampler : register(s0);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float scene_exposure()
{
    float manual = GradeParams.x;
    if (GradeParams.w < 0.5f)
    {
        return manual;
    }
    float average = exp(Adapted.SampleLevel(LinearSampler, float2(0.5f, 0.5f), 0.0f).r);
    float exposure = 0.20f / max(average, 0.0008f);
    // Deliberately asymmetric and NARROW: 2 EV total, from 2^-1.5 to 2^+0.5.
    // The job is to pull blown-out sand and snow DOWN, not to brighten dark
    // scenes up — a wide range would "fix" night by making it look like day,
    // which is exactly the mood the adaptation must not touch.
    float maximum = AmbientSun.w > 0.5f ? 1.0f : 1.414f;
    return clamp(exposure, 0.354f, maximum) * manual;
}

// Narkowicz's fit of the ACES filmic curve: a long shoulder that keeps
// texture in sunlit sand and snow instead of clipping them to paper white.
// The 0.725 prescale is the value that maps 18% grey to exactly where it
// landed before, so the curve buys highlight headroom without darkening the
// midtones (and therefore without touching night scenes).
float3 tonemap_aces(float3 color)
{
    color *= 0.725f;
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

// Reinhard-Jodie: the luminance-based result blended toward the per-channel
// result using the per-channel result itself as the weight. Darks stay
// luminance-based (no hue shift, saturation preserved), highlights desaturate
// to white gracefully instead of clipping to a flat primary.
float3 tonemap_reinhard_jodie(float3 color)
{
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float3 per_channel = color / (1.0f + color);
    return saturate(lerp(color / (1.0f + luminance), per_channel, per_channel));
}

// Hable's Uncharted 2 curve: a touch more contrast in the mids than ACES.
float3 hable_curve(float3 x)
{
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 tonemap_filmic(float3 color)
{
    const float white = 11.2f;
    return saturate(hable_curve(color * 2.0f) / hable_curve(float3(white, white, white)));
}

float3 tonemap_reinhard(float3 color)
{
    return tonemap_reinhard_jodie(color);
}

// The original soft-knee clamp, kept so the old look stays comparable.
float3 tonemap_legacy(float3 color)
{
    float luminance = dot(color, float3(0.299f, 0.587f, 0.114f));
    if (luminance > 0.0001f)
    {
        float compressed = luminance / (1.0f + max(0.0f, luminance - 1.0f) * 0.45f);
        color *= compressed / luminance;
    }
    return color;
}

float3 apply_tonemap(float3 color)
{
    int curve = (int)(PostParams.z + 0.5f);
    if (curve == 0)
    {
        return tonemap_legacy(color);
    }
    if (curve == 1)
    {
        return tonemap_reinhard(color);
    }
    if (curve == 2)
    {
        return tonemap_filmic(color);
    }
    return tonemap_aces(color);
}

// White balance driven by the fog colour, the way the reference renderers get
// their cohesion for free: the tint is the INVERSE of the fog hue, pulled 20%
// back toward neutral, then normalised so the smallest channel is exactly 1 —
// so it only ever brightens the channels the fog is eating, never darkens.
// This is what stops a heavily fogged desert or arctic view going monochrome.
float3 fog_white_balance()
{
    float3 tint = 1.0f / (FogAndEnd.rgb + 0.5f);
    tint = lerp(tint, float3(1.0f, 1.0f, 1.0f), 0.2f);
    float3 balanced = tint / max(min(tint.r, min(tint.g, tint.b)), 0.0001f);
    // Rebirth keeps dawn/desert/arctic atmosphere authored instead of fully
    // inverting its hue into grey. Classic retains the recovered unlimited
    // correction for strict visual parity.
    return AmbientSun.w > 0.5f ? min(balanced, 1.25f) : balanced;
}

// Applied BEFORE the tone curve, in linear light.
float3 apply_white_balance(float3 color, float strength)
{
    return lerp(color, color * fog_white_balance(), saturate(strength));
}

// Cheap cohesion grade after the curve: a slight global desaturation (every
// reference renderer runs 10-20% of it — it reads as filmic rather than
// washed out) and a symmetric S-curve that is neutral at black, mid AND
// white, so it can neither crush nights nor blow out highlights.
float3 apply_grade(float3 color, float strength)
{
    if (strength <= 0.001f)
    {
        return color;
    }
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    // OpenSpades' HDR path deliberately lands near 0.8 saturation. Rebirth
    // follows that restrained material palette; Classic keeps its recovered
    // 0.85 mix so the compatibility presentation remains stable.
    float saturation = AmbientSun.w > 0.5f ? 0.79f : 0.85f;
    float3 graded = lerp(float3(luminance, luminance, luminance), color, saturation);
    // iq's gain(x, 1.18): contrast that pins 0, 0.5 and 1.
    const float k = 1.18f;
    float3 low = 0.5f * pow(saturate(2.0f * graded), k);
    float3 high = 1.0f - 0.5f * pow(saturate(2.0f - 2.0f * graded), k);
    graded = lerp(low, high, step(0.5f, graded));
    return saturate(lerp(color, graded, saturate(strength)));
}

// Vlachos ordered dither. Once a shoulder compresses the highlights, smooth
// sky and sand gradients quantise into visible bands on 8-bit output; a
// sub-LSB dither removes them for three instructions.
float3 apply_dither(float3 color, float2 pixel)
{
    float3 noise = float3(dot(float2(171.0f, 231.0f), pixel),
                          dot(float2(171.0f, 231.0f), pixel),
                          dot(float2(171.0f, 231.0f), pixel));
    noise = frac(noise / float3(103.0f, 71.0f, 97.0f));
    return color + (noise - 0.5f) / 255.0f;
}

// Edge detection has to happen in roughly perceptual space. On a float scene
// target raw luma is unbounded, so a sunlit dune reads as one giant edge;
// compressing first keeps FXAA behaving the same at every brightness.
float scene_luma(float2 uv, float exposure)
{
    float3 color = Scene.Sample(LinearSampler, uv).rgb * exposure;
    float luminance = dot(color, float3(0.299f, 0.587f, 0.114f));
    return luminance / (1.0f + luminance);
}

// FXAA over the premultiplied scene: geometry edges soften, sky alpha blends
// consistently so the composite over the skybox stays clean.
float4 fxaa_scene(float2 uv, float exposure)
{
    float2 texel = TargetSize.zw;
    float luma_nw = scene_luma(uv + float2(-1.0f, -1.0f) * texel, exposure);
    float luma_ne = scene_luma(uv + float2(1.0f, -1.0f) * texel, exposure);
    float luma_sw = scene_luma(uv + float2(-1.0f, 1.0f) * texel, exposure);
    float luma_se = scene_luma(uv + float2(1.0f, 1.0f) * texel, exposure);
    float luma_m = scene_luma(uv, exposure);
    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    if (luma_max - luma_min < max(0.0312f, luma_max * 0.125f))
    {
        return Scene.Sample(LinearSampler, uv);
    }
    float2 direction = float2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                              (luma_nw + luma_sw) - (luma_ne + luma_se));
    float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.03125f, 0.0078125f);
    float scale = 1.0f / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, -8.0f, 8.0f) * texel;
    float4 sample_a = 0.5f * (Scene.Sample(LinearSampler, uv + direction * (1.0f / 3.0f - 0.5f)) +
                              Scene.Sample(LinearSampler, uv + direction * (2.0f / 3.0f - 0.5f)));
    float4 sample_b = sample_a * 0.5f +
                      0.25f * (Scene.Sample(LinearSampler, uv - direction * 0.5f) +
                               Scene.Sample(LinearSampler, uv + direction * 0.5f));
    float exposed_b = dot(sample_b.rgb * exposure, float3(0.299f, 0.587f, 0.114f));
    float luma_b = exposed_b / (1.0f + exposed_b);
    return (luma_b < luma_min || luma_b > luma_max) ? sample_a : sample_b;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    // Everything downstream works in exposed linear light; the bright pass
    // already exposed the bloom it produced.
    float exposure = scene_exposure();
    float4 scene = fxaa_scene(input.uv, exposure);
    float3 color = scene.rgb * exposure + Bloom.Sample(LinearSampler, input.uv).rgb * PostParams.x;
    if (PostSun.z > 0.02f)
    {
        float2 sun_uv = PostSun.xy;
        float2 delta = sun_uv - input.uv;
        float aspect = TargetSize.x * TargetSize.w;
        float sun_distance = length(float2(delta.x * aspect, delta.y));
        float2 outside = max(float2(0.0f, 0.0f), max(-sun_uv, sun_uv - 1.0f));
        float edge_fade = saturate(1.0f - (outside.x + outside.y) * 3.0f);
        float energy = 0.25f + 0.75f * PostSun.w;
        float3 sun_color = normalize(LightAndFogStart.rgb + 0.001f) * 1.05f;
        // God rays: march toward the sun accumulating sky visibility.  The march
        // start is dithered per pixel (interleaved gradient noise) so the discrete
        // taps blur into smooth shafts instead of repeating ghost silhouettes of
        // whatever occludes the sun.
        float accumulated = 0.0f;
        float weight = 1.0f;
        float total_weight = 0.0f;
        float jitter = frac(52.9829189f *
                            frac(dot(input.position.xy, float2(0.06711056f, 0.00583715f))));
        [unroll]
        for (int step = 0; step < 24; ++step)
        {
            float2 tap = saturate(input.uv + delta * ((step + jitter) / 24.0f) * 0.85f);
            accumulated += (1.0f - Scene.Sample(LinearSampler, tap).a) * weight;
            total_weight += weight;
            weight *= 0.96f;
        }
        float rays = (accumulated / total_weight) * saturate(1.0f - sun_distance * 1.1f);
        color += sun_color * rays * rays * PostParams.y * edge_fade * energy;
        // Sun glow disc, occluded by geometry in front of the sun.
        float sun_visibility = 1.0f - Scene.Sample(LinearSampler, saturate(sun_uv)).a;
        float glow = pow(saturate(1.0f - sun_distance / 0.35f), 3.0f) * 0.28f +
                     pow(saturate(1.0f - sun_distance / 0.06f), 2.0f) * 0.95f;
        color += sun_color * glow * sun_visibility * edge_fade * energy;
    }
    // White balance in linear light, tone map, then grade and dither.
    color = apply_white_balance(max(color, 0.0f), GradeParams.y);
    color = apply_tonemap(color);
    // The Rebirth scene has stayed in linear light from albedo through fog and
    // ACES. Encode it only now for SDL's ordinary UNORM presentation target.
    // Classic intentionally retains its recovered display-space pipeline.
    if (AmbientSun.w > 0.5f)
        color = sqrt(saturate(color));
    color = apply_grade(color, GradeParams.y);
    // A biome should be felt in distant air and palette, not painted over
    // nearby material lighting. The authored multiplier therefore enters as a
    // restrained display-space blend and never changes exposure or shadows.
    float biome_luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float3 biome_color = lerp(biome_luminance.xxx, color, BiomeParams.x) * BiomeTint.rgb;
    color = saturate(lerp(color, biome_color, BiomeTint.w));
    color = apply_dither(color, input.position.xy);
    return float4(color, scene.a);
}
)hlsl";
        const auto vertex_blob = compile_shader(post_vertex_source, "main", "vs_4_0");
        const auto bright_blob = compile_shader(bright_pixel_source, "main", "ps_4_0");
        const auto blur_blob = compile_shader(blur_pixel_source, "main", "ps_4_0");
        const auto composite_blob = compile_shader(composite_pixel_source, "main", "ps_4_0");
        const auto luminance_blob = compile_shader(luminance_pixel_source, "main", "ps_4_0");
        const auto adapt_blob = compile_shader(adapt_pixel_source, "main", "ps_4_0");
        if (vertex_blob == nullptr || bright_blob == nullptr || blur_blob == nullptr ||
            composite_blob == nullptr || luminance_blob == nullptr || adapt_blob == nullptr ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &post_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(bright_blob->GetBufferPointer(),
                                              bright_blob->GetBufferSize(), nullptr,
                                              &bright_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(blur_blob->GetBufferPointer(),
                                              blur_blob->GetBufferSize(), nullptr,
                                              &blur_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(composite_blob->GetBufferPointer(),
                                              composite_blob->GetBufferSize(), nullptr,
                                              &composite_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(luminance_blob->GetBufferPointer(),
                                              luminance_blob->GetBufferSize(), nullptr,
                                              &luminance_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(adapt_blob->GetBufferPointer(),
                                              adapt_blob->GetBufferSize(), nullptr,
                                              &adapt_pixel_shader_)))
        {
            post_vertex_shader_.Reset();
            return;
        }
        // Auto exposure is optional: if the driver refuses the tiny mipped
        // luminance target, the rest of the post chain still runs on the
        // manual exposure value.
        exposure_ready_ = create_exposure_targets();
        D3D11_DEPTH_STENCIL_DESC post_depth_description{};
        post_depth_description.DepthEnable = FALSE;
        D3D11_BUFFER_DESC blur_description{};
        blur_description.ByteWidth = sizeof(Float4);
        blur_description.Usage = D3D11_USAGE_DYNAMIC;
        blur_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        blur_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateDepthStencilState(&post_depth_description, &post_depth_state_)) ||
            FAILED(device_->CreateBuffer(&blur_description, nullptr, &blur_buffer_)))
        {
            post_vertex_shader_.Reset();
        }
    }

    // A 64x64 mipped luminance target plus a 1x1 ping-pong pair holding the
    // adapted value between frames. Total cost is a few kilobytes and two
    // trivial draws.
    [[nodiscard]] bool create_exposure_targets()
    {
        D3D11_TEXTURE2D_DESC luminance_description{};
        luminance_description.Width = luminance_extent;
        luminance_description.Height = luminance_extent;
        luminance_description.MipLevels = 0U; // Full chain down to 1x1.
        luminance_description.ArraySize = 1U;
        luminance_description.Format = DXGI_FORMAT_R32_FLOAT;
        luminance_description.SampleDesc.Count = 1U;
        luminance_description.Usage = D3D11_USAGE_DEFAULT;
        luminance_description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        luminance_description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&luminance_description, nullptr, &luminance_resource_)))
        {
            return false;
        }
        D3D11_RENDER_TARGET_VIEW_DESC luminance_target_description{};
        luminance_target_description.Format = DXGI_FORMAT_R32_FLOAT;
        luminance_target_description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device_->CreateRenderTargetView(
                luminance_resource_.Get(), &luminance_target_description, &luminance_rtv_)) ||
            FAILED(device_->CreateShaderResourceView(luminance_resource_.Get(), nullptr,
                                                     &luminance_srv_)))
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC adapt_description{};
        adapt_description.Width = 1U;
        adapt_description.Height = 1U;
        adapt_description.MipLevels = 1U;
        adapt_description.ArraySize = 1U;
        adapt_description.Format = DXGI_FORMAT_R32_FLOAT;
        adapt_description.SampleDesc.Count = 1U;
        adapt_description.Usage = D3D11_USAGE_DEFAULT;
        adapt_description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (std::size_t index = 0; index < adapt_resource_.size(); ++index)
        {
            if (FAILED(device_->CreateTexture2D(&adapt_description, nullptr,
                                                &adapt_resource_[index])) ||
                FAILED(device_->CreateRenderTargetView(adapt_resource_[index].Get(), nullptr,
                                                       &adapt_rtv_[index])) ||
                FAILED(device_->CreateShaderResourceView(adapt_resource_[index].Get(), nullptr,
                                                         &adapt_srv_[index])))
            {
                return false;
            }
        }
        // A 4-byte staging copy lets the CPU read the adapted value one frame
        // late, which is what keeps the separately drawn sky at the same
        // exposure as the tone-mapped world.
        D3D11_TEXTURE2D_DESC staging_description = adapt_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0U;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&staging_description, nullptr, &adapt_staging_)))
        {
            return false;
        }
        adapt_primed_ = false;
        return true;
    }

    // Skinned enemies and projectiles: world-space triangles sharing the terrain
    // depth, fog and Rebirth shadow fields. Soft spans keep their authored colour;
    // skinned geometry supplies normals plus independent sky/torch probes.
    void create_entity_pipeline()
    {
        static constexpr char entity_vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 normal : NORMAL0;
    float2 light_data : TEXCOORD1;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float2 light_data : TEXCOORD3;
};

PixelInput main(VertexInput input)
{
    PixelInput output;
    float3 relative = input.position - Eye.xyz;
    float camera_x = dot(relative, CameraRight.xyz);
    float camera_y = dot(relative, CameraUp.xyz);
    float camera_z = dot(relative, CameraForward.xyz);
    output.position = float4(camera_x * Projection.x,
                             camera_y * Projection.y,
                             camera_z * Projection.z + Projection.w,
                             camera_z);
    output.uv = input.uv;
    output.color = input.color;
    output.world_position = input.position;
    output.normal = input.normal;
    output.light_data = input.light_data;
    return output;
}
)hlsl";
        static constexpr char entity_sun_shadow_vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 main(VertexInput input) : SV_POSITION
{
    float4 sx = ShadowX;
    float4 sy = ShadowY;
    float4 sz = ShadowZ;
    if (ShadowCascades.w > 1.5f)
    {
        sx = ShadowX3;
        sy = ShadowY3;
        sz = ShadowZ3;
    }
    else if (ShadowCascades.w > 0.5f)
    {
        sx = ShadowX2;
        sy = ShadowY2;
        sz = ShadowZ2;
    }
    float u = dot(input.position, sx.xyz) + sx.w;
    float v = dot(input.position, sy.xyz) + sy.w;
    float depth = dot(input.position, sz.xyz) + sz.w;
    return float4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, depth, 1.0f);
}
)hlsl";
        static constexpr char entity_point_shadow_vertex_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 main(VertexInput input) : SV_POSITION
{
    float3 relative = input.position - Eye.xyz;
    float screen_x;
    float screen_y;
    float forward;
    int face = (int)(ShadowCascades.w + 0.5f);
    if (face == 0)
    {
        screen_x = -relative.z;
        screen_y = relative.y;
        forward = relative.x;
    }
    else if (face == 1)
    {
        screen_x = relative.z;
        screen_y = relative.y;
        forward = -relative.x;
    }
    else if (face == 2)
    {
        screen_x = relative.x;
        screen_y = -relative.z;
        forward = relative.y;
    }
    else if (face == 3)
    {
        screen_x = relative.x;
        screen_y = relative.z;
        forward = -relative.y;
    }
    else if (face == 4)
    {
        screen_x = relative.x;
        screen_y = relative.y;
        forward = relative.z;
    }
    else
    {
        screen_x = -relative.x;
        screen_y = relative.y;
        forward = -relative.z;
    }
    const float near_plane = 0.08f;
    const float far_plane = 9.25f;
    float depth = forward * far_plane / (far_plane - near_plane) -
                  near_plane * far_plane / (far_plane - near_plane);
    return float4(screen_x, screen_y, depth, forward);
}
)hlsl";
        static constexpr char entity_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
    float4 DynamicRadianceOrigin;
    float4 DynamicRadianceParams;
};

Texture2D Diffuse : register(t0);
Texture2DArray ShadowMap : register(t3);
Texture2DArray PointShadowMap : register(t6);
Texture3D<float4> DynamicRadiance : register(t7);
Texture2DArray ShadowTransmission : register(t8);
SamplerState DiffuseSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float2 light_data : TEXCOORD3;
    bool front_face : SV_IsFrontFace;
};

float3 entity_dynamic_seed(float3 world_position)
{
    if (DynamicRadianceOrigin.w < 0.5f)
        return 0.0f.xxx;
    int extent = (int)(DynamicRadianceParams.x + 0.5f);
    int3 cell = int3(floor(world_position)) - int3(DynamicRadianceOrigin.xyz);
    if (any(cell < int3(0, 0, 0)) || any(cell >= int3(extent, extent, extent)))
        return 0.0f.xxx;
    float3 packed = DynamicRadiance.Load(int4(cell, 0)).rgb;
    return packed * packed;
}

float3 entity_dynamic_bounce(float3 world_position)
{
    if (DynamicRadianceOrigin.w < 0.5f)
        return 0.0f.xxx;
    const float distance = 2.0f;
    float3 radiance = entity_dynamic_seed(world_position + float3(distance, 0.0f, 0.0f));
    radiance += entity_dynamic_seed(world_position + float3(-distance, 0.0f, 0.0f));
    radiance += entity_dynamic_seed(world_position + float3(0.0f, distance, 0.0f));
    radiance += entity_dynamic_seed(world_position + float3(0.0f, -distance, 0.0f));
    radiance += entity_dynamic_seed(world_position + float3(0.0f, 0.0f, distance));
    radiance += entity_dynamic_seed(world_position + float3(0.0f, 0.0f, -distance));
    return radiance * (0.34f / 6.0f);
}

void entity_cascade_transform(int cascade, out float4 sx, out float4 sy, out float4 sz)
{
    if (cascade == 0)
    {
        sx = ShadowX;
        sy = ShadowY;
        sz = ShadowZ;
    }
    else if (cascade == 1)
    {
        sx = ShadowX2;
        sy = ShadowY2;
        sz = ShadowZ2;
    }
    else
    {
        sx = ShadowX3;
        sy = ShadowY3;
        sz = ShadowZ3;
    }
}

float entity_cascade_visibility(int cascade, float3 world_position, float3 normal)
{
    float4 sx;
    float4 sy;
    float4 sz;
    entity_cascade_transform(cascade, sx, sy, sz);
    float3 sun_direction = normalize(SunAndOptions.xyz);
    float facing = saturate(dot(normal, sun_direction));
    float slope = 1.0f - facing;
    float texel_world = ShadowParams.y / max(length(sx.xyz), 0.0001f);
    float3 biased = world_position + normal * texel_world * (1.2f + 2.2f * slope) +
                    sun_direction * texel_world * 0.30f;
    float2 uv = float2(dot(biased, sx.xyz) + sx.w, dot(biased, sy.xyz) + sy.w);
    float depth = dot(biased, sz.xyz) + sz.w;
    if (any(uv < 0.002f) || any(uv > 0.998f))
        return 1.0f;
    float compare_bias = length(sz.xyz) * texel_world * (0.65f + slope * 1.25f);
    float receiver_depth = depth - compare_bias;
    if (ShadowCascades.w < -0.5f)
    {
        float legacy_visibility = 0.0f;
        [unroll]
        for (int row = -1; row <= 1; ++row)
        {
            [unroll]
            for (int column = -1; column <= 1; ++column)
            {
                float2 tap = uv + float2(column, row) * ShadowParams.y;
                float sample_visibility = ShadowMap.SampleCmpLevelZero(
                    ShadowSampler, float3(tap, (float)cascade), receiver_depth);
                float transmission = ShadowTransmission.SampleLevel(
                    DiffuseSampler, float3(tap, (float)cascade), 0.0f).r;
                legacy_visibility += lerp(sample_visibility, 1.0f, transmission);
            }
        }
        return legacy_visibility / 9.0f;
    }
    float blocker_depth = 0.0f;
    float blocker_count = 0.0f;
    static const float2 blocker_offsets[5] = {
        float2(0.0f, 0.0f), float2(-1.0f, 0.0f), float2(1.0f, 0.0f),
        float2(0.0f, -1.0f), float2(0.0f, 1.0f)
    };
    if (cascade < 2)
    {
        [unroll]
        for (int blocker_index = 0; blocker_index < 5; ++blocker_index)
        {
            float2 search_uv = clamp(
                uv + blocker_offsets[blocker_index] * ShadowParams.y * 2.0f,
                ShadowParams.y * 0.5f, 1.0f - ShadowParams.y * 0.5f);
            float stored = ShadowMap.SampleLevel(
                DiffuseSampler, float3(search_uv, (float)cascade), 0.0f).r;
            if (stored < receiver_depth - compare_bias * 0.25f)
            {
                blocker_depth += stored;
                blocker_count += 1.0f;
            }
        }
    }
    float blocker_distance = 0.0f;
    if (blocker_count > 0.0f)
    {
        float average_blocker = blocker_depth / blocker_count;
        blocker_distance = max(0.0f, receiver_depth - average_blocker) /
                           max(length(sz.xyz), 0.00001f);
    }
    float filter_radius = blocker_count > 0.0f
        ? clamp(0.72f + blocker_distance * 0.0045f / max(texel_world, 0.0001f),
                0.72f, 3.25f)
        : (cascade == 2 ? 0.95f : 0.72f);
    static const float2 poisson[8] = {
        float2( 0.000f,  0.000f), float2(-0.326f, -0.406f),
        float2(-0.840f, -0.074f), float2(-0.696f,  0.457f),
        float2(-0.203f,  0.621f), float2( 0.962f, -0.195f),
        float2( 0.473f, -0.480f), float2( 0.519f,  0.767f)
    };
    float3 stable_cell = floor(world_position * 2.0f);
    float orientation = frac(dot(stable_cell, float3(0.1031f, 0.11369f, 0.13787f)));
    float visibility = 0.0f;
    [unroll]
    for (int sample_index = 0; sample_index < 8; ++sample_index)
    {
        float2 disk = poisson[sample_index];
        float2 rotated = orientation < 0.5f ? disk : disk.yx;
        rotated.x *= frac(orientation * 2.0f) < 0.5f ? -1.0f : 1.0f;
        rotated.y *= frac(orientation * 4.0f) < 0.5f ? -1.0f : 1.0f;
        float2 tap = clamp(uv + rotated * ShadowParams.y * filter_radius,
                           ShadowParams.y * 0.5f, 1.0f - ShadowParams.y * 0.5f);
        float sample_visibility = ShadowMap.SampleCmpLevelZero(
            ShadowSampler, float3(tap, (float)cascade), receiver_depth);
        float transmission = ShadowTransmission.SampleLevel(
            DiffuseSampler, float3(tap, (float)cascade), 0.0f).r;
        visibility += lerp(sample_visibility, 1.0f, transmission);
    }
    return visibility / 8.0f;
}

float entity_sun_shadow(float3 world_position, float3 normal)
{
    if (ShadowParams.x <= 0.0f || ShadowParams.z <= 0.0f)
        return 1.0f;
    float camera_depth = max(dot(world_position - Eye.xyz, CameraForward.xyz), 0.0f);
    int cascade = camera_depth < ShadowCascades.x ? 0 :
                  (camera_depth < ShadowCascades.y ? 1 : 2);
    float visibility = entity_cascade_visibility(cascade, world_position, normal);
    if (cascade < 2)
    {
        float split_start = cascade == 0 ? 0.0f : ShadowCascades.x;
        float split_end = cascade == 0 ? ShadowCascades.x : ShadowCascades.y;
        float transition = saturate((camera_depth - lerp(split_start, split_end, 0.86f)) /
                                    max((split_end - split_start) * 0.14f, 0.001f));
        if (transition > 0.0f)
            visibility = lerp(visibility,
                              entity_cascade_visibility(cascade + 1, world_position, normal),
                              transition);
    }
    float far_fade = saturate((camera_depth - ShadowCascades.z * 0.88f) /
                              max(ShadowCascades.z * 0.12f, 0.001f));
    visibility = lerp(visibility, 1.0f, far_fade);
    float opacity = lerp(0.18f, 0.43f, saturate(ShadowParams.x)) * ShadowParams.z;
    return lerp(1.0f, visibility, opacity);
}

void entity_point_face(float3 relative, out int face, out float2 projected, out float forward)
{
    float3 axis = abs(relative);
    if (axis.x >= axis.y && axis.x >= axis.z)
    {
        face = relative.x >= 0.0f ? 0 : 1;
        projected = relative.x >= 0.0f ? float2(-relative.z, relative.y)
                                       : float2(relative.z, relative.y);
        forward = abs(relative.x);
    }
    else if (axis.y >= axis.z)
    {
        face = relative.y >= 0.0f ? 2 : 3;
        projected = relative.y >= 0.0f ? float2(relative.x, -relative.z)
                                       : float2(relative.x, relative.z);
        forward = abs(relative.y);
    }
    else
    {
        face = relative.z >= 0.0f ? 4 : 5;
        projected = relative.z >= 0.0f ? float2(relative.x, relative.y)
                                       : float2(-relative.x, relative.y);
        forward = abs(relative.z);
    }
}

float entity_point_shadow(float3 world_position, float3 normal)
{
    if (TorchAndEye.w <= 0.0f || ShadowParams.w <= 0.0f)
        return 1.0f;
    float3 toward_light = normalize(Eye.xyz - world_position);
    float3 relative = world_position + normal * 0.055f + toward_light * 0.018f - Eye.xyz;
    float range = length(relative);
    if (range <= 0.10f || range >= 9.0f)
        return 1.0f;
    int face;
    float2 projected;
    float forward;
    entity_point_face(relative, face, projected, forward);
    const float near_plane = 0.08f;
    const float far_plane = 9.25f;
    if (forward <= near_plane)
        return 1.0f;
    float2 uv = float2(0.5f + 0.5f * projected.x / forward,
                       0.5f - 0.5f * projected.y / forward);
    uv = clamp(uv, ShadowParams.w * 0.75f, 1.0f - ShadowParams.w * 0.75f);
    float compare_depth = far_plane / (far_plane - near_plane) -
                          near_plane * far_plane /
                              ((far_plane - near_plane) * forward);
    float visibility = 0.0f;
    [unroll]
    for (int row = -1; row <= 1; ++row)
    {
        [unroll]
        for (int column = -1; column <= 1; ++column)
        {
            float2 tap = clamp(uv + float2(column, row) * ShadowParams.w * 1.15f,
                               ShadowParams.w * 0.5f,
                               1.0f - ShadowParams.w * 0.5f);
            visibility += PointShadowMap.SampleCmpLevelZero(
                ShadowSampler, float3(tap, (float)face), compare_depth);
        }
    }
    return lerp(0.14f, 1.0f, visibility / 9.0f);
}

float4 main(PixelInput input) : SV_TARGET
{
    float4 texel = Diffuse.Sample(DiffuseSampler, input.uv);
    bool modern_lighting = AmbientSun.w > 0.5f;
    if (modern_lighting)
        texel.rgb *= texel.rgb;
    float horizontal_distance = distance(input.world_position.xz, Eye.xz);
    // Entities cap their fog: the original never fogged the dragon at all, so
    // a distant dragon reads as a silhouette instead of vanishing.
    float fog = saturate((horizontal_distance - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    float hell_depth = Eye.w * saturate((-32.0f - Eye.y) / 8.0f);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance * 0.032f)) * 0.62f;
    fog = max(fog, hell_air);
    fog = min(fog, 0.55f);
    float normal_length_squared = dot(input.normal, input.normal);
    // Vertex colour is material/albedo (BasicEffect.DiffuseColor times the
    // exported vertex colour), not illumination. Keep it separate so the
    // modern normal-lighting branch does not turn untextured skin and weapon
    // pieces white when it replaces the light accumulator.
    float3 entity_light = 1.0f.xxx;
    if (modern_lighting && normal_length_squared > 0.25f)
    {
        float3 normal = normalize(input.normal);
        if (!input.front_face)
            normal = -normal;
        float3 view_direction = normalize(Eye.xyz - input.world_position);
        float sky_exposure = saturate(input.light_data.x);
        float hemisphere = saturate(normal.y * 0.5f + 0.5f);
        float3 sky_irradiance = lerp(AmbientSun.rgb * 0.62f + FogAndEnd.rgb * 0.08f,
                                     AmbientSun.rgb * 1.34f + FogAndEnd.rgb * 0.13f,
                                     hemisphere);
        float3 ambient = sky_irradiance * sky_exposure *
                         (1.0f + WaterAndTime.w * 0.18f);
        ambient += entity_dynamic_bounce(input.world_position);

        float3 sun_direction = normalize(SunAndOptions.xyz);
        float n_dot_l = saturate(dot(normal, sun_direction));
        float direct_gate = pow(sky_exposure, 4.0f);
        float sun_visibility = entity_sun_shadow(input.world_position, normal);
        float3 direct = LightAndFogStart.rgb * n_dot_l * direct_gate * sun_visibility;

        float placed = saturate(input.light_data.y) *
                       (1.0f - sky_exposure * LightAndFogStart.g * 0.85f);
        // Entity vertices carry the same flood-filled block-light sample as
        // terrain. Use it to reveal monsters beside lava without granting a
        // through-wall or distance-independent enemy light.
        ambient += float3(0.36f, 0.048f, 0.007f) * hell_depth *
                   saturate(input.light_data.y * 1.15f);
        ambient += float3(0.12f, 0.018f, 0.003f) * hell_depth *
                   (0.35f + 0.65f * saturate(input.light_data.y));
        float held = TorchAndEye.w *
                     pow(saturate(1.0f - distance(input.world_position, Eye.xyz) / 9.0f), 2.0f);
        float held_facing = 0.24f + 0.96f * saturate(dot(normal, view_direction));
        float held_visibility = entity_point_shadow(input.world_position, normal);
        float3 torch_color = TorchAndEye.rgb * TorchAndEye.rgb;
        float3 torch = torch_color * (placed * 0.78f +
                                      held * held_facing * held_visibility);
        entity_light = ambient + direct + torch;
    }
    else if (modern_lighting && TorchAndEye.w > 0.0f)
    {
        // Opaque legacy props without normals retain their authored vertex
        // colour but still become readable in the moving local light.
        float held = TorchAndEye.w *
                     pow(saturate(1.0f - distance(input.world_position, Eye.xyz) / 9.0f), 2.0f);
        entity_light += TorchAndEye.rgb * TorchAndEye.rgb * held * 0.55f;
    }
    else if (modern_lighting)
    {
        // A handful of original prop meshes have no usable normals. They still
        // receive the restrained furnace floor so a close prop does not become
        // a literal hole in the image.
        entity_light += float3(0.042f, 0.006f, 0.001f) * hell_depth;
    }
    float3 lit = texel.rgb * input.color.rgb * entity_light;
    float3 fogged = lerp(lit, FogAndEnd.rgb, fog);
#if defined(OPENMINERZ_ALPHA_LIT)
    // Avatar expression and material-overlay batches are ordinary lit
    // BasicEffect surfaces with straight texture alpha. The D3D blend state is
    // premultiplied, so convert here while retaining the full entity lighting.
    float alpha = saturate(texel.a * input.color.a);
    return float4(fogged * alpha, alpha);
#else
    return float4(fogged, 1.0f);
#endif
}
)hlsl";
        // Particles and other soft billboards: no alpha clip, premultiplied
        // output so alpha and additive spans both depth-test against the world
        // and every entity that already wrote depth.
        static constexpr char particle_pixel_source[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
};

Texture2D Diffuse : register(t0);
Texture2D<float> SceneDepth : register(t8);
SamplerState DiffuseSampler : register(s0);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float2 light_data : TEXCOORD3;
};

float4 main(PixelInput input) : SV_TARGET
{
    float4 texel = Diffuse.Sample(DiffuseSampler, input.uv);
    if (AmbientSun.w > 0.5f)
        texel.rgb *= texel.rgb;
    float alpha = saturate(texel.a * input.color.a);
    if (input.light_data.y < -0.5f)
    {
        // Fade a soft billboard over its final 24 cm before the opaque scene
        // surface. The ordinary depth test still rejects everything behind the
        // wall; this removes the hard rectangular cut where a large smoke/fire
        // quad intersects it. Target/crack decals do not carry the marker.
        float raw_depth = SceneDepth.Load(int3(int2(input.position.xy), 0));
        float scene_depth = Projection.w / (raw_depth - Projection.z);
        float particle_depth = dot(input.world_position - Eye.xyz, CameraForward.xyz);
        alpha *= saturate((scene_depth - particle_depth) / 0.24f);
    }
    float horizontal_distance = distance(input.world_position.xz, Eye.xz);
    float fog = saturate((horizontal_distance - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    float hell_depth = Eye.w * saturate((-32.0f - Eye.y) / 8.0f);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance * 0.032f)) * 0.62f;
    fog = max(fog, hell_air);
    fog = min(fog, 0.55f);
    float3 lit = lerp(texel.rgb * input.color.rgb, FogAndEnd.rgb, fog);
    return float4(lit * alpha, alpha);
}
)hlsl";
        const auto vertex_blob = compile_shader(entity_vertex_source, "main", "vs_4_0");
        const auto sun_shadow_vertex_blob =
            compile_shader(entity_sun_shadow_vertex_source, "main", "vs_4_0");
        const auto point_shadow_vertex_blob =
            compile_shader(entity_point_shadow_vertex_source, "main", "vs_4_0");
        const auto pixel_blob = compile_shader(entity_pixel_source, "main", "ps_5_0");
        const std::string alpha_lit_source =
            std::string{"#define OPENMINERZ_ALPHA_LIT 1\n"} + entity_pixel_source;
        const auto alpha_lit_blob = compile_shader(alpha_lit_source.c_str(), "main", "ps_5_0");
        const auto particle_blob = compile_shader(particle_pixel_source, "main", "ps_4_0");
        if (vertex_blob == nullptr || sun_shadow_vertex_blob == nullptr ||
            point_shadow_vertex_blob == nullptr || pixel_blob == nullptr ||
            alpha_lit_blob == nullptr ||
            particle_blob == nullptr ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &entity_vertex_shader_)) ||
            FAILED(device_->CreateVertexShader(sun_shadow_vertex_blob->GetBufferPointer(),
                                               sun_shadow_vertex_blob->GetBufferSize(), nullptr,
                                               &entity_sun_shadow_vertex_shader_)) ||
            FAILED(device_->CreateVertexShader(point_shadow_vertex_blob->GetBufferPointer(),
                                               point_shadow_vertex_blob->GetBufferSize(), nullptr,
                                               &entity_point_shadow_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr,
                                              &entity_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(alpha_lit_blob->GetBufferPointer(),
                                              alpha_lit_blob->GetBufferSize(), nullptr,
                                              &entity_alpha_lit_pixel_shader_)) ||
            FAILED(device_->CreatePixelShader(particle_blob->GetBufferPointer(),
                                              particle_blob->GetBufferSize(), nullptr,
                                              &particle_pixel_shader_)))
            return;
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 5> entity_elements{{
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 12U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 20U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 36U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 1U, DXGI_FORMAT_R32G32_FLOAT, 0U, 48U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
        }};
        if (FAILED(device_->CreateInputLayout(entity_elements.data(),
                                              static_cast<UINT>(entity_elements.size()),
                                              vertex_blob->GetBufferPointer(),
                                              vertex_blob->GetBufferSize(), &entity_input_layout_)))
            return;
        // Premultiplied alpha and additive blends for the soft spans.
        D3D11_BLEND_DESC blend_description{};
        auto &target_blend = blend_description.RenderTarget[0];
        target_blend.BlendEnable = TRUE;
        target_blend.SrcBlend = D3D11_BLEND_ONE;
        target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOp = D3D11_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend_description, &entity_alpha_blend_state_)))
            return;
        target_blend.DestBlend = D3D11_BLEND_ONE;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target_blend.DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device_->CreateBlendState(&blend_description, &entity_additive_blend_state_)))
            return;
        // Premultiplied multiplicative decal: src*dst + dst*(1-src.a).
        // Transparent crack texels therefore leave the terrain untouched.
        target_blend.SrcBlend = D3D11_BLEND_DEST_COLOR;
        target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target_blend.DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device_->CreateBlendState(&blend_description, &entity_multiply_blend_state_)))
            return;
        entity_ready_ = true;
    }

    [[nodiscard]] ID3D11ShaderResourceView *entity_view(SDL_Texture *texture)
    {
        if (texture == nullptr)
            return nullptr;
        const auto found = entity_srv_cache_.find(texture);
        if (found != entity_srv_cache_.end())
            return found->second.Get();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
        auto *resource = static_cast<ID3D11Texture2D *>(
            SDL_GetPointerProperty(properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        if (resource != nullptr)
            device_->CreateShaderResourceView(resource, nullptr, &view);
        ID3D11ShaderResourceView *raw = view.Get();
        entity_srv_cache_.emplace(texture, std::move(view));
        return raw;
    }

    [[nodiscard]] bool upload_dynamic_radiance()
    {
        const std::size_t expected = static_cast<std::size_t>(world::dynamic_radiance_extent) *
                                     world::dynamic_radiance_extent *
                                     world::dynamic_radiance_extent;
        if (!dynamic_radiance_volume_.active || dynamic_radiance_volume_.texels.size() != expected)
        {
            return false;
        }
        if (dynamic_radiance_texture_ == nullptr)
        {
            D3D11_TEXTURE3D_DESC description{};
            description.Width = world::dynamic_radiance_extent;
            description.Height = world::dynamic_radiance_extent;
            description.Depth = world::dynamic_radiance_extent;
            description.MipLevels = 1U;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(
                    device_->CreateTexture3D(&description, nullptr, &dynamic_radiance_texture_)) ||
                FAILED(device_->CreateShaderResourceView(dynamic_radiance_texture_.Get(), nullptr,
                                                         &dynamic_radiance_srv_)))
            {
                dynamic_radiance_texture_.Reset();
                dynamic_radiance_srv_.Reset();
                return false;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(dynamic_radiance_texture_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                 &mapped)))
        {
            return false;
        }
        constexpr std::size_t bytes_per_cell = 4U;
        const std::size_t row_bytes =
            static_cast<std::size_t>(world::dynamic_radiance_extent) * bytes_per_cell;
        const auto *source =
            reinterpret_cast<const std::uint8_t *>(dynamic_radiance_volume_.texels.data());
        auto *destination = static_cast<std::uint8_t *>(mapped.pData);
        for (int z = 0; z < world::dynamic_radiance_extent; ++z)
            for (int y = 0; y < world::dynamic_radiance_extent; ++y)
            {
                const std::size_t source_row =
                    static_cast<std::size_t>(z * world::dynamic_radiance_extent + y) * row_bytes;
                const std::size_t destination_row =
                    static_cast<std::size_t>(z) * mapped.DepthPitch +
                    static_cast<std::size_t>(y) * mapped.RowPitch;
                std::memcpy(destination + destination_row, source + source_row, row_bytes);
            }
        context_->Unmap(dynamic_radiance_texture_.Get(), 0U);
        return true;
    }

    [[nodiscard]] bool upload_entity_batch()
    {
        const UINT vertex_count = static_cast<UINT>(entity_batch_.vertices.size());
        entity_uploaded_count_ = 0U;
        if (vertex_count == 0U)
            return true;
        if (entity_vertex_buffer_ == nullptr || entity_vertex_capacity_ < vertex_count)
        {
            entity_vertex_buffer_.Reset();
            entity_vertex_capacity_ = std::max(vertex_count, entity_vertex_capacity_ * 2U);
            const D3D11_BUFFER_DESC description{
                static_cast<UINT>(entity_vertex_capacity_ * sizeof(EntityVertex)),
                D3D11_USAGE_DYNAMIC,
                D3D11_BIND_VERTEX_BUFFER,
                D3D11_CPU_ACCESS_WRITE,
                0U,
                sizeof(EntityVertex)};
            if (FAILED(device_->CreateBuffer(&description, nullptr, &entity_vertex_buffer_)))
            {
                entity_ready_ = false;
                return false;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(entity_vertex_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                 &mapped)))
            return false;
        std::copy(entity_batch_.vertices.begin(), entity_batch_.vertices.end(),
                  static_cast<EntityVertex *>(mapped.pData));
        context_->Unmap(entity_vertex_buffer_.Get(), 0U);
        entity_uploaded_count_ = vertex_count;
        return true;
    }

    void draw_entity_shadow_geometry(ID3D11VertexShader *entity_shadow_shader,
                                     ID3D11VertexShader *terrain_shadow_shader)
    {
        if (entity_uploaded_count_ == 0U || entity_shadow_shader == nullptr)
            return;
        const UINT stride = sizeof(EntityVertex);
        const UINT offset = 0U;
        context_->IASetInputLayout(entity_input_layout_.Get());
        ID3D11Buffer *vertex_buffer = entity_vertex_buffer_.Get();
        context_->IASetVertexBuffers(0U, 1U, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(entity_shadow_shader, nullptr, 0U);
        context_->PSSetShader(nullptr, nullptr, 0U);
        for (const EntitySpan &span : entity_batch_.spans)
            if (span.blend == EntityBlend::opaque)
                context_->Draw(span.count, span.first);

        context_->IASetInputLayout(input_layout_.Get());
        context_->VSSetShader(terrain_shadow_shader, nullptr, 0U);
        context_->PSSetShader(shadow_pixel_shader_.Get(), nullptr, 0U);
    }

    void draw_entity_batch()
    {
        const UINT stride = sizeof(EntityVertex);
        const UINT offset = 0U;
        context_->IASetInputLayout(entity_input_layout_.Get());
        ID3D11Buffer *vertex_buffer = entity_vertex_buffer_.Get();
        context_->IASetVertexBuffers(0U, 1U, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(entity_vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(entity_pixel_shader_.Get(), nullptr, 0U);
        bool any_blended = false;
        for (const EntitySpan &span : entity_batch_.spans)
        {
            if (span.blend != EntityBlend::opaque)
            {
                any_blended = true;
                continue;
            }
            ID3D11ShaderResourceView *view = entity_view(span.texture);
            if (view == nullptr)
                continue;
            context_->PSSetShaderResources(0U, 1U, &view);
            context_->Draw(span.count, span.first);
        }
        // Soft spans second: depth-tested against everything opaque (terrain and
        // entities alike) but never writing depth themselves, so particles stop
        // showing through zombies and blocks.
        if (any_blended)
        {
            // D3D11 permits the same depth resource as an SRV only through a
            // read-only DSV. Soft particles sample it for intersection fading
            // while the normal LESS_EQUAL test still owns occlusion.
            ID3D11RenderTargetView *entity_target = target_view_.Get();
            context_->OMSetRenderTargets(1U, &entity_target, depth_view_readonly_.Get());
            context_->OMSetDepthStencilState(water_depth_state_.Get(), 0U);
            ID3D11ShaderResourceView *scene_depth = depth_srv_.Get();
            context_->PSSetShaderResources(8U, 1U, &scene_depth);
            EntityBlend active_blend = EntityBlend::opaque;
            ID3D11PixelShader *active_pixel_shader{};
            for (const EntitySpan &span : entity_batch_.spans)
            {
                if (span.blend == EntityBlend::opaque)
                    continue;
                ID3D11ShaderResourceView *view = entity_view(span.texture);
                if (view == nullptr)
                    continue;
                ID3D11PixelShader *pixel_shader =
                    span.blend == EntityBlend::alpha_lit
                        ? entity_alpha_lit_pixel_shader_.Get()
                        : particle_pixel_shader_.Get();
                if (pixel_shader != active_pixel_shader)
                {
                    active_pixel_shader = pixel_shader;
                    context_->PSSetShader(active_pixel_shader, nullptr, 0U);
                }
                if (span.blend != active_blend)
                {
                    active_blend = span.blend;
                    ID3D11BlendState *blend = entity_alpha_blend_state_.Get();
                    if (active_blend == EntityBlend::additive)
                        blend = entity_additive_blend_state_.Get();
                    else if (active_blend == EntityBlend::multiply)
                        blend = entity_multiply_blend_state_.Get();
                    context_->OMSetBlendState(blend, nullptr, 0xFFFFFFFFU);
                }
                context_->PSSetShaderResources(0U, 1U, &view);
                context_->Draw(span.count, span.first);
            }
            ID3D11ShaderResourceView *empty_depth{};
            context_->PSSetShaderResources(8U, 1U, &empty_depth);
            context_->OMSetRenderTargets(1U, &entity_target, depth_view_.Get());
            context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
            context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
        }
        // Restore the voxel pipeline so the water pass keeps working unchanged.
        context_->IASetInputLayout(input_layout_.Get());
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0U);
        const std::array material_views{atlas_array_view_.Get(), normal_spec_array_view_.Get(),
                                        metal_light_array_view_.Get()};
        context_->PSSetShaderResources(0U, static_cast<UINT>(material_views.size()),
                                       material_views.data());
    }

    [[nodiscard]] bool ensure_target(int width, int height)
    {
        if (target_texture_ != nullptr && target_width_ == width && target_height_ == height)
        {
            return true;
        }
        SDL_DestroyTexture(target_texture_);
        target_texture_ = nullptr;
        target_view_.Reset();
        target_srv_.Reset();
        target_resource_.Reset();
        presentation_resource_.Reset();
        depth_view_.Reset();
        depth_view_readonly_.Reset();
        depth_srv_.Reset();
        depth_texture_.Reset();
        scene_copy_resource_.Reset();
        scene_copy_srv_.Reset();
        post_resource_.Reset();
        post_rtv_.Reset();
        bloom_a_resource_.Reset();
        bloom_a_rtv_.Reset();
        bloom_a_srv_.Reset();
        bloom_b_resource_.Reset();
        bloom_b_rtv_.Reset();
        bloom_b_srv_.Reset();
        post_ready_ = false;
        target_width_ = 0;
        target_height_ = 0;

        target_texture_ = SDL_CreateTexture(renderer_, presentation_format_,
                                            SDL_TEXTUREACCESS_STATIC, width, height);
        if (target_texture_ == nullptr)
        {
            failure_reason_ =
                std::format("failed to create the SDL world texture ({})", SDL_GetError());
            return false;
        }
        const SDL_PropertiesID texture_properties = SDL_GetTextureProperties(target_texture_);
        auto *presentation_resource = static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
            texture_properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        if (presentation_resource == nullptr)
        {
            failure_reason_ = "SDL world texture did not expose its D3D11 resource";
            return false;
        }
        presentation_resource_ = presentation_resource;
        D3D11_TEXTURE2D_DESC color_description{};
        presentation_resource_->GetDesc(&color_description);
        color_description.Usage = D3D11_USAGE_DEFAULT;
        color_description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        color_description.CPUAccessFlags = 0U;
        color_description.MiscFlags = 0U;
        // The scene renders into a HALF-FLOAT target. With the old 8-bit
        // buffer every sunlit surface clipped at 1.0 before post ever ran, so
        // bright sand and snow arrived at the tone mapper as flat white with
        // the texture already destroyed. Float keeps the headroom that makes
        // exposure and a filmic curve mean anything.
        D3D11_TEXTURE2D_DESC scene_description = color_description;
        scene_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdr_scene_ =
            SUCCEEDED(device_->CreateTexture2D(&scene_description, nullptr, &target_resource_));
        if (!hdr_scene_ &&
            FAILED(device_->CreateTexture2D(&color_description, nullptr, &target_resource_)))
        {
            failure_reason_ = "failed to allocate the D3D11 world color target";
            return false;
        }
        if (FAILED(
                device_->CreateRenderTargetView(target_resource_.Get(), nullptr, &target_view_)) ||
            FAILED(
                device_->CreateShaderResourceView(target_resource_.Get(), nullptr, &target_srv_)))
        {
            failure_reason_ = "failed to bind the D3D11 world target as an RTV";
            return false;
        }
        // The world target composites over the separately drawn skybox; its
        // contents are premultiplied (cleared to transparent black, blended in
        // place), and the post pass adds premultiplied light on top.
        SDL_SetTextureBlendMode(target_texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(target_texture_, SDL_SCALEMODE_LINEAR);

        D3D11_TEXTURE2D_DESC depth_description{};
        depth_description.Width = static_cast<UINT>(width);
        depth_description.Height = static_cast<UINT>(height);
        depth_description.MipLevels = 1U;
        depth_description.ArraySize = 1U;
        depth_description.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depth_description.SampleDesc.Count = 1U;
        depth_description.Usage = D3D11_USAGE_DEFAULT;
        depth_description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_description{};
        depth_view_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC depth_resource_description{};
        depth_resource_description.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depth_resource_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        depth_resource_description.Texture2D.MipLevels = 1U;
        if (FAILED(device_->CreateTexture2D(&depth_description, nullptr, &depth_texture_)) ||
            FAILED(device_->CreateDepthStencilView(depth_texture_.Get(), &depth_view_description,
                                                   &depth_view_)))
        {
            failure_reason_ = "failed to allocate the voxel depth target";
            return false;
        }
        depth_view_description.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        if (FAILED(device_->CreateDepthStencilView(depth_texture_.Get(), &depth_view_description,
                                                   &depth_view_readonly_)) ||
            FAILED(device_->CreateShaderResourceView(depth_texture_.Get(),
                                                     &depth_resource_description, &depth_srv_)))
        {
            failure_reason_ = "failed to expose the voxel depth target for sampling";
            return false;
        }

        // Refraction snapshot + post-processing chain targets.  These are all
        // optional quality features: on failure the plain scene still presents.
        // The scene copy and bloom chain match the scene's precision; only the
        // final post target keeps the presentation format so the finished
        // frame can be copied straight into the SDL texture.
        const DXGI_FORMAT scene_format =
            hdr_scene_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : color_description.Format;
        D3D11_TEXTURE2D_DESC copy_description = color_description;
        copy_description.Format = scene_format;
        copy_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC post_description = color_description;
        post_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        bloom_width_ = std::max(1, width / 4);
        bloom_height_ = std::max(1, height / 4);
        D3D11_TEXTURE2D_DESC bloom_description = color_description;
        bloom_description.Format = scene_format;
        bloom_description.Width = static_cast<UINT>(bloom_width_);
        bloom_description.Height = static_cast<UINT>(bloom_height_);
        const bool extras_created =
            SUCCEEDED(
                device_->CreateTexture2D(&copy_description, nullptr, &scene_copy_resource_)) &&
            SUCCEEDED(device_->CreateShaderResourceView(scene_copy_resource_.Get(), nullptr,
                                                        &scene_copy_srv_)) &&
            SUCCEEDED(device_->CreateTexture2D(&post_description, nullptr, &post_resource_)) &&
            SUCCEEDED(device_->CreateRenderTargetView(post_resource_.Get(), nullptr, &post_rtv_)) &&
            SUCCEEDED(device_->CreateTexture2D(&bloom_description, nullptr, &bloom_a_resource_)) &&
            SUCCEEDED(
                device_->CreateRenderTargetView(bloom_a_resource_.Get(), nullptr, &bloom_a_rtv_)) &&
            SUCCEEDED(device_->CreateShaderResourceView(bloom_a_resource_.Get(), nullptr,
                                                        &bloom_a_srv_)) &&
            SUCCEEDED(device_->CreateTexture2D(&bloom_description, nullptr, &bloom_b_resource_)) &&
            SUCCEEDED(
                device_->CreateRenderTargetView(bloom_b_resource_.Get(), nullptr, &bloom_b_rtv_)) &&
            SUCCEEDED(
                device_->CreateShaderResourceView(bloom_b_resource_.Get(), nullptr, &bloom_b_srv_));
        post_ready_ = extras_created && post_vertex_shader_ != nullptr &&
                      post_depth_state_ != nullptr && blur_buffer_ != nullptr;
        if (!post_ready_ && hdr_scene_)
        {
            // Without the post chain the scene is copied straight into the SDL
            // texture, and CopyResource demands identical formats. Fall back to
            // a matching 8-bit scene target rather than presenting nothing.
            target_view_.Reset();
            target_srv_.Reset();
            target_resource_.Reset();
            hdr_scene_ = false;
            if (FAILED(device_->CreateTexture2D(&color_description, nullptr, &target_resource_)) ||
                FAILED(device_->CreateRenderTargetView(target_resource_.Get(), nullptr,
                                                       &target_view_)) ||
                FAILED(device_->CreateShaderResourceView(target_resource_.Get(), nullptr,
                                                         &target_srv_)))
            {
                failure_reason_ = "failed to allocate the D3D11 world color target";
                return false;
            }
        }
        target_width_ = width;
        target_height_ = height;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlas_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal_spec_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metal_light_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlas_array_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal_spec_array_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metal_light_array_view_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> tile_array_textures_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> aniso_sampler_state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scene_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> water_vertex_buffer_;
    std::map<std::tuple<int, int, int>, GpuChunkMesh> chunk_meshes_;
    std::map<std::tuple<int, int, int>, std::vector<std::array<int, 2>>> crash_site_columns_;
    std::array<std::array<float, 4>, maximum_crater_masks> crater_masks_{};
    bool crater_masks_dirty_{true};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> water_depth_state_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> water_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_resource_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> presentation_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> target_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_view_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_view_readonly_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shadow_texture_;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, 3> shadow_views_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shadow_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shadow_transmission_texture_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 3> shadow_transmission_views_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shadow_transmission_srv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadow_sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> shadow_rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shadow_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shadow_pixel_shader_;
    static constexpr UINT point_shadow_resolution = 384U;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> point_shadow_texture_;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, 6> point_shadow_views_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> point_shadow_srv_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> point_shadow_rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> point_shadow_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> dynamic_radiance_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> dynamic_radiance_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_copy_resource_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scene_copy_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> post_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> post_rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bloom_a_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bloom_a_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bloom_a_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bloom_b_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bloom_b_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bloom_b_srv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> post_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bright_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blur_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> composite_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> post_depth_state_;
    // Auto exposure: log-luminance downsample plus a 1x1 adaptation pair.
    static constexpr UINT luminance_extent = 64U;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> luminance_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> adapt_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> luminance_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> luminance_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> luminance_srv_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> adapt_resource_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 2> adapt_rtv_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> adapt_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> adapt_staging_;
    // Last value the CPU managed to read back, in exposure multiplier form.
    float readback_exposure_{1.0F};
    std::size_t adapt_index_{};
    bool adapt_primed_{};
    bool exposure_ready_{};
    float last_frame_seconds_{-1.0F};
    int tone_curve_{3};
    bool auto_exposure_enabled_{true};
    float exposure_compensation_{};
    float color_grading_{1.0F};
    Microsoft::WRL::ComPtr<ID3D11Buffer> blur_buffer_;
    int bloom_width_{1};
    int bloom_height_{1};
    bool post_ready_{};
    Microsoft::WRL::ComPtr<ID3D11VertexShader> entity_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> entity_sun_shadow_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> entity_point_shadow_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> entity_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> entity_alpha_lit_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> particle_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> entity_input_layout_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> entity_alpha_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> entity_additive_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> entity_multiply_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> entity_vertex_buffer_;
    std::unordered_map<SDL_Texture *, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
        entity_srv_cache_;
    UINT entity_vertex_capacity_{};
    UINT entity_uploaded_count_{};
    bool entity_ready_{};
    UINT vertex_count_{};
    UINT water_vertex_count_{};
#endif
    EntityBatch entity_batch_;
    world::DynamicRadianceVolume dynamic_radiance_volume_;

    SDL_Renderer *renderer_{};
    SDL_Texture *target_texture_{};
    int target_width_{};
    int target_height_{};
    SDL_PixelFormat presentation_format_{SDL_PIXELFORMAT_UNKNOWN};
    // True when the scene target is half-float (headroom above 1.0).
    bool hdr_scene_{};
    bool available_{};
    std::string failure_reason_;
};

DepthVoxelRenderer::DepthVoxelRenderer(SDL_Renderer *renderer, SDL_Texture *atlas,
                                       SDL_Texture *normal_spec_atlas,
                                       SDL_Texture *metal_light_atlas)
    : impl_(std::make_unique<Impl>(renderer, atlas, normal_spec_atlas, metal_light_atlas))
{
}

DepthVoxelRenderer::~DepthVoxelRenderer() = default;

bool DepthVoxelRenderer::available() const noexcept
{
    return impl_->available();
}

std::string_view DepthVoxelRenderer::failure_reason() const noexcept
{
    return impl_->failure_reason();
}

void DepthVoxelRenderer::rebuild(const std::vector<world::WorldFace> &faces, int minimum_x,
                                 int minimum_z, int maximum_x, int maximum_z)
{
    impl_->rebuild(faces, minimum_x, minimum_z, maximum_x, maximum_z);
}

void DepthVoxelRenderer::rebuild_chunks(const std::vector<world::ChunkFaceBatch> &batches,
                                        int minimum_x, int minimum_z, int maximum_x, int maximum_z)
{
    impl_->rebuild_chunks(batches, minimum_x, minimum_z, maximum_x, maximum_z);
}

void DepthVoxelRenderer::replace_chunk(const world::ChunkFaceBatch &batch)
{
    impl_->replace_chunk(batch);
}

void DepthVoxelRenderer::retain_chunks(int minimum_chunk_x, int minimum_chunk_z,
                                       int maximum_chunk_x, int maximum_chunk_z)
{
    impl_->retain_chunks(minimum_chunk_x, minimum_chunk_z, maximum_chunk_x, maximum_chunk_z);
}

void DepthVoxelRenderer::set_world_bounds(int minimum_x, int minimum_z, int maximum_x,
                                          int maximum_z)
{
    impl_->set_world_bounds(minimum_x, minimum_z, maximum_x, maximum_z);
}

void DepthVoxelRenderer::submit_entities(EntityBatch batch)
{
    impl_->submit_entities(std::move(batch));
}

void DepthVoxelRenderer::submit_dynamic_radiance(world::DynamicRadianceVolume volume)
{
    impl_->submit_dynamic_radiance(std::move(volume));
}

void DepthVoxelRenderer::set_grading(int tone_curve, bool auto_exposure,
                                     float exposure_compensation, float color_grading) noexcept
{
    impl_->set_grading(tone_curve, auto_exposure, exposure_compensation, color_grading);
}

DisplayGrading DepthVoxelRenderer::display_grading() const noexcept
{
    return impl_->display_grading();
}

bool DepthVoxelRenderer::draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
                              const world::PlayerState &player, float vertical_fov_degrees,
                              float render_scale, float elapsed_seconds, float visibility_distance,
                              world::BiomeKind biome, bool water_enabled, bool enhanced_foliage,
                              bool ambient_occlusion, bool ore_glow, float shadow_strength)
{
    return impl_->draw(metrics, sky, player, vertical_fov_degrees, render_scale, elapsed_seconds,
                       visibility_distance, biome, water_enabled, enhanced_foliage,
                       ambient_occlusion, ore_glow, shadow_strength);
}

} // namespace openminerz
