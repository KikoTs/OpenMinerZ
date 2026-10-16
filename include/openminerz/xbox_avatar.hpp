#pragma once

// Xbox avatars as skins on the retail character rig.
//
// The owner's add-on for the original game (KikoTs/castleforge-xbox-avatar and
// its two siblings) replaces a player's stock body, Character\SWATMale, with the
// player's own Xbox avatar exported as an `.ocavatar` file. Both bodies hang
// from the SAME 71 Xbox avatar bones: the add-on's model entity subclasses the
// stock one and is posed from the very skeleton the game animates
// (DNA.Avatars.Avatar.Skeleton). What differs is only where each body's joints
// sit, and the retail engine already has a rule for that. Avatar.OnUpdate gives
// every skeleton it owns the animated ROTATION AND SCALE of a bone and that
// skeleton's OWN bind translation:
//
//     pose[i]              = animated[i]
//     pose[i].Translation  = bindPose[i].Translation
//
// That is how one pose drives the body skeleton and the attachment skeleton
// (openminerz/retail_rig.hpp). An avatar is a third skeleton under the same rule,
// and this file is everything that is specific to it:
//
//   * the `.ocavatar` reader, strict enough for a file that arrived from another
//     player;
//   * the conversion of the export's conventions into the rig's, done ONCE at
//     load: the Windows avatar runtime exports a left-handed mesh, so positions,
//     normals, bind matrices and bone offsets are reflected in Z; the export has
//     no facing turn on its root, the rig's root constant supplies it exactly as
//     it does for the stock body; a body's build is carried as per-bone scale on
//     the export's bind pose and becomes per-bone scale here;
//   * the pose of that third skeleton from a rig::CharacterPose.
//
// Nothing is retargeted, mirrored at draw time, or tuned per item. The animation
// that reaches an avatar is bit-for-bit the pose the stock body receives.
//
// TWO PLACEMENTS OF THE SAME SKELETON
//   own proportions   every joint at the avatar's own (scaled) offsets. A tall
//                     avatar is tall. Used whenever the character is seen from
//                     outside; the held item then hangs from the avatar's own
//                     PropRight, the same bone the stock body's item hangs from.
//   stock joints      every joint where the stock body has it, the avatar's
//                     build applied about each joint. Used for the view from the
//                     character's own head (first person, the death camera):
//                     that camera and the held item are positioned by the stock
//                     attachment skeleton, and gameplay must not move them.
//
// This header is presentation only and has no dependency on SDL or Direct3D.

