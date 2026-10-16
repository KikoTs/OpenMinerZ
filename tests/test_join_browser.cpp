// Client-side discovery and join regressions: joining a session that already holds
// the local display name, own-lobby detection by Steam id, selection stability
// across refreshes, joining a stale row, and joining while a refresh is in flight.

#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_runtime_client.hpp"
#include "openminerz/net/session_browser.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz::net;

void expect(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

HostSessionSettings host_settings(ProtocolVersion protocol)
{
    HostSessionSettings settings;
    settings.server_name = "kikots's world";
    settings.protocol = protocol;
    settings.maximum_players = 8;
    settings.visibility = HostVisibility::public_lobby;
    settings.mod_manifest.protocol = protocol;
    return settings;
}

// A stand-in for the stock host's admission, independent of the OpenMinerZ host's
// own policy. The rule is the behaviour of
// SteamNetworkSessionProvider.HandleHostConnectionApproval: a request is denied
// with GamerAlreadyConnected when its Steam sender is already in the session or
// when any gamer in the session has the same display tag (ordinal comparison).
// An admitted request receives connected status 5 and the peer/GID table.
struct StockRuleHost final
{
    static constexpr PeerId host_peer = 900U;

    struct Gamer final
    {
        PeerId peer{};
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
    };

    StockRuleHost(ProtocolVersion protocol, std::string host_tag)
        : properties{make_session_properties(host_settings(protocol), false, 1, 0, false)}
    {
        gamers.push_back({host_peer, 0U, LegacyGamerIdentity{{9U, 9U}, std::move(host_tag)}});
    }

    [[nodiscard]] std::vector<LegacySteamNativeSend> approve(
        PeerId sender, std::span<const std::uint8_t> wire)
    {
        LegacySteamWirePacket packet;
        LegacySteamCodecError codec_error{};
        expect(decode_legacy_steam_wire(wire, packet, codec_error) &&
                   packet.message_type == SteamIncomingMessageType::connection_approval,
               "the client must send a stock connection approval");
        LegacyConnectionRequest request;
        expect(decode_legacy_connection_request(packet.payload, request, codec_error) &&
                   request.session_properties == properties,
               "the client's approval must decode with the stock codec");
        requested_tags.push_back(request.gamer.gamer_tag);
        const bool duplicate =
            std::any_of(gamers.begin(), gamers.end(), [&](const Gamer &existing) {
                return existing.peer == sender ||
                       existing.gamer.gamer_tag == request.gamer.gamer_tag;
            });
        if (duplicate)
        {
            // SteamWorks.Deny: native channel one, a zero byte, the reason, a NUL.
            std::vector<std::uint8_t> denial{0U};
            for (const char character : legacy_duplicate_gamer_denial)
                denial.push_back(static_cast<std::uint8_t>(character));
            denial.push_back(0U);
            return {LegacySteamNativeSend{sender, std::move(denial),
                                          SteamDeliveryMethod::reliable_ordered, 1U}};
        }
        LegacyConnectedResponse response;
        response.player_gid = next_gid++;
        for (const Gamer &existing : gamers)
        {
            response.peers.push_back(existing.gamer);
            response.peer_gids.push_back(existing.gid);
        }
        const std::array<std::uint8_t, 4U> connected_status{5U, 0U, 0U, 0U};
        const auto status_wire = encode_legacy_steam_wire(SteamIncomingMessageType::status_changed,
                                                          0U, connected_status, codec_error);
        const auto response_payload = encode_legacy_connected_response(response, codec_error);
        expect(status_wire.has_value() && response_payload.has_value(),
               "the stock admission records must encode");
        const auto response_wire = encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U,
                                                            *response_payload, codec_error);
        expect(response_wire.has_value(), "the stock roster record must encode");
        gamers.push_back({sender, response.player_gid, request.gamer});
        return {LegacySteamNativeSend{sender, *status_wire, SteamDeliveryMethod::reliable_ordered,
                                      0U},
                LegacySteamNativeSend{sender, *response_wire,
                                      SteamDeliveryMethod::reliable_ordered, 0U}};
    }

    SessionProperties properties;
    std::vector<Gamer> gamers;
    std::vector<std::string> requested_tags;
    std::uint8_t next_gid{1U};
};

