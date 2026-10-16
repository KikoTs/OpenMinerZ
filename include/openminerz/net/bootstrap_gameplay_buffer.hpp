#pragma once

#include "openminerz/net/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace openminerz::net
{
struct DeferredGameplayPacket final
{
    std::uint8_t sender_gid{};
    std::vector<std::uint8_t> packet;
};

// Gameplay keeps arriving while the ordered bootstrap is in flight. Apply it
// after the snapshot is installed, in arrival order, instead of mutating the
// previous world and losing those changes at installation.
class BootstrapGameplayBuffer final
{
  public:
    [[nodiscard]] bool push(std::uint8_t gid, std::span<const std::uint8_t> packet)
    {
        if (packets_.size() >= 4096U || packet.size() > 4U * 1024U * 1024U - bytes_)
            return false;
        packets_.push_back({gid, {packet.begin(), packet.end()}});
        bytes_ += packet.size();
        return true;
    }
    [[nodiscard]] std::vector<DeferredGameplayPacket> take() noexcept
    {
        bytes_ = 0U;
        return std::exchange(packets_, {});
    }
    void clear() noexcept
    {
        packets_.clear();
        bytes_ = 0U;
    }
    void forget_player(ProtocolVersion protocol, std::uint8_t gid)
    {
        std::erase_if(packets_, [&](const auto &event) {
            DecodedPacket decoded;
            PacketError error{};
            if (event.sender_gid != gid || !decode_packet(protocol, event.packet, decoded, error) ||
                !decoded.message)
                return false;
            const auto name = decoded.message->name;
            if (!player_routed_message(name) && name != "PlayerExistsMessage" &&
                name != "SpawnEnemyMessage")
                return false;
            bytes_ -= event.packet.size();
            return true;
        });
    }

  private:
    std::vector<DeferredGameplayPacket> packets_;
    std::size_t bytes_{};
};
} // namespace openminerz::net
