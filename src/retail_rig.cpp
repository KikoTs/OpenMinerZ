#include "openminerz/retail_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

// Ports of the DNA.Common character classes. Each function names the original
// it reproduces; the arithmetic (operand order, float/double widths, the
// millisecond rounding of TimeSpan.FromSeconds, the `Length - 2` key test) is
// kept as the shipped assemblies perform it, and verified against them by
// tools/OriginalClientInterop/RigParityProbe + tests/test_rig_parity.cpp.

namespace openminerz::rig
{
namespace
{

class Reader final
{
  public:
    explicit Reader(const std::filesystem::path &path) : stream_(path, std::ios::binary)
    {
        if (!stream_)
            throw std::runtime_error{"could not open the retail character"};
        std::error_code error;
        size_ = std::filesystem::file_size(path, error);
        if (error)
            throw std::runtime_error{"could not size the retail character"};
    }

    void bytes(void *destination, std::size_t count)
    {
        if (count > size_ - position_)
            throw std::runtime_error{"truncated retail character"};
        stream_.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
        if (!stream_)
            throw std::runtime_error{"failed reading the retail character"};
        position_ += count;
    }

    template <typename Value> Value read()
    {
        Value value{};
        bytes(&value, sizeof(value));
        return value;
    }

    [[nodiscard]] int count(int maximum, const char *what)
    {
        const auto value = read<std::int32_t>();
        if (value < 0 || value > maximum)
            throw std::runtime_error{std::string{"invalid retail character "} + what};
        return value;
    }

    // BinaryWriter.Write(string): 7-bit encoded byte length, then UTF-8.
    [[nodiscard]] std::string string()
    {
        std::uint32_t length{};
        for (int shift = 0; shift < 35; shift += 7)
        {
            const auto byte = read<std::uint8_t>();
            length |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U)
            {
                if (length > 4096U)
                    break;
                std::string result(length, '\0');
                bytes(result.data(), result.size());
                return result;
            }
        }
        throw std::runtime_error{"invalid retail character string"};
    }

    [[nodiscard]] Matrix matrix()
    {
        Matrix result{};
        bytes(result.data(), sizeof(float) * result.size());
        return result;
    }

    [[nodiscard]] Vec3 vec3()
    {
        Vec3 result;
        result.x = read<float>();
        result.y = read<float>();
        result.z = read<float>();
        return result;
    }

    [[nodiscard]] Quat quat()
    {
        Quat result;
        result.x = read<float>();
        result.y = read<float>();
        result.z = read<float>();
        result.w = read<float>();
        return result;
    }

  private:
    std::ifstream stream_;
    std::size_t size_{};
    std::size_t position_{};
};

// Matrix.CreateFromQuaternion.
Matrix from_quaternion(const Quat &q) noexcept
{
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float zw = q.z * q.w;
    const float zx = q.z * q.x;
    const float yw = q.y * q.w;
    const float yz = q.y * q.z;
    const float xw = q.x * q.w;
    Matrix m{};
    m[0] = 1.0F - 2.0F * (yy + zz);
    m[1] = 2.0F * (xy + zw);
    m[2] = 2.0F * (zx - yw);
    m[4] = 2.0F * (xy - zw);
    m[5] = 1.0F - 2.0F * (zz + xx);
    m[6] = 2.0F * (yz + xw);
    m[8] = 2.0F * (zx + yw);
    m[9] = 2.0F * (yz - xw);
    m[10] = 1.0F - 2.0F * (yy + xx);
    m[15] = 1.0F;
    return m;
}

std::array<float, 3> normalized(std::array<float, 3> v) noexcept
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float scale = 1.0F / length;
    return {v[0] * scale, v[1] * scale, v[2] * scale};
}

std::array<float, 3> cross(const std::array<float, 3> &a, const std::array<float, 3> &b) noexcept
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

template <typename Key, typename Interpolate>
Key sample_keys(const std::vector<Key> &keys, int index, float amount, Interpolate interpolate) noexcept
{
    const int length = static_cast<int>(keys.size());
    if (index >= length)
        return keys[static_cast<std::size_t>(length - 1)];
    Key value = keys[static_cast<std::size_t>(index)];
    // The original tests `index < Length - 2`, not `Length - 1`: the last key
    // interval is never interpolated, it holds and then snaps.
    if (index < length - 2)
        value = interpolate(value, keys[static_cast<std::size_t>(index + 1)], amount);
    return value;
}

} // namespace