#include "openminerz/retail_rig.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openminerz::xbox_avatar
{

inline constexpr int bone_count = rig::attachment_bone_count;
inline constexpr std::uint32_t file_signature = 0x5641434FU; // "OCAV"
inline constexpr std::int32_t newest_file_version = 4;

// AvatarBone ordinals this file refers to by name.
inline constexpr int bone_wrist_left = 33;
inline constexpr int bone_wrist_right = 36;
inline constexpr int bone_prop_left = 41;
inline constexpr int bone_special_left = 42;
inline constexpr int bone_special_right = 49;
inline constexpr int bone_first_finger = 37;
inline constexpr int bone_last_finger = 70;

// Every number a file may claim is checked against these before anything is
// allocated or decoded. The add-on's transfer ceiling is 4 MiB; a local file
// gets a little more room, since it never has to travel.
struct Limits final
{
    std::size_t file_bytes{4U * 1024U * 1024U};
    std::size_t batches{128U};
    std::size_t name_bytes{512U};
    std::size_t batch_vertices{65'536U};
    std::size_t total_vertices{200'000U};
    // Triangles actually drawn per frame, after unseen layers are dropped.
    std::size_t drawn_triangles{96'000U};
    int texture_side{2048};
    // Decoded RGBA texels across every distinct texture of one avatar.
    std::size_t texture_pixels{8U * 1024U * 1024U};
    float position_extent{8.0F};
    float bone_offset_extent{4.0F};
    float minimum_scale{0.25F};
    float maximum_scale{4.0F};
};
inline constexpr std::size_t network_file_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t local_file_bytes = 8U * 1024U * 1024U;

struct Image final
{
    int width{};
    int height{};
    // Straight (non-premultiplied) RGBA, 8 bits per channel, top row first.
    std::vector<std::uint8_t> rgba;
};

// Decodes one PNG. The reader has already checked the signature and that the
// header's dimensions are within Limits, so a decoder never sees an image it
// would have to refuse for size. Returns false when the data does not decode.
using ImageDecoder = std::function<bool(std::span<const std::uint8_t> png, Image &image)>;

// One skinned surface: positions, normals and weights shared by every material
// layer drawn over it. In the rig's handedness (XNA, right-handed).
struct Geometry final
{
    std::vector<std::array<float, 3>> position;
    std::vector<std::array<float, 3>> normal;
    std::vector<std::array<std::uint8_t, 4>> bones;
    std::vector<std::array<float, 4>> weights; // sums to one
    // Every vertex hangs from the head alone (hair, hat, face): it collapses
    // with the head when the character is seen from its own eyes.
    bool head_only{};
};

struct Layer final
{
    std::string name;
    std::uint32_t geometry{};
    // Index into Skin::textures, or -1 for an untextured (white) surface.
    std::int32_t texture{-1};
    // The first layer of a surface writes depth; layers painted over it blend.
    bool blended{};
    // Face layers address their texture with clamping; everything else wraps.
    bool clamp{};
    std::vector<std::array<float, 2>> uv;    // per geometry vertex
    std::vector<std::array<float, 4>> color; // per geometry vertex, straight alpha
    std::vector<std::uint16_t> indices;      // triangle list into the geometry
};

struct Bone final
{
    // Mesh (rig handedness) to bone space.
    rig::Matrix inverse_bind{};
    // This avatar's joint offset from its parent, in the parent's frame.
    rig::Vec3 translation{};
    // The avatar's build on this bone, and the same accumulated from the root.
    rig::Vec3 scale{1.0F, 1.0F, 1.0F};
    rig::Vec3 cumulative_scale{1.0F, 1.0F, 1.0F};
};

struct Skin final
{
    std::int32_t file_version{};
    std::array<Bone, bone_count> bones{};
    std::vector<Geometry> geometry;
    std::vector<Layer> layers;
    std::vector<Image> textures;
    std::size_t source_batches{};
    std::size_t triangles{};
    std::size_t vertices{};
    // max(cumulative scale) over the body: 1 for an avatar of stock build.
    float build{1.0F};
};

// Parses, validates and converts one `.ocavatar`. Safe to call on any thread
// and on any bytes: every failure returns nullptr with `error` saying why, and
// nothing is thrown. `cancel`, when given, is polled between batches and
// textures so a decode that is no longer wanted stops early.
[[nodiscard]] std::shared_ptr<const Skin> load(std::span<const std::uint8_t> file,
                                               const ImageDecoder &decoder, std::string &error,
                                               const Limits &limits = {},
                                               const std::atomic<bool> *cancel = nullptr);

// The dimensions a PNG declares, without decoding it.
[[nodiscard]] bool png_dimensions(std::span<const std::uint8_t> png, int &width,
                                  int &height) noexcept;

enum class Placement : std::uint8_t
{
    own_proportions,
    stock_joints
};

struct Pose final
{
    // Every avatar bone in avatar space, and the matching skinning palette.
    std::array<rig::Matrix, bone_count> world{};
    std::array<rig::Matrix, bone_count> palette{};
};

// The avatar's skeleton for this frame of `character`.
void pose(const Skin &skin, const rig::CharacterPose &character, Placement placement,
          Pose &result) noexcept;

// What the held item is parented to: the avatar's own PropRight when it stands
// on its own proportions, the stock attachment bone otherwise.
[[nodiscard]] rig::Matrix held_item_parent(const rig::CharacterPose &character, const Pose &pose,
                                           Placement placement) noexcept;

struct SkinnedGeometry final
{
    std::vector<std::array<float, 3>> position;
    std::vector<std::array<float, 3>> normal;
};

void skin_geometry(const Geometry &geometry, const std::array<rig::Matrix, bone_count> &palette,
                   SkinnedGeometry &result);

// FIPS 180-4 SHA-256: an avatar's identity on the wire and in every cache.
using Digest = std::array<std::uint8_t, 32>;
[[nodiscard]] Digest sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::string digest_text(const Digest &digest);

} // namespace openminerz::xbox_avatar
