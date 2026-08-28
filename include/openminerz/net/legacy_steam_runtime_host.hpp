#pragma once

#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/protocol_trace.hpp"
#include "openminerz/net/steam_bridge_client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::net
{

struct LegacySteamRuntimeHostOptions final
{
    HostSessionSettings settings;
    std::int32_t game_mode{1};
    std::int32_t difficulty{};
    bool infinite_resources{};
    std::optional<AuthoritativeBootstrapSnapshot> bootstrap_snapshot;
    std::filesystem::path helper_path;
    std::filesystem::path steam_api_path;
    std::optional<std::filesystem::path> trace_path;
};

enum class LegacySteamRuntimeEventKind : std::uint8_t
{
    peer_connected,
    peer_disconnected,
    gameplay,
    rejected,
    transport_failure
};

struct LegacySteamRuntimeEvent final
{
    LegacySteamRuntimeEventKind kind{LegacySteamRuntimeEventKind::gameplay};
    LegacySteamHostInbound inbound;
    LegacySteamAuditEvent audit;
    PeerId peer{};
    std::string detail;
};

[[nodiscard]] bool make_steam_bridge_host_config(const LegacySteamRuntimeHostOptions &options,
                                                 SteamBridgeHostConfig &config,
                                                 std::string &error) noexcept;

// The app-facing authority boundary. The x86 helper supplies only lobby-member
// wire datagrams; this class runs every stock envelope, approval, routing,
// application, and delivery check before emitting gameplay events.
class LegacySteamRuntimeHost final
{
  public:
    LegacySteamRuntimeHost() = default;
    ~LegacySteamRuntimeHost();

    LegacySteamRuntimeHost(const LegacySteamRuntimeHost &) = delete;
    LegacySteamRuntimeHost &operator=(const LegacySteamRuntimeHost &) = delete;
    LegacySteamRuntimeHost(LegacySteamRuntimeHost &&) noexcept;
    LegacySteamRuntimeHost &operator=(LegacySteamRuntimeHost &&) noexcept;

    [[nodiscard]] bool start(const LegacySteamRuntimeHostOptions &options,
                             SteamBridgeHostReady &ready, std::string &error);
    [[nodiscard]] bool pump(std::vector<LegacySteamRuntimeEvent> &events, std::string &error);
    [[nodiscard]] bool send_authoritative(const LegacySteamOutbound &outbound, std::string &error);
    [[nodiscard]] bool store_authoritative_inventory(PeerId peer,
                                                     const PlayerInventoryPayload &inventory);
    [[nodiscard]] bool apply_authoritative_crate_item(const ItemCratePayload &crate);
    [[nodiscard]] bool destroy_authoritative_crate(IntVector3 location);
    [[nodiscard]] bool set_authoritative_custom_door(IntVector3 location, std::uint8_t model_name);
    [[nodiscard]] bool destroy_authoritative_custom_block(IntVector3 location);
    [[nodiscard]] bool update_authoritative_spawner(IntVector3 location, bool started);
    [[nodiscard]] bool apply_authoritative_block_edit(IntVector3 position, std::uint8_t block_type);
    [[nodiscard]] bool apply_authoritative_block_replacements(std::span<const IntVector3> positions,
                                                              std::uint8_t block_type);
    [[nodiscard]] bool apply_authoritative_block_removals(std::span<const IntVector3> positions);
    [[nodiscard]] bool stop(std::string &error) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> gid_for(PeerId peer) const noexcept;
    [[nodiscard]] std::optional<PeerId> peer_for_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *gamer_for_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] bool gameplay_ready(PeerId peer, std::uint8_t player_gid) const noexcept;

  private:
    struct PendingPrePresenceInbound final
    {
        LegacySteamHostInbound inbound;
        std::string message_name;
    };

    [[nodiscard]] bool send_all(std::span<const LegacySteamNativeSend> sends, std::string &error);
    [[nodiscard]] bool queue_pre_presence(LegacySteamHostInbound inbound,
                                          std::string_view message_name);
    void replay_pre_presence(PeerId peer, std::vector<LegacySteamRuntimeEvent> &events);
    void forget_pre_presence(PeerId peer) noexcept;

    SteamBridgeClient bridge_;
    std::optional<LegacySteamHostSession> session_;
    std::optional<AuthoritativeBootstrapHost> bootstrap_;
    std::optional<ProtocolTraceWriter> trace_;
    ModManifest mod_manifest_;
    std::unordered_map<PeerId, std::vector<PendingPrePresenceInbound>> pre_presence_packets_;
    std::size_t pre_presence_bytes_{};
};

} // namespace openminerz::net