Matrix identity() noexcept
{
    Matrix m{};
    m[0] = m[5] = m[10] = m[15] = 1.0F;
    return m;
}

Matrix multiply(const Matrix &a, const Matrix &b) noexcept
{
    Matrix m;
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
            m[row * 4U + column] = a[row * 4U] * b[column] + a[row * 4U + 1U] * b[4U + column] +
                                   a[row * 4U + 2U] * b[8U + column] + a[row * 4U + 3U] * b[12U + column];
    return m;
}

Matrix invert(const Matrix &matrix) noexcept
{
    std::array<std::array<double, 8>, 4> rows{};
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
        {
            rows[row][column] = matrix[row * 4U + column];
            rows[row][column + 4U] = row == column ? 1.0 : 0.0;
        }
    for (std::size_t column = 0; column < 4U; ++column)
    {
        std::size_t pivot = column;
        for (std::size_t row = column + 1U; row < 4U; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
                pivot = row;
        if (std::abs(rows[pivot][column]) < 1.0e-30)
            return identity();
        std::swap(rows[pivot], rows[column]);
        const double divisor = rows[column][column];
        for (double &value : rows[column])
            value /= divisor;
        for (std::size_t row = 0; row < 4U; ++row)
        {
            if (row == column)
                continue;
            const double factor = rows[row][column];
            for (std::size_t element = 0; element < 8U; ++element)
                rows[row][element] -= factor * rows[column][element];
        }
    }
    Matrix result{};
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
            result[row * 4U + column] = static_cast<float>(rows[row][column + 4U]);
    return result;
}

Matrix compose(const BoneTransform &transform) noexcept
{
    Matrix m = from_quaternion(transform.rotation);
    const Vec3 &s = transform.scale;
    if (!(s.x == 1.0F && s.y == 1.0F && s.z == 1.0F))
    {
        // Matrix.CreateScale(scale) * Matrix.CreateFromQuaternion(rotation).
        for (std::size_t column = 0; column < 3U; ++column)
        {
            m[column] *= s.x;
            m[4U + column] *= s.y;
            m[8U + column] *= s.z;
        }
    }
    m[12] = transform.translation.x;
    m[13] = transform.translation.y;
    m[14] = transform.translation.z;
    return m;
}

// Quaternion.Slerp (XNA 4.0): shortest arc, linear only above 0.999999, and no
// renormalisation of the result.
Quat slerp(const Quat &a, const Quat &b, float amount) noexcept
{
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    bool flip = false;
    if (cosine < 0.0F)
    {
        flip = true;
        cosine = -cosine;
    }
    float first{};
    float second{};
    if (cosine > 0.999999F)
    {
        first = 1.0F - amount;
        second = flip ? -amount : amount;
    }
    else
    {
        const float angle = static_cast<float>(std::acos(static_cast<double>(cosine)));
        const float inverse_sine = static_cast<float>(1.0 / std::sin(static_cast<double>(angle)));
        first = static_cast<float>(std::sin(static_cast<double>((1.0F - amount) * angle))) * inverse_sine;
        second = flip ? static_cast<float>(-std::sin(static_cast<double>(amount * angle))) * inverse_sine
                      : static_cast<float>(std::sin(static_cast<double>(amount * angle))) * inverse_sine;
    }
    return {first * a.x + second * b.x, first * a.y + second * b.y, first * a.z + second * b.z,
            first * a.w + second * b.w};
}

Vec3 lerp(const Vec3 &a, const Vec3 &b, float amount) noexcept
{
    return {a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount, a.z + (b.z - a.z) * amount};
}

std::array<float, 3> transform_point(const float *p, const Matrix &m) noexcept
{
    return {p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
            p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14]};
}

std::array<float, 3> transform_vector(const float *p, const Matrix &m) noexcept
{
    return {p[0] * m[0] + p[1] * m[4] + p[2] * m[8], p[0] * m[1] + p[1] * m[5] + p[2] * m[9],
            p[0] * m[2] + p[1] * m[6] + p[2] * m[10]};
}

std::int64_t ticks_from_seconds(double seconds) noexcept
{
    // TimeSpan.Interval(value, 1000): (long)(value * 1000 + 0.5) milliseconds.
    const double milliseconds = seconds * 1000.0 + (seconds >= 0.0 ? 0.5 : -0.5);
    return static_cast<std::int64_t>(milliseconds) * 10'000;
}

