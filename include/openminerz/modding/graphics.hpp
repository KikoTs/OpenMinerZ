#pragma once

// The modding API's graphics and assets service: texture, sound and animation
// packs, models, lighting props, effects, atmosphere layers, sounds and music,
// and camera effects.
//
// PRESENTATION ONLY. Everything here is `capability::presentation` on the
// CLIENT VM: it changes what one player sees and hears, never the simulation,
// the wire or a save, so a client-scope pack works on any server, vanilla
// CastleMiner Z hosts included. Server mods name assets by string (a mod
// block's texture, a creature's model); this service resolves those names on
// each client. With no mod loaded nothing here runs: no pass is added, no
// shader changes, every loader reads exactly the file it always read.
//
// ---------------------------------------------------------------------------
// ASSET FOLDERS  (a mod's assets/ directory; names are case-sensitive in the
// mod, forward slashes, [A-Za-z0-9_.-] only, no "..", at most 8 levels)
//
//   textures/blocks/<tile>.png        replaces terrain atlas tile <tile> (0..63,
//                                     the retail atlas index of
//                                     world::atlas_tile: 0 dirt, 1 grass side,
//                                     2 grass top, 3 sand, 5 rock, 18 wood,
//                                     25 crate, ...). Any
//                                     square size; it is resampled to the
//                                     active pack's tile size (16 Classic,
//                                     higher for Classic HD) and mipmapped with
//                                     the rest of the atlas.
//   textures/blocks/<tile>_n.png      optional normal map of that tile (RG the
//                                     tangent-space slope, 128 flat)
//   textures/blocks/<tile>_m.png      optional material map (R smoothness, G/B
//                                     self-light). Without the two optional
//                                     maps an overridden tile is drawn flat and
//                                     matte instead of with the old tile's bumps.
//   textures/items/<item>.png         the inventory icon of retail item id <item>
//                                     (64x64 and 128x128 cells; any size)
//   textures/entities/<name>.png      a creature texture the content pack loads
//                                     as DerivedData/<name>.bmp
//   textures/sky/<name>.png           a sky face, e.g. sky_night_positivey.png
//   textures/ui/<name>.png            a HUD or screen sprite, e.g. hud_HudGrid.png
//   textures/particles/<name>.png     a particle sprite, e.g. torch_fire.png
//   textures/<anything>.png           mod-owned textures, referenced by name
//                                     (mod block faces, model materials, icons)
//   models/<name>.gltf | .glb         glTF 2.0 props and creatures: meshes,
//                                     PNG/JPEG base-colour textures, skins and
//                                     animation clips
//   animations/<Registration>.gltf    replaces a retail player clip by its
//                                     registered name ("Stand", "Walk",
//                                     "PickUse", ...; see animation_slots())
//   sounds/<name>.wav | .ogg          mod sounds, played by name
//   sounds/cues/<Cue>.wav | .ogg      replaces a retail XACT cue by name
//                                     ("Click", "Pistol", "Footstep", ...)
//   music/<name>.ogg | .wav           music tracks
//   luts/<name>.png                   a colour-grading LUT: a strip of N tiles
//                                     of N x N (N*N wide, N high; 16 or 32),
//                                     red across a tile, green down, blue by tile
//
// LAYERING: every enabled mod's assets/ joins one stack in load order; a later
// mod wins over an earlier one and every mod wins over the active content
// pack. A name may be pinned to one mod with "@<mod-id>/<name>". The stack is
// rebuilt at session start and end and on a developer reload; textures that
// were loaded from it are reloaded then.
//
// Full design and the parts that are not done yet: docs/modding/architecture.md
// and the report of the graphics area.
// ---------------------------------------------------------------------------

#include "openminerz/modding/api.hpp"
#include "openminerz/modding/gunfx.hpp" // (GunFX)
#include "openminerz/vfx.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openminerz
{
struct PostEffects;
namespace world
{
struct SkyState;
}
namespace rig
{
class RetailRig;
struct Clip;
} // namespace rig
} // namespace openminerz

namespace openminerz::modding::graphics
{

// Handle kinds of this service beyond the shared ones (light, effect, sound).
inline constexpr HandleKind model_handle_kind = static_cast<HandleKind>(140U);
inline constexpr HandleKind atmosphere_handle_kind = static_cast<HandleKind>(141U);

using Vec3 = vfx::Vec3;
using Color = vfx::Color;

// ===========================================================================
// Assets
// ===========================================================================

struct AssetSource final
{
    std::string mod_id;
    // The mod's assets/ directory.
    std::filesystem::path root;
};

// True when `name` is an acceptable mod-relative asset name (see ASSET FOLDERS).
// Absolute paths, drive letters, backslashes, "." / ".." components, empty
// components and anything outside [A-Za-z0-9_.-/] are refused.
[[nodiscard]] bool valid_asset_name(std::string_view name, std::string *reason = nullptr);
// root / name, but only when it stays inside root after following links.
[[nodiscard]] std::optional<std::filesystem::path> contained_path(const std::filesystem::path &root,
                                                                  std::string_view name);

class AssetStack final
{
  public:
    struct Resolved final
    {
        std::filesystem::path path;
        std::string mod_id;
        // The name as found, extension included ("textures/blocks/3.png").
        std::string name;
    };

    // Load order: the last source wins.
    void set_sources(std::vector<AssetSource> sources);
    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept
    {
        return sources_.empty();
    }
    [[nodiscard]] const std::vector<AssetSource> &sources() const noexcept
    {
        return sources_;
    }
    // Bumped whenever the sources change, so caches know to reload.
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    // `name` as given, or with each of `extensions` appended in turn when it
    // has none of them. "@mod/name" looks in that mod only.
    [[nodiscard]] std::optional<Resolved> resolve(
        std::string_view name, std::span<const std::string_view> extensions = {}) const;
    // Every file directly in `folder` with one of `extensions`, one per name
    // (the winning layer), sorted by name.
    [[nodiscard]] std::vector<Resolved> list(std::string_view folder,
                                             std::span<const std::string_view> extensions) const;

  private:
    std::vector<AssetSource> sources_;
    std::uint64_t generation_{};
};

// Bounded file reads for everything a mod hands the engine.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file_bounded(
    const std::filesystem::path &path, std::size_t max_bytes, std::string *error = nullptr);

// --- Images ----------------------------------------------------------------

struct Image final
{
    int width{};
    int height{};
    // Straight (not premultiplied) RGBA8, rows top to bottom.
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool empty() const noexcept
    {
        return width <= 0 || height <= 0 ||
               rgba.size() !=
                   static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    }
};

struct ImageLimits final
{
    int max_side{4096};
    std::size_t max_file_bytes{32U * 1024U * 1024U};
};

// PNG, JPEG, BMP and TGA.
[[nodiscard]] std::optional<Image> decode_image(std::span<const std::uint8_t> bytes,
                                                std::string *error = nullptr,
                                                const ImageLimits &limits = {});
[[nodiscard]] std::optional<Image> load_image(const std::filesystem::path &path,
                                              std::string *error = nullptr,
                                              const ImageLimits &limits = {});
// Box filter when shrinking, bilinear when growing.
[[nodiscard]] Image resample(const Image &source, int width, int height);
// Copies `source` into `target` at (x, y), clipped.
void blit(Image &target, const Image &source, int x, int y);

// --- Content-pack overrides ------------------------------------------------

// The terrain atlas tiles a pack replaces: diffuse plus the two optional
// material maps, keyed by tile (0..63).
struct TileOverrides final
{
    std::map<int, Image> diffuse;
    std::map<int, Image> normal_spec;
    std::map<int, Image> metal_light;

