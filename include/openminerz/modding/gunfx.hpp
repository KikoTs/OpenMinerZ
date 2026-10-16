#pragma once

// GunFX: weapon parts, animation events and procedural first-person motion
// for client mods (docs/modding/animation.md).
//
// PRESENTATION ONLY. A client mod names parts of the retail held-item models
// (cut out of the player's own installed content by boxes in the model's own
// space), animates them from its clip files, puts timed cues into clips
// (sounds, part attach/detach/show/hide/drop, a small camera kick) and turns
// on MW-style viewmodel motion. Nothing here reaches the simulation, the wire,
// a save, the aim or any gameplay timing: the clips keep the game's clock
// (openminerz/world/player_animation.hpp decides which clip plays when and
// for how long). With no mod calling `gunfx.*` the Library is empty and every
// hook in the renderer and the app returns at once.
//
// Pieces, all pure CPU (tests/test_mod_gunfx.cpp):
//   Library            what the mods declared (parts per model, cues per clip,
//                      motion settings), per mod, later mods winning.
//   *_from_value       the `gunfx` script library's argument parsing.
//   ClipTracks         the part tracks of the pack's clip files, read by the
//                      animation pack loader beside the bone keys.
//   cut_parts          which triangles of a retail model belong to each part.
//   PartPose/part_pose where each part is at a clip time (a pure function of
//                      the clip and the time, so a capture at a fixed progress
//                      and a player seen mid-reload are both exact).
//   CueCursor          which cues a playing clip passed this frame, and
//                      whether it ended or was cut short.
//   Motion             the procedural first-person motion (exact spring steps,
//                      frame-rate independent).
//
// MODEL SPACE: the space of a retail held model after its own mesh
// transforms, before the hand (what tools/ContentBridge writes as .omsm).
// For every retail gun the barrel points along -X, up is +Z, the gun's right
// side is +Y, one unit is one block.