double seconds_from_ticks(std::int64_t ticks) noexcept
{
    // TimeSpan.TotalSeconds: ticks * (1.0 / TicksPerSecond).
    return static_cast<double>(ticks) * 1.0e-7;
}

void Clip::copy_transforms(BonePose &pose, std::int64_t position_ticks, const BoneMask *influence) const noexcept
{
    const float frame = static_cast<float>(static_cast<double>(frame_rate) * seconds_from_ticks(position_ticks));
    const int index = static_cast<int>(frame);
    const float amount = frame - static_cast<float>(index);
    for (std::size_t bone = 0; bone < pose.size(); ++bone)
    {
        if (influence != nullptr && !(*influence)[bone])
            continue;
        if (positions[bone].empty() || rotations[bone].empty() || scales[bone].empty())
            continue;
        pose[bone].rotation = sample_keys(rotations[bone], index, amount, slerp);
        pose[bone].translation = sample_keys(positions[bone], index, amount, lerp);
        pose[bone].scale = sample_keys(scales[bone], index, amount, lerp);
    }
}

RetailRig::RetailRig(const std::filesystem::path &path)
{
    Reader reader{path};
    if (reader.read<std::uint32_t>() != 0x43524D4FU)
        throw std::runtime_error{"not a retail character file"};
    if (reader.read<std::int32_t>() != 1)
        throw std::runtime_error{"unsupported retail character version; re-import the content pack"};
    generation = reader.string();
    game_version = reader.string();
    graphics_profile = reader.string();
    body_asset = reader.string();

    if (reader.count(1024, "attachment bone count") != attachment_bone_count)
        throw std::runtime_error{"retail character does not carry the 71-bone attachment skeleton"};
    for (int index = 0; index < attachment_bone_count; ++index)
    {
        AttachmentBone &bone = attachment[static_cast<std::size_t>(index)];
        bone.name = reader.string();
        bone.native_name = reader.string();
        bone.parent = reader.read<std::int32_t>();
        if (bone.parent >= index || bone.parent < -1 || (index > 0 && bone.parent < 0))
            throw std::runtime_error{"retail attachment skeleton is not parent-first"};
        bone.bind = reader.matrix();
        bone.initial.translation = reader.vec3();
        bone.initial.rotation = reader.quat();
        bone.initial.scale = reader.vec3();
    }

    const int body_bones = reader.count(255, "body bone count");
    if (body_bones == 0)
        throw std::runtime_error{"retail character has no body skeleton"};
    body.resize(static_cast<std::size_t>(body_bones));
    for (BodyBone &bone : body)
    {
        bone.name = reader.string();
        bone.parent = reader.read<std::int32_t>();
        bone.source = reader.read<std::int32_t>();
        if (bone.parent < -1 || bone.parent >= body_bones || bone.source < 0 || bone.source >= attachment_bone_count)
            throw std::runtime_error{"invalid retail body bone"};
        bone.bind_local = reader.matrix();
        bone.inverse_bind = reader.matrix();
    }
    // Skeleton.CopyAbsoluteBoneTransformsTo walks from the roots; fix an order
    // in which every parent precedes its children.
    std::vector<bool> placed(body.size(), false);
    while (body_order.size() < body.size())
    {
        const std::size_t before = body_order.size();
        for (std::size_t index = 0; index < body.size(); ++index)
        {
            if (placed[index])
                continue;
            const int parent = body[index].parent;
            if (parent < 0 || placed[static_cast<std::size_t>(parent)])
            {
                placed[index] = true;
                body_order.push_back(static_cast<int>(index));
            }
        }
        if (body_order.size() == before)
            throw std::runtime_error{"retail body skeleton has a parent cycle"};
    }
    root_adjust = reader.matrix();
    eye_to_head = reader.matrix();

    const int part_count = reader.count(64, "mesh part count");
    parts.resize(static_cast<std::size_t>(part_count));
    for (MeshPart &part : parts)
    {
        part.name = reader.string();
        part.texture_width = reader.count(8192, "texture width");
        part.texture_height = reader.count(8192, "texture height");
        part.rgba.resize(static_cast<std::size_t>(part.texture_width) * static_cast<std::size_t>(part.texture_height) * 4U);
        if (!part.rgba.empty())
            reader.bytes(part.rgba.data(), part.rgba.size());
        part.vertices.resize(static_cast<std::size_t>(reader.count(1'000'000, "vertex count")));
        for (MeshVertex &vertex : part.vertices)
        {
            reader.bytes(vertex.position.data(), sizeof(float) * 3U);
            reader.bytes(vertex.normal.data(), sizeof(float) * 3U);
            reader.bytes(vertex.uv.data(), sizeof(float) * 2U);
            reader.bytes(vertex.bones.data(), 4U);
            reader.bytes(vertex.weights.data(), sizeof(float) * 4U);
            (void)reader.read<std::uint32_t>(); // vertex colour: white on every shipped body
            for (const std::uint8_t bone : vertex.bones)
                if (bone >= body.size())
                    throw std::runtime_error{"retail body vertex names a missing bone"};
        }
        part.indices.resize(static_cast<std::size_t>(reader.count(6'000'000, "index count")));
        if (part.indices.size() % 3U != 0U)
            throw std::runtime_error{"retail body mesh is not a triangle list"};
        for (std::uint32_t &index : part.indices)
        {
            index = reader.read<std::uint32_t>();
            if (index >= part.vertices.size())
                throw std::runtime_error{"retail body triangle names a missing vertex"};
        }
    }

    const int clip_count = reader.count(4096, "clip count");
    clips.resize(static_cast<std::size_t>(clip_count));
    for (Clip &clip : clips)
    {
        clip.asset = reader.string();
        clip.frame_rate = reader.read<std::int32_t>();
        clip.duration_ticks = reader.read<std::int64_t>();
        if (reader.count(1024, "clip bone count") != attachment_bone_count || clip.frame_rate <= 0 ||
            clip.duration_ticks < 0)
            throw std::runtime_error{"unsupported retail clip"};
        for (std::size_t bone = 0; bone < static_cast<std::size_t>(attachment_bone_count); ++bone)
        {
            clip.positions[bone].resize(static_cast<std::size_t>(reader.count(1'000'000, "key count")));
            for (Vec3 &key : clip.positions[bone])
                key = reader.vec3();
            clip.rotations[bone].resize(static_cast<std::size_t>(reader.count(1'000'000, "key count")));
            for (Quat &key : clip.rotations[bone])
                key = reader.quat();
            clip.scales[bone].resize(static_cast<std::size_t>(reader.count(1'000'000, "key count")));
            for (Vec3 &key : clip.scales[bone])
                key = reader.vec3();
            if (clip.positions[bone].empty() || clip.rotations[bone].empty() || clip.scales[bone].empty())
                throw std::runtime_error{"retail clip has a bone without keys"};
        }
    }

    const int registration_count = reader.count(4096, "registration count");
    registrations.resize(static_cast<std::size_t>(registration_count));
    for (Registration &registration : registrations)
    {
        registration.name = reader.string();
        registration.clip = reader.read<std::int32_t>();
        if (registration.clip < 0 || registration.clip >= clip_count)
            throw std::runtime_error{"retail registration names a missing clip"};
        registration.looping = reader.read<std::uint8_t>() != 0U;
        for (bool &flag : registration.influence)
            flag = reader.read<std::uint8_t>() != 0U;
    }

    const int item_count = reader.count(4096, "held item count");
    items.resize(static_cast<std::size_t>(item_count));
    for (HeldItem &item : items)
    {
        item.id = reader.read<std::int32_t>();
        item.name = reader.string();
        item.player_mode = reader.string();
        item.entity = reader.string();
        item.hand = reader.matrix();
        item.hand_remote = reader.matrix();
    }
}

