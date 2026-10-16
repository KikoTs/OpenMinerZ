// The world renderer's side of the modding graphics service
// (openminerz/modding/graphics.hpp): glTF props and creatures drawn with the
// creatures, lighting props handed to the lit passes, mod block tiles and the
// light mods put on the first-person body. Member functions of
// WorldPreviewRenderer, kept here with the rest of the graphics area.
//
// PRESENTATION ONLY. With no service set, or a service no mod has used, every
// function here returns at once and nothing is submitted.

#include "world_preview_renderer.hpp"

#include "avatar_viewmodel.hpp" // (Animation packs) reload_animation_pack
#include "depth_voxel_renderer.hpp"
#include "modding/graphics/mod_light_gpu.hpp"
#include "modding/graphics/mod_model_batch.hpp"
#include "modding/graphics/mod_output.hpp"
#include "modding/graphics/mod_surfaces.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <numbers>
#include <unordered_map>

namespace openminerz
{

namespace graphics = modding::graphics;

struct WorldPreviewRenderer::ModState final
{
    struct MaterialTexture final
    {
        std::shared_ptr<const graphics::Model> model;
        SDL_Texture *texture{};
    };
    struct LightSample final
    {
        float sun{1.0F};
        float torch{};
        bool sampled{};
    };
    // Model material -> SDL texture, keyed by material address; the model is
    // held so the address stays valid.
    std::map<const graphics::ModelMaterial *, MaterialTexture> textures;
    // (Polish) The same for emissive textures.
    std::map<const graphics::ModelMaterial *, MaterialTexture> emissive_textures;
    std::unordered_map<std::uint64_t, LightSample> light_samples;
    std::size_t light_cursor{};
    std::vector<graphics::Matrix4> world;
    std::vector<graphics::Matrix4> skin;
    graphics::LightFrame frame;
    // The lights of the last submitted frame, for the first-person body.
    std::vector<graphics::GpuLight> cpu_lights;
    bool lights_submitted{};
    // The LUT grade and fade, created by the first frame that asks for one.
    std::unique_ptr<ModScreenPass> screen;
    // (Polish) The mod block tiles for chips and debris (the terrain's live
    // in a D3D array the entity pass cannot sample): debris_columns to a row,
    // debris_tile_count of them; null without mod block tiles.
    SDL_Texture *debris_atlas{};
    std::size_t debris_tile_count{};
    std::size_t debris_rows{};
    std::size_t stat_frames{};
    double stat_build_milliseconds{};

