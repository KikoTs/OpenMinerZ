#pragma once

#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/world.hpp"
#include "openminerz/world_save.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz
{

class DepthVoxelRenderer;
class AvatarViewmodel;
class StaticModelAsset;

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
    [[nodiscard]] bool preload_step(std::size_t mesh_budget);
    [[nodiscard]] bool preload_complete() const noexcept;
    [[nodiscard]] float preload_progress() const noexcept;
    [[nodiscard]] bool stream_around(const world::PlayerState &player);
    // Teleport waits for terrain to recenter in the original. This synchronous
    // install provides the same safe handoff before player physics resumes.
    void recenter_on(const world::PlayerState &player);
    void set_view_distance(int view_distance);
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
    // Tone curve index (0 legacy, 1 Reinhard, 2 filmic, 3 ACES), auto
    // exposure, manual EV trim and colour-grading strength.
    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept;
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
    void spawn_blaster_impact(const world::BlockBreakUpdate &impact);
    // FireballEntity.Detonate: FlashEffect + FirePuff + BigSmokePuff +
    // BigRockBlast, exactly one emitter each at the impact point.
    void spawn_fireball_explosion(world::Vec3 position, bool ice, bool rock_chunks = true);
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
    [[nodiscard]] std::vector<CollectedBlockStack> update_effects(float delta_seconds,
                                                                  world::Vec3 player_position);
    void cycle_biome(int direction);
    // Jump straight to a biome band (0 classic .. 5 descent), then stream the
    // complete configured play window around that band.
    void set_biome(int index);
    [[nodiscard]] std::string_view biome_name() const noexcept;
    [[nodiscard]] int center_x() const noexcept;
    [[nodiscard]] int center_z() const noexcept;

  private:
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

    void load_original_atlas();
    void rebuild_region(int origin_chunk_x, int origin_chunk_z, world::BiomeKind biome);
    void install_region(world::WorldRegion region, world::BiomeKind biome);
    void discard_pending_stream();
    void discard_pending_mesh();
    void apply_saved_edits();
    void queue_chunk_rebuilds(std::vector<world::ChunkCoordinate> selection);
    void pump_chunk_rebuilds(std::size_t budget);
    [[nodiscard]] bool finish_stream_if_ready();
    void install_chunk_batches(std::vector<world::ChunkFaceBatch> rebuilt);
    void reindex_mesh_batches();
    void refresh_flat_faces();
    void draw_sky(const ui::UiMetrics &metrics, const world::SkyState &sky, float elapsed_seconds,
                  bool reduced_motion, float yaw, float pitch, float vertical_fov_degrees,
                  bool rebirth_presentation);
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
    };

    [[nodiscard]] bool register_spawned_network_pickup(BlockPickup &pickup, bool dropped);

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
    std::vector<ProjectileVisual> projectile_visuals_;
    std::vector<RocketVisual> rocket_visuals_;
    std::vector<ExplosiveFuseVisual> explosive_fuse_visuals_;
    std::vector<FireballVisual> fireball_visuals_;
    bool network_pickups_enabled_{};
    std::int32_t local_pickup_spawner_id_{-1};
    std::int32_t next_local_pickup_id_{};
    std::vector<SpawnedNetworkPickup> spawned_network_pickups_;
    std::unique_ptr<StaticModelAsset> fireball_model_;
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
        float glow{};
        float ball{};
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
    WorldPreviewStats stats_;
    world::BiomeKind biome_{world::BiomeKind::classic};
    int active_span_{};
    float visibility_distance_{66.0F};
    std::future<world::WorldRegion> stream_job_;
    bool stream_job_active_{};
    std::future<std::vector<world::ChunkFaceBatch>> mesh_job_;
    bool mesh_job_active_{};
    std::vector<world::ChunkCoordinate> active_mesh_rebuilds_;
    std::vector<world::ChunkCoordinate> pending_mesh_rebuilds_;
    bool defer_mesh_rebuild_once_{};
    world::BiomeKind pending_biome_{world::BiomeKind::classic};
};

} // namespace openminerz