LegacySteamClientSessionOptions client_options(ProtocolVersion protocol, PeerId local_peer,
                                               std::string tag, std::string host_name,
                                               const SessionProperties &properties)
{
    LegacySteamClientSessionOptions options;
    options.protocol = protocol;
    options.host_peer = StockRuleHost::host_peer;
    options.expected_host_name = std::move(host_name);
    options.session_properties = properties;
    // Identity is the Steam peer, exactly as the runtime builds it.
    options.local_gamer.player_id.resize(16U);
    for (std::size_t index = 0U; index < 8U; ++index)
        options.local_gamer.player_id[index] =
            static_cast<std::uint8_t>(local_peer >> (index * 8U));
    options.local_gamer.gamer_tag = std::move(tag);
    return options;
}

struct JoinOutcome final
{
    bool connected{};
    std::uint8_t attempts{};
    std::optional<std::string> final_denial;
};

// The admission loop of LegacySteamRuntimeClient::pump, minus the helper pipe.
template <typename Approve>
JoinOutcome join_with_retries(LegacySteamClientSession &client, PeerId local_peer,
                              Approve &&approve)
{
    JoinOutcome outcome;
    std::string error;
    std::optional<LegacySteamNativeSend> request = client.begin(error);
    expect(request.has_value(), error);
    for (std::size_t guard = 0U; guard < 16U && request.has_value(); ++guard)
    {
        std::optional<std::string> denial;
        for (const LegacySteamNativeSend &send : approve(local_peer, request->payload))
        {
            if (send.remote_peer != local_peer)
                continue;
            const LegacySteamClientSessionResult result = client.process_native_packet(
                StockRuleHost::host_peer, send.p2p_channel, send.payload);
            if (result.rejection_reason.has_value())
                denial = result.rejection_reason;
        }
        request.reset();
        if (denial.has_value())
        {
            outcome.final_denial = denial;
            if (*denial == legacy_duplicate_gamer_denial)
                request = client.retry_with_distinct_gamer_tag(error);
            if (request.has_value())
                outcome.final_denial.reset();
        }
    }
    outcome.connected = client.connected();
    outcome.attempts = client.admission_attempts();
    return outcome;
}

void same_name_join_succeeds_against_the_stock_duplicate_rule(ProtocolVersion protocol)
{
    StockRuleHost stock{protocol, "kikots"};
    const SessionProperties properties = stock.properties;
    const auto approve = [&](PeerId sender, std::span<const std::uint8_t> wire) {
        return stock.approve(sender, wire);
    };

    // A different Steam account whose persona equals the host's.
    LegacySteamClientSession second{client_options(protocol, 901U, "kikots", "kikots", properties)};
    const JoinOutcome joined = join_with_retries(second, 901U, approve);
    expect(joined.connected && !joined.final_denial.has_value() && joined.attempts == 2U,
           "a same-named player on another Steam account must be admitted on the retry");
    expect(second.local_gamer().gamer_tag == "kikots (2)" &&
               stock.requested_tags == std::vector<std::string>{"kikots", "kikots (2)"},
           "the retry must change only the display tag");
    const LegacyGamerIdentity *host_gamer = second.gamer_for(0U);
    expect(host_gamer != nullptr && host_gamer->gamer_tag == "kikots" &&
               second.local_gid().has_value() && *second.local_gid() != 0U,
           "the roster is keyed by session GID, so the host keeps the shared name");
    expect(second.local_gamer().player_id[0] == static_cast<std::uint8_t>(901U & 0xFFU),
           "the Steam-derived player id must not change with the display tag");

    // A third account with the same persona steps past both taken tags.
    LegacySteamClientSession third{client_options(protocol, 902U, "kikots", "kikots", properties)};
    const JoinOutcome third_joined = join_with_retries(third, 902U, approve);
    expect(third_joined.connected && third.local_gamer().gamer_tag == "kikots (3)" &&
               third_joined.attempts == 3U,
           "each further same-named player must find the next free display tag");
    expect(third.local_gid() != second.local_gid(),
           "same-named players must hold distinct session GIDs");

    // The stock comparison is ordinal: a case variant is a different tag.
    LegacySteamClientSession variant{
        client_options(protocol, 903U, "KikoTs", "kikots", properties)};
    const JoinOutcome variant_joined = join_with_retries(variant, 903U, approve);
    expect(variant_joined.connected && variant_joined.attempts == 1U &&
               variant.local_gamer().gamer_tag == "KikoTs",
           "a name that only resembles another must join untouched on the first request");

    // The same Steam account is already in the session: no display tag can help,
    // and the precise host reason must reach the player.
    LegacySteamClientSession ghost{client_options(protocol, 901U, "kikots", "kikots", properties)};
    const JoinOutcome ghost_joined = join_with_retries(ghost, 901U, approve);
    expect(!ghost_joined.connected && ghost_joined.attempts == legacy_maximum_admission_attempts &&
               ghost_joined.final_denial ==
                   std::optional<std::string>{std::string{legacy_duplicate_gamer_denial}},
           "a Steam account that is already connected must be refused after bounded retries");
    const JoinFailureNotice notice = describe_join_failure(*ghost_joined.final_denial);
    expect(notice.title == "ALREADY IN THIS SESSION" &&
               notice.detail.find("already connected") != std::string::npos,
           "the duplicate denial must be explained instead of shown as a raw code");
}

