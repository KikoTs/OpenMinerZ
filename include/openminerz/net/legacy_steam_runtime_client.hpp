#pragma once

#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/protocol_trace.hpp"
#include "openminerz/net/session_browser.hpp"
#include "openminerz/net/steam_bridge_client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

struct LegacySteamRuntimeClientOptions final
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    std::optional<ProtocolVersion> discovery_protocol;
    std::filesystem::path helper_path;
    std::filesystem::path steam_api_path;
    std::optional<std::filesystem::path> trace_path;
};

enum class LegacySteamRuntimeClientEventKind : std::uint8_t
{
    connected,
    peer_joined,
    peer_left,
    session_properties_changed,
    gameplay,
    rejected,
    transport_failure
};

struct LegacySteamRuntimeClientEvent final
{
    LegacySteamRuntimeClientEventKind kind{LegacySteamRuntimeClientEventKind::gameplay};
    std::uint8_t gid{};
    LegacyGamerIdentity gamer;
    std::vector<std::uint8_t> payload;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
    std::string detail;
};

[[nodiscard]] std::vector<DiscoveredSession> make_discovered_sessions(
    const SteamBridgeDiscoveryResults &results);

// App-facing stock client boundary. Discovery and lobby joining stay in the
// supervised x86 helper; all approval, GID, routing, typed payload, and
// delivery decisions stay in the x64 process.
class LegacySteamRuntimeClient final
{
  public:
    LegacySteamRuntimeClient() = default;
    ~LegacySteamRuntimeClient();

    LegacySteamRuntimeClient(const LegacySteamRuntimeClient &) = delete;
    LegacySteamRuntimeClient &operator=(const LegacySteamRuntimeClient &) = delete;
    LegacySteamRuntimeClient(LegacySteamRuntimeClient &&) noexcept;
    LegacySteamRuntimeClient &operator=(LegacySteamRuntimeClient &&) noexcept;

    [[nodiscard]] bool start_browser(const LegacySteamRuntimeClientOptions &options,
                                     SteamBridgeDiscoveryResults &results, std::string &error);
    [[nodiscard]] bool refresh(SteamBridgeDiscoveryResults &results, std::string &error);
    [[nodiscard]] bool join(std::uint64_t lobby_id, ProtocolVersion protocol,
                            const ModManifest &client_manifest, std::string password,
                            SteamBridgeJoinReady &ready, std::string &error);
    [[nodiscard]] bool pump(std::vector<LegacySteamRuntimeClientEvent> &events, std::string &error);
    [[nodiscard]] bool send(std::optional<std::uint8_t> recipient_gid,
                            std::span<const std::uint8_t> application_packet,
                            MessageDeliveryPolicy delivery, std::string &error);
    [[nodiscard]] bool stop(std::string &error) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> local_gid() const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *local_gamer() const noexcept;

  private:
    [[nodiscard]] bool send_native(const LegacySteamNativeSend &send, std::string &error);
    [[nodiscard]] bool negotiate_extension(const SteamBridgeJoinReady &ready,
                                           const ModManifest &client_manifest, std::string &error);

    SteamBridgeClient bridge_;
    std::optional<LegacySteamClientSession> session_;
    std::optional<LegacySteamRuntimeClientOptions> options_;
    std::optional<ProtocolTraceWriter> trace_;
    PeerId joined_host_peer_{};
};

} // namespace openminerz::net