    [[nodiscard]] bool empty() const noexcept
    {
        return diffuse.empty() && normal_spec.empty() && metal_light.empty();
    }
};
[[nodiscard]] TileOverrides collect_tile_overrides(const AssetStack &stack,
                                                   std::vector<std::string> *warnings = nullptr);
// Writes `tiles` into an atlas of `columns` x `columns` tiles, each resampled to
// the atlas tile size. Tiles outside the atlas are ignored.
void apply_tile_overrides(Image &atlas, const std::map<int, Image> &tiles, int columns = 8);
// The neutral material texels an overridden tile gets when it brings no maps
// of its own: a flat normal and a matte, unlit surface.
inline constexpr std::array<std::uint8_t, 4> flat_normal_spec{128U, 128U, 255U, 0U};
inline constexpr std::array<std::uint8_t, 4> matte_metal_light{64U, 0U, 0U, 255U};

// Terrain tiles for MOD blocks, allocated at session start from the block
// registry's texture references (Gameplay owns block definitions; this reads
// their texture names as plain strings). Retail uses atlas slices 0..63; mod
// tiles are slices 64.. of the same mipmapped array, so a mod block is shaded,
// lit and shadowed exactly like a retail one.
class BlockTextureTable final
{
  public:
    static constexpr int first_mod_tile = 64;
    static constexpr int max_mod_tiles = 192;

    // The slice for a texture reference ("textures/ruins_brick"), allocating
    // one the first time; nullopt when the table is full.
    std::optional<std::uint8_t> tile_for(std::string_view texture);
    [[nodiscard]] std::optional<std::uint8_t> find(std::string_view texture) const;
    [[nodiscard]] const std::vector<std::string> &textures() const noexcept
    {
        return textures_;
    }
    void clear() noexcept;

  private:
    std::vector<std::string> textures_;
};

// What the loaders deep inside the renderers and the audio see: the session's
// asset stack and the extra terrain tiles. Installed by the app, read
// process-wide. Nothing installed (the default, and every unmodded run) means
// every loader reads exactly the content-pack file it always read.
struct PresentationAssets final
{
    AssetStack stack;
    BlockTextureTable block_tiles;
};
void install_presentation_assets(std::shared_ptr<const PresentationAssets> assets);
[[nodiscard]] std::shared_ptr<const PresentationAssets> presentation_assets();
// The mod file that replaces content-pack file `file_name` ("hud_HudGrid.bmp")
// of `kind` ("ui", "sky", "entities", "particles"), or empty.
[[nodiscard]] std::filesystem::path derived_override(std::string_view kind,
                                                     std::string_view file_name);
// The sound file that replaces retail cue `cue`, or empty.
[[nodiscard]] std::filesystem::path cue_override(std::string_view cue);

// ===========================================================================
// Mod blocks on the terrain
// ===========================================================================

// How a client draws one mod block (Gameplay's replicated overlay,
// GameplayService::client_block_at): the retail fallback the world must still
// hold in that cell, the BlockTextureTable slices of its top, side and bottom
// faces, and its texture's mean colour (the dust a dig raises).
struct ModBlockLook final
{
    std::uint8_t fallback{};
    // Top, side, bottom.
    std::array<std::uint8_t, 3> tiles{};
    Color color{0.5F, 0.5F, 0.5F};
    bool liquid{}; // (mod-liquid) meshed as a liquid (openminerz/world/liquid.hpp)
};

// Which of ModBlockLook::tiles a face shows: 0 top, 1 the four sides, 2 bottom.
[[nodiscard]] std::size_t mod_block_face_slot(world::FaceDirection face) noexcept;

// Every mod block cell a client draws. Built on the main thread whenever the
// overlay or the registry changes and handed to the mesh workers as an
// immutable snapshot. The mesher itself never changes: apply() rewrites the
// tile of a mod block's faces after the fact and leaves every other face, and
// so every retail mesh, exactly as it was.
class ModBlockOverlay final
{
  public:
    void set(int x, int y, int z, const ModBlockLook &look);
    [[nodiscard]] bool empty() const noexcept
    {
        return cells_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return cells_.size();
    }
    // The look of a cell while it still holds the look's fallback.
    [[nodiscard]] const ModBlockLook *find(int x, int y, int z,
                                           world::BlockType block) const noexcept;
    // Gives the faces of mod blocks in `batch` their mod tiles. Returns how
    // many faces changed.
    std::size_t apply(world::ChunkFaceBatch &batch) const;
    // The mesh sections whose faces differ between two overlays (either may
    // be null, which is an empty overlay).
    [[nodiscard]] static std::vector<world::ChunkCoordinate> changed_sections(
        const ModBlockOverlay *before, const ModBlockOverlay *after);

  private:
    struct Cell final
    {
        int x{};
        int y{};
        int z{};
        ModBlockLook look;
    };
    std::unordered_map<std::uint64_t, Cell> cells_;
    // Sections holding at least one cell: a batch without any costs one lookup.
    std::unordered_set<std::uint64_t> sections_;
};

// (Polish) The tile a chip of the block in a cell shows for `face` (struck,
// broken, placed or blown up): the mod block's own when the cell holds one and
// `mod_tiles` (the debris can draw tiles past the retail 64), else the retail
// tile of `block`. `overlay` may be null.
[[nodiscard]] std::uint8_t debris_tile(const ModBlockOverlay *overlay, int x, int y, int z,
                                       world::BlockType block, world::FaceDirection face,
                                       bool mod_tiles) noexcept;

// ===========================================================================
// Models (glTF 2.0)
// ===========================================================================

using Matrix4 = std::array<float, 16>; // column-major, column vectors (glTF)

[[nodiscard]] Matrix4 matrix_identity() noexcept;
[[nodiscard]] Matrix4 matrix_multiply(const Matrix4 &left, const Matrix4 &right) noexcept;
[[nodiscard]] Matrix4 matrix_compose(const Vec3 &translation, const std::array<float, 4> &rotation,
                                     const Vec3 &scale) noexcept;
[[nodiscard]] Vec3 transform_point(const Matrix4 &matrix, const Vec3 &point) noexcept;
[[nodiscard]] Vec3 transform_direction(const Matrix4 &matrix, const Vec3 &direction) noexcept;

struct ModelLimits final
{
    std::size_t max_file_bytes{64U * 1024U * 1024U};
    std::size_t max_vertices{262144U};
    std::size_t max_indices{1048576U};
    std::size_t max_nodes{1024U};
    std::size_t max_joints{128U};
    std::size_t max_materials{64U};
    std::size_t max_primitives{256U};
    std::size_t max_animations{64U};
    std::size_t max_keys{1048576U};
    ImageLimits images{};
    // (Animation packs) false accepts a file with nodes and animations but no
    // triangle mesh, as Blender writes an armature exported on its own.
    bool require_mesh{true};
};

