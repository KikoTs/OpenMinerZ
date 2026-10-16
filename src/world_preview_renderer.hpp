#pragma once

#include "depth_voxel_renderer.hpp"
#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/player_nametag.hpp"
#include "openminerz/view_projection.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/far_terrain.hpp"
#include "openminerz/world/liquid.hpp" // mod-liquid
#include "openminerz/world/pickup_overflow_queue.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/terrain_window.hpp"
#include "openminerz/world/world.hpp"
#include "openminerz/world_save.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openminerz
{

class DepthVoxelRenderer;
class AvatarViewmodel;
namespace xbox_avatar
{
struct Skin;
}
class StaticModelAsset;
namespace modding::graphics
{
struct IconModel; // Modding, visuals area (openminerz/modding/icon_model.hpp)
struct ItemLook;     // (Items) openminerz/modding/held_item.hpp
struct HeldItemPose; // (Items)
struct PosedItemPart;
struct ItemPoseScratch;
}

struct WorldPreviewStats final
{
    std::int32_t seed{};
    std::size_t chunk_count{};
    std::size_t visible_face_count{};
    bool original_atlas_loaded{};
    bool original_skies_loaded{};
    bool depth_buffered{};
    bool original_avatar_loaded{};
    bool original_avatar_world_animations_loaded{};
    std::size_t avatar_animation_clip_count{};
    std::size_t avatar_hand_triangles{};
    std::size_t mining_particle_count{};
    std::size_t block_pickup_count{};
    std::size_t stream_serial{};
    bool stream_pending{};
    float last_stream_milliseconds{};
    // Block-edit remesh telemetry (cumulative): worker jobs launched, sections
    // they rebuilt, the deepest queue, the slowest main-thread pump, and how
    // long an edited section waited for a mesh that contains its edit.
    std::size_t edit_mesh_jobs{};
    std::size_t edit_mesh_sections{};
    std::size_t edit_mesh_peak_queue{};
    // How deep the rebuild queue is right now, which is what a sustained edit
    // backlog actually looks like.
    std::size_t edit_mesh_queue_depth{};
    float edit_mesh_peak_pump_milliseconds{};
    float edit_mesh_peak_latency_milliseconds{};
    float edit_mesh_total_latency_milliseconds{};
    std::size_t edit_mesh_latency_samples{};
    // Main-thread terrain streaming cost, split so a walking hitch can be
    // attributed: installing a newly streamed resident window, and installing
    // finished section meshes (vertex packing plus the GPU buffer upload).
    std::size_t region_install_count{};
    float region_install_total_milliseconds{};
    float region_install_peak_milliseconds{};
    std::size_t mesh_install_sections{};
    float mesh_install_total_milliseconds{};
    float mesh_install_peak_milliseconds{};
    // Sections finished by a worker but not yet handed to the GPU because the
    // per-frame install budget ran out, and the deepest that backlog grew.
    std::size_t mesh_install_deferred{};
    std::size_t mesh_install_peak_deferred{};
    // Light-influence probe: how many apron sections the exact "did this
    // section's light actually change" test admitted, against how many the
    // geometric apron would have queued.
    std::size_t light_probe_jobs{};
    std::size_t light_probe_sections{};
    std::size_t light_probe_candidate_sections{};
    float light_probe_peak_milliseconds{};
    std::filesystem::path atlas_path;
};

struct CollectedBlockStack final
{
    InventoryStack stack;
    world::Vec3 position;
    std::optional<std::int32_t> pickup_id;
    std::int32_t spawner_id{-1};
    bool display_on_pickup{};
};

struct SpawnedNetworkPickup final
{
    InventoryStack stack;
    world::Vec3 position;
    world::Vec3 velocity;
    std::int32_t pickup_id{};
    bool dropped{};
    bool display_on_pickup{};
};

struct RemotePlayerVisual final
{
    std::uint8_t gid{};
    InventoryItemId carried_item{InventoryItemId::bare_hands};
    world::Vec3 position;
    world::Vec3 velocity;
    float yaw{};
    float torso_pitch{};
    float movement_x{};
    float movement_y{};
    float animation_seconds{};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
    // Every animation channel of this player as the original's
    // PlayerUpdateMessage.Apply -> Player.UpdateAnimation leaves it (the
    // fields above are the raw flags it was computed from).
    world::PlayerAnimationPose animation{};
    bool animation_valid{};
    // (Items) The mod item in this player's hand, drawn instead of
    // `carried_item`'s model (null: none, and every unmodded session).
    std::shared_ptr<const modding::graphics::HeldItemPose> mod_item;
};

// Which explosive an explosion effect belongs to. PRESENTATION ONLY: it picks
// how the blast LOOKS in the Enhanced presentation (openminerz/vfx.hpp) and is
// read by nothing that decides damage, radius or which blocks break. `fireball`
// is a dragon's, which keeps its own recipe; the Original presentation draws
// the same original recipe for every style.
enum class ExplosionStyle : std::uint8_t
{
    fireball,
    tnt,
    c4,
    grenade,
    rocket,
};

class WorldPreviewRenderer final
{
  public:
    WorldPreviewRenderer(SDL_Renderer *renderer, std::int32_t seed, int view_distance,
                         int initial_biome = 0,
                         world::TerrainProfile terrain_profile = world::TerrainProfile::classic,
                         GameMode game_mode = GameMode::endurance);
    ~WorldPreviewRenderer();

    WorldPreviewRenderer(const WorldPreviewRenderer &) = delete;
    WorldPreviewRenderer &operator=(const WorldPreviewRenderer &) = delete;

    void draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
              const world::PlayerState &player, float vertical_fov_degrees, float elapsed_seconds,
              float render_scale, float camera_bob, float screen_shake, float viewmodel_fov_degrees,
              bool rebirth_presentation, bool reduced_motion, bool inspector_view,
              bool ambient_occlusion, bool ore_glow, float shadow_strength);
    [[nodiscard]] const WorldPreviewStats &stats() const noexcept;
    [[nodiscard]] const world::WorldRegion &region() const noexcept;
    [[nodiscard]] world::WorldRegion &region() noexcept;
    [[nodiscard]] world::BiomeKind biome() const noexcept;
    void rebuild_mesh();
    void rebuild_block_edit(int world_x, int world_y, int world_z);
    void rebuild_block_edits(const std::vector<std::array<int, 3>> &edits);
    // Same, with what each cell held before the caller changed it. That lets
    // the shading-apron probe compare the exact previous light field instead of
    // bracketing it, which is several times tighter for bulk destruction.
    void rebuild_block_edits(const std::vector<std::array<int, 3>> &edits,
                             const std::vector<world::BlockType> &previous_blocks);
    [[nodiscard]] bool preload_step(std::size_t mesh_budget);
    [[nodiscard]] bool preload_complete() const noexcept;
    [[nodiscard]] float preload_progress() const noexcept;
    [[nodiscard]] bool stream_around(const world::PlayerState &player);
    // Teleport waits for terrain to recenter in the original. This synchronous
    // install provides the same safe handoff before player physics resumes.
    void recenter_on(const world::PlayerState &player);
    // The view distance decides what is MESHED AND DRAWN, never what exists.
    void set_view_distance(int view_distance);
    // Where the simulated terrain sits: the shipped game's fixed 24 x 24 chunk
    // BlockTerrain window around the local player, whatever the view distance.
    // `region()` holds exactly these columns (give or take the frames a newly
    // scrolled row takes to generate), and everything the original gates on
    // BlockTerrain.RegionIsLoaded asks this.
    [[nodiscard]] const world::StockTerrainWindow &simulation_window() const noexcept
    {
        return simulation_window_;
    }
    // BlockTerrain.RegionIsLoaded for this client: inside the window AND the
    // column is actually resident.
    [[nodiscard]] bool region_loaded(float world_x, float world_z) const noexcept;
    void remember_edit(int world_x, int world_y, int world_z, world::BlockType block);
    [[nodiscard]] std::optional<world::BlockType> remembered_block(int world_x, int world_y,
                                                                   int world_z) const noexcept;
    // Resolve loaded cells, durable edits, or an untouched generated baseline.
    // The host uses this for validation even when a remote player is outside
    // the host camera's streamed window.
    [[nodiscard]] std::optional<world::BlockType> resolved_block(int world_x, int world_y,
                                                                 int world_z) const;
    // World persistence: every remembered block edit as {x, y, z, block}.
    [[nodiscard]] std::vector<std::array<int, 4>> exported_edits() const;
    void import_edits(const std::vector<std::array<int, 4>> &edits);
    // Replace one complete 16x128x16 delta column with a terrain authority's
    // latest snapshot. This is deliberately atomic from the renderer's point
    // of view: stale remembered edits are removed before the regenerated base
    // column and its authoritative deltas become visible.
    void replace_chunk_edits(int corner_x, int corner_z,
                             const std::vector<std::array<int, 4>> &edits);
    void spawn_mining_impact(const world::BlockBreakUpdate &impact);
    void set_particle_density(float density) noexcept;
    // How far behind the live simulation state the next draw sits, in seconds
    // (fixed-tick interpolation). Particles and pickups are drawn that far
    // back along their own motion so they share the camera's timeline. This
    // only affects drawing; update_effects() never sees it.
    void set_presentation_lag(float seconds) noexcept;
    // Tone curve index (0 legacy, 1 Reinhard, 2 filmic, 3 ACES), auto
    // exposure, manual EV trim and colour-grading strength.
    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept;
    // Graphics-quality settings for the depth renderer: sun-shadow map size
    // (texels per side) and reach (blocks), terrain anisotropic samples, and
    // the edge-smoothing, bloom and sun-shaft post effects.
    void set_quality(int shadow_resolution, float shadow_distance, int anisotropy, bool fxaa,
                     bool bloom, bool sun_shafts) noexcept;
    // The Enhanced presentation's sky and post-processing chain
    // (resolve_post_effects(settings)). Presentation only; a disabled set, which
    // is what the Original presentation resolves to, draws exactly as before.
    void set_post_effects(const PostEffects &effects) noexcept;
    // Far terrain (openminerz/world/far_terrain.hpp): the landscape beyond the
    // detailed window, drawn cheaply and never simulated. A distance of zero
    // turns it off; it also draws nothing in the Original presentation.
    // Presentation only: region(), region_loaded() and the simulation window
    // are untouched by it.
    void set_far_terrain(const world::FarTerrainSettings &settings);
    [[nodiscard]] world::FarTerrainStats far_terrain_stats() const;
    [[nodiscard]] world::FarTerrainGpuStats far_terrain_gpu_stats() const;
    // True when far terrain is off or has finished arriving for the last
    // camera (a diagnostic screenshot waits for this).
    [[nodiscard]] bool far_terrain_settled() const noexcept;
    // How far the held weapon is raised to the eye for the next draw (0 hip ..
    // 1 aiming), which opens the depth-of-field lens. Presentation only.
    void set_aim_progress(float progress) noexcept
    {
        aim_progress_ = progress;
    }
    // Authored SmokeEffect at a hit point. TracerManager does not recolor it
    // by enemy type; the tint parameter remains useful to other effect paths.
    void spawn_impact_smoke(world::Vec3 position, const std::array<float, 3> &tint);
    // TracerManager terrain contact: SparksEffect + SmokeEffect, with none of
    // the mining burst's block-colored rock debris.
    void spawn_tracer_impact(const world::BlockBreakUpdate &impact);
    // The original FlashEffect: a handful of huge additive flashes.
    void spawn_flash_effect(world::Vec3 position, std::array<float, 3> tint = {1.0F, 1.0F, 1.0F});
    // BlasterShot's distinct contact recipe: BlasterFlash at LocalScale .01,
    // SparksEffect, and untinted SmokeEffect (no mining-rock fragments).
    void spawn_blaster_impact(const world::BlockBreakUpdate &impact,
                              std::array<float, 3> tint = {1.0F, 0.2F, 0.04F});
    // FireballEntity.Detonate: FlashEffect + FirePuff + BigSmokePuff +
    // BigRockBlast, exactly one emitter each at the impact point.
    void spawn_fireball_explosion(world::Vec3 position, bool ice, bool rock_chunks = true,
                                  ExplosionStyle style = ExplosionStyle::fireball);
    // --- The Enhanced presentation's effects layer (openminerz/vfx.hpp, which
    // explains how to add an effect). Presentation only. In the Original
    // presentation `enhanced_effects()` is false, `spawn_effect` does nothing
    // and returns an invalid handle, and `effects()` is null: callers keep the
    // original particle recipes for it.
    [[nodiscard]] bool enhanced_effects() const noexcept;
    vfx::EffectHandle spawn_effect(const vfx::EffectDesc &desc, const vfx::SpawnParams &params);
    // For effects that follow something or are stopped early (System::move,
    // set_direction, set_intensity, kill).
    [[nodiscard]] vfx::System *effects() noexcept;
    // --- Weapon, dragon, mining and creature effects of the Enhanced
    // presentation (openminerz/vfx_combat.hpp, world_preview_combat_vfx.cpp).
    // Presentation only: each reports something that has ALREADY happened,
    // where it happened. In the Original presentation they do nothing, or run
    // the original recipe where the original has one (noted on each).
    // A gun fired. `laser` weapons flash in `bolt_color` and leave no smoke;
    // `shotgun` is the scatter weapons. With no `world_muzzle` the shot is the
    // local player's and the flash sits on the muzzle the viewmodel last drew.
    // (Original: nothing; it has no muzzle flash.)
    void spawn_gun_flash(InventoryItemId item, bool laser, bool shotgun,
                         const std::array<float, 3> &bolt_color,
                         std::optional<world::Vec3> world_muzzle, world::Vec3 direction,
                         world::Vec3 shooter_velocity);
    // A block set down. (Original: ProcessDigMessage's burst, as retail.)
    void spawn_block_placed(const world::BlockBreakUpdate &placement);
    // A bullet landing on a creature or a player. (Original: SmokeEffect.)
    void spawn_creature_hit(world::Vec3 position);
    // A creature killed, something clawing out of the ground, a player or
    // creature appearing out of thin air, a blade on something hard, a foot on
    // dry ground. (Original: nothing.)
    void spawn_creature_death(world::Vec3 feet);
    void spawn_ground_burst(world::Vec3 feet);
    void spawn_arrival(world::Vec3 feet);
    void spawn_melee_sparks(world::Vec3 point, world::Vec3 normal,
                            const std::array<float, 3> *blade_color = nullptr);
    void spawn_footstep_dust(world::Vec3 feet, world::Vec3 velocity);
    void spawn_block_pickup(const world::BlockBreakUpdate &mined);
    void spawn_dropped_stack(const InventoryStack &stack, const world::PlayerState &player);
    // PickupManager.CreateUpwardPickup: non-player drops scatter along a
    // normalized random XZ/0.1 vector. Callers supply the source's exact speed
    // (ordinary/enemy drops 1.5, crate ejection 3.0).
    void spawn_dropped_stack_at(const InventoryStack &stack, world::Vec3 position,
                                float upward_speed);
    // PickupManager.CreatePickup(item, location, false, false), used by
    // InventoryItem.CreatesWhenDug for model-backed custom blocks such as doors.
    void spawn_created_stack_at(const InventoryStack &stack, world::Vec3 position);
    // PossibleLootType.PlaceWorldItem routes through CreateUpwardPickup at
    // speed 1.5, with dropped=false and the pickup notification enabled.
    void spawn_loot_stack_at(const InventoryStack &stack, world::Vec3 position,
                             world::Vec3 velocity);
    void set_network_pickups_enabled(bool enabled, std::int32_t local_spawner_id = -1) noexcept;
    [[nodiscard]] std::vector<SpawnedNetworkPickup> drain_spawned_network_pickups(
        std::int32_t spawner_id);
    [[nodiscard]] bool spawn_network_pickup(const InventoryStack &stack, world::Vec3 position,
                                            world::Vec3 velocity, std::int32_t pickup_id,
                                            std::int32_t spawner_id, bool dropped,
                                            bool display_on_pickup = false);
    [[nodiscard]] std::optional<CollectedBlockStack> find_network_pickup(
        std::int32_t pickup_id, std::int32_t spawner_id) const;
    [[nodiscard]] std::optional<CollectedBlockStack> consume_network_pickup(
        std::int32_t pickup_id, std::int32_t spawner_id);
    void clear_network_pickup_request(std::int32_t pickup_id, std::int32_t spawner_id) noexcept;
    // Enemies rendered by the next draw; the pointer must outlive the frame.
    void set_enemies(const std::vector<world::Enemy> *enemies) noexcept;
    void set_remote_players(const std::vector<RemotePlayerVisual> *players) noexcept;
    // The first-person camera the last draw() actually used (after view bob,
    // recoil shake and the death camera), for screen-space overlays that must
    // stay glued to a world point such as player nametags.
    [[nodiscard]] const NametagCamera &overlay_camera() const noexcept
    {
        return overlay_camera_;
    }
    // The held gun's muzzle as the last draw() put it on screen, in the
    // VIEWMODEL pass's own camera (it has its own field of view; see
    // AvatarViewmodel::draw). Empty when that frame drew no first-person gun
    // (dead, scoped, hidden, or not holding one). Presentation only.
    [[nodiscard]] std::optional<view::ViewmodelPoint> first_person_muzzle() const noexcept
    {
        return first_person_muzzle_;
    }
    void forget_remote_player(std::uint8_t gid) noexcept;
    void clear_player_animations() noexcept;
    // The Xbox avatar one player wears in place of the stock body, or nullptr
    // for the stock body (AvatarViewmodel::set_player_skin). `gid` is a remote
    // player's gamer id, or 256 for the local player. Presentation only; callers
    // may restate it every frame.
    void set_player_avatar(std::uint16_t gid, std::shared_ptr<const xbox_avatar::Skin> skin);
    [[nodiscard]] bool player_avatar_applied(std::uint16_t gid) const noexcept;
    // Completes every pending avatar upload now instead of over the following
    // frames. For captures taken on a known frame only.
    void finish_player_avatars();
    void set_dragon(const world::Dragon *dragon) noexcept;
    // WorldInfo's lower-voxel door-model table; owned by the application and
    // kept stable for the renderer lifetime.
    void set_doors(const WorldSaveDoorMap *doors) noexcept;
    // World-space projectile streaks (bullet tracers, arrows, fireballs)
    // drawn depth-tested with the entities; replaced every frame.
    void set_projectiles(std::vector<ProjectileVisual> projectiles) noexcept;
    void set_rockets(std::vector<RocketVisual> rockets) noexcept;
    void set_explosive_fuses(std::vector<ExplosiveFuseVisual> fuses) noexcept;
    void set_fireball_visuals(std::vector<FireballVisual> fireballs) noexcept;
    [[nodiscard]] EnemyModelLibrary *enemy_models() noexcept;
    // The imported 8x8 terrain atlas (nullptr until derived data is present);
    // the inventory UI samples it for real block icons.
    [[nodiscard]] SDL_Texture *terrain_atlas() const noexcept
    {
        return atlas_;
    }
    // `player_alive` is Player.ValidLivingGamer: a dead player collects nothing.
    [[nodiscard]] std::vector<CollectedBlockStack> update_effects(float delta_seconds,
                                                                  world::Vec3 player_position,
                                                                  bool player_alive = true);
    void cycle_biome(int direction);
    // Jump straight to a biome band (0 classic .. 5 descent), then stream the
    // complete configured play window around that band.
    void set_biome(int index);
    [[nodiscard]] std::string_view biome_name() const noexcept;
    // The Lagoon water enhancement (retail_parity.hpp). Retail has no water,
    // so this is false unless the application's gate says the enhancement is in
    // effect; false draws no water plane, no underwater tint and no swimming
    // pose for other players.
    void set_water_enabled(bool enabled) noexcept
    {
        water_extra_enabled_ = enabled;
    }
    // Modding, visuals area (openminerz/modding/icon_model.hpp): how a mod
    // item looks in 3D - its icon as a solid sprite - for a dropped pickup of
    // it and for the first-person hand holding it. Presentation only. Unset
    // (the default, and every unmodded run) draws every pickup and held item
    // exactly as before; a source that has no model for an item does too.
    // (Items) A look is the item's glTF model or its icon
    // (openminerz/modding/held_item.hpp); the hand gets the whole held item
    // (grip, hold style, the item's clips), as do remote players
    // (RemotePlayerVisual::mod_item) and the local body seen from outside.
    using ModItemModels = std::function<std::shared_ptr<const modding::graphics::ItemLook>(
        std::uint32_t mod_item)>;
    void set_mod_item_models(ModItemModels models)
    {
        mod_item_models_ = std::move(models);
    }
    void set_held_mod_item(std::shared_ptr<const modding::graphics::HeldItemPose> pose) noexcept
    {
        held_mod_item_ = std::move(pose);
    }
    // (GunFX) Your current reload started with an empty magazine: the
    // viewmodel plays a pack's "<Clip>Empty" reload. Presentation only.
    void set_gunfx_reload_empty(bool empty) noexcept
    {
        gunfx_reload_empty_ = empty;
    }
    [[nodiscard]] int center_x() const noexcept;
    [[nodiscard]] int center_z() const noexcept;
    // --- Mods (graphics area: openminerz/modding/graphics.hpp, member
    // functions in src/modding/graphics/world_preview_mod.cpp). Presentation
    // only. The service's glTF models are drawn with the creatures, its lights
    // reach the lit passes and the first-person body, and its atmosphere
    // layers' fog band blends over the renderer's. Null (the default), or a
    // service no mod has used, draws exactly as before.
    void set_mod_graphics(modding::graphics::GraphicsService *service) noexcept;
    // The installed texture packs changed: the terrain atlases (with their
    // tile overrides and mod block tiles), the sky faces and the particle
    // sprites are loaded again, and the creature skins.
    void reload_mod_textures();
    // (Visuals) The mod blocks this client draws (null: none). The sections
    // whose faces change are meshed again; every other mesh is left alone.
    void set_mod_block_overlay(std::shared_ptr<const modding::graphics::ModBlockOverlay> overlay);
    // --- mod-world: a server or core mod's world generator
    // (openminerz/world/custom_terrain.hpp; null: retail; member functions in
    // src/modding/worldgen/world_preview_terrain.cpp). Installing one
    // regenerates the resident window, the far terrain and every mesh from it.
    void set_custom_terrain(std::shared_ptr<const world::CustomTerrain> terrain);
    [[nodiscard]] const world::WorldGenerator &generator() const noexcept
    {
        return generator_;
    }
    // How the generated mod blocks are drawn (null: as their fallbacks). The
    // sections that hold any are meshed again.
    void set_mod_palette(std::shared_ptr<const world::ModPaletteLooks> looks);
    // The palette index of the generated mod block in a cell (0: none);
    // `before_change`: what the cell held right before its last change.
    [[nodiscard]] std::uint8_t generated_mod_cell(int x, int y, int z, bool before_change) const;
    // The tile a face of the generated mod block standing in a cell (or that
    // stood there until `block` was dug out of it) shows; nullopt for a
    // retail block or one drawn as its fallback. What its chips wear.
    [[nodiscard]] std::optional<std::uint8_t> generated_mod_tile(int x, int y, int z,
                                                                 world::BlockType block,
                                                                 world::FaceDirection face) const;
    // (Visuals) The block registry gave mod blocks new textures: the terrain
    // arrays get their tiles (BlockTextureTable) again.
    void install_mod_block_tiles()
    {
        mod_install_tiles();
    }
    // --- (mod-liquid) Liquid mod blocks (openminerz/world/liquid.hpp; member
    // functions in src/world_preview_liquid.cpp). The table the terrain is
    // meshed with (null: none, the retail mesher exactly); the sections
    // whose liquid cells changed are meshed again.
    void set_liquids(std::shared_ptr<const world::LiquidTable> table);
    // Whether a cell holds a liquid, inside the terrain window or beyond it
    // (the generated baseline resolved_block reads; edits replace it).
    [[nodiscard]] bool resolved_liquid(int x, int y, int z) const;
    // The liquid a player's eyes are under the surface of, or null.
    [[nodiscard]] const world::LiquidTable::Kind *eye_liquid(const world::PlayerState &player) const;

  private:
    struct ModState;
    struct ModStateDeleter final
    {
        void operator()(ModState *state) const noexcept;
    };
    std::unique_ptr<ModState, ModStateDeleter> mod_state_;
    // (Visuals) per-creature animation, light and hit-flash state, and the
    // creature skins (world_preview_creatures.cpp).
    struct ModCreatures;
    struct ModCreaturesDeleter final
    {
        void operator()(ModCreatures *state) const noexcept;
    };
    std::unique_ptr<ModCreatures, ModCreaturesDeleter> mod_creatures_;
    modding::graphics::GraphicsService *mod_graphics_{};
    bool gunfx_reload_empty_{}; // (GunFX)
    [[nodiscard]] ModState &mod_state();
    // (Visuals) The SDL texture of a model material's base colour (cached), or
    // nullptr when it has none.
    [[nodiscard]] SDL_Texture *mod_material_texture(
        const std::shared_ptr<const modding::graphics::Model> &model,
        const modding::graphics::ModelMaterial &material);
    // (Polish) The texture a material's emissive light is drawn with: its
    // emissive texture (cached), or white; nullptr when it gives off none.
    [[nodiscard]] SDL_Texture *mod_emissive_texture(
        const std::shared_ptr<const modding::graphics::Model> &model,
        const modding::graphics::ModelMaterial &material);
    [[nodiscard]] bool mod_models_pending() const noexcept;
    // `sample_light` answers {sky, torch} light (0..1) at a point. `enhanced`:
    // the lit shader lights the props (vertex colour is albedo); otherwise
    // their light is baked into the vertex colour, as the creatures' is.
    void mod_append_models(EntityBatch &batch, const world::PlayerState &camera,
                           const world::SkyState &sky, float vertical_fov_degrees, float aspect,
                           bool enhanced,
                           const std::function<std::array<float, 2>(world::Vec3)> &sample_light);
    void mod_submit_lights(const world::PlayerState &camera, float vertical_fov_degrees,
                           float aspect);
    [[nodiscard]] std::array<float, 3> mod_light_at(world::Vec3 position) const;
    // Adds the installed mod block tiles to the terrain arrays (at load).
    void mod_install_tiles();
    // The atmosphere layers' LUT grade and the camera fade, over the world and
    // the first-person body, under the HUD.
    void mod_screen_effects(const ui::UiMetrics &metrics);
    // (Visuals) mod creatures and NPCs (GraphicsService::creatures), drawn
    // like the props.
    void mod_append_creatures(EntityBatch &batch, const world::PlayerState &camera,
                              const world::SkyState &sky, float vertical_fov_degrees,
                              float aspect, bool enhanced,
                              const std::function<std::array<float, 2>(world::Vec3)> &sample_light);
    // (Visuals) The mean colour of the mod block in a cell, when it holds one.
    [[nodiscard]] std::optional<std::array<float, 3>> mod_block_color(int x, int y, int z,
                                                                      world::BlockType block) const;
    // (Polish) The tile a chip of the block in a cell shows on `face`: a mod
    // block's own once the debris atlas below has its tiles, else retail.
    [[nodiscard]] std::uint8_t mod_debris_tile(int x, int y, int z, world::BlockType block,
                                               world::FaceDirection face) const;
    // (Polish) The debris atlas holding mod block tile `tile` (64 and up) and
    // the tile's uv rectangle {u, v, width, height} in it; null without one.
    [[nodiscard]] SDL_Texture *mod_debris_atlas(std::uint8_t tile,
                                                std::array<float, 4> &rectangle) const;
    std::shared_ptr<const modding::graphics::ModBlockOverlay> mod_block_overlay_;
    std::shared_ptr<const world::ModPaletteLooks> mod_palette_; // mod-world
    std::shared_ptr<const world::LiquidTable> liquids_;         // mod-liquid
    // (mod-liquid) Chunk::mod_cell of an untouched cell beyond the window, from
    // the cached generated baseline (0 where an edit replaced the cell).
    [[nodiscard]] std::uint8_t baseline_mod_cell(int x, int y, int z) const;
    // mod-world: every baked section is out of date (a new terrain, new looks).
    void remesh_everything();
    // mod-world: the far landscape's table of the generator's mod blocks and
    // (fix, wave 5) its side profiles.
    void push_far_mod_table();

    struct RenderFace final
    {
        world::WorldFace face;
        int depth{};
    };

    struct BlockCoordinate final
    {
        int x{};
        int y{};
        int z{};

        bool operator==(const BlockCoordinate &) const noexcept = default;
    };

    struct BlockCoordinateHash final
    {
        [[nodiscard]] std::size_t operator()(const BlockCoordinate &coordinate) const noexcept;
    };

    struct ChunkCoordinateHash final
    {
        [[nodiscard]] std::size_t operator()(world::ChunkCoordinate coordinate) const noexcept;
    };

    using SkyCube = std::array<SDL_Texture *, 6>;

    // An inclusive rectangle of chunk columns. Empty when maximum < minimum.
    struct ChunkRect final
    {
        int minimum_x{};
        int minimum_z{};
        int maximum_x{-1};
        int maximum_z{-1};

        [[nodiscard]] bool empty() const noexcept
        {
            return maximum_x < minimum_x || maximum_z < minimum_z;
        }
        [[nodiscard]] bool contains(int chunk_x, int chunk_z) const noexcept
        {
            return chunk_x >= minimum_x && chunk_x <= maximum_x && chunk_z >= minimum_z &&
                   chunk_z <= maximum_z;
        }
        [[nodiscard]] ChunkRect intersect(const ChunkRect &other) const noexcept
        {
            return {std::max(minimum_x, other.minimum_x), std::max(minimum_z, other.minimum_z),
                    std::min(maximum_x, other.maximum_x), std::min(maximum_z, other.maximum_z)};
        }
        // Equal as sets of columns: every empty rectangle is the same one.
        [[nodiscard]] bool same_columns(const ChunkRect &other) const noexcept
        {
            if (empty() || other.empty())
                return empty() == other.empty();
            return minimum_x == other.minimum_x && minimum_z == other.minimum_z &&
                   maximum_x == other.maximum_x && maximum_z == other.maximum_z;
        }
    };

    void load_original_atlas();
    void rebuild_region(int origin_chunk_x, int origin_chunk_z, world::BiomeKind biome);
    void install_region(world::WorldRegion region, world::BiomeKind biome);
    [[nodiscard]] ChunkRect region_rect() const noexcept;
    // The columns the view distance asks for around the focus chunk.
    [[nodiscard]] ChunkRect desired_render_window() const noexcept;
    // The columns that should be resident: simulation_window_'s.
    [[nodiscard]] ChunkRect desired_resident_window() const noexcept;
    // Brings the meshed window to `desired_render_window()` clipped to what is
    // resident: drops the meshes of columns that left it and queues the ones
    // that entered. With `previous_region` (the resident window before an
    // install) it also re-bakes columns whose neighbours arrived or left.
    void sync_render_window(const ChunkRect *previous_region = nullptr);
    void set_focus(float world_x, float world_z) noexcept;
    // Starts generating the simulation window's missing columns on a worker.
    void begin_stream_to_simulation_window(world::BiomeKind biome);
    void cancel_stream_job();
    void discard_pending_stream();
    void discard_pending_mesh();
    void apply_saved_edits();
    // `immediate` entries hold a change the player just made and jump the queue.
    void queue_chunk_rebuilds(std::vector<world::ChunkCoordinate> selection,
                              bool immediate = false);
    void pump_chunk_rebuilds(std::size_t budget, float upload_budget_milliseconds);
    // Hands finished section payloads to the GPU under a strict per-frame time
    // budget, so a worker that returns a whole chunk column at once can never
    // put every one of its buffer uploads in the same frame.
    void pump_mesh_uploads(float budget_milliseconds);
    // Picks the far landscape for this frame's camera and submits it.
    void update_far_terrain(const ui::UiMetrics &metrics, const world::SkyState &sky,
                            const world::PlayerState &camera, float vertical_fov_degrees,
                            float elapsed_seconds, bool rebirth_presentation);
    [[nodiscard]] bool finish_stream_if_ready();
    // What one mesh worker produced: the CPU face batches the simulation and
    // the SDL fallback read, plus each section's packed GPU payload.
    struct MeshJobResult final
    {
        std::vector<world::ChunkFaceBatch> batches;
        std::vector<PackedChunkMesh> uploads;
    };
    // `mod_blocks` (Visuals): the mod block overlay the faces get, or null.
    [[nodiscard]] static MeshJobResult build_mesh_job(
        const world::WorldRegion &snapshot, const std::vector<world::ChunkCoordinate> &selection,
        const modding::graphics::ModBlockOverlay *mod_blocks = nullptr,
        const world::ModPaletteLooks *mod_palette = nullptr,  // mod-world
        const world::LiquidTable *liquids = nullptr);         // mod-liquid
    void install_chunk_batches(MeshJobResult rebuilt);
    void reindex_mesh_batches();
    void refresh_flat_faces();
    void draw_sky(const ui::UiMetrics &metrics, const world::SkyState &sky, float elapsed_seconds,
                  bool reduced_motion, float yaw, float pitch, float vertical_fov_degrees,
                  bool rebirth_presentation, const world::BiomeAtmosphere &atmosphere);
    void draw_inspector(const ui::UiMetrics &metrics, const world::SkyState &sky,
                        bool ambient_occlusion, bool ore_glow, float shadow_strength);
    void draw_first_person(const ui::UiMetrics &metrics, const world::SkyState &sky,
                           const world::PlayerState &player, float vertical_fov_degrees,
                           bool ambient_occlusion, bool ore_glow, float shadow_strength);
    void draw_target_outline(const ui::UiMetrics &metrics, const world::PlayerState &player,
                             float vertical_fov_degrees, float elapsed_seconds);
    void draw_mining_effects(const ui::UiMetrics &metrics, const world::PlayerState &player,
                             float vertical_fov_degrees, float elapsed_seconds);
    void append_target_decals(EntityBatch &batch, const world::PlayerState &player,
                              float elapsed_seconds);
    void append_pickup_models(EntityBatch &batch);
    void append_door_models(EntityBatch &batch, const world::PlayerState &player,
                            const world::SkyState &sky);

    // Which exported model (plus tints/orientation) renders a ground pickup.
    struct PickupModelChoice final
    {
        StaticModelAsset *model{};
        float scale{1.0F};
        float pitch{};
        std::array<float, 3> primary{1.0F, 1.0F, 1.0F};
        std::array<float, 3> secondary{1.0F, 1.0F, 1.0F};
    };
    [[nodiscard]] PickupModelChoice pickup_model_choice(InventoryItemId item) const;

    enum class ParticleKind : std::uint8_t
    {
        rock,
        spark,
        // Material-coloured dust from a block face and neutral impact smoke are
        // separate recipes. Both stay independent from persistent
        // rocket/fireball/explosion smoke so first-person visibility tuning
        // cannot weaken large authored effects.
        mining_dust,
        impact_smoke,
        smoke,
        torch_fire,
        torch_smoke,
        // Original explosion/fireball recipes: additive Flash/Fire bursts,
        // alpha Ice puffs, and gravity-driven Rock shrapnel.
        flash,
        fire_soft,
        ice_flash,
        ice_soft,
        blast_rock,
    };

    struct MiningParticle final
    {
        world::Vec3 position{};
        world::Vec3 velocity{};
        world::BlockType block{};
        std::uint8_t tile{};
        ParticleKind kind{ParticleKind::rock};
        float age{};
        float lifetime{};
        float size{};
        float end_size{};
        float rotation{};
        float angular_velocity{};
        float opacity{1.0F};
        // Smoke tint; ordinary tracer impacts keep the authored white value.
        std::array<float, 3> tint{1.0F, 1.0F, 1.0F};
    };

    struct BlockPickup final
    {
        world::Vec3 position{};
        world::Vec3 velocity{};
        InventoryItemId item{InventoryItemId::bare_hands};
        world::BlockType block{};
        std::uint16_t count{1U};
        std::uint16_t rounds_in_clip{};
        float durability{1.0F};
        std::array<float, 3> waypoint{};
        std::string waypoint_name{"Alpha"};
        bool waypoint_valid{};
        float age{};
        float time_left{30.0F};
        bool ready_for_pickup{true};
        bool flying_to_player{};
        float fly_speed{};
        float bounce_phase{};
        float rotation{};
        std::optional<std::int32_t> pickup_id;
        std::int32_t spawner_id{-1};
        bool request_pending{};
        bool display_on_pickup{};
        // Modding, visuals area: the mod item the stack is (InventoryStack::
        // mod_item), kept for this peer's own drops so the pickup shows the mod
        // item and gives it back. The stock pickup message carries `item`, the
        // fallback; a pickup another peer created is always 0.
        std::uint32_t mod_item{};
    };

    [[nodiscard]] bool register_spawned_network_pickup(BlockPickup &pickup, bool dropped);

    // --- The Enhanced effects layer (world_preview_vfx.cpp). Every hook is a
    // no-op, and every `bool` one returns false, in the Original presentation.
    struct VfxState;
    struct VfxStateDeleter final
    {
        void operator()(VfxState *state) const noexcept;
    };
    std::unique_ptr<VfxState, VfxStateDeleter> vfx_state_;
    // Whether the last draw used the Enhanced chain with a working effects
    // renderer: effects spawned from then on go to the new layer.
    bool enhanced_effects_{};
    [[nodiscard]] VfxState &vfx_state();
    void vfx_set_enhanced(bool enhanced);
    // True when the new layer took the effect and the older recipe must not run.
    [[nodiscard]] bool vfx_spawn_impact_smoke(world::Vec3 position,
                                              const std::array<float, 3> &tint);
    [[nodiscard]] bool vfx_spawn_explosion(world::Vec3 position, ExplosionStyle style,
                                           bool rock_chunks);
    // Placed torches: begin, one call per torch in range, end.
    void vfx_torch_begin();
    [[nodiscard]] bool vfx_torch(int x, int y, int z, world::Vec3 flame);
    void vfx_torch_end();
    void vfx_update(float delta, world::Vec3 listener);
    void vfx_diagnostic(const world::PlayerState &camera);
    void vfx_adopt_particle(const MiningParticle &particle);
    // Builds what `camera` sees and hands it to the depth renderer.
    void vfx_submit(const world::PlayerState &camera, float elapsed_seconds);
    [[nodiscard]] bool vfx_debris_pending() const noexcept;
    void vfx_append_debris(EntityBatch &batch);
    void vfx_apply_camera_shake(world::PlayerState &camera, float elapsed_seconds) const;
    // Light from effects at a point, for the first-person body.
    [[nodiscard]] std::array<float, 3> vfx_light_at(world::Vec3 position) const;

    // --- Weapon, dragon, mining and creature effects on that layer
    // (world_preview_combat_vfx.cpp). The same rule: inert, and every `bool`
    // false, in the Original presentation.
    struct CombatVfxState;
    struct CombatVfxStateDeleter final
    {
        void operator()(CombatVfxState *state) const noexcept;
    };
    std::unique_ptr<CombatVfxState, CombatVfxStateDeleter> combat_vfx_state_;
    [[nodiscard]] CombatVfxState &combat_vfx_state();
    // `strike` is a vfx::Strike (openminerz/vfx_combat.hpp).
    void combat_vfx_strike(int strike, world::BlockType block, world::Vec3 point,
                           world::Vec3 normal);
    // True when the new layer took the effect and the older recipe must not run.
    [[nodiscard]] bool combat_vfx_mining_impact(const world::BlockBreakUpdate &impact);
    [[nodiscard]] bool combat_vfx_tracer_impact(const world::BlockBreakUpdate &impact);
    [[nodiscard]] bool combat_vfx_blaster_impact(const world::BlockBreakUpdate &impact,
                                                 const std::array<float, 3> &tint);
    [[nodiscard]] bool combat_vfx_dragon_hit(world::Vec3 position);
    [[nodiscard]] bool combat_vfx_fireball_detonation(world::Vec3 position, bool ice,
                                                      bool rock_chunks, ExplosionStyle style);
    // Moves what follows something (fireballs, the dragon's mouth, grenades).
    // Call before vfx_update on the same tick.
    void combat_vfx_update(float delta, world::Vec3 player_position);
    void combat_vfx_pickup_collected(world::Vec3 position);
    // Adds what is drawn exactly where something is this frame (tracer glow).
    void combat_vfx_frame(const vfx::View &view, vfx::Frame &frame, float elapsed_seconds);
    // OPENMINERZ_DIAGNOSTIC_VFX="combat:<scene>,...": a scripted weapon, dragon
    // or mining scene run through the real spawn functions. True when it was
    // played forward to its `age` and the effects must now hold still.
    [[nodiscard]] bool combat_vfx_diagnostic(std::string_view spec,
                                             const world::PlayerState &camera);
    void combat_vfx_scene_event();
    void combat_vfx_play_scene(float delta);

    struct DeferredPickup final
    {
        BlockPickup pickup;
        bool dropped{};
        // A create that arrived from another peer already carries its
        // sender-fixed identity and must not be given a local pickup id when it
        // is finally released.
        bool networked{};
    };

    // PickupManager._createQueue: requests past the 1000-entity bound wait here
    // instead of being destroyed, and leave as entities leave the world.
    void release_deferred_pickups();

    SDL_Renderer *renderer_{};
    SDL_Texture *atlas_{};
    SDL_Texture *normal_spec_atlas_{};
    SDL_Texture *metal_light_atlas_{};
    SDL_Texture *block_cracks_{};
    SDL_Texture *block_selector_{};
    SDL_Texture *mining_smoke_{};
    SDL_Texture *mining_sparks_{};
    SDL_Texture *mining_rocks_{};
    std::unique_ptr<DepthVoxelRenderer> depth_renderer_;
    std::unique_ptr<AvatarViewmodel> avatar_viewmodel_;
    std::unique_ptr<EnemyModelLibrary> enemy_models_;
    const std::vector<world::Enemy> *visible_enemies_{};
    const std::vector<RemotePlayerVisual> *visible_remote_players_{};
    NametagCamera overlay_camera_{};
    std::optional<view::ViewmodelPoint> first_person_muzzle_;
    std::vector<ProjectileVisual> projectile_visuals_;
    std::vector<RocketVisual> rocket_visuals_;
    std::vector<ExplosiveFuseVisual> explosive_fuse_visuals_;
    std::vector<FireballVisual> fireball_visuals_;
    bool network_pickups_enabled_{};
    std::int32_t local_pickup_spawner_id_{-1};
    std::int32_t next_local_pickup_id_{};
    std::vector<SpawnedNetworkPickup> spawned_network_pickups_;
    world::PickupOverflowQueue<DeferredPickup> deferred_pickups_;
    // Modding, visuals area: see set_mod_item_models / set_held_mod_item.
    ModItemModels mod_item_models_;
    std::shared_ptr<const modding::graphics::HeldItemPose> held_mod_item_;
    // The pickup's mod item look, or nullptr to draw it as its fallback.
    [[nodiscard]] std::shared_ptr<const modding::graphics::ItemLook> pickup_mod_model(
        const BlockPickup &pickup) const;
    // (Items) Scratch for posing a pickup's mod model.
    std::vector<modding::graphics::PosedItemPart> pickup_item_parts_;
    std::unique_ptr<modding::graphics::ItemPoseScratch> pickup_item_scratch_;
    std::unique_ptr<StaticModelAsset> rocket_model_;
    SDL_Texture *white_texture_{};
    // Placed-torch props (TorchCloud port): the real torch model per placed
    // torch block plus the original TorchFire/TorchSmoke emitters.
    struct TorchInstance final
    {
        int x{};
        int y{};
        int z{};
        world::BlockType variant{};
        float fire_accumulator{};
        float smoke_accumulator{};
    };
    std::vector<TorchInstance> torch_cache_;
    float torch_scan_timer_{};
    void update_torch_props(float delta, world::Vec3 player_position);
    SDL_Texture *torch_fire_texture_{};
    SDL_Texture *torch_smoke_texture_{};
    SDL_Texture *flash_texture_{};
    SDL_Texture *ice_flash_texture_{};
    SDL_Texture *ice_puff_texture_{};
    // Staged one-shot emitters (FirePuff runs 2s, BigSmokePuff 3s) ported from
    // the original ParticleEmitter recipes.
    struct EffectEmitter final
    {
        ParticleKind kind{ParticleKind::fire_soft};
        world::Vec3 position{};
        float rate{};
        float time_left{};
        float accumulator{};
        float life_base{1.0F};
        float life_variation{};
        float horizontal_min{};
        float horizontal_max{};
        float vertical_min{};
        float vertical_max{};
        float start_min{};
        float start_max{};
        float end_min{};
        float end_max{};
        float rotate_min{};
        float rotate_max{};
        std::array<float, 3> tint{1.0F, 1.0F, 1.0F};
    };
    std::vector<EffectEmitter> effect_emitters_;
    // Per-fireball trail accumulators (FireTrail/SmokeTrail/FireGlow/
    // FireBallEffect all ride along with the fireball).
    struct FireballTrailState final
    {
        float fire{};
        float smoke{};
        world::Vec3 previous{};
        bool initialized{};
        bool seen{};
    };
    std::unordered_map<int, FireballTrailState> fireball_trails_;
    struct RocketTrailState final
    {
        float fire{};
        float smoke{};
        bool seen{};
    };
    std::unordered_map<int, RocketTrailState> rocket_trails_;
    struct FuseTrailState final
    {
        float smoke{};
        bool seen{};
    };
    std::unordered_map<int, FuseTrailState> fuse_trails_;
    void update_effect_emitters(float delta);
    void emit_fireball_trails(float delta);
    void emit_rocket_and_fuse_trails(float delta);
    const world::Dragon *visible_dragon_{};
    const WorldSaveDoorMap *visible_doors_{};
    struct EnemyLightSample final
    {
        float sun{1.0F};
        float torch{};
    };
    std::unordered_map<std::uint32_t, EnemyLightSample> enemy_light_cache_;
    std::size_t enemy_light_cursor_{};
    float viewmodel_light_seconds_{-1.0F};
    world::Vec3 viewmodel_light_position_{};
    std::array<float, 3> viewmodel_ambient_{};
    std::array<float, 3> viewmodel_directional_{};
    std::unique_ptr<StaticModelAsset> pickup_pickaxe_;
    std::unique_ptr<StaticModelAsset> pickup_compass_;
    std::unique_ptr<StaticModelAsset> pickup_pistol_;
    std::unique_ptr<StaticModelAsset> pickup_knife_;
    std::unique_ptr<StaticModelAsset> pickup_ammo_;
    std::unique_ptr<StaticModelAsset> pickup_torch_;
    std::unique_ptr<StaticModelAsset> pickup_assault_rifle_;
    std::unique_ptr<StaticModelAsset> pickup_shotgun_;
    std::unique_ptr<StaticModelAsset> pickup_rifle_;
    std::unique_ptr<StaticModelAsset> pickup_smg_;
    std::unique_ptr<StaticModelAsset> pickup_lmg_;
    std::unique_ptr<StaticModelAsset> pickup_launcher_;
    std::unique_ptr<StaticModelAsset> pickup_space_assault_rifle_;
    std::unique_ptr<StaticModelAsset> pickup_space_pistol_;
    std::unique_ptr<StaticModelAsset> pickup_space_shotgun_;
    std::unique_ptr<StaticModelAsset> pickup_space_rifle_;
    std::unique_ptr<StaticModelAsset> pickup_space_smg_;
    std::unique_ptr<StaticModelAsset> pickup_ore_;
    std::unique_ptr<StaticModelAsset> pickup_gems_;
    std::unique_ptr<StaticModelAsset> pickup_bars_;
    std::unique_ptr<StaticModelAsset> door_wood_;
    std::unique_ptr<StaticModelAsset> door_iron_;
    std::unique_ptr<StaticModelAsset> door_diamond_;
    std::unique_ptr<StaticModelAsset> door_tech_;
    SkyCube clear_sky_{};
    SkyCube dawn_sky_{};
    SkyCube sunset_sky_{};
    SkyCube night_sky_{};
    world::WorldGenerator generator_;
    world::WorldRegion region_;
    std::vector<world::ChunkFaceBatch> mesh_batches_;
    std::unordered_map<world::ChunkCoordinate, std::size_t, ChunkCoordinateHash>
        mesh_batch_indices_;
    std::vector<RenderFace> faces_;
    bool faces_dirty_{};
    std::unordered_map<BlockCoordinate, world::BlockType, BlockCoordinateHash> saved_edits_;
    // Host-side validation can query cells outside the local camera window.
    // Cache a bounded set of untouched columns so spawner/explosion scans do
    // not regenerate 32K voxels for every neighboring cell.
    mutable std::unordered_map<world::ChunkCoordinate, world::Chunk, ChunkCoordinateHash>
        authority_baselines_;
    std::vector<MiningParticle> mining_particles_;
    std::vector<BlockPickup> block_pickups_;
    std::uint32_t effect_random_state_{0xC45A91E3U};
    float particle_density_{1.0F};
    float presentation_lag_seconds_{};
    // Enhanced post-processing inputs (presentation only): the last settings
    // handed in, the aim progress for the next draw, and the eye's baked
    // skylight eased over time so walking under a roof fades the air's glow
    // instead of switching it.
    PostEffects post_effects_{};
    float aim_progress_{};
    float eye_sky_light_{1.0F};
    float eye_sky_seconds_{-1.0F};
    // Far terrain; created the first time the setting asks for it.
    std::unique_ptr<world::FarTerrain> far_terrain_;
    bool far_terrain_submitted_{};
    bool far_terrain_settled_{};
    float far_terrain_quiet_since_{-1.0F};
    // What this frame's section uploads used of the upload budget.
    float mesh_upload_spent_milliseconds_{};
    WorldPreviewStats stats_;
    world::BiomeKind biome_{world::BiomeKind::classic};
    bool water_extra_enabled_{};
    // How many columns across the view distance asks to have meshed. The
    // resident window (region_) is always the original's 24, so that what is
    // simulated never depends on a graphics setting.
    int active_span_{};
    float visibility_distance_{66.0F};
    world::StockTerrainWindow simulation_window_{};
    // The column the meshed window is centred on: the local player's.
    int focus_chunk_x_{};
    int focus_chunk_z_{};
    // The columns that currently have (or are queued for) a mesh.
    ChunkRect render_window_{};
    std::future<world::WorldRegion> stream_job_;
    bool stream_job_active_{};
    std::future<MeshJobResult> mesh_job_;
    bool mesh_job_active_{};
    // Section payloads finished by a worker and waiting for a frame with room
    // in its GPU-upload budget. Installed strictly in arrival order, so a later
    // rebuild of the same section always supersedes an earlier one.
    std::deque<PackedChunkMesh> pending_uploads_;
    std::vector<world::ChunkCoordinate> active_mesh_rebuilds_;
    std::vector<world::ChunkCoordinate> pending_mesh_rebuilds_;
    // Membership of pending_mesh_rebuilds_, so merging a new edit's sections
    // costs O(added) rather than rebuilding a set over the whole backlog.
    std::unordered_set<world::ChunkCoordinate, ChunkCoordinateHash> pending_lookup_;
    // How many entries at the front of the queue carry a just-made change.
    std::size_t pending_priority_{};
    // Edits whose light-influence apron has not been measured yet, and the
    // worker measuring one batch of them. The sections holding the change are
    // queued without waiting for this; only the shading apron does, which costs
    // a frame of shading latency and saves most of the apron's bakes.
    std::vector<std::array<int, 3>> pending_light_edits_;
    // Parallel to pending_light_edits_ while every batch merged into it told us
    // what it replaced; cleared (and left empty) as soon as one did not.
    std::vector<world::BlockType> pending_light_previous_;
    bool pending_light_previous_known_{true};
    void queue_block_edits(const std::vector<std::array<int, 3>> &edits);
    struct LightProbeResult final
    {
        std::vector<world::ChunkCoordinate> sections;
        // What the geometric rule would have queued, for comparison.
        std::size_t candidates{};
        float milliseconds{};
    };
    std::future<LightProbeResult> light_probe_job_;
    bool light_probe_active_{};
    void pump_light_probe();
    bool defer_mesh_rebuild_once_{};
    // When each edited section first asked for a mesh that includes its edit,
    // and when the worker that is currently running took its snapshot.
    std::unordered_map<world::ChunkCoordinate, std::chrono::steady_clock::time_point,
                       ChunkCoordinateHash>
        edit_waiting_since_;
    std::chrono::steady_clock::time_point mesh_job_started_{};
    world::BiomeKind pending_biome_{world::BiomeKind::classic};
};

} // namespace openminerz
