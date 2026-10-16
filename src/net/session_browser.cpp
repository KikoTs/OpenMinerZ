#include "openminerz/net/session_browser.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openminerz::net
{
namespace
{

bool printable(std::string_view value, bool allow_empty) noexcept
{
    if (!allow_empty && value.empty())
        return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
    });
}

bool valid_property(const SessionProperties &properties, std::size_t index, std::int32_t minimum,
                    std::int32_t maximum) noexcept
{
    return properties[index].has_value() && *properties[index] >= minimum &&
           *properties[index] <= maximum;
}

bool source_matches(DiscoverySource source, DiscoveryFilter filter) noexcept
{
    switch (filter)
    {
    case DiscoveryFilter::all:
        return true;
    case DiscoveryFilter::friends:
        return source == DiscoverySource::friend_session;
    case DiscoveryFilter::public_lobbies:
        return source == DiscoverySource::public_lobby;
    case DiscoveryFilter::lan:
        return source == DiscoverySource::lan;
    case DiscoveryFilter::direct:
        return source == DiscoverySource::direct;
    }
    return false;
}

std::string lower(std::string_view value)
{
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool contains_query(const DiscoveredSession &session, std::string_view query)
{
    if (query.empty())
        return true;
    const std::string needle = lower(query);
    return lower(session.server_name).find(needle) != std::string::npos ||
           lower(session.host_name).find(needle) != std::string::npos ||
           lower(session.server_message).find(needle) != std::string::npos;
}

std::uint32_t sortable_ping(const DiscoveredSession &session) noexcept
{
    return session.ping_milliseconds.value_or(std::numeric_limits<std::uint32_t>::max());
}

} // namespace

bool parse_direct_endpoint(std::string_view text, DirectEndpoint &endpoint, std::string &error)
{
    DirectEndpoint parsed;
    if (text.empty() || text.size() > 320U || !printable(text, false))
    {
        error = "Address must contain 1-320 printable bytes.";
        return false;
    }

    std::string_view host = text;
    std::string_view port_text;
    if (text.front() == '[')
    {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close == 1U ||
            (close + 1U < text.size() && text[close + 1U] != ':'))
        {
            error = "Bracketed IPv6 address is malformed.";
            return false;
        }
        host = text.substr(1U, close - 1U);
        if (close + 1U < text.size())
            port_text = text.substr(close + 2U);
    }
    else
    {
        const std::size_t first_colon = text.find(':');
        const std::size_t last_colon = text.rfind(':');
        if (first_colon != std::string_view::npos && first_colon == last_colon)
        {
            host = text.substr(0U, first_colon);
            port_text = text.substr(first_colon + 1U);
        }
    }
    if (host.empty() || host.size() > 253U ||
        host.find_first_of(" /\\\t\r\n") != std::string_view::npos)
    {
        error = "Host name or address is invalid.";
        return false;
    }
    if (!port_text.empty())
    {
        std::uint32_t port{};
        for (const char character : port_text)
        {
            if (character < '0' || character > '9')
            {
                error = "Port must be a decimal number.";
                return false;
            }
            const std::uint32_t digit = static_cast<std::uint32_t>(character - '0');
            if (port > 6553U || (port == 6553U && digit > 5U))
            {
                error = "Port must be between 1 and 65535.";
                return false;
            }
            port = port * 10U + digit;
        }
        if (port == 0U)
        {
            error = "Port must be between 1 and 65535.";
            return false;
        }
        parsed.port = static_cast<std::uint16_t>(port);
    }
    else if (text.back() == ':')
    {
        error = "Port is missing after the colon.";
        return false;
    }
    parsed.host = std::string{host};
    endpoint = std::move(parsed);
    error.clear();
    return true;
}