struct ModelVertex final
{
    Vec3 position{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    std::array<float, 2> uv{};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{};
};

struct ModelMaterial final
{
    std::string name;
    // Empty when the material has no base-colour texture.
    Image base_color;
    std::array<float, 4> color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    // (Polish) The light the surface gives off: emissiveFactor times
    // KHR_materials_emissive_strength (0..16 a channel), times the
    // emissive texture when there is one (empty otherwise, and always empty
    // while the factor is black). Drawn unlit over the lit surface.
    Vec3 emissive{};
    Image emissive_texture;
    bool blend{};
    bool mask{};
    float alpha_cutoff{0.5F};
    bool double_sided{};
};

struct ModelPrimitive final
{
    int node{-1};
    int material{-1};
    // Index into Model::skins, or -1 for a rigid mesh that follows its node.
    int skin{-1};
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices; // triangles
};

struct ModelNode final
{
    std::string name;
    int parent{-1};
    Vec3 translation{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F}; // x y z w
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

struct ModelSkin final
{
    std::vector<int> joints; // node indices
    std::vector<Matrix4> inverse_bind;
};

struct AnimationChannel final
{
    enum class Path : std::uint8_t
    {
        translation,
        rotation,
        scale
    };
    enum class Interpolation : std::uint8_t
    {
        linear,
        step,
        cubic
    };
    int node{-1};
    Path path{Path::translation};
    Interpolation interpolation{Interpolation::linear};
    std::vector<float> times;
    // 3 or 4 floats per key; cubic keys carry in-tangent, value, out-tangent.
    std::vector<float> values;
};

struct ModelAnimation final
{
    std::string name;
    float duration{};
    std::vector<AnimationChannel> channels;
};

struct Model final
{
    std::vector<ModelNode> nodes;
    std::vector<int> node_order; // parents before children
    std::vector<ModelMaterial> materials;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimation> animations;
    // Bind-pose bounds in model space.
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    float radius{};
    std::size_t vertex_count{};
    std::size_t triangle_count{};

    [[nodiscard]] int find_animation(std::string_view name) const noexcept;
};

// Parses a .gltf (with its buffers and images in the same folder or below,
// or as data: URIs) or a .glb. Every count is bounded by `limits`; anything
// malformed or over a bound is refused with a message, never trusted.
[[nodiscard]] std::shared_ptr<Model> load_model(const std::filesystem::path &path,
                                                std::string *error = nullptr,
                                                const ModelLimits &limits = {});
[[nodiscard]] std::shared_ptr<Model> parse_model(std::span<const std::uint8_t> bytes,
                                                 const std::filesystem::path &base_directory,
                                                 std::string *error = nullptr,
                                                 const ModelLimits &limits = {});

// Each node's model-space matrix with `animation` (or none, -1) sampled at
// `seconds` (wrapped when `loop`).
void evaluate_pose(const Model &model, int animation, float seconds, bool loop,
                   std::vector<Matrix4> &world);
// Skinning matrices of `skin` for a pose from evaluate_pose.
void skin_matrices(const Model &model, int skin, const std::vector<Matrix4> &world,
                   std::vector<Matrix4> &out);
// (Animation packs) every node's LOCAL transform with `animation` sampled at
// `seconds` (clamped to the clip, not wrapped): linear, step and cubic keys,
// rotations along the shorter arc.
void sample_animation(const Model &model, int animation, float seconds,
                      std::vector<ModelNode> &local);

// ===========================================================================
// Animation packs (player clips on the retail rig)
// ===========================================================================

// Registrations of the retail character a pack may replace, and what each is.
struct AnimationSlot final
{
    std::string_view registration;
    std::string_view description;
};
[[nodiscard]] std::span<const AnimationSlot> animation_slots() noexcept;

// Converts one animation of `model` into a retail clip: glTF nodes are matched
// to the 71 attachment bones by name ("Root", "BackLower", "Head",
// "PropRight", ...) and sampled at the rig's 30 Hz. Every bone but Root is
// read in its own local space; Root is read in the authoring frame
// (openminerz/modding/animation_authoring.hpp), whatever nodes sit above it.
// Translation, rotation and scale are replaced separately: a channel the file
// does not animate keeps its keys from `fallback` (the clip it replaces), so a
// pack may animate only the arms of "GunReload". The clip plays on the
// replaced clip's clock, because the game, not a pack, decides how long a
// swing or a reload lasts: its keys play from the start at their own speed, a
// shorter clip holds its last pose until the game ends it and a longer one is
// cut off, exactly as the engine treats its own clips.
//
// (GunFX) With `stretch` (reload clips: gunfx::reload_clip) the file's keys
// are spread over the replaced clip's length instead, so the clip plays over
// exactly the game's reload window whatever length it was authored at.
[[nodiscard]] std::optional<rig::Clip> clip_from_animation(const Model &model, int animation,
                                                           const rig::RetailRig &rig,
                                                           const rig::Clip *fallback,
                                                           std::string *error = nullptr,
                                                           bool stretch = false);
// The same for the model's first animation.
[[nodiscard]] std::optional<rig::Clip> clip_from_model(const Model &model,
                                                       const rig::RetailRig &rig,
                                                       const rig::Clip *fallback,
                                                       std::string *error = nullptr);
// Replaces every registration the stack's animations/ folder names: a file
// named after a registration ("Walk.glb") replaces it with its animation of
// that name (or its only animation), and any file also replaces each
// registration one of its animations is named after, so one .glb exported
// with every Blender action works. Animations named "REF_..." (the game's own
// clips, exported for reference) are never used. Returns the names replaced;
// problems go to `warnings`. Called when the character loads, so a pack
// applies to the next world entered.
//
// (GunFX) Reload clips are stretched (clip_from_animation). Animations named
// after a clip variant ("PistolReloadEmpty", "PumpShotgunReload@rpg";
// gunfx::split_clip_name) are ADDED as registrations of their own, with the
// base clip's influence and length, after every plain clip is replaced.
// `tracks` receives, per replaced or added registration, the file's length
// and the part tracks: animated nodes that are not bones and are named like
// a part ("mag", "slide"; gunfx::valid_part_name), sampled like the bones.
std::vector<std::string> apply_animation_pack(rig::RetailRig &rig, const AssetStack &stack,
                                              std::vector<std::string> *warnings = nullptr,
                                              gunfx::TrackTable *tracks = nullptr);
// The process-wide hook the character loader calls (no-op without packs).
// (GunFX) Installs the packs' part tracks (gunfx::install_tracks; none
// without a pack).
void apply_installed_animation_pack(rig::RetailRig &rig);

// ===========================================================================
// Lighting props
// ===========================================================================

enum class LightShape : std::uint8_t
{
    point,
    spot
};

struct LightDesc final
{
    Vec3 position{};
    // Linear colour; multiplied by intensity. 1 is about a placed torch.
    Color color{1.0F, 0.78F, 0.52F};
    float intensity{1.0F};
    // Blocks; the light reaches zero there (0.5 .. 64).
    float radius{8.0F};
    // 0 steady .. 1 flickers down to nothing (fire), and how fast.
    float flicker{};
    float flicker_speed{9.0F};
    // 0 none .. 1 pulses down to nothing, cycles per second, phase (0..1).
    float pulse{};
    float pulse_speed{1.0F};
    float pulse_phase{};
    LightShape shape{LightShape::point};
    // Spot lights: the axis, and the full-strength / zero angles (degrees).
    Vec3 direction{0.0F, -1.0F, 0.0F};
    float inner_angle{25.0F};
    float outer_angle{40.0F};
    bool enabled{true};
    // Requested soft shadow. Not drawn in this version (see the report); kept
    // so a mod written now gets shadows when they land.
    bool shadow{};
};

// Something a light, model or effect follows: an entity or player handle
// (resolved each frame by the backend) plus an offset in its frame (x to its
// right, y up, -z ahead of it: the game's camera convention).
struct Attachment final
{
    Handle target{};
    Vec3 offset{};
    // Turn the offset with the target's yaw (and an attached model with it,
    // its +Z, glTF's front, to the target's front).
    bool rotate{true};
};

using TargetLocator = std::function<bool(Handle target, Vec3 &position, float &yaw)>;

class LightStore final
{
  public:
    static constexpr std::size_t max_lights = 4096U;
    static constexpr std::size_t max_lights_per_mod = 1024U;

