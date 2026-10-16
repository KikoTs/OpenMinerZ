#pragma once

#include "openminerz/net/steam_bridge_protocol.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace openminerz::net
{

struct SteamBridgeClientOptions final
{
    std::filesystem::path helper_path;
    std::filesystem::path steam_api_path;
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds host_timeout{20'000};
    std::chrono::milliseconds discovery_timeout{20'000};
    std::chrono::milliseconds join_timeout{20'000};
};

enum class SteamBridgeClientEventKind : std::uint8_t
{
    inbound_wire,
    peer_left,
    failure,
    heartbeat,
    stopped,
    // Host mode: `peer` entered the lobby and `peer_name` is its Steam persona.
    peer_entered
};

struct SteamBridgeClientEvent final
{
    SteamBridgeClientEventKind kind{SteamBridgeClientEventKind::heartbeat};
    SteamBridgeInboundWire inbound;
    PeerId peer{};
    SteamBridgeFailure failure;
    std::string peer_name;
};

// What one IPC round trip through the supervised x86 helper actually costs.
// Measured against the real named pipe and the real helper loop in
// `--protocol-test` mode, which never initializes Steam or creates a lobby.
struct SteamBridgeIpcProbe final
{
    double round_trip_p50_ms{};
    double round_trip_p95_ms{};
    double round_trip_maximum_ms{};
    // Frames per second when a batch is pipelined instead of ping-ponged: the
    // ceiling on host relay datagrams the helper can forward.
    double burst_frames_per_second{};
    std::size_t samples{};
};

class SteamBridgeClient final
{
  public:
    SteamBridgeClient();
    ~SteamBridgeClient();

    SteamBridgeClient(const SteamBridgeClient &) = delete;
    SteamBridgeClient &operator=(const SteamBridgeClient &) = delete;
    SteamBridgeClient(SteamBridgeClient &&) noexcept;
    SteamBridgeClient &operator=(SteamBridgeClient &&) noexcept;

    [[nodiscard]] static bool protocol_self_test(const std::filesystem::path &helper_path,
                                                 std::chrono::milliseconds timeout,
                                                 std::string &error);
    // Offline latency/throughput certificate for the game-to-helper transport.
    [[nodiscard]] static bool ipc_probe(const std::filesystem::path &helper_path,
                                        std::size_t samples, SteamBridgeIpcProbe &probe,
                                        std::string &error);

    [[nodiscard]] bool start_host(const SteamBridgeClientOptions &options,
                                  const SteamBridgeHostConfig &config, SteamBridgeHostReady &ready,
                                  std::string &error);
    [[nodiscard]] bool start_discovery(const SteamBridgeClientOptions &options,
                                       const SteamBridgeDiscoveryRequest &request,
                                       SteamBridgeDiscoveryResults &results, std::string &error);
    [[nodiscard]] bool refresh_discovery(const SteamBridgeDiscoveryRequest &request,
                                         SteamBridgeDiscoveryResults &results, std::string &error);
    [[nodiscard]] bool join(const SteamBridgeJoinRequest &request, SteamBridgeJoinReady &ready,
                            std::string &error);
    [[nodiscard]] bool send(const SteamBridgeOutboundWire &datagram, std::string &error);
    [[nodiscard]] bool update_host(const SteamBridgeHostConfig &config, std::string &error);
    [[nodiscard]] bool poll(std::optional<SteamBridgeClientEvent> &event, std::string &error);
    [[nodiscard]] bool stop(std::string &error) noexcept;
    [[nodiscard]] bool running() const noexcept;

  private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace openminerz::net