void same_name_join_succeeds_against_the_openminerz_host(ProtocolVersion protocol)
{
    LegacySteamHostSessionOptions options;
    options.settings = host_settings(protocol);
    options.host_gamer = LegacyGamerIdentity{{9U, 9U}, "kikots"};
    LegacySteamHostSession host{options};
    LegacySteamClientSession client{
        client_options(protocol, 901U, "kikots", "kikots", host.session_properties())};
    // Whichever policy the host applies to a shared name (admit and disambiguate,
    // or the stock denial), the client must end up admitted.
    const JoinOutcome joined = join_with_retries(
        client, 901U, [&](PeerId sender, std::span<const std::uint8_t> wire) {
            return host.process_wire_packet(sender, wire).sends;
        });
    expect(joined.connected && host.connected(901U) &&
               client.local_gid() == host.gid_for(901U) && *client.local_gid() != 0U,
           "a player named like the host must be able to join an OpenMinerZ host");
    const LegacyGamerIdentity *host_gamer = client.gamer_for(0U);
    expect(host_gamer != nullptr && host_gamer->gamer_tag == "kikots",
           "a roster entry that repeats the local name must not collide with the local player");
}

void display_tag_retry_is_bounded_and_only_for_the_duplicate_denial(ProtocolVersion protocol)
{
    const SessionProperties properties =
        make_session_properties(host_settings(protocol), false, 1, 0, false);
    LegacySteamClientSession client{
        client_options(protocol, 901U, "kikots", "kikots", properties)};
    std::string error;
    expect(!client.retry_with_distinct_gamer_tag(error).has_value(),
           "a retry is only valid after a denial");
    expect(client.begin(error).has_value(), error);
    const std::vector<std::uint8_t> password_denial{0U, 'I', 'n', 'c', 'o', 'r', 'r', 'e', 'c',
                                                    't', 'P', 'a', 's', 's', 'w', 'o', 'r', 'd',
                                                    0U};
    const LegacySteamClientSessionResult denied =
        client.process_native_packet(StockRuleHost::host_peer, 1U, password_denial);
    expect(denied.rejection_reason == std::optional<std::string>{"IncorrectPassword"} &&
               !client.retry_with_distinct_gamer_tag(error).has_value() &&
               client.local_gamer().gamer_tag == "kikots",
           "a password denial must stay final and must not rename the player");
    expect(describe_join_failure("IncorrectPassword").retry_password &&
               !describe_join_failure("GameIsFull").retry_password &&
               describe_join_failure("GameIsFull").title == "SERVER IS FULL" &&
               describe_join_failure("some transport text").detail == "some transport text",
           "denials must map to precise reasons and unknown text must pass through");

    // Tags stay inside the stock 128-byte bound without splitting UTF-8: the cut
    // for the suffix lands inside a two-byte code point and must back out of it.
    std::string long_tag(123U, 'a');
    long_tag += "\xC3\xA9";
    long_tag += "aaa";
    const std::string distinct = distinct_gamer_tag(long_tag, 2U);
    expect(long_tag.size() == 128U && distinct == std::string(123U, 'a') + " (2)" &&
               distinct_gamer_tag("kikots's world", 1U) == "kikots's world" &&
               distinct_gamer_tag("kikots", 4U) == "kikots (4)",
           "distinct tags must be bounded, UTF-8 safe, and leave the first attempt untouched");
}