    // The world renderer's members go first (mod_state_ is declared before
    // them), so the depth renderer that cached views of these is gone.
    ~ModState()
    {
        for (auto &[material, entry] : textures)
        {
            (void)material;
            SDL_DestroyTexture(entry.texture);
        }
        for (auto &[material, entry] : emissive_textures)
        {
            (void)material;
            SDL_DestroyTexture(entry.texture);
        }
        SDL_DestroyTexture(debris_atlas);
    }
};

void WorldPreviewRenderer::ModStateDeleter::operator()(ModState *state) const noexcept
{
    delete state;
}

namespace
{

graphics::ViewFrustum camera_frustum(const world::PlayerState &camera, float vertical_fov_degrees,
                                     float aspect, float far_plane)
{
    const float sine_yaw = std::sin(camera.yaw);
    const float cosine_yaw = std::cos(camera.yaw);
    const float sine_pitch = std::sin(camera.pitch);
    const float cosine_pitch = std::cos(camera.pitch);
    return graphics::make_frustum(
        {camera.position.x, camera.position.y + world::PlayerSimulation::eye_height,
         camera.position.z},
        {sine_yaw * cosine_pitch, sine_pitch, -cosine_yaw * cosine_pitch},
        {cosine_yaw, 0.0F, sine_yaw},
        {-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch},
        vertical_fov_degrees * std::numbers::pi_v<float> / 180.0F, aspect, 0.05F, far_plane);
}

// Where an instance is this frame (following its attachment), or false.
bool instance_position(const graphics::GraphicsService &service,
                       const graphics::ModelStore::Entry &entry, graphics::Vec3 &position,
                       float &yaw_degrees)
{
    position = entry.desc.position;
    yaw_degrees = entry.desc.yaw;
    if (!entry.attachment)
        return true;
    graphics::Vec3 target{};
    float yaw{};
    if (!service.locator()(entry.attachment->target, target, yaw))
        return false;
    graphics::Vec3 offset = entry.attachment->offset;
    if (entry.attachment->rotate)
    {
        offset = {offset.x * std::cos(yaw) - offset.z * std::sin(yaw), offset.y,
                  offset.x * std::sin(yaw) + offset.z * std::cos(yaw)};
        // (Visuals) the model's +Z (glTF's front) turns to the target's front,
        // as a creature's does.
        yaw_degrees += 180.0F - yaw * 180.0F / std::numbers::pi_v<float>;
    }
    position = target + offset;
    return true;
}

} // namespace

void WorldPreviewRenderer::set_mod_graphics(modding::graphics::GraphicsService *service) noexcept
{
    mod_graphics_ = service;
    if (depth_renderer_ != nullptr)
        depth_renderer_->set_mod_atmosphere(service != nullptr ? &service->atmosphere() : nullptr);
    // (GunFX) the client mods' weapon parts, clip cues and viewmodel motion.
    if (avatar_viewmodel_ != nullptr)
        avatar_viewmodel_->set_gunfx(service);
}

WorldPreviewRenderer::ModState &WorldPreviewRenderer::mod_state()
{
    if (mod_state_ == nullptr)
        mod_state_.reset(new ModState{});
    return *mod_state_;
}

SDL_Texture *WorldPreviewRenderer::mod_material_texture(
    const std::shared_ptr<const graphics::Model> &model, const graphics::ModelMaterial &material)
{
    if (material.base_color.empty())
        return nullptr;
    ModState &state = mod_state();
    auto found = state.textures.find(&material);
    if (found == state.textures.end())
    {
        SDL_Texture *created = nullptr;
        if (SDL_Surface *surface = surface_from_image(material.base_color))
        {
            created = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_DestroySurface(surface);
            if (created != nullptr)
            {
                SDL_SetTextureScaleMode(created, SDL_SCALEMODE_LINEAR);
                SDL_SetTextureBlendMode(created, SDL_BLENDMODE_NONE);
            }
        }
        found = state.textures.emplace(&material, ModState::MaterialTexture{model, created}).first;
    }
    return found->second.texture;
}

SDL_Texture *WorldPreviewRenderer::mod_emissive_texture(
    const std::shared_ptr<const graphics::Model> &model, const graphics::ModelMaterial &material)
{
    if (material.emissive.x <= 0.0F && material.emissive.y <= 0.0F && material.emissive.z <= 0.0F)
        return nullptr;
    if (material.emissive_texture.empty())
        return white_texture_;
    ModState &state = mod_state();
    auto found = state.emissive_textures.find(&material);
    if (found == state.emissive_textures.end())
    {
        SDL_Texture *created = nullptr;
        if (SDL_Surface *surface = surface_from_image(material.emissive_texture))
        {
            created = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_DestroySurface(surface);
            if (created != nullptr)
            {
                SDL_SetTextureScaleMode(created, SDL_SCALEMODE_LINEAR);
                SDL_SetTextureBlendMode(created, SDL_BLENDMODE_NONE);
            }
        }
        found = state.emissive_textures.emplace(&material, ModState::MaterialTexture{model, created})
                    .first;
    }
    return found->second.texture != nullptr ? found->second.texture : white_texture_;
}

bool WorldPreviewRenderer::mod_models_pending() const noexcept
{
    return mod_graphics_ != nullptr && (!mod_graphics_->models().entries().empty() ||
                                        !mod_graphics_->creatures().empty() ||
                                        mod_creatures_ != nullptr);
}

void WorldPreviewRenderer::mod_append_models(
    EntityBatch &batch, const world::PlayerState &camera, const world::SkyState &sky,
    float vertical_fov_degrees, float aspect, bool enhanced,
    const std::function<std::array<float, 2>(world::Vec3)> &sample_light)
{
    if (!mod_models_pending())
        return;
    // (Visuals) mod creatures and NPCs, then the props.
    mod_append_creatures(batch, camera, sky, vertical_fov_degrees, aspect, enhanced, sample_light);
    if (mod_graphics_->models().entries().empty())
        return;
    ModState &state = mod_state();
    graphics::GraphicsService &service = *mod_graphics_;
    const graphics::ViewFrustum frustum =
        camera_frustum(camera, vertical_fov_degrees, aspect, visibility_distance_ + 48.0F);
    const std::array<float, 3> sun_color =
        mod_model_sun_color(sky.ambient.red, sky.ambient.green, sky.ambient.blue, sky.direct.red,
                            sky.direct.green, sky.direct.blue);
    // Light at each prop refreshes a few props per frame, as for creatures.
    std::size_t refresh_budget = 6U;
    std::size_t index = 0U;
    for (const graphics::ModelStore::Entry &entry : service.models().entries())
    {
        ++index;
        if (!entry.desc.visible)
            continue;
        const auto model = service.model_cache().get(service.assets(), entry.desc.model);
        if (model == nullptr)
            continue;
        graphics::Vec3 position{};
        float yaw{};
        if (!instance_position(service, entry, position, yaw))
            continue;
        const int animation =
            entry.desc.animation.empty() ? -1 : model->find_animation(entry.desc.animation);
        const float scale = std::clamp(entry.desc.scale, 0.01F, 64.0F);
        // Animated parts may leave the bind-pose bounds.
        const float radius = model->radius * scale * (animation >= 0 ? 1.5F : 1.0F) + 0.25F;
        if (!graphics::sphere_visible(frustum, position, radius))
            continue;
        auto &sample = state.light_samples[entry.id];
        if (!sample.sampled || ((index + state.light_cursor) % 16U == 0U && refresh_budget > 0U))
        {
            if (sample.sampled)
                --refresh_budget;
            const std::array<float, 2> light = sample_light({position.x, position.y, position.z});
            sample = {light[0], light[1], true};
        }
        const ModModelLight light{enhanced, sample.sun, sample.torch, sky.daylight, sun_color};
        graphics::evaluate_pose(*model, animation, entry.time, entry.desc.loop, state.world);
        const graphics::Matrix4 instance = graphics::matrix_compose(
            position, mod_quaternion_from_euler(yaw, entry.desc.pitch, entry.desc.roll),
            {scale, scale, scale});
        for (const graphics::ModelPrimitive &primitive : model->primitives)
        {
            const graphics::ModelMaterial *material =
                primitive.material >= 0 &&
                        static_cast<std::size_t>(primitive.material) < model->materials.size()
                    ? &model->materials[static_cast<std::size_t>(primitive.material)]
                    : nullptr;
            SDL_Texture *texture = white_texture_;
            if (material != nullptr)
                if (SDL_Texture *own = mod_material_texture(model, *material))
                    texture = own;
            if (texture == nullptr)
                continue;
            const std::array<float, 4> factor =
                material != nullptr ? material->color_factor : std::array<float, 4>{1, 1, 1, 1};
            const auto first = static_cast<std::uint32_t>(batch.vertices.size());
            append_mod_primitive(batch, *model, primitive, state.world, state.skin, instance,
                                 texture,
                                 {factor[0] * entry.desc.tint.r, factor[1] * entry.desc.tint.g,
                                  factor[2] * entry.desc.tint.b, factor[3]},
                                 material != nullptr && (material->blend || material->mask)
                                     ? EntityBlend::alpha_lit
                                     : EntityBlend::opaque,
                                 light);
            // (Polish) and the light it gives off.
            if (material != nullptr)
                append_mod_emissive(batch, first, mod_emissive_texture(model, *material),
                                    material->emissive);
        }
    }
    ++state.light_cursor;
}

void WorldPreviewRenderer::mod_submit_lights(const world::PlayerState &camera,
                                             float vertical_fov_degrees, float aspect)
{
    if (depth_renderer_ == nullptr)
        return;
    // The atmosphere layers' LUT, which the depth renderer grades the world
    // image with (nothing is graded without one).
    if (mod_graphics_ != nullptr)
    {
        const graphics::ResolvedLut lut = mod_graphics_->atmosphere().lut();
        depth_renderer_->set_mod_grade(lut.strength > 0.0F ? mod_graphics_->lut(lut.name) : nullptr,
                                       lut.name, lut.strength);
    }
    else if (mod_state_ != nullptr)
        depth_renderer_->set_mod_grade(nullptr, {}, 0.0F);
    const bool any = mod_graphics_ != nullptr && mod_graphics_->lights().size() > 0U;
    if (!any)
    {
        // Turn the path off once after the last light went; nothing otherwise.
        if (mod_state_ != nullptr && mod_state_->lights_submitted)
        {
            mod_state_->frame.clear();
            mod_state_->cpu_lights.clear();
            depth_renderer_->submit_mod_lights(mod_state_->frame);
            mod_state_->lights_submitted = false;
        }
        return;
    }
    ModState &state = mod_state();
    const float reach = visibility_distance_ + 24.0F;
    const auto started = std::chrono::steady_clock::now();
    graphics::build_light_frame(mod_graphics_->lights(), mod_graphics_->clock(),
                                camera_frustum(camera, vertical_fov_degrees, aspect, reach), reach,
                                256U, mod_graphics_->locator(), state.frame);
    state.cpu_lights = state.frame.lights;
    // OPENMINERZ_MOD_GRAPHICS_STATS: what the last draw's light clusters held
    // (read back from the GPU), every 240 frames.
    static const bool stats = SDL_getenv("OPENMINERZ_MOD_GRAPHICS_STATS") != nullptr;
    if (stats)
    {
        state.stat_build_milliseconds +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (++state.stat_frames >= 240U)
        {
            const graphics::LightClusterStats clusters = depth_renderer_->mod_light_statistics();
            SDL_Log("mod.lights: %zu drawn of %zu (culled %zu) clusters=%dx%dx%d lit=%zu/%zu "
                    "references=%zu busiest=%u dropped=%zu cpu=%.3f ms/frame",
                    state.frame.lights.size(), state.frame.considered, state.frame.culled,
                    clusters.tiles_x, clusters.tiles_y, graphics::ClusterGrid::slices,
                    clusters.lit_clusters, clusters.clusters, clusters.references,
                    clusters.busiest, clusters.dropped,
                    state.stat_build_milliseconds / static_cast<double>(state.stat_frames));
            state.stat_frames = 0U;
            state.stat_build_milliseconds = 0.0;
        }
    }
    depth_renderer_->submit_mod_lights(state.frame);
    state.lights_submitted = true;
}

std::array<float, 3> WorldPreviewRenderer::mod_light_at(world::Vec3 position) const
{
    if (mod_state_ == nullptr || mod_state_->cpu_lights.empty())
        return {};
    const graphics::Color light =
        graphics::light_at(std::span<const graphics::GpuLight>{mod_state_->cpu_lights},
                           {position.x, position.y, position.z});
    return {light.r, light.g, light.b};
}

void WorldPreviewRenderer::reload_mod_textures()
{
    std::vector<SDL_Texture *> old{atlas_,
                                   normal_spec_atlas_,
                                   metal_light_atlas_,
                                   block_cracks_,
                                   block_selector_,
                                   mining_smoke_,
                                   mining_sparks_,
                                   mining_rocks_,
                                   torch_fire_texture_,
                                   flash_texture_,
                                   ice_flash_texture_,
                                   ice_puff_texture_,
                                   torch_smoke_texture_};
    atlas_ = normal_spec_atlas_ = metal_light_atlas_ = nullptr;
    block_cracks_ = block_selector_ = mining_smoke_ = mining_sparks_ = mining_rocks_ = nullptr;
    torch_fire_texture_ = flash_texture_ = ice_flash_texture_ = ice_puff_texture_ = nullptr;
    torch_smoke_texture_ = nullptr;
    for (SkyCube *cube : {&clear_sky_, &dawn_sky_, &sunset_sky_, &night_sky_})
        for (SDL_Texture *&face : *cube)
        {
            old.push_back(face);
            face = nullptr;
        }
    // (Visuals) the creature skins too, so a pack needs no new world.
    if (enemy_models_ != nullptr)
        for (SDL_Texture *texture : enemy_models_->reload_textures())
            old.push_back(texture);
    load_original_atlas();
    if (depth_renderer_ != nullptr)
    {
        for (SDL_Texture *texture : old)
            if (texture != nullptr)
                depth_renderer_->forget_entity_texture(texture);
        depth_renderer_->replace_atlases(atlas_, normal_spec_atlas_, metal_light_atlas_);
        mod_install_tiles();
    }
    for (SDL_Texture *texture : old)
        SDL_DestroyTexture(texture);
    // (Animation packs) the player's clips too: the character loaded with
    // the world, often before the session installed its packs.
    if (avatar_viewmodel_ != nullptr)
        avatar_viewmodel_->reload_animation_pack();
}

void WorldPreviewRenderer::mod_screen_effects(const ui::UiMetrics &metrics)
{
    if (mod_graphics_ == nullptr)
        return;
    // A fade is not motion: the accessibility options leave it as it is.
    const graphics::CameraOutput camera = mod_graphics_->camera().output({});
    if (camera.fade_alpha <= 0.0F)
        return;
    ModState &state = mod_state();
    if (state.screen == nullptr)
        state.screen = std::make_unique<ModScreenPass>(renderer_);
    state.screen->fade(camera.fade_color, camera.fade_alpha, metrics.output_width,
                       metrics.output_height);
}

void WorldPreviewRenderer::mod_install_tiles()
{
    if (depth_renderer_ == nullptr)
        return;
    const std::vector<graphics::Image> tiles = mod_block_tile_images();
    if (tiles.empty())
        return;
    depth_renderer_->install_mod_tiles(tiles);

    // (Polish) The same tiles for chips: 16 to a row, 32 texels square (a
    // chip shows a quarter of one at a few pixels), an unreadable one magenta
    // as on the terrain.
    constexpr int size = 32;
    constexpr std::size_t columns = 16U;
    const std::size_t rows = (tiles.size() + columns - 1U) / columns;
    graphics::Image atlas;
    atlas.width = static_cast<int>(columns) * size;
    atlas.height = static_cast<int>(rows) * size;
    atlas.rgba.assign(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height) *
                          4U,
                      0U);
    for (std::size_t index = 0U; index < tiles.size(); ++index)
    {
        const graphics::Image &tile = tiles[index];
        const int left = static_cast<int>(index % columns) * size;
        const int top = static_cast<int>(index / columns) * size;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                std::uint8_t *out = &atlas.rgba[(static_cast<std::size_t>(top + y) *
                                                     static_cast<std::size_t>(atlas.width) +
                                                 static_cast<std::size_t>(left + x)) *
                                                4U];
                if (tile.empty())
                {
                    out[0] = 255U;
                    out[1] = 0U;
                    out[2] = 255U;
                    out[3] = 255U;
                    continue;
                }
                const std::size_t source =
                    (static_cast<std::size_t>(y * tile.height / size) *
                         static_cast<std::size_t>(tile.width) +
                     static_cast<std::size_t>(x * tile.width / size)) *
                    4U;
                std::copy_n(&tile.rgba[source], 4U, out);
            }
    }
    SDL_Texture *created = nullptr;
    if (SDL_Surface *surface = surface_from_image(atlas))
    {
        created = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (created != nullptr)
        {
            SDL_SetTextureScaleMode(created, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(created, SDL_BLENDMODE_NONE);
        }
    }
    ModState &state = mod_state();
    if (state.debris_atlas != nullptr)
    {
        depth_renderer_->forget_entity_texture(state.debris_atlas);
        SDL_DestroyTexture(state.debris_atlas);
    }
    state.debris_atlas = created;
    state.debris_tile_count = created != nullptr ? tiles.size() : 0U;
    state.debris_rows = rows;
}

