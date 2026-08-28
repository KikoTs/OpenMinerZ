#pragma once

#include "openminerz/net/admission.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::net
{

enum class DiscoverySource : std::uint8_t
{
    friend_session,
    public_lobby,
    lan,
    direct
};

enum class DiscoveryFilter : std::uint8_t
{
    all,
    friends,
    public_lobbies,
    lan,
    direct
};

struct DirectEndpoint final
{
    std::string host;
    std::uint16_t port{25343U};

    auto operator<=>(const DirectEndpoint &) const = default;
};

struct DiscoveredSession final
{
    std::uint64_t session_id{};
    std::string server_name;
    std::string host_name;
    std::string server_message;
    DiscoverySource source{DiscoverySource::public_lobby};
    SessionProperties properties{};
    std::uint8_t current_players{};
    std::uint8_t maximum_players{};
    bool password_required{};
    bool advertises_extension{};
    bool extension_metadata_valid{true};
    bool platform_authorized{true};
    bool favorite{};
    std::optional<std::uint32_t> ping_milliseconds;
    std::optional<ModManifest> required_manifest;
    std::optional<DirectEndpoint> endpoint;
};

enum class JoinCompatibility : std::uint8_t
{
    joinable,
    full,
    platform_restricted,
    malformed_advertisement,
    unsupported_protocol,
    extension_required,
    mod_mismatch
};

struct JoinAssessment final
{
    JoinCompatibility status{JoinCompatibility::malformed_advertisement};
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    bool password_required{};
    std::string detail;

    [[nodiscard]] bool joinable() const noexcept
    {
        return status == JoinCompatibility::joinable;
    }
};

struct SessionBrowserOptions final
{
    DiscoveryFilter filter{DiscoveryFilter::all};
    std::string query;
    bool compatible_only{};
    bool hide_full{};
};

inline constexpr std::size_t maximum_discovered_sessions = 512U;

[[nodiscard]] bool parse_direct_endpoint(std::string_view text, DirectEndpoint &endpoint,
                                         std::string &error);
[[nodiscard]] bool validate_session_advertisement(const DiscoveredSession &session,
                                                  ProtocolVersion &protocol,
                                                  std::string &error) noexcept;
[[nodiscard]] JoinAssessment assess_join(const DiscoveredSession &session,
                                         const ModManifest &client_manifest,
                                         bool client_advertises_extension);
[[nodiscard]] PeerAdmissionRequest make_peer_admission_request(const DiscoveredSession &session,
                                                               PeerId peer, std::string password,
                                                               bool client_advertises_extension);
[[nodiscard]] const char *discovery_source_label(DiscoverySource source) noexcept;
[[nodiscard]] const char *join_compatibility_label(JoinCompatibility status) noexcept;

class SessionBrowser final
{
  public:
    void replace(std::vector<DiscoveredSession> sessions);
    void set_options(SessionBrowserOptions options);
    void set_client(ModManifest manifest, bool advertises_extension);

    [[nodiscard]] const std::vector<DiscoveredSession> &sessions() const noexcept
    {
        return sessions_;
    }
    [[nodiscard]] const std::vector<std::size_t> &visible_indices() const noexcept
    {
        return visible_indices_;
    }
    [[nodiscard]] const SessionBrowserOptions &options() const noexcept
    {
        return options_;
    }
    [[nodiscard]] JoinAssessment assessment(std::size_t session_index) const;

  private:
    void rebuild();

    std::vector<DiscoveredSession> sessions_;
    std::vector<std::size_t> visible_indices_;
    SessionBrowserOptions options_;
    ModManifest client_manifest_;
    bool client_advertises_extension_{true};
};

} // namespace openminerz::net