    struct Entry final
    {
        std::uint64_t id{};
        std::string mod_id;
        LightDesc desc;
        std::optional<Attachment> attachment;
    };

    [[nodiscard]] std::optional<std::uint64_t> create(std::string_view mod_id,
                                                      const LightDesc &desc);
    [[nodiscard]] Entry *find(std::uint64_t id) noexcept;
    [[nodiscard]] const Entry *find(std::uint64_t id) const noexcept;
    bool remove(std::uint64_t id) noexcept;
    void remove_mod(std::string_view mod_id) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }
    [[nodiscard]] std::size_t count_for(std::string_view mod_id) const noexcept;
    [[nodiscard]] const std::vector<Entry> &entries() const noexcept
    {
        return entries_;
    }

  private:
    std::vector<Entry> entries_;
    std::uint64_t next_id_{1U};
};

struct ViewFrustum final
{
    // Inward-facing planes (a, b, c, d): a*x + b*y + c*z + d >= 0 inside.
    std::array<std::array<float, 4>, 6> planes{};
    Vec3 eye{};
};
[[nodiscard]] ViewFrustum make_frustum(Vec3 eye, Vec3 forward, Vec3 right, Vec3 up,
                                       float vertical_fov_radians, float aspect, float near_plane,
                                       float far_plane) noexcept;
[[nodiscard]] bool sphere_visible(const ViewFrustum &frustum, Vec3 center, float radius) noexcept;

// One light as the lit shaders read it: three float4 per light.
struct GpuLight final
{
    float position[3]{};
    float radius{};
    // Colour times intensity times flicker, linear.
    float color[3]{};
    // cos(outer angle) for spot lights, -2 for point lights.
    float spot_cos_outer{-2.0F};
    float direction[3]{0.0F, -1.0F, 0.0F};
    float spot_cos_inner{-1.0F};
};
static_assert(sizeof(GpuLight) == 48U, "the light buffer reads three float4 per light");

struct LightFrame final
{
    // Most important first.
    std::vector<GpuLight> lights;
    // Enabled lights considered, and how many the frustum or range removed.
    std::size_t considered{};
    std::size_t culled{};

    [[nodiscard]] bool empty() const noexcept
    {
        return lights.empty();
    }
    void clear() noexcept;
};

// 0..1 brightness of a flickering/pulsing light at `seconds`. Deterministic.
[[nodiscard]] float light_modulation(const LightDesc &desc, std::uint64_t seed,
                                     double seconds) noexcept;
// (1 - d/r)^2, zero past the radius: the falloff the shaders use.
[[nodiscard]] float light_attenuation(float distance, float radius) noexcept;
// Evaluates, culls (frustum and `max_distance`) and keeps the `max_visible`
// most important, most important first.
void build_light_frame(const LightStore &store, double seconds, const ViewFrustum &frustum,
                       float max_distance, std::size_t max_visible, const TargetLocator &locate,
                       LightFrame &out);

// --- Light clusters ----------------------------------------------------------
//
// The lit shaders find a pixel's mod lights through view-space clusters
// ("froxels"): screen tiles of about tile_pixels across, times depth slices
// that grow geometrically away from the eye. Each frame the GPU lists, per
// cluster, the lights whose sphere touches the cluster's view-space box
// (mod_light_gpu.cpp); a pixel finds its cluster from its world position and
// loops over that list only. Neighbouring pixels read the same list, and the
// cost per pixel follows how many lights overlap there, not how many exist.
// A point off the grid (behind the eye, or off the screen, as the
// first-person body can be) loops over every light instead, and a point
// deeper than any light reaches over none.
//
// The functions below are the reference of that mapping. The shaders mirror
// them exactly (mod_light_hlsl), and the tests prove with them that no light
// is lost: every light that reaches a point is listed in the point's cluster.

// The camera the clusters are laid out for.
struct ClusterView final
{
    Vec3 eye{};
    // Orthonormal camera axes; `forward` points into the screen.
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    // NDC per unit of view-space x and y at depth one: the projection's
    // y_scale / aspect and y_scale.
    float scale_x{1.0F};
    float scale_y{1.0F};
    // The render target in pixels, which decides the number of tiles.
    int width{1};
    int height{1};
};

struct ClusterGrid final
{
    static constexpr int tile_pixels = 64;
    static constexpr int max_tiles_x = 128;
    static constexpr int max_tiles_y = 80;
    static constexpr int slices = 24;
    // Slice 0 runs from the eye to first_depth, slice k (1 .. slices - 2)
    // from first_depth * growth^(k-1) to first_depth * growth^k, and the
    // last one from last_depth to far_depth.
    static constexpr float first_depth = 1.0F;
    static constexpr float last_depth = 160.0F;
    // Points up to this far past the screen's edge (in NDC) still belong to
    // the edge tiles, whose boxes reach that far.
    static constexpr float ndc_margin = 1.02F;
    static constexpr std::uint32_t max_per_cluster = 64U;

    ClusterView view;
    int tiles_x{1};
    int tiles_y{1};
    // No light reaches deeper than this (the deepest point of any sphere).
    float far_depth{};

    [[nodiscard]] std::size_t cluster_count() const noexcept
    {
        return static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y) *
               static_cast<std::size_t>(slices);
    }
    // log2 of the growth from one slice to the next.
    [[nodiscard]] static float log2_growth() noexcept;
    // Where slice `slice` begins (0 for the first).
    [[nodiscard]] static float slice_start(int slice) noexcept;
};

[[nodiscard]] ClusterGrid make_cluster_grid(const ClusterView &view,
                                            std::span<const GpuLight> lights) noexcept;
// A world point in view space: x right, y up, z depth along `forward`.
[[nodiscard]] Vec3 to_view_space(const ClusterView &view, Vec3 position) noexcept;

struct ClusterLookup final
{
    enum class Kind : std::uint8_t
    {
        cluster,  // `cluster` lists the lights
        unlit,    // deeper than every light reaches: none
        off_grid  // every light must be tested
    };
    Kind kind{Kind::off_grid};
    std::uint32_t cluster{};
};
[[nodiscard]] ClusterLookup cluster_of(const ClusterGrid &grid, Vec3 position) noexcept;

// A cluster's view-space box, grown a little so a point the shader's float
// maths puts in a cluster is always inside its box.
struct ClusterBox final
{
    Vec3 minimum{};
    Vec3 maximum{};
};
[[nodiscard]] ClusterBox cluster_box(const ClusterGrid &grid, std::uint32_t cluster) noexcept;

// Per cluster, the lights touching it, in light (importance) order.
struct ClusterLists final
{
    // How many lights touch each cluster (may exceed max_per_cluster) ...
    std::vector<std::uint32_t> counts;
    // ... and the first max_per_cluster of them, at cluster * max_per_cluster.
    std::vector<std::uint16_t> indices;
};
void build_light_clusters(std::span<const GpuLight> lights, const ClusterGrid &grid,
                          ClusterLists &out);

// What one frame's cluster lists held (diagnostics).
struct LightClusterStats final
{
    int tiles_x{};
    int tiles_y{};
    std::size_t clusters{};
    std::size_t lit_clusters{};
    // Light-in-cluster references, and how many did not fit their cluster.
    std::size_t references{};
    std::size_t dropped{};
    std::uint32_t busiest{};
    bool valid{};
};
[[nodiscard]] LightClusterStats cluster_stats(const ClusterGrid &grid,
                                              std::span<const std::uint32_t> counts) noexcept;
