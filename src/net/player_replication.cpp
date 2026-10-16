#include "openminerz/net/player_replication.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace openminerz::net
{
namespace
{

std::uint32_t rounded_shift(std::uint32_t value, unsigned shift) noexcept
{
    const std::uint32_t result = value >> shift;
    const std::uint32_t remainder_mask = (std::uint32_t{1} << shift) - 1U;
    const std::uint32_t remainder = value & remainder_mask;
    const std::uint32_t halfway = std::uint32_t{1} << (shift - 1U);
    return result +
           (remainder > halfway || (remainder == halfway && (result & 1U) != 0U) ? 1U : 0U);
}

std::uint8_t packed_angle(float value, float offset, float period) noexcept
{
    const float normalized = std::clamp((value + offset) / period, 0.0F, 1.0F);
    // PlayerUpdateMessage.SendData: (byte)Math.Round((double)(255f * num)). The
    // original is an x86 build, so that product is formed at double precision
    // and only then rounded half-to-even. A float product lands exactly on a
    // half (-60 degrees -> 42.5) where the original is just above it.
    return static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(normalized) * 255.0));
}

std::uint8_t packed_movement_component(float value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp((value + 1.0F) * 7.0F, 0.0F, 14.0F));
}

float squared_distance(const world::Vec3 &left, const world::Vec3 &right) noexcept
{
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    const float dz = left.z - right.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

bool pack_half_single(float value, std::uint16_t &packed) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFFU)
        return false;
    if (exponent == 0U)
    {
        packed = sign;
        return true;
    }

    int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31)
        return false;
    if (half_exponent <= 0)
    {
        if (half_exponent < -10)
        {
            packed = sign;
            return true;
        }
        const std::uint32_t normalized_mantissa = mantissa | 0x800000U;
        const unsigned shift = static_cast<unsigned>(14 - half_exponent);
        const std::uint32_t half_mantissa = rounded_shift(normalized_mantissa, shift);
        packed = static_cast<std::uint16_t>(sign | half_mantissa);
        return true;
    }

    std::uint32_t half_mantissa = rounded_shift(mantissa, 13U);
    if (half_mantissa == 0x400U)
    {
        half_mantissa = 0U;
        ++half_exponent;
        if (half_exponent >= 31)
            return false;
    }
    packed = static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(half_exponent) << 10U) |
                                        static_cast<std::uint16_t>(half_mantissa));
    return true;
}

float unpack_half_single(std::uint16_t packed) noexcept
{
    const bool negative = (packed & 0x8000U) != 0U;
    const std::uint16_t exponent = static_cast<std::uint16_t>((packed >> 10U) & 0x1FU);
    const std::uint16_t mantissa = static_cast<std::uint16_t>(packed & 0x03FFU);
    float value{};
    if (exponent == 0U)
        value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31U)
        value = mantissa == 0U ? std::numeric_limits<float>::infinity()
                               : std::numeric_limits<float>::quiet_NaN();
    else
        value = std::ldexp(1.0F + static_cast<float>(mantissa) / 1024.0F,
                           static_cast<int>(exponent) - 15);
    return negative ? -value : value;
}

bool make_player_update(const LocalPlayerReplicationInput &input, ProtocolVersion protocol,
                        PlayerUpdatePayload &payload, PayloadError &error)
{
    if (!std::isfinite(input.position.x) || !std::isfinite(input.position.y) ||
        !std::isfinite(input.position.z) || !std::isfinite(input.velocity.x) ||
        !std::isfinite(input.velocity.y) || !std::isfinite(input.velocity.z) ||
        !std::isfinite(input.yaw) || !std::isfinite(input.torso_pitch) ||
        !std::isfinite(input.movement_x) || !std::isfinite(input.movement_y))
    {
        error = PayloadError::non_finite;
        return false;
    }

    PlayerUpdatePayload converted;
    converted.local_position = {input.position.x, input.position.y, input.position.z};
    // Native camera yaw turns toward +X; XNA's positive Y rotation faces -X.
    float yaw_turns = -input.yaw / (2.0F * std::numbers::pi_v<float>);
    yaw_turns -= std::floor(yaw_turns);
    converted.yaw =
        static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(yaw_turns) * 255.0));
    converted.torso_pitch = packed_angle(input.torso_pitch, std::numbers::pi_v<float> * 0.5F,
                                         std::numbers::pi_v<float>);
    if (input.using_tool)
        converted.state_flags |= PlayerUpdatePayload::using_flag;
    if (input.dead)
        converted.state_flags |= PlayerUpdatePayload::dead_flag;
    if (input.shouldering)
        converted.state_flags |= PlayerUpdatePayload::shouldering_flag;
    if (input.reloading)
        converted.state_flags |= PlayerUpdatePayload::reloading_flag;
    if (input.throwing_grenade)
        converted.state_flags |= PlayerUpdatePayload::throwing_grenade_flag;
    if (input.ready_to_throw_grenade)
        converted.state_flags |= PlayerUpdatePayload::ready_to_throw_grenade_flag;
    converted.player_mode = static_cast<std::uint8_t>(input.animation_mode);
    const std::uint8_t movement_x = packed_movement_component(input.movement_x);
    const std::uint8_t movement_y = packed_movement_component(input.movement_y);
    converted.packed_movement = static_cast<std::uint8_t>((movement_y << 4U) | movement_x);
    if (!pack_half_single(input.velocity.x, converted.velocity_x) ||
        !pack_half_single(input.velocity.y, converted.velocity_y) ||
        !pack_half_single(input.velocity.z, converted.velocity_z))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    const auto wire = encode_player_update_payload(converted, protocol, error);
    if (!wire.has_value())
        return false;
    payload = converted;
    error = PayloadError::none;
    return true;
}