DiscoveredSession lobby(std::uint64_t id, std::string name, std::string host_name,
                        PeerId host_peer, ProtocolVersion protocol)
{
    DiscoveredSession session;
    session.session_id = id;
    session.server_name = std::move(name);
    session.host_name = std::move(host_name);
    session.server_message = "Tue, 06 Oct 2026 01:00:00 GMT";
    session.properties = make_session_properties(host_settings(protocol), false, 1, 0, false);
    session.current_players = 1U;
    session.maximum_players = 8U;
    session.host_peer = host_peer;
    return session;
}

void own_lobby_is_identified_by_steam_id_never_by_name()
{
    constexpr PeerId local_peer = 76'561'198'000'000'001ULL;
    constexpr PeerId other_peer = 76'561'198'000'000'002ULL;

    SteamBridgeDiscoveryResults results;
    results.local_peer = local_peer;
    results.persona_name = "kikots";
    SteamBridgeDiscoveredLobby same_name;
    same_name.lobby_id = 11U;
    same_name.host_peer = other_peer;
    same_name.current_players = 1U;
    same_name.maximum_players = 8U;
    same_name.name = "kikots's world";
    same_name.host_name = "kikots"; // another account with the local persona
    same_name.created_at = "Tue, 06 Oct 2026 01:00:00 GMT";
    same_name.session_properties =
        make_session_properties(host_settings(ProtocolVersion::classic_v3), false, 1, 0, false);
    SteamBridgeDiscoveredLobby own = same_name;
    own.lobby_id = 12U;
    own.host_peer = local_peer;
    own.name = "left behind";
    own.host_name = "a renamed persona"; // the local account under an older name
    SteamBridgeDiscoveredLobby modern = same_name;
    modern.lobby_id = 13U;
    modern.name = "kikots's modern world";
    modern.session_properties =
        make_session_properties(host_settings(ProtocolVersion::modern_v4), false, 1, 0, false);
    results.lobbies = {same_name, own, modern};

    const std::vector<DiscoveredSession> sessions = make_discovered_sessions(results);
    expect(sessions.size() == 3U && sessions[0].host_peer == other_peer &&
               sessions[1].host_peer == local_peer && sessions[0].server_name == "kikots's world",
           "discovery must carry the lobby owner's Steam id and an apostrophe-bearing name");

    SessionBrowser browser;
    JoinBrowserFlow flow;
    static_cast<void>(flow.discovery_succeeded(browser, sessions, results.local_peer));
    expect(browser.sessions().size() == 3U && browser.visible_indices().size() == 2U &&
               browser.own_session_count() == 1U && !browser.visible_row_of(12U).has_value() &&
               browser.visible_row_of(11U).has_value() && browser.visible_row_of(13U).has_value(),
           "only the lobby owned by the local Steam id may be withheld; a host that merely "
           "shares the local name must be listed for both protocols");

    ModManifest classic;
    ModManifest modern_manifest;
    modern_manifest.protocol = ProtocolVersion::modern_v4;
    expect(assess_join(sessions[0], classic, true, local_peer).joinable() &&
               assess_join(sessions[2], modern_manifest, true, local_peer).joinable() &&
               assess_join(sessions[2], modern_manifest, true, local_peer).protocol ==
                   ProtocolVersion::modern_v4 &&
               assess_join(sessions[1], classic, true, local_peer).status ==
                   JoinCompatibility::own_session &&
               assess_join(sessions[1], classic, true, 0U).joinable(),
           "joinability must follow the owner id for Classic and Modern, never the host name");

    // Asking to join the own lobby anyway fails at once with the reason.
    const JoinBrowserAction refused =
        flow.request_join(12U, {}, JoinTransportView{false, true, false}, browser);
    expect(refused.kind == JoinBrowserAction::Kind::none && refused.failure.has_value() &&
               refused.failure->title == "YOUR OWN LOBBY" && !flow.join_pending(),
           "an own lobby must be refused before any transport work, with its reason");
}

