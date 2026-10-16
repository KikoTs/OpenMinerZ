#include "depth_voxel_renderer.hpp"

#include "far_terrain_renderer.hpp"
#include "hell_atmosphere.hpp"
#include "modding/graphics/mod_light_gpu.hpp"
#include "modding/graphics/mod_output.hpp"
#include "vfx/vfx_renderer.hpp"
#include "post_processing.hpp"

#include "openminerz/view_projection.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/hell_atmosphere.hpp"
#include "openminerz/world/liquid.hpp" // mod-liquid
#include "openminerz/world/shadow_cascade.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
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

#include "shader_cache.hpp"
#endif

namespace openminerz
{
namespace
{
constexpr std::size_t maximum_crater_masks = 32U;

// The vertex layout and its packing live at file scope, outside the D3D11
// guard, so a mesh worker can build a section's payload without a device.
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

[[nodiscard]] std::array<float, 3> face_normal(world::FaceDirection direction) noexcept
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

// Foliage sways in the vertex shader; everything else stays rigid.
[[nodiscard]] float block_wave(world::BlockType type) noexcept
{
    return type == world::BlockType::leaves ? 1.0F : 0.0F;
}

// Snow sparkles and ice reflects: per-vertex gloss lifts the authored material
// response for those surfaces without touching the derived texture atlases.
[[nodiscard]] float block_gloss(world::BlockType type) noexcept
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

[[nodiscard]] bool is_torch_block(world::BlockType type) noexcept
{
    return type >= world::BlockType::torch && type <= world::BlockType::torch_neg_y;
}

[[nodiscard]] std::uint32_t pack_unorm8(const std::array<std::uint8_t, 4> &light) noexcept
{
    return static_cast<std::uint32_t>(light[0]) | static_cast<std::uint32_t>(light[1]) << 8U |
           static_cast<std::uint32_t>(light[2]) << 16U |
           static_cast<std::uint32_t>(light[3]) << 24U;
}

[[nodiscard]] std::uint32_t pack_material(float gloss, float wave, std::uint8_t tile,
                                          std::uint8_t lava) noexcept
{
    const auto unorm = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return static_cast<std::uint32_t>(unorm(gloss)) |
           static_cast<std::uint32_t>(unorm(wave)) << 8U |
           static_cast<std::uint32_t>(tile) << 16U | static_cast<std::uint32_t>(lava) << 24U;
}

[[nodiscard]] std::vector<GpuVertex> build_vertices(const std::vector<world::WorldFace> &faces)
{
    std::vector<GpuVertex> vertices;
    vertices.reserve(faces.size() * 6U);
    constexpr std::array<int, 6> triangle_order{0, 1, 2, 0, 2, 3};
    for (const world::WorldFace &face : faces)
    {
        // Torch blocks render as the real torch prop through the entity
        // pass now — never as voxel geometry. (mod-liquid) Liquid faces are
        // drawn translucent, after everything opaque (build_liquid_vertices).
        if (is_torch_block(face.block) || face.liquid != 0U)
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
                                // (The whole byte: mod block tiles are 64..255.)
                                pack_material(gloss, wave, face.tile, face.lava_light[index]),
                                pack_unorm8(face.indirect_light),
                                pack_unorm8(face.block_light_lobe)});
        }
    }
    return vertices;
}

// (mod-liquid) The faces of liquid cells (WorldFace::liquid), for the
// translucent pass: a cell with no liquid above it has its top face, and the
// upper edge of its sides, at the lowered surface. Emission -2 marks them for
// the terrain shader's liquid branch (-1 is the Lagoon's sea).
[[nodiscard]] std::vector<GpuVertex> build_liquid_vertices(const std::vector<world::WorldFace> &faces)
{
    std::vector<GpuVertex> vertices;
    constexpr std::array<int, 6> triangle_order{0, 1, 2, 0, 2, 3};
    for (const world::WorldFace &face : faces)
    {
        if (face.liquid == 0U)
            continue;
        const float x = static_cast<float>(face.x);
        const float y = static_cast<float>(face.y);
        const float z = static_cast<float>(face.z);
        const float top = y + 1.0F - (face.liquid == 2U ? world::liquid_surface_drop : 0.0F);
        std::array<std::array<float, 3>, 4> corners{};
        switch (face.direction)
        {
        case world::FaceDirection::positive_x:
            corners = {{{{x + 1.0F, y, z}}, {{x + 1.0F, top, z}}, {{x + 1.0F, top, z + 1.0F}},
                        {{x + 1.0F, y, z + 1.0F}}}};
            break;
        case world::FaceDirection::negative_z:
            corners = {{{{x + 1.0F, y, z}}, {{x, y, z}}, {{x, top, z}}, {{x + 1.0F, top, z}}}};
            break;
        case world::FaceDirection::negative_x:
            corners = {{{{x, y, z + 1.0F}}, {{x, top, z + 1.0F}}, {{x, top, z}}, {{x, y, z}}}};
            break;
        case world::FaceDirection::positive_z:
            corners = {{{{x, y, z + 1.0F}}, {{x + 1.0F, y, z + 1.0F}}, {{x + 1.0F, top, z + 1.0F}},
                        {{x, top, z + 1.0F}}}};
            break;
        case world::FaceDirection::positive_y:
            corners = {{{{x, top, z}}, {{x + 1.0F, top, z}}, {{x + 1.0F, top, z + 1.0F}},
                        {{x, top, z + 1.0F}}}};
            break;
        case world::FaceDirection::negative_y:
            corners = {{{{x, y, z + 1.0F}}, {{x + 1.0F, y, z + 1.0F}}, {{x + 1.0F, y, z}}, {{x, y, z}}}};
            break;
        }
        const auto texture_coordinates =
            world::atlas_uv_corners(face.direction, 0.0F, 0.0F, 1.0F, 1.0F);
        const auto normal = face_normal(face.direction);
        for (const int corner : triangle_order)
        {
            const auto index = static_cast<std::size_t>(corner);
            vertices.push_back({corners[index][0], corners[index][1], corners[index][2],
                                texture_coordinates[index][0], texture_coordinates[index][1],
                                normal[0], normal[1], normal[2],
                                static_cast<float>(face.ambient_occlusion[index]) / 255.0F,
                                static_cast<float>(face.sky_light[index]) / 255.0F, -2.0F,
                                static_cast<float>(face.block_light[index]) / 255.0F,
                                pack_material(0.0F, 0.0F, face.tile, face.lava_light[index]),
                                pack_unorm8(face.indirect_light),
                                pack_unorm8(face.block_light_lobe)});
        }
    }
    return vertices;
}

[[nodiscard]] std::vector<std::array<int, 2>> scan_crash_site_columns(
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
    return columns;
}

// OPENMINERZ_DEEP_UNDERGROUND_SHADERS=0 compiles the terrain and creature
// shaders without the deep underground's blocks (hell_atmosphere.hpp): text
// for text the shaders they were before it existed. Diagnostic only; it is how
// the Original presentation is shown, pixel for pixel, to be untouched by it.
[[nodiscard]] bool deep_underground_shaders_wanted() noexcept
{
    const char *value = SDL_getenv("OPENMINERZ_DEEP_UNDERGROUND_SHADERS");
    return value == nullptr || std::string_view{value} != "0";
}

// OPENMINERZ_DIAGNOSTIC_RENDER_CLOCK=SECONDS draws every frame of the run as
// if the world clock stood at that moment. Diagnostic only: everything a
// shader animates (flicker, pulses, drift, haze, embers) is then the same in
// two runs, so two stills can be compared pixel for pixel, and a sequence of
// runs a fraction of a second apart is a motion sequence.
[[nodiscard]] float diagnostic_render_clock(float elapsed_seconds) noexcept
{
    static const float pinned = [] {
        const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_RENDER_CLOCK");
        return value != nullptr ? static_cast<float>(std::atof(value)) : -1.0F;
    }();
    return pinned >= 0.0F ? pinned : elapsed_seconds;
}
} // namespace

PackedChunkMesh DepthVoxelRenderer::pack_chunk(const world::ChunkFaceBatch &batch)
{
    PackedChunkMesh packed;
    packed.coordinate = batch.coordinate;
    packed.vertex_stride = static_cast<std::uint32_t>(sizeof(GpuVertex));
    packed.crash_columns = scan_crash_site_columns(batch.faces);
    packed.lava_columns = scan_lava_columns(batch.faces);
    std::vector<GpuVertex> vertices = build_vertices(batch.faces);
    packed.vertex_count = static_cast<std::uint32_t>(vertices.size());
    // (mod-liquid) Liquid faces after the opaque ones, in the same buffer.
    const std::vector<GpuVertex> liquid = build_liquid_vertices(batch.faces);
    packed.liquid_vertex_count = static_cast<std::uint32_t>(liquid.size());
    vertices.insert(vertices.end(), liquid.begin(), liquid.end());
    packed.vertices.resize(vertices.size() * sizeof(GpuVertex));
    if (!vertices.empty())
        std::memcpy(packed.vertices.data(), vertices.data(), packed.vertices.size());
    return packed;
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
        // Compiled here, with the rest of the pipeline, so switching to the
        // Enhanced presentation or turning an effect on never compiles a
        // shader during play. Optional: without it the Enhanced presentation
        // falls back to the chain above.
        post_processor_ = std::make_unique<PostProcessor>(device_.Get(), context_.Get());
        // The deep underground's lava map, air pass and embers, compiled at
        // load like everything else. Optional in the same way.
        hell_ = std::make_unique<HellAtmosphere>(device_.Get(), context_.Get());
        // The far landscape and the surface air that goes with it
        // (far_terrain_renderer.hpp). Optional in the same way: without it the
        // Enhanced presentation ends at the detailed window, as before.
        far_terrain_ = std::make_unique<FarTerrainRenderer>(device_.Get(), context_.Get());
        // The Enhanced presentation's effects (vfx/vfx_renderer.hpp): shaders
        // compiled and flipbooks baked here, at load. Optional in the same way:
        // without it the older particle path draws every effect.
        vfx_ = std::make_unique<VfxRenderer>(device_.Get(), context_.Get());
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
        track_lava_faces(faces);
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
        if (hell_ != nullptr)
            hell_->lava().clear();
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
        if (hell_ != nullptr)
            hell_->lava().set_section(key, scan_lava_columns(batch.faces));
        if (batch.faces.empty())
        {
            chunk_meshes_.erase(key);
            return;
        }
        auto vertices = build_vertices(batch.faces);
        GpuChunkMesh mesh{};
        mesh.vertex_count = static_cast<UINT>(vertices.size());
        // (mod-liquid) Liquid faces after the opaque ones.
        const auto liquid = build_liquid_vertices(batch.faces);
        mesh.liquid_count = static_cast<UINT>(liquid.size());
        vertices.insert(vertices.end(), liquid.begin(), liquid.end());
        if (vertices.empty())
        {
            chunk_meshes_.erase(key);
            return;
        }
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

    // Everything replace_chunk does that needs no device has already run on the
    // mesh worker; all that is left here is the buffer upload.
    void install_packed_chunk(PackedChunkMesh packed)
    {
#if defined(_WIN32)
        if (!available_)
        {
            return;
        }
        const auto key = std::tuple{packed.coordinate.x, packed.coordinate.z, packed.coordinate.y};
        track_crash_site_columns(key, std::move(packed.crash_columns));
        if (hell_ != nullptr)
            hell_->lava().set_section(key, std::move(packed.lava_columns));
        if (packed.vertex_count == 0U && packed.liquid_vertex_count == 0U)
        {
            chunk_meshes_.erase(key);
            return;
        }
        GpuChunkMesh mesh{};
        mesh.vertex_count = static_cast<UINT>(packed.vertex_count);
        mesh.liquid_count = static_cast<UINT>(packed.liquid_vertex_count); // (mod-liquid)
        const D3D11_BUFFER_DESC description{static_cast<UINT>(packed.vertices.size()),
                                            D3D11_USAGE_IMMUTABLE,
                                            D3D11_BIND_VERTEX_BUFFER,
                                            0U,
                                            0U,
                                            packed.vertex_stride};
        const D3D11_SUBRESOURCE_DATA data{packed.vertices.data(), 0U, 0U};
        if (FAILED(device_->CreateBuffer(&description, &data, &mesh.vertex_buffer)))
        {
            available_ = false;
            failure_reason_ = "failed to upload a chunk-local voxel mesh";
            return;
        }
        chunk_meshes_.insert_or_assign(key, std::move(mesh));
#else
        (void)packed;
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
        if (hell_ != nullptr)
            hell_->lava().retain(minimum_chunk_x, minimum_chunk_z, maximum_chunk_x,
                                 maximum_chunk_z);
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

    void forget_entity_texture(SDL_Texture *texture) noexcept
    {
        entity_srv_cache_.erase(texture);
        // A batch still held from an earlier frame must not name it either.
        std::erase_if(entity_batch_.spans,
                      [texture](const EntitySpan &span) { return span.texture == texture; });
    }

    void submit_dynamic_radiance(world::DynamicRadianceVolume volume)
    {
        dynamic_radiance_volume_ = std::move(volume);
    }

    // --- Enhanced presentation: the effects layer (vfx/vfx_renderer.hpp).
    [[nodiscard]] bool vfx_available() const noexcept
    {
#if defined(_WIN32)
        return vfx_ != nullptr && vfx_->ready() && post_processor_ != nullptr &&
               post_processor_->ready();
#else
        return false;
#endif
    }

    void submit_vfx(vfx::Frame &frame, const VfxOptions &options)
    {
#if defined(_WIN32)
        if (vfx_ != nullptr && vfx_->ready())
            vfx_->submit(frame, options);
#else
        (void)frame;
        (void)options;
#endif
    }

    // --- Mod graphics area (openminerz/modding/graphics.hpp). ---
    void submit_mod_lights(const modding::graphics::LightFrame &frame)
    {
#if defined(_WIN32)
        if (!available_)
            return;
        // Nothing is created until a mod has a light.
        if (mod_lights_ == nullptr)
        {
            if (frame.empty())
                return;
            mod_lights_ = std::make_unique<ModLightGpu>(device_.Get(), context_.Get());
        }
        mod_lights_->submit(frame);
#else
        (void)frame;
#endif
    }

    [[nodiscard]] modding::graphics::LightClusterStats mod_light_statistics()
    {
#if defined(_WIN32)
        if (mod_lights_ != nullptr)
            return mod_lights_->statistics();
#endif
        return {};
    }

    void set_mod_atmosphere(const modding::graphics::AtmosphereStack *stack) noexcept
    {
        mod_atmosphere_ = stack;
    }

    void set_mod_grade(const modding::graphics::Image *lut, std::string_view name, float strength)
    {
        mod_grade_lut_ = strength > 0.001F ? lut : nullptr;
        mod_grade_name_ = name;
        mod_grade_strength_ = strength;
    }

    // (mod-liquid)
    void set_liquid_view(bool inside, std::array<float, 3> color) noexcept
    {
        liquid_view_inside_ = inside;
        liquid_view_color_ = color;
    }

#if defined(_WIN32)
    // The lit pixel shaders of this draw: their mod-light variants while it
    // has mod lights, otherwise exactly the shaders compiled at load.
    [[nodiscard]] ID3D11PixelShader *lit_terrain_shader()
    {
        return mod_lights_active_
                   ? mod_lights_->variant(ModLightGpu::Variant::terrain, terrain_pixel_text_)
                   : pixel_shader_.Get();
    }
    [[nodiscard]] ID3D11PixelShader *lit_entity_shader()
    {
        return mod_lights_active_
                   ? mod_lights_->variant(ModLightGpu::Variant::entity, entity_pixel_text_)
                   : entity_pixel_shader_.Get();
    }
    [[nodiscard]] ID3D11PixelShader *lit_entity_alpha_shader()
    {
        return mod_lights_active_ ? mod_lights_->variant(ModLightGpu::Variant::entity_alpha_lit,
                                                         entity_alpha_lit_text_)
                                  : entity_alpha_lit_pixel_shader_.Get();
    }
#endif

    // A texture pack changed under a live world: the three atlases again, as
    // the constructor takes them (mod tiles are installed again after).
    void replace_atlases(SDL_Texture *atlas, SDL_Texture *normal_spec_atlas,
                         SDL_Texture *metal_light_atlas)
    {
#if defined(_WIN32)
        if (!available_ || atlas == nullptr || normal_spec_atlas == nullptr ||
            metal_light_atlas == nullptr)
            return;
        const auto resource_of = [](SDL_Texture *texture) {
            return static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
                SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        };
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlas_view;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal_spec_view;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metal_light_view;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlas_array;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal_spec_array;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metal_light_array;
        ID3D11Texture2D *atlas_resource = resource_of(atlas);
        ID3D11Texture2D *normal_spec_resource = resource_of(normal_spec_atlas);
        ID3D11Texture2D *metal_light_resource = resource_of(metal_light_atlas);
        if (atlas_resource == nullptr || normal_spec_resource == nullptr ||
            metal_light_resource == nullptr ||
            FAILED(device_->CreateShaderResourceView(atlas_resource, nullptr, &atlas_view)) ||
            FAILED(device_->CreateShaderResourceView(normal_spec_resource, nullptr,
                                                     &normal_spec_view)) ||
            FAILED(device_->CreateShaderResourceView(metal_light_resource, nullptr,
                                                     &metal_light_view)) ||
            !build_tile_array(atlas_resource, atlas_array) ||
            !build_tile_array(normal_spec_resource, normal_spec_array) ||
            !build_tile_array(metal_light_resource, metal_light_array))
        {
            std::cerr << "Texture pack: the terrain atlases could not be replaced\n";
            return;
        }
        atlas_view_ = atlas_view;
        normal_spec_view_ = normal_spec_view;
        metal_light_view_ = metal_light_view;
        atlas_array_view_ = atlas_array;
        normal_spec_array_view_ = normal_spec_array;
        metal_light_array_view_ = metal_light_array;
#else
        (void)atlas;
        (void)normal_spec_atlas;
        (void)metal_light_atlas;
#endif
    }

    // Rebuilds the three tile arrays with the mod slices after the retail 64:
    // the diffuse tiles as given, and a flat, matte material for each.
    void install_mod_tiles(const std::vector<modding::graphics::Image> &tiles)
    {
#if defined(_WIN32)
        if (!available_ || tiles.empty())
            return;
        const auto rebuild = [&](Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view,
                                 const std::function<std::array<std::uint8_t, 4>(
                                     std::size_t, int, int, int)> &texel) {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> old;
            if (FAILED(resource.As(&old)))
                return false;
            D3D11_TEXTURE2D_DESC description{};
            old->GetDesc(&description);
            const UINT retail = 64U;
            const UINT slices = retail + static_cast<UINT>(std::min<std::size_t>(tiles.size(), 192U));
            D3D11_TEXTURE2D_DESC grown = description;
            grown.ArraySize = slices;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> array;
            if (FAILED(device_->CreateTexture2D(&grown, nullptr, &array)))
                return false;
            for (UINT slice = 0U; slice < retail; ++slice)
                context_->CopySubresourceRegion(
                    array.Get(), D3D11CalcSubresource(0U, slice, grown.MipLevels), 0U, 0U, 0U,
                    old.Get(), D3D11CalcSubresource(0U, slice, description.MipLevels), nullptr);
            // BGRA or RGBA, whichever the pack's atlas was created as.
            const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              description.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
            const int side = static_cast<int>(description.Width);
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(side) * side * 4U);
            for (UINT slice = retail; slice < slices; ++slice)
            {
                const std::size_t tile = slice - retail;
                for (int y = 0; y < side; ++y)
                    for (int x = 0; x < side; ++x)
                    {
                        std::array<std::uint8_t, 4> value = texel(tile, x, y, side);
                        if (bgra)
                            std::swap(value[0], value[2]);
                        std::memcpy(&pixels[(static_cast<std::size_t>(y) * side + x) * 4U],
                                    value.data(), 4U);
                    }
                context_->UpdateSubresource(array.Get(),
                                            D3D11CalcSubresource(0U, slice, grown.MipLevels),
                                            nullptr, pixels.data(), static_cast<UINT>(side) * 4U,
                                            0U);
            }
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> grown_view;
            if (FAILED(device_->CreateShaderResourceView(array.Get(), nullptr, &grown_view)))
                return false;
            context_->GenerateMips(grown_view.Get());
            view = grown_view;
            tile_array_textures_.push_back(std::move(array));
            return true;
        };
        // Resampled once to the pack's tile size.
        D3D11_TEXTURE2D_DESC atlas_description{};
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            atlas_array_view_->GetResource(&resource);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource.As(&texture)))
                return;
            texture->GetDesc(&atlas_description);
        }
        const int side = static_cast<int>(atlas_description.Width);
        std::vector<modding::graphics::Image> sized;
        sized.reserve(tiles.size());
        for (const modding::graphics::Image &tile : tiles)
            sized.push_back(modding::graphics::resample(tile, side, side));
        const auto diffuse = [&](std::size_t tile, int x, int y, int) {
            const modding::graphics::Image &image = sized[tile];
            if (image.empty())
                return std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U};
            const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            return std::array<std::uint8_t, 4>{image.rgba[at], image.rgba[at + 1U],
                                               image.rgba[at + 2U], image.rgba[at + 3U]};
        };
        const auto flat = [](std::size_t, int, int, int) {
            return modding::graphics::flat_normal_spec;
        };
        const auto matte = [](std::size_t, int, int, int) {
            return modding::graphics::matte_metal_light;
        };
        if (!rebuild(atlas_array_view_, diffuse) || !rebuild(normal_spec_array_view_, flat) ||
            !rebuild(metal_light_array_view_, matte))
            std::cerr << "Mod block tiles could not be added to the terrain arrays\n";