const Registration *RetailRig::find(std::string_view name) const noexcept
{
    for (const Registration &registration : registrations)
        if (registration.name == name)
            return &registration;
    return nullptr;
}

const HeldItem *RetailRig::item(int id) const noexcept
{
    for (const HeldItem &item : items)
        if (item.id == id)
            return &item;
    return nullptr;
}

std::size_t RetailRig::triangle_count() const noexcept
{
    std::size_t count{};
    for (const MeshPart &part : parts)
        count += part.indices.size() / 3U;
    return count;
}

// ----------------------------------------------------------------- ClipPlayer

std::string_view ClipPlayer::name() const noexcept
{
    return registration_ == nullptr ? std::string_view{} : std::string_view{registration_->name};
}

std::int64_t ClipPlayer::duration_ticks() const noexcept
{
    return clip_ == nullptr ? 0 : clip_->duration_ticks;
}

double ClipPlayer::duration_seconds() const noexcept
{
    return seconds_from_ticks(duration_ticks());
}

float ClipPlayer::progress() const noexcept
{
    return static_cast<float>(seconds_from_ticks(current_ticks_) / duration_seconds());
}

void ClipPlayer::set_progress(float value) noexcept
{
    on_pong_ = false;
    current_ticks_ = ticks_from_seconds(static_cast<double>(value) * duration_seconds());
}

