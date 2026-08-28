#include "openminerz/net/session_browser.hpp"

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
                           bool client_advertises_extension)
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
    }
    return "UNKNOWN";
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

JoinAssessment SessionBrowser::assessment(std::size_t session_index) const
{
    if (session_index >= sessions_.size())
        return {JoinCompatibility::malformed_advertisement, ProtocolVersion::classic_v3, false,
                "Session selection is out of range."};
    return assess_join(sessions_[session_index], client_manifest_, client_advertises_extension_);
}

void SessionBrowser::rebuild()
{
    visible_indices_.clear();
    for (std::size_t index = 0U; index < sessions_.size(); ++index)
    {
        const DiscoveredSession &session = sessions_[index];
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

} // namespace openminerz::net