// What the shaders compute for a point (the first-person body and tests).
// With no normal every light counts as facing it.
[[nodiscard]] Color light_at(const LightFrame &frame, Vec3 position,
                             const Vec3 *normal = nullptr) noexcept;
[[nodiscard]] Color light_at(std::span<const GpuLight> lights, Vec3 position,
                             const Vec3 *normal = nullptr) noexcept;

// ===========================================================================
// Atmosphere and post layers
// ===========================================================================

struct AtmosphereValues final
{
    // Sky and light (display-referred colours, like world::SkyState).
    std::optional<Color> sky_zenith;
    std::optional<Color> sky_horizon;
    std::optional<Color> fog_color;
    std::optional<Color> ambient;
    std::optional<Color> sun_color;
    // Multipliers of the ambient and sun light.
    std::optional<float> ambient_intensity;
    std::optional<float> sun_intensity;
    // Fog band in blocks from the eye.
    std::optional<float> fog_start;
    std::optional<float> fog_end;
    // Exposure trim in EV, bloom multiplier, and the lens controls.
    std::optional<float> exposure;
    std::optional<float> bloom;
    std::optional<float> saturation;
    std::optional<float> contrast;
    std::optional<float> temperature;
    std::optional<float> vignette;
    // A LUT asset ("luts/dungeon") and how strongly it grades.
    std::string lut;
    float lut_strength{1.0F};
};

struct AtmosphereRegion final
{
    enum class Kind : std::uint8_t
    {
        everywhere,
        box,
        sphere
    };
    Kind kind{Kind::everywhere};
    Vec3 minimum{};
    Vec3 maximum{};
    Vec3 center{};
    float radius{};
    // Blocks over which the layer fades in at the region's edge.
    float feather{4.0F};
};

struct AtmosphereLayerDesc final
{
    AtmosphereValues values;
    AtmosphereRegion region;
    // Higher layers apply over lower ones.
    int priority{};
    float fade_in{1.0F};
    float fade_out{1.0F};
    // 0..1 overall strength.
    float strength{1.0F};
};

// How much of a region the eye is in: 1 inside, 0 beyond `feather`.
[[nodiscard]] float region_weight(const AtmosphereRegion &region, Vec3 eye) noexcept;

struct ResolvedLut final
{
    std::string name;
    float strength{};
};

class AtmosphereStack final
{
  public:
    static constexpr std::size_t max_layers = 64U;

    [[nodiscard]] std::optional<std::uint64_t> create(std::string_view mod_id,
                                                      const AtmosphereLayerDesc &desc);
    bool update(std::uint64_t id, const AtmosphereLayerDesc &desc);
    // Fades the layer out (over fade_out) and then drops it; `immediate` now.
    bool remove(std::uint64_t id, bool immediate = false);
    void remove_mod(std::string_view mod_id) noexcept;
    void clear() noexcept;
    // Advances the fades and measures the regions around the eye.
    void step(float seconds, Vec3 eye);
    // True while any layer has any weight (nothing to apply otherwise).
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return layers_.size();
    }
    [[nodiscard]] float weight(std::uint64_t id) const noexcept;

    // Blends the layers over what the game computed this frame.
    void apply(world::SkyState &sky) const;
    // Lens and bloom (Enhanced presentation) and the exposure trim (EV).
    void apply(PostEffects &effects, float &exposure_ev) const;
    // Fog band in blocks, blended from `start`/`end`; false when no layer sets it.
    [[nodiscard]] bool fog_band(float &start, float &end) const;
    [[nodiscard]] ResolvedLut lut() const;

  private:
    struct Layer final
    {
        std::uint64_t id{};
        std::string mod_id;
        AtmosphereLayerDesc desc;
        float fade{};
        float region{};
        bool removing{};
        std::uint64_t order{};
        [[nodiscard]] float weight() const noexcept
        {
            return fade * region * desc.strength;
        }
    };
    [[nodiscard]] std::vector<const Layer *> ordered() const;
    std::vector<Layer> layers_;
    std::uint64_t next_id_{1U};
    std::uint64_t next_order_{};
};

// Samples a LUT strip (N*N x N) at a colour, trilinear. Used by tests and by
// the CPU path; the GPU applies the same mapping.
[[nodiscard]] Color sample_lut(const Image &lut, Color color) noexcept;
[[nodiscard]] bool valid_lut(const Image &lut) noexcept;

// ===========================================================================
// Camera effects
// ===========================================================================

struct CameraAccessibility final
{
    // Settings "Reduced motion": no shake and no field-of-view pulses.
    bool reduced_motion{};
    // Settings "Explosion camera shake" (0..1) scales every mod shake.
    float shake_scale{1.0F};
};

struct CameraOutput final
{
    // Radians added to the view this frame.
    float yaw{};
    float pitch{};
    float roll{};
    // Degrees added to the vertical field of view.
    float fov{};
    // A full-screen colour over the world (under the HUD).
    Color fade_color{};
    float fade_alpha{};

    [[nodiscard]] bool any() const noexcept
    {
        return yaw != 0.0F || pitch != 0.0F || roll != 0.0F || fov != 0.0F || fade_alpha > 0.0F;
    }
};

class CameraEffects final
{
  public:
    static constexpr std::size_t max_shakes = 16U;
    static constexpr float max_shake_degrees = 4.0F;

    // `strength` 0..1 of the largest shake, for `seconds`, `frequency` Hz.
    void shake(float strength, float seconds, float frequency = 14.0F);
    // Fades the overlay to `alpha` (0 clear .. 1 opaque) of `color` over
    // `seconds`, from wherever it is now.
    void fade(Color color, float alpha, float seconds);
    // Widens (positive) or narrows the view by `degrees`, out and back over
    // `seconds`.
    void fov_pulse(float degrees, float seconds);
    // (GunFX) A small kick of the view: up `pitch` and left `yaw` degrees,
    // there in the first quarter of `seconds` and eased back by its end. Not
    // scaled by the explosion shake setting; none with Reduced motion.
    void kick(float pitch_degrees, float yaw_degrees, float seconds);
    void clear() noexcept;
    void step(float seconds);
    [[nodiscard]] CameraOutput output(const CameraAccessibility &options) const noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    struct Shake final
    {
        float strength{};
        float duration{};
        float frequency{};
        float age{};
        float seed{};
    };
    struct Pulse final
    {
        float degrees{};
        float duration{};
        float age{};
    };
    struct Kick final // (GunFX)
    {
        float pitch{};
        float yaw{};
        float duration{};
        float age{};
    };
    std::vector<Shake> shakes_;
    std::vector<Pulse> pulses_;
    std::vector<Kick> kicks_; // (GunFX)
    Color fade_color_{};
    float fade_from_{};
    float fade_to_{};
    float fade_duration_{};
    float fade_age_{};
    float clock_{};
    std::uint32_t seed_{0x9E3779B9U};
};

// ===========================================================================
// Sounds and music
// ===========================================================================

// Interleaved 16-bit PCM, mono or stereo.
struct PcmBuffer final
{
    int channels{};
    int sample_rate{};
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::size_t frames() const noexcept
    {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0U;
    }
    [[nodiscard]] double seconds() const noexcept
    {
        return sample_rate > 0 ? static_cast<double>(frames()) / sample_rate : 0.0;
    }
};