bool validate_session_advertisement(const DiscoveredSession &session, ProtocolVersion &protocol,
                                    std::string &error) noexcept
{
    if (session.session_id == 0U || session.server_name.size() > 128U ||
        session.host_name.size() > 128U || session.server_message.size() > 128U ||
        !printable(session.server_name, false) || !printable(session.host_name, false) ||
        !printable(session.server_message, true))
    {
        error = "Lobby identity is malformed.";
        return false;
    }
    if (session.maximum_players == 0U || session.maximum_players > 16U ||
        session.current_players > session.maximum_players)
    {
        error = "Advertised player capacity is invalid.";
        return false;
    }
    if (!session.properties[0].has_value())
    {
        error = "Protocol property is missing.";
        return false;
    }
    if (*session.properties[0] == static_cast<std::int32_t>(ProtocolVersion::classic_v3))
        protocol = ProtocolVersion::classic_v3;
    else if (*session.properties[0] == static_cast<std::int32_t>(ProtocolVersion::modern_v4))
        protocol = ProtocolVersion::modern_v4;
    else
    {
        error = "Protocol property is unsupported.";
        return false;
    }
    const std::int32_t maximum_mode = protocol == ProtocolVersion::classic_v3 ? 3 : 5;
    if (!valid_property(session.properties, 1U, 0, 1) ||
        !valid_property(session.properties, 2U, 0, maximum_mode) ||
        !valid_property(session.properties, 3U, 0, 3) ||
        !valid_property(session.properties, 4U, 0, 1) ||
        !valid_property(session.properties, 5U, 0, 2) || session.properties[6].has_value() ||
        session.properties[7].has_value())
    {
        error = "Original eight-slot session properties are malformed.";
        return false;
    }
    if (session.source == DiscoverySource::direct && !session.endpoint.has_value())
    {
        error = "Direct session is missing an endpoint.";
        return false;
    }
    if (!session.extension_metadata_valid)
    {
        error = "Advertised OpenMinerZ extension metadata is malformed.";
        return false;
    }
    if (session.required_manifest.has_value())
    {
        std::string manifest_error;
        if (!validate_mod_manifest(*session.required_manifest, manifest_error) ||
            session.required_manifest->protocol != protocol || !session.advertises_extension)
        {
            error = "Advertised mod contract is malformed.";
            return false;
        }
    }
    error.clear();
    return true;
}

JoinAssessment assess_join(const DiscoveredSession &session, const ModManifest &client_manifest,
                           bool client_advertises_extension, PeerId local_peer)
{
    JoinAssessment result;
    std::string error;
    if (!validate_session_advertisement(session, result.protocol, error))
    {
        result.status = error == "Protocol property is unsupported."
                            ? JoinCompatibility::unsupported_protocol
                            : JoinCompatibility::malformed_advertisement;
        result.detail = std::move(error);
        return result;
    }
    result.password_required = session.password_required;
    // Compared by platform id only: another player may legitimately share the
    // local display name, and the local account may host under any name.
    if (local_peer != 0U && session.host_peer == local_peer)
    {
        result.status = JoinCompatibility::own_session;
        result.detail = "This lobby is hosted by your own Steam account.";
        return result;
    }
    if (session.properties[1] != 0)
    {
        result.status = JoinCompatibility::session_closed;
        result.detail = "This game is no longer accepting players.";
        return result;
    }
    if (session.current_players >= session.maximum_players)
    {
        result.status = JoinCompatibility::full;
        result.detail = "Session is full.";
        return result;
    }
    if (!session.platform_authorized)
    {
        result.status = JoinCompatibility::platform_restricted;
        result.detail = "Host policy requires a friend or invitation.";
        return result;
    }
    if (client_manifest.protocol != result.protocol)
    {
        result.status = JoinCompatibility::mod_mismatch;
        result.detail = "Selected client profile does not match this session.";
        return result;
    }
    if (session.required_manifest.has_value())
    {
        const ModCompatibilityResult compatibility = compare_mod_manifests(
            *session.required_manifest, client_advertises_extension ? &client_manifest : nullptr,
            client_advertises_extension);
        if (!compatibility.compatible())
        {
            result.status = compatibility.status == ModCompatibility::extension_required
                                ? JoinCompatibility::extension_required
                                : JoinCompatibility::mod_mismatch;
            result.detail = compatibility.detail;
            return result;
        }
    }
    result.status = JoinCompatibility::joinable;
    result.detail = session.password_required ? "Password required before admission."
                                              : "Session is compatible and ready to join.";
    return result;
}

