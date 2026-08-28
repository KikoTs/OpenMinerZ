#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/world/player.hpp"

#include <cstdint>

namespace openminerz::net
{

struct LocalPlayerReplicationInput final
{
    world::Vec3 position;
    world::Vec3 velocity;
    float yaw{};
    float torso_pitch{};
    float movement_x{};
    float movement_y{};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
};

struct RemotePlayerReplicationState final
{
    world::Vec3 position;
    world::Vec3 velocity;
    float yaw{};
    float torso_pitch{};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    float movement_x{};
    float movement_y{};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
};

[[nodiscard]] bool pack_half_single(float value, std::uint16_t &packed) noexcept;
[[nodiscard]] float unpack_half_single(std::uint16_t packed) noexcept;

[[nodiscard]] bool make_player_update(const LocalPlayerReplicationInput &input,
                                      ProtocolVersion protocol, PlayerUpdatePayload &payload,
                                      PayloadError &error);
[[nodiscard]] bool apply_player_update(const PlayerUpdatePayload &payload, ProtocolVersion protocol,
                                       RemotePlayerReplicationState &state);

[[nodiscard]] MessageDeliveryPolicy player_update_delivery(
    const PlayerUpdatePayload &payload) noexcept;

} // namespace openminerz::net