bool ClipPlayer::finished() const noexcept
{
    if (looping)
        return false;
    if (ping_pong && on_pong_)
        return progress() <= 0.0F;
    return progress() >= 1.0F;
}

std::int64_t ClipPlayer::position_ticks() const noexcept
{
    return reversed ? duration_ticks() - current_ticks_ : current_ticks_;
}

void ClipPlayer::set_clip(const Registration &registration, const Clip &clip) noexcept
{
    // AnimationPlayer.SetClip, then AvatarAnimationManager.GetAnimation's
    // assignments. Whether the player is paused is deliberately left alone.
    on_pong_ = false;
    reversed = false;
    ping_pong = false;
    current_ticks_ = 0;
    speed = 1.0F;
    clip_ = &clip;
    registration_ = &registration;
    looping = registration.looping;
    influence_ = registration.influence;
}

void ClipPlayer::advance(std::int64_t elapsed_ticks) noexcept
{
    if (clip_ == nullptr)
        return;
    const std::int64_t step = ticks_from_seconds(seconds_from_ticks(elapsed_ticks) * static_cast<double>(speed));
    if (playing_)
        current_ticks_ += on_pong_ ? -step : step;
    const std::int64_t duration = clip_->duration_ticks;
    if (current_ticks_ > duration)
    {
        if (ping_pong)
        {
            current_ticks_ = duration - (current_ticks_ - duration);
            on_pong_ = true;
        }
        else if (looping)
        {
            if (duration <= 0)
                current_ticks_ = 0;
            while (current_ticks_ > duration)
                current_ticks_ -= duration;
        }
        else
            current_ticks_ = duration;
    }
    else if (current_ticks_ < 0)
    {
        if (ping_pong)
        {
            if (looping)
            {
                on_pong_ = false;
                current_ticks_ = -current_ticks_;
            }
            else
                current_ticks_ = 0;
        }
        else if (looping)
        {
            if (duration <= 0)
                current_ticks_ = 0;
            while (current_ticks_ < 0)
                current_ticks_ += duration;
        }
        else
            current_ticks_ = 0;
    }
}

// ------------------------------------------------------------------- Animator

Animator::Animator(const RetailRig &rig) : rig_(&rig) {}

ClipPlayer *Animator::play_ticks(std::string_view name, int channel, std::int64_t blend_ticks)
{
    if (channel < 0 || channel >= channel_count)
        return nullptr;
    const Registration *registration = rig_->find(name);
    if (registration == nullptr)
        return nullptr;
    const auto index = static_cast<std::size_t>(channel);
    const auto slot = static_cast<std::size_t>(next_player_[index]);
    ClipPlayer &player = players_[index][slot];
    if (!created_[index][slot])
    {
        player = ClipPlayer{};
        created_[index][slot] = true;
    }
    player.set_clip(*registration, rig_->clips[static_cast<std::size_t>(registration->clip)]);
    next_player_[index] = (next_player_[index] + 1) % 3;
    replay_ticks(channel, &player, blend_ticks);
    return &player;
}

void Animator::replay_ticks(int channel, ClipPlayer *player, std::int64_t blend_ticks) noexcept
{
    if (channel < 0 || channel >= channel_count)
        return;
    // AnimBlender.Play.
    Blender &blender = blenders_[static_cast<std::size_t>(channel)];
    blender.previous = blender.current;
    blender.current = player;
    blender.blend_total_ticks = blend_ticks;
    blender.blend_current_ticks = 0;
}

ClipPlayer *Animator::play(std::string_view name, int channel, double blend_seconds)
{
    return play_ticks(name, channel, ticks_from_seconds(blend_seconds));
}