void selection_follows_the_lobby_id_across_refreshes()
{
    const ProtocolVersion classic = ProtocolVersion::classic_v3;
    SessionBrowser browser;
    browser.replace({lobby(1U, "ALPHA", "A", 101U, classic), lobby(2U, "BRAVO", "B", 102U, classic),
                     lobby(3U, "CHARLIE", "C", 103U, classic)});
    const auto row_ids = [&] {
        std::vector<std::uint64_t> ids;
        for (const std::size_t index : browser.visible_indices())
            ids.push_back(browser.sessions()[index].session_id);
        return ids;
    };
    const std::vector<std::uint64_t> before = row_ids();
    expect(before == std::vector<std::uint64_t>{1U, 2U, 3U}, "rows sort by name");
    browser.select(2U);
    expect(browser.selected_index() == std::optional<std::size_t>{1U},
           "selecting a row must resolve to its session");

    // The refresh returns the same servers in another order, renames one so the
    // sort changes, and adds a new one.
    DiscoveredSession renamed = lobby(2U, "ZULU (WAS BRAVO)", "B", 102U, classic);
    renamed.current_players = 5U;
    browser.replace({lobby(3U, "CHARLIE", "C", 103U, classic), renamed,
                     lobby(4U, "AARDVARK", "D", 104U, classic),
                     lobby(1U, "ALPHA", "A", 101U, classic)});
    const std::vector<std::uint64_t> after = row_ids();
    expect(after == std::vector<std::uint64_t>{4U, 1U, 3U, 2U}, "the refresh reordered the rows");
    const std::optional<std::size_t> selected = browser.selected_index();
    expect(selected.has_value() && browser.sessions()[*selected].session_id == 2U &&
               browser.sessions()[*selected].current_players == 5U,
           "the selection must still be the same lobby, with its refreshed data");
    expect(browser.sessions()[1U].session_id == 2U && before[1U] == 2U && after[1U] == 1U,
           "an index-based selection would now target a different server");

    // The highlighted row follows its server; action buttons keep their slot.
    expect(remap_browser_selection(1U, before, after) == 3U,
           "the highlight must follow the highlighted lobby id");
    expect(remap_browser_selection(3U, before, after) == 4U &&
               remap_browser_selection(6U, before, after) == 7U,
           "Refresh and Back must stay highlighted when the row count changes");

    // The selected server disappears: the selection expires instead of sliding.
    browser.replace({lobby(1U, "ALPHA", "A", 101U, classic)});
    const std::vector<std::uint64_t> shrunk = row_ids();
    expect(!browser.selected_index().has_value() && browser.selected_id() == 2U,
           "an expired row must not resolve to whatever took its position");
    expect(remap_browser_selection(3U, after, shrunk) == 1U &&
               remap_browser_selection(0U, after, shrunk) == 0U &&
               remap_browser_selection(1U, after, shrunk) == 0U,
           "a highlight whose row expired must stay in range and never invent a row");
    // It resolves again as soon as the lobby is listed again.
    browser.replace({lobby(2U, "BRAVO", "B", 102U, classic)});
    expect(browser.selected_index() == std::optional<std::size_t>{0U},
           "a transient miss must not lose the selection");
}