PeerAdmissionRequest make_peer_admission_request(const DiscoveredSession &session, PeerId peer,
                                                 std::string password,
                                                 bool client_advertises_extension)
{
    ProtocolVersion ignored{};
    std::string error;
    if (!validate_session_advertisement(session, ignored, error))
        throw std::invalid_argument("invalid session advertisement: " + error);
    return {peer, session.properties, std::move(password),
            client_advertises_extension && session.advertises_extension,
            session.platform_authorized};
}

const char *discovery_source_label(DiscoverySource source) noexcept
{
    switch (source)
    {
    case DiscoverySource::friend_session:
        return "FRIEND";
    case DiscoverySource::public_lobby:
        return "PUBLIC";
    case DiscoverySource::lan:
        return "LAN";
    case DiscoverySource::direct:
        return "DIRECT";
    }
    return "UNKNOWN";
}

const char *join_compatibility_label(JoinCompatibility status) noexcept
{
    switch (status)
    {
    case JoinCompatibility::joinable:
        return "COMPATIBLE";
    case JoinCompatibility::full:
        return "FULL";
    case JoinCompatibility::session_closed:
        return "CLOSED";
    case JoinCompatibility::platform_restricted:
        return "INVITE REQUIRED";
    case JoinCompatibility::malformed_advertisement:
        return "INVALID";
    case JoinCompatibility::unsupported_protocol:
        return "UNSUPPORTED";
    case JoinCompatibility::extension_required:
        return "MOD EXTENSION";
    case JoinCompatibility::mod_mismatch:
        return "MOD MISMATCH";
    case JoinCompatibility::own_session:
        return "YOUR OWN LOBBY";
    }
    return "UNKNOWN";
}

JoinFailureNotice describe_join_failure(std::string_view reason)
{
    struct Known final
    {
        std::string_view code;
        std::string_view title;
        std::string_view detail;
        bool retry_password;
    };
    // Host denial codes are DNA.Net.GamerServices.NetworkSession.ResultCode names;
    // lobby-enter codes mirror the names the stock client reports for Steam's
    // EChatRoomEnterResponse values.
    static constexpr Known known[]{
        {"GamerAlreadyConnected", "ALREADY IN THIS SESSION",
         "The host reports your Steam account or player name as already connected. If you just "
         "left, wait a few seconds and join again.",
         false},
        {"IncorrectPassword", "INCORRECT PASSWORD",
         "The host rejected the password. Enter it again and retry.", true},
        {"GameIsFull", "SERVER IS FULL", "Every player slot on this server is taken.", false},
        {"GameNoLongerExists", "SERVER NO LONGER AVAILABLE",
         "This server went offline or stopped accepting players.", false},
        {"ServerHasNewerVersion", "VERSION MISMATCH",
         "The host runs a newer network version than this profile.", false},
        {"ServerHasOlderVersion", "VERSION MISMATCH",
         "The host runs an older network version than this profile.", false},
        {"VersionIsInvalid", "VERSION MISMATCH",
         "The host could not read this profile's join request.", false},
        {"GameNamesDontMatch", "VERSION MISMATCH", "The host is running a different game.", false},
        {"SessionPropertiesDontMatch", "SERVER SETTINGS CHANGED",
         "The host changed its game settings after it was listed. Join again to use the new "
         "settings.",
         false},
        // Also what a server that only admits OpenMinerZ players says to the
        // original game (legacy_modded_session_denial); an OpenMinerZ client
        // never meets that case, it negotiates first.
        {"ConnectionDenied", "HOST DENIED THE CONNECTION",
         "The host only admits friends or invited players, or has blocked this account.", false},
        {"HostDeniedConnection", "HOST UNREACHABLE",
         "A connection to the host could not be established.", false},
        {"HostDisconnected", "HOST DISCONNECTED", "The host left before the join completed.",
         false},
        {"YouLackTheNeededPermissions", "NOT ALLOWED TO JOIN",
         "Steam did not admit this account. The lobby is friends-only or invite-only.", false},
        {"BannedBySteam", "NOT ALLOWED TO JOIN", "Steam reports this account as banned here.",
         false},
        {"YouAreALimitedUserAndCannotJoin", "NOT ALLOWED TO JOIN",
         "Limited Steam accounts cannot join lobbies.", false},
        {"UnknownSteamErrorJoiningGame", "STEAM COULD NOT JOIN THE LOBBY",
         "Steam reported an error while entering the lobby. Try again.", false},
        {"HostDidNotAnswer", "HOST DID NOT ANSWER",
         "The host did not answer the join request. It may have gone offline.", false},
        {"HostStoppedResponding", "HOST STOPPED RESPONDING",
         "The host stopped sending the world before loading finished.", false},
        {"OwnSteamAccount", "YOUR OWN LOBBY",
         "This lobby is hosted by your own Steam account, so it cannot be joined from it.",
         false},
        {"JoinCancelled", "JOIN CANCELLED", "The join was cancelled.", false},
    };
    for (const Known &entry : known)
        if (reason == entry.code)
            return {std::string{entry.title}, std::string{entry.detail}, entry.retry_password};
    return {"COULD NOT JOIN",
            reason.empty() ? std::string{"The join ended without a reported reason."}
                           : std::string{reason},
            false};
}