void Animator::clear(int channel, double blend_seconds)
{
    replay_ticks(channel, nullptr, ticks_from_seconds(blend_seconds));
}

void Animator::replay(int channel, ClipPlayer *player, double blend_seconds)
{
    replay_ticks(channel, player, ticks_from_seconds(blend_seconds));
}

ClipPlayer *Animator::active(int channel) noexcept
{
    return channel < 0 || channel >= channel_count ? nullptr : blenders_[static_cast<std::size_t>(channel)].current;
}

const ClipPlayer *Animator::active(int channel) const noexcept
{
    return channel < 0 || channel >= channel_count ? nullptr : blenders_[static_cast<std::size_t>(channel)].current;
}

bool Animator::is(int channel, std::string_view name) const noexcept
{
    const ClipPlayer *player = active(channel);
    return player != nullptr && player->name() == name;
}

void Animator::set_blend_elapsed_ticks(int channel, std::int64_t elapsed_ticks) noexcept
{
    if (channel >= 0 && channel < channel_count)
        blenders_[static_cast<std::size_t>(channel)].blend_current_ticks = elapsed_ticks;
}

ChannelStates Animator::update(std::int64_t elapsed_ticks) noexcept
{
    ChannelStates states{};
    for (std::size_t channel = 0; channel < blenders_.size(); ++channel)
    {
        // AnimBlender.Update.
        Blender &blender = blenders_[channel];
        if (blender.current == nullptr && blender.previous == nullptr)
            continue;
        if (blender.current != nullptr)
            blender.current->advance(elapsed_ticks);
        // A player replayed onto its own channel is both: it advances twice.
        if (blender.previous != nullptr)
            blender.previous->advance(elapsed_ticks);
        blender.blend_current_ticks += elapsed_ticks;
        float blend = 1.0F;
        if (seconds_from_ticks(blender.blend_total_ticks) > 0.0)
            blend = static_cast<float>(seconds_from_ticks(blender.blend_current_ticks) /
                                       seconds_from_ticks(blender.blend_total_ticks));
        ChannelState &state = states[channel];
        if (blender.current != nullptr)
        {
            state.current = blender.current->registration();
            state.current_position = blender.current->position_ticks();
        }
        if (blend >= 1.0F)
        {
            blender.previous = nullptr;
            state.blend = 1.0F;
            continue;
        }
        state.blend = blend;
        if (blender.previous != nullptr)
        {
            state.previous = blender.previous->registration();
            state.previous_position = blender.previous->position_ticks();
        }
    }
    return states;
}

// -------------------------------------------------------------- CharacterPose

CharacterPose::CharacterPose(const RetailRig &rig) : animations(rig), rig_(&rig)
{
    for (std::size_t bone = 0; bone < skeleton_.size(); ++bone)
        skeleton_[bone] = rig.attachment[bone].initial;
    body_world_.assign(rig.body.size(), identity());
    skin_.assign(rig.body.size(), identity());
    // Avatar.InitalizeParts -> GetAvatarPart: the parts exist at the bind pose
    // before the first update, and the first eye camera is built from them.
    pose_from_skeleton();
    eye_ = eye_from(attachment_[bone_head]);
}

void CharacterPose::update(std::int64_t elapsed_ticks)
{
    apply(animations.update(elapsed_ticks));
}

void CharacterPose::update_seconds(double elapsed_seconds)
{
    // XNA hands the game a TimeSpan: whole 100 ns ticks.
    update(static_cast<std::int64_t>(std::llround(elapsed_seconds * static_cast<double>(ticks_per_second))));
}

