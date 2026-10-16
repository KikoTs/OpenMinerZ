#pragma once

#include "openminerz/net/admission.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
    // Lobby owner's platform id (zero when the platform did not report one).
    // Identity is always compared through this id, never through host_name.
    PeerId host_peer{};
};

enum class JoinCompatibility : std::uint8_t
{
    joinable,
    full,
    platform_restricted,
    malformed_advertisement,
    unsupported_protocol,
    extension_required,
    mod_mismatch,
    session_closed,
    own_session
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
// local_peer is the signed-in platform id. A lobby it owns can never be joined
// (the host and the joiner would be one transport endpoint); zero disables the check.
[[nodiscard]] JoinAssessment assess_join(const DiscoveredSession &session,
                                         const ModManifest &client_manifest,
                                         bool client_advertises_extension,
                                         PeerId local_peer = 0U);
[[nodiscard]] PeerAdmissionRequest make_peer_admission_request(const DiscoveredSession &session,
                                                               PeerId peer, std::string password,
                                                               bool client_advertises_extension);
[[nodiscard]] const char *discovery_source_label(DiscoverySource source) noexcept;
[[nodiscard]] const char *join_compatibility_label(JoinCompatibility status) noexcept;

// Text shown when a join cannot proceed. retry_password asks the UI to forget the
// stored password so the next attempt prompts again.
struct JoinFailureNotice final
{
    std::string title;
    std::string detail;
    bool retry_password{};
};

// Maps a host denial code, a Steam lobby-enter code, or a transport error to the
// precise reason the player should read. Unknown text passes through verbatim.
[[nodiscard]] JoinFailureNotice describe_join_failure(std::string_view reason);

// Keeps the highlighted browser entry on the same server (or the same trailing
// action button) when the row list is rebuilt. Rows are identified by lobby id;
// entries at or beyond the row count are the action buttons that follow the rows.
[[nodiscard]] std::size_t remap_browser_selection(
    std::size_t selection, std::span<const std::uint64_t> previous_rows,
    std::span<const std::uint64_t> current_rows) noexcept;

class SessionBrowser final
{
  public:
    // Replaces the list. The selection is tracked by lobby id, so it survives a
    // refresh that reorders rows and simply stops resolving when its row expires.
    void replace(std::vector<DiscoveredSession> sessions);
    void set_options(SessionBrowserOptions options);
    void set_client(ModManifest manifest, bool advertises_extension);
    // Lobbies owned by this platform id are the player's own (live elsewhere or
    // left behind by an earlier session). They are never offered as rows.
    void set_local_peer(PeerId local_peer);
    void select(std::uint64_t session_id) noexcept;

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
    [[nodiscard]] PeerId local_peer() const noexcept
    {
        return local_peer_;
    }
    [[nodiscard]] std::size_t own_session_count() const noexcept
    {
        return own_session_count_;
    }
    [[nodiscard]] std::uint64_t selected_id() const noexcept
    {
        return selected_id_;
    }
    [[nodiscard]] std::optional<std::size_t> selected_index() const noexcept
    {
        return index_of(selected_id_);
    }
    [[nodiscard]] std::optional<std::size_t> index_of(std::uint64_t session_id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> visible_row_of(
        std::uint64_t session_id) const noexcept;
    [[nodiscard]] JoinAssessment assessment(std::size_t session_index) const;

  private:
    void rebuild();

    std::vector<DiscoveredSession> sessions_;
    std::vector<std::size_t> visible_indices_;
    SessionBrowserOptions options_;
    ModManifest client_manifest_;
    PeerId local_peer_{};
    std::uint64_t selected_id_{};
    std::size_t own_session_count_{};
    bool client_advertises_extension_{true};
};

// What the browser may do with the Steam helper right now. The helper runs one
// asynchronous operation at a time and is consumed by a joined session.
struct JoinTransportView final
{
    bool busy{};       // a discovery or join is in flight
    bool ready{};      // an idle browsing helper holds the list currently shown
    bool in_session{}; // a joined session owns the helper
};

struct JoinBrowserAction final
{
    enum class Kind : std::uint8_t
    {
        none,
        start_discovery,
        start_join
    };

    Kind kind{Kind::none};
    bool quiet{true};
    std::uint64_t session_id{};
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    std::string password;
    std::optional<JoinFailureNotice> failure;
};

// Decides when the server browser refreshes and how a Join request reaches the
// helper. It never requires the player to refresh by hand: a join requested while
// a refresh is in flight, or while no helper is alive, is queued and re-resolved by
// lobby id against the next discovery, which either starts it or fails it with the
// reason. The owner performs the asynchronous work and reports its outcome.
class JoinBrowserFlow final
{
  public:
    using Assessor = std::function<JoinAssessment(const DiscoveredSession &)>;

    static constexpr float refresh_interval_seconds = 10.0F;
    static constexpr float first_retry_seconds = 2.0F;
    static constexpr float maximum_retry_seconds = 30.0F;

    // Overrides the default assessment (the session's own protocol with no mods).
    void set_assessor(Assessor assessor);

    void browser_opened() noexcept;
    void request_refresh() noexcept;
    // The helper was given up outside the browser (left a server, started hosting).
    void transport_lost() noexcept;

    [[nodiscard]] JoinBrowserAction tick(float seconds, bool browser_visible,
                                         const JoinTransportView &transport);
    [[nodiscard]] JoinBrowserAction request_join(std::uint64_t session_id, std::string password,
                                                 const JoinTransportView &transport,
                                                 const SessionBrowser &browser);
    void cancel_join() noexcept;

    [[nodiscard]] JoinBrowserAction discovery_succeeded(SessionBrowser &browser,
                                                        std::vector<DiscoveredSession> sessions,
                                                        PeerId local_peer);
    [[nodiscard]] JoinBrowserAction discovery_failed(SessionBrowser &browser,
                                                     std::string_view error);
    [[nodiscard]] JoinFailureNotice join_failed(std::string_view reason);

    [[nodiscard]] bool join_pending() const noexcept
    {
        return pending_.has_value();
    }
    [[nodiscard]] std::uint64_t pending_session() const noexcept
    {
        return pending_.has_value() ? pending_->session_id : 0U;
    }
    [[nodiscard]] std::size_t consecutive_failures() const noexcept
    {
        return consecutive_failures_;
    }
    [[nodiscard]] float seconds_until_refresh() const noexcept
    {
        return refresh_due_ ? 0.0F : refresh_timer_;
    }

  private:
    struct PendingJoin final
    {
        std::uint64_t session_id{};
        std::string password;
    };

    [[nodiscard]] JoinBrowserAction resolve_pending(const SessionBrowser &browser);
    [[nodiscard]] JoinBrowserAction start_discovery(bool quiet) noexcept;

    Assessor assessor_;
    std::optional<PendingJoin> pending_;
    std::size_t consecutive_failures_{};
    float refresh_timer_{refresh_interval_seconds};
    bool refresh_due_{};
    bool manual_refresh_{};
};

// Bounds every network wait of a join so a silent host cannot hold the loading
// screen forever. Any inbound traffic or stage change restarts it.
class JoinProgressWatchdog final
{
  public:
    void restart() noexcept
    {
        idle_seconds_ = 0.0F;
    }
    [[nodiscard]] bool stalled(float seconds, float limit_seconds) noexcept
    {
        idle_seconds_ += seconds;
        return idle_seconds_ >= limit_seconds;
    }

  private:
    float idle_seconds_{};
};

} // namespace openminerz::net