std::uint8_t WorldPreviewRenderer::mod_debris_tile(int x, int y, int z, world::BlockType block,
                                                   world::FaceDirection face) const
{
    const bool mod_tiles = mod_state_ != nullptr && mod_state_->debris_atlas != nullptr;
    // (fix) A generated mod block's chips wear its tiles too (mod-world's
    // palette); a placed mod block in the cell comes first.
    if (mod_tiles && (mod_block_overlay_ == nullptr || mod_block_overlay_->find(x, y, z, block) == nullptr))
        if (const auto tile = generated_mod_tile(x, y, z, block, face))
            return *tile;
    return graphics::debris_tile(mod_block_overlay_.get(), x, y, z, block, face, mod_tiles);
}

SDL_Texture *WorldPreviewRenderer::mod_debris_atlas(std::uint8_t tile,
                                                    std::array<float, 4> &rectangle) const
{
    if (mod_state_ == nullptr || mod_state_->debris_atlas == nullptr ||
        tile < graphics::BlockTextureTable::first_mod_tile)
        return nullptr;
    const auto index = static_cast<std::size_t>(tile - graphics::BlockTextureTable::first_mod_tile);
    if (index >= mod_state_->debris_tile_count)
        return nullptr;
    const float width = 1.0F / 16.0F;
    const float height = 1.0F / static_cast<float>(mod_state_->debris_rows);
    rectangle = {static_cast<float>(index % 16U) * width,
                 static_cast<float>(index / 16U) * height, width, height};
    return mod_state_->debris_atlas;
}

} // namespace openminerz