#else
        (void)tiles;
#endif
    }

    // --- Enhanced presentation: the far landscape (far_terrain_renderer.hpp).
    void submit_far_terrain(world::FarTerrainFrame frame)
    {
#if defined(_WIN32)
        if (far_terrain_ != nullptr)
            far_terrain_->submit(std::move(frame));
#else
        (void)frame;
#endif
    }

    // mod-world
    void set_far_mod_palette(const world::ModPaletteLooks &looks,
                             std::span<const world::BlockType> fallbacks,
                             std::span<const world::FarSurfaceProfile> profiles)
    {
#if defined(_WIN32)
        if (far_terrain_ != nullptr)
            far_terrain_->set_mod_palette(looks, fallbacks, profiles);
#else
        (void)looks;
        (void)fallbacks;
        (void)profiles;
#endif
    }

    [[nodiscard]] FarTerrainGpuStats far_terrain_stats() const noexcept
    {
#if defined(_WIN32)
        if (far_terrain_ != nullptr)
            return far_terrain_->stats();
#endif
        return {};
    }

    // --- Enhanced presentation: sky and post-processing (post_processing.hpp).
    void set_post_effects(const PostEffects &effects) noexcept
    {
        post_effects_ = effects;
        apply_post_effect_overrides(post_effects_);
    }

    void set_post_view(const PostView &view) noexcept
    {
        post_view_ = view;
    }

    void set_sky_plates(const std::array<SDL_Texture *, 6> &first,
                        const std::array<SDL_Texture *, 6> &second, float second_weight) noexcept
    {
        sky_plate_first_ = first;
        sky_plate_second_ = second;
        sky_plate_second_weight_ = std::clamp(second_weight, 0.0F, 1.0F);
    }

    [[nodiscard]] bool draws_sky() const noexcept
    {
        return drew_sky_;
    }

    [[nodiscard]] bool deep_light_model() const noexcept
    {
#if defined(_WIN32)
        return hell_frame_.model;
#else
        return false;
#endif
    }

    [[nodiscard]] float lava_light_at(float x, float y, float z, float normal_y) const noexcept
    {
#if defined(_WIN32)
        return hell_ != nullptr && hell_frame_.model
                   ? hell_->lava_light_at(x, y, z, normal_y) * hell_->pulse()
                   : 0.0F;
#else
        (void)x;
        (void)y;
        (void)z;
        (void)normal_y;
        return 0.0F;
#endif
    }

    void draw_lens(float elapsed_seconds)
    {
#if defined(_WIN32)
        if (!drew_sky_ || post_processor_ == nullptr || !post_processor_->lens_needed())
            return;
        // Everything SDL has queued (the world image, the first-person body)
        // must be on the target before it is read back and graded in place.
        if (!SDL_FlushRenderer(renderer_))
            return;
        const D3DStateBackup previous_state{context_.Get()};
        post_processor_->lens(previous_state.render_target.Get(), elapsed_seconds);
        previous_state.restore(context_.Get());
        (void)SDL_FlushRenderer(renderer_);
#else
        (void)elapsed_seconds;
#endif
    }

    // (Visuals) A mod's colour grade over the finished world AND the
    // first-person body composited on it, in place on the target, both
    // presentations; nothing without a LUT.
    void draw_mod_grade()
    {
#if defined(_WIN32)
        if (!available_ || mod_grade_lut_ == nullptr)
            return;
        if (!SDL_FlushRenderer(renderer_))
            return;
        const D3DStateBackup previous_state{context_.Get()};
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
        if (previous_state.render_target != nullptr)
            previous_state.render_target->GetResource(&resource);
        if (resource != nullptr && SUCCEEDED(resource.As(&target)))
        {
            if (mod_grade_pass_ == nullptr)
                mod_grade_pass_ = std::make_unique<ModScreenPass>(renderer_);
            mod_grade_pass_->grade(target.Get(), mod_grade_lut_, mod_grade_name_,
                                   mod_grade_strength_);
        }
        previous_state.restore(context_.Get());
        (void)SDL_FlushRenderer(renderer_);
#endif
    }

    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept
    {
        tone_curve_ = std::clamp(tone_curve, 0, 3);
        auto_exposure_enabled_ = auto_exposure;
        exposure_compensation_ = std::clamp(exposure_compensation, -2.0F, 2.0F);
        color_grading_ = std::clamp(color_grading, 0.0F, 1.5F);
    }

    // Values are stored here and turned into GPU resources at the start of
    // the next draw, where the device context is known to be usable.
    void set_quality(int shadow_resolution, float shadow_distance, int anisotropy, bool fxaa,
                     bool bloom, bool sun_shafts) noexcept
    {
        requested_shadow_resolution_ =
            static_cast<unsigned>(std::clamp(shadow_resolution, 512, 4096));
        shadow_distance_ = std::clamp(shadow_distance, 32.0F, 150.0F);
        requested_anisotropy_ = static_cast<unsigned>(std::clamp(anisotropy, 1, 16));
        fxaa_enabled_ = fxaa;
        bloom_enabled_ = bloom;
        sun_shafts_enabled_ = sun_shafts;
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
                            [[maybe_unused]] world::BiomeKind biome, bool water_enabled,
                            bool enhanced_foliage, bool ambient_occlusion, bool ore_glow,
                            float shadow_strength)
    {
#if defined(_WIN32)
        if (!available_ || (vertex_buffer_ == nullptr && chunk_meshes_.empty()))
        {
            return false;
        }
        elapsed_seconds = diagnostic_render_clock(elapsed_seconds);
        rebuild_crater_masks_if_needed();
        apply_quality_resources();
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
        // The Enhanced presentation's own sky and post-processing chain. It
        // needs the half-float scene and every one of its targets; if anything
        // is missing the frame is drawn exactly as before it existed. The
        // Original presentation never takes this path.
        bool enhanced_post = false;
        if (enhanced_foliage && post_effects_.enabled && post_ready_ && hdr_scene_ &&
            post_resource_ != nullptr)
        {
            D3D11_TEXTURE2D_DESC post_description{};
            post_resource_->GetDesc(&post_description);
            if (post_processor_ != nullptr)
                post_processor_->set_effects(post_effects_);
            enhanced_post = post_processor_ != nullptr && post_processor_->ready() &&
                            post_processor_->resize(target_width, target_height,
                                                    post_description.Format);
        }
        drew_sky_ = enhanced_post;
        const D3DStateBackup previous_state{context_.Get()};
        const bool dynamic_radiance_ready = upload_dynamic_radiance();
        constexpr float pi = 3.14159265358979323846F;
        // The frame's own world angle: the player's "Field of view" setting,
        // already narrowed by any shoulder zoom (openminerz/view_projection.hpp).
        // Vertical, Hor+: x follows the target's aspect ratio. The sky, the
        // overlay camera and every other world pass sanitise the same way, so
        // they all agree; nothing here may clamp tighter (an old 50-degree
        // floor used to cut every zoom above 1.5x short).
        const float vertical_fov =
            view::sanitized_fov_degrees(vertical_fov_degrees) * pi / 180.0F;
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
        // The Enhanced presentation's deep underground (hell_atmosphere.hpp):
        // follow the eye with the lava map and fix this frame's constants. For
        // the Original presentation, and without the post-processing chain,
        // every one of them is zero and nothing below changes.
        hell_frame_ = {};
        if (hell_ != nullptr)
        {
            HellFrameInput hell_input;
            hell_input.enhanced =
                enhanced_post && hell_atmosphere_enabled && deep_underground_terrain_ &&
                deep_underground_creatures_;
            hell_input.eye = {player.position.x,
                              player.position.y + world::PlayerSimulation::eye_height,
                              player.position.z};
            hell_input.eye_presence = sky.hell;
            hell_input.eye_sky_light = post_view_.eye_sky_light;
            hell_input.daylight = sky.daylight;
            hell_input.elapsed_seconds = elapsed_seconds;
            hell_input.width = target_width;
            hell_input.height = target_height;
            hell_input.effects = post_effects_;
            hell_frame_ = hell_->begin_frame(hell_input);
        }
        // w selects the older flat red fill of the lit shaders; the model above
        // replaces it whenever it runs.
        constants.eye = {player.position.x, player.position.y + world::PlayerSimulation::eye_height,
                         player.position.z,
                         hell_atmosphere_enabled && !hell_frame_.model ? 1.0F : 0.0F};
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
            biome_atmosphere_enabled
                ? world::biome_atmosphere_at(player.position.x, player.position.z)
                : world::BiomeAtmosphere{};
        const float fog_end = std::clamp(visibility_distance - 10.0F, 24.0F, 118.0F);
        // Fog is a band near the draw limit, not an atmosphere-wide soup: the
        // original's distance haze only bites in the last stretch before the
        // chunk edge (the desert used to drown in orange from 24 blocks out).
        float fog_start =
            std::max(biome_atmosphere_enabled ? 18.0F : 24.0F,
                     fog_end * std::lerp(biome_atmosphere.fog_start_ratio, 0.72F, sky.hell));
        // --- The Enhanced atmosphere and far terrain (far_terrain_renderer.hpp,
        // atmosphere_shader.hpp). Above ground the air is one function applied
        // to every pixel after this pass, so the fade below is no longer fog:
        //   * with far terrain drawn it is where the detailed window hands
        //     over to the far landscape standing in the same place, and it
        //     starts as late as the window allows;
        //   * with none it slides out of reach, and the atmosphere's own
        //     density buries the window's edge.
        // Underground there is no sky's air and it is the cave fog it always
        // was: in full where no skylight reaches the eye, and in Hell.
        // (mod-liquid) Inside a liquid there is no air and no far landscape:
        // the liquid's own fog is the distance (and fills the background).
        const bool atmosphere_active =
            enhanced_post && far_terrain_ != nullptr && far_terrain_->ready() && !liquid_view_inside_;
        const bool far_terrain_active = atmosphere_active && far_terrain_->active();
        float fade_end = fog_end;
        if (far_terrain_active)
            fog_start = std::lerp(world::far_handover(visibility_distance).start, fog_start,
                                  std::clamp(sky.hell, 0.0F, 1.0F));
        else if (atmosphere_active)
        {
            const float open_air = std::clamp(post_view_.eye_sky_light, 0.0F, 1.0F) *
                                   (1.0F - std::clamp(sky.hell, 0.0F, 1.0F));
            fog_start += open_air * 4096.0F;
            fade_end += open_air * 4096.0F;
        }
        // Mod atmosphere layers (graphics area) blend their fog band over the
        // one above; without a layer that sets one nothing changes.
        if (mod_atmosphere_ != nullptr)
            (void)mod_atmosphere_->fog_band(fog_start, fade_end);
        // (mod-liquid) Inside a liquid the distance fogs into its colour
        // within a few blocks (the colour itself is set below).
        if (liquid_view_inside_)
        {
            fog_start = 0.0F;
            fade_end = 16.0F;
        }
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
            // The deep underground's model lights the distance with the
            // lava's glow in the air; what lies beyond is the cavern's dark.
            if (hell_frame_.model)
            {
                const float deep = std::clamp(sky.hell, 0.0F, 1.0F);
                presentation_fog = {
                    std::lerp(presentation_fog.red, world::hell_far_haze.red, deep),
                    std::lerp(presentation_fog.green, world::hell_far_haze.green, deep),
                    std::lerp(presentation_fog.blue, world::hell_far_haze.blue, deep)};
            }
        }
        if (liquid_view_inside_)
        {
            // (mod-liquid) The liquid's own colour, deeper and darker than its
            // texture, dimmed at night like the rest of the world.
            const float shade = 0.55F * (0.2F + 0.8F * sky.daylight);
            presentation_fog = {liquid_view_color_[0] * shade, liquid_view_color_[1] * shade,
                                liquid_view_color_[2] * shade};
            if (enhanced_foliage)
                presentation_fog = {presentation_fog.red * presentation_fog.red,
                                    presentation_fog.green * presentation_fog.green,
                                    presentation_fog.blue * presentation_fog.blue};
        }
        constants.liquid_view = {liquid_view_inside_ ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};
        constants.fog_and_end = {presentation_fog.red, presentation_fog.green,
                                 presentation_fog.blue, fade_end};
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
            sun_shadows_disabled
                ? 0.0F
                : quality_shadow_strength * solar_shadow_visibility * (1.0F - sky.hell);
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
        // Both follow the Shadows setting: the map size decides how sharp an
        // edge is, the distance how many chunks every cascade has to draw.
        const float shadow_resolution = static_cast<float>(shadow_resolution_);
        const float shadow_far =
            std::min(shadow_distance_, std::clamp(visibility_distance + 18.0F, 48.0F, 150.0F));
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
                const auto slice = world::shadow_slice(
                    cascade == 0 ? near_plane : shadow_starts[cascade - 1], shadow_starts[cascade],
                    shadow_splits[cascade], tangent_x, tangent_y);
                const float slice_near = slice.near_depth;
                const float slice_far = shadow_splits[cascade];
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
                            const float d = -(wx * dx + wy * dy + wz * dz);
                            minimum_d = std::min(minimum_d, d);
                            maximum_d = std::max(maximum_d, d);
                        }
                }

                // A fixed sphere radius, unlike a light-space bounding square,
                // does not change texel size when the camera rotates.
                const float padding = std::array{2.0F, 5.0F, 11.0F}[cascade];
                const float extent = std::max(slice.radius, 4.0F) + padding;
                const float texel_world = (2.0F * extent) / shadow_resolution;
                const float center_x = constants.eye.x + constants.forward.x * slice.center_depth;
                const float center_y = constants.eye.y + constants.forward.y * slice.center_depth;
                const float center_z = constants.eye.z + constants.forward.z * slice.center_depth;
                float center_u = center_x * rx + center_y * ry + center_z * rz;
                float center_v = center_x * ux + center_y * uy + center_z * uz;
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
                shadow_draw_radii[cascade] =
                    slice.center_depth + slice.radius + std::array{22.0F, 38.0F, 64.0F}[cascade];
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
            constants.post_sun = {
                0.5F + 0.5F * (cx / safe_z) * constants.projection.x,
                0.5F - 0.5F * (cy / safe_z) * constants.projection.y,
                enhanced_foliage && (sky.sun_y <= -0.005F || sky.hell > 0.99F) ? -1.0F : cz,
                sky.daylight};
        }
        constants.post_params = {bloom_enabled_ ? 0.85F : 0.0F, sun_shafts_enabled_ ? 0.42F : 0.0F,
                                 static_cast<float>(tone_curve_), elapsed_seconds};
        constants.target_size = {
            static_cast<float>(target_width), static_cast<float>(target_height),
            1.0F / static_cast<float>(target_width), 1.0F / static_cast<float>(target_height)};
        // The legacy curve keeps the old un-exposed look so the comparison is
        // honest; the real curves expect scene light around middle grey.
        const float base_exposure = tone_curve_ == 0 ? 1.0F : 1.15F;
        // z scales the bloom's quadratic weight rather than thresholding it.
        // w is zero for manual exposure and otherwise how far auto exposure
        // may open: exactly one, except deep in the Enhanced presentation's
        // Hell (world::hell_exposure_limit), which only its composite reads.
        constants.grade_params = {base_exposure * std::pow(2.0F, exposure_compensation_),
                                  color_grading_, 0.62F,
                                  exposure_ready_ && auto_exposure_enabled_
                                      ? hell_frame_.exposure_limit
                                      : 0.0F};
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
            biome_atmosphere_enabled
                ? biome_atmosphere.grade_strength * atmosphere_presence * (1.0F - sky.hell)
                : 0.0F};
        // z is the Enhanced sun-shadow penumbra scale (0.5 is the width this
        // renderer always drew) and w switches the Enhanced torch flicker; the
        // Original presentation receives the neutral pair and ignores both.
        constants.biome_params = {biome_atmosphere_enabled ? biome_atmosphere.saturation : 1.0F,
                                  ore_glow ? 1.0F : 0.0F,
                                  enhanced_post ? post_effects_.shadow_softness : 0.5F,
                                  enhanced_post && !post_effects_.reduced_motion ? 1.0F : 0.0F};
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
        // Mod graphics area: mod lights make the lit terrain shader dearer per
        // pixel, so a frame with them draws the sections nearest first and the
        // depth test rejects what is hidden before it is shaded. Opaque faces
        // give the same image in any order. (Polish) Enhanced uses it on every
        // frame (see below); Original frames without mod lights keep the
        // order above.
        const auto draw_world_geometry_nearest_first = [&](float draw_radius) {
            thread_local std::vector<std::pair<float, const GpuChunkMesh *>> order;
            order.clear();
            const float draw_radius_squared = draw_radius * draw_radius;
            for (const auto &[coordinate, mesh] : chunk_meshes_)
            {
                const float dx =
                    (static_cast<float>(std::get<0>(coordinate)) + 0.5F) * world::chunk_width -
                    player.position.x;
                const float dz =
                    (static_cast<float>(std::get<1>(coordinate)) + 0.5F) * world::chunk_depth -
                    player.position.z;
                if (dx * dx + dz * dz > draw_radius_squared)
                    continue;
                const float dy = static_cast<float>(world::world_min_y) +
                                 (static_cast<float>(std::get<2>(coordinate)) + 0.5F) *
                                     world::chunk_section_height -
                                 constants.eye.y;
                order.emplace_back(dx * dx + dy * dy + dz * dz, &mesh);
            }
            std::sort(order.begin(), order.end(),
                      [](const auto &a, const auto &b) { return a.first < b.first; });
            for (const auto &[distance, mesh] : order)
            {
                (void)distance;
                ID3D11Buffer *chunk_buffer = mesh->vertex_buffer.Get();
                context_->IASetVertexBuffers(0U, 1U, &chunk_buffer, &stride, &offset);
                context_->Draw(mesh->vertex_count, 0U);
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
        // The deep underground's constants, lava map and noise, on registers
        // of their own for the whole frame (all zero outside the Enhanced
        // presentation, where no shader branch then reads them).
        if (hell_ != nullptr)
            hell_->bind();
        // The lights of this frame's effects, likewise on a register of their
        // own (none outside the Enhanced presentation's chain, where the lit
        // shaders' loop over them then runs zero times).
        if (vfx_ != nullptr)
            vfx_->begin_frame(enhanced_post);
        // Lighting props placed by mods (graphics area), likewise on registers
        // of their own, in both presentations. The lit passes switch to the
        // shaders' mod-light variants only for a frame that has such lights.
        // Their cluster lists are built here, for this frame's camera.
        mod_lights_active_ = false;
        if (mod_lights_ != nullptr &&
            mod_lights_->begin_frame(
                {{constants.eye.x, constants.eye.y, constants.eye.z},
                 {constants.right.x, constants.right.y, constants.right.z},
                 {constants.up.x, constants.up.y, constants.up.z},
                 {constants.forward.x, constants.forward.y, constants.forward.z},
                 constants.projection.x,
                 constants.projection.y,
                 target_width,
                 target_height}))
        {
            mod_lights_active_ =
                mod_lights_->variant(ModLightGpu::Variant::terrain, terrain_pixel_text_) !=
                    nullptr &&
                mod_lights_->variant(ModLightGpu::Variant::entity, entity_pixel_text_) != nullptr &&
                mod_lights_->variant(ModLightGpu::Variant::entity_alpha_lit,
                                     entity_alpha_lit_text_) != nullptr;
            if (!mod_lights_active_)
                mod_lights_->unbind();
        }
        // Far terrain's shadow field for the lit pass, likewise on registers of
        // its own (far_terrain_renderer.hpp). Zero constants, which switch the
        // shader's term off, unless far terrain is drawn this frame.
        if (far_terrain_ != nullptr)
            far_terrain_->bind_near_shadow(
                {static_cast<double>(constants.eye.x), static_cast<double>(constants.eye.y),
                 static_cast<double>(constants.eye.z)},
                elapsed_seconds,
                far_terrain_active && constants.shadow_params.x > 0.0F
                    ? std::lerp(0.70F, 0.94F, std::clamp(constants.shadow_params.x, 0.0F, 1.0F)) *
                          std::clamp(constants.shadow_params.z, 0.0F, 1.0F) * (1.0F - sky.hell)
                    : 0.0F);

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
                const D3D11_VIEWPORT shadow_viewport{0.0F, 0.0F, shadow_resolution,
                                                     shadow_resolution, 0.0F, 1.0F};
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

        // Enhanced only: everything the sky and post-processing chain is told
        // about this frame, fixed before anything is drawn so the entity pass
        // can read last frame's camera from the same constants.
        PostFrame post_frame{};
        if (enhanced_post)
        {
            post_frame.width = target_width;
            post_frame.height = target_height;
            post_frame.scene_resource = target_resource_.Get();
            post_frame.scene_target = target_view_.Get();
            post_frame.scene_view = target_srv_.Get();
            post_frame.depth_readonly = depth_view_readonly_.Get();
            post_frame.depth_view = depth_srv_.Get();
            post_frame.scene_constants = scene_buffer_.Get();
            post_frame.linear_sampler = sampler_state_.Get();
            post_frame.shadow_sampler = shadow_sampler_.Get();
            post_frame.shadow_view = sun_shadows_ready ? shadow_srv_.Get() : nullptr;
            post_frame.shadow_transmission_view =
                sun_shadows_ready && canopy_shadow_transmission_ready
                    ? shadow_transmission_srv_.Get()
                    : nullptr;
            post_frame.output_target = post_rtv_.Get();
            post_frame.eye = {constants.eye.x, constants.eye.y, constants.eye.z};
            post_frame.right = {constants.right.x, constants.right.y, constants.right.z};
            post_frame.up = {constants.up.x, constants.up.y, constants.up.z};
            post_frame.forward = {constants.forward.x, constants.forward.y, constants.forward.z};
            post_frame.projection = {constants.projection.x, constants.projection.y,
                                     constants.projection.z, constants.projection.w};
            post_frame.vertical_fov_degrees = vertical_fov * 180.0F / pi;
            post_frame.elapsed_seconds = elapsed_seconds;
            post_frame.delta_seconds =
                post_last_seconds_ < 0.0F
                    ? 1.0F / 60.0F
                    : std::clamp(elapsed_seconds - post_last_seconds_, 0.0F, 0.25F);
            post_last_seconds_ = elapsed_seconds;
            post_frame.sky = sky;
            post_frame.view = post_view_;
            post_frame.shadow_far = shadow_far;
            post_frame.fog_end = fog_end;
            // (Pass 2b lays the one atmosphere over the scene; the resolve then
            // has no fog of its own to re-aim, and the light-shaft pass only
            // takes shadowed sunlight out of that atmosphere.)
            // (mod-liquid) Inside a liquid its fog stays its fog: nothing is
            // re-aimed at a sky the liquid hides.
            post_frame.atmosphere = atmosphere_active || liquid_view_inside_;
            post_frame.hell = hell_frame_.model ? hell_.get() : nullptr;
            post_frame.deep_grade = hell_frame_.deep_grade;
            post_frame.vfx = vfx_ != nullptr && vfx_->ready() ? vfx_.get() : nullptr;
            std::array<ID3D11ShaderResourceView *, 12> plates{};
            bool plates_bound = true;
            for (std::size_t face = 0U; face < 6U; ++face)
            {
                plates[face] = entity_view(sky_plate_first_[face]);
                plates[face + 6U] = entity_view(
                    sky_plate_second_[face] != nullptr ? sky_plate_second_[face]
                                                       : sky_plate_first_[face]);
                plates_bound = plates_bound && plates[face] != nullptr;
            }
            post_processor_->set_sky_plates(plates, sky_plate_second_weight_, plates_bound);
            post_processor_->begin_frame(post_frame);
        }

        // 2) Main lit pass into the world colour target.
        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        // (mod-liquid) Inside a liquid, what nothing covers is its fog, not sky.
        const std::array<float, 4> liquid_background{constants.fog_and_end.x, constants.fog_and_end.y,
                                                      constants.fog_and_end.z, 1.0F};
        context_->ClearRenderTargetView(target_view_.Get(), liquid_view_inside_
                                                                ? liquid_background.data()
                                                                : transparent.data());
        context_->ClearDepthStencilView(depth_view_.Get(), D3D11_CLEAR_DEPTH, 1.0F, 0U);
        ID3D11RenderTargetView *render_target = target_view_.Get();
        // Enhanced only: terrain and opaque entities also write what the
        // post-processing chain needs to know about each surface (normal,
        // baked occlusion, light shares, skylight). Blended passes below bind
        // the colour target alone and leave that data untouched.
        // The fourth target exists only while motion blur is on: entities
        // write how far each of their pixels moved on screen. Terrain does not
        // move, so its blend state masks that target and it keeps "use the
        // camera's motion".
        const std::array<ID3D11RenderTargetView *, 4> scene_targets{
            render_target, enhanced_post ? post_processor_->surface_target(0U) : nullptr,
            enhanced_post ? post_processor_->surface_target(1U) : nullptr,
            enhanced_post ? post_processor_->surface_target(2U) : nullptr};
        const bool motion_targets =
            scene_targets[3] != nullptr && terrain_motion_blend_state_ != nullptr;
        if (enhanced_post)
            post_processor_->clear_surface_targets();
        context_->OMSetRenderTargets(motion_targets ? 4U : (enhanced_post ? 3U : 1U),
                                     scene_targets.data(), depth_view_.Get());
        context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
        context_->OMSetBlendState(motion_targets ? terrain_motion_blend_state_.Get()
                                                 : blend_state_.Get(),
                                  nullptr, 0xFFFFFFFFU);
        entity_motion_vectors_ = motion_targets;
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(target_width), static_cast<float>(target_height),
            0.0F, 1.0F};
        context_->RSSetViewports(1U, &viewport);
        context_->RSSetState(rasterizer_state_.Get());
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(lit_terrain_shader(), nullptr, 0U);
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
        // (Polish) Enhanced draws the sections nearest first on every frame:
        // early depth rejects more of its dearer shading behind near walls
        // (5120x1440 Ultra: a cave 5.22 -> 4.66 ms, a walk 7.58 -> 6.50 ms
        // median; 1080p Default: no change), and the image is the same (0
        // differing pixels in day, dusk, cave, underwater, foliage and far
        // views). Original keeps the map order except on frames with mod
        // lights. OPENMINERZ_DIAGNOSTIC_TERRAIN_ORDER=map|near forces one
        // order, for such comparisons.
        static const int terrain_order_override = [] {
            const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_TERRAIN_ORDER");
            if (value == nullptr)
                return 0;
            return std::string_view{value} == "near" ? 1 : std::string_view{value} == "map" ? -1 : 0;
        }();
        const bool nearest_first =
            !chunk_meshes_.empty() &&
            (terrain_order_override != 0 ? terrain_order_override > 0 : mod_lights_active_ || enhanced_foliage);
        if (nearest_first)
            draw_world_geometry_nearest_first(visibility_distance + 24.0F);
        else
            draw_world_geometry(visibility_distance + 24.0F);

        if (entity_buffer_ready)
        {
            // Entities write every target, their own motion included.
            if (motion_targets)
                context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
            draw_entity_batch();
        }

        // (mod-liquid) Liquid faces over everything opaque, farthest section
        // first, blended and depth-tested without writing depth; they read
        // the scene's depth to see how much liquid lies before it.
        {
            thread_local std::vector<std::pair<float, const GpuChunkMesh *>> liquid_order;
            liquid_order.clear();
            const float liquid_radius = visibility_distance + 24.0F;
            for (const auto &[coordinate, mesh] : chunk_meshes_)
            {
                if (mesh.liquid_count == 0U)
                    continue;
                const float dx = (static_cast<float>(std::get<0>(coordinate)) + 0.5F) * world::chunk_width -
                                 constants.eye.x;
                const float dz = (static_cast<float>(std::get<1>(coordinate)) + 0.5F) * world::chunk_depth -
                                 constants.eye.z;
                if (dx * dx + dz * dz > liquid_radius * liquid_radius)
                    continue;
                const float dy = static_cast<float>(world::world_min_y) +
                                 (static_cast<float>(std::get<2>(coordinate)) + 0.5F) *
                                     world::chunk_section_height -
                                 constants.eye.y;
                liquid_order.emplace_back(dx * dx + dy * dy + dz * dz, &mesh);
            }
            if (!liquid_order.empty())
            {
                std::sort(liquid_order.begin(), liquid_order.end(),
                          [](const auto &a, const auto &b) { return a.first > b.first; });
                context_->OMSetRenderTargets(1U, &render_target, depth_view_readonly_.Get());
                context_->OMSetDepthStencilState(water_depth_state_.Get(), 0U);
                context_->OMSetBlendState(water_blend_state_.Get(), nullptr, 0xFFFFFFFFU);
                context_->IASetInputLayout(input_layout_.Get());
                context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
                context_->PSSetShader(lit_terrain_shader(), nullptr, 0U);
                ID3D11ShaderResourceView *scene_depth = depth_srv_.Get();
                context_->PSSetShaderResources(5U, 1U, &scene_depth);
                for (const auto &[distance, mesh] : liquid_order)
                {
                    (void)distance;
                    ID3D11Buffer *chunk_buffer = mesh->vertex_buffer.Get();
                    context_->IASetVertexBuffers(0U, 1U, &chunk_buffer, &stride, &offset);
                    context_->Draw(mesh->liquid_count, mesh->vertex_count);
                }
                ID3D11ShaderResourceView *no_depth{};
                context_->PSSetShaderResources(5U, 1U, &no_depth);
                // Then the surface's depth (and, Enhanced, what the
                // post-processing chain reads of a surface) without its
                // colour, so what follows - the air, the far landscape and
                // the hand-over between them, screen-space occlusion - reads
                // the liquid where it is, not the bed seen through it.
                const std::array<ID3D11RenderTargetView *, 3> surface_targets{
                    nullptr, enhanced_post ? post_processor_->surface_target(0U) : nullptr,
                    enhanced_post ? post_processor_->surface_target(1U) : nullptr};
                context_->OMSetRenderTargets(enhanced_post ? 3U : 0U,
                                             enhanced_post ? surface_targets.data() : nullptr,
                                             depth_view_.Get());
                context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
                context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
                if (!enhanced_post)
                    context_->PSSetShader(nullptr, nullptr, 0U);
                for (const auto &[distance, mesh] : liquid_order)
                {
                    (void)distance;
                    ID3D11Buffer *chunk_buffer = mesh->vertex_buffer.Get();
                    context_->IASetVertexBuffers(0U, 1U, &chunk_buffer, &stride, &offset);
                    context_->Draw(mesh->liquid_count, mesh->vertex_count);
                }
                context_->PSSetShader(lit_terrain_shader(), nullptr, 0U);
                context_->OMSetRenderTargets(1U, &render_target, depth_view_.Get());
                context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
            }
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

        // 2b) Enhanced only: the atmosphere over everything the scene holds,
        // from each pixel's own depth, and the far landscape (when far terrain
        // is on) with the open sky's air UNDER it where it holds no depth
        // (far_terrain_renderer.hpp, atmosphere_shader.hpp).
        if (atmosphere_active)
        {
            FarTerrainPass far_pass;
            far_pass.width = target_width;
            far_pass.height = target_height;
            far_pass.scene_target = target_view_.Get();
            far_pass.depth_view = depth_srv_.Get();
            far_pass.surface_view = post_processor_->surface_view(1U);
            far_pass.atlas_view = atlas_array_view_.Get();
            far_pass.scene_constants = scene_buffer_.Get();
            far_pass.eye = {static_cast<double>(constants.eye.x),
                            static_cast<double>(constants.eye.y),
                            static_cast<double>(constants.eye.z)};
            far_pass.sky = sky;
            far_pass.storminess = post_view_.storminess;
            far_pass.eye_sky_light = post_view_.eye_sky_light;
            far_pass.elapsed_seconds = elapsed_seconds;
            far_pass.visibility = visibility_distance;
            far_pass.shadow_opacity =
                constants.shadow_params.x > 0.0F
                    ? std::lerp(0.70F, 0.94F, std::clamp(constants.shadow_params.x, 0.0F, 1.0F)) *
                          std::clamp(constants.shadow_params.z, 0.0F, 1.0F) * (1.0F - sky.hell)
                    : 0.0F;
            far_terrain_->draw(far_pass);
        }

        // 2c) Enhanced only: the sky, composed UNDER everything the scene
        // already holds (and so behind the atmosphere's share of each pixel),
        // which makes the frame opaque from here on.
        // (mod-liquid) Not from inside a liquid: its fog is the background.
        if (enhanced_post && !liquid_view_inside_)
            post_processor_->draw_sky(post_frame);

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
                        const float maximum_exposure =
                            enhanced_foliage ? hell_frame_.exposure_limit : 1.414F;
                        readback_exposure_ = std::clamp(0.20F / std::max(average, 0.0008F), 0.354F,
                                                        maximum_exposure);
                    }
                }
            }
            last_frame_seconds_ = elapsed_seconds;
            ID3D11ShaderResourceView *adapted_view =
                auto_exposure ? adapt_srv_[adapt_index_].Get() : nullptr;

            if (enhanced_post)
            {
                // 3b) Enhanced: occlusion, volumetric light, motion blur, depth
                // of field, bloom, the display transform, edge smoothing and
                // sharpening (post_processing.hpp). The first-person body and
                // held item are composited after this returns, so none of it
                // can touch them, and the 2D interface is drawn after that.
                post_frame.adapted_view = adapted_view;
                post_processor_->run(post_frame);
            }
            else
            {
            // 3c) The original chain, unchanged (and deliberately left at its
            // indentation): the Original presentation always runs this.
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
            // Bloom off (a player setting) skips the bright pass and both blurs.
            if (bloom_enabled_)
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
            if (bloom_enabled_)
            {
                blur_pass(bloom_b_rtv_.Get(), bloom_a_srv_.Get(), 1.0F, 0.0F);
                blur_pass(bloom_a_rtv_.Get(), bloom_b_srv_.Get(), 0.0F, 1.0F);
            }
            else
            {
                // The bright-pass state set above must not leak into the composite.
                ID3D11ShaderResourceView *cleared = nullptr;
                context_->PSSetShaderResources(0U, 1U, &cleared);
                context_->OMSetRenderTargets(0U, nullptr, nullptr);
            }

            context_->RSSetViewports(1U, &viewport);
            ID3D11RenderTargetView *post_target = post_rtv_.Get();
            context_->OMSetRenderTargets(1U, &post_target, nullptr);
            context_->PSSetShader(composite_shader(), nullptr, 0U);
            // An unbound texture samples as zero, so a stale bloom image from
            // before the option was switched off can never reach the screen.
            ID3D11ShaderResourceView *bloom_view = bloom_enabled_ ? bloom_a_srv_.Get() : nullptr;
            const std::array composite_views{target_srv_.Get(), bloom_view, adapted_view};
            context_->PSSetShaderResources(0U, static_cast<UINT>(composite_views.size()),
                                           composite_views.data());
            context_->Draw(3U, 0U);
            context_->PSSetShaderResources(0U, static_cast<UINT>(empty_views.size()),
                                           empty_views.data());
            context_->OMSetRenderTargets(0U, nullptr, nullptr);
            }
            context_->CopyResource(presentation_resource_.Get(), post_resource_.Get());
        }
        else
        {
            context_->CopyResource(presentation_resource_.Get(), target_resource_.Get());
        }
        // (Graphics area) a mod's colour grade is drawn later, by
        // draw_mod_grade, over this image and the first-person body together.
        retire_entity_batch();
        if (hell_ != nullptr)
            hell_->unbind();
        if (vfx_ != nullptr)
            vfx_->unbind();
        if (mod_lights_ != nullptr)
            mod_lights_->unbind();
        if (far_terrain_ != nullptr)
            far_terrain_->unbind_near_shadow();
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
    struct GpuChunkMesh final
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
        UINT vertex_count{};
        // (mod-liquid) Translucent liquid faces after the opaque ones.
        UINT liquid_count{};
    };

    void track_crash_site_faces(const std::tuple<int, int, int> &key,
                                const std::vector<world::WorldFace> &faces)
    {
        track_crash_site_columns(key, scan_crash_site_columns(faces));
    }

    // The single-mesh path hands over every face at once: file its lava under
    // the chunk each column stands in, as the chunked path does.
    void track_lava_faces(const std::vector<world::WorldFace> &faces)
    {
        if (hell_ == nullptr)
            return;
        world::LavaField &lava = hell_->lava();
        lava.clear();
        std::map<std::tuple<int, int, int>, std::vector<world::LavaColumn>> chunks;
        const auto chunk_of = [](int block) {
            return block >= 0 ? block / world::lava_field_chunk
                              : -((-block + world::lava_field_chunk - 1) / world::lava_field_chunk);
        };
        for (const world::LavaColumn &column : scan_lava_columns(faces))
            chunks[std::tuple{chunk_of(column.x), chunk_of(column.z), 0}].push_back(column);
        for (auto &[key, columns] : chunks)
            lava.set_section(key, std::move(columns));
    }

    void track_crash_site_columns(const std::tuple<int, int, int> &key,
                                  std::vector<std::array<int, 2>> columns)
    {
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
        // (mod-liquid) x = 1 while the camera is inside a mod liquid. Last,
        // so every shader that declares less of this buffer reads it as before.
        Float4 liquid_view;
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

    // Bring the GPU objects in line with set_quality(). A request the driver
    // refuses is dropped and the previous object stays in use, so a failed
    // 4096-square shadow allocation falls back instead of losing shadows.
    void apply_quality_resources()
    {
        // "Texture detail" (Enhanced only): crisper is a negative mip bias.
        // The Original presentation resolves to zero, the sampler it always had.
        const float requested_mip_bias =
            post_effects_.enabled ? -0.75F * std::clamp(post_effects_.texture_detail, -1.0F, 1.0F)
                                  : 0.0F;
        if (requested_anisotropy_ != anisotropy_ || requested_mip_bias != mip_bias_)
        {
            D3D11_SAMPLER_DESC description{};
            description.Filter = requested_anisotropy_ > 1U ? D3D11_FILTER_ANISOTROPIC
                                                            : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            description.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            description.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            description.MaxAnisotropy = requested_anisotropy_;
            description.MipLODBias = requested_mip_bias;
            description.MinLOD = 0.0F;
            description.MaxLOD = D3D11_FLOAT32_MAX;
            Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
            if (SUCCEEDED(device_->CreateSamplerState(&description, &sampler)))
                aniso_sampler_state_ = std::move(sampler);
            anisotropy_ = requested_anisotropy_;
            mip_bias_ = requested_mip_bias;
        }
        if (requested_shadow_resolution_ != shadow_resolution_)
        {
            if (!resize_sun_shadow_targets(requested_shadow_resolution_))
                requested_shadow_resolution_ = shadow_resolution_;
        }
    }

    // Replace the three sun cascades (depth plus the leaf-transmission channel)
    // with a different square size. Everything is created first and swapped in
    // only when complete; the lit pass reads the texel size from a constant.
    [[nodiscard]] bool resize_sun_shadow_targets(UINT resolution)
    {
        if (shadow_texture_ == nullptr || shadow_srv_ == nullptr)
            return false;
        D3D11_TEXTURE2D_DESC depth_description{};
        shadow_texture_->GetDesc(&depth_description);
        depth_description.Width = resolution;
        depth_description.Height = resolution;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture;
        if (FAILED(device_->CreateTexture2D(&depth_description, nullptr, &depth_texture)))
            return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_description{};
        shadow_views_[0]->GetDesc(&depth_view_description);
        D3D11_SHADER_RESOURCE_VIEW_DESC depth_resource_description{};
        shadow_srv_->GetDesc(&depth_resource_description);
        std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, 3> depth_views;
        for (std::size_t cascade = 0; cascade < depth_views.size(); ++cascade)
        {
            depth_view_description.Texture2DArray.FirstArraySlice = static_cast<UINT>(cascade);
            if (FAILED(device_->CreateDepthStencilView(depth_texture.Get(), &depth_view_description,
                                                       &depth_views[cascade])))
                return false;
        }
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_resource;
        if (FAILED(device_->CreateShaderResourceView(depth_texture.Get(),
                                                     &depth_resource_description, &depth_resource)))
            return false;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> transmission_texture;
        std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 3> transmission_views;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> transmission_resource;
        if (shadow_transmission_texture_ != nullptr && shadow_transmission_srv_ != nullptr &&
            shadow_transmission_views_[0] != nullptr)
        {
            D3D11_TEXTURE2D_DESC transmission_description{};
            shadow_transmission_texture_->GetDesc(&transmission_description);
            transmission_description.Width = resolution;
            transmission_description.Height = resolution;
            D3D11_RENDER_TARGET_VIEW_DESC transmission_view_description{};
            shadow_transmission_views_[0]->GetDesc(&transmission_view_description);
            D3D11_SHADER_RESOURCE_VIEW_DESC transmission_resource_description{};
            shadow_transmission_srv_->GetDesc(&transmission_resource_description);
            if (FAILED(device_->CreateTexture2D(&transmission_description, nullptr,
                                                &transmission_texture)))
                return false;
            for (std::size_t cascade = 0; cascade < transmission_views.size(); ++cascade)
            {
                transmission_view_description.Texture2DArray.FirstArraySlice =
                    static_cast<UINT>(cascade);
                if (FAILED(device_->CreateRenderTargetView(transmission_texture.Get(),
                                                           &transmission_view_description,
                                                           &transmission_views[cascade])))
                    return false;
            }
            if (FAILED(device_->CreateShaderResourceView(transmission_texture.Get(),
                                                         &transmission_resource_description,
                                                         &transmission_resource)))
                return false;
        }

        shadow_texture_ = std::move(depth_texture);
        shadow_views_ = std::move(depth_views);
        shadow_srv_ = std::move(depth_resource);
        if (transmission_texture != nullptr)
        {
            shadow_transmission_texture_ = std::move(transmission_texture);
            shadow_transmission_views_ = std::move(transmission_views);
            shadow_transmission_srv_ = std::move(transmission_resource);
        }
        shadow_resolution_ = resolution;
        return true;
    }

    // The composite pass, optionally compiled without edge smoothing and/or
    // sun shafts. A reduced variant is built the first time a player turns one
    // of them off, so the default configuration costs nothing extra at start.
    [[nodiscard]] ID3D11PixelShader *composite_shader()
    {
        const std::size_t variant = (fxaa_enabled_ ? 0U : 1U) | (sun_shafts_enabled_ ? 0U : 2U);
        if (variant == 0U || composite_source_ == nullptr)
            return composite_pixel_shader_.Get();
        auto &shader = composite_variants_[variant];
        if (shader == nullptr && !composite_variant_failed_[variant])
        {
            std::string source;
            if ((variant & 1U) != 0U)
                source += "#define OPENMINERZ_POST_NO_FXAA 1\n";
            if ((variant & 2U) != 0U)
                source += "#define OPENMINERZ_POST_NO_SHAFTS 1\n";
            source += composite_source_;
            // An optional variant failing must not mark the renderer broken.
            const std::string previous_failure = failure_reason_;
            const auto blob = compile_shader(source.c_str(), "main", "ps_4_0");
            if (blob == nullptr ||
                FAILED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  nullptr, &shader)))
            {
                composite_variant_failed_[variant] = true;
                failure_reason_ = previous_failure;
                shader.Reset();
            }
        }
        return shader != nullptr ? shader.Get() : composite_pixel_shader_.Get();
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(const char *source,
                                                                  const char *entry,
                                                                  const char *target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // Cached for the process (shader_cache.hpp): a world entry builds this
        // renderer again and must not compile every pipeline a second time.
        const HRESULT result = compile_hlsl_cached(
            source, std::char_traits<char>::length(source), nullptr, nullptr, nullptr, entry,
            target, D3DCOMPILE_ENABLE_STRICTNESS, 0U, &shader, &errors);
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
    // r = gloss, g = foliage wave, b * 255 = texture slice, a = lava light.
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
    float lava_light : TEXCOORD10;
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
    output.lava_light = input.material.a;
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
    float4 LiquidView;
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
    float lava_light : TEXCOORD10;
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

// Point lights thrown by this frame's effects (vfx/vfx_renderer.hpp): an
// explosion, a flame. Bound only by the Enhanced presentation's chain; the
// count reads as zero everywhere else and the loop does not run.
cbuffer VfxLights : register(b9)
{
    float4 VfxLightCount;
    float4 VfxLightPosRadius[16];
    float4 VfxLightColor[16];
};

float3 vfx_point_lights(float3 world_position, float3 normal)
{
    float3 light = 0.0f.xxx;
    int count = (int)VfxLightCount.x;
    [loop]
    for (int index = 0; index < count; ++index)
    {
        float3 offset = VfxLightPosRadius[index].xyz - world_position;
        float radius = VfxLightPosRadius[index].w;
        float range_squared = dot(offset, offset);
        if (range_squared < radius * radius)
        {
            float range = sqrt(range_squared);
            float share = saturate(1.0f - range / radius);
            float facing = saturate(dot(normal, offset / max(range, 0.001f)) * 0.82f + 0.18f);
            light += VfxLightColor[index].rgb * (share * share * facing);
        }
    }
    return light;
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
    float3 biased = world_position + geometric_normal * texel_world * (0.55f + 1.2f * slope) +
                    sun_direction_n * texel_world * 0.35f;
    float2 uv = float2(dot(biased, sx.xyz) + sx.w, dot(biased, sy.xyz) + sy.w);
    float depth = dot(biased, sz.xyz) + sz.w;
    if (any(uv < 0.002f) || any(uv > 0.998f))
        return 1.0f;
    float texel = ShadowParams.y;
    float compare_bias = length(sz.xyz) * texel_world * (0.30f + slope * 0.65f);
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
    // "Shadow softness" (Enhanced only): how fast the penumbra opens with the
    // distance to the caster. 1 is the width this renderer always drew; the
    // Original presentation always gets exactly that.
    float softness = AmbientSun.w > 0.5f ? BiomeParams.z * 2.0f : 1.0f;
    float filter_radius = blocker_count > 0.0f
        ? clamp(0.72f + blocker_distance * 0.0045f * softness / max(texel_world, 0.0001f),
                0.72f, 0.72f + 2.53f * softness)
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
    float3 biased = world_position + geometric_normal * texel_world * (0.55f + 1.2f * slope) +
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
    float shadow_opacity = (AmbientSun.w > 0.5f ? lerp(0.70f, 0.94f, saturate(ShadowParams.x)) : lerp(0.18f, 0.43f, saturate(ShadowParams.x))) *
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

// --- Far terrain (far_terrain_renderer.hpp, bind_near_shadow). ---
// The shadow field swept from the far landscape's heights: per texel, the
// height below which the ground is shaded by terrain further toward the sun.
// The sun cascades only know casters near the camera, so without this a
// mountain's evening shadow would darken the far landscape and stop at the
// edge of the detailed window. x counts every caster, y only those beyond the
// cascades' reach, which is what is laid over ground the cascades cover. All
// zero (and the term off) unless far terrain is drawn.
cbuffer FarShadow : register(b7)
{
    float4 FarShadowField[2];
    float4 FarShadowParams;
};
Texture2D<float2> FarShadowOlder : register(t22);
Texture2D<float2> FarShadowNewer : register(t23);
SamplerState FarShadowSampler : register(s8);

float far_field_light(Texture2D<float2> field, float4 placement, float2 relative_xz, float height,
                      float beyond)
{
    float2 uv = (relative_xz + placement.xy) * placement.z;
    float2 margin = min(uv, 1.0f - uv);
    float inside = saturate(min(margin.x, margin.y) * 24.0f) * placement.w;
    // Heights are stored in 1/64 block above the world's floor.
    float2 tops = field.SampleLevel(FarShadowSampler, uv, 0.0f) * (65535.0f / 64.0f);
    float softness = FarShadowParams.y;
    float light = saturate((height - lerp(tops.y, tops.x, beyond) + softness * 0.25f) / softness);
    return lerp(1.0f, light, inside);
}

float far_field_shadow(float3 world_position)
{
    if (FarShadowParams.z < 0.5f)
        return 1.0f;
    float camera_depth = max(dot(world_position - Eye.xyz, CameraForward.xyz), 0.0f);
    // Past the cascades' own fade nothing else shades the ground at all.
    float beyond = saturate((camera_depth - ShadowCascades.z * 0.88f) /
                            max(ShadowCascades.z * 0.12f, 0.001f));
    float2 relative_xz = world_position.xz - Eye.xz;
    float height = world_position.y + 64.0f;
    float light = lerp(far_field_light(FarShadowOlder, FarShadowField[0], relative_xz, height, beyond),
                       far_field_light(FarShadowNewer, FarShadowField[1], relative_xz, height, beyond),
                       FarShadowParams.x);
    return lerp(1.0f, light, FarShadowParams.w);
}

float hash31(float3 value)
{
    value = frac(value * 0.1031f);
    value += dot(value, value.yzx + 33.33f);
    return frac((value.x + value.y) * value.z);
}

// Returns the lit colour and, for the Enhanced post-processing chain, what it
// needs to know about the surface (ignored when only one target is bound, as
// in the Original presentation):
//   surface0  xyz = world normal, w = baked occlusion already applied to the
//             colour
//   surface1  x = share of this pixel's light that is NOT direct sun,
//             y = baked skylight, z = 1 for an entity, w = how much
//             screen-space occlusion may still darken this pixel (0 = none)
float4 shade(PixelInput input, out float4 surface0, out float4 surface1)
{
    surface0 = float4(0.5f, 1.0f, 0.5f, 1.0f);
    surface1 = float4(1.0f, 1.0f, 0.0f, 1.0f);
    float distance_to_eye = distance(input.world_position, Eye.xyz);
    float horizontal_distance_to_eye = distance(input.world_position.xz, Eye.xz);
    float fog = saturate((horizontal_distance_to_eye - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    if (AmbientSun.w > 0.5f)
        fog = fog * fog * (3.0f - 2.0f * fog);
    // Hell has suspended ash and heat haze even before the normal streaming-edge
    // curtain. It reveals depth as warm silhouettes instead of raising every
    // cave surface with a flat ambient term. Eye.w is zero for Classic and for
    // the matched legacy diagnostic path.
    float hell_depth = Eye.w * smoothstep(24.0f, 36.0f, -Eye.y);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance_to_eye * 0.025f)) * 0.38f;
    fog = max(fog, hell_air);
    if (input.emission < -1.5f)
    {
        // (mod-liquid) A mod's liquid block, drawn over the finished opaque
        // scene: its own texture lit by the face's baked sky and torch light,
        // and as see-through as the liquid between it and what lies behind
        // is thin. Enhanced adds the Lagoon water's moving surface, sky
        // reflection and sun glint.
        bool modern_liquid = AmbientSun.w > 0.5f;
        bool from_inside = LiquidView.x > 0.5f;
        float4 liquid_texel = DiffuseAtlas.Sample(AnisoSampler, float3(input.uv, input.tile));
        // Enhanced keeps about half of the tile's detail over its mean colour:
        // a surface, not a carpet of repeated tiles.
        if (modern_liquid)
            liquid_texel.rgb = lerp(DiffuseAtlas.SampleLevel(AtlasSampler, float3(0.5f, 0.5f, input.tile), 8.0f).rgb,
                                    liquid_texel.rgb, 0.5f);
        float3 albedo = modern_liquid ? liquid_texel.rgb * liquid_texel.rgb : liquid_texel.rgb;
        float3 liquid_normal = normalize(input.normal);
        float liquid_sky = saturate(input.sky_light);
        float liquid_torch = saturate(input.torch_light);
        float3 liquid_sun = normalize(SunAndOptions.xyz);
        float liquid_shadow = sun_shadow(input.world_position, liquid_normal, distance_to_eye);
        float3 torch_tint = modern_liquid ? TorchAndEye.rgb * TorchAndEye.rgb : TorchAndEye.rgb;
        float3 liquid_light =
            AmbientSun.rgb * liquid_sky * (1.0f + WaterAndTime.w * 0.25f) +
            LightAndFogStart.rgb * pow(liquid_sky, 30.0f) *
                (0.80f + saturate(dot(liquid_normal, liquid_sun)) * 0.20f) * liquid_shadow +
            torch_tint * liquid_torch * (1.0f - liquid_sky * LightAndFogStart.g);
        float3 body = albedo * liquid_light;
        // The liquid the eye looks through: from this face to the scene's
        // own depth behind it (all of it where only sky lies behind).
        float behind_depth = SceneDepth.Load(int3(input.position.xy, 0)).r;
        float behind_camera_z = Projection.w / min(behind_depth - Projection.z, -0.00001f);
        float thickness = behind_depth >= 0.999999f ? 64.0f
                                                    : max(0.0f, behind_camera_z - input.position.w);
        float opacity = lerp(0.42f, 0.92f, 1.0f - exp(-thickness * 0.30f));
        // A texture drawn partly transparent makes a clearer liquid.
        opacity *= saturate(0.35f + 0.65f * liquid_texel.a);
        // Deep liquid darkens toward a richer shade of its own colour.
        body *= lerp(float3(1.0f, 1.0f, 1.0f), float3(0.42f, 0.62f, 0.78f),
                     saturate(1.0f - exp(-thickness * 0.10f)));
        // From inside, the surface is a window: the water up to it is the fog.
        if (from_inside)
            opacity = 0.32f;
        if (modern_liquid)
        {
            float time = WaterAndTime.x;
            float2 p = input.world_position.xz;
            float2 grad = float2(0.0f, 0.0f);
            grad += float2(0.9f, 0.36f) * cos(dot(p, float2(0.9f, 0.36f)) * 1.9f + time * 1.35f) * 0.045f;
            grad += float2(-0.42f, 0.91f) * cos(dot(p, float2(-0.42f, 0.91f)) * 3.1f - time * 1.1f) * 0.03f;
            grad += float2(0.65f, -0.76f) * cos(dot(p, float2(0.65f, -0.76f)) * 5.3f + time * 1.9f) * 0.02f;
            float3 wave_normal = abs(liquid_normal.y) > 0.5f
                                     ? normalize(float3(-grad.x, 1.0f, -grad.y)) * sign(liquid_normal.y)
                                     : liquid_normal;
            float3 liquid_view = normalize(Eye.xyz - input.world_position);
            float3 facing_normal = dot(wave_normal, liquid_view) < 0.0f ? -wave_normal : wave_normal;
            float fresnel = 0.02f + 0.98f * pow(1.0f - saturate(dot(facing_normal, liquid_view)), 5.0f);
            float liquid_daylight = 0.35f + WaterAndTime.w * 0.65f;
            float3 reflect_direction = reflect(-liquid_view, facing_normal);
            float3 sky_reflection =
                lerp(FogAndEnd.rgb,
                     lerp(FogAndEnd.rgb, AmbientSun.rgb + float3(0.06f, 0.10f, 0.16f), 0.6f) * liquid_daylight,
                     saturate(reflect_direction.y));
            float glint = pow(saturate(dot(reflect_direction, liquid_sun)), 720.0f) * 2.6f +
                          pow(saturate(dot(reflect_direction, liquid_sun)), 48.0f) * 0.22f;
            if (!from_inside)
            {
                float reflect_share = saturate(fresnel * 1.05f) * liquid_sky;
                body = lerp(body, sky_reflection, reflect_share) +
                       LightAndFogStart.rgb * glint * WaterAndTime.w * liquid_shadow * liquid_sky;
                opacity = max(opacity, reflect_share);
            }
        }
        body = lerp(body, FogAndEnd.rgb, fog);
        opacity = lerp(opacity, 1.0f, fog);
        // What the Enhanced chain learns of the surface (its depth pass):
        // its own normal, and nothing for screen-space occlusion to take.
        surface0 = float4(liquid_normal * 0.5f + 0.5f, 1.0f);
        surface1 = float4(1.0f, liquid_sky, 0.0f, 0.0f);
        return float4(body, opacity);
    }
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
    bool blood_stone = modern_lighting && abs(input.tile - 19.0f) < 0.25f;
    float3 geometric_normal = normalize(input.normal);
    float2 world_uv = abs(geometric_normal.y) > 0.5f
                          ? input.world_position.xz
                          : (abs(geometric_normal.x) > 0.5f
                                 ? input.world_position.zy
                                 : input.world_position.xy);
    if (modern_lighting)
    {
        // A second, lower-frequency world-anchored read breaks the obvious
        // one-texture-per-cube repetition. Because the UV does not reset at
        // voxel boundaries, neighbouring faces of the same material form one
        // continuous patch instead of a checkerboard of cloned tiles.
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
    if (blood_stone)
    {
        // The source is almost pure red, with zero green/blue in its cracks.
        // Recover a mineral albedo from that signal instead of clamping every
        // dark channel to grey. The installed rock tile supplies fine grain;
        // the original blood-stone veins and their dark recesses stay intact.
        float3 stone_detail = DiffuseAtlas.Sample(
            AnisoSampler, float3(world_uv * 1.35f + float2(0.17f, 0.41f), 5.0f)).rgb;
        float stone_luma = dot(stone_detail, float3(0.2126f, 0.7152f, 0.0722f));
        float mineral = lerp(pow(max(texel.r, 0.0001f), 0.64f),
                              stone_luma * stone_luma * 1.4f, 0.65f);
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        // (In the deep underground the tint keeps some green and blue and
        // deepens in the recesses, so the stone can read brown in the dark
        // and orange near lava; hell_atmosphere.hpp. Above it, and wherever
        // that model is off, blood stone is the red it always was.)
        texel.rgb = mineral * lerp(float3(0.92f, 0.115f, 0.065f), hell_blood_stone(mineral),
                                   HellModel.x * hell_depth_presence(input.world_position.y));
#else
        texel.rgb = mineral * float3(0.92f, 0.115f, 0.065f);
#endif
    }
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
    if (blood_stone)
    {
        // Broad ridges in the old normal map read as inflated rubber in HDR.
        // Keep a little of their relief and add the stone's shorter-scale slope.
        float2 stone_normal = NormalSpecAtlas.Sample(
            AnisoSampler, float3(world_uv * 1.35f + float2(0.17f, 0.41f), 5.0f)).rg;
        float2 stone_slope = stone_normal * 2.0f - 1.0f;
        stone_slope.x *= abs(geometric_normal.y) > 0.5f ? geometric_normal.y
                          : (abs(geometric_normal.x) > 0.5f ? -geometric_normal.x
                                                             : geometric_normal.z);
        tangent_xy = (normal_spec.rg * 2.0f - 1.0f) * 0.08f +
                     stone_slope * 0.34f;
    }
    float tangent_z = sqrt(saturate(1.0f - dot(tangent_xy, tangent_xy)));
    float3 surface_normal = normalize(tangent * tangent_xy.x + bitangent * tangent_xy.y +
                                      geometric_normal * tangent_z);
    float4 metal_light = MetalLightAtlas.Sample(AnisoSampler, tile_uv);
    float authored_gloss = max(saturate(metal_light.r), saturate(input.gloss));
    // Treat the authored red response primarily as smoothness. Dirt and grass
    // both carry about 0.30 in this channel; mapping that directly to highlight
    // intensity was the source of their wet-plastic sheen.
    float material_roughness = lerp(0.90f, 0.18f, authored_gloss);
    if (blood_stone)
    {
        authored_gloss = 0.0f;
        material_roughness = 0.96f;
    }
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
        // Authored Hell fill, independent of skylight and lava proximity.
        // Match world::hell_ambient; retain directional relief and corner AO.
        float hell_surface = Eye.w * smoothstep(24.0f, 36.0f, -input.world_position.y);
        float hell_facing = 0.56f + 0.44f * saturate(0.5f - surface_normal.y * 0.5f);
        ambient_term = lerp(ambient_term, float3(1.10f, 0.20f, 0.075f) * hell_facing,
                            hell_surface);
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        // The deep underground (hell_atmosphere.hpp), which replaces that fill
        // whenever it runs (Eye.w is then zero): the cavern's own warm glow
        // and the lava lake as an area light, by how deep this surface is.
        ambient_term += hell_surface_light(input.world_position, surface_normal, ao).rgb;
#endif
        // The held light's directly visible surface seeds live in a tiny
        // player-centred 3-D clipmap. Gathering them in this face hemisphere
        // adds real moving color bounce without screen-space leakage.
        ambient_term += dynamic_radiance_gather(
            input.world_position, geometric_normal, tangent, bitangent) * material_ao;
        // An explosion or a flame nearby lights this surface for as long as it
        // burns (Enhanced effects only; see vfx_point_lights).
        ambient_term += vfx_point_lights(input.world_position, surface_normal);
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
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
    // Light the rock beside lava gives off itself (the deep underground).
    float3 hell_hot = 0.0f.xxx;
#endif
    if (modern_lighting)
    {
        // Lava has its own transported field, packed in the spare material
        // byte. Both lava types illuminate nearby geometry in orange-red;
        // held and placed torches retain their warmer cream colour.
        float lava_level = min(placed_torch_level, saturate(input.lava_light));
        float lava_share = lava_level / max(placed_torch_level, 0.0001f);
        float lava_energy = pow(lava_level, 1.6f) * placed_response +
                            local_torch_bounce * lava_share;
        float ordinary_energy = max(0.0f, placed_torch_level - lava_level) * placed_response +
                                held_glow * held_response * held_visibility +
                                local_torch_bounce * (1.0f - lava_share);
        if (BiomeParams.w > 0.5f)
        {
            // A flame is never still, and lava breathes. Smooth in space and
            // time (no cell seams); distant torches drift out of step. Off
            // with Reduced motion.
            float3 p = input.world_position;
            float t = WaterAndTime.x;
            ordinary_energy *= 1.0f + 0.034f * sin(t * 9.3f + dot(p, float3(0.23f, 0.17f, 0.19f))) +
                               0.022f * sin(t * 17.9f + dot(p, float3(-0.13f, 0.21f, 0.11f)) + 1.7f) +
                               0.012f * sin(t * 31.1f + dot(p, float3(0.07f, -0.11f, 0.29f)) + 4.1f);
            lava_energy *= 1.0f + 0.045f * sin(t * 1.35f + dot(p, float3(0.11f, 0.05f, 0.09f)));
        }
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        float3 lava_color = float3(2.8f, 0.60f, 0.075f);
        if (HellModel.x > 0.5f)
        {
            // The deep underground's model: lava light that is yellower right
            // at the lava, breathes in slow unrelated swells, and heats the
            // rock beside it until that glows by itself.
            float lava_depth = hell_depth_presence(input.world_position.y);
            lava_color = hell_lava_near_color(lava_level, lava_depth);
            lava_energy *= hell_lava_flicker(input.world_position);
            hell_hot = hell_incandescence(
                lava_level, dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f)) * 4.0f,
                lava_depth);
        }
        torch_component = (torch_color * ordinary_energy + lava_color * lava_energy) *
                          (1.0f - sky_exposure * LightAndFogStart.g * 0.65f);
#else
        torch_component = (torch_color * ordinary_energy +
                           float3(2.8f, 0.60f, 0.075f) * lava_energy) *
                          (1.0f - sky_exposure * LightAndFogStart.g * 0.65f);
#endif
    }
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
    // (Far terrain: shade from the landscape beyond the cascades' reach.)
    shadow = min(shadow, far_field_shadow(input.world_position));
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
#if defined(OPENMINERZ_MOD_LIGHTS)
    // Lighting props placed by mods (modding/graphics.hpp). Compiled in only
    // while a mod has lights, so an unmodded frame runs this shader as it was.
    ambient_term += mod_point_lights(input.world_position, surface_normal, 0.15f);
#endif
    float3 light_total = ambient_term + torch_component + direct_term + transient_direct;
    float3 lit = texel.rgb * light_total * material_ao;
    float3 half_direction = normalize(sun_direction + view_direction);
    if (modern_lighting)
    {
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        // Rock hot enough to glow by itself (the deep underground; zero
        // anywhere the lava's flood-filled light does not reach).
        lit += hell_hot * material_ao;
#endif
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
        float metalness = blood_stone ? 0.0f : smoothstep(0.82f, 0.98f, saturate(metal_light.r));
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
    // Keep each pack's authored mineral color. Slot 7 is orange iron and slot
    // 8 is green copper in both originals; a guessed metal palette made these
    // white/blue and orange. Mask neutral rock/highlights as well as smoothness;
    // coal (slot 9) has no colored veins and must not emit pale edge highlights.
    bool colored_ore = (input.tile >= 5.5f && input.tile < 8.5f) ||
                       (input.tile > 9.5f && input.tile <= 10.5f);
    if (BiomeParams.y > 0.5f && colored_ore)
    {
        float peak = max(texel.r, max(texel.g, texel.b));
        float low = min(texel.r, min(texel.g, texel.b));
        float chroma = (peak - low) / max(peak, 0.0001f);
        float ore_mask = smoothstep(0.34f, 0.88f, saturate(metal_light.r)) *
                         smoothstep(0.12f, 0.45f, chroma);
        float3 ore_tint = texel.rgb / max(peak, 0.0001f);
        float ore_strength = 0.34f;
        float pulse = 0.92f + 0.08f * sin(WaterAndTime.x * 1.45f +
                                         dot(input.world_position, float3(0.31f, 0.17f, 0.23f)));
        lit += ore_tint * ore_mask * ore_strength * pulse;
    }
    lit = lerp(lit, texel.rgb * 1.12f, saturate(input.emission));
    if (modern_lighting && input.emission > 0.5f && input.tile > 10.5f && input.tile < 11.5f)
    {
        // Full-bright display albedo alone made lava look like cold black rock
        // after linear decoding. Its hot cracks need HDR emission and bloom.
        float heat = saturate(max(texel.r, max(texel.g, texel.b)) * 3.5f);
        float molten = smoothstep(0.004f, 0.14f, heat);
        float3 hot_cracks = lerp(float3(2.2f, 0.20f, 0.005f),
                                 float3(5.5f, 1.8f, 0.16f), heat * heat);
        // Classic's lava plate has many more bright texels than Modern's.
        // Normalize their coverage so changing packs does not blow out a pool.
        float source_coverage = DiffuseAtlas.SampleLevel(
            AtlasSampler, float3(0.5f, 0.5f, input.tile), 7.0f).r;
        float emission_scale = clamp(0.10f / max(source_coverage, 0.01f), 0.36f, 1.0f);
        lit = float3(0.025f, 0.001f, 0.0003f) + hot_cracks * molten * emission_scale;
    }
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
    // The deep underground's molten rock (hell_atmosphere.hpp), in place of
    // the lava above whenever that model runs: the pack's own lava artwork
    // read as two layers that drift across each other, under plates of crust
    // that crack and breathe. The derivatives are taken out here and every
    // read inside names its own, so this is a real branch only lava takes.
    float2 lava_face_dx = ddx(world_uv);
    float2 lava_face_dy = ddy(world_uv);
    [branch]
    if (HellModel.x > 0.5f && input.emission > 0.5f && input.tile > 10.5f && input.tile < 11.5f)
    {
        float2 lava_face = hell_lava_face(input.world_position, geometric_normal);
        float4 lava_layers = hell_lava_layers(lava_face, lava_face_dx, lava_face_dy);
        float3 lava_first = DiffuseAtlas.SampleGrad(
            AnisoSampler, float3(lava_layers.xy, input.tile),
            lava_face_dx * HELL_LAVA_FIRST_SCALE, lava_face_dy * HELL_LAVA_FIRST_SCALE).rgb;
        float3 lava_second = DiffuseAtlas.SampleGrad(
            AnisoSampler, float3(lava_layers.zw, input.tile),
            lava_face_dx * HELL_LAVA_SECOND_SCALE, lava_face_dy * HELL_LAVA_SECOND_SCALE).rgb;
        // A pack with more bright artwork is turned down, as above.
        float lava_coverage = DiffuseAtlas.SampleLevel(
            AtlasSampler, float3(0.5f, 0.5f, input.tile), 7.0f).r;
        float art_scale = sqrt(clamp(0.10f / max(lava_coverage, 0.01f), 0.36f, 1.0f));
        lit = hell_molten(lava_face, lava_face_dx, lava_face_dy,
                          max(lava_first.r, max(lava_first.g, lava_first.b)) * art_scale,
                          max(lava_second.r, max(lava_second.g, lava_second.b)) * art_scale) *
              (1.0f + HELL_MOLTEN_DAYLIGHT * sky_exposure * WaterAndTime.w);
    }
#endif
    // Screen-space occlusion and contact shadows may only take away light that
    // is really there to lose: nothing from a glowing surface, nothing from
    // the fog in front of it, and (contact shadows) only the sun's share.
    float untouchable = saturate(max(saturate(input.emission), fog));
    float all_light = dot(light_total, float3(0.2126f, 0.7152f, 0.0722f));
    float sun_light = dot(direct_term, float3(0.2126f, 0.7152f, 0.0722f));
    // A leaf cluster is mostly gaps: left alone, screen-space occlusion would
    // read every gap as a crease and turn canopies to soot, so leaves take
    // half of it.
    surface0 = float4(geometric_normal * 0.5f + 0.5f, material_ao);
    surface1 = float4(lerp(saturate(1.0f - sun_light / max(all_light, 0.0001f)), 1.0f, untouchable),
                      sky_exposure, 0.0f, (enhanced_leaf ? 0.5f : 1.0f) * (1.0f - untouchable));
    return float4(lerp(lit, FogAndEnd.rgb, fog), 1.0f);
}

struct PixelOutput
{
    float4 color : SV_TARGET0;
    float4 surface0 : SV_TARGET1;
    float4 surface1 : SV_TARGET2;
};

PixelOutput main(PixelInput input)
{
    PixelOutput output;
    output.color = shade(input, output.surface0, output.surface1);
    return output;
}
)hlsl";
        const auto vertex_blob = compile_shader(vertex_source, "main", "vs_4_0");
        // Array-comparison sampling for the three shadow cascades needs the
        // native D3D11 shader model; ps_4_0 cannot lower a dynamic array slice.
        // The deep underground's constants and lighting functions come first
        // (hell_atmosphere.hpp); they declare nothing this shader declares, and
        // they define the switch its `#if defined(OPENMINERZ_DEEP_UNDERGROUND)`
        // blocks test. Without them (the diagnostic below, or a compiler that
        // refuses them) the shader is, text for text, the one it was before.
        const bool deep_underground_shaders = deep_underground_shaders_wanted();
        auto pixel_blob = deep_underground_shaders
                              ? compile_shader(
                                    (HellAtmosphere::shader_library() + pixel_source).c_str(),
                                    "main", "ps_5_0")
                              : nullptr;
        deep_underground_terrain_ = pixel_blob != nullptr;
        if (pixel_blob == nullptr)
        {
            if (deep_underground_shaders)
                SDL_Log("Deep terrain shader unavailable: %s", failure_reason_.c_str());
            failure_reason_.clear();
            pixel_blob = compile_shader(pixel_source, "main", "ps_5_0");
        }
        // The exact text compiled, for the mod-light variant (graphics area).
        terrain_pixel_text_ = deep_underground_terrain_
                                  ? HellAtmosphere::shader_library() + pixel_source
                                  : std::string{pixel_source};
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
        {
            // Opaque, but leaves a fourth target (per-object motion, bound only
            // while motion blur is on) untouched: terrain does not move, and a
            // pixel shader that does not write a bound target leaves it
            // undefined. Optional; without it motion blur is camera-only.
            D3D11_BLEND_DESC motion_description{};
            motion_description.IndependentBlendEnable = TRUE;
            for (std::size_t target = 0U; target < 3U; ++target)
                motion_description.RenderTarget[target].RenderTargetWriteMask =
                    D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(device_->CreateBlendState(&motion_description,
                                                 &terrain_motion_blend_state_)))
                terrain_motion_blend_state_.Reset();
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
#if defined(OPENMINERZ_POST_NO_FXAA)
    // Edge smoothing is switched off in the settings: pass the scene through.
    return Scene.Sample(LinearSampler, uv);
#else
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
#endif
}