void a_stale_row_joins_after_one_automatic_rediscovery(ProtocolVersion protocol)
{
    SessionBrowser browser;
    JoinBrowserFlow flow;
    constexpr PeerId local_peer = 500U;
    static_cast<void>(flow.discovery_succeeded(
        browser, {lobby(7U, "kikots's world", "kikots", 600U, protocol)}, local_peer));

    // The helper that produced the list is gone (a failed join, or the player came
    // back from a server). The row is still on screen.
    const JoinTransportView no_helper{false, false, false};
    const JoinBrowserAction queued = flow.request_join(7U, "ore", no_helper, browser);
    expect(queued.kind == JoinBrowserAction::Kind::start_discovery && queued.quiet &&
               !queued.failure.has_value() && flow.join_pending() && flow.pending_session() == 7U,
           "joining a stale row must start discovery by itself, never ask for a manual refresh");

    // The lobby is still there, with changed data. The join uses the fresh row.
    DiscoveredSession fresh = lobby(7U, "kikots's world", "kikots", 600U, protocol);
    fresh.current_players = 3U;
    fresh.password_required = true;
    const JoinBrowserAction started =
        flow.discovery_succeeded(browser, {lobby(8U, "OTHER", "x", 601U, protocol), fresh},
                                 local_peer);
    expect(started.kind == JoinBrowserAction::Kind::start_join && started.session_id == 7U &&
               started.protocol == protocol && started.password == "ore" &&
               !started.failure.has_value() && !flow.join_pending(),
           "a stale but living row must join on the re-resolved lobby id and protocol");

    // The same request when the lobby died: fail fast, with the reason.
    static_cast<void>(flow.request_join(7U, {}, no_helper, browser));
    const JoinBrowserAction dead = flow.discovery_succeeded(
        browser, {lobby(8U, "OTHER", "x", 601U, protocol)}, local_peer);
    expect(dead.kind == JoinBrowserAction::Kind::none && dead.failure.has_value() &&
               dead.failure->title == "SERVER NO LONGER AVAILABLE" && !flow.join_pending(),
           "a dead row must fail fast with a clear message and no join attempt");

    // The lobby filled up, or stopped accepting players, while the row was stale.
    static_cast<void>(flow.request_join(8U, {}, no_helper, browser));
    DiscoveredSession full = lobby(8U, "OTHER", "x", 601U, protocol);
    full.current_players = full.maximum_players;
    const JoinBrowserAction full_result = flow.discovery_succeeded(browser, {full}, local_peer);
    expect(full_result.kind == JoinBrowserAction::Kind::none && full_result.failure.has_value() &&
               full_result.failure->title == "SERVER IS FULL",
           "a server that filled up must be refused with that reason");
    static_cast<void>(flow.request_join(8U, {}, no_helper, browser));
    DiscoveredSession closed = lobby(8U, "OTHER", "x", 601U, protocol);
    closed.properties[1] = 1;
    const JoinBrowserAction closed_result =
        flow.discovery_succeeded(browser, {closed}, local_peer);
    expect(closed_result.failure.has_value() &&
               closed_result.failure->title == "SERVER NO LONGER AVAILABLE",
           "a session that closed to joins must be refused with that reason");

    // A row the live helper no longer lists is re-resolved too, not refused blind.
    const JoinBrowserAction unlisted =
        flow.request_join(7U, {}, JoinTransportView{false, true, false}, browser);
    expect(unlisted.kind == JoinBrowserAction::Kind::start_discovery && flow.join_pending(),
           "an unlisted row must be looked up again before it is declared dead");
    flow.cancel_join();
    expect(!flow.join_pending(), "cancelling must drop the queued join");
}

