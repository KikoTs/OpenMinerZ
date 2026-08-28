#include "avatar_viewmodel.hpp"
#include "static_model_asset.hpp"

#include "openminerz/inventory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

struct SourceVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> bindings{};
    std::array<std::uint8_t, 4> weights{};
    std::array<std::uint8_t, 4> color{};
};

using Matrix4 = std::array<float, 16>;

struct BoneTransform final
{
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
};

struct AnimationFrameSample final
{
    const BoneTransform *first{};
    const BoneTransform *second{};
    float amount{};
};

struct NativeAnimation final
{
    int bone_count{};
    int frame_count{};
    int sample_rate{};
    float duration{};
    std::vector<int> parents;
    std::vector<BoneTransform> frames;

    [[nodiscard]] AnimationFrameSample sample(float seconds, bool loop) const noexcept
    {
        if (frames.empty() || duration <= 0.0F)
            return {};
        const float position = loop ? std::fmod(std::max(0.0F, seconds), duration)
                                    : std::clamp(seconds, 0.0F, duration);
        const float fractional_frame = position * static_cast<float>(sample_rate);
        const int first_index =
            std::clamp(static_cast<int>(std::floor(fractional_frame)), 0, frame_count - 1);
        const int second_index = std::min(first_index + 1, frame_count - 1);
        const float amount = first_index == second_index
                                 ? 0.0F
                                 : std::clamp(fractional_frame - static_cast<float>(first_index),
                                              0.0F, 1.0F);
        return {frames.data() + static_cast<std::size_t>(first_index * bone_count),
                frames.data() + static_cast<std::size_t>(second_index * bone_count), amount};
    }
};

class BinaryReader final
{
  public:
    explicit BinaryReader(const std::filesystem::path &path)
        : stream_(path, std::ios::binary), size_(std::filesystem::file_size(path))
    {
        if (!stream_)
        {
            throw std::runtime_error{"could not open .omavatar"};
        }
    }

    template <typename Value> Value read()
    {
        Value result{};
        read_bytes(&result, sizeof(result));
        return result;
    }

    void read_bytes(void *destination, std::size_t count)
    {
        if (count > size_ - position_)
        {
            throw std::runtime_error{"truncated .omavatar"};
        }
        stream_.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
        if (!stream_)
        {
            throw std::runtime_error{"failed reading .omavatar"};
        }
        position_ += count;
    }

    [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t count)
    {
        std::vector<std::uint8_t> result(count);
        read_bytes(result.data(), result.size());
        return result;
    }

    [[nodiscard]] std::string dotnet_string()
    {
        std::uint32_t length{};
        int shift{};
        for (int byte = 0; byte < 5; ++byte)
        {
            const auto value = read<std::uint8_t>();
            length |= static_cast<std::uint32_t>(value & 0x7FU) << shift;
            if ((value & 0x80U) == 0U)
            {
                if (length > 1024U * 1024U)
                {
                    throw std::runtime_error{"invalid .omavatar string"};
                }
                std::string result(length, '\0');
                read_bytes(result.data(), result.size());
                return result;
            }
            shift += 7;
        }
        throw std::runtime_error{"invalid .omavatar string prefix"};
    }

  private:
    std::ifstream stream_;
    std::size_t size_{};
    std::size_t position_{};
};

Matrix4 identity_matrix() noexcept
{
    Matrix4 result{};
    result[0] = result[5] = result[10] = result[15] = 1.0F;
    return result;
}

Matrix4 multiply_matrix(const Matrix4 &left, const Matrix4 &right) noexcept
{
    Matrix4 result{};
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
            for (std::size_t inner = 0; inner < 4U; ++inner)
                result[row * 4U + column] +=
                    left[row * 4U + inner] * right[inner * 4U + column];
    return result;
}

Matrix4 quaternion_matrix(const std::array<float, 4> &q) noexcept
{
    const float xx = q[0] * q[0] * 2.0F;
    const float yy = q[1] * q[1] * 2.0F;
    const float zz = q[2] * q[2] * 2.0F;
    const float xy = q[0] * q[1] * 2.0F;
    const float xz = q[0] * q[2] * 2.0F;
    const float yz = q[1] * q[2] * 2.0F;
    const float wx = q[3] * q[0] * 2.0F;
    const float wy = q[3] * q[1] * 2.0F;
    const float wz = q[3] * q[2] * 2.0F;
    Matrix4 result = identity_matrix();
    result[0] = 1.0F - yy - zz;
    result[1] = xy + wz;
    result[2] = xz - wy;
    result[4] = xy - wz;
    result[5] = 1.0F - xx - zz;
    result[6] = yz + wx;
    result[8] = xz + wy;
    result[9] = yz - wx;
    result[10] = 1.0F - xx - yy;
    return result;
}

std::array<float, 4> slerp_quaternion(const std::array<float, 4> &left,
                                      const std::array<float, 4> &right,
                                      float amount) noexcept
{
    std::array<float, 4> adjusted_right = right;
    float cosine = left[0] * right[0] + left[1] * right[1] + left[2] * right[2] +
                   left[3] * right[3];
    // q and -q describe the same rotation. Use the shortest arc exactly as XNA's
    // Quaternion.Slerp does for the original AnimationClip.CopyTransforms path.
    if (cosine < 0.0F)
    {
        cosine = -cosine;
        for (float &component : adjusted_right)
            component = -component;
    }
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    std::array<float, 4> result{};
    if (cosine > 0.9995F)
    {
        for (std::size_t component = 0U; component < result.size(); ++component)
            result[component] = left[component] +
                                (adjusted_right[component] - left[component]) * amount;
    }
    else
    {
        const float angle = std::acos(cosine);
        const float inverse_sine = 1.0F / std::sin(angle);
        const float left_weight = std::sin((1.0F - amount) * angle) * inverse_sine;
        const float right_weight = std::sin(amount * angle) * inverse_sine;
        for (std::size_t component = 0U; component < result.size(); ++component)
            result[component] =
                left[component] * left_weight + adjusted_right[component] * right_weight;
    }
    const float inverse_length =
        1.0F / std::max(0.000001F,
                        std::sqrt(result[0] * result[0] + result[1] * result[1] +
                                  result[2] * result[2] + result[3] * result[3]));
    for (float &component : result)
        component *= inverse_length;
    return result;
}

BoneTransform interpolate_transform(const BoneTransform &first, const BoneTransform &second,
                                    float amount) noexcept
{
    BoneTransform result;
    for (std::size_t component = 0U; component < result.translation.size(); ++component)
    {
        result.translation[component] = first.translation[component] +
                                        (second.translation[component] -
                                         first.translation[component]) *
                                            amount;
        result.scale[component] =
            first.scale[component] + (second.scale[component] - first.scale[component]) * amount;
    }
    result.rotation = slerp_quaternion(first.rotation, second.rotation, amount);
    return result;
}

Matrix4 compose_matrix(const BoneTransform &transform) noexcept
{
    Matrix4 result = quaternion_matrix(transform.rotation);
    for (std::size_t column = 0; column < 3U; ++column)
    {
        result[column] *= transform.scale[0];
        result[4U + column] *= transform.scale[1];
        result[8U + column] *= transform.scale[2];
    }
    result[12] = transform.translation[0];
    result[13] = transform.translation[1];
    result[14] = transform.translation[2];
    return result;
}

Matrix4 rotation_z(float radians) noexcept
{
    Matrix4 result = identity_matrix();
    result[0] = std::cos(radians);
    result[1] = std::sin(radians);
    result[4] = -std::sin(radians);
    result[5] = std::cos(radians);
    return result;
}

Matrix4 rotation_x(float radians) noexcept
{
    Matrix4 result = identity_matrix();
    result[5] = std::cos(radians);
    result[6] = std::sin(radians);
    result[9] = -std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix4 rotation_y(float radians) noexcept
{
    Matrix4 result = identity_matrix();
    result[0] = std::cos(radians);
    result[2] = -std::sin(radians);
    result[8] = std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix4 scale_matrix(const std::array<float, 3> &scale) noexcept
{
    Matrix4 result = identity_matrix();
    result[0] = scale[0];
    result[5] = scale[1];
    result[10] = scale[2];
    return result;
}

std::array<float, 3> transform_point(const std::array<float, 3> &point,
                                     const Matrix4 &matrix) noexcept
{
    return {point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8] + matrix[12],
            point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9] + matrix[13],
            point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]};
}

std::array<float, 3> transform_direction(const std::array<float, 3> &direction,
                                         const Matrix4 &matrix) noexcept
{
    std::array<float, 3> result{
        direction[0] * matrix[0] + direction[1] * matrix[4] + direction[2] * matrix[8],
        direction[0] * matrix[1] + direction[1] * matrix[5] + direction[2] * matrix[9],
        direction[0] * matrix[2] + direction[1] * matrix[6] + direction[2] * matrix[10]};
    const float length = std::sqrt(result[0] * result[0] + result[1] * result[1] +
                                   result[2] * result[2]);
    if (length > 0.000001F)
        for (float &component : result)
            component /= length;
    return result;
}

NativeAnimation load_animation(const std::filesystem::path &path)
{
    BinaryReader reader{path};
    if (reader.read<std::uint32_t>() != 0x415A4D4FU || reader.read<std::int32_t>() != 1)
        throw std::runtime_error{"invalid native avatar animation"};
    NativeAnimation result;
    result.bone_count = reader.read<std::int32_t>();
    result.frame_count = reader.read<std::int32_t>();
    result.duration = reader.read<float>();
    result.sample_rate = reader.read<std::int32_t>();
    if (result.bone_count != 71 || result.frame_count <= 0 || result.frame_count > 3600 ||
        result.sample_rate <= 0)
        throw std::runtime_error{"unsupported native avatar animation"};
    result.parents.resize(static_cast<std::size_t>(result.bone_count));
    for (int &parent : result.parents)
        parent = reader.read<std::int32_t>();
    result.frames.resize(static_cast<std::size_t>(result.bone_count * result.frame_count));
    for (auto &transform : result.frames)
    {
        for (float &value : transform.translation)
            value = reader.read<float>();
        for (float &value : transform.rotation)
            value = reader.read<float>();
        for (float &value : transform.scale)
            value = reader.read<float>();
    }
    return result;
}

std::filesystem::path derived_data_path()
{
    return original_derived_data_path();
}

std::array<float, 16> inverse_matrix(const std::array<float, 16> &matrix)
{
    std::array<std::array<double, 8>, 4> rows{};
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            rows[row][column] = matrix[row * 4U + column];
            rows[row][column + 4U] = row == column ? 1.0 : 0.0;
        }
    }
    for (std::size_t column = 0; column < 4U; ++column)
    {
        std::size_t pivot = column;
        for (std::size_t row = column + 1U; row < 4U; ++row)
        {
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
            {
                pivot = row;
            }
        }
        if (std::abs(rows[pivot][column]) < 1.0e-10)
        {
            throw std::runtime_error{"singular .omavatar bind matrix"};
        }
        std::swap(rows[pivot], rows[column]);
        const double divisor = rows[column][column];
        for (double &value : rows[column])
        {
            value /= divisor;
        }
        for (std::size_t row = 0; row < 4U; ++row)
        {
            if (row == column)
            {
                continue;
            }
            const double multiplier = rows[row][column];
            for (std::size_t element = 0; element < 8U; ++element)
            {
                rows[row][element] -= multiplier * rows[column][element];
            }
        }
    }
    std::array<float, 16> result{};
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            result[row * 4U + column] = static_cast<float>(rows[row][column + 4U]);
        }
    }
    return result;
}

struct ViewmodelTriangle final
{
    float depth{};
    int render_layer{1};
    SDL_Texture *texture{};
    std::array<SDL_Vertex, 3> vertices{};
    std::array<std::array<float, 3>, 3> view{};
    // View-space normal (with forward-positive Z) plus material reflectivity.
    // Zero reflectivity keeps avatar skin, cloth and held blocks on the stock
    // diffuse path; only authored metal/tool surfaces receive the extra lobe.
    std::array<std::array<float, 4>, 3> normal_material{};
};

struct AvatarBatch final
{
    std::string name;
    std::uint32_t category_mask{};
    int face_texture_usage{-1};
    int face_frame{-1};
    bool material_overlay{};
    std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
    std::vector<SourceVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint16_t> first_person_indices;
    std::vector<std::uint16_t> left_person_indices;
    std::vector<std::uint8_t> texture_png;
    SDL_Texture *texture{};
};

struct CarrierVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> color{};
    std::array<std::uint8_t, 4> bindings{};
    std::array<float, 4> weights{};
};

struct CarrierPart final
{
    std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
    bool use_texture{};
    bool use_vertex_color{};
    std::vector<CarrierVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint16_t> right_arm_indices;
    std::vector<std::uint16_t> right_finger_indices;
    std::vector<std::uint16_t> left_arm_indices;
    std::vector<std::uint16_t> left_finger_indices;
    SDL_Texture *texture{};
};

constexpr std::array<int, 71> avatar_parents{
    -1, 0, 0, 0, 0, 1, 2, 2, 3, 3,
    1, 6, 5, 6, 5, 8, 5, 8, 5, 14,
    12, 11, 16, 15, 14, 20, 20, 20, 22, 22,
    22, 25, 25, 25, 28, 28, 28, 33, 33, 33,
    33, 33, 33, 33, 36, 36, 36, 36, 36, 36,
    36, 37, 38, 39, 40, 43, 44, 45, 46, 47,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    60};

bool avatar_descends_from(int bone, int ancestor) noexcept
{
    while (bone >= 0 && bone < static_cast<int>(avatar_parents.size()))
    {
        if (bone == ancestor)
            return true;
        bone = avatar_parents[static_cast<std::size_t>(bone)];
    }
    return false;
}

// Native equivalent of the original DNA AnimBlender/LayeredAnimationPlayer pair.
// Each channel owns a current and a previous clip player; Play() snapshots the
// running player and cross-fades to the next one over the requested blend time.
// While a blend is active both players keep advancing, and bones only one side
// influences fade against whatever the lower channels already wrote, exactly as
// AnimBlender.Update does for the original avatar rig.
struct ChannelPlayer final
{
    const NativeAnimation *clip{};
    int influence_root{-1}; // -1 influences every bone, as Stand does on channel 0.
    std::array<int, 2> excluded_roots{-1, -1};
    float time{};
    float speed{1.0F};
    bool looping{true};
    bool time_driven{true}; // false: the cue supplies normalized progress every frame.

    [[nodiscard]] bool influences(int bone) const noexcept
    {
        if (influence_root >= 0 && !avatar_descends_from(bone, influence_root))
            return false;
        return std::ranges::none_of(excluded_roots, [bone](int root) {
            return root >= 0 && avatar_descends_from(bone, root);
        });
    }

    void advance(float delta_seconds) noexcept
    {
        if (clip == nullptr || clip->duration <= 0.0F)
            return;
        time += delta_seconds * speed;
        if (looping)
        {
            time = std::fmod(time, clip->duration);
            if (time < 0.0F)
                time += clip->duration;
        }
        else
            time = std::clamp(time, 0.0F, clip->duration);
    }

    [[nodiscard]] bool finished() const noexcept
    {
        if (clip == nullptr || looping)
            return false;
        return speed < 0.0F ? time <= 0.0001F : time >= clip->duration - 0.0001F;
    }

    void sample_pose(std::array<BoneTransform, 71> &pose) const noexcept
    {
        const AnimationFrameSample frame = clip->sample(time, looping);
        if (frame.first == nullptr || frame.second == nullptr)
            return;
        for (std::size_t bone = 0U; bone < pose.size(); ++bone)
            pose[bone] =
                interpolate_transform(frame.first[bone], frame.second[bone], frame.amount);
    }
};

enum class ShoulderAnimationPhase : std::uint8_t
{
    unshouldered,
    shouldering,
    shouldered,
    unshouldering,
};

struct AnimationChannel final
{
    ChannelPlayer current{};
    ChannelPlayer previous{};
    bool has_current{};
    bool has_previous{};
    float blend_total{};
    float blend_elapsed{};

    void play(const ChannelPlayer &next, float blend_seconds) noexcept
    {
        previous = current;
        has_previous = has_current;
        current = next;
        has_current = true;
        blend_total = blend_seconds;
        blend_elapsed = 0.0F;
    }

    void clear(float blend_seconds) noexcept
    {
        previous = current;
        has_previous = has_current;
        has_current = false;
        blend_total = blend_seconds;
        blend_elapsed = 0.0F;
    }

    void advance(float delta_seconds) noexcept
    {
        blend_elapsed += delta_seconds;
        if (has_previous && (blend_total <= 0.0F || blend_elapsed >= blend_total))
            has_previous = false;
        if (has_current && current.time_driven)
            current.advance(delta_seconds);
        // The previous player has lost its external driver, so it free-runs
        // through the fade the way AnimBlender keeps updating both players.
        if (has_previous)
            previous.advance(delta_seconds);
    }

    void apply(std::array<BoneTransform, 71> &bones) const noexcept
    {
        if (!has_current && !has_previous)
            return;
        const float factor =
            blend_total > 0.0F ? std::min(1.0F, blend_elapsed / blend_total) : 1.0F;
        std::array<BoneTransform, 71> current_pose{};
        std::array<BoneTransform, 71> previous_pose{};
        if (has_current)
            current.sample_pose(current_pose);
        if (factor >= 1.0F)
        {
            if (!has_current)
                return;
            for (std::size_t bone = 0U; bone < bones.size(); ++bone)
                if (current.influences(static_cast<int>(bone)))
                    bones[bone] = current_pose[bone];
            return;
        }
        if (has_previous)
            previous.sample_pose(previous_pose);
        for (std::size_t bone = 0U; bone < bones.size(); ++bone)
        {
            const bool from_previous =
                has_previous && previous.influences(static_cast<int>(bone));
            const bool from_current = has_current && current.influences(static_cast<int>(bone));
            if (from_previous && from_current)
                bones[bone] = interpolate_transform(previous_pose[bone], current_pose[bone],
                                                    factor);
            else if (from_previous)
                bones[bone] = interpolate_transform(previous_pose[bone], bones[bone], factor);
            else if (from_current)
                bones[bone] = interpolate_transform(bones[bone], current_pose[bone], factor);
        }
    }

    void finish_blends() noexcept
    {
        has_previous = false;
        blend_total = 0.0F;
        blend_elapsed = 0.0F;
    }
};

struct WorldPlayerAnimationState final
{
    AnimationChannel locomotion;
    AnimationChannel tilt;
    AnimationChannel item;
    AnimationChannel action;
    AnimationChannel head;
    AnimationChannel death;
    ShoulderAnimationPhase shoulder_phase{ShoulderAnimationPhase::unshouldered};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    InventoryItemId carried_item{InventoryItemId::bare_hands};
    float last_animation_seconds{-1.0F};
    float head_seconds{};
    float last_seen_seconds{};
    bool initialized{};
    bool action_active{};
    bool reloading{};
    bool dead{};
};

float branch_weight(const SourceVertex &vertex, int ancestor) noexcept
{
    float result{};
    for (std::size_t influence = 0; influence < vertex.bindings.size(); ++influence)
    {
        if (avatar_descends_from(vertex.bindings[influence], ancestor))
            result += static_cast<float>(vertex.weights[influence]) / 255.0F;
    }
    return result;
}

bool avatar_is_finger(int bone, bool right) noexcept
{
    constexpr std::array<int, 5> left_fingers{37, 38, 39, 40, 43};
    constexpr std::array<int, 5> right_fingers{44, 45, 46, 47, 50};
    const auto &fingers = right ? right_fingers : left_fingers;
    return std::any_of(fingers.begin(), fingers.end(),
                       [bone](int root) { return avatar_descends_from(bone, root); });
}

bool item_uses_support_hand(std::uint16_t item) noexcept
{
    const bool conventional_firearm =
        (item >= 50U && item <= 54U) || (item >= 56U && item <= 60U) ||
        (item >= 62U && item <= 66U) || (item >= 68U && item <= 72U);
    const bool space_firearm = item >= 82U && item <= 101U;
    return conventional_firearm || space_firearm || item == 103U || item == 104U ||
           (item >= 119U && item <= 122U);
}

enum class WeaponFamily : std::uint8_t
{
    none,
    assault,
    pistol,
    shotgun,
    rifle,
    smg,
    lmg,
    space_assault,
    space_pistol,
    space_shotgun,
    space_rifle,
    space_smg,
    rpg,
};

