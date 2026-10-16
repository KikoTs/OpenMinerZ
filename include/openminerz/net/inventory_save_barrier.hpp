#pragma once

#include "openminerz/net/bootstrap_codec.hpp"

#include <array>

namespace openminerz::net
{
// Uses only original reliable-ordered messages. A matching host readback is
// required: a local send/echo or a different player's inventory is not an ACK.
class InventorySaveBarrier final
{
  public:
    InventorySaveBarrier(ProtocolVersion protocol, std::uint8_t player_gid,
                         PlayerInventoryPayload inventory);
    [[nodiscard]] std::optional<std::array<std::vector<std::uint8_t>, 2>> packets() const;
    [[nodiscard]] bool accepts(std::uint8_t sender_gid, std::span<const std::uint8_t> packet) const;

  private:
    ProtocolVersion protocol_;
    std::uint8_t player_gid_;
    PlayerInventoryPayload inventory_;
};
} // namespace openminerz::net