void a_join_requested_during_a_refresh_starts_when_the_refresh_ends(ProtocolVersion protocol)
{
    SessionBrowser browser;
    JoinBrowserFlow flow;
    constexpr PeerId local_peer = 500U;
    static_cast<void>(flow.discovery_succeeded(
        browser, {lobby(7U, "kikots's world", "kikots", 600U, protocol)}, local_peer));

    // A periodic refresh is in flight; the helper cannot take a join yet.
    const JoinTransportView refreshing{true, false, false};
    const JoinBrowserAction queued = flow.request_join(7U, "pw", refreshing, browser);
    expect(queued.kind == JoinBrowserAction::Kind::none && !queued.failure.has_value() &&
               flow.join_pending(),
           "a join during a refresh must be accepted and queued, not refused");
    expect(flow.tick(5.0F, true, refreshing).kind == JoinBrowserAction::Kind::none &&
               flow.join_pending(),
           "nothing else may start while the refresh is in flight");
    const JoinBrowserAction started = flow.discovery_succeeded(
        browser, {lobby(7U, "kikots's world", "kikots", 600U, protocol)}, local_peer);
    expect(started.kind == JoinBrowserAction::Kind::start_join && started.session_id == 7U &&
               started.password == "pw" && started.protocol == protocol,
           "the queued join must start from the refresh that was in flight");

    // With an idle helper and a listed row the join starts immediately.
    const JoinBrowserAction immediate =
        flow.request_join(7U, {}, JoinTransportView{false, true, false}, browser);
    expect(immediate.kind == JoinBrowserAction::Kind::start_join && !flow.join_pending(),
           "an idle helper must take the join without another discovery");

    // The refresh in flight fails: the queued join reports why, the browser keeps
    // its rows once, expires them on the second failure, and retries by itself.
    static_cast<void>(flow.request_join(7U, {}, refreshing, browser));
    const JoinBrowserAction failed =
        flow.discovery_failed(browser, "SteamAPI_Init returned false");
    expect(failed.kind == JoinBrowserAction::Kind::none && failed.failure.has_value() &&
               failed.failure->title == "COULD NOT REACH STEAM" &&
               failed.failure->detail == "SteamAPI_Init returned false" && !flow.join_pending(),
           "a failed refresh must fail the queued join with the transport reason");
    expect(browser.sessions().size() == 1U && flow.consecutive_failures() == 1U,
           "one failed refresh must not empty the list");
    const JoinTransportView idle_dead{false, false, false};
    expect(flow.tick(1.9F, true, idle_dead).kind == JoinBrowserAction::Kind::none &&
               flow.tick(0.2F, true, idle_dead).kind == JoinBrowserAction::Kind::start_discovery,
           "a failed refresh must be retried automatically after a short delay");
    static_cast<void>(flow.discovery_failed(browser, "SteamAPI_Init returned false"));
    expect(browser.sessions().empty() && flow.consecutive_failures() == 2U &&
               flow.seconds_until_refresh() == 4.0F,
           "rows that could not be confirmed twice must expire, with a growing retry delay");
    for (int attempt = 0; attempt < 6; ++attempt)
        static_cast<void>(flow.discovery_failed(browser, "offline"));
    expect(flow.seconds_until_refresh() == JoinBrowserFlow::maximum_retry_seconds,
           "the retry delay must be capped");

    // A join queued behind an operation that ended without a discovery (a join
    // that was cancelled) is picked up by the next tick.
    static_cast<void>(flow.discovery_succeeded(
        browser, {lobby(7U, "kikots's world", "kikots", 600U, protocol)}, local_peer));
    static_cast<void>(flow.request_join(7U, {}, refreshing, browser));
    expect(flow.tick(0.016F, false, idle_dead).kind == JoinBrowserAction::Kind::start_discovery,
           "a queued join must restart discovery even when the browser is not on screen");
}