WeaponFamily weapon_family(PlayerAnimationMode mode) noexcept
{
    switch (mode)
    {
    case PlayerAnimationMode::assault: return WeaponFamily::assault;
    case PlayerAnimationMode::pistol: return WeaponFamily::pistol;
    case PlayerAnimationMode::pump_shotgun: return WeaponFamily::shotgun;
    case PlayerAnimationMode::bolt_rifle: return WeaponFamily::rifle;
    case PlayerAnimationMode::smg: return WeaponFamily::smg;
    case PlayerAnimationMode::lmg: return WeaponFamily::lmg;
    case PlayerAnimationMode::space_assault: return WeaponFamily::space_assault;
    case PlayerAnimationMode::space_pistol: return WeaponFamily::space_pistol;
    case PlayerAnimationMode::space_pump_shotgun: return WeaponFamily::space_shotgun;
    case PlayerAnimationMode::space_bolt_rifle: return WeaponFamily::space_rifle;
    case PlayerAnimationMode::space_smg: return WeaponFamily::space_smg;
    case PlayerAnimationMode::rpg: return WeaponFamily::rpg;
    default: return WeaponFamily::none;
    }
}

WeaponFamily weapon_family(std::uint16_t item) noexcept
{
    return weapon_family(item_player_animation_mode(static_cast<InventoryItemId>(item)));
}

struct WeaponClipNames final
{
    std::string_view idle;
    std::string_view walk;
    std::string_view use;
    std::string_view shoulder;
    std::string_view shoulder_idle;
    std::string_view shoulder_walk;
    std::string_view shoulder_use;
    std::string_view reload;
};

WeaponClipNames weapon_clip_names(WeaponFamily family) noexcept
{
    switch (family)
    {
    case WeaponFamily::assault:
        return {"HoldAssultIdle", "AKRun", "AssaultShoot", "AssaultShoulder",
                "AssaultShoulderIdle", "AssaultShoulderWalk", "AssaultShoulderShoot",
                "AssaultReload"};
    case WeaponFamily::pistol:
        return {"HoldPistolIdle", "PistolWalk", "PistolShoot", "PistolShoulder",
                "PistolShoulderIdle", "PistolShoulderWalk", "PistolShoulderShoot",
                "PistolReload"};
    case WeaponFamily::shotgun:
        return {"HoldAssultIdle", "AKRun", "PumpShotgunShoot", "AssaultShoulder",
                "AssaultShoulderIdle", "AssaultShoulderWalk", "PumpShotgunShoulderShoot",
                "PumpShotgunReload"};
    case WeaponFamily::rifle:
        return {"RifleIdle", "RifleWalk", "RifleShoot", "RifleShoulder",
                "RifleShoulderIdle", "RifleShoulderWalk", "RifleShoulderShoot", "RifleReload"};
    case WeaponFamily::smg:
        return {"SMGIdle", "SMGWalk", "SMGShoot", "SMGShoulder", "SMGShoulderIdle",
                "SMGShoulderWalk", "SMGShoulderShoot", "SMGReload"};
    case WeaponFamily::lmg:
        return {"LMGIdle", "LMGRun", "LMGShoot", "LMGShoulder", "LMGShoulderIdle",
                "LMGShoulderWalk", "LMGShoulderShoot", "LMGReload"};
    case WeaponFamily::space_assault:
        return {"LaserGunIdle", "LaserGunRun", "LaserGunShoot", "LaserGunShoulder",
                "LaserGunShoulderIdle", "LaserGunShoulderWalk", "LaserGunShoulderShoot",
                "LaserGunReload"};
    case WeaponFamily::space_pistol:
        return {"LaserPistolIdle", "LaserPistolRun", "LaserPistolShoot",
                "LaserPistolShoulder", "LaserPistolShoulderIdle", "LaserPistolShoulderWalk",
                "LaserPistolShoulderShoot", "LaserPistolReload"};
    case WeaponFamily::space_shotgun:
        return {"LaserGunIdle", "LaserGunRun", "LaserShotgunShoot", "LaserGunShoulder",
                "LaserGunShoulderIdle", "LaserGunShoulderWalk",
                "LaserShotgunShoulderShoot", "LaserShotgunReload"};
    case WeaponFamily::space_rifle:
        return {"LaserRifleIdle", "LaserRifleRun", "LaserRifleShoot",
                "LaserRifleShoulder", "LaserRifleShoulderIdle", "LaserRifleShoulderWalk",
                "LaserRifleShoulderShoot", "LaserRifleReload"};
    case WeaponFamily::space_smg:
        return {"LaserSMGIdle", "LaserSMGRun", "LaserSMGShoot", "LaserSMGShoulder",
                "LaserSMGShoulderIdle", "LaserSMGShoulderWalk", "LaserSMGShoulderShoot",
                "LaserSMGReload"};
    case WeaponFamily::rpg:
        return {"RPGHold", "RPGWalk", "RPGShoot", "AssaultShoulder",
                "AssaultShoulderIdle", "AssaultShoulderWalk", "PumpShotgunShoulderShoot",
                "PumpShotgunReload"};
    case WeaponFamily::none:
    default:
        return {};
    }
}

void select_first_person_geometry(AvatarBatch &batch)
{
    // The mapped avatar batches supply clothing sleeves over the ProxyBoy carrier's
    // skin arms. Select the complete shoulder-down branches: the depth-tested
    // viewmodel pass resolves the overlap per pixel exactly like the original
    // GunEyePoint render resolved ProxyBoy against its clothing.
    constexpr float minimum_branch_weight = 0.15F;
    std::vector<bool> is_right_arm(batch.vertices.size(), false);
    std::vector<bool> is_left_arm(batch.vertices.size(), false);
    std::vector<float> right_wrist_weight(batch.vertices.size(), 0.0F);
    std::vector<float> left_wrist_weight(batch.vertices.size(), 0.0F);
    for (std::size_t index = 0; index < batch.vertices.size(); ++index)
    {
        const float left = branch_weight(batch.vertices[index], 20);  // ShoulderLeft
        const float right = branch_weight(batch.vertices[index], 22); // ShoulderRight
        is_right_arm[index] = right >= minimum_branch_weight && right >= left;
        is_left_arm[index] = left >= minimum_branch_weight && left > right;
        right_wrist_weight[index] = branch_weight(batch.vertices[index], 36); // WristRight
        left_wrist_weight[index] = branch_weight(batch.vertices[index], 33);  // WristLeft
    }
    const bool base_body = batch.name.starts_with("00000002-") &&
                           batch.name.find(":body:") != std::string::npos;
    const bool hand_component = (batch.category_mask & 0x00000080U) != 0U;
    const bool combined_sleeve = (batch.category_mask & 0x00000008U) != 0U ||
                                 (batch.category_mask & 0x00800000U) != 0U;
    if (hand_component && !combined_sleeve)
        return;
    for (std::size_t triangle = 0; triangle + 2U < batch.indices.size(); triangle += 3U)
    {
        const auto first = batch.indices[triangle];
        const auto second = batch.indices[triangle + 1U];
        const auto third = batch.indices[triangle + 2U];
        const auto append_branch = [&](bool selected, float first_wrist, float second_wrist,
                                       float third_wrist,
                                       std::vector<std::uint16_t> &destination) {
            if (!selected)
                return;
            // Remove the naked imported palm where the watertight carrier takes
            // over, but preserve clothing batches that intentionally cover it.
            const bool duplicate_naked_hand = (base_body || combined_sleeve) &&
                                              first_wrist >= minimum_branch_weight &&
                                              second_wrist >= minimum_branch_weight &&
                                              third_wrist >= minimum_branch_weight;
            if (duplicate_naked_hand)
                return;
            destination.push_back(first);
            destination.push_back(second);
            destination.push_back(third);
        };
        append_branch(is_right_arm[first] && is_right_arm[second] && is_right_arm[third],
                      right_wrist_weight[first], right_wrist_weight[second],
                      right_wrist_weight[third], batch.first_person_indices);
        append_branch(is_left_arm[first] && is_left_arm[second] && is_left_arm[third],
                      left_wrist_weight[first], left_wrist_weight[second],
                      left_wrist_weight[third], batch.left_person_indices);
    }
}

SDL_Texture *texture_from_png(SDL_Renderer *renderer, const std::vector<std::uint8_t> &png)
{
    if (png.empty())
    {
        return nullptr;
    }
    SDL_IOStream *stream = SDL_IOFromConstMem(png.data(), png.size());
    if (stream == nullptr)
    {
        return nullptr;
    }
    SDL_Surface *surface = SDL_LoadPNG_IO(stream, true);
    if (surface == nullptr)
    {
        return nullptr;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture != nullptr)
    {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

} // namespace

class AvatarViewmodel::Impl final
{
  public:
    Impl(SDL_Renderer *renderer, const std::filesystem::path &path) : renderer_(renderer)
    {
        // BasicEffect renders an untextured mesh as white multiplied by its
        // authored diffuse/vertex colour. The shared D3D entity pass requires
        // an SRV, so provide that implicit white material explicitly. Without
        // it the untextured skin head and several gun/tool parts were skipped.
        if (SDL_Surface *white = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32))
        {
            SDL_FillSurfaceRect(white, nullptr,
                                SDL_MapSurfaceRGBA(white, 255U, 255U, 255U, 255U));
            white_texture_ = SDL_CreateTextureFromSurface(renderer_, white);
            SDL_DestroySurface(white);
            if (white_texture_ != nullptr)
                SDL_SetTextureScaleMode(white_texture_, SDL_SCALEMODE_LINEAR);
        }
        try
        {
            BinaryReader reader{path};
            if (reader.read<std::uint32_t>() != 0x5641434FU)
            {
                throw std::runtime_error{"invalid .omavatar signature"};
            }
            const int version = reader.read<std::int32_t>();
            const int bone_count = reader.read<std::int32_t>();
            if (version < 1 || version > 3 || bone_count != 71)
            {
                throw std::runtime_error{"unsupported .omavatar skeleton"};
            }
            std::vector<std::array<float, 16>> inverse_bind_pose(
                static_cast<std::size_t>(bone_count));
            for (auto &matrix : inverse_bind_pose)
            {
                for (float &component : matrix)
                {
                    component = reader.read<float>();
                }
            }
            inverse_bind_pose_ = inverse_bind_pose;
            if (version >= 2)
            {
                source_pose_local_.resize(static_cast<std::size_t>(bone_count));
                for (auto &matrix : source_pose_local_)
                {
                    for (float &component : matrix)
                        component = reader.read<float>();
                }
            }
            else
            {
                source_pose_local_.resize(static_cast<std::size_t>(bone_count));
                source_pose_local_[0] = inverse_matrix(inverse_bind_pose_[0]);
                for (int bone = 1; bone < bone_count; ++bone)
                {
                    const auto absolute = inverse_matrix(inverse_bind_pose_[static_cast<std::size_t>(bone)]);
                    const auto parent_absolute = inverse_matrix(inverse_bind_pose_[0]);
                    source_pose_local_[static_cast<std::size_t>(bone)] =
                        multiply_matrix(absolute, inverse_matrix(parent_absolute));
                }
            }
            const int batch_count = reader.read<std::int32_t>();
            if (batch_count <= 0 || batch_count > 128)
            {
                throw std::runtime_error{"invalid .omavatar batch count"};
            }
            batches_.reserve(static_cast<std::size_t>(batch_count));
            std::array<float, 3> base_skin_diffuse{1.0F, 1.0F, 1.0F};
            std::vector<std::uint8_t> base_skin_texture;
            for (int batch_index = 0; batch_index < batch_count; ++batch_index)
            {
                AvatarBatch batch;
                batch.name = reader.dotnet_string();
                const std::size_t face_layer = batch.name.find(":face-layer-");
                if (face_layer != std::string::npos)
                {
                    const std::size_t usage_begin = face_layer + std::string_view{":face-layer-"}.size();
                    const std::size_t frame_marker = batch.name.find("-frame-", usage_begin);
                    if (frame_marker != std::string::npos)
                    {
                        const std::size_t frame_begin = frame_marker + std::string_view{"-frame-"}.size();
                        const std::size_t frame_end = batch.name.find(':', frame_begin);
                        try
                        {
                            batch.face_texture_usage = std::stoi(
                                batch.name.substr(usage_begin, frame_marker - usage_begin));
                            batch.face_frame = std::stoi(batch.name.substr(
                                frame_begin, frame_end == std::string::npos
                                                 ? std::string::npos
                                                 : frame_end - frame_begin));
                        }
                        catch (...)
                        {
                            batch.face_texture_usage = -1;
                            batch.face_frame = -1;
                        }
                    }
                }
                if (version >= 3)
                {
                    batch.category_mask = reader.read<std::uint32_t>();
                    (void)reader.read<std::int32_t>(); // Avatar shader id.
                    (void)reader.read<std::uint8_t>(); // Palette mask.
                    for (int component = 0; component < 12; ++component)
                    {
                        (void)reader.read<float>();
                    }
                }
                const int vertex_count = reader.read<std::int32_t>();
                if (vertex_count <= 0 || vertex_count > 100000)
                {
                    throw std::runtime_error{"invalid .omavatar vertex count"};
                }
                batch.vertices.resize(static_cast<std::size_t>(vertex_count));
                for (auto &vertex : batch.vertices)
                {
                    for (float &component : vertex.position)
                    {
                        component = reader.read<float>();
                    }
                    for (float &component : vertex.normal)
                    {
                        component = reader.read<float>();
                    }
                    for (float &component : vertex.uv)
                    {
                        component = reader.read<float>();
                    }
                    reader.read_bytes(vertex.bindings.data(), vertex.bindings.size());
                    reader.read_bytes(vertex.weights.data(), vertex.weights.size());
                    reader.read_bytes(vertex.color.data(), vertex.color.size());
                }
                const int index_count = reader.read<std::int32_t>();
                if (index_count <= 0 || index_count > 1000000 || index_count % 3 != 0)
                {
                    throw std::runtime_error{"invalid .omavatar index count"};
                }
                batch.indices.resize(static_cast<std::size_t>(index_count));
                for (auto &index : batch.indices)
                {
                    index = reader.read<std::uint16_t>();
                    if (index >= batch.vertices.size())
                    {
                        throw std::runtime_error{"invalid .omavatar triangle index"};
                    }
                }
                for (float &component : batch.diffuse)
                {
                    component = reader.read<float>();
                }
                const int texture_length = reader.read<std::int32_t>();
                if (texture_length < 0 || texture_length > 32 * 1024 * 1024)
                {
                    throw std::runtime_error{"invalid .omavatar texture length"};
                }
                batch.texture_png = reader.bytes(static_cast<std::size_t>(texture_length));
                select_first_person_geometry(batch);
                const bool base_body = batch.name.starts_with("00000002-") &&
                                       batch.name.find(":body:") != std::string::npos;
                const bool material_overlay =
                    batch.name.find(":material-overlay-") != std::string::npos;
                batch.material_overlay = material_overlay;
                if (base_body && !material_overlay)
                {
                    base_skin_diffuse = batch.diffuse;
                    base_skin_texture = batch.texture_png;
                }
                const bool hand_component = (batch.category_mask & 0x00000080U) != 0U;
                if (!material_overlay && hand_component)
                {
                    // A bare hand component uses the base body's skin
                    // material; its own vertex mask is not a standalone tint.
                    batch.diffuse = base_skin_diffuse;
                    batch.texture_png = base_skin_texture;
                }
                batch.texture = texture_from_png(renderer_, batch.texture_png);
                triangle_count_ += batch.first_person_indices.size() / 3U;
                batches_.push_back(std::move(batch));
            }
            loaded_ = triangle_count_ > 0U;
            const auto derived = derived_data_path();
            pick_idle_ = load_animation(derived / "fps_idle.omza");
            pick_use_ = load_animation(derived / "fps_pick.omza");
            generic_idle_ = load_animation(derived / "generic_idle.omza");
            generic_use_ = load_animation(derived / "generic_use.omza");
            block_idle_ = load_animation(derived / "block_idle.omza");
            block_use_ = load_animation(derived / "block_use.omza");
            const auto animation_directory = derived / "animations";
            if (std::filesystem::exists(animation_directory))
            {
                for (const auto &entry : std::filesystem::directory_iterator(animation_directory))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".omza")
                        animation_library_.emplace(entry.path().stem().string(),
                                                   load_animation(entry.path()));
                }
            }
            pickaxe_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pickaxe.omsm");
            compass_ = std::make_unique<StaticModelAsset>(renderer_, derived / "compass.omsm");
            pistol_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pistol.omsm");
            knife_ = std::make_unique<StaticModelAsset>(renderer_, derived / "knife.omsm");
            ammo_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ammo.omsm");
            torch_ = std::make_unique<StaticModelAsset>(renderer_, derived / "torch.omsm");
            // Every held weapon/tool family from the original inventory classes.
            // StaticModelAsset tolerates a missing export by reporting !loaded().
            spade_ = std::make_unique<StaticModelAsset>(renderer_, derived / "spade.omsm");
            axe_ = std::make_unique<StaticModelAsset>(renderer_, derived / "axe.omsm");
            ak_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ak.omsm");
            shotgun_ = std::make_unique<StaticModelAsset>(renderer_, derived / "shotgun.omsm");
            rifle_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rifle.omsm");
            smg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "smg.omsm");
            lmg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "lmg.omsm");
            rpg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rpg.omsm");
            grenade_ = std::make_unique<StaticModelAsset>(renderer_, derived / "grenade.omsm");
            space_ak_ = std::make_unique<StaticModelAsset>(renderer_, derived / "space_ak.omsm");
            space_pistol_ =
                std::make_unique<StaticModelAsset>(renderer_, derived / "space_pistol.omsm");
            space_shotgun_ =
                std::make_unique<StaticModelAsset>(renderer_, derived / "space_shotgun.omsm");
            space_rifle_ =
                std::make_unique<StaticModelAsset>(renderer_, derived / "space_rifle.omsm");
            space_smg_ = std::make_unique<StaticModelAsset>(renderer_, derived / "space_smg.omsm");
            saber_ = std::make_unique<StaticModelAsset>(renderer_, derived / "saber.omsm");
            // Palm-carried materials and instruments (ModelInventoryItemClass /
            // Compass-style Hand transforms).
            ore_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ore.omsm");
            gems_ = std::make_unique<StaticModelAsset>(renderer_, derived / "gems.omsm");
            bars_ = std::make_unique<StaticModelAsset>(renderer_, derived / "bars.omsm");
            clock_ = std::make_unique<StaticModelAsset>(renderer_, derived / "clock.omsm");
            locator_ = std::make_unique<StaticModelAsset>(renderer_, derived / "locator.omsm");
            teleporter_ =
                std::make_unique<StaticModelAsset>(renderer_, derived / "teleporter.omsm");
            gunpowder_ =
                std::make_unique<StaticModelAsset>(renderer_, derived / "gunpowder.omsm");
            BinaryReader carrier_reader{derived / "proxy_hand.omhc"};
            if (carrier_reader.read<std::uint32_t>() != 0x43484D4FU)
                throw std::runtime_error{"invalid proxy hand carrier"};
            const int carrier_version = carrier_reader.read<std::int32_t>();
            if (carrier_version != 2)
                throw std::runtime_error{"outdated proxy hand carrier"};
            const int carrier_bones = carrier_reader.read<std::int32_t>();
            const int carrier_parts = carrier_reader.read<std::int32_t>();
            if (carrier_bones != 71 || carrier_parts <= 0 || carrier_parts > 32)
                throw std::runtime_error{"unsupported proxy hand carrier"};
            proxy_avatar_bone_.resize(static_cast<std::size_t>(carrier_bones));
            proxy_parents_.resize(static_cast<std::size_t>(carrier_bones));
            avatar_shape_scale_.resize(static_cast<std::size_t>(carrier_bones));
            for (int &avatar_bone : proxy_avatar_bone_)
                avatar_bone = carrier_reader.read<std::int32_t>();
            for (int &parent : proxy_parents_)
                parent = carrier_reader.read<std::int32_t>();
            for (auto &scale : avatar_shape_scale_)
                for (float &component : scale)
                    component = carrier_reader.read<float>();
            for (std::size_t proxy_bone = 0; proxy_bone < proxy_avatar_bone_.size(); ++proxy_bone)
            {
                const int avatar_bone = proxy_avatar_bone_[proxy_bone];
                if (avatar_bone < 0 || avatar_bone >= carrier_bones ||
                    (proxy_bone > 0U &&
                     (proxy_parents_[proxy_bone] < 0 ||
                      proxy_parents_[proxy_bone] >= static_cast<int>(proxy_bone))))
                    throw std::runtime_error{"invalid proxy skeleton mapping"};
                proxy_bone_by_avatar_[static_cast<std::size_t>(avatar_bone)] =
                    static_cast<int>(proxy_bone);
            }
            proxy_inverse_bind_.resize(static_cast<std::size_t>(carrier_bones));
            proxy_bind_local_.resize(static_cast<std::size_t>(carrier_bones));
            for (auto *matrices : {&proxy_inverse_bind_, &proxy_bind_local_})
                for (auto &matrix : *matrices)
                    for (float &component : matrix)
                        component = carrier_reader.read<float>();
            const auto is_descendant = [&](int bone, int ancestor) {
                while (bone >= 0 && bone < static_cast<int>(block_idle_.parents.size()))
                {
                    if (bone == ancestor)
                        return true;
                    bone = block_idle_.parents[static_cast<std::size_t>(bone)];
                }
                return false;
            };
            carrier_parts_.reserve(static_cast<std::size_t>(carrier_parts));
            for (int part_index = 0; part_index < carrier_parts; ++part_index)
            {
                CarrierPart part;
                for (float &component : part.diffuse)
                    component = carrier_reader.read<float>();
                part.use_texture = carrier_reader.read<std::uint8_t>() != 0U;
                part.use_vertex_color = carrier_reader.read<std::uint8_t>() != 0U;
                const int texture_length = carrier_reader.read<std::int32_t>();
                if (texture_length < 0 || texture_length > 32 * 1024 * 1024)
                    throw std::runtime_error{"invalid carrier texture"};
                part.texture = texture_from_png(
                    renderer_, carrier_reader.bytes(static_cast<std::size_t>(texture_length)));
                const int vertex_count = carrier_reader.read<std::int32_t>();
                if (vertex_count <= 0 || vertex_count > 100000)
                    throw std::runtime_error{"invalid carrier vertices"};
                part.vertices.resize(static_cast<std::size_t>(vertex_count));
                for (auto &vertex : part.vertices)
                {
                    for (float &component : vertex.position)
                        component = carrier_reader.read<float>();
                    for (float &component : vertex.normal)
                        component = carrier_reader.read<float>();
                    for (float &component : vertex.uv)
                        component = carrier_reader.read<float>();
                    carrier_reader.read_bytes(vertex.color.data(), vertex.color.size());
                    carrier_reader.read_bytes(vertex.bindings.data(), vertex.bindings.size());
                    for (float &weight : vertex.weights)
                        weight = carrier_reader.read<float>();
                }
                const int index_count = carrier_reader.read<std::int32_t>();
                if (index_count <= 0 || index_count > 1000000 || index_count % 3 != 0)
                    throw std::runtime_error{"invalid carrier indices"};
                part.indices.resize(static_cast<std::size_t>(index_count));
                for (auto &index : part.indices)
                {
                    index = carrier_reader.read<std::uint16_t>();
                    if (index >= part.vertices.size())
                        throw std::runtime_error{"invalid carrier triangle"};
                }
                for (std::size_t triangle = 0; triangle < part.indices.size(); triangle += 3U)
                {
                    float right_weight{};
                    float left_weight{};
                    float right_finger_weight{};
                    float left_finger_weight{};
                    for (std::size_t corner = 0; corner < 3U; ++corner)
                    {
                        const auto &vertex = part.vertices[part.indices[triangle + corner]];
                        for (std::size_t influence = 0; influence < vertex.bindings.size(); ++influence)
                        {
                            const int proxy_bone = vertex.bindings[influence];
                            const int avatar_bone = proxy_avatar_bone_[
                                static_cast<std::size_t>(proxy_bone)];
                            // The stock FPS pass renders the complete right
                            // sleeve/arm chain, not only WristRight's hand
                            // descendants. ShoulderRight is AvatarBone 22;
                            // selecting from there retains shoulder, elbow,
                            // forearm, wrist and articulated fingers as one rig.
                            if (is_descendant(avatar_bone, 22))
                                right_weight += vertex.weights[influence];
                            // The left arm mirrors the right: complete ShoulderLeft chain.
                            // The depth pass near-clips whatever crosses the camera plane,
                            // the same way the stock 0.01m near plane did.
                            if (is_descendant(avatar_bone, 20))
                                left_weight += vertex.weights[influence];
                            if (avatar_is_finger(avatar_bone, true))
                                right_finger_weight += vertex.weights[influence];
                            if (avatar_is_finger(avatar_bone, false))
                                left_finger_weight += vertex.weights[influence];
                        }
                    }
                    if (right_weight >= left_weight && right_weight > 0.01F)
                    {
                        auto &destination = right_finger_weight >= right_weight * 0.35F
                                                ? part.right_finger_indices
                                                : part.right_arm_indices;
                        destination.insert(
                            destination.end(),
                            part.indices.begin() + static_cast<std::ptrdiff_t>(triangle),
                            part.indices.begin() + static_cast<std::ptrdiff_t>(triangle + 3U));
                    }
                    else if (left_weight > 0.01F)
                    {
                        auto &destination = left_finger_weight >= left_weight * 0.35F
                                                ? part.left_finger_indices
                                                : part.left_arm_indices;
                        destination.insert(
                            destination.end(),
                            part.indices.begin() + static_cast<std::ptrdiff_t>(triangle),
                            part.indices.begin() + static_cast<std::ptrdiff_t>(triangle + 3U));
                    }
                }
                carrier_triangle_count_ +=
                    (part.right_arm_indices.size() + part.right_finger_indices.size() +
                     part.left_arm_indices.size() + part.left_finger_indices.size()) /
                    3U;
                carrier_parts_.push_back(std::move(part));
            }
            if (carrier_triangle_count_ == 0U)
                throw std::runtime_error{"proxy carrier has no first-person arm geometry"};
            triangle_count_ += carrier_triangle_count_;
            loaded_ = true;
            animation_loaded_ = true;
