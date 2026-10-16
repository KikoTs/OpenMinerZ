#pragma once

// The retail character rig, in retail's own bone spaces.
//
// CastleMiner Z draws every player (both generations, both views) with one
// skinned body, Character\SWATMale, driven by DNA.Avatars.Avatar. An Avatar owns
// TWO skeletons that share one animated pose:
//
//   * the 71-bone ATTACHMENT skeleton (Xbox avatar bone order, DefaultBindPose
//     offsets). It carries the eye camera (Head) and the held item (PropRight);
//   * the BODY skeleton that skins SWATMale. Every frame each body bone copies
//     the rotation and scale of the attachment bone with the same name and
//     keeps its own bind translation; the root is the animated root times a
//     constant (a half turn about Y lifted to the hips).
//
// Clips are AnimationClip assets: per bone, independently reduced position,
// rotation and scale key arrays at 30 Hz. Nothing here retargets, mirrors or
// resamples them: the content pack's character.omrc (written by
// tools/ContentBridge from the user's own game) holds the keys, both skeletons,
// the mesh and the registrations exactly as the game holds them, and this file
// is a port of the code that evaluates them:
//
//   Clip::copy_transforms        AnimationClip.CopyTransforms
//   ClipPlayer                   AnimationPlayer
//   Animator                     AvatarAnimationCollection / LayeredAnimationPlayer
//                                and its sixteen AnimBlender channels
//   CharacterPose                Avatar.OnUpdate, Avatar.UpdateParts and the
//                                body's SkinnedModelEntity update
//
// tools/OriginalClientInterop/RigParityProbe runs the original classes and
// tests/test_rig_parity.cpp replays the same calls here, bone for bone.
//
// ---------------------------------------------------------------------------
// THE SEAM WITH THE ANIMATION STATE MACHINE (which clip plays when)
//
// Everything below "play clip X on channel C" belongs to this file. Whoever
// decides WHAT plays drives it in one of two equivalent ways:
//
//   1. Retail-shaped: call Animator::play / clear / replay and tweak the
//      returned ClipPlayer (speed, looping, reversed, ping_pong, progress,
//      pause), exactly as Player.UpdateAnimation does with
//      Avatar.Animations. Clip names are the game's registered names
//      ("Stand", "GunIdle", "PickUse", ...); channels are the game's
//      (0 lower body, 1 torso tilt, 2 upper body/item, 3 action, 4 head, 5 death).
//      CharacterPose::update(elapsed) then advances and evaluates.
//
//   2. Plain data: fill a ChannelStates (per channel: current clip + position,
//      previous clip + position, blend factor) from your own clocks and call
//      CharacterPose::apply. This is what (1) produces internally.
// ---------------------------------------------------------------------------

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::rig
{

inline constexpr int attachment_bone_count = 71;
inline constexpr int channel_count = 16;
inline constexpr std::int64_t ticks_per_second = 10'000'000;

// AvatarBone ordinals the engine and the game refer to by name.
inline constexpr int bone_root = 0;
inline constexpr int bone_back_lower = 1;
inline constexpr int bone_back_upper = 5;
inline constexpr int bone_neck = 14;
inline constexpr int bone_collar_right = 16;
inline constexpr int bone_head = 19;
inline constexpr int bone_prop_right = 48;

// XNA layout: row-major, row vectors (v' = v * M), translation in [12..14].
using Matrix = std::array<float, 16>;

struct Vec3 final
{
    float x{};
    float y{};
    float z{};
};

struct Quat final
{
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct BoneTransform final
{
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

using BonePose = std::array<BoneTransform, attachment_bone_count>;
using BoneMask = std::array<bool, attachment_bone_count>;

[[nodiscard]] Matrix identity() noexcept;
[[nodiscard]] Matrix multiply(const Matrix &left, const Matrix &right) noexcept;
[[nodiscard]] Matrix invert(const Matrix &matrix) noexcept;
// Bone.ComposeTransform: scale, then rotation, translation written last.
[[nodiscard]] Matrix compose(const BoneTransform &transform) noexcept;
[[nodiscard]] Quat slerp(const Quat &first, const Quat &second, float amount) noexcept;
[[nodiscard]] Vec3 lerp(const Vec3 &first, const Vec3 &second, float amount) noexcept;
[[nodiscard]] std::array<float, 3> transform_point(const float *point, const Matrix &matrix) noexcept;
[[nodiscard]] std::array<float, 3> transform_vector(const float *vector, const Matrix &matrix) noexcept;
// TimeSpan.FromSeconds: rounds to the nearest whole millisecond.
[[nodiscard]] std::int64_t ticks_from_seconds(double seconds) noexcept;
[[nodiscard]] double seconds_from_ticks(std::int64_t ticks) noexcept;

struct Clip final
{
    std::string asset;
    int frame_rate{30};
    std::int64_t duration_ticks{};
    std::array<std::vector<Vec3>, attachment_bone_count> positions;
    std::array<std::vector<Quat>, attachment_bone_count> rotations;
    std::array<std::vector<Vec3>, attachment_bone_count> scales;

    // AnimationClip.CopyTransforms. Bones outside `influence` are not written.
    void copy_transforms(BonePose &pose, std::int64_t position_ticks,
                         const BoneMask *influence) const noexcept;
};

// One AvatarAnimationManager.RegisterAnimation entry.
struct Registration final
{
    std::string name;
    int clip{};
    bool looping{true};
    BoneMask influence{};
};

struct MeshVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> bones{};
    std::array<float, 4> weights{};
};

struct MeshPart final
{
    std::string name;
    int texture_width{};
    int texture_height{};
    std::vector<std::uint8_t> rgba;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct AttachmentBone final
{
    std::string name;        // AvatarBone name, e.g. "PropRight"
    std::string native_name; // the body bone it drives, e.g. "RT_PROP__Skeleton"
    int parent{-1};
    Matrix bind{};           // Avatar.BindPose
    BoneTransform initial{}; // the skeleton before any clip has written to it
};

struct BodyBone final
{
    std::string name;
    int parent{-1};
    int source{}; // attachment bone whose rotation and scale it copies
    Matrix bind_local{};
    Matrix inverse_bind{};
};

// InventoryItemClass.CreateEntity(ItemUse.Hand).LocalToParent, parented to PropRight.
struct HeldItem final
{
    int id{};
    std::string name;
    std::string player_mode;
    std::string entity;
    Matrix hand{};
    Matrix hand_remote{};
};

class RetailRig final
{
  public:
    // Throws std::runtime_error with a description when the file is unusable.
    explicit RetailRig(const std::filesystem::path &path);
    // An empty rig, filled in by hand (tests).
    RetailRig() = default;

    std::string generation; // "classic" or "modern"
    std::string game_version;
    std::string graphics_profile;
    std::string body_asset;
    std::array<AttachmentBone, attachment_bone_count> attachment;
    std::vector<BodyBone> body;
    std::vector<int> body_order; // parents before children
    Matrix root_adjust{};        // Avatar.OnUpdate's constant on the body root
    Matrix eye_to_head{};        // Avatar.eyeToHead after SetEyePoint
    std::vector<MeshPart> parts;
    std::vector<Clip> clips;
    std::vector<Registration> registrations;
    std::vector<HeldItem> items;

    [[nodiscard]] const Registration *find(std::string_view name) const noexcept;
    [[nodiscard]] const HeldItem *item(int id) const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
};

class Animator;

// AnimationPlayer.
class ClipPlayer final
{
  public:
    float speed{1.0F};
    bool looping{true};
    bool reversed{};
    bool ping_pong{};

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool has_clip() const noexcept { return clip_ != nullptr; }
    void pause() noexcept { playing_ = false; }
    void play() noexcept { playing_ = true; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] float progress() const noexcept;
    void set_progress(float value) noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] std::int64_t current_ticks() const noexcept { return current_ticks_; }
    void set_current_ticks(std::int64_t ticks) noexcept { current_ticks_ = ticks; }
    [[nodiscard]] std::int64_t duration_ticks() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;
    // The clip position CopyTransforms is evaluated at (reversal applied).
    [[nodiscard]] std::int64_t position_ticks() const noexcept;
    [[nodiscard]] bool influences(int bone) const noexcept { return influence_[static_cast<std::size_t>(bone)]; }
    [[nodiscard]] const Registration *registration() const noexcept { return registration_; }
    [[nodiscard]] const Clip *clip() const noexcept { return clip_; }

    // AnimationPlayer.Update(TimeSpan): steps the clock; the step is rounded to
    // whole milliseconds, as TimeSpan.FromSeconds does.
    void advance(std::int64_t elapsed_ticks) noexcept;

  private:
    friend class Animator;
    void set_clip(const Registration &registration, const Clip &clip) noexcept;

    const Registration *registration_{};
    const Clip *clip_{};
    BoneMask influence_{};
    std::int64_t current_ticks_{};
    bool on_pong_{};
    bool playing_{true};
};

// The plain-data form of one AnimBlender after its clocks have advanced.
struct ChannelState final
{
    const Registration *current{};
    std::int64_t current_position{};
    const Registration *previous{};
    std::int64_t previous_position{};
    // blend clock / blend time; >= 1 means the channel is entirely `current`.
    float blend{1.0F};
};
using ChannelStates = std::array<ChannelState, channel_count>;

// AvatarAnimationCollection: sixteen AnimBlender channels, three recycled
// players per channel.
class Animator final
{
  public:
    explicit Animator(const RetailRig &rig);
    // The channels point at this object's own players.
    Animator(const Animator &) = delete;
    Animator &operator=(const Animator &) = delete;

    // Play(id, channel, blendTime). Returns nullptr when the name is not
    // registered in this content pack (the original would throw).
    ClipPlayer *play(std::string_view name, int channel, double blend_seconds);
    // ClearAnimation(channel, blendTime).
    void clear(int channel, double blend_seconds);
    // PlayAnimation(channel, player, blendTime) with an existing player.
    void replay(int channel, ClipPlayer *player, double blend_seconds);
    // The same three with the blend time already in TimeSpan ticks.
    ClipPlayer *play_ticks(std::string_view name, int channel, std::int64_t blend_ticks);
    void replay_ticks(int channel, ClipPlayer *player, std::int64_t blend_ticks) noexcept;
    // Animations[channel]: the channel's current player, or nullptr.
    [[nodiscard]] ClipPlayer *active(int channel) noexcept;
    [[nodiscard]] const ClipPlayer *active(int channel) const noexcept;
    [[nodiscard]] bool is(int channel, std::string_view name) const noexcept;
    // Places a channel's fade clock, for a caller that owns an exact timeline
    // (a capture of a known moment) rather than accumulating frame times.
    void set_blend_elapsed_ticks(int channel, std::int64_t elapsed_ticks) noexcept;

    // LayeredAnimationPlayer.Update: advances every channel's clocks and returns
    // what each channel now contributes.
    [[nodiscard]] ChannelStates update(std::int64_t elapsed_ticks) noexcept;

    [[nodiscard]] const RetailRig &rig() const noexcept { return *rig_; }

  private:
    struct Blender final
    {
        ClipPlayer *current{};
        ClipPlayer *previous{};
        std::int64_t blend_total_ticks{2'500'000};
        std::int64_t blend_current_ticks{};
    };

    const RetailRig *rig_{};
    std::array<Blender, channel_count> blenders_{};
    std::array<std::array<ClipPlayer, 3>, channel_count> players_{};
    std::array<std::array<bool, 3>, channel_count> created_{};
    std::array<int, channel_count> next_player_{};
};

// One DNA.Avatars.Avatar together with its body.
class CharacterPose final
{
  public:
    explicit CharacterPose(const RetailRig &rig);

    Animator animations;
    // Avatar.HideHead: the head bone is scaled to a point (first person).
    bool hide_head{};

    // Entity.Update for the avatar and its body: advance the animator, then pose.
    void update(std::int64_t elapsed_ticks);
    void update_seconds(double elapsed_seconds);
    // The same pose step from caller-owned clocks (see the seam note above).
    void apply(const ChannelStates &channels);

    // Avatar._bonesToAvatar: every attachment bone in avatar space.
    [[nodiscard]] const std::array<Matrix, attachment_bone_count> &attachment() const noexcept { return attachment_; }
    // Avatar.EyePointCamera.LocalToParent. The original builds it from the head
    // of the PREVIOUS update; so does this.
    [[nodiscard]] const Matrix &eye() const noexcept { return eye_; }
    // The same camera from this update's head, for callers that must not lag.
    [[nodiscard]] Matrix eye_now() const noexcept;
    // The held item's parent: PropRight in avatar space.
    [[nodiscard]] const Matrix &right_hand() const noexcept { return attachment_[bone_prop_right]; }
    // The body's WorldBoneTransforms (avatar space) and skinning palette.
    [[nodiscard]] const std::vector<Matrix> &body_world() const noexcept { return body_world_; }
    [[nodiscard]] const std::vector<Matrix> &skin() const noexcept { return skin_; }
    [[nodiscard]] const BonePose &skeleton() const noexcept { return skeleton_; }
    [[nodiscard]] const RetailRig &rig() const noexcept { return *rig_; }

  private:
    void pose_from_skeleton();
    [[nodiscard]] Matrix eye_from(const Matrix &head) const noexcept;

    const RetailRig *rig_{};
    BonePose skeleton_{};
    std::array<Matrix, attachment_bone_count> attachment_{};
    Matrix eye_{};
    std::vector<Matrix> body_world_;
    std::vector<Matrix> skin_;
};

} // namespace openminerz::rig