void the_browser_refreshes_itself_and_recovers_after_a_failed_join()
{
    SessionBrowser browser;
    JoinBrowserFlow flow;
    const JoinTransportView idle{false, true, false};
    const JoinTransportView busy{true, false, false};
    const JoinTransportView dead{false, false, false};
    const JoinTransportView joined{false, false, true};
    const ProtocolVersion classic = ProtocolVersion::classic_v3;

    expect(flow.tick(1.0F, false, dead).kind == JoinBrowserAction::Kind::none,
           "nothing is refreshed while the browser is closed");
    flow.browser_opened();
    const JoinBrowserAction on_open = flow.tick(0.016F, true, dead);
    expect(on_open.kind == JoinBrowserAction::Kind::start_discovery && on_open.quiet,
           "opening the browser must refresh without being asked");
    expect(flow.tick(60.0F, true, busy).kind == JoinBrowserAction::Kind::none,
           "a refresh in flight must never be doubled");
    static_cast<void>(
        flow.discovery_succeeded(browser, {lobby(1U, "ALPHA", "A", 101U, classic)}, 500U));
    expect(flow.tick(JoinBrowserFlow::refresh_interval_seconds - 0.5F, true, idle).kind ==
                   JoinBrowserAction::Kind::none &&
               flow.tick(1.0F, true, idle).kind == JoinBrowserAction::Kind::start_discovery,
           "an open browser must refresh periodically");
    static_cast<void>(
        flow.discovery_succeeded(browser, {lobby(1U, "ALPHA", "A", 101U, classic)}, 500U));

    // A manual refresh pressed while one is in flight is satisfied by it.
    flow.request_refresh();
    expect(flow.tick(0.016F, true, busy).kind == JoinBrowserAction::Kind::none,
           "a manual refresh waits for the operation in flight");
    static_cast<void>(
        flow.discovery_succeeded(browser, {lobby(1U, "ALPHA", "A", 101U, classic)}, 500U));
    expect(flow.tick(0.016F, true, idle).kind == JoinBrowserAction::Kind::none,
           "a completed discovery satisfies the latched manual refresh");
    flow.request_refresh();
    const JoinBrowserAction manual = flow.tick(0.016F, true, idle);
    expect(manual.kind == JoinBrowserAction::Kind::start_discovery && !manual.quiet,
           "a manual refresh must run at once and may report its result");
    static_cast<void>(
        flow.discovery_succeeded(browser, {lobby(1U, "ALPHA", "A", 101U, classic)}, 500U));

    // The host refuses the join. The helper is gone, but the browser recovers on
    // the very next tick and the reason is ready for the screen.
    const JoinFailureNotice notice = flow.join_failed("GamerAlreadyConnected");
    expect(notice.title == "ALREADY IN THIS SESSION",
           "the host's refusal must be explained to the player");
    expect(flow.tick(0.016F, true, dead).kind == JoinBrowserAction::Kind::start_discovery,
           "after a failed join the browser must restore its transport by itself");

    // While in a session the browser leaves the helper alone; afterwards it
    // refreshes as soon as it is shown again.
    static_cast<void>(
        flow.discovery_succeeded(browser, {lobby(1U, "ALPHA", "A", 101U, classic)}, 500U));
    expect(flow.tick(600.0F, true, joined).kind == JoinBrowserAction::Kind::none,
           "a joined session owns the helper");
    flow.transport_lost();
    expect(flow.tick(0.016F, false, dead).kind == JoinBrowserAction::Kind::none &&
               flow.tick(0.016F, true, dead).kind == JoinBrowserAction::Kind::start_discovery,
           "returning from a server must leave the browser working");
    const JoinBrowserAction in_session = flow.request_join(1U, {}, joined, browser);
    expect(in_session.kind == JoinBrowserAction::Kind::none && in_session.failure.has_value(),
           "a second join while in a session must be refused with its reason");

    JoinProgressWatchdog watchdog;
    expect(!watchdog.stalled(19.0F, 20.0F) && watchdog.stalled(1.0F, 20.0F),
           "a silent host must end the wait");
    watchdog.restart();
    expect(!watchdog.stalled(19.0F, 20.0F), "progress must restart the wait");
}

struct TestCase final
{
    std::string_view name;
    std::function<void()> body;
};

} // namespace

int main()
{
    const auto both = [](void (*test)(ProtocolVersion)) {
        return [test] {
            test(ProtocolVersion::classic_v3);
            test(ProtocolVersion::modern_v4);
        };
    };
    const std::vector<TestCase> tests{
        {"same_name_join_succeeds_against_the_stock_duplicate_rule",
         both(same_name_join_succeeds_against_the_stock_duplicate_rule)},
        {"same_name_join_succeeds_against_the_openminerz_host",
         both(same_name_join_succeeds_against_the_openminerz_host)},
        {"display_tag_retry_is_bounded_and_only_for_the_duplicate_denial",
         both(display_tag_retry_is_bounded_and_only_for_the_duplicate_denial)},
        {"own_lobby_is_identified_by_steam_id_never_by_name",
         own_lobby_is_identified_by_steam_id_never_by_name},
        {"selection_follows_the_lobby_id_across_refreshes",
         selection_follows_the_lobby_id_across_refreshes},
        {"a_stale_row_joins_after_one_automatic_rediscovery",
         both(a_stale_row_joins_after_one_automatic_rediscovery)},
        {"a_join_requested_during_a_refresh_starts_when_the_refresh_ends",
         both(a_join_requested_during_a_refresh_starts_when_the_refresh_ends)},
        {"the_browser_refreshes_itself_and_recovers_after_a_failed_join",
         the_browser_refreshes_itself_and_recovers_after_a_failed_join},
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