#if defined(_WIN32)
            // The depth-tested pass is optional: without it the painter fallback still
            // renders, it just cannot resolve self-overlapping arm geometry.
            initialize_depth_pipeline();
#endif
        }
        catch (const std::exception &exception)
        {
            std::cerr << "Avatar viewmodel failed to load: " << exception.what() << '\n';
            loaded_ = false;
            batches_.clear();
            triangle_count_ = 0U;
        }
    }

    ~Impl()
    {
        for (auto &batch : batches_)
        {
            SDL_DestroyTexture(batch.texture);
        }
        for (auto &part : carrier_parts_)
        {
            SDL_DestroyTexture(part.texture);
        }
        SDL_DestroyTexture(white_texture_);
        SDL_DestroyTexture(depth_target_texture_);
    }

    void draw(const ui::UiMetrics &metrics, const world::PlayerState &player, float elapsed_seconds,
              bool reduced_motion, float viewmodel_fov_degrees, SDL_Texture *block_atlas)
    {
        if (!loaded_)
        {
            return;
        }

        const float height = static_cast<float>(metrics.output_height);
        const float width = static_cast<float>(metrics.output_width);
        const float idle =
            reduced_motion ? 0.0F : std::sin(elapsed_seconds * 1.7F) * height * 0.002F;
        constexpr float pi = 3.14159265358979323846F;
        const bool holding_block = player.selected_block != world::BlockType::empty &&
                                   player.selected_item_id != 14U;
        const PlayerAnimationMode animation_mode = item_player_animation_mode(
            static_cast<InventoryItemId>(player.selected_item_id));
        const NativeAnimation *idle_animation = &pick_idle_;
        const NativeAnimation *use_animation = &pick_use_;
        if (holding_block)
        {
            idle_animation = &block_idle_;
            use_animation = &block_use_;
        }
        else if (animation_mode == PlayerAnimationMode::generic)
        {
            // InventoryItemClass defaults to Generic: this covers the stick,
            // instruments, resources, components and every base model item.
            idle_animation = &generic_idle_;
            use_animation = &generic_use_;
        }
        const auto clip_or = [&](std::string_view name, const NativeAnimation *fallback) {
            const auto found = animation_library_.find(std::string{name});
            return found == animation_library_.end() ? fallback : &found->second;
        };
        const WeaponFamily family = weapon_family(player.selected_item_id);
        const WeaponClipNames weapon_names = weapon_clip_names(family);
        const NativeAnimation *grenade_cook_animation = nullptr;
        const NativeAnimation *grenade_throw_animation = nullptr;
        if (player.selected_item_id == 110U)
        {
            // PlayerMode.Grenade: hold clip from CollarRight; the throw is the
            // BackUpper-rooted action clip (cook staging still pending app-side).
            idle_animation = clip_or("GrenadeIdle", idle_animation);
            grenade_cook_animation = clip_or("GrenadeCook", use_animation);
            grenade_throw_animation = clip_or("GrenadeThrow", use_animation);
            use_animation = grenade_throw_animation;
        }
        // PickWalk/BlockWalk/GenericWalk are registered from the same content the
        // idle clips come from; the original plays them on channel 2 while moving.
        const NativeAnimation *walk_animation =
            holding_block ? clip_or("BlockHoldWalk", idle_animation)
            : player.selected_item_id == 110U ? clip_or("GrenadeWalk", idle_animation)
            : animation_mode == PlayerAnimationMode::generic
                ? clip_or("GenericWalk", idle_animation)
                : clip_or("FPSWalk", idle_animation);
        const NativeAnimation *shoulder_animation = idle_animation;
        const NativeAnimation *shoulder_idle_animation = idle_animation;
        const NativeAnimation *shoulder_walk_animation = idle_animation;
        const NativeAnimation *shoulder_use_animation = use_animation;
        const NativeAnimation *reload_animation = idle_animation;
        if (family != WeaponFamily::none)
        {
            idle_animation = clip_or(weapon_names.idle, idle_animation);
            walk_animation = clip_or(weapon_names.walk, idle_animation);
            use_animation = clip_or(weapon_names.use, use_animation);
            shoulder_animation = clip_or(weapon_names.shoulder, idle_animation);
            shoulder_idle_animation = clip_or(weapon_names.shoulder_idle, idle_animation);
            shoulder_walk_animation =
                clip_or(weapon_names.shoulder_walk, shoulder_idle_animation);
            shoulder_use_animation = clip_or(weapon_names.shoulder_use, use_animation);
            reload_animation = clip_or(weapon_names.reload, idle_animation);
        }
        world::ViewmodelCue cue = player.viewmodel_cue;
        if (cue == world::ViewmodelCue::idle && player.using_tool)
            cue = world::ViewmodelCue::primary_use;
        const NativeAnimation *stand_animation = clip_or("Stand0", nullptr);
        const AnimationFrameSample stand_frame =
            stand_animation == nullptr ? AnimationFrameSample{}
                                       : stand_animation->sample(elapsed_seconds, true);
        if (!animation_loaded_ || stand_frame.first == nullptr || stand_frame.second == nullptr)
            return;

        // CastleMiner Z keeps Stand on channel 0, the held-item/locomotion clip on channel 2 and
        // the one-shot action clip on channel 3, each with a registered influence root. Every
        // clip change goes through AnimBlender with the original blend times, so switching
        // items, starting to walk, shouldering or finishing a swing cross-fades instead of
        // snapping between poses.
        const bool first_frame = last_elapsed_seconds_ < 0.0F ||
                                 elapsed_seconds < last_elapsed_seconds_;
        const float delta_seconds =
            first_frame ? 0.0F : std::min(elapsed_seconds - last_elapsed_seconds_, 0.25F);
        last_elapsed_seconds_ = elapsed_seconds;
        // Locomotion/idle registrations use CollarRight for pick/block/generic items and
        // BackUpper for every firearm family and the bare-fist FPS clips. GenericUse is the one
        // generic-item action registered from BackUpper.
        const bool generic_item = animation_mode == PlayerAnimationMode::generic;
        const bool fist_item = player.selected_item_id == 0U;
        const bool grenade_item = player.selected_item_id == 110U;
        const int locomotion_root =
            (family != WeaponFamily::none || fist_item) ? 5 : 16;
        // GenericUse and the grenade cook/throw clips register from BackUpper.
        const int action_root =
            (family != WeaponFamily::none || fist_item || generic_item || grenade_item) ? 5 : 16;
        const bool moving = std::hypot(player.velocity.x, player.velocity.z) > 0.1F;
        const float cue_progress = std::clamp(player.viewmodel_cue_progress, 0.0F, 1.0F);

        const NativeAnimation *desired_item = moving ? walk_animation : idle_animation;
        float item_progress = -1.0F; // negative: the channel free-runs and loops.
        float item_blend = 0.25F;
        const NativeAnimation *desired_action = nullptr;
        switch (cue)
        {
        case world::ViewmodelCue::primary_use:
            desired_action = use_animation;
            break;
        case world::ViewmodelCue::shoulder:
            desired_item = shoulder_animation;
            item_progress = cue_progress;
            item_blend = 0.0F;
            break;
        case world::ViewmodelCue::unshoulder:
            // The cue already carries lowering progress; the original replays the
            // shoulder clip reversed with a zero blend.
            desired_item = shoulder_animation;
            item_progress = 1.0F - cue_progress;
            item_blend = 0.0F;
            break;
        case world::ViewmodelCue::shoulder_idle:
            // Shouldered movement swaps to the shoulder-walk clip with a zero blend and
            // back to shoulder-idle with the original 0.1s blend.
            desired_item = moving ? shoulder_walk_animation : shoulder_idle_animation;
            item_blend = moving ? 0.0F : 0.1F;
            break;
        case world::ViewmodelCue::shoulder_use:
            desired_item = moving ? shoulder_walk_animation : shoulder_idle_animation;
            item_blend = moving ? 0.0F : 0.1F;
            desired_action = shoulder_use_animation;
            break;
        case world::ViewmodelCue::reload:
            desired_item = reload_animation;
            item_progress = cue_progress;
            break;
        case world::ViewmodelCue::grenade_cook:
            desired_action = grenade_cook_animation != nullptr ? grenade_cook_animation
                                                                : use_animation;
            break;
        case world::ViewmodelCue::grenade_throw:
            desired_action = grenade_throw_animation != nullptr ? grenade_throw_animation
                                                                 : use_animation;
            break;
        case world::ViewmodelCue::idle:
        default:
            break;
        }

        if (!item_channel_.has_current || item_channel_.current.clip != desired_item)
        {
            ChannelPlayer next{};
            next.clip = desired_item;
            next.influence_root = locomotion_root;
            next.time_driven = item_progress < 0.0F;
            next.looping = next.time_driven;
            next.time = next.time_driven ? 0.0F
                                         : item_progress * desired_item->duration;
            item_channel_.play(next, first_frame ? 0.0F : item_blend);
        }
        else
        {
            ChannelPlayer &running = item_channel_.current;
            running.influence_root = locomotion_root;
            running.time_driven = item_progress < 0.0F;
            running.looping = running.time_driven;
            if (item_progress >= 0.0F)
                running.time = item_progress * running.clip->duration;
        }
        if (desired_action != nullptr)
        {
            // A fresh swing restarts the action clip with a zero blend, exactly like
            // Play(clip, 3, TimeSpan.Zero) on the UsingTool edge. Progress wrapping
            // backwards is the continuous-mining swing boundary.
            const bool restart = !action_channel_.has_current ||
                                 action_channel_.current.clip != desired_action ||
                                 cue_progress + 0.25F < last_action_progress_;
            if (restart)
            {
                ChannelPlayer next{};
                next.clip = desired_action;
                next.influence_root = action_root;
                next.time_driven = false;
                next.looping = false;
                next.time = cue_progress * desired_action->duration;
                action_channel_.play(next, 0.0F);
            }
            else
            {
                action_channel_.current.influence_root = action_root;
                action_channel_.current.time = cue_progress * desired_action->duration;
            }
            last_action_progress_ = cue_progress;
        }
        else if (action_channel_.has_current)
        {
            // ClearAnimation(3, 0.25s): the finished action pose fades back to the
            // locomotion pose underneath instead of snapping.
            action_channel_.clear(first_frame ? 0.0F : 0.25F);
            last_action_progress_ = 0.0F;
        }
        item_channel_.advance(delta_seconds);
        action_channel_.advance(delta_seconds);
        if (first_frame)
        {
            item_channel_.finish_blends();
            action_channel_.finish_blends();
        }

        std::array<BoneTransform, 71> interpolated_frame{};
        for (std::size_t bone = 0U; bone < interpolated_frame.size(); ++bone)
        {
            interpolated_frame[bone] = interpolate_transform(
                stand_frame.first[bone], stand_frame.second[bone], stand_frame.amount);
        }
        item_channel_.apply(interpolated_frame);
        action_channel_.apply(interpolated_frame);

        std::array<Matrix4, 71> absolute{};
        std::array<Matrix4, 71> skin{};
        std::array<Matrix4, 71> avatar_skin{};
        Matrix4 proxy_root = rotation_y(pi);
        proxy_root[12] = 0.0F;
        proxy_root[13] = 0.7769F;
        proxy_root[14] = -0.008664F;
        for (std::size_t proxy_bone = 0; proxy_bone < absolute.size(); ++proxy_bone)
        {
            const int avatar_bone = proxy_avatar_bone_[proxy_bone];
            const Matrix4 animated =
                compose_matrix(interpolated_frame[static_cast<std::size_t>(avatar_bone)]);
            // Avatar.OnUpdate in the original client preserves the animation's complete 3x3
            // transform (rotation and authored scale) and replaces only the translation with
            // ProxyBoy's bind-pose offset. Normalizing the rows here subtly changed hand volume
            // and accumulated into badly displaced fingers in two-handed weapon poses.
            Matrix4 local = animated;
            if (proxy_bone == 0U)
            {
                // This is the exact ProxyBoy root adjustment used by the
                // source carrier geometry.
                local = multiply_matrix(local, proxy_root);
                absolute[proxy_bone] = local;
            }
            else
            {
                local[12] = proxy_bind_local_[proxy_bone][12];
                local[13] = proxy_bind_local_[proxy_bone][13];
                local[14] = proxy_bind_local_[proxy_bone][14];
                absolute[proxy_bone] = multiply_matrix(
                    local, absolute[static_cast<std::size_t>(proxy_parents_[proxy_bone])]);
            }
            skin[proxy_bone] = multiply_matrix(
                multiply_matrix(proxy_inverse_bind_[proxy_bone],
                                scale_matrix(avatar_shape_scale_[static_cast<std::size_t>(avatar_bone)])),
                absolute[proxy_bone]);
        }
        for (std::size_t avatar_bone = 0; avatar_bone < avatar_skin.size(); ++avatar_bone)
        {
            const std::size_t proxy_bone =
                static_cast<std::size_t>(proxy_bone_by_avatar_[avatar_bone]);
            avatar_skin[avatar_bone] = multiply_matrix(
                multiply_matrix(inverse_bind_pose_[avatar_bone],
                                scale_matrix(avatar_shape_scale_[avatar_bone])),
                absolute[proxy_bone]);
        }
        // CastleMiner Z renders the first-person rig through Avatar.EyePointCamera.
        // Avatar.OnUpdate rebuilds that camera from the animated head transform,
        // strips its body-shape scale, then applies a 180-degree X/Z eye rotation
        // and a 0.05m head-local lift. Reproduce that camera instead of flattening
        // the arm along a hand-tuned 2-D corridor; the real 90-degree projection
        // preserves the palm, finger curl and PropRight attachment at every aspect.
        const auto head_bone = static_cast<std::size_t>(proxy_bone_by_avatar_[19U]);
        Matrix4 head_world = absolute[head_bone];
        for (std::size_t row = 0; row < 3U; ++row)
        {
            const float length = std::sqrt(head_world[row * 4U] * head_world[row * 4U] +
                                           head_world[row * 4U + 1U] * head_world[row * 4U + 1U] +
                                           head_world[row * 4U + 2U] * head_world[row * 4U + 2U]);
            if (length > 0.000001F)
                for (std::size_t column = 0; column < 3U; ++column)
                    head_world[row * 4U + column] /= length;
        }
        Matrix4 eye_to_head = multiply_matrix(rotation_x(pi), rotation_z(pi));
        eye_to_head[13] = 0.05F;
        const Matrix4 view = inverse_matrix(multiply_matrix(eye_to_head, head_world));
        const float aspect = width / std::max(1.0F, height);
        // The lower bound admits the shouldered zoom (half the configured FOV, down to
        // 27.5 degrees for the 55-degree minimum setting).
        const float viewmodel_fov =
            std::clamp(viewmodel_fov_degrees, 25.0F, 110.0F) * pi / 180.0F;
        const float focal = 1.0F / std::tan(viewmodel_fov * 0.5F);
        struct ScreenProjection final
        {
            SDL_FPoint point{};
            std::array<float, 3> camera{};
            float depth{};
            bool visible{};
        };
        const auto screen_position = [&](const std::array<float, 3> &position) {
            const auto camera = transform_point(position, view);
            const float depth = -camera[2];
            ScreenProjection result{};
            result.camera = {camera[0], camera[1], depth};
            result.depth = depth;
            if (depth <= 0.011F)
                return result;
            const float normalized_x = camera[0] * focal / (depth * aspect);
            const float normalized_y = camera[1] * focal / depth;
            result.point = {(normalized_x * 0.5F + 0.5F) * width,
                            (0.5F - normalized_y * 0.5F) * height + idle};
            result.visible = true;
            return result;
        };
        const auto prop_bone = static_cast<std::size_t>(proxy_bone_by_avatar_[48U]);
        const auto posed_position = [&](const CarrierVertex &source) {
            std::array<float, 3> position{};
            float total_weight{};
            for (std::size_t binding = 0; binding < source.bindings.size(); ++binding)
            {
                const float weight = source.weights[binding];
                if (weight <= 0.0F)
                    continue;
                const auto transformed = transform_point(
                    source.position, skin[static_cast<std::size_t>(source.bindings[binding])]);
                position[0] += transformed[0] * weight;
                position[1] += transformed[1] * weight;
                position[2] += transformed[2] * weight;
                total_weight += weight;
            }
            if (total_weight < 1.0F)
            {
                position[0] += source.position[0] * (1.0F - total_weight);
                position[1] += source.position[1] * (1.0F - total_weight);
                position[2] += source.position[2] * (1.0F - total_weight);
            }
            return position;
        };
        const auto posed_carrier_normal = [&](const CarrierVertex &source) {
            std::array<float, 3> normal{};
            float total_weight{};
            for (std::size_t binding = 0; binding < source.bindings.size(); ++binding)
            {
                const float weight = source.weights[binding];
                if (weight <= 0.0F)
                    continue;
                const auto transformed = transform_direction(
                    source.normal, skin[static_cast<std::size_t>(source.bindings[binding])]);
                normal[0] += transformed[0] * weight;
                normal[1] += transformed[1] * weight;
                normal[2] += transformed[2] * weight;
                total_weight += weight;
            }
            if (total_weight <= 0.0001F)
                normal = source.normal;
            return transform_direction(normal, view);
        };

        using Triangle = ViewmodelTriangle;
        // The GPU depth pass clips triangles against the true 0.01m near plane like the
        // original GunEyePoint render, so it keeps anything with one visible corner. The
        // painter fallback has no clipping and must drop those triangles entirely.
        const bool depth_pass = depth_ready_;
        const auto wrap_uv = [](float coordinate) {
            float wrapped = coordinate - std::floor(coordinate);
            if (coordinate > 0.0F && std::abs(wrapped) < 0.00001F)
                wrapped = 1.0F;
            return wrapped;
        };
        std::vector<Triangle> triangles;
        StaticModelAsset *held_model{};
        switch (player.selected_item_id)
        {
        case 13U: held_model = pickaxe_.get(); break;
        case 14U: held_model = torch_.get(); break;
        case 15U: case 16U: case 17U: case 18U:
            held_model = ore_.get(); break;
        case 19U: held_model = gems_.get(); break;
        case 20U: case 21U: case 22U:
            held_model = bars_.get(); break;
        case 23U: case 24U: case 25U: case 26U: case 27U: case 28U:
            held_model = pickaxe_.get(); break;
        case 29U: case 30U: case 31U: case 32U: case 33U:
            held_model = spade_.get(); break;
        case 34U: case 35U: case 36U: case 37U: case 38U:
            held_model = axe_.get(); break;
        case 39U: held_model = compass_.get(); break;
        case 40U: held_model = clock_.get(); break;
        case 41U: case 42U: case 43U: case 111U:
        case 44U: case 45U: case 46U: case 47U: case 48U: case 105U: case 109U:
            held_model = ammo_.get(); break;
        case 77U: held_model = locator_.get(); break;
        case 78U: held_model = teleporter_.get(); break;
        case 106U: case 108U:
            held_model = gunpowder_.get(); break;
        case 49U: case 55U: case 61U: case 67U:
            held_model = knife_.get(); break;
        case 50U: case 56U: case 62U: case 68U:
            held_model = ak_.get(); break;
        case 51U: case 57U: case 63U: case 69U:
            held_model = pistol_.get(); break;
        case 52U: case 58U: case 64U: case 70U:
            held_model = shotgun_.get(); break;
        case 53U: case 59U: case 65U: case 71U:
            held_model = rifle_.get(); break;
        case 54U: case 60U: case 66U: case 72U:
            held_model = smg_.get(); break;
        case 82U: case 87U: case 92U: case 97U:
            held_model = space_ak_.get(); break;
        case 83U: case 88U: case 93U: case 98U:
            held_model = space_pistol_.get(); break;
        case 84U: case 89U: case 94U: case 99U:
            held_model = space_shotgun_.get(); break;
        case 85U: case 90U: case 95U: case 100U:
            held_model = space_rifle_.get(); break;
        case 86U: case 91U: case 96U: case 101U:
            held_model = space_smg_.get(); break;
        case 103U: case 104U: case 112U: case 113U:
            held_model = rpg_.get(); break;
        case 110U: held_model = grenade_.get(); break;
        case 119U: case 120U: case 121U: case 122U:
            held_model = lmg_.get(); break;
        case 81U: case 114U: case 115U: case 116U: case 117U: case 118U:
            held_model = saber_.get(); break;
        default: break;
        }
        const bool holding_model = held_model != nullptr && held_model->loaded();
        // The stock first-person pass always renders both arms and trusts the authored
        // pose to keep the support hand out of frame. Only the painter fallback still
        // needs the conservative gate, because it cannot depth-resolve the overlap.
        const bool show_left_hand = depth_pass ||
                                    item_uses_support_hand(player.selected_item_id) ||
                                    cue == world::ViewmodelCue::primary_use ||
                                    cue == world::ViewmodelCue::reload;
        std::size_t item_triangles = holding_block ? 12U : 0U;
        if (holding_model)
            for (const auto &part : held_model->parts())
                item_triangles += part.indices.size() / 3U;
        triangles.reserve(carrier_triangle_count_ + item_triangles);
        const auto posed_avatar_position = [&](const SourceVertex &source) {
            std::array<float, 3> position{};
            float total_weight{};
            for (std::size_t influence = 0; influence < source.bindings.size(); ++influence)
            {
                const float weight = static_cast<float>(source.weights[influence]) / 255.0F;
                if (weight <= 0.0F)
                    continue;
                const auto transformed = transform_point(
                    source.position, avatar_skin[static_cast<std::size_t>(source.bindings[influence])]);
                position[0] += transformed[0] * weight;
                position[1] += transformed[1] * weight;
                position[2] += transformed[2] * weight;
                total_weight += weight;
            }
            if (total_weight <= 0.0001F)
                return source.position;
            if (std::abs(total_weight - 1.0F) > 0.0001F)
            {
                position[0] /= total_weight;
                position[1] /= total_weight;
                position[2] /= total_weight;
            }
            return position;
        };
        const auto posed_avatar_normal = [&](const SourceVertex &source) {
            std::array<float, 3> normal{};
            float total_weight{};
            for (std::size_t influence = 0; influence < source.bindings.size(); ++influence)
            {
                const float weight = static_cast<float>(source.weights[influence]) / 255.0F;
                if (weight <= 0.0F)
                    continue;
                const auto transformed = transform_direction(
                    source.normal,
                    avatar_skin[static_cast<std::size_t>(source.bindings[influence])]);
                normal[0] += transformed[0] * weight;
                normal[1] += transformed[1] * weight;
                normal[2] += transformed[2] * weight;
                total_weight += weight;
            }
            if (total_weight <= 0.0001F)
                normal = source.normal;
            return transform_direction(normal, view);
        };
        const char *fp_debug = SDL_getenv("OPENMINERZ_FP_DEBUG");
        const bool debug_skip_avatar = fp_debug != nullptr && fp_debug[0] == '1';
        const bool debug_skip_carrier = fp_debug != nullptr && fp_debug[0] == '2';
        for (const auto &batch : batches_)
        {
            if (debug_skip_avatar)
                break;
            const bool base_body = batch.name.starts_with("00000002-") &&
                                   batch.name.find(":body:") != std::string::npos;
            const auto append_avatar_branch = [&](const std::vector<std::uint16_t> &indices) {
                for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3U)
                {
                    Triangle output;
                    output.render_layer = 0;
                    output.texture = batch.texture;
                    bool visible = true;
                    bool any_visible = false;
                    for (std::size_t corner = 0; corner < 3U; ++corner)
                    {
                        const auto &source = batch.vertices[indices[triangle + corner]];
                        const auto projected = screen_position(posed_avatar_position(source));
                        visible = visible && projected.visible;
                        any_visible = any_visible || projected.visible;
                        output.view[corner] = projected.camera;
                        const std::array<float, 3> camera_normal = posed_avatar_normal(source);
                        const std::array<float, 3> light = environment_light(camera_normal);
                        const float vertex_red =
                            base_body ? 1.0F : static_cast<float>(source.color[0]) / 255.0F;
                        const float vertex_green =
                            base_body ? 1.0F : static_cast<float>(source.color[1]) / 255.0F;
                        const float vertex_blue =
                            base_body ? 1.0F : static_cast<float>(source.color[2]) / 255.0F;
                        const float vertex_alpha =
                            base_body ? 1.0F : static_cast<float>(source.color[3]) / 255.0F;
                        output.vertices[corner] = {
                            projected.point,
                            {batch.diffuse[0] * vertex_red * light[0],
                             batch.diffuse[1] * vertex_green * light[1],
                             batch.diffuse[2] * vertex_blue * light[2], vertex_alpha},
                            {wrap_uv(source.uv[0]), wrap_uv(source.uv[1])}};
                        // Negative response identifies rough fabric/skin to the
                        // Rebirth shader without enabling a polished specular lobe.
                        output.normal_material[corner] = {
                            camera_normal[0], camera_normal[1], -camera_normal[2], -0.36F};
                        output.depth += projected.depth;
                    }
                    if (depth_pass ? any_visible : visible)
                        triangles.push_back(output);
                }
            };
            append_avatar_branch(batch.first_person_indices);
            if (show_left_hand)
                append_avatar_branch(batch.left_person_indices);
        }
        for (const auto &part : carrier_parts_)
        {
            if (debug_skip_carrier)
                break;
            const auto append_carrier_branch = [&](const std::vector<std::uint16_t> &indices,
                                                   int render_layer) {
                for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3U)
                {
                    Triangle output;
                    output.render_layer = render_layer;
                    output.texture = part.use_texture ? part.texture : nullptr;
                    bool visible = true;
                    bool any_visible = false;
                    for (std::size_t corner = 0; corner < 3U; ++corner)
                    {
                        const auto &source = part.vertices[indices[triangle + corner]];
                        const auto position = posed_position(source);
                        const auto projected = screen_position(position);
                        visible = visible && projected.visible;
                        any_visible = any_visible || projected.visible;
                        output.view[corner] = projected.camera;
                        const std::array<float, 3> camera_normal = posed_carrier_normal(source);
                        const std::array<float, 3> light = environment_light(camera_normal);
                        const float vertex_red = part.use_vertex_color
                                                     ? static_cast<float>(source.color[0]) / 255.0F
                                                     : 1.0F;
                        const float vertex_green = part.use_vertex_color
                                                       ? static_cast<float>(source.color[1]) / 255.0F
                                                       : 1.0F;
                        const float vertex_blue = part.use_vertex_color
                                                      ? static_cast<float>(source.color[2]) / 255.0F
                                                      : 1.0F;
                        const float vertex_alpha = part.use_vertex_color
                                                       ? static_cast<float>(source.color[3]) / 255.0F
                                                       : 1.0F;
                        output.vertices[corner] = {
                            projected.point,
                            {part.diffuse[0] * vertex_red * light[0],
                             part.diffuse[1] * vertex_green * light[1],
                             part.diffuse[2] * vertex_blue * light[2], vertex_alpha},
                            {wrap_uv(source.uv[0]), wrap_uv(source.uv[1])}};
                        output.normal_material[corner] = {
                            camera_normal[0], camera_normal[1], -camera_normal[2], -0.46F};
                        output.depth += projected.depth;
                    }
                    if (depth_pass ? any_visible : visible)
                        triangles.push_back(output);
                }
            };
            // Sleeves/palms are behind the held item; articulated fingers wrap over its grip.
            // This is a stable painter decomposition for SDL's no-depth-buffer geometry path.
            append_carrier_branch(part.right_arm_indices, 0);
            if (show_left_hand)
                append_carrier_branch(part.left_arm_indices, 0);
            append_carrier_branch(part.right_finger_indices, 2);
            if (show_left_hand)
                append_carrier_branch(part.left_finger_indices, 2);
        }
        if (holding_model)
        {
            Matrix4 item_local = identity_matrix();
            std::array<float, 3> primary_tint{1.0F, 1.0F, 1.0F};
            std::array<float, 3> secondary_tint{1.0F, 1.0F, 1.0F};
            const auto rgb = [](int red, int green, int blue) {
                constexpr float scale = 1.0F / 255.0F;
                return std::array<float, 3>{red * scale, green * scale, blue * scale};
            };
            primary_tint =
                item_material_tint(static_cast<InventoryItemId>(player.selected_item_id));
            switch (player.selected_item_id)
            {
            case 13U: // StickInventoryItemClass.Hand: PickAxe model with a hidden tool head.
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.11126215F;
                break;
            case 14U: // TorchInventoryItemClass.Hand
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F)));
                item_local[13] = 0.11126215F;
                break;
            case 23U: case 24U: case 25U: case 26U: case 27U: case 28U:
                // PickInventoryItemClass.Hand
                // XNA's row-vector matrix for quaternion qx*qy composes as Ry*Rx.
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[12] = -0.01F;
                item_local[13] = 0.11126215F;
                break;
            case 29U: case 30U: case 31U: case 32U: case 33U:
                // SpadeInventoryClass.Hand
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.09F;
                break;
            case 34U: case 35U: case 36U: case 37U: case 38U:
                // AxeInventoryClass.Hand
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.11126215F;
                break;
            case 39U: // CompassInventoryItemClass.Hand
            case 40U: // Clock, Locator and Teleporter share the compass grip.
            case 77U:
            case 78U:
                item_local = quaternion_matrix(
                    {0.6469873F, 0.1643085F, 0.7078394F, -0.2310277F});
                item_local[13] = 0.09360941F;
                break;
            case 15U: case 16U: case 17U: case 18U: case 19U:
                // ModelInventoryItemClass.Hand, "Chunk" branch: ore/gem chunks
                // sit smaller and lower in the palm.
                item_local = multiply_matrix(
                    scale_matrix({0.35F, 0.35F, 0.35F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.0719255F;
                switch (player.selected_item_id)
                {
                case 15U: primary_tint = rgb(20, 20, 20); break;
                case 16U: primary_tint = rgb(99, 146, 131); break;
                case 17U: primary_tint = rgb(183, 65, 14); break;
                case 18U: primary_tint = rgb(255, 215, 0); break;
                default: primary_tint = rgb(0, 255, 255); break;
                }
                break;
            case 20U: case 21U: case 22U:
                // Metal bars use the standard palm transform with their tint.
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                primary_tint = player.selected_item_id == 20U   ? rgb(128, 128, 128)
                               : player.selected_item_id == 21U ? rgb(184, 115, 51)
                                                                : rgb(255, 215, 0);
                break;
            case 41U: case 42U: case 43U: case 111U:
            case 44U: case 45U: case 46U: case 47U: case 48U: case 105U: case 109U:
                // ModelInventoryItemClass.Hand (Ammo family) with the tier tints
                // the 2D icons already use.
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                primary_tint = rgb(169, 169, 169);
                secondary_tint = rgb(234, 227, 150);
                switch (player.selected_item_id)
                {
                case 41U: secondary_tint = rgb(181, 166, 66); break;
                case 42U: secondary_tint = rgb(128, 128, 128); break;
                case 43U: secondary_tint = rgb(255, 215, 0); break;
                case 111U: secondary_tint = rgb(0, 255, 255); break;
                case 45U: primary_tint = rgb(128, 128, 128); break;
                case 46U: primary_tint = rgb(255, 215, 0); break;
                case 47U: primary_tint = rgb(0, 255, 255); break;
                case 48U:
                    primary_tint = rgb(139, 0, 0);
                    secondary_tint = rgb(0, 255, 255);
                    break;
                case 109U:
                    primary_tint = rgb(50, 205, 50);
                    secondary_tint = rgb(169, 169, 169);
                    break;
                default: break;
                }
                break;
            case 106U: case 108U:
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                if (player.selected_item_id == 108U)
                    primary_tint = rgb(224, 48, 40);
                break;
            case 51U: case 57U: case 63U: case 69U:
                // Gun base hand transform is identity; the Colt model needs this tiny
                // local correction to sit flush in the ProxyBoy grip.
                item_local = multiply_matrix(
                    multiply_matrix(rotation_y(0.018F), rotation_x(-0.020F)), rotation_z(0.009F));
                item_local[12] = -0.015F;
                break;
            default:
                // Guns, knives and the grenade attach with an identity hand transform.
                break;
            }
            const Matrix4 item_parent = multiply_matrix(item_local, absolute[prop_bone]);
            Matrix4 model_local = identity_matrix();
            if (player.selected_item_id == 39U)
            {
                // CompassEntity rotates its model from the player's horizontal
                // forward vector toward global start (the original uses -WorldPosition).
                const float toward_x = -player.position.x;
                const float toward_z = -player.position.z;
                if (toward_x * toward_x + toward_z * toward_z > 0.000001F)
                {
                    const float target_yaw = std::atan2(toward_x, -toward_z);
                    model_local = rotation_y(player.yaw - target_yaw);
                }
            }
            else if (player.selected_item_id == 77U || player.selected_item_id == 78U)
            {
                // GPSEntity uses the selected GPSItem.PointToLocation for the local
                // player, including the initial zero-vector target. Rotate only the
                // device model under the fixed compass-style hand attachment.
                const float toward_x = player.selected_waypoint.x - player.position.x;
                const float toward_z = player.selected_waypoint.z - player.position.z;
                if (toward_x * toward_x + toward_z * toward_z > 0.000001F)
                {
                    const float target_yaw = std::atan2(toward_x, -toward_z);
                    model_local = rotation_y(player.yaw - target_yaw);
                }
            }
            else if (player.selected_item_id == 40U)
            {
                // ClockEntity uses Quaternion.CreateFromAxisAngle(Vector3.Down,
                // 2*pi*TimeOfDay), rotating the authored dial beneath the hand.
                model_local = rotation_y(-2.0F * pi * player.clock_time_of_day);
            }
            const bool firearm = weapon_family(player.selected_item_id) != WeaponFamily::none ||
                                 player.selected_item_id == 103U ||
                                 player.selected_item_id == 104U;
            const bool lmg = player.selected_item_id >= 119U && player.selected_item_id <= 122U;
            const bool space_firearm =
                player.selected_item_id >= 82U && player.selected_item_id <= 104U;
            const bool edged_tool =
                (player.selected_item_id >= 23U && player.selected_item_id <= 38U) ||
                player.selected_item_id == 49U || player.selected_item_id == 55U ||
                player.selected_item_id == 61U || player.selected_item_id == 67U;
            const bool laser_saber = player.selected_item_id == 81U ||
                                     (player.selected_item_id >= 114U &&
                                      player.selected_item_id <= 118U);
            const bool gray_guided_launcher = player.selected_item_id == 104U ||
                                               player.selected_item_id == 113U;
            std::array<float, 3> torch_tip_view{};
            float torch_tip_screen_y = std::numeric_limits<float>::max();
            for (const auto &part : held_model->parts())
            {
                if (player.selected_item_id == 13U && (part.material_flags & 1U) != 0U)
                    continue;
                const Matrix4 part_world = multiply_matrix(
                    multiply_matrix(part.transform, model_local), item_parent);
                const Matrix4 part_to_view = multiply_matrix(part_world, view);
                const std::array<float, 3> part_tint = gray_guided_launcher
                    ? rgb(128, 128, 128)
                    : ((part.material_flags & 2U) != 0U
                           ? secondary_tint
                           : ((part.material_flags & 1U) != 0U
                                  ? primary_tint
                                  : std::array<float, 3>{1.0F, 1.0F, 1.0F}));
                // XNA used per-pixel BasicEffect lighting for every hand prop,
                // while DNAEffect guns also sampled the world reflection map.
                // Preserve rough handles/paint and restrict the tighter lobe to
                // recolored metal, firearms and the advanced weapon family.
                const float material_response =
                    laser_saber                 ? 0.30F
                    : space_firearm             ? 0.22F
                    : (firearm || lmg)          ? ((part.material_flags & 1U) != 0U ? 0.22F
                                                                                     : 0.14F)
                    : edged_tool                ? ((part.material_flags & 1U) != 0U ? 0.18F
                                                                                     : 0.025F)
                    : ((part.material_flags & 1U) != 0U ? 0.12F : 0.02F);
                for (std::size_t triangle = 0; triangle < part.indices.size(); triangle += 3U)
                {
                    Triangle output;
                    // The prop is sandwiched between palm/sleeve and the digit-only grip layer.
                    // Drawing every hand triangle over it made the back of the hand visibly pass
                    // through picks and torches.
                    output.render_layer = 1;
                    output.texture = part.texture;
                    bool visible = true;
                    bool any_visible = false;
                    for (std::size_t corner = 0; corner < 3U; ++corner)
                    {
                        const auto &source = part.vertices[part.indices[triangle + corner]];
                        const auto projected = screen_position(transform_point(source.position,
                                                                               part_world));
                        if (player.selected_item_id == 14U && projected.visible &&
                            projected.point.y < torch_tip_screen_y)
                        {
                            torch_tip_screen_y = projected.point.y;
                            torch_tip_view = projected.camera;
                        }
                        visible = visible && projected.visible;
                        any_visible = any_visible || projected.visible;
                        output.view[corner] = projected.camera;
                        const std::array<float, 3> camera_normal =
                            transform_direction(source.normal, part_to_view);
                        output.normal_material[corner] =
                            {camera_normal[0], camera_normal[1], -camera_normal[2],
                             material_response};
                        // env_light_direction_ is camera-space. Compare it with
                        // the transformed part normal, not the original model-
                        // space normal, so weapon lighting follows the real sun.
                        const std::array<float, 3> light = environment_light(camera_normal);
                        constexpr float color_scale = 1.0F / 255.0F;
                        output.vertices[corner] = {
                            projected.point,
                            {part.diffuse[0] * source.color[0] * color_scale * part_tint[0] *
                                 light[0],
                             part.diffuse[1] * source.color[1] * color_scale * part_tint[1] *
                                 light[1],
                             part.diffuse[2] * source.color[2] * color_scale * part_tint[2] *
                                 light[2],
                             source.color[3] * color_scale},
                            {wrap_uv(source.uv[0]), wrap_uv(source.uv[1])}};
                        output.depth += projected.depth;
                    }
                    if (depth_pass ? any_visible : visible)
                        triangles.push_back(output);
                }
            }
            if (depth_pass && player.selected_item_id == 14U &&
                torch_tip_screen_y < std::numeric_limits<float>::max())
            {
                // The stock torch pairs its charred mesh with a flame particle.
                // Reconstruct that effect as a camera-facing HDR billboard at
                // the highest posed model vertex so it follows every animation.
                const float flicker = 0.86F + 0.14F *
                    std::sin(elapsed_seconds * 17.0F + std::sin(elapsed_seconds * 7.3F));
                const float pixel_to_view_y =
                    2.0F * torch_tip_view[2] / (height * focal);
                const float pixel_to_view_x =
                    2.0F * torch_tip_view[2] * aspect / (width * focal);
                const float half_width = height * 0.024F * pixel_to_view_x * flicker;
                const float flame_height = height * 0.092F * pixel_to_view_y * flicker;
                const float bottom = torch_tip_view[1] - height * 0.004F * pixel_to_view_y;
                const float top = torch_tip_view[1] + flame_height;
                const float sway = std::sin(elapsed_seconds * 11.0F) * half_width * 0.22F;
                const float flame_depth = std::max(0.0105F, torch_tip_view[2] - 0.0020F);
                const std::array<std::array<float, 3>, 4> corners{{
                    {torch_tip_view[0] - half_width, bottom, flame_depth},
                    {torch_tip_view[0] + half_width, bottom, flame_depth},
                    {torch_tip_view[0] + half_width * 0.72F + sway, top, flame_depth},
                    {torch_tip_view[0] - half_width * 0.72F + sway, top, flame_depth},
                }};
                constexpr std::array<SDL_FPoint, 4> uv{{
                    {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}}};
                constexpr std::array<std::size_t, 6> order{0U, 1U, 2U, 0U, 2U, 3U};
                for (std::size_t half = 0; half < 2U; ++half)
                {
                    Triangle flame;
                    flame.render_layer = 3;
                    for (std::size_t corner = 0; corner < 3U; ++corner)
                    {
                        const std::size_t index = order[half * 3U + corner];
                        flame.view[corner] = corners[index];
                        flame.vertices[corner] = {{}, {1.0F, 1.0F, 1.0F, 1.0F}, uv[index]};
                        flame.normal_material[corner] = {0.0F, 0.0F, 1.0F, -2.0F};
                        flame.depth += corners[index][2];
                    }
                    triangles.push_back(flame);
                }
            }
        }
        if (holding_block && block_atlas != nullptr)
        {
            // BlockInventoryItemClass(ItemUse.Hand), recovered from the
            // original executable: S(.1) * Q * T(0,.06533548,0), parented to
            // PropRight. Put the cube in the same painter list as the fingers
            // so near finger triangles naturally wrap over its faces.
            // BlockEntity applies its .1 CubeScale in the terrain shader rather
            // than in the entity matrix. SDL's direct vertex path has no copy
            // of that shader's packed-cube expansion, so the equivalent visual
            // factor against the exported carrier is .075. The small screen
            // calibration is measured against the stock 1280x720 GunEyePoint
            // pass and is height-relative, so it remains stable at 4K/ultrawide.
            Matrix4 item_local = multiply_matrix(
                scale_matrix({0.075F, 0.075F, 0.075F}),
                quaternion_matrix({-0.02067951F, -0.007977718F,
                                   0.03257636F, 0.9992235F}));
            // BlockEntity's packed-cube expansion centers the cube on the
            // attachment point; the corner-origin export sat it half a cube
            // behind the palm ("at the back of the hand"). Center it and give
            // it the original's small lift so the fingers wrap the front face.
            item_local[12] = -0.0375F;
            item_local[13] = 0.06533548F - 0.0375F;
            item_local[14] = -0.0375F;
            const Matrix4 item_world = multiply_matrix(item_local, absolute[prop_bone]);
            // BlockVertex::_faceVertices is a 0..1 cube; its origin is the
            // PropRight attachment point rather than its geometric center.
            constexpr std::array<std::array<float, 3>, 8> local_corners{{
                {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F},
                {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 1.0F},
            }};
            std::array<ScreenProjection, 8> cube{};
            for (std::size_t corner = 0; corner < cube.size(); ++corner)
            {
                cube[corner] = screen_position(transform_point(local_corners[corner], item_world));
                cube[corner].point.x -= height * 0.022F;
                cube[corner].point.y += height * 0.021F;
                // The same height-relative screen calibration expressed in view space so the
                // depth-tested path lands on identical pixels after projection.
                const float cube_depth = cube[corner].camera[2];
                cube[corner].camera[0] -=
                    height * 0.022F * 2.0F * cube_depth * aspect / (width * focal);
                cube[corner].camera[1] -= 0.042F * cube_depth / focal;
            }
            struct BlockFace final
            {
                std::array<std::size_t, 4> corners{};
                world::FaceDirection direction{};
                float light{};
            };
            constexpr std::array<BlockFace, 6> faces{{
                {{{1U, 2U, 6U, 5U}}, world::FaceDirection::positive_x, 0.76F},
                {{{0U, 4U, 7U, 3U}}, world::FaceDirection::negative_x, 0.70F},
                {{{3U, 7U, 6U, 2U}}, world::FaceDirection::positive_y, 1.00F},
                {{{0U, 1U, 5U, 4U}}, world::FaceDirection::negative_y, 0.58F},
                {{{4U, 5U, 6U, 7U}}, world::FaceDirection::positive_z, 0.86F},
                {{{0U, 3U, 2U, 1U}}, world::FaceDirection::negative_z, 0.82F},
            }};
            constexpr std::array<std::size_t, 6> order{0U, 1U, 2U, 0U, 2U, 3U};
            for (const auto &face : faces)
            {
                // The depth-tested pass draws every face and lets the depth buffer discard
                // the interior ones; the painter must cull back faces itself.
                if (!depth_pass)
                {
                    if (!cube[face.corners[0]].visible || !cube[face.corners[1]].visible ||
                        !cube[face.corners[2]].visible || !cube[face.corners[3]].visible)
                        continue;
                    const SDL_FPoint a = cube[face.corners[0]].point;
                    const SDL_FPoint b = cube[face.corners[1]].point;
                    const SDL_FPoint c = cube[face.corners[2]].point;
                    const float signed_area = (b.x - a.x) * (c.y - a.y) -
                                              (b.y - a.y) * (c.x - a.x);
                    if (signed_area >= -0.001F)
                        continue;
                }
                const auto tile = world::atlas_tile(player.selected_block, face.direction);
                const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
                const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
                const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
                const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
                const auto uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
                for (std::size_t half = 0; half < 2U; ++half)
                {
                    Triangle output;
                    // Like the stock BlockInventoryItem renderer, resolve the cube and fingers
                    // together so the palm can sit behind it while the near fingers wrap its edge.
                    output.render_layer = 1;
                    output.texture = block_atlas;
                    for (std::size_t vertex = 0; vertex < 3U; ++vertex)
                    {
                        const std::size_t face_corner = order[half * 3U + vertex];
                        const auto &projected = cube[face.corners[face_corner]];
                        // The held block obeys the same environment light as
                        // the hands so it darkens at night and in caves.
                        const float env =
                            std::min(1.0F, (env_ambient_[0] + env_ambient_[1] +
                                            env_ambient_[2]) / 3.0F +
                                               (env_directional_[0] + env_directional_[1] +
                                                env_directional_[2]) / 6.0F);
                        output.vertices[vertex] = {
                            projected.point,
                            {face.light * env, face.light * env, face.light * env, 1.0F},
                            {uv[face_corner][0], uv[face_corner][1]}};
                        output.view[vertex] = projected.camera;
                        output.depth += projected.depth;
                    }
                    triangles.push_back(output);
                }
            }
        }
#if defined(_WIN32)
        if (depth_pass)
        {
            if (render_depth_pass(triangles, metrics.output_width, metrics.output_height, focal,
                                  aspect, idle))
                return;
            // The device rejected the frame; future frames fall back to the painter.
        }
#endif
        std::stable_sort(
            triangles.begin(), triangles.end(),
            [](const Triangle &left, const Triangle &right) {
                if (left.render_layer != right.render_layer)
                    return left.render_layer < right.render_layer;
                return left.depth > right.depth;
            });
        std::vector<SDL_Vertex> run;
        run.reserve(triangles.size() * 3U);
        SDL_Texture *run_texture{};
        bool run_started{};
        const auto flush_run = [&]() {
            if (!run.empty())
                SDL_RenderGeometry(renderer_, run_texture, run.data(),
                                   static_cast<int>(run.size()), nullptr, 0);
            run.clear();
        };
        for (const auto &triangle : triangles)
        {
            if (run_started && triangle.texture != run_texture)
                flush_run();
            run_texture = triangle.texture;
            run_started = true;
            run.insert(run.end(), triangle.vertices.begin(), triangle.vertices.end());
        }
        flush_run();
    }

    void append_world_player(EntityBatch &output, std::uint8_t gid,
                             InventoryItemId carried_item, world::Vec3 feet, float yaw,
                             float torso_pitch, float animation_seconds,
                             float movement_x, float movement_y,
                             PlayerAnimationMode animation_mode, bool using_tool,
                             bool dead, bool shouldering, bool reloading,
                             bool throwing_grenade, bool ready_to_throw_grenade,
                             float sky_light, float torch_light, SDL_Texture *block_atlas)
    {
        if (!loaded_ || batches_.empty() || inverse_bind_pose_.size() != 71U ||
            source_pose_local_.size() != 71U)
            return;

        const auto clip_or = [&](std::string_view name, const NativeAnimation *fallback) {
            const auto found = animation_library_.find(std::string{name});
            return found == animation_library_.end() || found->second.frames.empty()
                       ? fallback
                       : &found->second;
        };
        const float forward_amount = std::abs(movement_y);
        const float strafe_amount = std::abs(movement_x);
        const float movement_amount = std::max(forward_amount, strafe_amount);
        const bool moving = movement_amount >= 0.1F;
        const bool running = movement_amount >= 0.8F;
        const NativeAnimation *stand_animation = clip_or("Stand0", &generic_idle_);
        const NativeAnimation *base_animation = stand_animation;
        if (moving)
            base_animation = clip_or(running ? "Run" : "Walk", stand_animation);

        const NativeAnimation *item_idle = &generic_idle_;
        const NativeAnimation *item_walk = clip_or("GenericWalk", item_idle);
        const NativeAnimation *action_animation = &generic_use_;
        const NativeAnimation *shoulder_animation = item_idle;
        const NativeAnimation *shoulder_idle_animation = item_idle;
        const NativeAnimation *reload_animation = item_idle;
        int item_root = 16;
        int action_root = 5;

        const WeaponFamily family = weapon_family(animation_mode);
        if (family != WeaponFamily::none)
        {
            const WeaponClipNames names = weapon_clip_names(family);
            item_root = 5;
            item_idle = clip_or(names.idle, item_idle);
            item_walk = clip_or(names.walk, item_idle);
            shoulder_animation = clip_or(names.shoulder, item_idle);
            shoulder_idle_animation = clip_or(names.shoulder_idle, item_idle);
            reload_animation = clip_or(names.reload, item_idle);
            action_animation = clip_or(shouldering ? names.shoulder_use : names.use,
                                       action_animation);
        }
        else
        {
            switch (animation_mode)
            {
            case PlayerAnimationMode::pick:
                item_idle = clip_or("FPSIdle", &generic_idle_);
                item_walk = clip_or("PickWalk", clip_or("FPSWalk", item_idle));
                action_animation = clip_or("FPSPick", &generic_use_);
                action_root = 16;
                break;
            case PlayerAnimationMode::block:
                item_idle = clip_or("BlockHoldIdle", &block_idle_);
                item_walk = clip_or("BlockWalk", clip_or("BlockHoldWalk", item_idle));
                action_animation = clip_or("BlockUse", &block_use_);
                action_root = 16;
                break;
            case PlayerAnimationMode::grenade:
                item_idle = clip_or("GrenadeIdle", &generic_idle_);
                item_walk = clip_or("GrenadeWalk", item_idle);
                if (throwing_grenade)
                    action_animation = clip_or(ready_to_throw_grenade ? "GrenadeCook"
                                                                      : "GrenadeThrow",
                                               &generic_use_);
                break;
            case PlayerAnimationMode::fist:
                item_root = 5;
                item_idle = clip_or("FPSIdle", &generic_idle_);
                item_walk = clip_or("FPSWalk", item_idle);
                action_animation = clip_or("FPSPick", &generic_use_);
                break;
            default:
                break;
            }
        }

        if (base_animation == nullptr || base_animation->frames.empty())
            return;

        // Keep one six-channel blender per network GID. Player.UpdateAnimation in
        // the original is stateful: locomotion, torso tilt, item stance, action,
        // idle head and death are independent layers with their own clocks and
        // cross-fades. Sampling every layer from global time made swings loop,
        // aim/reload snap and every remote avatar move in lockstep.
        WorldPlayerAnimationState &animation = world_animation_states_[gid];
        const bool identity_changed = !animation.initialized ||
                                      animation.animation_mode != animation_mode ||
                                      animation.carried_item != carried_item;
        if (identity_changed)
        {
            animation = {};
            animation.initialized = true;
            animation.animation_mode = animation_mode;
            animation.carried_item = carried_item;
            animation.head_seconds = static_cast<float>(gid) * 0.371F;
        }
        const float delta_seconds =
            animation.last_animation_seconds < 0.0F ||
                    animation_seconds < animation.last_animation_seconds
                ? 0.0F
                : std::clamp(animation_seconds - animation.last_animation_seconds, 0.0F, 0.25F);
        animation.last_animation_seconds = animation_seconds;
        animation.last_seen_seconds = animation_seconds;
        animation.head_seconds += delta_seconds;
        for (AnimationChannel *channel : {&animation.locomotion, &animation.tilt,
                                          &animation.item, &animation.action,
                                          &animation.head, &animation.death})
            channel->advance(delta_seconds);

        float locomotion_speed = 1.0F;
        if (moving)
        {
            locomotion_speed =
                forward_amount > strafe_amount
                    ? (forward_amount < 0.8F ? (forward_amount - 0.1F) / 0.4F
                                             : 0.8F + (forward_amount - 0.8F))
                    : strafe_amount;
        }
        const bool locomotion_reversed = moving && movement_y < 0.0F;
        if (!animation.locomotion.has_current ||
            animation.locomotion.current.clip != base_animation)
        {
            ChannelPlayer next{};
            next.clip = base_animation;
            next.speed = locomotion_reversed ? -locomotion_speed : locomotion_speed;
            next.time = locomotion_reversed ? base_animation->duration : 0.0F;
            animation.locomotion.play(next, identity_changed ? 0.0F : 0.25F);
        }
        else
        {
            animation.locomotion.current.speed =
                locomotion_reversed ? -locomotion_speed : locomotion_speed;
        }

        const NativeAnimation *tilt_animation = clip_or("Tilt", nullptr);
        if (tilt_animation != nullptr)
        {
            if (!animation.tilt.has_current || animation.tilt.current.clip != tilt_animation)
            {
                ChannelPlayer next{};
                next.clip = tilt_animation;
                next.influence_root = 1; // BackLower
                next.excluded_roots = {16, 12}; // keep both collar/arm branches item-authored
                next.looping = false;
                next.time_driven = false;
                animation.tilt.play(next, 0.0F);
            }
            const float pitch_degrees =
                torso_pitch * 180.0F / std::numbers::pi_v<float>;
            animation.tilt.current.time =
                std::clamp((pitch_degrees + 90.0F) / 180.0F, 0.0F, 1.0F) *
                tilt_animation->duration;
        }

        const NativeAnimation *desired_item = moving ? item_walk : item_idle;
        bool desired_item_looping = true;
        float desired_item_speed = moving ? movement_amount : 1.0F;
        float desired_item_blend = 0.25F;
        float desired_item_start = 0.0F;
        bool restart_item = false;
        if (family == WeaponFamily::none)
        {
            animation.shoulder_phase = ShoulderAnimationPhase::unshouldered;
        }
        else if (reloading)
        {
            // Player.UpdateAnimation lowers an aimed gun before starting its
            // reload clip. Preserve that visible transition for remote players
            // instead of snapping directly from the shoulder to the magazine.
            if (animation.shoulder_phase == ShoulderAnimationPhase::shouldered ||
                animation.shoulder_phase == ShoulderAnimationPhase::shouldering)
            {
                animation.shoulder_phase = ShoulderAnimationPhase::unshouldering;
                restart_item = true;
            }
            if (animation.shoulder_phase == ShoulderAnimationPhase::unshouldering &&
                animation.item.has_current && animation.item.current.finished())
                animation.shoulder_phase = ShoulderAnimationPhase::unshouldered;
            if (animation.shoulder_phase == ShoulderAnimationPhase::unshouldering)
            {
                desired_item = shoulder_animation;
                desired_item_looping = false;
                desired_item_speed = -1.0F;
                desired_item_start = shoulder_animation->duration;
                desired_item_blend = 0.25F;
            }
            else
            {
                desired_item = reload_animation;
                desired_item_looping = false;
                desired_item_blend = 0.25F;
                restart_item = !animation.reloading;
            }
        }
        else if (shouldering)
        {
            if (animation.shoulder_phase == ShoulderAnimationPhase::unshouldered ||
                animation.shoulder_phase == ShoulderAnimationPhase::unshouldering)
            {
                animation.shoulder_phase = ShoulderAnimationPhase::shouldering;
                restart_item = true;
            }
            if (animation.shoulder_phase == ShoulderAnimationPhase::shouldering &&
                animation.item.has_current && animation.item.current.finished())
                animation.shoulder_phase = ShoulderAnimationPhase::shouldered;
            if (animation.shoulder_phase == ShoulderAnimationPhase::shouldered)
            {
                // Stock Player.UpdateAnimation intentionally uses the shouldered
                // idle registration for movement too; the lower-body channel still walks.
                desired_item = shoulder_idle_animation;
                desired_item_looping = true;
                desired_item_blend = 0.1F;
            }
            else
            {
                desired_item = shoulder_animation;
                desired_item_looping = false;
                desired_item_blend = 0.0F;
            }
        }
        else
        {
            if (animation.shoulder_phase == ShoulderAnimationPhase::shouldered ||
                animation.shoulder_phase == ShoulderAnimationPhase::shouldering)
            {
                animation.shoulder_phase = ShoulderAnimationPhase::unshouldering;
                restart_item = true;
            }
            if (animation.shoulder_phase == ShoulderAnimationPhase::unshouldering)
            {
                if (animation.item.has_current && animation.item.current.finished())
                    animation.shoulder_phase = ShoulderAnimationPhase::unshouldered;
                else
                {
                    desired_item = shoulder_animation;
                    desired_item_looping = false;
                    desired_item_speed = -1.0F;
                    desired_item_start = shoulder_animation->duration;
                    desired_item_blend = 0.0F;
                }
            }
        }
        if (restart_item || !animation.item.has_current ||
            animation.item.current.clip != desired_item ||
            animation.item.current.looping != desired_item_looping)
        {
            ChannelPlayer next{};
            next.clip = desired_item;
            next.influence_root = item_root;
            next.looping = desired_item_looping;
            next.speed = desired_item_speed;
            next.time = desired_item_start;
            animation.item.play(next, identity_changed ? 0.0F : desired_item_blend);
        }
        else
        {
            animation.item.current.influence_root = item_root;
            animation.item.current.speed = desired_item_speed;
        }
        animation.reloading = reloading;

        const bool action_active = using_tool || throwing_grenade;
        if (action_active)
        {
            const bool restart_action = !animation.action_active ||
                                        !animation.action.has_current ||
                                        animation.action.current.clip != action_animation ||
                                        animation.action.current.finished();
            if (restart_action)
            {
                ChannelPlayer next{};
                next.clip = action_animation;
                next.influence_root = action_root;
                next.looping = false;
                animation.action.play(next, 0.0F);
            }
        }
        else if (animation.action.has_current && animation.action.current.finished())
        {
            animation.action.clear(0.25F);
        }
        animation.action_active = action_active;

        const NativeAnimation *head_animation = clip_or("MaleIdleLookAround", nullptr);
        if (head_animation != nullptr)
        {
            if (!animation.head.has_current || animation.head.current.clip != head_animation)
            {
                ChannelPlayer next{};
                next.clip = head_animation;
                next.influence_root = 14; // Neck
                next.looping = false;
                next.time_driven = false;
                animation.head.play(next, 0.25F);
            }
            const float cycle = std::max(0.0001F, head_animation->duration * 2.0F);
            const float phase = std::fmod(animation.head_seconds, cycle);
            animation.head.current.time = phase <= head_animation->duration
                                              ? phase
                                              : cycle - phase;
        }

        if (dead && !animation.dead)
        {
            const NativeAnimation *death_animation = clip_or("Faint", stand_animation);
            ChannelPlayer next{};
            next.clip = death_animation;
            next.looping = false;
            animation.death.play(next, 0.25F);
        }
        else if (!dead && animation.dead)
        {
            animation.death.clear(0.25F);
        }
        animation.dead = dead;

        std::array<BoneTransform, 71> pose{};
        animation.locomotion.apply(pose);
        animation.tilt.apply(pose);
        animation.item.apply(pose);
        animation.action.apply(pose);
        animation.head.apply(pose);
        animation.death.apply(pose);

        std::array<Matrix4, 71> absolute{};
        std::array<Matrix4, 71> skin{};
        Matrix4 mirror = identity_matrix();
        mirror[10] = -1.0F;
        constexpr float pi = std::numbers::pi_v<float>;
        for (std::size_t bone = 0U; bone < absolute.size(); ++bone)
        {
            Matrix4 animated = compose_matrix(pose[bone]);
            animated = bone == 0U
                           ? multiply_matrix(animated, rotation_y(pi))
                           : multiply_matrix(multiply_matrix(mirror, animated), mirror);

            // OpenClassicAvatarMod.RetargetAnimatedLocal: keep the current
            // animation's rotation while preserving the exported avatar's own
            // body proportions and joint offsets.
            Matrix4 local = animated;
            const Matrix4 &source = source_pose_local_[bone];
            for (std::size_t row = 0U; row < 3U; ++row)
            {
                const float animated_length = std::sqrt(
                    animated[row * 4U] * animated[row * 4U] +
                    animated[row * 4U + 1U] * animated[row * 4U + 1U] +
                    animated[row * 4U + 2U] * animated[row * 4U + 2U]);
                const float source_length = std::sqrt(
                    source[row * 4U] * source[row * 4U] +
                    source[row * 4U + 1U] * source[row * 4U + 1U] +
                    source[row * 4U + 2U] * source[row * 4U + 2U]);
                const float scale = source_length / std::max(0.000001F, animated_length);
                for (std::size_t column = 0U; column < 3U; ++column)
                    local[row * 4U + column] *= scale;
            }
            local[12] = source[12];
            local[13] = source[13];
            local[14] = source[14];
            local[15] = 1.0F;

            const int parent = avatar_parents[bone];
            absolute[bone] = parent >= 0 && parent < static_cast<int>(bone)
                                 ? multiply_matrix(local, absolute[static_cast<std::size_t>(parent)])
                                 : local;
            skin[bone] = multiply_matrix(inverse_bind_pose_[bone], absolute[bone]);
        }

        // Xbox avatar exports are authored facing +X, while CMZ network yaw
        // and the playable camera use -Z as forward. Rotate that basis once so
        // a remote avatar's chest, hands, and movement direction agree with the
        // decoded PlayerUpdate yaw.
        const float model_yaw = yaw + std::numbers::pi_v<float> * 0.5F;
        const float cosine = std::cos(model_yaw);
        const float sine = std::sin(model_yaw);
        const auto to_world = [&](std::array<float, 3> point) {
            // ImportedAvatarModelEntity.RenderWorld starts with Scale(1,1,-1).
            point[2] = -point[2];
            return world::Vec3{feet.x + point[0] * cosine + point[2] * sine,
                               feet.y + point[1],
                               feet.z - point[0] * sine + point[2] * cosine};
        };
        const auto normal_to_world = [&](std::array<float, 3> normal) {
            normal[2] = -normal[2];
            const std::array<float, 3> result{
                normal[0] * cosine + normal[2] * sine, normal[1],
                -normal[0] * sine + normal[2] * cosine};
            const float length = std::sqrt(result[0] * result[0] + result[1] * result[1] +
                                           result[2] * result[2]);
            if (length <= 0.000001F)
                return std::array<float, 3>{0.0F, 1.0F, 0.0F};
            return std::array<float, 3>{result[0] / length, result[1] / length,
                                        result[2] / length};
        };
        const auto skin_position = [&](const SourceVertex &source) {
            std::array<float, 3> result{};
            float total{};
            for (std::size_t influence = 0U; influence < source.bindings.size(); ++influence)
            {
                const float weight = static_cast<float>(source.weights[influence]) / 255.0F;
                if (weight <= 0.0F || source.bindings[influence] >= skin.size())
                    continue;
                const auto point = transform_point(source.position, skin[source.bindings[influence]]);
                for (std::size_t component = 0U; component < 3U; ++component)
                    result[component] += point[component] * weight;
                total += weight;
            }
            if (total <= 0.0001F)
                return source.position;
            if (std::abs(total - 1.0F) > 0.0001F)
                for (float &component : result)
                    component /= total;
            return result;
        };
        const auto skin_normal = [&](const SourceVertex &source) {
            std::array<float, 3> result{};
            float total{};
            for (std::size_t influence = 0U; influence < source.bindings.size(); ++influence)
            {
                const float weight = static_cast<float>(source.weights[influence]) / 255.0F;
                if (weight <= 0.0F || source.bindings[influence] >= skin.size())
                    continue;
                const auto normal = transform_direction(source.normal, skin[source.bindings[influence]]);
                for (std::size_t component = 0U; component < 3U; ++component)
                    result[component] += normal[component] * weight;
                total += weight;
            }
            return total > 0.0001F ? result : source.normal;
        };

        const bool authored_death_pose = dead && animation.death.has_current &&
                                         animation.death.current.clip != stand_animation;
        const float lowered = dead && !authored_death_pose ? -0.72F : 0.0F;
        feet.y += lowered;
        for (const AvatarBatch &avatar_batch : batches_)
        {
            // Neutral face frames are native CMZ's default expression. Rendering
            // every expression layer at once produces the old white-face artifact.
            if (avatar_batch.indices.empty() ||
                (avatar_batch.face_texture_usage >= 0 && avatar_batch.face_frame != 0))
                continue;
            EntitySpan span;
            span.texture = avatar_batch.texture != nullptr ? avatar_batch.texture : white_texture_;
            span.first = static_cast<std::uint32_t>(output.vertices.size());
            span.blend = avatar_batch.material_overlay ||
                                 avatar_batch.face_texture_usage >= 0
                             ? EntityBlend::alpha_lit
                             : EntityBlend::opaque;
            const bool base_body = avatar_batch.name.starts_with("00000002-") &&
                                   avatar_batch.name.find(":body:") != std::string::npos;
            for (const std::uint16_t index : avatar_batch.indices)
            {
                const SourceVertex &source = avatar_batch.vertices[index];
                const world::Vec3 position = to_world(skin_position(source));
                const auto normal = normal_to_world(skin_normal(source));
                const float vertex_red = base_body ? 1.0F : source.color[0] / 255.0F;
                const float vertex_green = base_body ? 1.0F : source.color[1] / 255.0F;
                const float vertex_blue = base_body ? 1.0F : source.color[2] / 255.0F;
                const float vertex_alpha = base_body ? 1.0F : source.color[3] / 255.0F;
                EntityVertex vertex;
                vertex.x = position.x;
                vertex.y = position.y;
                vertex.z = position.z;
                vertex.u = source.uv[0] - std::floor(source.uv[0]);
                vertex.v = source.uv[1] - std::floor(source.uv[1]);
                vertex.red = avatar_batch.diffuse[0] * vertex_red;
                vertex.green = avatar_batch.diffuse[1] * vertex_green;
                vertex.blue = avatar_batch.diffuse[2] * vertex_blue;
                vertex.alpha = vertex_alpha;
                vertex.normal_x = normal[0];
                vertex.normal_y = normal[1];
                vertex.normal_z = normal[2];
                vertex.sky_light = std::clamp(sky_light, 0.0F, 1.0F);
                vertex.torch_light = std::clamp(torch_light, 0.0F, 1.5F);
                output.vertices.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(output.vertices.size()) - span.first;
            if (span.count > 0U)
                output.spans.push_back(span);
        }

        const std::uint16_t carried_value = static_cast<std::uint16_t>(carried_item);
        // OpenClassicAvatarMod.ApplyThirdPersonItemAnchors does not leave the
        // proxy child at raw PropRight. It moves that anchor toward the average
        // of the five right-finger bases, capped at 0.22 m, so imported hand
        // proportions still wrap the stock item models correctly.
        Matrix4 prop_anchor = absolute[48U];
        constexpr std::array<std::size_t, 5> right_grip_bones{44U, 45U, 46U, 47U, 50U};
        std::array<float, 3> grip_center{};
        for (const std::size_t bone : right_grip_bones)
            for (std::size_t component = 0U; component < grip_center.size(); ++component)
                grip_center[component] += absolute[bone][12U + component] /
                                          static_cast<float>(right_grip_bones.size());
        std::array<float, 3> grip_delta{
            grip_center[0] - prop_anchor[12],
            grip_center[1] - prop_anchor[13],
            grip_center[2] - prop_anchor[14],
        };
        const float grip_distance = std::sqrt(grip_delta[0] * grip_delta[0] +
                                              grip_delta[1] * grip_delta[1] +
                                              grip_delta[2] * grip_delta[2]);
        if (grip_distance > 0.22F)
            for (float &component : grip_delta)
                component *= 0.22F / grip_distance;
        prop_anchor[12] += grip_delta[0];
        prop_anchor[13] += grip_delta[1];
        prop_anchor[14] += grip_delta[2];
        StaticModelAsset *held_model{};
        switch (carried_value)
        {
        case 13U: held_model = pickaxe_.get(); break;
        case 14U: held_model = torch_.get(); break;
        case 15U: case 16U: case 17U: case 18U: held_model = ore_.get(); break;
        case 19U: held_model = gems_.get(); break;
        case 20U: case 21U: case 22U: held_model = bars_.get(); break;
        case 23U: case 24U: case 25U: case 26U: case 27U: case 28U:
            held_model = pickaxe_.get(); break;
        case 29U: case 30U: case 31U: case 32U: case 33U:
            held_model = spade_.get(); break;
        case 34U: case 35U: case 36U: case 37U: case 38U:
            held_model = axe_.get(); break;
        case 39U: held_model = compass_.get(); break;
        case 40U: held_model = clock_.get(); break;
        case 41U: case 42U: case 43U: case 44U: case 45U: case 46U:
        case 47U: case 48U: case 105U: case 109U: case 111U:
            held_model = ammo_.get(); break;
        case 49U: case 55U: case 61U: case 67U: held_model = knife_.get(); break;
        case 50U: case 56U: case 62U: case 68U: held_model = ak_.get(); break;
        case 51U: case 57U: case 63U: case 69U: held_model = pistol_.get(); break;
        case 52U: case 58U: case 64U: case 70U: held_model = shotgun_.get(); break;
        case 53U: case 59U: case 65U: case 71U: held_model = rifle_.get(); break;
        case 54U: case 60U: case 66U: case 72U: held_model = smg_.get(); break;
        case 77U: held_model = locator_.get(); break;
        case 78U: held_model = teleporter_.get(); break;
        case 81U: case 114U: case 115U: case 116U: case 117U: case 118U:
            held_model = saber_.get(); break;
        case 82U: case 87U: case 92U: case 97U: held_model = space_ak_.get(); break;
        case 83U: case 88U: case 93U: case 98U: held_model = space_pistol_.get(); break;
        case 84U: case 89U: case 94U: case 99U: held_model = space_shotgun_.get(); break;
        case 85U: case 90U: case 95U: case 100U: held_model = space_rifle_.get(); break;
        case 86U: case 91U: case 96U: case 101U: held_model = space_smg_.get(); break;
        case 103U: case 104U: case 112U: case 113U: held_model = rpg_.get(); break;
        case 106U: case 108U: held_model = gunpowder_.get(); break;
        case 110U: held_model = grenade_.get(); break;
        case 119U: case 120U: case 121U: case 122U: held_model = lmg_.get(); break;
        default: break;
        }

        if (held_model != nullptr && held_model->loaded())
        {
            Matrix4 item_local = identity_matrix();
            std::array<float, 3> primary_tint = item_material_tint(carried_item);
            std::array<float, 3> secondary_tint{1.0F, 1.0F, 1.0F};
            const auto rgb = [](int red, int green, int blue) {
                constexpr float scale = 1.0F / 255.0F;
                return std::array<float, 3>{red * scale, green * scale, blue * scale};
            };
            switch (carried_value)
            {
            case 13U:
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.11126215F;
                break;
            case 14U:
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F)));
                item_local[13] = 0.11126215F;
                break;
            case 23U: case 24U: case 25U: case 26U: case 27U: case 28U:
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[12] = -0.01F;
                item_local[13] = 0.11126215F;
                break;
            case 29U: case 30U: case 31U: case 32U: case 33U:
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.09F;
                break;
            case 34U: case 35U: case 36U: case 37U: case 38U:
                item_local = multiply_matrix(rotation_y(pi * 0.25F), rotation_x(pi * 0.5F));
                item_local[13] = 0.11126215F;
                break;
            case 39U: case 40U: case 77U: case 78U:
                item_local = quaternion_matrix(
                    {0.6469873F, 0.1643085F, 0.7078394F, -0.2310277F});
                item_local[13] = 0.09360941F;
                break;
            case 15U: case 16U: case 17U: case 18U: case 19U:
                item_local = multiply_matrix(
                    scale_matrix({0.35F, 0.35F, 0.35F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.0719255F;
                switch (carried_value)
                {
                case 15U: primary_tint = rgb(20, 20, 20); break;
                case 16U: primary_tint = rgb(99, 146, 131); break;
                case 17U: primary_tint = rgb(183, 65, 14); break;
                case 18U: primary_tint = rgb(255, 215, 0); break;
                default: primary_tint = rgb(0, 255, 255); break;
                }
                break;
            case 20U: case 21U: case 22U:
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                primary_tint = carried_value == 20U   ? rgb(128, 128, 128)
                               : carried_value == 21U ? rgb(184, 115, 51)
                                                      : rgb(255, 215, 0);
                break;
            case 41U: case 42U: case 43U: case 44U: case 45U: case 46U:
            case 47U: case 48U: case 105U: case 109U: case 111U:
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                primary_tint = rgb(169, 169, 169);
                secondary_tint = rgb(234, 227, 150);
                switch (carried_value)
                {
                case 41U: secondary_tint = rgb(181, 166, 66); break;
                case 42U: secondary_tint = rgb(128, 128, 128); break;
                case 43U: secondary_tint = rgb(255, 215, 0); break;
                case 111U: secondary_tint = rgb(0, 255, 255); break;
                case 45U: primary_tint = rgb(128, 128, 128); break;
                case 46U: primary_tint = rgb(255, 215, 0); break;
                case 47U: primary_tint = rgb(0, 255, 255); break;
                case 48U:
                    primary_tint = rgb(139, 0, 0);
                    secondary_tint = rgb(0, 255, 255);
                    break;
                case 109U:
                    primary_tint = rgb(50, 205, 50);
                    secondary_tint = rgb(169, 169, 169);
                    break;
                default: break;
                }
                break;
            case 106U: case 108U:
                item_local = multiply_matrix(
                    scale_matrix({0.5F, 0.5F, 0.5F}),
                    quaternion_matrix({0.4816553F, 0.05900274F, 0.8705468F, -0.08170173F}));
                item_local[13] = 0.1119255F;
                if (carried_value == 108U)
                    primary_tint = rgb(224, 48, 40);
                break;
            case 51U: case 57U: case 63U: case 69U:
                item_local = multiply_matrix(
                    multiply_matrix(rotation_y(0.018F), rotation_x(-0.020F)),
                    rotation_z(0.009F));
                item_local[12] = -0.015F;
                break;
            default: break;
            }

            const Matrix4 item_parent = multiply_matrix(item_local, prop_anchor);
            const bool gray_guided_launcher = carried_value == 104U || carried_value == 113U;
            for (const StaticModelPart &part : held_model->parts())
            {
                if (carried_value == 13U && (part.material_flags & 1U) != 0U)
                    continue;
                const Matrix4 part_world = multiply_matrix(part.transform, item_parent);
                const std::array<float, 3> part_tint =
                    gray_guided_launcher
                        ? rgb(128, 128, 128)
                        : ((part.material_flags & 2U) != 0U
                               ? secondary_tint
                               : ((part.material_flags & 1U) != 0U
                                      ? primary_tint
                                      : std::array<float, 3>{1.0F, 1.0F, 1.0F}));
                EntitySpan span;
                span.texture = part.texture != nullptr ? part.texture : white_texture_;
                span.first = static_cast<std::uint32_t>(output.vertices.size());
                constexpr float color_scale = 1.0F / 255.0F;
                for (const std::uint32_t index : part.indices)
                {
                    if (index >= part.vertices.size())
                        continue;
                    const StaticModelVertex &source = part.vertices[index];
                    const world::Vec3 position =
                        to_world(transform_point(source.position, part_world));
                    const auto normal = normal_to_world(
                        transform_direction(source.normal, part_world));
                    EntityVertex vertex;
                    vertex.x = position.x;
                    vertex.y = position.y;
                    vertex.z = position.z;
                    vertex.u = source.uv[0] - std::floor(source.uv[0]);
                    vertex.v = source.uv[1] - std::floor(source.uv[1]);
                    vertex.red = part.diffuse[0] * source.color[0] * color_scale * part_tint[0];
                    vertex.green = part.diffuse[1] * source.color[1] * color_scale * part_tint[1];
                    vertex.blue = part.diffuse[2] * source.color[2] * color_scale * part_tint[2];
                    vertex.alpha = source.color[3] * color_scale;
                    vertex.normal_x = normal[0];
                    vertex.normal_y = normal[1];
                    vertex.normal_z = normal[2];
                    vertex.sky_light = std::clamp(sky_light, 0.0F, 1.0F);
                    vertex.torch_light = std::clamp(torch_light, 0.0F, 1.5F);
                    output.vertices.push_back(vertex);
                }
                span.count = static_cast<std::uint32_t>(output.vertices.size()) - span.first;
                if (span.count > 0U)
                    output.spans.push_back(span);
            }
        }

        const world::BlockType held_block = item_block(carried_item);
        if (held_block != world::BlockType::empty && carried_value != 14U &&
            block_atlas != nullptr)
        {
            Matrix4 block_local = multiply_matrix(
                scale_matrix({0.1F, 0.1F, 0.1F}),
                quaternion_matrix({-0.02067951F, -0.007977718F,
                                   0.03257636F, 0.9992235F}));
            block_local[12] = -0.05F;
            block_local[13] = 0.06533548F - 0.05F;
            block_local[14] = -0.05F;
            const Matrix4 block_world = multiply_matrix(block_local, prop_anchor);
            constexpr std::array<std::array<float, 3>, 8> corners{{
                {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F},
                {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 1.0F},
            }};
            struct WorldBlockFace final
            {
                std::array<std::size_t, 4> corners{};
                std::array<float, 3> normal{};
                world::FaceDirection direction{};
            };
            constexpr std::array<WorldBlockFace, 6> faces{{
                {{{1U, 2U, 6U, 5U}}, {1.0F, 0.0F, 0.0F}, world::FaceDirection::positive_x},
                {{{0U, 4U, 7U, 3U}}, {-1.0F, 0.0F, 0.0F}, world::FaceDirection::negative_x},
                {{{3U, 7U, 6U, 2U}}, {0.0F, 1.0F, 0.0F}, world::FaceDirection::positive_y},
                {{{0U, 1U, 5U, 4U}}, {0.0F, -1.0F, 0.0F}, world::FaceDirection::negative_y},
                {{{4U, 5U, 6U, 7U}}, {0.0F, 0.0F, 1.0F}, world::FaceDirection::positive_z},
                {{{0U, 3U, 2U, 1U}}, {0.0F, 0.0F, -1.0F}, world::FaceDirection::negative_z},
            }};
            constexpr std::array<std::size_t, 6> order{0U, 1U, 2U, 0U, 2U, 3U};
            for (const WorldBlockFace &face : faces)
            {
                const auto tile = world::atlas_tile(held_block, face.direction);
                const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
                const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
                const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
                const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
                const auto uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
                const auto normal = normal_to_world(
                    transform_direction(face.normal, block_world));
                EntitySpan span;
                span.texture = block_atlas;
                span.first = static_cast<std::uint32_t>(output.vertices.size());
                for (std::size_t index : order)
                {
                    const std::size_t corner = face.corners[index];
                    const world::Vec3 position =
                        to_world(transform_point(corners[corner], block_world));
                    EntityVertex vertex;
                    vertex.x = position.x;
                    vertex.y = position.y;
                    vertex.z = position.z;
                    vertex.u = uv[index][0];
                    vertex.v = uv[index][1];
                    vertex.normal_x = normal[0];
                    vertex.normal_y = normal[1];
                    vertex.normal_z = normal[2];
                    vertex.sky_light = std::clamp(sky_light, 0.0F, 1.0F);
                    vertex.torch_light = std::clamp(torch_light, 0.0F, 1.5F);
                    output.vertices.push_back(vertex);
                }
                span.count = static_cast<std::uint32_t>(output.vertices.size()) - span.first;
                output.spans.push_back(span);
            }
        }
    }

#if defined(_WIN32)
    struct DepthVertex final
    {
        std::array<float, 3> position{};
        std::array<float, 2> uv{};
        std::array<float, 4> color{};
        std::array<float, 4> normal_material{};
    };

    struct alignas(16) ViewmodelConstants final
    {
        float x_scale{};
        float y_scale{};
        float depth_scale{};
        float depth_offset{};
        float ndc_offset_x{};
        float ndc_offset_y{};
        float padding0{};
        float padding1{};
        std::array<float, 4> light_direction{};
        std::array<float, 4> specular_color{};
        // x = world exposure, y = tone-curve index, z = grade strength.
        std::array<float, 4> display_grading{};
        std::array<float, 4> fog_color{};
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
            context->PSGetShaderResources(0U, 1U, pixel_resource.ReleaseAndGetAddressOf());
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
            ID3D11ShaderResourceView *pixel_resource_pointer = pixel_resource.Get();
            context->PSSetShaderResources(0U, 1U, &pixel_resource_pointer);
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
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pixel_resource;
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

    void initialize_depth_pipeline()
    {
        const SDL_PropertiesID renderer_properties = SDL_GetRendererProperties(renderer_);
        auto *device = static_cast<ID3D11Device *>(SDL_GetPointerProperty(
            renderer_properties, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
        if (device == nullptr)
            return;
        device_ = device;
        ID3D11DeviceContext *context{};
        device_->GetImmediateContext(&context);
        context_.Attach(context);

        static constexpr char vertex_source[] = R"hlsl(
cbuffer Viewmodel : register(b0)
{
    float4 Projection;
    float4 Offset;
    float4 LightDirection;
    // rgb = directional sun colour, w = Rebirth material-path flag.
    float4 SpecularColor;
    // x = world exposure, y = tone-curve index, z = grade strength.
    float4 DisplayGrading;
    float4 FogColor;
};

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 normalMaterial : NORMAL0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 normalMaterial : TEXCOORD1;
    float3 viewPosition : TEXCOORD2;
};

PixelInput main(VertexInput input)
{
    PixelInput output;
    float depth = input.position.z;
    output.position = float4(input.position.x * Projection.x + depth * Offset.x,
                             input.position.y * Projection.y + depth * Offset.y,
                             depth * Projection.z + Projection.w,
                             depth);
    output.uv = input.uv;
    output.color = input.color;
    output.normalMaterial = input.normalMaterial;
    output.viewPosition = input.position;
    return output;
}
)hlsl";
        static constexpr char pixel_source[] = R"hlsl(
cbuffer Viewmodel : register(b0)
{
    float4 Projection;
    float4 Offset;
    // xyz = directional sun, w = local torch specular strength.
    float4 LightDirection;
    // rgb = directional sun colour, w = Rebirth material-path flag.
    float4 SpecularColor;
    // x = world exposure, y = tone-curve index, z = grade strength.
    float4 DisplayGrading;
    float4 FogColor;
};

Texture2D Diffuse : register(t0);
SamplerState DiffuseSampler : register(s0);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 normalMaterial : TEXCOORD1;
    float3 viewPosition : TEXCOORD2;
};

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

float3 tonemap_reinhard(float3 color)
{
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float3 perChannel = color / (1.0f + color);
    return saturate(lerp(color / (1.0f + luminance), perChannel, perChannel));
}

float3 hable_curve(float3 value)
{
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;
    return ((value * (A * value + C * B) + D * E) /
            (value * (A * value + B) + D * F)) - E / F;
}

float3 tonemap_filmic(float3 color)
{
    const float white = 11.2f;
    return saturate(hable_curve(color * 2.0f) /
                    hable_curve(float3(white, white, white)));
}

float3 tonemap_legacy(float3 color)
{
    float luminance = dot(color, float3(0.299f, 0.587f, 0.114f));
    if (luminance > 0.0001f)
    {
        float compressed = luminance /
                           (1.0f + max(0.0f, luminance - 1.0f) * 0.45f);
        color *= compressed / luminance;
    }
    return color;
}

float3 apply_tonemap(float3 color)
{
    int curve = (int)(DisplayGrading.y + 0.5f);
    if (curve == 0)
        return tonemap_legacy(color);
    if (curve == 1)
        return tonemap_reinhard(color);
    if (curve == 2)
        return tonemap_filmic(color);
    return tonemap_aces(color);
}

float3 apply_white_balance(float3 color)
{
    float3 tint = 1.0f / (FogColor.rgb + 0.5f);
    tint = lerp(tint, float3(1.0f, 1.0f, 1.0f), 0.2f);
    tint /= max(min(tint.r, min(tint.g, tint.b)), 0.0001f);
    tint = min(tint, 1.25f);
    return lerp(color, color * tint, saturate(DisplayGrading.z));
}

float3 apply_grade(float3 color)
{
    float strength = DisplayGrading.z;
    if (strength <= 0.001f)
        return color;
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float3 graded = lerp(float3(luminance, luminance, luminance), color, 0.79f);
    const float contrast = 1.18f;
    float3 low = 0.5f * pow(saturate(2.0f * graded), contrast);
    float3 high = 1.0f - 0.5f * pow(saturate(2.0f - 2.0f * graded), contrast);
    graded = lerp(low, high, step(0.5f, graded));
    return saturate(lerp(color, graded, saturate(strength)));
}

float3 apply_dither(float3 color, float2 pixel)
{
    float3 noise = float3(dot(float2(171.0f, 231.0f), pixel),
                          dot(float2(171.0f, 231.0f), pixel),
                          dot(float2(171.0f, 231.0f), pixel));
    noise = frac(noise / float3(103.0f, 71.0f, 97.0f));
    return color + (noise - 0.5f) / 255.0f;
}

float4 main(PixelInput input) : SV_TARGET
{
    float4 texel = Diffuse.Sample(DiffuseSampler, input.uv);
    float4 color = texel * input.color;
    clip(color.a - 0.01f);
    bool modernMaterial = SpecularColor.w > 0.5f;
    if (input.normalMaterial.w < -1.5f)
    {
        // Camera-facing held-torch flame. Its tapered alpha comes from UVs so
        // the effect stays soft at any resolution without a billboard asset.
        float rise = 1.0f - input.uv.y;
        float lateral = abs(input.uv.x - 0.5f) * 2.0f;
        float flameWidth = lerp(0.92f, 0.10f, rise);
        float silhouette = saturate((flameWidth - lateral) * 10.0f);
        silhouette *= smoothstep(0.0f, 0.13f, rise) *
                      (1.0f - smoothstep(0.91f, 1.0f, rise));
        clip(silhouette - 0.01f);
        float core = saturate(1.0f - lateral / max(flameWidth, 0.04f));
        float3 flame = lerp(float3(1.0f, 0.055f, 0.005f),
                            float3(1.0f, 0.68f, 0.07f), core * core);
        color = float4(flame, silhouette * 0.88f);
        if (modernMaterial)
        {
            color.rgb *= max(DisplayGrading.x, 0.55f) * 2.2f;
            color.rgb = apply_tonemap(color.rgb);
            color.rgb = sqrt(saturate(color.rgb));
            color.rgb = apply_grade(color.rgb);
            color.rgb = apply_dither(color.rgb, input.position.xy);
        }
        return color;
    }
    // Rebirth shades the combined display-referred albedo/tint in linear
    // light, then encodes at the end. With no specular term this round-trip is
    // neutral, so cloth/skin retain their authored appearance.
    if (modernMaterial)
    {
        color.rgb = texel.rgb * texel.rgb * input.color.rgb;
        if (input.normalMaterial.w < -0.001f)
        {
            // Display-white legacy suit textures are not a physical 100%
            // reflector. Give cloth a matte energy ceiling so it keeps folds
            // and stitching instead of clipping to paper white after ACES.
            float fabricWeight = saturate(-input.normalMaterial.w * 2.0f);
            float3 fabricNormal = normalize(input.normalMaterial.xyz);
            float3 fabricView = normalize(-input.viewPosition);
            if (dot(fabricNormal, fabricView) < 0.0f)
                fabricNormal = -fabricNormal;
            float fabricFacing = saturate(dot(fabricNormal, fabricView));
            float fabricEnergy = lerp(1.0f, 0.62f, fabricWeight);
            float grazingOcclusion = lerp(0.90f, 1.0f, fabricFacing);
            color.rgb *= fabricEnergy * grazingOcclusion;
        }
    }
    if (input.normalMaterial.w > 0.001f)
    {
        // The original textures already contain fine scratches, edge wear and
        // inset labels. Preserve that authored relief after minification with a
        // very small local-contrast term on reflective parts only.
        uint textureWidth;
        uint textureHeight;
        Diffuse.GetDimensions(textureWidth, textureHeight);
        float2 texelSize = 1.0f / max(float2(textureWidth, textureHeight), 1.0f.xx);
        float centerLuma = dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float neighborhoodLuma =
            dot(Diffuse.Sample(DiffuseSampler, input.uv + float2(texelSize.x, 0.0f)).rgb,
                float3(0.2126f, 0.7152f, 0.0722f)) +
            dot(Diffuse.Sample(DiffuseSampler, input.uv - float2(texelSize.x, 0.0f)).rgb,
                float3(0.2126f, 0.7152f, 0.0722f)) +
            dot(Diffuse.Sample(DiffuseSampler, input.uv + float2(0.0f, texelSize.y)).rgb,
                float3(0.2126f, 0.7152f, 0.0722f)) +
            dot(Diffuse.Sample(DiffuseSampler, input.uv - float2(0.0f, texelSize.y)).rgb,
                float3(0.2126f, 0.7152f, 0.0722f));
        float detail = centerLuma - neighborhoodLuma * 0.25f;
        float detailWeight = saturate(input.normalMaterial.w * 6.0f);
        float detailGain = modernMaterial ? 0.55f : 1.35f;
        color.rgb *= 1.0f + clamp(detail * detailGain, -0.07f, 0.10f) * detailWeight;
        float3 normal = normalize(input.normalMaterial.xyz);
        float3 viewDirection = normalize(-input.viewPosition);
        if (dot(normal, viewDirection) < 0.0f)
            normal = -normal;
        float3 lightDirection = modernMaterial
                                    ? normalize(LightDirection.xyz)
                                    : normalize(float3(-0.38f, 0.78f, -0.49f));
        float3 halfDirection = normalize(lightDirection + viewDirection);
        float facing = saturate(dot(normal, viewDirection));
        float response = saturate(input.normalMaterial.w);
        if (modernMaterial)
        {
            float smoothness = saturate(response / 0.30f);
            float roughness = lerp(0.84f, 0.30f, smoothness);
            float nDotL = saturate(dot(normal, lightDirection));
            float nDotV = max(facing, 0.001f);
            float nDotH = saturate(dot(normal, halfDirection));
            float vDotH = saturate(dot(viewDirection, halfDirection));
            float alpha = roughness * roughness;
            float alpha2 = alpha * alpha;
            float denom = nDotH * nDotH * (alpha2 - 1.0f) + 1.0f;
            float distribution = alpha2 / max(3.14159265f * denom * denom, 0.0001f);
            float geometryK = (roughness + 1.0f);
            geometryK = geometryK * geometryK * 0.125f;
            float geometryV = nDotV / lerp(nDotV, 1.0f, geometryK);
            float geometryL = nDotL / lerp(nDotL, 1.0f, geometryK);
            float metalness = smoothstep(0.22f, 0.30f, response);
            float3 f0 = lerp(float3(0.028f, 0.028f, 0.028f), color.rgb, metalness);
            float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - vDotH, 5.0f);
            float3 specular = fresnel * distribution * geometryV * geometryL /
                              max(4.0f * nDotV * nDotL, 0.001f);
            color.rgb += SpecularColor.rgb * specular * nDotL;

            // A held/nearby torch sits close to the camera, so its light and
            // view directions coincide. Keep this warm secondary lobe separate
            // from the sun instead of turning the whole prop orange or glossy.
            if (LightDirection.w > 0.001f)
            {
                float localNDotL = facing;
                float localNDotH = facing;
                float localDenom = localNDotH * localNDotH * (alpha2 - 1.0f) + 1.0f;
                float localDistribution =
                    alpha2 / max(3.14159265f * localDenom * localDenom, 0.0001f);
                float localGeometry = nDotV / lerp(nDotV, 1.0f, geometryK);
                float3 localSpecular = f0 * localDistribution * localGeometry * localGeometry /
                                       max(4.0f * nDotV * localNDotL, 0.001f);
                color.rgb += float3(1.0f, 0.849f, 0.555f) * LightDirection.w *
                             localSpecular * localNDotL;
            }
        }
        else
        {
            float power = lerp(14.0f, 68.0f, saturate(response * 3.0f));
            float highlight = pow(saturate(dot(normal, halfDirection)), power) * response;
            float fresnel = pow(1.0f - facing, 5.0f) * response * 0.22f;
            float skyMix = saturate(normal.y * 0.5f + 0.5f);
            float3 environment = lerp(float3(0.07f, 0.09f, 0.13f),
                                      float3(0.48f, 0.54f, 0.62f), skyMix);
            color.rgb += highlight * float3(1.0f, 0.97f, 0.90f) + fresnel * environment;
        }
    }
    if (modernMaterial)
    {
        color.rgb = max(color.rgb, 0.0f) * DisplayGrading.x;
        color.rgb = apply_white_balance(color.rgb);
        color.rgb = apply_tonemap(color.rgb);
        color.rgb = sqrt(saturate(color.rgb));
        color.rgb = apply_grade(color.rgb);
        color.rgb = apply_dither(color.rgb, input.position.xy);
    }
    return color;
}
)hlsl";
        const auto compile = [](const char *source, const char *target) {
            Microsoft::WRL::ComPtr<ID3DBlob> shader;
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            if (FAILED(D3DCompile(source, std::char_traits<char>::length(source), nullptr,
                                  nullptr, nullptr, "main", target,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0U, &shader, &errors)))
            {
                if (SDL_getenv("OPENMINERZ_GRAPHICS_TRACE") != nullptr && errors != nullptr)
                    SDL_Log("Viewmodel shader compile failed: %.*s",
                            static_cast<int>(errors->GetBufferSize()),
                            static_cast<const char *>(errors->GetBufferPointer()));
                return Microsoft::WRL::ComPtr<ID3DBlob>{};
            }
            return shader;
        };
        const auto vertex_blob = compile(vertex_source, "vs_4_0");
        const auto pixel_blob = compile(pixel_source, "ps_4_0");
        if (vertex_blob == nullptr || pixel_blob == nullptr ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &depth_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr,
                                              &depth_pixel_shader_)))
            return;
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> input_elements{{
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D11_INPUT_PER_VERTEX_DATA,
             0U},
            {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 12U, D3D11_INPUT_PER_VERTEX_DATA, 0U},
            {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 20U, D3D11_INPUT_PER_VERTEX_DATA,
             0U},
            {"NORMAL", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 36U,
             D3D11_INPUT_PER_VERTEX_DATA, 0U},
        }};
        if (FAILED(device_->CreateInputLayout(
                input_elements.data(), static_cast<UINT>(input_elements.size()),
                vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                &depth_input_layout_)))
            return;
        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = sizeof(ViewmodelConstants);
        constant_description.Usage = D3D11_USAGE_DYNAMIC;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&constant_description, nullptr,
                                         &depth_constant_buffer_)))
            return;
        D3D11_DEPTH_STENCIL_DESC depth_description{};
        depth_description.DepthEnable = TRUE;
        depth_description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_description.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device_->CreateDepthStencilState(&depth_description, &depth_depth_state_)))
            return;
        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rasterizer_description,
                                                  &depth_rasterizer_state_)))
            return;
        D3D11_BLEND_DESC blend_description{};
        auto &target_blend = blend_description.RenderTarget[0];
        target_blend.BlendEnable = TRUE;
        target_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOp = D3D11_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend_description, &depth_blend_state_)))
            return;
        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.MinLOD = 0.0F;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler_description, &depth_sampler_state_)))
            return;
        constexpr std::array<std::uint8_t, 4> white_pixel{255U, 255U, 255U, 255U};
        D3D11_TEXTURE2D_DESC white_description{};
        white_description.Width = 1U;
        white_description.Height = 1U;
        white_description.MipLevels = 1U;
        white_description.ArraySize = 1U;
        white_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        white_description.SampleDesc.Count = 1U;
        white_description.Usage = D3D11_USAGE_IMMUTABLE;
        white_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA white_data{white_pixel.data(), 4U, 4U};
        if (FAILED(device_->CreateTexture2D(&white_description, &white_data,
                                            &depth_white_texture_)) ||
            FAILED(device_->CreateShaderResourceView(depth_white_texture_.Get(), nullptr,
                                                     &depth_white_view_)))
            return;
        depth_ready_ = true;
        if (SDL_getenv("OPENMINERZ_GRAPHICS_TRACE") != nullptr)
            SDL_Log("Depth-tested HDR viewmodel pipeline: ready");
    }

    [[nodiscard]] bool ensure_depth_target(int width, int height)
    {
        if (depth_target_texture_ != nullptr && depth_target_width_ == width &&
            depth_target_height_ == height)
            return true;
        SDL_DestroyTexture(depth_target_texture_);
        depth_target_texture_ = nullptr;
        depth_target_view_.Reset();
        depth_color_resource_.Reset();
        depth_presentation_resource_.Reset();
        depth_stencil_view_.Reset();
        depth_stencil_texture_.Reset();
        depth_target_width_ = 0;
        depth_target_height_ = 0;
        depth_target_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                                  SDL_TEXTUREACCESS_STATIC, width, height);
        if (depth_target_texture_ == nullptr)
            return false;
        const SDL_PropertiesID texture_properties =
            SDL_GetTextureProperties(depth_target_texture_);
        auto *presentation = static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
            texture_properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        if (presentation == nullptr)
            return false;
        depth_presentation_resource_ = presentation;
        D3D11_TEXTURE2D_DESC color_description{};
        depth_presentation_resource_->GetDesc(&color_description);
        color_description.Usage = D3D11_USAGE_DEFAULT;
        color_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        color_description.CPUAccessFlags = 0U;
        color_description.MiscFlags = 0U;
        if (FAILED(device_->CreateTexture2D(&color_description, nullptr,
                                            &depth_color_resource_)) ||
            FAILED(device_->CreateRenderTargetView(depth_color_resource_.Get(), nullptr,
                                                   &depth_target_view_)))
            return false;
        SDL_SetTextureBlendMode(depth_target_texture_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(depth_target_texture_, SDL_SCALEMODE_LINEAR);
        D3D11_TEXTURE2D_DESC stencil_description{};
        stencil_description.Width = static_cast<UINT>(width);
        stencil_description.Height = static_cast<UINT>(height);
        stencil_description.MipLevels = 1U;
        stencil_description.ArraySize = 1U;
        stencil_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        stencil_description.SampleDesc.Count = 1U;
        stencil_description.Usage = D3D11_USAGE_DEFAULT;
        stencil_description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device_->CreateTexture2D(&stencil_description, nullptr,
                                            &depth_stencil_texture_)) ||
            FAILED(device_->CreateDepthStencilView(depth_stencil_texture_.Get(), nullptr,
                                                   &depth_stencil_view_)))
            return false;
        depth_target_width_ = width;
        depth_target_height_ = height;
        return true;
    }

    [[nodiscard]] ID3D11ShaderResourceView *depth_view_for(SDL_Texture *texture)
    {
        if (texture == nullptr)
            return depth_white_view_.Get();
        const auto found = depth_srv_cache_.find(texture);
        if (found != depth_srv_cache_.end())
            return found->second.Get();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
        auto *resource = static_cast<ID3D11Texture2D *>(SDL_GetPointerProperty(
            properties, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
        if (resource != nullptr)
            device_->CreateShaderResourceView(resource, nullptr, &view);
        ID3D11ShaderResourceView *raw = view.Get();
        depth_srv_cache_.emplace(texture, std::move(view));
        return raw != nullptr ? raw : depth_white_view_.Get();
    }

    [[nodiscard]] bool render_depth_pass(const std::vector<ViewmodelTriangle> &triangles,
                                         int width, int height, float focal, float aspect,
                                         float idle_pixels)
    {
        if (triangles.empty())
            return true;
        // Supersample the arm/held-item pass 2x and let the linear downscale
        // resolve it: cheap geometry, and block-item and sleeve edges stop
        // stair-stepping.
        const int render_width = width * 2;
        const int render_height = height * 2;
        if (!ensure_depth_target(render_width, render_height))
        {
            depth_ready_ = false;
            return false;
        }
        struct Span final
        {
            SDL_Texture *texture{};
            UINT first{};
            UINT count{};
            int render_layer{};
        };
        depth_vertices_.clear();
        depth_vertices_.reserve(triangles.size() * 3U);
        std::vector<Span> spans;
        // Keep the source graph grouped for texture batching, but resolve the
        // complete articulated rig in one depth buffer. Clearing depth between
        // palm, prop and finger groups made every finger triangle overwrite a
        // held tool even when it was physically behind the grip, which read as
        // the weapon phasing through a pasted-on hand.
        for (int render_layer = 0; render_layer <= 3; ++render_layer)
        {
            for (const auto &triangle : triangles)
            {
                if (triangle.render_layer != render_layer)
                    continue;
                if (spans.empty() || spans.back().texture != triangle.texture ||
                    spans.back().render_layer != render_layer)
                    spans.push_back({triangle.texture,
                                     static_cast<UINT>(depth_vertices_.size()), 0U,
                                     render_layer});
                for (std::size_t corner = 0; corner < 3U; ++corner)
                {
                    DepthVertex vertex;
                    vertex.position = triangle.view[corner];
                    if (render_layer == 2)
                    {
                        // A sub-two-millimetre contact bias lets curled digits
                        // meet an intersecting grip without pulling the whole
                        // hand in front of the weapon, as the old layer clear did.
                        vertex.position[2] = std::max(0.0105F, vertex.position[2] - 0.0015F);
                    }
                    vertex.uv = {triangle.vertices[corner].tex_coord.x,
                                 triangle.vertices[corner].tex_coord.y};
                    vertex.color = {triangle.vertices[corner].color.r,
                                    triangle.vertices[corner].color.g,
                                    triangle.vertices[corner].color.b,
                                    triangle.vertices[corner].color.a};
                    vertex.normal_material = triangle.normal_material[corner];
                    depth_vertices_.push_back(vertex);
                }
                spans.back().count += 3U;
            }
        }
        const UINT vertex_count = static_cast<UINT>(depth_vertices_.size());
        if (depth_vertex_buffer_ == nullptr || depth_vertex_capacity_ < vertex_count)
        {
            depth_vertex_buffer_.Reset();
            depth_vertex_capacity_ = std::max(vertex_count, depth_vertex_capacity_ * 2U);
            const D3D11_BUFFER_DESC description{
                static_cast<UINT>(depth_vertex_capacity_ * sizeof(DepthVertex)),
                D3D11_USAGE_DYNAMIC,
                D3D11_BIND_VERTEX_BUFFER,
                D3D11_CPU_ACCESS_WRITE,
                0U,
                sizeof(DepthVertex)};
            if (FAILED(device_->CreateBuffer(&description, nullptr, &depth_vertex_buffer_)))
            {
                depth_ready_ = false;
                return false;
            }
        }
        if (!SDL_FlushRenderer(renderer_))
            return false;
        const D3DStateBackup previous_state{context_.Get()};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(depth_vertex_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                 &mapped)))
        {
            previous_state.restore(context_.Get());
            return false;
        }
        std::copy(depth_vertices_.begin(), depth_vertices_.end(),
                  static_cast<DepthVertex *>(mapped.pData));
        context_->Unmap(depth_vertex_buffer_.Get(), 0U);
        // Match the stock GunEyePointCamera projection: a 0.01m near plane so close hand
        // geometry clips exactly where the original clipped it.
        constexpr float near_plane = 0.01F;
        constexpr float far_plane = 32.0F;
        ViewmodelConstants constants{};
        constants.x_scale = focal / aspect;
        constants.y_scale = focal;
        constants.depth_scale = far_plane / (far_plane - near_plane);
        constants.depth_offset = -near_plane * far_plane / (far_plane - near_plane);
        constants.ndc_offset_x = 0.0F;
        constants.ndc_offset_y = -2.0F * idle_pixels / static_cast<float>(height);
        constants.light_direction = {env_light_direction_[0], env_light_direction_[1],
                                     -env_light_direction_[2], env_local_torch_};
        constants.specular_color = {env_directional_[0], env_directional_[1],
                                    env_directional_[2], env_rebirth_ ? 1.0F : 0.0F};
        constants.display_grading = {env_exposure_, static_cast<float>(env_tone_curve_),
                                     env_color_grading_, 0.0F};
        constants.fog_color = {env_fog_color_[0], env_fog_color_[1], env_fog_color_[2], 0.0F};
        if (FAILED(context_->Map(depth_constant_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                 &mapped)))
        {
            previous_state.restore(context_.Get());
            return false;
        }
        *static_cast<ViewmodelConstants *>(mapped.pData) = constants;
        context_->Unmap(depth_constant_buffer_.Get(), 0U);
        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        context_->ClearRenderTargetView(depth_target_view_.Get(), transparent.data());
        context_->ClearDepthStencilView(depth_stencil_view_.Get(), D3D11_CLEAR_DEPTH, 1.0F, 0U);
        ID3D11RenderTargetView *render_target = depth_target_view_.Get();
        context_->OMSetRenderTargets(1U, &render_target, depth_stencil_view_.Get());
        context_->OMSetDepthStencilState(depth_depth_state_.Get(), 0U);
        context_->OMSetBlendState(depth_blend_state_.Get(), nullptr, 0xFFFFFFFFU);
        const D3D11_VIEWPORT viewport{0.0F,
                                      0.0F,
                                      static_cast<float>(render_width),
                                      static_cast<float>(render_height),
                                      0.0F,
                                      1.0F};
        context_->RSSetViewports(1U, &viewport);
        context_->RSSetState(depth_rasterizer_state_.Get());
        const UINT stride = sizeof(DepthVertex);
        const UINT offset = 0U;
        context_->IASetInputLayout(depth_input_layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer *vertex_buffer = depth_vertex_buffer_.Get();
        context_->IASetVertexBuffers(0U, 1U, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(depth_vertex_shader_.Get(), nullptr, 0U);
        ID3D11Buffer *constant_buffer = depth_constant_buffer_.Get();
        context_->VSSetConstantBuffers(0U, 1U, &constant_buffer);
        context_->PSSetShader(depth_pixel_shader_.Get(), nullptr, 0U);
        context_->PSSetConstantBuffers(0U, 1U, &constant_buffer);
        ID3D11SamplerState *sampler = depth_sampler_state_.Get();
        context_->PSSetSamplers(0U, 1U, &sampler);
        for (const Span &span : spans)
        {
            ID3D11ShaderResourceView *view = depth_view_for(span.texture);
            context_->PSSetShaderResources(0U, 1U, &view);
            context_->Draw(span.count, span.first);
        }
        ID3D11ShaderResourceView *empty_view{};
        context_->PSSetShaderResources(0U, 1U, &empty_view);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->CopyResource(depth_presentation_resource_.Get(), depth_color_resource_.Get());
        previous_state.restore(context_.Get());
        if (!SDL_FlushRenderer(renderer_))
            return false;
        const SDL_FRect destination{0.0F, 0.0F, static_cast<float>(width),
                                    static_cast<float>(height)};
        return SDL_RenderTexture(renderer_, depth_target_texture_, nullptr, &destination);
    }
#endif

    SDL_Renderer *renderer_{};
    SDL_Texture *white_texture_{};
    std::vector<AvatarBatch> batches_;
    std::vector<Matrix4> inverse_bind_pose_;
    std::vector<Matrix4> source_pose_local_;
    std::vector<Matrix4> proxy_inverse_bind_;
    std::vector<Matrix4> proxy_bind_local_;
    std::vector<int> proxy_avatar_bone_;
    std::vector<int> proxy_parents_;
    std::vector<std::array<float, 3>> avatar_shape_scale_;
    std::array<int, 71> proxy_bone_by_avatar_{};
    std::vector<CarrierPart> carrier_parts_;
    std::unique_ptr<StaticModelAsset> pickaxe_;
    std::unique_ptr<StaticModelAsset> compass_;
    std::unique_ptr<StaticModelAsset> pistol_;
    std::unique_ptr<StaticModelAsset> knife_;
    std::unique_ptr<StaticModelAsset> ammo_;
    std::unique_ptr<StaticModelAsset> torch_;
    std::unique_ptr<StaticModelAsset> spade_;
    std::unique_ptr<StaticModelAsset> axe_;
    std::unique_ptr<StaticModelAsset> ak_;
    std::unique_ptr<StaticModelAsset> shotgun_;
    std::unique_ptr<StaticModelAsset> rifle_;
    std::unique_ptr<StaticModelAsset> smg_;
    std::unique_ptr<StaticModelAsset> lmg_;
    std::unique_ptr<StaticModelAsset> rpg_;
    std::unique_ptr<StaticModelAsset> grenade_;
    std::unique_ptr<StaticModelAsset> space_ak_;
    std::unique_ptr<StaticModelAsset> space_pistol_;
    std::unique_ptr<StaticModelAsset> space_shotgun_;
    std::unique_ptr<StaticModelAsset> space_rifle_;
    std::unique_ptr<StaticModelAsset> space_smg_;
    std::unique_ptr<StaticModelAsset> saber_;
    std::unique_ptr<StaticModelAsset> ore_;
    std::unique_ptr<StaticModelAsset> gems_;
    std::unique_ptr<StaticModelAsset> bars_;
    std::unique_ptr<StaticModelAsset> clock_;
    std::unique_ptr<StaticModelAsset> locator_;
    std::unique_ptr<StaticModelAsset> teleporter_;
    std::unique_ptr<StaticModelAsset> gunpowder_;
    NativeAnimation pick_idle_;
    NativeAnimation pick_use_;
    NativeAnimation generic_idle_;
    NativeAnimation generic_use_;
    NativeAnimation block_idle_;
    NativeAnimation block_use_;
    std::unordered_map<std::string, NativeAnimation> animation_library_;
    std::unordered_map<std::uint8_t, WorldPlayerAnimationState> world_animation_states_;
    AnimationChannel item_channel_;
    AnimationChannel action_channel_;
    // GetAvatarColor: ambient plus a directional sun term (Lambert against
    // the camera-space sun direction), with a soft curvature term so the
    // sculpted read of the old fixed shade survives.
    [[nodiscard]] std::array<float, 3>
    environment_light(const std::array<float, 3> &normal) const noexcept
    {
        const float lambert =
            std::max(0.0F, normal[0] * env_light_direction_[0] +
                               normal[1] * env_light_direction_[1] +
                               normal[2] * env_light_direction_[2]);
        const float shape =
            std::clamp(0.90F + normal[1] * 0.10F - normal[2] * 0.06F, 0.72F, 1.0F);
        return {std::min(1.0F, (env_ambient_[0] + env_directional_[0] * lambert) * shape),
                std::min(1.0F, (env_ambient_[1] + env_directional_[1] * lambert) * shape),
                std::min(1.0F, (env_ambient_[2] + env_directional_[2] * lambert) * shape)};
    }

    float last_elapsed_seconds_{-1.0F};
    float last_action_progress_{};
    // GetAvatarColor environment: ambient + directional sun (camera space).
    std::array<float, 3> env_ambient_{0.62F, 0.62F, 0.62F};
    std::array<float, 3> env_directional_{0.38F, 0.38F, 0.38F};
    std::array<float, 3> env_light_direction_{0.30F, 0.85F, 0.20F};
    std::array<float, 3> env_fog_color_{0.29F, 0.34F, 0.36F};
    float env_exposure_{1.0F};
    float env_local_torch_{};
    float env_color_grading_{1.0F};
    int env_tone_curve_{3};
    bool env_rebirth_{};
    std::size_t triangle_count_{};
    std::size_t carrier_triangle_count_{};
    bool loaded_{};
    bool animation_loaded_{};
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> depth_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> depth_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> depth_input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> depth_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> depth_vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_depth_state_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> depth_rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> depth_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> depth_sampler_state_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_white_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_white_view_;
    std::unordered_map<SDL_Texture *, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
        depth_srv_cache_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_color_resource_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_presentation_resource_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> depth_target_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_stencil_texture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
    std::vector<DepthVertex> depth_vertices_;
    UINT depth_vertex_capacity_{};
#endif
    SDL_Texture *depth_target_texture_{};
    int depth_target_width_{};
    int depth_target_height_{};
    bool depth_ready_{};
};

AvatarViewmodel::AvatarViewmodel(SDL_Renderer *renderer, std::filesystem::path asset_path)
    : impl_(std::make_unique<Impl>(renderer, asset_path))
{
}

AvatarViewmodel::~AvatarViewmodel() = default;

bool AvatarViewmodel::loaded() const noexcept
{
    return impl_->loaded_;
}

bool AvatarViewmodel::world_animation_ready() const noexcept
{
    if (!impl_->loaded_ || impl_->inverse_bind_pose_.size() != 71U ||
        impl_->source_pose_local_.size() != 71U)
        return false;
    for (const std::string_view required : {"Stand0", "Walk", "Run", "Faint"})
    {
        const auto found = impl_->animation_library_.find(std::string{required});
        if (found == impl_->animation_library_.end() || found->second.frames.empty())
            return false;
    }
    return true;
}

std::size_t AvatarViewmodel::animation_clip_count() const noexcept
{
    return std::ranges::count_if(impl_->animation_library_, [](const auto &entry) {
        return !entry.second.frames.empty();
    });
}

std::size_t AvatarViewmodel::triangle_count() const noexcept
{
    return impl_->triangle_count_;
}

void AvatarViewmodel::set_environment_light(const std::array<float, 3> &ambient,
                                            const std::array<float, 3> &directional,
                                            const std::array<float, 3> &direction_camera,
                                            bool rebirth_presentation, float local_torch,
                                            float exposure,
                                            int tone_curve, float color_grading,
                                            const std::array<float, 3> &fog_color) noexcept
{
    impl_->env_ambient_ = ambient;
    impl_->env_directional_ = directional;
    impl_->env_light_direction_ = direction_camera;
    impl_->env_fog_color_ = fog_color;
    impl_->env_local_torch_ = std::clamp(local_torch, 0.0F, 1.5F);
    impl_->env_exposure_ = std::clamp(exposure, 0.1F, 4.0F);
    impl_->env_tone_curve_ = std::clamp(tone_curve, 0, 3);
    impl_->env_color_grading_ = std::clamp(color_grading, 0.0F, 1.5F);
    impl_->env_rebirth_ = rebirth_presentation;
}

void AvatarViewmodel::draw(const ui::UiMetrics &metrics, const world::PlayerState &player,
                           float elapsed_seconds, bool reduced_motion,
                           float viewmodel_fov_degrees, SDL_Texture *block_atlas)
{
    impl_->draw(metrics, player, elapsed_seconds, reduced_motion, viewmodel_fov_degrees,
                block_atlas);
}

void AvatarViewmodel::append_world_player(EntityBatch &batch, std::uint8_t gid,
                                          InventoryItemId carried_item,
                                          world::Vec3 feet, float yaw,
                                          float torso_pitch, float animation_seconds,
                                          float movement_x, float movement_y,
                                          PlayerAnimationMode animation_mode, bool using_tool,
                                          bool dead, bool shouldering, bool reloading,
                                          bool throwing_grenade, bool ready_to_throw_grenade,
                                          float sky_light, float torch_light,
                                          SDL_Texture *block_atlas)
{
    impl_->append_world_player(batch, gid, carried_item, feet, yaw,
                               torso_pitch, animation_seconds,
                               movement_x, movement_y, animation_mode, using_tool, dead,
                               shouldering, reloading, throwing_grenade,
                               ready_to_throw_grenade, sky_light, torch_light, block_atlas);
}

std::filesystem::path default_avatar_path()
{
    // Runtime uses the validated local import only. Developer export folders
    // are never probed implicitly; a future avatar picker can validate
    // and import a user-selected file explicitly.
    return derived_data_path() / "avatar.omavatar";
}

} // namespace openminerz