std::size_t remap_browser_selection(std::size_t selection,
                                    std::span<const std::uint64_t> previous_rows,
                                    std::span<const std::uint64_t> current_rows) noexcept
{
    if (selection >= previous_rows.size())
        return current_rows.size() + (selection - previous_rows.size());
    const auto found =
        std::find(current_rows.begin(), current_rows.end(), previous_rows[selection]);
    if (found != current_rows.end())
        return static_cast<std::size_t>(found - current_rows.begin());
    // The highlighted server expired. Stay near it, or fall to the first action.
    return std::min(selection, current_rows.size());
}

void SessionBrowser::replace(std::vector<DiscoveredSession> sessions)
{
    if (sessions.size() > maximum_discovered_sessions)
        sessions.resize(maximum_discovered_sessions);
    std::vector<DiscoveredSession> unique;
    unique.reserve(sessions.size());
    for (DiscoveredSession &session : sessions)
    {
        if (session.session_id == 0U ||
            std::any_of(unique.begin(), unique.end(), [&](const DiscoveredSession &candidate) {
                return candidate.session_id == session.session_id;
            }))
            continue;
        unique.push_back(std::move(session));
    }
    sessions_ = std::move(unique);
    rebuild();
}

void SessionBrowser::set_options(SessionBrowserOptions options)
{
    options_ = std::move(options);
    if (options_.query.size() > 128U)
        options_.query.resize(128U);
    rebuild();
}

void SessionBrowser::set_client(ModManifest manifest, bool advertises_extension)
{
    client_manifest_ = std::move(manifest);
    client_advertises_extension_ = advertises_extension;
    rebuild();
}

void SessionBrowser::set_local_peer(PeerId local_peer)
{
    local_peer_ = local_peer;
    rebuild();
}

void SessionBrowser::select(std::uint64_t session_id) noexcept
{
    selected_id_ = session_id;
}

std::optional<std::size_t> SessionBrowser::index_of(std::uint64_t session_id) const noexcept
{
    if (session_id == 0U)
        return std::nullopt;
    for (std::size_t index = 0U; index < sessions_.size(); ++index)
        if (sessions_[index].session_id == session_id)
            return index;
    return std::nullopt;
}

std::optional<std::size_t> SessionBrowser::visible_row_of(
    std::uint64_t session_id) const noexcept
{
    if (session_id == 0U)
        return std::nullopt;
    for (std::size_t row = 0U; row < visible_indices_.size(); ++row)
        if (sessions_[visible_indices_[row]].session_id == session_id)
            return row;
    return std::nullopt;
}