void CharacterPose::apply(const ChannelStates &channels)
{
    BonePose current{};
    BonePose previous{};
    for (const ChannelState &channel : channels)
    {
        if (channel.current == nullptr && channel.previous == nullptr)
            continue;
        if (channel.current != nullptr)
            rig_->clips[static_cast<std::size_t>(channel.current->clip)].copy_transforms(
                current, channel.current_position, &channel.current->influence);
        if (channel.blend >= 1.0F)
        {
            if (channel.current == nullptr)
                continue;
            for (std::size_t bone = 0; bone < skeleton_.size(); ++bone)
                if (channel.current->influence[bone])
                    skeleton_[bone] = current[bone];
            continue;
        }
        if (channel.previous != nullptr)
            rig_->clips[static_cast<std::size_t>(channel.previous->clip)].copy_transforms(
                previous, channel.previous_position, &channel.previous->influence);
        const float amount = channel.blend;
        for (std::size_t bone = 0; bone < skeleton_.size(); ++bone)
        {
            const bool from_previous = channel.previous != nullptr && channel.previous->influence[bone];
            const bool to_current = channel.current != nullptr && channel.current->influence[bone];
            BoneTransform &target = skeleton_[bone];
            if (from_previous && to_current)
            {
                target.translation = lerp(previous[bone].translation, current[bone].translation, amount);
                target.rotation = slerp(previous[bone].rotation, current[bone].rotation, amount);
                target.scale = lerp(previous[bone].scale, current[bone].scale, amount);
            }
            else if (from_previous)
            {
                // Fading out: toward whatever the lower channels wrote.
                target.translation = lerp(previous[bone].translation, target.translation, amount);
                target.rotation = slerp(previous[bone].rotation, target.rotation, amount);
                target.scale = lerp(previous[bone].scale, target.scale, amount);
            }
            else if (to_current)
            {
                target.translation = lerp(target.translation, current[bone].translation, amount);
                target.rotation = slerp(target.rotation, current[bone].rotation, amount);
                target.scale = lerp(target.scale, current[bone].scale, amount);
            }
        }
    }

    // Avatar.OnUpdate reads the head part BEFORE UpdateParts refreshes it.
    eye_ = eye_from(attachment_[bone_head]);
    if (hide_head)
        skeleton_[bone_head].scale = {0.001F, 0.001F, 0.001F};
    pose_from_skeleton();
}

Matrix CharacterPose::eye_now() const noexcept
{
    return eye_from(attachment_[bone_head]);
}

Matrix CharacterPose::eye_from(const Matrix &head) const noexcept
{
    // Matrix.CreateWorld(translation, normalised Forward, normalised Up).
    const std::array<float, 3> forward = normalized({-head[8], -head[9], -head[10]});
    const std::array<float, 3> up = normalized({head[4], head[5], head[6]});
    const std::array<float, 3> z = normalized({-forward[0], -forward[1], -forward[2]});
    const std::array<float, 3> x = normalized(cross(up, z));
    const std::array<float, 3> y = cross(z, x);
    Matrix world{};
    world[0] = x[0]; world[1] = x[1]; world[2] = x[2];
    world[4] = y[0]; world[5] = y[1]; world[6] = y[2];
    world[8] = z[0]; world[9] = z[1]; world[10] = z[2];
    world[12] = head[12]; world[13] = head[13]; world[14] = head[14];
    world[15] = 1.0F;
    return multiply(rig_->eye_to_head, world);
}

void CharacterPose::pose_from_skeleton()
{
    std::array<Matrix, attachment_bone_count> local;
    for (std::size_t bone = 0; bone < local.size(); ++bone)
        local[bone] = compose(skeleton_[bone]);

    // The body: Avatar.OnUpdate writes its default pose, ModelEntity.OnUpdate
    // assumes it and accumulates world transforms, SkinnedModelEntity skins.
    for (const int index : rig_->body_order)
    {
        const auto bone = static_cast<std::size_t>(index);
        const BodyBone &body = rig_->body[bone];
        Matrix pose;
        if (bone == 0U)
            pose = multiply(local[0], rig_->root_adjust);
        else
        {
            pose = local[static_cast<std::size_t>(body.source)];
            pose[12] = body.bind_local[12];
            pose[13] = body.bind_local[13];
            pose[14] = body.bind_local[14];
        }
        body_world_[bone] = body.parent < 0 ? pose : multiply(pose, body_world_[static_cast<std::size_t>(body.parent)]);
        skin_[bone] = multiply(body.inverse_bind, body_world_[bone]);
    }

    // Avatar.UpdateParts, for every bone.
    attachment_[0] = multiply(local[0], rig_->attachment[0].bind);
    for (std::size_t bone = 1; bone < attachment_.size(); ++bone)
    {
        Matrix part = local[bone];
        const Matrix &bind = rig_->attachment[bone].bind;
        part[12] = bind[12];
        part[13] = bind[13];
        part[14] = bind[14];
        attachment_[bone] = multiply(part, attachment_[static_cast<std::size_t>(rig_->attachment[bone].parent)]);
    }
}

} // namespace openminerz::rig
