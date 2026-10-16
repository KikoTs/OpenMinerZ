#pragma once

#include "openminerz/net/host_session.hpp"
#include "openminerz/net/transport.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace openminerz::net
{

enum class AdmissionStatus : std::uint8_t
{
    awaiting_extension,
    admitted,
    rejected_duplicate,
    rejected_full,
    rejected_platform_policy,
    rejected_password,
    rejected_session_properties,
    rejected_extension_required,
    rejected_malformed_extension,
    rejected_mod_manifest,
    disconnected
};

struct AdmissionDecision final
{
    AdmissionStatus status{AdmissionStatus::disconnected};
    std::string detail;

    [[nodiscard]] bool accepted() const noexcept
    {
        return status == AdmissionStatus::awaiting_extension || status == AdmissionStatus::admitted;
    }
};

struct PeerAdmissionRequest final
{
    PeerId peer{};
    SessionProperties properties{};
    std::string password;
    bool advertised_extension{};
    bool platform_authorized{true};
};

struct ExtensionAdmissionResult final
{
    AdmissionDecision decision;
    std::vector<std::uint8_t> reply;
};

class HostAdmissionGate final
{
  public:
    HostAdmissionGate(HostSessionSettings settings, bool join_closed, std::int32_t game_mode,
                      std::int32_t difficulty, bool infinite_resources,
                      bool host_occupies_player_slot = true);

    [[nodiscard]] AdmissionDecision begin(const PeerAdmissionRequest &request);
    [[nodiscard]] ExtensionAdmissionResult receive_extension(PeerId peer,
                                                             std::span<const std::uint8_t> packet);
    [[nodiscard]] bool may_forward_gameplay(PeerId peer) const noexcept;
    // (Network/mod channel) The session carries a server/core mod, so mod
    // channel frames exist at all.
    [[nodiscard]] bool mod_channel_enabled() const noexcept;
    // Mod channel frames may flow with this peer: the session has a mod
    // channel and the peer was admitted through the extension hello.
    [[nodiscard]] bool may_exchange_mod_frames(PeerId peer) const noexcept;
    // Download frames may come from a peer that advertised the extension and
    // has not finished its hello yet (or finished it).
    [[nodiscard]] bool may_download_mods(PeerId peer) const noexcept;
    [[nodiscard]] AdmissionStatus status(PeerId peer) const noexcept;
    void disconnect(PeerId peer) noexcept;

    [[nodiscard]] std::size_t reserved_peer_count() const noexcept;
    [[nodiscard]] std::size_t admitted_peer_count() const noexcept;
    [[nodiscard]] const SessionProperties &advertised_properties() const noexcept
    {
        return properties_;
    }

  private:
    struct PeerState final
    {
        AdmissionStatus status{AdmissionStatus::disconnected};
        // Admitted through the extension hello (never true for a stock peer).
        bool extension{};
    };

    [[nodiscard]] ExtensionAdmissionResult reject_extension(PeerId peer, std::uint64_t nonce,
                                                            AdmissionStatus status,
                                                            std::string detail);

    HostSessionSettings settings_;
    SessionProperties properties_;
    bool host_occupies_player_slot_{};
    std::unordered_map<PeerId, PeerState> peers_;
};

} // namespace openminerz::net
