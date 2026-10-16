#pragma once

#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/mod_download.hpp"
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
    transport_failure,
    // The Xbox Avatar add-on (openminerz/net/avatar_sync.hpp): `gid` announced
    // it and `payload` holds the one byte of its avatar message id.
    avatar_capability,
    // An add-on avatar message from `gid`. Never a stock message.
    avatar_sync,
    // (Network) One mod channel frame from the host (`payload`), only in a
    // session whose negotiated manifest carries a server/core mod.
    mod_channel
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
    // With `downloads` (Network), the mods this client presents are chosen
    // per session from `downloads->local` and the mod cache
    // (select_session_mods) instead of `client_manifest`, and any server mod
    // the host requires but this client lacks is offered for download before
    // the extension hello (net/mod_download.hpp). `downloads->selection`
    // receives the mods the session runs.
    [[nodiscard]] bool join(std::uint64_t lobby_id, ProtocolVersion protocol,
                            const ModManifest &client_manifest, std::string password,
                            SteamBridgeJoinReady &ready, std::string &error,
                            ModJoinDownloadContext *downloads = nullptr);
    [[nodiscard]] bool pump(std::vector<LegacySteamRuntimeClientEvent> &events, std::string &error);
    [[nodiscard]] bool send(std::optional<std::uint8_t> recipient_gid,
                            std::span<const std::uint8_t> application_packet,
                            MessageDeliveryPolicy delivery, std::string &error);
    // The Xbox Avatar add-on's extra message to one gamer. Fails, sending
    // nothing, unless that gamer announced the add-on in this session
    // (LegacySteamClientSession::create_avatar_outbound).
    [[nodiscard]] bool send_avatar(std::uint8_t recipient_gid,
                                   std::span<const std::uint8_t> avatar_packet, std::string &error);
    [[nodiscard]] bool avatar_capable(std::uint8_t gid) const noexcept;
    // Announces the add-on to everyone in the session: a duplicate, stock
    // PlayerExistsMessage from the local gamer whose avatar description ends in
    // the capability marker (net::make_avatar_capability_presence).
    [[nodiscard]] bool send_avatar_capability(std::string &error);
    [[nodiscard]] bool stop(std::string &error) noexcept;
    [[nodiscard]] bool save_inventory_and_wait(const PlayerInventoryPayload &inventory,
                                               std::string &error);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> local_gid() const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *local_gamer() const noexcept;
    // Whether this session's host accepted an OpenMinerZ extension hello. A
    // stock or OpenClassic host advertises no extension metadata, so the
    // handshake is skipped and this stays false: the host then relays every
    // peer's traffic unconditionally and each peer must apply the original's
    // own receipt rules for itself.
    [[nodiscard]] bool host_extension_capable() const noexcept;
    // (Network) The host accepted our hello for a manifest that carries a
    // server/core mod: mod channel frames flow both ways.
    [[nodiscard]] bool mod_channel_open() const noexcept;
    // (Network) One mod channel frame to the host. Fails, sending nothing,
    // unless mod_channel_open().
    [[nodiscard]] bool send_mod_frame(std::span<const std::uint8_t> frame, std::string &error);

  private:
    [[nodiscard]] bool send_native(const LegacySteamNativeSend &send, std::string &error);
    [[nodiscard]] bool negotiate_extension(const SteamBridgeJoinReady &ready,
                                           const ModManifest &client_manifest, std::string &error);
    [[nodiscard]] bool download_required_mods(const SteamBridgeJoinReady &ready,
                                              ProtocolVersion protocol, const std::string &password,
                                              ModJoinDownloadContext &downloads,
                                              ModManifest &client_manifest, std::string &error);

    SteamBridgeClient bridge_;
    std::optional<LegacySteamClientSession> session_;
    std::optional<LegacySteamRuntimeClientOptions> options_;
    std::optional<ProtocolTraceWriter> trace_;
    PeerId joined_host_peer_{};
    bool host_extension_capable_{};
    bool mod_channel_open_{};
};

} // namespace openminerz::net
