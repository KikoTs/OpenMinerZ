#include "openminerz/net/avatar_sync.hpp"

#include "openminerz/net/payload_codec.hpp"

#include <algorithm>

// Every rule here is the add-on's own (AvatarNetworkBridge and
// ZZAvatarSyncMessage in KikoTs/castleforge-xbox-avatar,
// src/XboxAvatar/Runtime/AvatarRuntime.cs). tools/OriginalClientInterop/
// AvatarAddonProbe executes the add-on's assembly against these encoders and
// decoders; tests/test_xbox_avatar.cpp pins the same bytes without it.

namespace openminerz::net
{
namespace
{

// How soon a second manifest may follow the first to the same peer. The
// receiving add-on drops any manifest within five seconds of the last one it
// accepted, so a re-offer waits that out rather than being silently lost.
constexpr double reoffer_interval_seconds = avatar_manifest_interval_seconds + 0.5;
// A repeated hello re-sends the manifest it is waiting on at most this often.
constexpr double hello_repeat_interval_seconds = 1.0;
constexpr double never = -1.0e9;

bool has_prefix_at(std::span<const std::uint8_t> data, std::size_t offset) noexcept
{
    return offset + avatar_marker_prefix.size() <= data.size() &&
           std::equal(avatar_marker_prefix.begin(), avatar_marker_prefix.end(),
                      data.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_avatar_sync_payload(const AvatarSyncMessage &message)
{
    if (message.payload.size() > avatar_chunk_payload)
        return std::nullopt;
    BoundedWriter writer{avatar_sync_header_bytes + avatar_chunk_payload};
    if (!writer.write_u8(message.protocol) || !writer.write_u8(message.kind) ||
        !writer.write_u32(message.transfer_id) || !writer.write_i32(message.total_length) ||
        !writer.write_u16(message.chunk_index) || !writer.write_u16(message.chunk_count) ||
        !writer.write_bytes(message.hash) ||
        !writer.write_u16(static_cast<std::uint16_t>(message.payload.size())) ||
        !writer.write_bytes(message.payload))
        return std::nullopt;
    return writer.take();
}

bool decode_avatar_sync_payload(std::span<const std::uint8_t> payload, AvatarSyncMessage &message)
{
    BoundedReader reader{payload};
    AvatarSyncMessage decoded;
    std::uint16_t length{};
    if (!reader.read_u8(decoded.protocol) || !reader.read_u8(decoded.kind) ||
        !reader.read_u32(decoded.transfer_id) || !reader.read_i32(decoded.total_length) ||
        !reader.read_u16(decoded.chunk_index) || !reader.read_u16(decoded.chunk_count) ||
        !reader.read_bytes(decoded.hash) || !reader.read_u16(length) ||
        length > avatar_chunk_payload || reader.remaining() != length)
        return false;
    decoded.payload.resize(length);
    if (!reader.read_bytes(decoded.payload))
        return false;
    message = std::move(decoded);
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_avatar_sync_packet(std::uint8_t message_id,
                                                                   const AvatarSyncMessage &message)
{
    const auto payload = encode_avatar_sync_payload(message);
    if (!payload.has_value())
        return std::nullopt;
    // Message.DoSendInternal: the id, SendData, then the XOR-8 of both.
    std::vector<std::uint8_t> packet(payload->size() + minimum_packet_size);
    packet[0] = message_id;
    std::copy(payload->begin(), payload->end(), packet.begin() + 1);
    packet.back() = xor8(std::span<const std::uint8_t>{packet}.first(packet.size() - 1U));
    return packet;
}

bool decode_avatar_sync_packet(std::span<const std::uint8_t> packet, std::uint8_t &message_id,
                               AvatarSyncMessage &message)
{
    if (packet.size() < avatar_sync_header_bytes + minimum_packet_size ||
        packet.size() > avatar_sync_header_bytes + avatar_chunk_payload + minimum_packet_size ||
        xor8(packet.first(packet.size() - 1U)) != packet.back())
        return false;
    if (!decode_avatar_sync_payload(packet.subspan(1U, packet.size() - minimum_packet_size), message))
        return false;
    message_id = packet.front();
    return true;
}

bool is_avatar_sync_packet(std::span<const std::uint8_t> packet, std::uint8_t message_id) noexcept
{
    try
    {
        std::uint8_t id{};
        AvatarSyncMessage message;
        return decode_avatar_sync_packet(packet, id, message) && id == message_id;
    }
    catch (...)
    {
        return false;
    }
}

std::uint8_t natural_avatar_message_id(ProtocolVersion protocol) noexcept
{
    return static_cast<std::uint8_t>(message_manifest(protocol).size());
}

bool valid_avatar_message_id(ProtocolVersion protocol, std::uint8_t id) noexcept
{
    return static_cast<std::size_t>(id) >= message_manifest(protocol).size();
}

AvatarCapabilityMarker read_avatar_capability_marker(
    std::span<const std::uint8_t> description) noexcept
{
    AvatarCapabilityMarker marker;
    marker.description_length = description.size();
    const std::size_t with_id = avatar_marker_prefix.size() + 2U;
    const std::size_t without_id = avatar_marker_prefix.size() + 1U;
    if (description.size() >= with_id && has_prefix_at(description, description.size() - with_id) &&
        description[description.size() - 2U] == avatar_marker_with_message_id)
    {
        marker.present = true;
        marker.message_id = description.back();
        marker.description_length = description.size() - with_id;
    }
    else if (description.size() >= without_id &&
             has_prefix_at(description, description.size() - without_id) &&
             description.back() == avatar_protocol_version)
    {
        marker.present = true;
        marker.description_length = description.size() - without_id;
    }
    return marker;
}

std::vector<std::uint8_t> append_avatar_capability_marker(std::span<const std::uint8_t> description,
                                                          std::optional<std::uint8_t> message_id)
{
    std::vector<std::uint8_t> decorated{description.begin(), description.end()};
    decorated.insert(decorated.end(), avatar_marker_prefix.begin(), avatar_marker_prefix.end());
    if (message_id.has_value())
    {
        decorated.push_back(avatar_marker_with_message_id);
        decorated.push_back(*message_id);
    }
    else
        decorated.push_back(avatar_protocol_version);
    return decorated;
}

AvatarCapabilityAnnouncement read_avatar_capability_announcement(
    ProtocolVersion protocol, std::span<const std::uint8_t> application_packet)
{
    AvatarCapabilityAnnouncement announcement;
    DecodedPacket decoded;
    PacketError packet_error{};
    if (!decode_packet(protocol, application_packet, decoded, packet_error) ||
        decoded.message == nullptr || decoded.message->name != "PlayerExistsMessage")
        return announcement;
    PlayerExistsPayload presence;
    PayloadError payload_error{};
    if (!decode_player_exists_payload(decoded.payload, presence, payload_error))
        return announcement;
    const AvatarCapabilityMarker marker = read_avatar_capability_marker(presence.avatar_description);
    if (!marker.present)
        return announcement;
    const std::uint8_t id = marker.message_id.value_or(natural_avatar_message_id(protocol));
    // An id inside the stock table would be read as that stock message.
    if (!valid_avatar_message_id(protocol, id))
        return announcement;
    announcement.present = true;
    announcement.message_id = id;
    announcement.reported_id = marker.message_id.has_value();
    return announcement;
}

std::optional<std::vector<std::uint8_t>> make_avatar_capability_presence(
    ProtocolVersion protocol, std::string_view gamer_tag, std::span<const std::uint8_t> player_id)
{
    const MessageDescriptor *descriptor = message_by_name(protocol, "PlayerExistsMessage");
    if (descriptor == nullptr)
        return std::nullopt;
    PlayerExistsPayload presence;
    presence.request_response = false;
    // CastleMinerZGame.SetupNewGamer: new AvatarDescription(new byte[10]).
    const std::array<std::uint8_t, 10> description{};
    presence.avatar_description = append_avatar_capability_marker(description, std::nullopt);
    presence.gamer_tag = std::string{gamer_tag};
    presence.player_id.assign(player_id.begin(), player_id.end());
    PayloadError payload_error{};
    const auto payload = encode_player_exists_payload(presence, payload_error);
    if (!payload.has_value())
        return std::nullopt;
    PacketError packet_error{};
    return encode_packet(protocol, descriptor->id, *payload, packet_error);
}

bool AvatarCapabilityRegistry::announce(std::uint8_t gid, std::uint8_t message_id,
                                        bool agnostic) noexcept
{
    Entry &entry = entries_[gid];
    entry.capable = true;
    entry.message_id = message_id;
    entry.agnostic = entry.agnostic || agnostic;
    return true;
}

void AvatarCapabilityRegistry::forget(std::uint8_t gid) noexcept
{
    entries_[gid] = {};
}

void AvatarCapabilityRegistry::clear() noexcept
{
    entries_.fill({});
}

bool AvatarCapabilityRegistry::capable(std::uint8_t gid) const noexcept
{
    return entries_[gid].capable;
}

std::optional<std::uint8_t> AvatarCapabilityRegistry::message_id(std::uint8_t gid) const noexcept
{
    return entries_[gid].capable ? std::optional<std::uint8_t>{entries_[gid].message_id}
                                 : std::nullopt;
}

void AvatarCapabilityRegistry::set_agnostic(std::uint8_t gid, bool agnostic) noexcept
{
    entries_[gid].agnostic = agnostic;
}

bool AvatarCapabilityRegistry::agnostic(std::uint8_t gid) const noexcept
{
    return entries_[gid].agnostic;
}

std::size_t AvatarCapabilityRegistry::count() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const Entry &entry) { return entry.capable; }));
}

// ----------------------------------------------------------- AvatarSyncSession

void AvatarSyncSession::reset(ProtocolVersion protocol) noexcept
{
    protocol_ = protocol;
    advertised_ = false;
    peers_.clear();
    offers_.clear();
    outgoing_.clear();
    incoming_.clear();
    sends_.clear();
    received_.clear();
    next_cleanup_ = never;
    last_update_ = never;
    chunk_credit_ = 0.0;
}

void AvatarSyncSession::set_policy(bool offer_local, bool request_remote, double now)
{
    static_cast<void>(now);
    if (offer_local != offer_local_)
    {
        offer_local_ = offer_local;
        if (!offer_local_)
        {
            offers_.clear();
            outgoing_.clear();
        }
        for (auto &[gid, peer] : peers_)
            peer.offer_due = offer_local_ && local_bytes_ != nullptr;
    }
    if (request_remote != request_remote_)
    {
        request_remote_ = request_remote;
        if (!request_remote_)
            incoming_.clear();
        for (auto &[gid, peer] : peers_)
        {
            // Asking again is the only way to be told what a peer wears now.
            peer.hello_pending = request_remote_;
            peer.applied.reset();
            if (!request_remote_)
                peer.hello_sent = false;
        }
    }
}

void AvatarSyncSession::set_local_avatar(std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                                         const AvatarHash &hash, double now)
{
    static_cast<void>(now);
    if (bytes != nullptr && (bytes->empty() || bytes->size() > avatar_maximum_asset_bytes))
        bytes.reset();
    if ((bytes == nullptr) == (local_bytes_ == nullptr) && (bytes == nullptr || hash == local_hash_))
        return;
    local_bytes_ = std::move(bytes);
    local_hash_ = hash;
    // Whatever was on offer or in flight is the previous avatar.
    offers_.clear();
    outgoing_.clear();
    for (auto &[gid, peer] : peers_)
        peer.offer_due = offer_local_ && local_bytes_ != nullptr;
}

void AvatarSyncSession::set_advertised(bool advertised) noexcept
{
    advertised_ = advertised;
}

void AvatarSyncSession::peer_capable(std::uint8_t gid, std::uint8_t message_id, double now)
{
    static_cast<void>(now);
    if (!valid_avatar_message_id(protocol_, message_id))
        return;
    if (const auto found = peers_.find(gid); found != peers_.end())
    {
        if (found->second.message_id == message_id)
            return;
        // The same gamer id now speaks another id: a different client.
        peer_left(gid);
    }
    Peer peer;
    peer.message_id = message_id;
    peer.hello_pending = request_remote_;
    peer.offer_due = offer_local_ && local_bytes_ != nullptr;
    peers_.emplace(gid, peer);
}

void AvatarSyncSession::peer_left(std::uint8_t gid) noexcept
{
    peers_.erase(gid);
    std::erase_if(offers_, [gid](const Offer &offer) { return offer.gid == gid; });
    std::erase_if(outgoing_, [gid](const Outgoing &transfer) { return transfer.gid == gid; });
    drop_incoming_for(gid);
    std::erase_if(sends_, [gid](const AvatarSyncSend &send) { return send.recipient_gid == gid; });
    std::erase_if(received_, [gid](const AvatarReceived &done) { return done.gid == gid; });
}

void AvatarSyncSession::queue(std::uint8_t gid, const AvatarSyncMessage &message)
{
    // The gate: a packet exists only for a peer that announced the add-on.
    const auto peer = peers_.find(gid);
    if (peer == peers_.end())
        return;
    auto packet = encode_avatar_sync_packet(peer->second.message_id, message);
    if (!packet.has_value())
        return;
    sends_.push_back({gid, std::move(*packet)});
}

std::uint32_t AvatarSyncSession::next_transfer_id() noexcept
{
    const std::uint32_t result = next_transfer_id_++;
    if (next_transfer_id_ == 0U)
        next_transfer_id_ = 1U;
    return result;
}

void AvatarSyncSession::offer(std::uint8_t gid, double now)
{
    const auto peer = peers_.find(gid);
    if (peer == peers_.end() || local_bytes_ == nullptr || !offer_local_)
        return;
    std::erase_if(offers_, [gid](const Offer &existing) { return existing.gid == gid; });
    Offer created;
    created.gid = gid;
    created.transfer_id = next_transfer_id();
    created.bytes = local_bytes_;
    created.hash = local_hash_;
    created.expires = now + avatar_transfer_timeout_seconds;
    AvatarSyncMessage manifest;
    manifest.kind = static_cast<std::uint8_t>(AvatarPacketKind::manifest);
    manifest.transfer_id = created.transfer_id;
    manifest.total_length = static_cast<std::int32_t>(created.bytes->size());
    manifest.chunk_count = static_cast<std::uint16_t>(avatar_chunk_count(created.bytes->size()));
    manifest.hash = created.hash;
    offers_.push_back(std::move(created));
    queue(gid, manifest);
    peer->second.last_manifest_sent = now;
    peer->second.offer_due = false;
    ++counters_.manifests_sent;
}

void AvatarSyncSession::handle_hello(std::uint8_t gid, double now)
{
    const auto peer = peers_.find(gid);
    if (peer == peers_.end() || !offer_local_ || local_bytes_ == nullptr)
        return;
    // One offer and one transfer per peer at a time: a peer saying hello at
    // packet rate must not fill the queue with copies of the same avatar.
    if (std::any_of(outgoing_.begin(), outgoing_.end(),
                    [gid](const Outgoing &transfer) { return transfer.gid == gid; }))
        return;
    const auto pending = std::find_if(offers_.begin(), offers_.end(), [&](const Offer &existing) {
        return existing.gid == gid && existing.expires > now;
    });
    if (pending != offers_.end())
    {
        // The offer may have been made before this peer could accept it (an
        // add-on client only takes a manifest from a peer it has greeted).
        // Repeat it instead of leaving the hello unanswered - bounded, so a
        // flood of hellos is answered once a second, not once each.
        peer->second.repeat_due = true;
        repeat_offer(gid, now);
        return;
    }
    offer(gid, now);
}

void AvatarSyncSession::repeat_offer(std::uint8_t gid, double now)
{
    const auto peer = peers_.find(gid);
    if (peer == peers_.end() || !peer->second.repeat_due)
        return;
    const auto pending = std::find_if(offers_.begin(), offers_.end(), [&](const Offer &existing) {
        return existing.gid == gid && existing.expires > now;
    });
    if (pending == offers_.end())
    {
        peer->second.repeat_due = false;
        return;
    }
    if (now - peer->second.last_manifest_sent < hello_repeat_interval_seconds)
        return;
    AvatarSyncMessage manifest;
    manifest.kind = static_cast<std::uint8_t>(AvatarPacketKind::manifest);
    manifest.transfer_id = pending->transfer_id;
    manifest.total_length = static_cast<std::int32_t>(pending->bytes->size());
    manifest.chunk_count = static_cast<std::uint16_t>(avatar_chunk_count(pending->bytes->size()));
    manifest.hash = pending->hash;
    pending->expires = now + avatar_transfer_timeout_seconds;
    queue(gid, manifest);
    peer->second.last_manifest_sent = now;
    peer->second.repeat_due = false;
    ++counters_.manifests_sent;
}

void AvatarSyncSession::handle_manifest(std::uint8_t gid, const AvatarSyncMessage &message,
                                        double now)
{
    const auto peer = peers_.find(gid);
    if (peer == peers_.end() || !request_remote_)
        return;
    if (message.total_length <= 0 ||
        static_cast<std::size_t>(message.total_length) > avatar_maximum_asset_bytes ||
        message.chunk_count != avatar_chunk_count(static_cast<std::size_t>(message.total_length)))
        return;
    // Only a peer we asked may answer, and not faster than the answer can be
    // used: each one costs a hash and possibly a rebuilt model.
    if (!peer->second.hello_sent)
        return;
    if (now - peer->second.last_manifest_received < avatar_manifest_interval_seconds)
    {
        // Too soon to act on. The add-on drops it here and the new avatar is
        // never seen; but the offer behind it stands for forty-five seconds, so
        // the latest one is kept and answered once the interval has passed
        // (update). One header per peer: nothing a flood can grow.
        AvatarSyncMessage kept = message;
        kept.payload.clear();
        peer->second.deferred_manifest = std::move(kept);
        return;
    }
    peer->second.deferred_manifest.reset();
    peer->second.last_manifest_received = now;
    if (peer->second.applied.has_value() && *peer->second.applied == message.hash)
        return;
    // This sender's own stalled transfer gives way before the cap is judged.
    drop_incoming_for(gid);
    if (incoming_.size() >= avatar_maximum_incoming_transfers)
        return;
    Incoming transfer;
    transfer.gid = gid;
    transfer.transfer_id = message.transfer_id;
    transfer.total_length = message.total_length;
    transfer.chunk_count = message.chunk_count;
    transfer.hash = message.hash;
    transfer.bytes.resize(static_cast<std::size_t>(message.total_length));
    transfer.received.assign(message.chunk_count, false);
    transfer.expires = now + avatar_first_chunk_timeout_seconds;
    incoming_.push_back(std::move(transfer));
    AvatarSyncMessage request;
    request.kind = static_cast<std::uint8_t>(AvatarPacketKind::request);
    request.transfer_id = message.transfer_id;
    request.total_length = message.total_length;
    request.chunk_count = message.chunk_count;
    request.hash = message.hash;
    queue(gid, request);
    ++counters_.requests_sent;
}

void AvatarSyncSession::handle_request(std::uint8_t gid, const AvatarSyncMessage &message,
                                       double now)
{
    const auto found = std::find_if(offers_.begin(), offers_.end(), [&](const Offer &existing) {
        return existing.gid == gid && existing.transfer_id == message.transfer_id;
    });
    if (found == offers_.end() || now > found->expires || message.total_length < 0 ||
        static_cast<std::size_t>(message.total_length) != found->bytes->size() ||
        message.chunk_count != avatar_chunk_count(found->bytes->size()) || message.hash != found->hash)
        return;
    Outgoing transfer;
    transfer.gid = gid;
    transfer.transfer_id = found->transfer_id;
    transfer.bytes = found->bytes;
    transfer.hash = found->hash;
    transfer.chunk_count = message.chunk_count;
    offers_.erase(found);
    outgoing_.push_back(std::move(transfer));
}

void AvatarSyncSession::handle_chunk(std::uint8_t gid, const AvatarSyncMessage &message, double now)
{
    const auto found = std::find_if(incoming_.begin(), incoming_.end(), [&](const Incoming &existing) {
        return existing.gid == gid && existing.transfer_id == message.transfer_id;
    });
    if (found == incoming_.end() || message.total_length != found->total_length ||
        message.chunk_count != found->chunk_count || message.chunk_index >= found->chunk_count ||
        message.hash != found->hash)
        return;
    const std::size_t offset = static_cast<std::size_t>(message.chunk_index) * avatar_chunk_payload;
    const std::size_t expected =
        std::min(avatar_chunk_payload, static_cast<std::size_t>(found->total_length) - offset);
    if (message.payload.size() != expected)
        return;
    if (!found->received[message.chunk_index])
    {
        std::copy(message.payload.begin(), message.payload.end(),
                  found->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        found->received[message.chunk_index] = true;
        ++found->received_count;
    }
    found->expires = now + avatar_transfer_timeout_seconds;
    if (found->received_count != found->chunk_count)
        return;
    AvatarReceived done;
    done.gid = gid;
    done.hash = found->hash;
    done.bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(found->bytes));
    incoming_.erase(found);
    received_.push_back(std::move(done));
}

void AvatarSyncSession::receive(std::uint8_t sender_gid, std::span<const std::uint8_t> packet,
                                double now)
{
    const auto peer = peers_.find(sender_gid);
    std::uint8_t id{};
    AvatarSyncMessage message;
    // A custom packet from a peer without the marker is stale, spoofed or from
    // an incompatible revision. And nobody can have recognised this client
    // before its own marker went out, so nothing is answered before then.
    if (!advertised_ || peer == peers_.end() || !decode_avatar_sync_packet(packet, id, message) ||
        id != peer->second.message_id || message.protocol != avatar_protocol_version)
    {
        ++counters_.packets_ignored;
        return;
    }
    switch (static_cast<AvatarPacketKind>(message.kind))
    {
    case AvatarPacketKind::hello:
        handle_hello(sender_gid, now);
        break;
    case AvatarPacketKind::manifest:
        handle_manifest(sender_gid, message, now);
        break;
    case AvatarPacketKind::request:
        handle_request(sender_gid, message, now);
        break;
    case AvatarPacketKind::chunk:
        handle_chunk(sender_gid, message, now);
        break;
    default:
        ++counters_.packets_ignored;
        break;
    }
}

void AvatarSyncSession::update(double now)
{
    if (now >= next_cleanup_)
    {
        cleanup(now);
        next_cleanup_ = now + avatar_cleanup_interval_seconds;
    }
    if (!advertised_)
        return;
    for (auto &[gid, peer] : peers_)
    {
        if (peer.hello_pending && request_remote_)
        {
            AvatarSyncMessage hello;
            hello.kind = static_cast<std::uint8_t>(AvatarPacketKind::hello);
            queue(gid, hello);
            peer.hello_pending = false;
            peer.hello_sent = true;
            ++counters_.hellos_sent;
        }
        if (peer.offer_due && offer_local_ && local_bytes_ != nullptr &&
            now - peer.last_manifest_sent >= reoffer_interval_seconds)
            offer(gid, now);
        else if (peer.repeat_due)
            repeat_offer(gid, now);
        if (peer.deferred_manifest.has_value() &&
            now - peer.last_manifest_received >= avatar_manifest_interval_seconds)
        {
            const AvatarSyncMessage manifest = std::move(*peer.deferred_manifest);
            peer.deferred_manifest.reset();
            handle_manifest(gid, manifest, now);
        }
    }

    // The add-on serves up to four chunks per game update, so its rate follows
    // the frame rate. Here the same ceiling is paced by the clock instead: a
    // client drawing hundreds of frames a second must not push megabytes a
    // second through the host's relay.
    chunk_credit_ = last_update_ <= never / 2.0
                        ? 1.0
                        : std::min(static_cast<double>(avatar_chunks_per_update),
                                   chunk_credit_ + std::max(0.0, now - last_update_) *
                                                       avatar_chunks_per_second);
    last_update_ = now;
    // Round-robin, one chunk per transfer per turn, so a full lobby joining at
    // once does not leave the last player waiting through everyone else's.
    std::size_t sent{};
    while (sent < avatar_chunks_per_update && chunk_credit_ >= 1.0 && !outgoing_.empty())
    {
        Outgoing transfer = std::move(outgoing_.front());
        outgoing_.pop_front();
        if (!peers_.contains(transfer.gid) || transfer.next_chunk >= transfer.chunk_count)
            continue;
        chunk_credit_ -= 1.0;
        const std::size_t offset = static_cast<std::size_t>(transfer.next_chunk) * avatar_chunk_payload;
        const std::size_t length = std::min(avatar_chunk_payload, transfer.bytes->size() - offset);
        AvatarSyncMessage chunk;
        chunk.kind = static_cast<std::uint8_t>(AvatarPacketKind::chunk);
        chunk.transfer_id = transfer.transfer_id;
        chunk.total_length = static_cast<std::int32_t>(transfer.bytes->size());
        chunk.chunk_index = static_cast<std::uint16_t>(transfer.next_chunk);
        chunk.chunk_count = static_cast<std::uint16_t>(transfer.chunk_count);
        chunk.hash = transfer.hash;
        chunk.payload.assign(transfer.bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                             transfer.bytes->begin() + static_cast<std::ptrdiff_t>(offset + length));
        queue(transfer.gid, chunk);
        ++transfer.next_chunk;
        ++sent;
        ++counters_.chunks_sent;
        if (transfer.next_chunk < transfer.chunk_count)
            outgoing_.push_back(std::move(transfer));
    }
}

void AvatarSyncSession::cleanup(double now)
{
    std::erase_if(offers_, [&](const Offer &offer) {
        return offer.expires < now || !peers_.contains(offer.gid);
    });
    std::erase_if(incoming_, [&](const Incoming &transfer) {
        return transfer.expires < now || !peers_.contains(transfer.gid);
    });
}

void AvatarSyncSession::drop_incoming_for(std::uint8_t gid) noexcept
{
    std::erase_if(incoming_, [gid](const Incoming &transfer) { return transfer.gid == gid; });
}

std::vector<AvatarSyncSend> AvatarSyncSession::take_sends() noexcept
{
    return std::exchange(sends_, {});
}

std::vector<AvatarReceived> AvatarSyncSession::take_received() noexcept
{
    return std::exchange(received_, {});
}

void AvatarSyncSession::transfer_verified(std::uint8_t gid, const AvatarHash &hash,
                                          bool valid) noexcept
{
    const auto peer = peers_.find(gid);
    if (peer == peers_.end())
        return;
    if (valid)
    {
        peer->second.applied = hash;
        ++counters_.transfers_completed;
    }
}

bool AvatarSyncSession::peer_ready(std::uint8_t gid) const noexcept
{
    return peers_.contains(gid);
}

std::size_t AvatarSyncSession::incoming_transfers() const noexcept
{
    return incoming_.size();
}

std::size_t AvatarSyncSession::outgoing_transfers() const noexcept
{
    return outgoing_.size();
}

std::size_t AvatarSyncSession::pending_offers() const noexcept
{
    return offers_.size();
}

std::size_t AvatarSyncSession::buffered_bytes() const noexcept
{
    std::size_t total{};
    for (const Incoming &transfer : incoming_)
        total += transfer.bytes.size();
    return total;
}

} // namespace openminerz::net