float3 display_color(float3 color)
{
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
    return color;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    // Everything downstream works in exposed linear light; the bright pass
    // already exposed the bloom it produced.
    float exposure = scene_exposure();
    float4 scene = fxaa_scene(input.uv, exposure);
    float3 color = Bloom.Sample(LinearSampler, input.uv).rgb * PostParams.x;
    if (PostSun.z > 0.02f)
    {
        float2 sun_uv = PostSun.xy;
        float2 delta = sun_uv - input.uv;
        float aspect = TargetSize.x * TargetSize.w;
        // The glow and shaft radii below are ANGLES around the sun, written as
        // screen heights at the default 74-degree field of view (1.32704 is
        // 1 / tan(37 degrees), Projection.y is 1 / tan(fov / 2)). Measuring the
        // distance in those units keeps the glow the same size against the sky
        // at every field of view and through a shouldered zoom.
        float sun_distance =
            length(float2(delta.x * aspect, delta.y)) * (1.32704f / max(Projection.y, 0.001f));
        float2 outside = max(float2(0.0f, 0.0f), max(-sun_uv, sun_uv - 1.0f));
        float edge_fade = saturate(1.0f - (outside.x + outside.y) * 3.0f);
        float energy = 0.25f + 0.75f * PostSun.w;
        float3 sun_color = normalize(LightAndFogStart.rgb + 0.001f) * 1.05f;
#if !defined(OPENMINERZ_POST_NO_SHAFTS)
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
#endif
        // Sun glow disc, occluded by geometry in front of the sun.
        float sun_visibility = 1.0f - Scene.Sample(LinearSampler, saturate(sun_uv)).a;
        float glow = pow(saturate(1.0f - sun_distance / 0.35f), 3.0f) * 0.28f +
                     pow(saturate(1.0f - sun_distance / 0.06f), 2.0f) * 0.95f;
        color += sun_color * glow * sun_visibility * edge_fade * energy;
    }
    // FXAA stores premultiplied linear coverage. Nonlinear tone mapping or gamma
    // must operate on the unassociated colour; mapping coverage itself created
    // bright outlines around mountains and every fine leaf against the sky.
    float3 additive_light = color;
    color += scene.rgb * exposure / max(scene.a, 0.0001f);
    color = display_color(color) * scene.a;
    if (scene.a < 0.9999f)
        color += display_color(additive_light) * (1.0f - scene.a);
    color += (apply_dither(0.0f.xxx, input.position.xy)) * scene.a;
    return float4(color, scene.a);
}
)hlsl";
        // Kept so composite_shader() can build the reduced variants on demand.
        composite_source_ = composite_pixel_source;
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