struct AudioLimits final
{
    std::size_t max_file_bytes{48U * 1024U * 1024U};
    // Decoded length of one sound (music streams and is bounded by bytes only).
    double max_seconds{120.0};
    int min_rate{8000};
    int max_rate{192000};
};

// RIFF/WAVE: PCM 8/16/24/32-bit and IEEE float 32, 1..8 channels (mixed down
// to stereo).
[[nodiscard]] std::optional<PcmBuffer> decode_wav(std::span<const std::uint8_t> bytes,
                                                  std::string *error = nullptr,
                                                  const AudioLimits &limits = {});
// Ogg Vorbis (stb_vorbis).
[[nodiscard]] std::optional<PcmBuffer> decode_ogg(std::span<const std::uint8_t> bytes,
                                                  std::string *error = nullptr,
                                                  const AudioLimits &limits = {});
// Either, by the file's magic.
[[nodiscard]] std::optional<PcmBuffer> decode_audio(std::span<const std::uint8_t> bytes,
                                                    std::string *error = nullptr,
                                                    const AudioLimits &limits = {});

// A decoded sound or an Ogg stream (music), shared by every voice playing it.
class SoundAsset final
{
  public:
    static std::shared_ptr<SoundAsset> from_pcm(PcmBuffer pcm);
    // Keeps the compressed bytes and decodes while playing (music).
    static std::shared_ptr<SoundAsset> from_ogg_stream(std::vector<std::uint8_t> bytes,
                                                       std::string *error = nullptr);
    [[nodiscard]] bool streamed() const noexcept
    {
        return !ogg_.empty();
    }
    [[nodiscard]] const PcmBuffer &pcm() const noexcept
    {
        return pcm_;
    }
    [[nodiscard]] const std::vector<std::uint8_t> &ogg() const noexcept
    {
        return ogg_;
    }
    [[nodiscard]] int channels() const noexcept
    {
        return channels_;
    }
    [[nodiscard]] int sample_rate() const noexcept
    {
        return sample_rate_;
    }

  private:
    PcmBuffer pcm_;
    std::vector<std::uint8_t> ogg_;
    int channels_{};
    int sample_rate_{};
};

enum class AudioBus : std::uint8_t
{
    effects,
    ambience,
    music
};

struct VoiceParams final
{
    float volume{1.0F};
    // Playback rate (0.25 .. 4).
    float pitch{1.0F};
    bool loop{};
    // A positioned sound attenuates and pans; none plays straight (2D).
    std::optional<Vec3> position;
    float min_distance{2.0F};
    float max_distance{32.0F};
    AudioBus bus{AudioBus::effects};
    float fade_in{};
};

struct MixLevels final
{
    float master{1.0F};
    float music{1.0F};
    float effects{1.0F};
    float ambience{1.0F};
    bool muted{};
};

// Gain and pan of a positioned sound: inverse-distance between min and max
// distance (0 beyond), equal-power pan by the angle to the listener's right.
struct SpatialGain final
{
    float left{1.0F};
    float right{1.0F};
};
[[nodiscard]] SpatialGain spatial_gain(Vec3 listener, float listener_yaw, Vec3 source,
                                       float min_distance, float max_distance) noexcept;

// (Polish) A diagnostic tap on a Mixer: the first `seconds` of what it mixed
// and notes of what it was asked to do, for proving a mod's sound without
// ears (OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV). The whole capture is allocated
// up front, so the audio thread only copies. Attach with Mixer::set_recorder;
// write after detaching it.
class MixRecorder final
{
  public:
    struct Mark final
    {
        std::uint64_t frame{}; // output frame (from the first captured one)
        double seconds{};      // game-thread wall clock since the recorder was made
        std::string text;
    };

    MixRecorder(int sample_rate, double seconds);

    [[nodiscard]] int sample_rate() const noexcept
    {
        return rate_;
    }
    [[nodiscard]] std::size_t frames() const noexcept
    {
        return captured_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool full() const noexcept
    {
        return frames() * 2U >= samples_.size();
    }
    [[nodiscard]] const std::vector<Mark> &marks() const noexcept
    {
        return marks_;
    }
    // Audio thread: keeps what still fits of `count` interleaved stereo frames.
    void capture(const float *interleaved, std::size_t count) noexcept;
    void mark(std::uint64_t frame, std::string text);
    // A 16-bit stereo PCM WAV of the capture, and beside it "<wav>.marks.txt":
    // one "<frame> <audio s> <wall s> <text>" line per mark.
    bool write(const std::filesystem::path &wav, std::string &error) const;

  private:
    int rate_{};
    std::vector<std::int16_t> samples_;
    std::atomic<std::size_t> captured_{}; // written by the audio thread
    std::chrono::steady_clock::time_point made_;
    double first_block_seconds_{-1.0}; // wall clock of the first captured block
    std::vector<Mark> marks_;
};

// A small software mixer. The audio thread calls mix(); everything else is
// called from the game thread. Safe to use with no device (tests).
class Mixer final
{
  public:
    static constexpr std::size_t max_voices = 64U;

    explicit Mixer(int output_rate = 48000);
    ~Mixer();
    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;

    [[nodiscard]] int output_rate() const noexcept
    {
        return output_rate_;
    }
    // 0 when nothing could be started (no free voice, empty asset).
    std::uint64_t play(std::shared_ptr<const SoundAsset> sound, const VoiceParams &params);
    void stop(std::uint64_t voice, float fade_seconds = 0.0F);
    void set_position(std::uint64_t voice, Vec3 position);
    void set_volume(std::uint64_t voice, float volume);
    [[nodiscard]] bool playing(std::uint64_t voice) const;
    // Crossfades from whatever music plays to `track` (nullptr fades to silence).
    void play_music(std::shared_ptr<const SoundAsset> track, float crossfade_seconds, float volume,
                    bool loop);
    // (fix) Whether a mod's music track is playing (not fading out to silence):
    // the game's own soundtrack steps aside meanwhile.
    [[nodiscard]] bool music_playing() const;
    void set_listener(Vec3 position, float yaw);
    void set_levels(const MixLevels &levels);
    void stop_all();
    [[nodiscard]] std::size_t voice_count() const;
    // Interleaved stereo float, `frames` frames, added to nothing (overwrites).
    void mix(float *output, std::size_t frames);
    // (Polish) Diagnostics: from now on every mixed block also goes to
    // `recorder` (nullptr detaches), and play/music/stop calls leave a mark
    // at the output frame they take effect. mark() adds one of the caller's.
    void set_recorder(MixRecorder *recorder);
    void mark(std::string_view text);

  private:
    struct Voice;
    [[nodiscard]] Voice *find_voice(std::uint64_t id);
    [[nodiscard]] const Voice *find_voice(std::uint64_t id) const;
    std::uint64_t start_voice(std::shared_ptr<const SoundAsset> sound, const VoiceParams &params,
                              bool music);
    mutable std::mutex mutex_;
    int output_rate_{};
    std::vector<std::unique_ptr<Voice>> voices_;
    std::uint64_t next_id_{1U};
    std::uint64_t music_voice_{};
    Vec3 listener_{};
    float listener_yaw_{};
    MixLevels levels_{};
    MixRecorder *recorder_{};
    std::uint64_t mixed_{};          // frames mixed so far
    std::uint64_t recorder_start_{}; // mixed_ when recorder_ was attached
    void record(std::string text);   // under mutex_
};

// ===========================================================================
// Effects
// ===========================================================================

// Recipes defined by mods from Luau tables. The vfx system keeps a pointer to
// every recipe it is playing, so a definition is never freed while the
// service lives; redefining a name retires the old one instead.
class EffectLibrary final
{
  public:
    static constexpr std::size_t max_definitions = 512U;
    static constexpr std::size_t max_definitions_per_mod = 128U;