#include "openminerz/inventory.hpp"
#include "openminerz/modding/api.hpp"
#include "openminerz/retail_rig.hpp"
#include "openminerz/vfx.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::modding::gunfx
{

using Vec3 = vfx::Vec3;

// ===========================================================================
// Definitions
// ===========================================================================

struct Box final
{
    Vec3 min{};
    Vec3 max{};

    [[nodiscard]] bool contains(Vec3 point) const noexcept
    {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
};

inline constexpr std::size_t max_parts_per_model = 32U;
inline constexpr std::size_t max_cues_per_clip = 128U;
inline constexpr std::size_t max_boxes_per_part = 16U;

struct PartSpec final
{
    std::string name;
    // Triangles whose centroid lies in a box and in no exclude box.
    std::vector<Box> boxes;
    std::vector<Box> exclude;
    // Only this retail mesh part (0-based); with no box, the whole mesh.
    int mesh{-1};
    // Whole connected islands whose bounds' centre lies in a box.
    bool islands{};
    // The same triangles as another part, drawn as a part of its own.
    std::string copy;
    // The pack's own glTF mesh instead of retail triangles: its origin at
    // `pivot`, turned by `rotation` (degrees about X, Y, Z) and scaled.
    std::string model;
    Vec3 rotation{};
    float scale{1.0F};
    // Rotation and scale centre in model space; the selection's bounds'
    // centre when absent.
    std::optional<Vec3> pivot;
    bool hidden{};
};

enum class CueAction : std::uint8_t
{
    sound,
    show,
    hide,
    attach,
    detach,
    drop,
    kick,
};

enum class CueAudience : std::uint8_t
{
    all,
    self,   // your own first-person player only
    others, // other players only
};

struct Cue final
{
    // Seconds of the authored clip, or (fraction) a share 0..1 of the clip.
    float time{};
    bool fraction{};
    CueAction action{CueAction::sound};
    CueAudience who{CueAudience::all};
    std::string part;  // show, hide, attach, detach, drop
    std::string sound; // sound: an asset name ("sounds/mag_out")
    std::string bone;  // attach (and show): a rig bone name ("PropLeft")
    float volume{1.0F};
    float pitch{1.0F};
    float volume_jitter{};
    float pitch_jitter{};
    // attach (and show on a bone): seconds to ease from the gun into the
    // hand; detach: from the hand back onto the track.
    float blend{0.1F};
    // drop: m/s in the player's frame (x right, y up, -z ahead), spin (rad/s
    // about the same axes) and how long the prop stays.
    Vec3 velocity{0.0F, -1.0F, 0.0F};
    Vec3 spin{};
    float life{5.0F};
    // kick: degrees (x pitch up, y yaw left, z roll), springing back over
    // `kick_seconds`.
    Vec3 kick{};
    float kick_seconds{0.15F};
};

struct ClipCues final
{
    std::string mod_id;
    std::string clip;  // registration name, variants included
    std::string model; // empty: any model
    std::vector<Cue> cues; // sorted by time
    std::vector<std::string> mute;
    // Authored length in seconds for a clip with no file in the pack.
    std::optional<float> length;
    float remote_volume{0.6F};
    float remote_distance{24.0F};
};

// The families of PlayerAnimationMode, named as their clips are.
enum class Family : std::uint8_t
{
    generic,
    fist,
    pick,
    block,
    grenade,
    pistol,
    smg,
    gun,
    rifle,
    pump_shotgun,
    lmg,
    rpg,
    laser_gun,
    laser_pistol,
    laser_rifle,
    laser_smg,
    laser_shotgun,
    count,
};
[[nodiscard]] std::string_view family_name(Family family) noexcept;
[[nodiscard]] std::optional<Family> family_from_name(std::string_view name) noexcept;
[[nodiscard]] Family family_of(PlayerAnimationMode mode) noexcept;
[[nodiscard]] bool gun_family(Family family) noexcept;

// Multipliers of the built-in motion (1 = the tuned default, 0 = off).
struct Tuning final
{
    float sway{1.0F};
    float bob{1.0F};
    float roll{1.0F};
    float sprint{1.0F};
    float jump{1.0F};
    float breathe{1.0F};
    float recoil{1.0F};
    float switch_ease{1.0F};
    float aim_ease{1.0F};    // 0 retail linear Shoulder .. 1 eased
    float aim_damping{0.3F}; // share of sway/bob/roll/breathe kept while aimed
    float weight{1.0F};      // heavier: slower springs, more lag, smaller kicks
};

struct MotionSettings final
{
    std::string mod_id;
    Tuning base;
    std::array<Tuning, static_cast<std::size_t>(Family::count)> families{};

    [[nodiscard]] const Tuning &tuning(Family family) const noexcept
    {
        return families[static_cast<std::size_t>(family)];
    }
};

// --- Script values (throw ScriptError with a readable message) ------------

[[nodiscard]] std::vector<PartSpec> parts_from_value(const Value &parts);
[[nodiscard]] ClipCues cues_from_value(std::string_view clip, const Value &cues,
                                       const Value &options);
// nil or {enabled=false}: none.
[[nodiscard]] std::optional<MotionSettings> motion_from_value(const Value &settings);

[[nodiscard]] bool valid_part_name(std::string_view name) noexcept;

// --- Clip names and variants ------------------------------------------------

// "PistolReloadEmpty@rpg" -> base "PistolReload", empty, model "rpg".
struct ClipName final
{
    std::string_view base;
    bool empty{};
    std::string_view model;
};
[[nodiscard]] ClipName split_clip_name(std::string_view name) noexcept;
// The variant names a held model / an empty reload look for, best first:
// "XEmpty@gen/m", "XEmpty@m", "XEmpty", "X@gen/m", "X@m", then "X" itself
// (`generation` "classic" or "modern"; empty: no generation's own).
[[nodiscard]] std::vector<std::string> variant_names(std::string_view base, std::string_view model,
                                                     bool empty, std::string_view generation = {});
// A variant's model: "ak", or one generation's "classic/ak", "modern/ak".
[[nodiscard]] bool valid_variant_model(std::string_view model) noexcept;
// Reload clips are stretched to the game's reload window.
[[nodiscard]] bool reload_clip(std::string_view name) noexcept;

// The retail held model an item draws ("ak", "pistol", ...; empty when none),
// the stem of its DerivedData/<model>.omsm.
[[nodiscard]] std::string_view held_model_name(std::uint16_t item) noexcept;

// ===========================================================================
// What every mod declared
// ===========================================================================

class Library final
{
  public:
    void define_parts(std::string_view mod_id, std::string_view model, std::vector<PartSpec> parts);
    void define_cues(ClipCues cues);
    void define_motion(std::string_view mod_id, std::optional<MotionSettings> motion);
    void release_mod(std::string_view mod_id);
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept
    {
        return parts_.empty() && cues_.empty() && motions_.empty();
    }
    // Changes whenever a definition does (renderers rebuild their caches).
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return revision_;
    }
    // `generation` "classic" or "modern": "classic/ak" wins over "ak".
    [[nodiscard]] const std::vector<PartSpec> *parts(std::string_view generation,
                                                     std::string_view model) const;
    // The cue list for `clip` with `model` in hand: one for that model wins
    // over one for any model.
    [[nodiscard]] const ClipCues *cues(std::string_view clip, std::string_view model) const;
    [[nodiscard]] const MotionSettings *motion() const noexcept;
    // Whether the cue list chosen for `clip` (and its variants, as the
    // renderer chooses them) mutes the retail cue `cue`.
    [[nodiscard]] bool mutes(std::string_view clip, std::string_view model, bool empty,
                             std::string_view cue, std::string_view generation = {}) const;
    [[nodiscard]] bool any_cues() const noexcept
    {
        return !cues_.empty();
    }

  private:
    template <typename Value> struct Entry final
    {
        std::string mod_id;
        Value value;
    };
    // Per key, every mod's definition in call order: the last one wins, and
    // an earlier one comes back when a later mod goes.
    std::map<std::string, std::vector<Entry<std::vector<PartSpec>>>, std::less<>> parts_;
    std::map<std::string, std::vector<Entry<ClipCues>>, std::less<>> cues_;
    std::vector<MotionSettings> motions_;
    std::uint64_t revision_{1U};
};

// ===========================================================================
// Part tracks (the pack's clip files)
// ===========================================================================

// A part's motion keys at 30 Hz from the clip's start, on the clip's own
// clock (a stretched reload's keys already span the game's clip). An empty
// channel is identity.
struct PartTrack final
{
    std::string part;
    std::vector<Vec3> translation;
    std::vector<std::array<float, 4>> rotation; // x y z w
    std::vector<Vec3> scale;
};

struct ClipTracks final
{
    std::string registration;
    // The authored file's length, and whether it was stretched onto the
    // registration's (the game's) length.
    float authored_seconds{};
    bool stretched{};
    std::int64_t duration_ticks{};
    std::vector<PartTrack> parts;

    [[nodiscard]] const PartTrack *find(std::string_view part) const noexcept;
};

// Every registration the installed packs give part tracks or a length, by name.
using TrackTable = std::map<std::string, ClipTracks, std::less<>>;
// Installed by the animation pack loader (apply_installed_animation_pack),
// read by the renderer; null when no pack clip is installed.
[[nodiscard]] std::shared_ptr<const TrackTable> installed_tracks() noexcept;
void install_tracks(std::shared_ptr<const TrackTable> table) noexcept;

// Where a cue fires, in clip ticks, for a registration of `duration_ticks`:
// `at` is a share of it; `time` is authored seconds, stretched by the
// file's (or the cue list's) authored length for a stretched clip.
[[nodiscard]] std::int64_t cue_ticks(const Cue &cue, const ClipCues &list, const ClipTracks *tracks,
                                     std::int64_t duration_ticks) noexcept;

// ===========================================================================
// Cutting parts out of a model
// ===========================================================================

// One retail mesh part in model space.
struct MeshTriangles final
{
    std::span<const std::array<float, 3>> positions;
    std::span<const std::uint32_t> indices;
};

struct CutPart final
{
    // Triangle numbers (index / 3) per mesh.
    std::vector<std::vector<std::uint32_t>> triangles;
    Vec3 pivot{};
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::size_t count{};
};

struct CutResult final
{
    std::vector<CutPart> parts; // same order as the specs
    // Per mesh, per triangle: the part that took it from the body (-1: none).
    std::vector<std::vector<int>> owner;
};

[[nodiscard]] CutResult cut_parts(std::span<const PartSpec> parts,
                                  std::span<const MeshTriangles> meshes);

// ===========================================================================
// Part poses at a clip time
// ===========================================================================

// A part's motion in model space (row vectors, as rig::Matrix):
// T(-pivot) * S * R * T(pivot + t).
[[nodiscard]] rig::Matrix part_motion(const PartTrack *track, std::int64_t position_ticks,
                                      Vec3 pivot) noexcept;

struct PartState final
{
    bool visible{true};
    // Dropped (hidden until the clip ends); the bone below is kept, so the
    // drop starts where the part was carried.
    bool dropped{};
    // Riding a rig bone since the cue at `attach_cue` (an index into the
    // cue list), or -1.
    int bone{-1};
    int attach_cue{-1};
    // Easing from the gun into the hand over the attach cue's `blend`
    // (1: in the hand).
    float attach_weight{1.0F};
    // Easing back from the bone onto the track: the detach cue and how far
    // through its blend this time is (1: done).
    int detach_cue{-1};
    float detach_weight{1.0F};
};

// Each part's state at `position_ticks` of a clip with cues `list` (null:
// every part at rest, hidden ones hidden). `bone_index` resolves a bone name.
void part_states(std::span<const PartSpec> parts, const ClipCues *list, const ClipTracks *tracks,
                 std::int64_t duration_ticks, std::int64_t position_ticks,
                 const std::function<int(std::string_view)> &bone_index,
                 std::vector<PartState> &states);

// ===========================================================================
// Firing cues
// ===========================================================================

struct CueCursor final
{
    const void *clip{}; // identity of the playing clip (its registration)
    std::int64_t position{};
    bool started{};

    struct Step final
    {
        // The cues to fire, in order (indices into the list's cues).
        std::vector<int> fired;
        // The clip that played before ended (reached its end) or was cut short.
        bool ended{};
        bool cut{};
        // A new instance of a clip started (a shot, a reload cycle).
        bool restarted{};
    };
    // One frame of a channel: `clip` null when the channel plays nothing.
    // `late_start` ticks: a clip first seen further in than that (joined
    // late, a capture) fires nothing it already passed.
    Step advance(const void *clip, std::int64_t position, std::int64_t duration, bool looping,
                 const ClipCues *list, const ClipTracks *tracks, std::int64_t late_start = 1'500'000);
    void reset() noexcept
    {
        *this = {};
    }

  private:
    std::int64_t duration_{};
    std::int64_t last_step_{};
    bool looping_{};
    // Played backwards (a Shoulder clip lowering the sights), or joined past
    // its middle: nothing fires until it is seen playing forward.
    bool reversed_{};
};

// ===========================================================================
// Procedural first-person motion
// ===========================================================================

struct MotionInput final
{
    float seconds{};    // frame time
    float yaw{};        // camera, radians
    float pitch{};
    Vec3 velocity{};    // world, blocks per second
    bool grounded{true};
    bool sprinting{};
    bool flying{};
    float aim{};        // 0 hip .. 1 aimed
    bool shot{};        // a shot clip started this frame
    bool switched{};    // the item in hand changed this frame
    float kick_roll{};  // degrees: a cue's kick rolls the weapon
    bool reduced_motion{};
    Family family{Family::generic};
    std::uint32_t seed{};
};

struct MotionOutput final
{
    // The offset applied to the viewmodel in its camera's space (XNA: x right,
    // y up, -z ahead), row vectors: p' = p * offset.
    rig::Matrix offset{rig::identity()};
    // The eased share of the Shoulder clip (aim_ease), as a function of the
    // linear one; identity without motion.
    float aim_ease{};
};

// A damped spring stepped exactly (frame-rate independent): frequency in Hz,
// damping ratio (1 = critical).
struct Spring final
{
    float position{};
    float velocity{};

    void step(float target, float frequency, float damping, float seconds) noexcept;
};

class Motion final
{
  public:
    MotionOutput step(const MotionSettings &settings, const MotionInput &input) noexcept;
    void reset() noexcept;
    // Eases a Shoulder clip's share (0..1) by `amount` (0 linear .. 1 eased).
    [[nodiscard]] static float ease_aim(float share, float amount) noexcept;

  private:
    bool started_{};
    float last_yaw_{};
    float last_pitch_{};
    bool was_grounded_{true};
    float fall_speed_{};
    float bob_phase_{};
    float bob_amount_{};
    float clock_{};
    std::uint32_t random_{0x9E3779B9U};
    Spring sway_yaw_;
    Spring sway_pitch_;
    Spring roll_;
    Spring sprint_;
    Spring vertical_;
    Spring recoil_back_;
    Spring recoil_pitch_;
    Spring recoil_yaw_;
    Spring recoil_roll_;
    Spring lowered_;
};

} // namespace openminerz::modding::gunfx