// Last frame's camera (the head of post_processing.cpp's Post buffer), bound
// only by the Enhanced presentation while motion blur is on.
cbuffer PostCamera : register(b2)
{
    float4 PostSize;
    float4 PostTime;
    float4 PrevEye;
    float4 PrevRight;
    float4 PrevUp;
    float4 PrevForward;
    float4 PrevProjection;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 normal : NORMAL0;
    float2 light_data : TEXCOORD1;
    // The same vertex one frame ago (second stream).
    float3 previous : PREVIOUS;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float2 light_data : TEXCOORD3;
    // Clip-space x, y, w now and one frame ago, for per-object motion blur.
    float3 motion_now : TEXCOORD4;
    float3 motion_before : TEXCOORD5;
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
    output.motion_now = output.position.xyw;
    // A vertex that jumped (a respawn, a different creature in the same slot)
    // did not travel there: treat it as standing still.
    float3 before = input.previous;
    float3 travelled = before - input.position;
    if (dot(travelled, travelled) > 2.25f)
        before = input.position;
    float3 previous_relative = before - PrevEye.xyz;
    output.motion_before = float3(dot(previous_relative, PrevRight.xyz) * PrevProjection.x,
                                  dot(previous_relative, PrevUp.xyz) * PrevProjection.y,
                                  dot(previous_relative, PrevForward.xyz));
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
    float3 motion_now : TEXCOORD4;
    float3 motion_before : TEXCOORD5;
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

// Point lights thrown by this frame's effects (vfx/vfx_renderer.hpp). Bound
// only by the Enhanced presentation's chain; zero lights everywhere else.
cbuffer VfxLights : register(b9)
{
    float4 VfxLightCount;
    float4 VfxLightPosRadius[16];
    float4 VfxLightColor[16];
};

float3 entity_vfx_point_lights(float3 world_position, float3 normal)
{
    float3 light = 0.0f.xxx;
    int count = (int)VfxLightCount.x;
    [loop]
    for (int index = 0; index < count; ++index)
    {
        float3 offset = VfxLightPosRadius[index].xyz - world_position;
        float radius = VfxLightPosRadius[index].w;
        float range_squared = dot(offset, offset);
        if (range_squared < radius * radius)
        {
            float range = sqrt(range_squared);
            float share = saturate(1.0f - range / radius);
            float facing = saturate(dot(normal, offset / max(range, 0.001f)) * 0.82f + 0.18f);
            light += VfxLightColor[index].rgb * (share * share * facing);
        }
    }
    return light;
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
    float opacity = (AmbientSun.w > 0.5f ? lerp(0.70f, 0.94f, saturate(ShadowParams.x)) : lerp(0.18f, 0.43f, saturate(ShadowParams.x))) * ShadowParams.z;
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

// Lit colour plus the terrain shader's surface data (see `shade` there). A
// prop without normals, a projectile or a glowing effect reports "nothing may
// darken this pixel further", so screen-space occlusion leaves it alone.
float4 shade(PixelInput input, out float4 surface0, out float4 surface1)
{
    surface0 = float4(0.5f, 1.0f, 0.5f, 1.0f);
    surface1 = float4(1.0f, 1.0f, 1.0f, 0.0f);
    float4 texel = Diffuse.Sample(DiffuseSampler, input.uv);
    bool modern_lighting = AmbientSun.w > 0.5f;
    if (modern_lighting)
        texel.rgb *= texel.rgb;
    float horizontal_distance = distance(input.world_position.xz, Eye.xz);
    // Entities cap their fog: the original never fogged the dragon at all, so
    // a distant dragon reads as a silhouette instead of vanishing.
    float fog = saturate((horizontal_distance - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    if (AmbientSun.w > 0.5f)
        fog = fog * fog * (3.0f - 2.0f * fog);
    float hell_depth = Eye.w * smoothstep(24.0f, 36.0f, -Eye.y);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance * 0.025f)) * 0.38f;
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
        ambient += entity_vfx_point_lights(input.world_position, normal);
#if defined(OPENMINERZ_MOD_LIGHTS)
        ambient += mod_point_lights(input.world_position, normal, 0.15f);
#endif

        float3 sun_direction = normalize(SunAndOptions.xyz);
        float n_dot_l = saturate(dot(normal, sun_direction));
        float direct_gate = pow(sky_exposure, 4.0f);
        float sun_visibility = entity_sun_shadow(input.world_position, normal);
        float3 direct = LightAndFogStart.rgb * n_dot_l * direct_gate * sun_visibility;

        float placed = saturate(input.light_data.y) *
                       (1.0f - sky_exposure * LightAndFogStart.g * 0.85f);
        float hell_actor = Eye.w * smoothstep(24.0f, 36.0f, -input.world_position.y);
        ambient = lerp(ambient, float3(1.10f, 0.20f, 0.075f) *
                                (0.56f + 0.44f * saturate(0.5f - normal.y * 0.5f)), hell_actor);
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        // The deep underground (hell_atmosphere.hpp), which replaces that fill
        // whenever it runs: creatures stand in the same glow as the rock, lit
        // from below by the same lava.
        float4 hell_light = hell_surface_light(input.world_position, normal, 1.0f);
        ambient += hell_light.rgb;
#endif
        float held = TorchAndEye.w *
                     pow(saturate(1.0f - distance(input.world_position, Eye.xyz) / 9.0f), 2.0f);
        float held_facing = 0.24f + 0.96f * saturate(dot(normal, view_direction));
        float held_visibility = entity_point_shadow(input.world_position, normal);
        float3 torch_color = TorchAndEye.rgb * TorchAndEye.rgb;
        float3 torch = torch_color * (placed * 0.78f +
                                      held * held_facing * held_visibility);
#if defined(OPENMINERZ_DEEP_UNDERGROUND)
        // A creature carries one flood-filled light value, torches and lava
        // together. Deep down almost all of it is lava, so there it takes the
        // lava's colour and comes mostly from below.
        if (hell_light.a > 0.0f)
            torch = lerp(torch_color * 0.78f,
                         hell_lava_near_color(placed, hell_light.a) *
                             (0.30f + 0.55f * saturate(0.6f - normal.y * 0.6f)),
                         hell_light.a) * placed +
                    torch_color * (held * held_facing * held_visibility);
#endif
        entity_light = ambient + direct + torch;
        float all_light = dot(entity_light, float3(0.2126f, 0.7152f, 0.0722f));
        float sun_light = dot(direct, float3(0.2126f, 0.7152f, 0.0722f));
        surface0 = float4(normal * 0.5f + 0.5f, 1.0f);
        surface1 = float4(lerp(saturate(1.0f - sun_light / max(all_light, 0.0001f)), 1.0f, fog),
                          sky_exposure, 1.0f, 1.0f - fog);
        // A lit prop too small for screen-space shading to resolve (the
        // effects layer's explosion debris, marked by a vertex alpha of zero)
        // asks to be left alone by it, as an unlit prop does: half-resolution
        // occlusion and contact shadows otherwise paint a chunk a few pixels
        // wide solid black.
        if (input.color.a < 0.5f)
            surface1 = float4(1.0f, sky_exposure, 1.0f, 0.0f);
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
        // Unnormalled props share the same authored Hell tint.
        entity_light = lerp(entity_light, float3(1.10f, 0.20f, 0.075f), hell_depth);
    }
    float3 lit = texel.rgb * input.color.rgb * entity_light;
#if defined(OPENMINERZ_MOD_LIGHTS)
    // Mod lights on what the branch above did not light with them: the
    // Original presentation (vertex colours carry its baked light) and props
    // without normals, which take them from every side.
    if (!(modern_lighting && normal_length_squared > 0.25f))
        lit += texel.rgb * mod_point_lights(input.world_position,
                                            normal_length_squared > 0.25f
                                                ? normalize(input.normal)
                                                : float3(0.0f, 1.0f, 0.0f),
                                            normal_length_squared > 0.25f ? 0.15f : 1.0f);
#endif
    if (input.light_data.x < -2.5f)
    {
        float edge = abs(input.uv.x - 0.5f) * 2.0f;
        clip(0.98f - edge);
        lit = input.color.rgb * (1.8f + 1.4f * pow(saturate(1.0f-edge), 4.0f));
    }
    else if (input.light_data.x < -1.5f)
        lit = texel.rgb * max(entity_light, 0.25f) + input.color.rgb * texel.a * 3.2f;
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

struct PixelOutput
{
    float4 color : SV_TARGET0;
    float4 surface0 : SV_TARGET1;
    float4 surface1 : SV_TARGET2;
    // How far this pixel moved on screen since last frame (uv), alpha one.
    // Bound only while the Enhanced motion blur is on.
    float4 motion : SV_TARGET3;
};

PixelOutput main(PixelInput input)
{
    PixelOutput output;
    output.color = shade(input, output.surface0, output.surface1);
    float2 now = input.motion_now.xy / max(input.motion_now.z, 0.0001f);
    float2 before = input.motion_before.xy / max(input.motion_before.z, 0.0001f);
    output.motion = float4((now - before) * float2(0.5f, -0.5f), 0.0f, 1.0f);
    return output;
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
Texture3D<float> EffectNoise : register(t9);
SamplerState DiffuseSampler : register(s0);
SamplerState EffectNoiseSampler : register(s9);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 volume_center : TEXCOORD2;
    float2 light_data : TEXCOORD3;
};

// Animated 3D density integrated along the viewing ray, inside a spherical
// proxy. No flame/smoke image is sampled. Scene depth clips the ray interval,
// so volumes intersect walls continuously and cannot paint over foregrounds.
float vfx_noise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    return EffectNoise.SampleLevel(EffectNoiseSampler, (i + f + 0.5f) / 64.0f, 0).r;
}
float4 volume_effect(PixelInput input)
{
    float radius = max(input.light_data.x, 0.015f);
    bool core = input.light_data.y < -4.5f;
    float3 axis = core ? normalize(input.color.rgb) : float3(0,1,0);
    float stretch = core ? 2.4f : 1.0f;
    float3 ray = normalize(input.world_position - Eye.xyz);
    float3 offset = Eye.xyz - input.volume_center;
    float3 local_ray = ray + axis * dot(ray, axis) * (1.0f/stretch - 1.0f);
    float3 local_offset = offset + axis * dot(offset, axis) * (1.0f/stretch - 1.0f);
    float a = dot(local_ray, local_ray);
    float b = dot(local_offset, local_ray);
    float discriminant = b * b - a * (dot(local_offset, local_offset) - radius * radius);
    clip(discriminant);
    float reach = sqrt(max(discriminant, 0.0f));
    float entry = max(0.0f, (-b - reach) / a);
    float exit = (-b + reach) / a;
    float raw = SceneDepth.Load(int3(int2(input.position.xy), 0));
    float scene_z = Projection.w / (raw - Projection.z);
    exit = min(exit, scene_z / max(dot(ray, CameraForward.xyz), 0.0001f));
    clip(exit - entry);
    float step_length = (exit - entry) / 14.0f;
    float transmittance = 1.0f;
    float3 radiance = 0.0f;
    bool smoke = input.light_data.y > -2.5f;
    bool ice = abs(input.light_data.y + 4.0f) < 0.2f || input.light_data.y < -5.5f;
    float time = WaterAndTime.x;
    [loop] for (int j = 0; j < 14; ++j)
    {
        float3 position = Eye.xyz + ray * (entry + ((float)j + 0.5f) * step_length);
        float3 local = position - input.volume_center;
        float3 q = (local + axis * dot(local, axis) * (1.0f/stretch - 1.0f)) / radius;
        float3 flow = q * float3(3.1f, smoke ? 3.1f : 2.0f, 3.1f) + input.volume_center * 0.17f;
        flow.y -= time * (smoke ? 0.55f : 2.4f);
        float billow = vfx_noise(flow * 0.63f + 3.7f);
        flow += (billow - 0.5f) * 1.8f;
        float noise = vfx_noise(flow) * 0.58f + vfx_noise(flow * 2.03f + 7.1f) * 0.28f +
                      vfx_noise(flow * 4.11f - 3.2f) * 0.14f;
        float edge = saturate(((smoke ? 0.80f : 0.60f) - dot(q, q) +
                              (billow - 0.5f) * (smoke ? 1.1f : 1.6f)) * 2.8f) *
                     saturate((1.0f - dot(q,q)) * 6.0f);
        float density = edge * smoothstep(smoke ? 0.22f : 0.30f, 0.72f, noise);
        // Optical depth is integrated from the eye when inside the plume;
        // there is no close-range quad fade or opaque full-screen smoke card.
        float opacity = 1.0f - exp(-density * step_length / radius * (smoke ? 3.2f : 4.0f) * input.color.a);
        float3 color;
        if (smoke)
        {
            float daylight = saturate(WaterAndTime.w);
            float top_light = saturate(q.y * 0.5f + 0.5f);
            float self_shadow = exp(-density * 1.6f * (1.0f - top_light));
            float lighting = 0.24f + (0.36f + 0.65f * daylight) * self_shadow;
            color = input.color.rgb * lighting;
        }
        else
        {
            float temperature = saturate((noise - 0.23f) * 1.85f - dot(q,q) * 0.16f);
            float3 outer = ice ? float3(0.012f, 0.045f, 0.75f) : float3(0.75f, 0.008f, 0.001f);
            float3 inner = ice ? float3(0.08f, 1.2f, 3.2f) : float3(4.0f, 0.65f, 0.025f);
            color = lerp(outer, inner, temperature * temperature);
            color += (ice ? float3(1.4f, 2.4f, 3.0f) : float3(5.0f, 2.2f, 0.35f)) *
                     pow(temperature, 8.0f);
        }
        radiance += transmittance * opacity * color;
        transmittance *= 1.0f - opacity;
        if (transmittance < 0.015f) break;
    }
    float alpha = 1.0f - transmittance;
    float distance_to_volume = distance(input.volume_center, Eye.xyz);
    float fog = saturate((distance_to_volume - LightAndFogStart.w) /
                         max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    return float4(lerp(radiance, FogAndEnd.rgb * alpha, fog), alpha);
}

float4 main(PixelInput input) : SV_TARGET
{
    if (input.light_data.y <= -1.5f)
        return volume_effect(input);
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
    if (AmbientSun.w > 0.5f)
        fog = fog * fog * (3.0f - 2.0f * fog);
    float hell_depth = Eye.w * smoothstep(24.0f, 36.0f, -Eye.y);
    float hell_air = hell_depth * (1.0f - exp(-horizontal_distance * 0.025f)) * 0.38f;
    fog = max(fog, hell_air);
    fog = min(fog, 0.55f);
    float3 surface = texel.rgb * input.color.rgb;
#if defined(OPENMINERZ_EMISSIVE)
    // Additive spans are light sources: flames, sparks, muzzle and explosion
    // flashes. In the Enhanced presentation's scene light they have to be
    // brighter than a sunlit wall, or they would not bloom like one.
    if (AmbientSun.w > 0.5f)
        surface *= 2.4f;
#endif
    float3 lit = lerp(surface, FogAndEnd.rgb, fog);
    return float4(lit * alpha, alpha);
}
)hlsl";
        const auto vertex_blob = compile_shader(entity_vertex_source, "main", "vs_4_0");
        const auto sun_shadow_vertex_blob =
            compile_shader(entity_sun_shadow_vertex_source, "main", "vs_4_0");
        const auto point_shadow_vertex_blob =
            compile_shader(entity_point_shadow_vertex_source, "main", "vs_4_0");
        // The deep underground's constants and lighting functions come first
        // (hell_atmosphere.hpp), as in the terrain shader, and with the same
        // fallback to the shader exactly as it was.
        std::string lit_entity_source =
            deep_underground_shaders_wanted()
                ? HellAtmosphere::shader_library() + entity_pixel_source
                : std::string{entity_pixel_source};
        auto pixel_blob = compile_shader(lit_entity_source.c_str(), "main", "ps_5_0");
        deep_underground_creatures_ = pixel_blob != nullptr && deep_underground_shaders_wanted();
        if (pixel_blob == nullptr && deep_underground_shaders_wanted())
        {
            failure_reason_.clear();
            lit_entity_source = entity_pixel_source;
            pixel_blob = compile_shader(lit_entity_source.c_str(), "main", "ps_5_0");
        }
        const std::string alpha_lit_source =
            std::string{"#define OPENMINERZ_ALPHA_LIT 1\n"} + lit_entity_source;
        const auto alpha_lit_blob = compile_shader(alpha_lit_source.c_str(), "main", "ps_5_0");
        // The exact texts compiled, for the mod-light variants (graphics area).
        entity_pixel_text_ = lit_entity_source;
        entity_alpha_lit_text_ = alpha_lit_source;
        const auto particle_blob = compile_shader(particle_pixel_source, "main", "ps_4_0");
        // Additive spans (flames, sparks, flashes) use a variant that gives
        // them a light source's brightness in the Enhanced presentation. It is
        // optional: without it they are drawn by the plain particle shader.
        {
            const std::string emissive_source =
                std::string{"#define OPENMINERZ_EMISSIVE 1\n"} + particle_pixel_source;
            const std::string previous_failure = failure_reason_;
            const auto emissive_blob = compile_shader(emissive_source.c_str(), "main", "ps_4_0");
            if (emissive_blob == nullptr ||
                FAILED(device_->CreatePixelShader(emissive_blob->GetBufferPointer(),
                                                  emissive_blob->GetBufferSize(), nullptr,
                                                  &particle_emissive_pixel_shader_)))
            {
                particle_emissive_pixel_shader_.Reset();
                failure_reason_ = previous_failure;
            }
        }
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
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 6> entity_elements{{
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 12U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 20U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 36U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 1U, DXGI_FORMAT_R32G32_FLOAT, 0U, 48U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            // Second stream: the position of the same vertex one frame ago.
            {"PREVIOUS", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 1U, 0U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
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
        // A small procedural 3D scalar field, not a smoke/fire picture. Hardware
        // trilinear sampling replaces hundreds of hashes per ray step, keeping
        // large overlapping plumes practical at close range.
        std::vector<std::uint8_t> effect_noise(64U * 64U * 64U);
        std::uint32_t noise_state = 0x57465833U;
        for (auto &value : effect_noise)
        {
            noise_state ^= noise_state << 13U;
            noise_state ^= noise_state >> 17U;
            noise_state ^= noise_state << 5U;
            value = static_cast<std::uint8_t>(noise_state >> 24U);
        }
        D3D11_TEXTURE3D_DESC noise_description{};
        noise_description.Width = noise_description.Height = noise_description.Depth = 64U;
        noise_description.MipLevels = 1U;
        noise_description.Format = DXGI_FORMAT_R8_UNORM;
        noise_description.Usage = D3D11_USAGE_IMMUTABLE;
        noise_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA noise_data{effect_noise.data(), 64U, 64U*64U};
        Microsoft::WRL::ComPtr<ID3D11Texture3D> noise_texture;
        D3D11_SAMPLER_DESC noise_sampler{};
        noise_sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        noise_sampler.AddressU = noise_sampler.AddressV = noise_sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        noise_sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateTexture3D(&noise_description, &noise_data, &noise_texture)) ||
            FAILED(device_->CreateShaderResourceView(noise_texture.Get(), nullptr, &effect_noise_view_)) ||
            FAILED(device_->CreateSamplerState(&noise_sampler, &effect_noise_sampler_)))
        {
            failure_reason_ = "Could not create the procedural effect density field";
            return;
        }
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

    // The opaque spans of the current batch: which vertices belong to what.
    void entity_layout(std::vector<std::array<std::uintptr_t, 3>> &layout) const
    {
        layout.clear();
        for (const EntitySpan &span : entity_batch_.spans)
            if (span.blend == EntityBlend::opaque)
                layout.push_back({span.first, span.count,
                                  reinterpret_cast<std::uintptr_t>(span.texture)});
    }

    // Stream 1 of the entity pass: where each vertex was one frame ago, or the
    // current positions when last frame's buffer does not describe the same
    // vertices (then only the camera's motion is seen).
    [[nodiscard]] ID3D11Buffer *entity_previous_stream()
    {
        if (!entity_motion_vectors_ || !entity_previous_valid_ ||
            entity_previous_buffer_ == nullptr)
            return entity_vertex_buffer_.Get();
        std::vector<std::array<std::uintptr_t, 3>> layout;
        entity_layout(layout);
        return layout == entity_previous_layout_ ? entity_previous_buffer_.Get()
                                                 : entity_vertex_buffer_.Get();
    }

    // End of a frame drawn with per-object motion: this frame's vertices
    // become "one frame ago" and the older buffer is reused for the next upload.
    void retire_entity_batch()
    {
        entity_previous_valid_ = entity_motion_vectors_ && !entity_batch_.vertices.empty() &&
                                 entity_uploaded_count_ == entity_batch_.vertices.size();
        if (!entity_previous_valid_)
        {
            entity_previous_layout_.clear();
            return;
        }
        entity_layout(entity_previous_layout_);
        std::swap(entity_vertex_buffer_, entity_previous_buffer_);
        std::swap(entity_vertex_capacity_, entity_previous_capacity_);
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
        // Stream 0 is this frame's vertices, stream 1 the same vertices one
        // frame ago (or this frame's again; see entity_previous_stream).
        const std::array<ID3D11Buffer *, 2> vertex_buffers{entity_vertex_buffer_.Get(),
                                                           entity_previous_stream()};
        const std::array<UINT, 2> strides{stride, stride};
        const std::array<UINT, 2> offsets{offset, offset};
        context_->IASetVertexBuffers(0U, 2U, vertex_buffers.data(), strides.data(),
                                     offsets.data());
        ID3D11Buffer *previous_camera =
            entity_motion_vectors_ && post_processor_ != nullptr
                ? post_processor_->camera_constants()
                : nullptr;
        context_->VSSetConstantBuffers(2U, 1U, &previous_camera);
        context_->VSSetShader(entity_vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(lit_entity_shader(), nullptr, 0U);
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
            ID3D11SamplerState *mesh_sampler =
                span.repeat_texture ? aniso_sampler_state_.Get() : sampler_state_.Get();
            context_->PSSetSamplers(0U, 1U, &mesh_sampler);
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
            // Slot t8 is shared: particles read scene depth there, while the lit
            // alpha spans run the entity shader, which reads the canopy
            // transmission array the main pass bound there. Keep that view to hand
            // to alpha spans and back to the water pass afterwards.
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> main_pass_transmission;
            context_->PSGetShaderResources(8U, 1U, main_pass_transmission.GetAddressOf());
            ID3D11ShaderResourceView *scene_depth = depth_srv_.Get();
            context_->PSSetShaderResources(8U, 1U, &scene_depth);
            ID3D11ShaderResourceView *noise_view = effect_noise_view_.Get();
            ID3D11SamplerState *noise_sampler = effect_noise_sampler_.Get();
            context_->PSSetShaderResources(9U, 1U, &noise_view);
            context_->PSSetSamplers(9U, 1U, &noise_sampler);
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
                        ? lit_entity_alpha_shader()
                        : (span.blend == EntityBlend::additive &&
                                   particle_emissive_pixel_shader_ != nullptr
                               ? particle_emissive_pixel_shader_.Get()
                               : particle_pixel_shader_.Get());
                if (pixel_shader != active_pixel_shader)
                {
                    active_pixel_shader = pixel_shader;
                    context_->PSSetShader(active_pixel_shader, nullptr, 0U);
                    ID3D11ShaderResourceView *slot8 = span.blend == EntityBlend::alpha_lit
                                                          ? main_pass_transmission.Get()
                                                          : scene_depth;
                    context_->PSSetShaderResources(8U, 1U, &slot8);
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
                ID3D11SamplerState *mesh_sampler =
                    span.repeat_texture ? aniso_sampler_state_.Get() : sampler_state_.Get();
                context_->PSSetSamplers(0U, 1U, &mesh_sampler);
                context_->Draw(span.count, span.first);
            }
            // Rebinding t8 also unbinds the depth SRV before depth is writable again.
            ID3D11ShaderResourceView *restored_transmission = main_pass_transmission.Get();
            context_->PSSetShaderResources(8U, 1U, &restored_transmission);
            ID3D11ShaderResourceView *empty_noise{};
            context_->PSSetShaderResources(9U, 1U, &empty_noise);
            context_->OMSetRenderTargets(1U, &entity_target, depth_view_.Get());
            context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
            context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFFU);
        }
        // Restore the voxel pipeline so the water pass keeps working unchanged.
        ID3D11SamplerState *default_sampler = sampler_state_.Get();
        context_->PSSetSamplers(0U, 1U, &default_sampler);
        context_->IASetInputLayout(input_layout_.Get());
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(lit_terrain_shader(), nullptr, 0U);
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
    // Player quality settings (see set_quality). The "requested" values are
    // what the settings ask for; the others describe the live GPU objects.
    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, 4> composite_variants_;
    std::array<bool, 4> composite_variant_failed_{};
    const char *composite_source_{};
    UINT requested_shadow_resolution_{2048U};
    UINT shadow_resolution_{2048U};
    UINT requested_anisotropy_{8U};
    UINT anisotropy_{8U};
    float mip_bias_{};
    float shadow_distance_{150.0F};
    bool fxaa_enabled_{true};
    bool bloom_enabled_{true};
    bool sun_shafts_enabled_{true};
    // The Enhanced presentation's sky and post-processing chain; null when the
    // device could not build it, in which case the chain above is used.
    std::unique_ptr<PostProcessor> post_processor_;
    // The Enhanced presentation's deep underground (hell_atmosphere.hpp): the
    // lava map the lit shaders read, its air pass and its embers; null or not
    // ready() when the device could not build it, in which case Hell keeps the
    // older flat fill. `hell_frame_` is what it decided for the last draw.
    std::unique_ptr<HellAtmosphere> hell_;
    HellFrameState hell_frame_{};
    // The Enhanced presentation's effects (vfx/vfx_renderer.hpp); null or not
    // ready() when the device could not build it.
    std::unique_ptr<VfxRenderer> vfx_;
    // Mod graphics area: lighting props (modding/graphics/mod_light_gpu.hpp),
    // created by the first frame that has any; whether this draw uses them;
    // the exact text of each lit pixel shader, from which their mod-light
    // variants are compiled; and the atmosphere layers' fog band.
    std::unique_ptr<ModLightGpu> mod_lights_;
    bool mod_lights_active_{};
    std::string terrain_pixel_text_;
    std::string entity_pixel_text_;
    std::string entity_alpha_lit_text_;
    const modding::graphics::AtmosphereStack *mod_atmosphere_{};
    // A mod's colour grade of the finished world image (mod_output.hpp).
    const modding::graphics::Image *mod_grade_lut_{};
    std::string mod_grade_name_;
    float mod_grade_strength_{};
    std::unique_ptr<ModScreenPass> mod_grade_pass_;
    // (mod-liquid) The camera inside a mod liquid, and the liquid's colour.
    bool liquid_view_inside_{};
    std::array<float, 3> liquid_view_color_{};
    // Whether each lit shader was compiled with that model; it runs only when
    // both were.
    bool deep_underground_terrain_{};
    bool deep_underground_creatures_{};
    // The far landscape and the surface air (far_terrain_renderer.hpp); null or
    // not active() when the device could not build it or the setting is off.
    std::unique_ptr<FarTerrainRenderer> far_terrain_;
    float post_last_seconds_{-1.0F};
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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> effect_noise_view_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> effect_noise_sampler_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> particle_emissive_pixel_shader_;
    // Per-object motion for the Enhanced motion blur: last frame's entity
    // vertices are kept and bound as a second stream. They describe the same
    // vertices only while the opaque spans are laid out exactly as last frame;
    // otherwise the current buffer is bound twice and a moving creature blurs
    // with the camera alone for that frame.
    Microsoft::WRL::ComPtr<ID3D11BlendState> terrain_motion_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> entity_previous_buffer_;
    UINT entity_previous_capacity_{};
    std::vector<std::array<std::uintptr_t, 3>> entity_previous_layout_;
    bool entity_previous_valid_{};
    bool entity_motion_vectors_{};
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
    PostEffects post_effects_{};
    PostView post_view_{};
    std::array<SDL_Texture *, 6> sky_plate_first_{};
    std::array<SDL_Texture *, 6> sky_plate_second_{};
    float sky_plate_second_weight_{};
    bool drew_sky_{};

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

void DepthVoxelRenderer::install_packed_chunk(PackedChunkMesh packed)
{
    impl_->install_packed_chunk(std::move(packed));
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

void DepthVoxelRenderer::forget_entity_texture(SDL_Texture *texture) noexcept
{
    impl_->forget_entity_texture(texture);
}

void DepthVoxelRenderer::submit_dynamic_radiance(world::DynamicRadianceVolume volume)
{
    impl_->submit_dynamic_radiance(std::move(volume));
}

void DepthVoxelRenderer::submit_far_terrain(world::FarTerrainFrame frame)
{
    impl_->submit_far_terrain(std::move(frame));
}

void DepthVoxelRenderer::set_far_mod_palette(const world::ModPaletteLooks &looks,
                                             std::span<const world::BlockType> fallbacks,
                                             std::span<const world::FarSurfaceProfile> profiles)
{
    impl_->set_far_mod_palette(looks, fallbacks, profiles);
}

bool DepthVoxelRenderer::vfx_available() const noexcept
{
    return impl_->vfx_available();
}

void DepthVoxelRenderer::submit_vfx(vfx::Frame &frame, const VfxOptions &options)
{
    impl_->submit_vfx(frame, options);
}

// Mod graphics area.
void DepthVoxelRenderer::submit_mod_lights(const modding::graphics::LightFrame &frame)
{
    impl_->submit_mod_lights(frame);
}

modding::graphics::LightClusterStats DepthVoxelRenderer::mod_light_statistics()
{
    return impl_->mod_light_statistics();
}

void DepthVoxelRenderer::install_mod_tiles(const std::vector<modding::graphics::Image> &tiles)
{
    impl_->install_mod_tiles(tiles);
}

void DepthVoxelRenderer::replace_atlases(SDL_Texture *atlas, SDL_Texture *normal_spec_atlas,
                                         SDL_Texture *metal_light_atlas)
{
    impl_->replace_atlases(atlas, normal_spec_atlas, metal_light_atlas);
}

void DepthVoxelRenderer::set_mod_atmosphere(
    const modding::graphics::AtmosphereStack *stack) noexcept
{
    impl_->set_mod_atmosphere(stack);
}

void DepthVoxelRenderer::set_mod_grade(const modding::graphics::Image *lut, std::string_view name,
                                       float strength)
{
    impl_->set_mod_grade(lut, name, strength);
}

world::FarTerrainGpuStats DepthVoxelRenderer::far_terrain_stats() const noexcept
{
    return impl_->far_terrain_stats();
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

void DepthVoxelRenderer::set_post_effects(const PostEffects &effects) noexcept
{
    impl_->set_post_effects(effects);
}

void DepthVoxelRenderer::set_post_view(const PostView &view) noexcept
{
    impl_->set_post_view(view);
}

void DepthVoxelRenderer::set_sky_plates(const std::array<SDL_Texture *, 6> &first,
                                        const std::array<SDL_Texture *, 6> &second,
                                        float second_weight) noexcept
{
    impl_->set_sky_plates(first, second, second_weight);
}

bool DepthVoxelRenderer::draws_sky() const noexcept
{
    return impl_->draws_sky();
}

void DepthVoxelRenderer::draw_lens(float elapsed_seconds)
{
    impl_->draw_lens(elapsed_seconds);
}

void DepthVoxelRenderer::draw_mod_grade()
{
    impl_->draw_mod_grade();
}

void DepthVoxelRenderer::set_liquid_view(bool inside, std::array<float, 3> color) noexcept
{
    impl_->set_liquid_view(inside, color);
}

bool DepthVoxelRenderer::deep_light_model() const noexcept
{
    return impl_->deep_light_model();
}

float DepthVoxelRenderer::lava_light_at(float x, float y, float z, float normal_y) const noexcept
{
    return impl_->lava_light_at(x, y, z, normal_y);
}

void DepthVoxelRenderer::set_quality(int shadow_resolution, float shadow_distance, int anisotropy,
                                     bool fxaa, bool bloom, bool sun_shafts) noexcept
{
    impl_->set_quality(shadow_resolution, shadow_distance, anisotropy, fxaa, bloom, sun_shafts);
}

bool DepthVoxelRenderer::draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
                              const world::PlayerState &player, float vertical_fov_degrees,
                              float render_scale, float elapsed_seconds, float visibility_distance,
                              [[maybe_unused]] world::BiomeKind biome, bool water_enabled,
                              bool enhanced_foliage, bool ambient_occlusion, bool ore_glow,
                              float shadow_strength)
{
    return impl_->draw(metrics, sky, player, vertical_fov_degrees, render_scale, elapsed_seconds,
                       visibility_distance, biome, water_enabled, enhanced_foliage,
                       ambient_occlusion, ore_glow, shadow_strength);
}

} // namespace openminerz
