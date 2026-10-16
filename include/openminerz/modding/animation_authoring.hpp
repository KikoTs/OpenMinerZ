#pragma once

// Authoring animation packs for the player character
// (docs/modding/animation.md): the frame a pack's clips are written in,
// the rig a modder animates against (`openminerz_mod_sdk export-rig`) and the
// checks behind `openminerz_mod_sdk check-anim`. Pure CPU; no device.
//
// ---------------------------------------------------------------------------
// THE AUTHORING FRAME
//
// A retail clip holds local transforms of the 71 Xbox avatar bones
// (openminerz/retail_rig.hpp). Below the root every joint's rest frame is
// parallel to its parent's, so a bone's local rotation in a file IS the
// rotation the clip holds, and a pack reads it unchanged. The root is the
// exception: the engine places it as `clip root * Avatar.BindPose[0]`, a half
// turn about Y lifted to the hips. A file holds the root where it is drawn
// instead, in glTF's own convention:
//
//     authoring space   = avatar space turned a half turn about +Y
//     Root (in a file)  = authoring_frame(rig) * Root (in the clip)
//
// so +Y is up, the character faces +Z with its left hand on +X, one unit is one
// block (a metre in Blender), and at rest the Root sits at the hips, unrotated.
// The pack loader reads the Root's transform in that space whatever nodes the
// file puts above it (Blender adds an "Armature" node), so what a modder sees
// in Blender is what the game draws. Avatar space, authoring space, glTF and
// Blender (after its importer's Y-up to Z-up turn) are all right-handed:
// nothing is mirrored anywhere. (An Xbox avatar `.ocavatar` export is
// left-handed; xbox_avatar.cpp converts that once at load, and it never meets
// a pack: an avatar skin is posed from the same clips as the stock body.)
// ---------------------------------------------------------------------------

#include "openminerz/modding/graphics.hpp"
#include "openminerz/retail_rig.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::modding::graphics
{

// Animations whose name starts with this are the game's own clips, exported
// for timing reference. The pack loader never uses them; check-anim and the
// SDK validator refuse a mod that carries one.
inline constexpr std::string_view reference_clip_prefix = "REF_";
inline constexpr int clip_frame_rate = 30;

// The 71 bones in rig order, by XNA's AvatarBone names ("Root", "BackLower",
// ...). The scale and twist helpers that enum leaves unnamed go by their
// number ("4", "31"), as the game names them.
[[nodiscard]] std::span<const std::string_view> avatar_bone_names() noexcept;
// A rig bone by a name a pack may use for it: the bone's name or the body bone
// it drives. -1 when the skeleton has no such bone.
[[nodiscard]] int find_rig_bone(const rig::RetailRig &rig, std::string_view name) noexcept;

// Avatar space -> authoring space: a half turn about +Y.
[[nodiscard]] Matrix4 authoring_from_avatar() noexcept;
// The Root's rest transform in authoring space (see above). Identity for a rig
// without a usable bind pose (an empty test rig).
[[nodiscard]] Matrix4 authoring_frame(const rig::RetailRig &rig) noexcept;

// The skeleton a file is authored against: each bone's rest transform as a
// glTF node (relative to its parent; Root relative to the scene) and its
// matrix in authoring space.
struct AuthoringBone final
{
    std::string name;
    int parent{-1};
    Vec3 translation{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Matrix4 world{};
};
[[nodiscard]] std::vector<AuthoringBone> authoring_skeleton(const rig::RetailRig &rig);

// The node of the file each rig bone reads from (by find_rig_bone, the bone's
// own name winning over its body bone's), -1 where the file has none.
using BoneNodes = std::array<int, rig::attachment_bone_count>;
[[nodiscard]] BoneNodes match_bones(const Model &model, const rig::RetailRig &rig);
// Every matched bone's transform as a clip holds it, `seconds` into
// `animation`: a bone's local transform, the Root's through the authoring
// frame. Bones without a node are left as they are in `pose`.
void sample_clip_pose(const Model &model, int animation, float seconds,
                      const rig::RetailRig &rig, const BoneNodes &nodes, rig::BonePose &pose);

// --- export-rig -----------------------------------------------------------

struct RigExportOptions final
{
    // Registered clip names to include as animations ("Stand", "PickUse").
    // These are the GAME'S animations, read from the user's own installed
    // content: for local timing reference only, never for a mod.
    std::vector<std::string> reference_clips;
    // Prepended to each reference clip's name (tests pass "" to feed an
    // export straight back through the pack loader).
    std::string reference_prefix{reference_clip_prefix};
    // A generated stand-in body: a box along every limb, skinned rigidly to
    // its bone (blue left, red right). No game art.
    bool proxy_mesh{true};
};

// A binary glTF 2.0 (.glb) of the rig in authoring space: the 71 bones as a
// skin with their rest pose, the proxy body and the requested clips at 30 Hz.
// Empty with `error` set when a requested clip is not registered.
[[nodiscard]] std::vector<std::uint8_t> export_rig(const rig::RetailRig &rig,
                                                   const RigExportOptions &options,
                                                   std::string *error = nullptr);

// --- Which clips a pack file replaces --------------------------------------

struct PackTarget final
{
    int animation{-1};
    std::string registration;
};
// The rule apply_animation_pack follows (see graphics.hpp), for any file in
// animations/: `registered` says whether the character registers a name.
[[nodiscard]] std::vector<PackTarget> pack_targets(
    const Model &model, std::string_view file_stem,
    const std::function<bool(std::string_view)> &registered,
    std::vector<std::string> *warnings = nullptr);

// --- check-anim -------------------------------------------------------------

// One imported character to check against ("Classic 1.6.3.8"). Without any,
// the check uses the bone names above and the clip names both generations
// register, and skips what needs the skeleton (hierarchy, scale, timing,
// influence, matching the game's own clips).
struct AnimationCheckRig final
{
    std::string label;
    const rig::RetailRig *rig{};
};

struct AnimationCheck final
{
    enum class Level : std::uint8_t
    {
        error,
        warning,
        note
    };
    struct Message final
    {
        Level level{Level::note};
        std::string text;
    };
    struct Clip final
    {
        std::string animation;    // its name in the file
        std::string registration; // the clip it replaces; empty when none
        float seconds{};
        // Bones by animated channel, in rig order (scaled: keys other than 1).
        std::vector<std::string> rotated;
        std::vector<std::string> moved;
        std::vector<std::string> scaled;
        // Animated bones the replaced clip's influence never writes.
        std::vector<std::string> ignored;
        // Per checked character: how long the game plays it, and how.
        std::vector<std::string> timing;
    };
    std::vector<Message> messages;
    std::vector<Clip> clips;

    [[nodiscard]] std::size_t count(Level level) const noexcept;
};

[[nodiscard]] AnimationCheck check_animation(const Model &model, std::string_view file_stem,
                                             std::span<const AnimationCheckRig> rigs = {});

// Every clip name the game registers (both generations unless a rig is given),
// sorted; the names a pack file or animation may use.
[[nodiscard]] std::vector<std::string> registered_clip_names(
    std::span<const AnimationCheckRig> rigs = {});

// The limits a pack file is loaded with (no mesh needed).
[[nodiscard]] ModelLimits animation_file_limits() noexcept;

} // namespace openminerz::modding::graphics