bool apply_player_update(const PlayerUpdatePayload &payload, ProtocolVersion protocol,
                         RemotePlayerReplicationState &state)
{
    PayloadError error{};
    if (!encode_player_update_payload(payload, protocol, error).has_value())
        return false;
    RemotePlayerReplicationState converted;
    converted.position = {payload.local_position.x, payload.local_position.y,
                          payload.local_position.z};
    converted.velocity = {unpack_half_single(payload.velocity_x),
                          unpack_half_single(payload.velocity_y),
                          unpack_half_single(payload.velocity_z)};
    converted.yaw = -static_cast<float>(payload.yaw) / 255.0F * (2.0F * std::numbers::pi_v<float>);
    converted.yaw = std::remainder(converted.yaw, 2.0F * std::numbers::pi_v<float>);
    converted.torso_pitch =
        static_cast<float>(payload.torso_pitch) / 255.0F * std::numbers::pi_v<float> -
        std::numbers::pi_v<float> * 0.5F;
    converted.animation_mode = static_cast<PlayerAnimationMode>(payload.player_mode);
    converted.movement_x =
        static_cast<float>(payload.packed_movement & 0x0FU) / 14.0F * 2.0F - 1.0F;
    converted.movement_y = static_cast<float>(payload.packed_movement >> 4U) / 14.0F * 2.0F - 1.0F;
    converted.using_tool = (payload.state_flags & PlayerUpdatePayload::using_flag) != 0U;
    converted.dead = (payload.state_flags & PlayerUpdatePayload::dead_flag) != 0U;
    converted.shouldering = (payload.state_flags & PlayerUpdatePayload::shouldering_flag) != 0U;
    converted.reloading = (payload.state_flags & PlayerUpdatePayload::reloading_flag) != 0U;
    converted.throwing_grenade =
        (payload.state_flags & PlayerUpdatePayload::throwing_grenade_flag) != 0U;
    converted.ready_to_throw_grenade =
        (payload.state_flags & PlayerUpdatePayload::ready_to_throw_grenade_flag) != 0U;
    state = converted;
    return true;
}

MessageDeliveryPolicy player_update_delivery(const PlayerUpdatePayload &payload) noexcept
{
    return (payload.state_flags &
            (PlayerUpdatePayload::using_flag | PlayerUpdatePayload::reloading_flag)) != 0U
               ? MessageDeliveryPolicy::reliable_ordered
               : MessageDeliveryPolicy::unreliable_sequenced;
}

void RemotePlayerPresentation::receive(const RemotePlayerReplicationState &state,
                                       float seconds) noexcept
{
    const auto visible = sample(seconds);
    const bool continuous = initialized_ && seconds >= received_seconds_ &&
                            seconds - received_seconds_ <= 0.5F && state.dead == latest_.dead &&
                            squared_distance(visible.position, state.position) <= 4.0F;
    // Measure how often this peer's packets actually arrive. Everything below
    // is expressed in that observed cadence instead of a fixed window.
    if (initialized_ && seconds > received_seconds_)
        interval_ = std::clamp(interval_ * 0.7F + (seconds - received_seconds_) * 0.3F,
                               minimum_interval, maximum_interval);
    // Re-stamping arrivals onto the measured cadence was tried and measured
    // worse: a packet stamped slightly in the future stops the body until its
    // own timestamp is reached. The arrival instant stays the baseline.
    correction_ = continuous && !state.dead
                      ? world::Vec3{visible.position.x - state.position.x,
                                    visible.position.y - state.position.y,
                                    visible.position.z - state.position.z}
                      : world::Vec3{};
    yaw_correction_ =
        continuous ? std::remainder(visible.yaw - state.yaw, 2.0F * std::numbers::pi_v<float>)
                   : 0.0F;
    pitch_correction_ = continuous ? visible.torso_pitch - state.torso_pitch : 0.0F;
    latest_ = state;
    received_seconds_ = seconds;
    initialized_ = true;
}

RemotePlayerReplicationState RemotePlayerPresentation::sample(float seconds) const noexcept
{
    auto result = latest_;
    // Extrapolate across one missed packet plus the arrival jitter, never
    // further. Stopping at a flat 100 ms froze the body for the rest of a late
    // gap and then had to catch up, which was the largest single source of
    // visible judder on an impaired link.
    const float horizon = std::clamp(interval_ + 0.1F, 0.1F, maximum_horizon);
    const float elapsed = std::max(seconds - received_seconds_, 0.0F);
    const float age = std::min(elapsed, horizon);
    const float smoothing = std::clamp(2.0F * interval_, 0.1F, maximum_horizon);
    const float correction_weight = 1.0F - std::min(elapsed, smoothing) / smoothing;
    const float prediction = latest_.dead ? 0.0F : age;
    result.position.x += latest_.velocity.x * prediction + correction_.x * correction_weight;
    result.position.y += latest_.velocity.y * prediction + correction_.y * correction_weight;
    result.position.z += latest_.velocity.z * prediction + correction_.z * correction_weight;
    result.yaw += yaw_correction_ * correction_weight;
    result.torso_pitch += pitch_correction_ * correction_weight;
    return result;
}

} // namespace openminerz::net
