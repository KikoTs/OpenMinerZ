#pragma once

#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/transport.hpp"

#include <filesystem>
#include <optional>

namespace openminerz::net
{
// Private host-side records, keyed by admitted platform peer rather than session
// GID or gamer tag. The directory identifies the world; each record binds its
// protocol and peer and validates the complete original PINV payload.
[[nodiscard]] bool read_peer_inventory(const std::filesystem::path &directory,
                                       ProtocolVersion protocol, PeerId peer,
                                       std::optional<PlayerInventoryPayload> &inventory);
[[nodiscard]] bool write_peer_inventory(const std::filesystem::path &directory,
                                        ProtocolVersion protocol, PeerId peer,
                                        const PlayerInventoryPayload &inventory);
} // namespace openminerz::net