JoinAssessment SessionBrowser::assessment(std::size_t session_index) const
{
    if (session_index >= sessions_.size())
        return {JoinCompatibility::malformed_advertisement, ProtocolVersion::classic_v3, false,
                "Session selection is out of range."};
    return assess_join(sessions_[session_index], client_manifest_, client_advertises_extension_,
                       local_peer_);
}

void SessionBrowser::rebuild()
{
    visible_indices_.clear();
    own_session_count_ = 0U;
    for (std::size_t index = 0U; index < sessions_.size(); ++index)
    {
        const DiscoveredSession &session = sessions_[index];
        if (local_peer_ != 0U && session.host_peer == local_peer_)
        {
            ++own_session_count_;
            continue;
        }
        if (!source_matches(session.source, options_.filter) ||
            !contains_query(session, options_.query))
            continue;
        const JoinAssessment result = assessment(index);
        if ((options_.compatible_only && !result.joinable()) ||
            (options_.hide_full && result.status == JoinCompatibility::full))
            continue;
        visible_indices_.push_back(index);
    }
    std::stable_sort(visible_indices_.begin(), visible_indices_.end(),
                     [&](std::size_t left, std::size_t right) {
                         const DiscoveredSession &a = sessions_[left];
                         const DiscoveredSession &b = sessions_[right];
                         if (a.favorite != b.favorite)
                             return a.favorite > b.favorite;
                         if (a.source == DiscoverySource::friend_session !=
                             (b.source == DiscoverySource::friend_session))
                             return a.source == DiscoverySource::friend_session;
                         if (sortable_ping(a) != sortable_ping(b))
                             return sortable_ping(a) < sortable_ping(b);
                         return lower(a.server_name) < lower(b.server_name);
                     });
}

void JoinBrowserFlow::set_assessor(Assessor assessor)
{
    assessor_ = std::move(assessor);
}

void JoinBrowserFlow::browser_opened() noexcept
{
    refresh_due_ = true;
}

void JoinBrowserFlow::request_refresh() noexcept
{
    refresh_due_ = true;
    manual_refresh_ = true;
}

void JoinBrowserFlow::transport_lost() noexcept
{
    refresh_due_ = true;
}

JoinBrowserAction JoinBrowserFlow::start_discovery(bool quiet) noexcept
{
    refresh_due_ = false;
    manual_refresh_ = false;
    refresh_timer_ = refresh_interval_seconds;
    JoinBrowserAction action;
    action.kind = JoinBrowserAction::Kind::start_discovery;
    action.quiet = quiet;
    return action;
}

JoinBrowserAction JoinBrowserFlow::tick(float seconds, bool browser_visible,
                                        const JoinTransportView &transport)
{
    if (transport.in_session)
    {
        pending_.reset();
        return {};
    }
    // One helper operation at a time. Requests stay latched until it completes.
    if (transport.busy)
        return {};
    // A queued join outlived the operation it waited behind (for example a join
    // that was cancelled). Discover again; that result resolves it by lobby id.
    if (pending_.has_value())
        return start_discovery(true);
    if (!browser_visible)
        return {};
    if (refresh_due_)
        return start_discovery(!manual_refresh_);
    refresh_timer_ -= std::max(0.0F, seconds);
    if (refresh_timer_ <= 0.0F)
        return start_discovery(true);
    return {};
}