    // Throws ScriptError naming the field ("emitters[2].life: ...").
    [[nodiscard]] static vfx::EffectDesc parse(const Value &table);
    const vfx::EffectDesc &define(std::string_view mod_id, std::string_view name,
                                  const Value &table);
    // "name" (the calling mod's, then a built-in recipe) or "mod:name".
    [[nodiscard]] const vfx::EffectDesc *find(std::string_view mod_id, std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return live_.size();
    }

  private:
    struct Stored final
    {
        std::string key; // "mod:name"
        vfx::EffectDesc desc;
    };
    std::map<std::string, Stored *, std::less<>> live_;
    std::deque<std::unique_ptr<Stored>> storage_;
};

// ===========================================================================
// Model instances
// ===========================================================================

struct ModelInstanceDesc final
{
    std::string model; // asset name ("models/lantern")
    Vec3 position{};
    // Degrees.
    float yaw{};
    float pitch{};
    float roll{};
    float scale{1.0F};
    Color tint{1.0F, 1.0F, 1.0F};
    std::string animation;
    bool loop{true};
    float speed{1.0F};
    bool visible{true};
};

class ModelStore final
{
  public:
    static constexpr std::size_t max_instances = 4096U;
    static constexpr std::size_t max_instances_per_mod = 1024U;

    struct Entry final
    {
        std::uint64_t id{};
        std::string mod_id;
        ModelInstanceDesc desc;
        std::optional<Attachment> attachment;
        float time{};
    };

    [[nodiscard]] std::optional<std::uint64_t> create(std::string_view mod_id,
                                                      const ModelInstanceDesc &desc);
    [[nodiscard]] Entry *find(std::uint64_t id) noexcept;
    [[nodiscard]] const Entry *find(std::uint64_t id) const noexcept;
    bool remove(std::uint64_t id) noexcept;
    void remove_mod(std::string_view mod_id) noexcept;
    void clear() noexcept;
    void step(float seconds) noexcept;
    [[nodiscard]] const std::vector<Entry> &entries() const noexcept
    {
        return entries_;
    }
    [[nodiscard]] std::size_t count_for(std::string_view mod_id) const noexcept;

  private:
    std::vector<Entry> entries_;
    std::uint64_t next_id_{1U};
};

// Loaded models by resolved file, shared by every instance.
class ModelCache final
{
  public:
    [[nodiscard]] std::shared_ptr<const Model> get(const AssetStack &stack, std::string_view name,
                                                   std::string *error = nullptr);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return models_.size();
    }

  private:
    std::uint64_t generation_{~0ULL};
    std::map<std::string, std::shared_ptr<const Model>, std::less<>> models_;
    std::map<std::string, std::string, std::less<>> failures_;
};

// ===========================================================================
// Creatures (mod entities as a client draws them)
// ===========================================================================

// One mod creature or NPC this frame, as the app hands it to the renderer from
// Gameplay's replicated entities (GameplayService::client_entities).
struct CreatureView final
{
    std::uint64_t id{};
    // "mod:boar": messages name it.
    std::string kind;
    // Asset names. A model that cannot be loaded draws a stand-in box of the
    // creature's size; `texture` replaces the model's base-colour textures.
    std::string model;
    std::string texture;
    // The replicated animation state: idle, walk, run, attack, hurt, death or
    // a mod's own clip name.
    std::string state{"idle"};
    // Drawn over the head when not empty.
    std::string name;
    // Feet, and the game's yaw (radians, 0 faces -Z). A glTF model's +Z is its
    // front, as the glTF convention has it.
    Vec3 position{};
    float yaw{};
    float health{1.0F};
    float width{0.6F};
    float height{1.8F};
    // (mod-ai) Nose up in radians (projectiles in flight), and a size factor
    // of the model.
    float pitch{};
    float scale{1.0F};
};

// The clip of a model a replicated state plays: the clip of that name (any
// case; "Armature|Walk" and "walk_cycle" both answer "walk"), then the state's
// stand-ins (run: walk; walk: run; hurt: hit, damage; death: die, dead; idle:
// stand). -1, the bind pose, when nothing matches.
[[nodiscard]] int find_state_clip(const Model &model, std::string_view state) noexcept;
// idle, walk, run and a mod's own states loop; attack, hurt and death play
// once, death holding its last frame.
[[nodiscard]] bool state_loops(std::string_view state) noexcept;

// One clip in a blend of several.
struct PoseLayer final
{
    int animation{-1};
    float seconds{};
    bool loop{true};
    float weight{1.0F};
};
// evaluate_pose over several clips at once: each node's translation, rotation
// and scale are blended by the layers' weights.
void evaluate_blended_pose(const Model &model, std::span<const PoseLayer> layers,
                           std::vector<Matrix4> &world);

// A creature's animation: the clip of its replicated state, cross-faded over
// blend_seconds whenever the state changes.
class CreatureAnimator final
{
  public:
    static constexpr float blend_seconds = 0.2F;

    // Every frame, with the entity's state: a new state starts a cross-fade
    // from wherever the old one was. A different model starts over.
    void play(const Model &model, std::string_view state);
    // `speed` scales the playback (walk and run follow the ground speed).
    void step(float seconds, float speed = 1.0F);
    // (fix, wave 5) A creature of its own: its looping clips start at a phase
    // and run at a pace (0.92 to 1.08) drawn from `seed` (its entity id), so
    // a herd spawned together does not breathe and step in lockstep. Clips
    // that play once keep their timing. Without a call: phase 0, pace 1.
    void vary(std::uint64_t seed) noexcept;
    [[nodiscard]] std::span<const PoseLayer> layers() const noexcept
    {
        return {layers_.data(), count_};
    }
    [[nodiscard]] const std::string &state() const noexcept
    {
        return state_;
    }
    // A clip that plays once has reached its end (none counts as ended).
    [[nodiscard]] bool finished() const noexcept;

  private:
    const Model *model_{};
    std::string state_;
    // [0] the state playing, [1] the one fading out.
    std::array<PoseLayer, 2> layers_{};
    std::array<float, 2> durations_{};
    std::size_t count_{};
    float fade_{1.0F};
    float phase_{}; // of a looping clip's length (vary)
    float pace_{1.0F};
};

// ===========================================================================
// The service
// ===========================================================================

// What the service needs from the game, implemented by the app
// (src/app_mod_graphics.inc). Everything is main-thread.
class GraphicsBackend
{
  public:
    virtual ~GraphicsBackend() = default;

    // Where an entity or player is now; false when it is gone.
    [[nodiscard]] virtual bool locate(Handle target, Vec3 &position, float &yaw) const = 0;
    // The Enhanced presentation draws effects; the Original has no effects layer.
    [[nodiscard]] virtual bool effects_available() const = 0;
    // Effects are not available YET (no world drawn so far this session): an
    // effect spawned now waits for the first frame instead of being refused.
    [[nodiscard]] virtual bool effects_pending() const
    {
        return false;
    }
    virtual vfx::EffectHandle spawn_effect(const vfx::EffectDesc &desc,
                                           const vfx::SpawnParams &params) = 0;
    [[nodiscard]] virtual bool effect_alive(vfx::EffectHandle handle) const = 0;
    virtual void move_effect(vfx::EffectHandle handle, Vec3 position, Vec3 velocity) = 0;
    virtual void kill_effect(vfx::EffectHandle handle, bool immediate) = 0;
    // The mixer of mod sounds, or nullptr when there is no audio device. With
    // `open` false it never opens one (stopping and following voices).
    [[nodiscard]] virtual Mixer *mixer(bool open) = 0;
};

