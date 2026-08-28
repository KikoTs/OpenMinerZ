#include "openminerz/net/admission.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace openminerz::net
{
namespace
{

bool constant_time_equal(std::string_view expected, std::string_view actual) noexcept
{
    const std::size_t count = std::max(expected.size(), actual.size());
    std::size_t difference = expected.size() ^ actual.size();
    for (std::size_t index = 0U; index < count; ++index)
    {
        const unsigned char left =
            index < expected.size() ? static_cast<unsigned char>(expected[index]) : 0U;
        const unsigned char right =
            index < actual.size() ? static_cast<unsigned char>(actual[index]) : 0U;
        difference |= static_cast<std::size_t>(left ^ right);
    }
    return difference == 0U;
}

} // namespace

HostAdmissionGate::HostAdmissionGate(HostSessionSettings settings, bool game_begun,
                                     std::int32_t game_mode, std::int32_t difficulty,
                                     bool infinite_resources, bool host_occupies_player_slot)
    : settings_{std::move(settings)},
      properties_{make_session_properties(settings_, game_begun, game_mode, difficulty,
                                          infinite_resources)},
      host_occupies_player_slot_{host_occupies_player_slot}
{
}

AdmissionDecision HostAdmissionGate::begin(const PeerAdmissionRequest &request)
{
    if (request.peer == 0U)
        return {AdmissionStatus::rejected_platform_policy, "Peer identity is invalid."};
    if (const auto existing = peers_.find(request.peer);
        existing != peers_.end() && existing->second.status != AdmissionStatus::disconnected)
        return {AdmissionStatus::rejected_duplicate, "Peer already has an admission attempt."};

    const std::size_t occupied = reserved_peer_count() + (host_occupies_player_slot_ ? 1U : 0U);
    if (occupied >= static_cast<std::size_t>(settings_.maximum_players))
    {
        peers_[request.peer] = {AdmissionStatus::rejected_full};
        return {AdmissionStatus::rejected_full, "Session is full."};
    }
    if (settings_.join_policy != HostJoinPolicy::anyone && !request.platform_authorized)
    {
        peers_[request.peer] = {AdmissionStatus::rejected_platform_policy};
        return {AdmissionStatus::rejected_platform_policy,
                "Peer is not authorized by the host join policy."};
    }
    if (request.password.size() > 256U ||
        !constant_time_equal(settings_.password, request.password))
    {
        peers_[request.peer] = {AdmissionStatus::rejected_password};
        return {AdmissionStatus::rejected_password, "Incorrect session password."};
    }
    if (request.properties != properties_)
    {
        peers_[request.peer] = {AdmissionStatus::rejected_session_properties};
        return {AdmissionStatus::rejected_session_properties,
                "Session properties do not match the host."};
    }
    if (requires_extension(settings_.mod_manifest) && !request.advertised_extension)
    {
        peers_[request.peer] = {AdmissionStatus::rejected_extension_required};
        return {AdmissionStatus::rejected_extension_required,
                "This session requires OpenMinerZ mod negotiation."};
    }

    const AdmissionStatus accepted = request.advertised_extension
                                         ? AdmissionStatus::awaiting_extension
                                         : AdmissionStatus::admitted;
    peers_[request.peer] = {accepted};
    return {accepted, accepted == AdmissionStatus::admitted
                          ? "Stock-compatible peer admitted."
                          : "Waiting for OpenMinerZ extension hello."};
}

ExtensionAdmissionResult HostAdmissionGate::reject_extension(PeerId peer, std::uint64_t nonce,
                                                             AdmissionStatus status,
                                                             std::string detail)
{
    peers_[peer] = {status};
    ExtensionMessage rejection;
    rejection.kind = ExtensionMessageKind::server_reject;
    rejection.nonce = nonce;
    rejection.manifest.protocol = settings_.protocol;
    rejection.detail = detail.substr(0U, maximum_extension_detail_bytes);
    return {{status, std::move(detail)}, encode_extension_message(rejection)};
}

ExtensionAdmissionResult HostAdmissionGate::receive_extension(PeerId peer,
                                                              std::span<const std::uint8_t> packet)
{
    const auto found = peers_.find(peer);
    if (found == peers_.end() || found->second.status != AdmissionStatus::awaiting_extension)
        return {{AdmissionStatus::rejected_duplicate,
                 "Peer is not waiting for an extension handshake."},
                {}};

    ExtensionMessage hello;
    ExtensionDecodeError decode_error{};
    if (!decode_extension_message(packet, hello, decode_error) ||
        hello.kind != ExtensionMessageKind::client_hello)
        return reject_extension(peer, 0U, AdmissionStatus::rejected_malformed_extension,
                                "Malformed OpenMinerZ extension hello.");

    const ModCompatibilityResult compatibility =
        compare_mod_manifests(settings_.mod_manifest, &hello.manifest, true);
    if (!compatibility.compatible())
        return reject_extension(peer, hello.nonce, AdmissionStatus::rejected_mod_manifest,
                                compatibility.detail);

    found->second.status = AdmissionStatus::admitted;
    ExtensionMessage acceptance;
    acceptance.kind = ExtensionMessageKind::server_accept;
    acceptance.nonce = hello.nonce;
    acceptance.manifest = settings_.mod_manifest;
    return {{AdmissionStatus::admitted, "OpenMinerZ peer admitted."},
            encode_extension_message(acceptance)};
}

bool HostAdmissionGate::may_forward_gameplay(PeerId peer) const noexcept
{
    const auto found = peers_.find(peer);
    return found != peers_.end() && found->second.status == AdmissionStatus::admitted;
}

AdmissionStatus HostAdmissionGate::status(PeerId peer) const noexcept
{
    const auto found = peers_.find(peer);
    return found == peers_.end() ? AdmissionStatus::disconnected : found->second.status;
}

void HostAdmissionGate::disconnect(PeerId peer) noexcept
{
    peers_.erase(peer);
}

std::size_t HostAdmissionGate::reserved_peer_count() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(peers_.begin(), peers_.end(), [](const auto &entry) {
            return entry.second.status == AdmissionStatus::awaiting_extension ||
                   entry.second.status == AdmissionStatus::admitted;
        }));
}

std::size_t HostAdmissionGate::admitted_peer_count() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(peers_.begin(), peers_.end(), [](const auto &entry) {
            return entry.second.status == AdmissionStatus::admitted;
        }));
}

} // namespace openminerz::net