JoinBrowserAction JoinBrowserFlow::request_join(std::uint64_t session_id, std::string password,
                                                const JoinTransportView &transport,
                                                const SessionBrowser &browser)
{
    JoinBrowserAction action;
    if (transport.in_session)
    {
        action.failure = JoinFailureNotice{"ALREADY IN A SESSION",
                                           "Leave the current server before joining another.",
                                           false};
        return action;
    }
    if (session_id == 0U)
    {
        action.failure = describe_join_failure("GameNoLongerExists");
        return action;
    }
    pending_ = PendingJoin{session_id, std::move(password)};
    // Wait for the operation in flight. Its completion resolves this request.
    if (transport.busy)
        return action;
    // The idle helper produced the rows on screen, so a listed lobby is current.
    if (transport.ready && browser.index_of(session_id).has_value())
        return resolve_pending(browser);
    // No live helper, or the row is not in the helper's latest list: discover
    // first, then re-resolve the lobby by id.
    return start_discovery(true);
}

void JoinBrowserFlow::cancel_join() noexcept
{
    pending_.reset();
    refresh_due_ = true;
}

JoinBrowserAction JoinBrowserFlow::resolve_pending(const SessionBrowser &browser)
{
    JoinBrowserAction action;
    if (!pending_.has_value())
        return action;
    PendingJoin pending = std::move(*pending_);
    pending_.reset();
    const std::optional<std::size_t> index = browser.index_of(pending.session_id);
    if (!index.has_value())
    {
        action.failure = describe_join_failure("GameNoLongerExists");
        return action;
    }
    const DiscoveredSession &session = browser.sessions()[*index];
    JoinAssessment assessment;
    if (assessor_)
        assessment = assessor_(session);
    else
    {
        ModManifest manifest;
        if (session.properties[0] == static_cast<std::int32_t>(ProtocolVersion::modern_v4))
            manifest.protocol = ProtocolVersion::modern_v4;
        assessment = assess_join(session, manifest, true, browser.local_peer());
    }
    if (!assessment.joinable())
    {
        action.failure =
            assessment.status == JoinCompatibility::full ? describe_join_failure("GameIsFull")
            : assessment.status == JoinCompatibility::session_closed
                ? describe_join_failure("GameNoLongerExists")
            : assessment.status == JoinCompatibility::own_session
                ? describe_join_failure("OwnSteamAccount")
                : JoinFailureNotice{join_compatibility_label(assessment.status),
                                    assessment.detail, false};
        return action;
    }
    action.kind = JoinBrowserAction::Kind::start_join;
    action.session_id = pending.session_id;
    action.protocol = assessment.protocol;
    action.password = std::move(pending.password);
    return action;
}

JoinBrowserAction JoinBrowserFlow::discovery_succeeded(SessionBrowser &browser,
                                                       std::vector<DiscoveredSession> sessions,
                                                       PeerId local_peer)
{
    browser.set_local_peer(local_peer);
    browser.replace(std::move(sessions));
    consecutive_failures_ = 0U;
    refresh_due_ = false;
    manual_refresh_ = false;
    refresh_timer_ = refresh_interval_seconds;
    return resolve_pending(browser);
}

JoinBrowserAction JoinBrowserFlow::discovery_failed(SessionBrowser &browser,
                                                    std::string_view error)
{
    ++consecutive_failures_;
    refresh_due_ = false;
    manual_refresh_ = false;
    float delay = first_retry_seconds;
    for (std::size_t attempt = 1U;
         attempt < consecutive_failures_ && delay < maximum_retry_seconds; ++attempt)
        delay *= 2.0F;
    refresh_timer_ = std::min(delay, maximum_retry_seconds);
    // One failed refresh keeps the list (a join still re-resolves every row). A
    // second in a row means the rows can no longer be confirmed, so they expire.
    if (consecutive_failures_ >= 2U)
        browser.replace({});
    JoinBrowserAction action;
    if (pending_.has_value())
    {
        pending_.reset();
        action.failure = JoinFailureNotice{
            "COULD NOT REACH STEAM",
            error.empty() ? std::string{"The server list could not be refreshed."}
                          : std::string{error},
            false};
    }
    return action;
}

JoinFailureNotice JoinBrowserFlow::join_failed(std::string_view reason)
{
    // A failed join consumes the helper. Restore the list without being asked.
    pending_.reset();
    refresh_due_ = true;
    return describe_join_failure(reason);
}

} // namespace openminerz::net