struct ServiceLimits final
{
    std::size_t max_effects_per_mod{256U};
    std::size_t max_voices_per_mod{32U};
    std::size_t max_sound_assets{256U};
};

class GraphicsService final
{
  public:
    explicit GraphicsService(GraphicsBackend &backend, ServiceLimits limits = {});
    ~GraphicsService();
    GraphicsService(const GraphicsService &) = delete;
    GraphicsService &operator=(const GraphicsService &) = delete;

    // --- Session ---------------------------------------------------------
    // The enabled mods' asset folders in load order. Rebuilds the stack and
    // drops every cache; installs it for the content loaders.
    void set_sources(std::vector<AssetSource> sources);
    // Asked before the service first resolves an asset after each step(), so
    // the app can install the session's packs before scripts look for one.
    void set_source_refresh(std::function<void()> refresh);
    void refresh_sources();
    // Drops everything one mod made (its VM is going away).
    void release_mod(std::string_view mod_id);
    // Drops everything every mod made, keeping the asset stack.
    void reset();
    [[nodiscard]] const AssetStack &assets() const noexcept
    {
        return assets_->stack;
    }
    [[nodiscard]] BlockTextureTable &block_tiles() noexcept
    {
        return assets_->block_tiles;
    }

    // --- Per frame (client) ---------------------------------------------
    // Advances clocks, fades, attachments and finished voices.
    void step(float seconds, Vec3 eye);
    [[nodiscard]] double clock() const noexcept
    {
        return clock_;
    }
    // True when no mod has made anything this service draws or plays.
    [[nodiscard]] bool idle() const noexcept;

    [[nodiscard]] LightStore &lights() noexcept
    {
        return lights_;
    }
    [[nodiscard]] const LightStore &lights() const noexcept
    {
        return lights_;
    }
    [[nodiscard]] ModelStore &models() noexcept
    {
        return models_;
    }
    [[nodiscard]] const ModelStore &models() const noexcept
    {
        return models_;
    }
    [[nodiscard]] ModelCache &model_cache() noexcept
    {
        return model_cache_;
    }
    [[nodiscard]] AtmosphereStack &atmosphere() noexcept
    {
        return atmosphere_;
    }
    [[nodiscard]] const AtmosphereStack &atmosphere() const noexcept
    {
        return atmosphere_;
    }
    [[nodiscard]] CameraEffects &camera() noexcept
    {
        return camera_;
    }
    [[nodiscard]] EffectLibrary &effect_library() noexcept
    {
        return effects_;
    }
    // (GunFX) Weapon parts, clip cues and viewmodel motion the client mods
    // declared (openminerz/modding/gunfx.hpp); empty without one.
    [[nodiscard]] gunfx::Library &gunfx() noexcept
    {
        return gunfx_;
    }
    [[nodiscard]] const gunfx::Library &gunfx() const noexcept
    {
        return gunfx_;
    }
    // The mod creatures to draw this frame, set by the app from Gameplay's
    // replicated entities (empty: none, and nothing is drawn).
    [[nodiscard]] std::vector<CreatureView> &creatures() noexcept
    {
        return creatures_;
    }
    [[nodiscard]] const std::vector<CreatureView> &creatures() const noexcept
    {
        return creatures_;
    }
    [[nodiscard]] const CreatureView *creature(std::uint64_t id) const noexcept;
    // Effects spawned before the first frame, still waiting for it.
    [[nodiscard]] std::size_t pending_effects() const noexcept
    {
        return pending_effects_.size();
    }
    [[nodiscard]] TargetLocator locator() const;
    // A loaded LUT by asset name (cached), or nullptr.
    [[nodiscard]] const Image *lut(std::string_view name);
    // A decoded (or streamed) mod sound by asset name, cached.
    [[nodiscard]] std::shared_ptr<const SoundAsset> sound(std::string_view name,
                                                          std::string *error = nullptr);

    // --- What scripts call (also usable from C++) ----------------------
    Handle spawn_effect(std::string_view mod_id, std::string_view recipe,
                        const vfx::SpawnParams &params, std::optional<Attachment> attachment);
    bool stop_effect(Handle handle, bool immediate);
    bool move_effect(Handle handle, Vec3 position, Vec3 velocity);
    Handle play_sound(std::string_view mod_id, std::string_view name, const VoiceParams &params,
                      std::optional<Attachment> attachment);
    bool stop_sound(Handle handle, float fade_seconds);
    bool play_music(std::string_view mod_id, std::string_view name, float crossfade_seconds,
                    float volume, bool loop);
    void stop_music(float fade_seconds);

  private:
    struct EffectRecord final
    {
        std::uint64_t id{};
        std::string mod_id;
        vfx::EffectHandle handle{};
        std::optional<Attachment> attachment;
        Vec3 last_position{};
    };
    // An effect spawned while effects_pending(): spawned by the first step()
    // that has effects, dropped if they never come.
    struct PendingEffect final
    {
        std::uint64_t id{};
        std::string mod_id;
        const vfx::EffectDesc *desc{};
        vfx::SpawnParams params;
        std::optional<Attachment> attachment;
        float age{};
    };
    static constexpr float pending_effect_seconds = 10.0F;
    void spawn_pending_effects(float seconds);
    struct SoundRecord final
    {
        std::uint64_t id{};
        std::string mod_id;
        std::uint64_t voice{};
        std::optional<Attachment> attachment;
    };

    GraphicsBackend &backend_;
    ServiceLimits limits_;
    std::shared_ptr<PresentationAssets> assets_;
    LightStore lights_;
    ModelStore models_;
    ModelCache model_cache_;
    AtmosphereStack atmosphere_;
    CameraEffects camera_;
    EffectLibrary effects_;
    gunfx::Library gunfx_; // (GunFX)
    std::vector<EffectRecord> effect_records_;
    std::vector<PendingEffect> pending_effects_;
    std::vector<CreatureView> creatures_;
    std::vector<SoundRecord> sound_records_;
    std::map<std::string, std::shared_ptr<const SoundAsset>, std::less<>> sounds_;
    std::map<std::string, Image, std::less<>> luts_;
    std::uint64_t next_effect_id_{1U};
    std::uint64_t next_sound_id_{1U};
    double clock_{};
    std::function<void()> refresh_;
    std::uint64_t steps_{};
    std::uint64_t refreshed_at_{~0ULL};
};

// Registers the `light`, `fx`, `model`, `atmosphere`, `sound`, `music`,
// `camera`, `assets` and (GunFX) `gunfx` libraries (client side,
// presentation capability).
void register_graphics_api(ApiRegistry &registry, ScriptHost &host, GraphicsService &service);

// --- Script value helpers (exposed for tests) ------------------------------

// A colour from {r, g, b}, {r=, g=, b=}, a vector or "#rrggbb".
[[nodiscard]] Color color_from_value(const Value &value);
[[nodiscard]] LightDesc light_from_value(const Value &table, const LightDesc &base = {});
[[nodiscard]] AtmosphereLayerDesc atmosphere_from_value(const Value &table);
[[nodiscard]] ModelInstanceDesc model_instance_from_value(const Value &table,
                                                          const ModelInstanceDesc &base = {});

} // namespace openminerz::modding::graphics
