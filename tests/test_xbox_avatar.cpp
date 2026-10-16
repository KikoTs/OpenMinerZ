// Xbox avatars: the `.ocavatar` reader, the skin skeleton, and the add-on's
// network protocol - all without the game, the add-on, a GPU or a network.
//
//   openminerz_xbox_avatar_tests                       run everything
//   openminerz_xbox_avatar_tests --write-fixtures DIR  write OUR bytes for
//       tools/OriginalClientInterop/AvatarAddonProbe to feed the add-on
//   openminerz_xbox_avatar_tests --check-probe DIR     read what the add-on wrote
//       back and hold it against our readers
//   openminerz_xbox_avatar_tests --peer                this client's side of a
//       live exchange with the add-on's own code, over stdin and stdout

#include "xbox_avatar_fixture.hpp"

#include "openminerz/net/avatar_sync.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/xbox_avatar.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace
{
using namespace openminerz;
using namespace openminerz::net;

void check(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

std::vector<std::uint8_t> bytes_of(std::string_view hex)
{
    std::vector<std::uint8_t> result;
    const auto nibble = [](char digit) {
        return digit >= 'a' ? digit - 'a' + 10 : digit >= 'A' ? digit - 'A' + 10 : digit - '0';
    };
    for (std::size_t index = 0; index + 1U < hex.size(); index += 2U)
        result.push_back(static_cast<std::uint8_t>(nibble(hex[index]) * 16 + nibble(hex[index + 1U])));
    return result;
}

const xbox_avatar::ImageDecoder decoder = test::decode_stored_png;

// ------------------------------------------------------------------- digest

void digest()
{
    const std::string_view abc = "abc";
    check(xbox_avatar::digest_text(xbox_avatar::sha256(
              {reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size()})) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256(abc)");
    check(xbox_avatar::digest_text(xbox_avatar::sha256({})) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256(empty)");
    // Lengths around the padding boundary.
    const std::vector<std::uint8_t> block(119U, 0x61U);
    check(xbox_avatar::digest_text(xbox_avatar::sha256(std::span{block}.first(55U))) ==
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
          "SHA-256(55 a)");
    check(xbox_avatar::digest_text(xbox_avatar::sha256(std::span{block}.first(56U))) ==
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
          "SHA-256(56 a)");
    check(xbox_avatar::digest_text(xbox_avatar::sha256(block)) ==
              "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb",
          "SHA-256(119 a)");
}

// ----------------------------------------------------------- wire, pinned

void capability_marker()
{
    const std::vector<std::uint8_t> description(10U, 0U);
    // AvatarNetworkBridge.CapabilityMarker / MarkerPrefix, byte for byte.
    const auto form1 = append_avatar_capability_marker(description, std::nullopt);
    check(form1 == bytes_of("000000000000000000004f435841434150" "01"), "marker form 1 bytes");
    const auto form2 = append_avatar_capability_marker(description, std::uint8_t{55});
    check(form2 == bytes_of("000000000000000000004f435841434150" "0237"), "marker form 2 bytes");

    AvatarCapabilityMarker marker = read_avatar_capability_marker(form1);
    check(marker.present && !marker.message_id.has_value() && marker.description_length == 10U,
          "marker form 1 read");
    marker = read_avatar_capability_marker(form2);
    check(marker.present && marker.message_id == std::uint8_t{55} && marker.description_length == 10U,
          "marker form 2 read");
    // A bare marker (empty description) is still a marker.
    check(read_avatar_capability_marker(bytes_of("4f43584143415001")).present, "bare form 1");
    check(read_avatar_capability_marker(bytes_of("4f435841434150022a")).message_id == std::uint8_t{42},
          "bare form 2");
    // Not markers: a stock description, a wrong version, a truncated prefix,
    // a marker that is not at the end.
    for (const std::string_view not_marker :
         {"00000000000000000000", "4f43584143415003", "4f43584143415000", "43584143415001",
          "4f4358414341500100", "4f435841434150", ""})
        check(!read_avatar_capability_marker(bytes_of(not_marker)).present, "false marker accepted");
    // Form 2 wins when both could match, exactly as the add-on tests it first:
    // "...OCXACAP" 0x02 0x01 is form 2 with id 1.
    marker = read_avatar_capability_marker(bytes_of("4f4358414341500201"));
    check(marker.present && marker.message_id == std::uint8_t{1}, "form precedence");

    for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        const std::uint8_t natural = natural_avatar_message_id(protocol);
        check(natural == (protocol == ProtocolVersion::classic_v3 ? 53U : 55U),
              "the add-on's id on a bare client is one past the stock table");
        check(valid_avatar_message_id(protocol, natural) && !valid_avatar_message_id(protocol, 0U) &&
                  !valid_avatar_message_id(protocol, static_cast<std::uint8_t>(natural - 1U)),
              "stock ids are never avatar ids");
        const auto *exists = message_by_name(protocol, "PlayerExistsMessage");
        PayloadError payload_error{};
        PacketError packet_error{};
        const auto announce = [&](std::vector<std::uint8_t> decorated) {
            PlayerExistsPayload presence;
            presence.request_response = false;
            presence.avatar_description = std::move(decorated);
            presence.gamer_tag = "Avatar";
            presence.player_id = {1U, 2U, 3U};
            const auto payload = encode_player_exists_payload(presence, payload_error);
            check(payload.has_value(), "presence fixture");
            const auto packet = encode_packet(protocol, exists->id, *payload, packet_error);
            check(packet.has_value(), "presence frame");
            return read_avatar_capability_announcement(protocol, *packet);
        };
        check(!announce(description).present, "a stock presence announces nothing");
        auto announcement = announce(form1);
        check(announcement.present && !announcement.reported_id && announcement.message_id == natural,
              "form 1 means the natural id");
        announcement = announce(form2);
        check(announcement.present && announcement.reported_id && announcement.message_id == 55U,
              "form 2 carries the id");
        // An id inside the stock table would be read as that stock message.
        check(!announce(append_avatar_capability_marker(description, std::uint8_t{3})).present,
              "an id inside the stock table is refused");
    }
}

AvatarSyncMessage sample_message(AvatarPacketKind kind, std::size_t payload_bytes)
{
    AvatarSyncMessage message;
    message.kind = static_cast<std::uint8_t>(kind);
    message.transfer_id = 0x01020304U;
    message.total_length = 2'053'049;
    message.chunk_index = 0x0A0BU;
    message.chunk_count = 685U;
    for (std::size_t index = 0; index < message.hash.size(); ++index)
        message.hash[index] = static_cast<std::uint8_t>(0xA0U + index);
    message.payload.resize(payload_bytes);
    for (std::size_t index = 0; index < payload_bytes; ++index)
        message.payload[index] = static_cast<std::uint8_t>(index * 7U + 3U);
    return message;
}

void sync_codec()
{
    // ZZAvatarSyncMessage.SendData for a hello: the add-on sends protocol 1,
    // kind 1 and zeros.
    AvatarSyncMessage hello;
    const auto hello_payload = encode_avatar_sync_payload(hello);
    check(hello_payload.has_value() && hello_payload->size() == avatar_sync_header_bytes &&
              *hello_payload == bytes_of("0101" "00000000" "00000000" "0000" "0000"
                                         "0000000000000000000000000000000000000000000000000000000000000000"
                                         "0000"),
          "hello payload bytes");
    // id 53, payload, XOR-8 of everything before it: 53 ^ 1 ^ 1 = 53.
    const auto hello_packet = encode_avatar_sync_packet(53U, hello);
    check(avatar_sync_header_bytes == 48U && hello_packet.has_value() && hello_packet->size() == 50U &&
              hello_packet->front() == 53U &&
              hello_packet->back() == 53U,
          "hello packet frame");

    const AvatarSyncMessage chunk = sample_message(AvatarPacketKind::chunk, 3U);
    const auto chunk_payload = encode_avatar_sync_payload(chunk);
    check(chunk_payload.has_value() &&
              *chunk_payload == bytes_of("0104" "04030201" "b9531f00" "0b0a" "ad02"
                                         "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
                                         "0300" "030a11"),
          "chunk payload bytes");

    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2999}, std::size_t{3000}})
        for (const auto kind : {AvatarPacketKind::hello, AvatarPacketKind::manifest,
                                AvatarPacketKind::request, AvatarPacketKind::chunk})
        {
            const AvatarSyncMessage message = sample_message(kind, size);
            const auto packet = encode_avatar_sync_packet(55U, message);
            check(packet.has_value() && packet->size() == 50U + size, "packet size");
            std::uint8_t id{};
            AvatarSyncMessage decoded;
            check(decode_avatar_sync_packet(*packet, id, decoded) && id == 55U && decoded == message,
                  "packet round trip");
            check(is_avatar_sync_packet(*packet, 55U) && !is_avatar_sync_packet(*packet, 53U),
                  "packet id is part of its identity");
            // Corruption: every single-byte change is caught by the checksum
            // or the layout, a truncation or an extra byte by the length.
            for (const std::size_t at : {std::size_t{0}, std::size_t{1}, std::size_t{20}, packet->size() - 1U})
            {
                auto damaged = *packet;
                damaged[at] ^= 0x40U;
                check(!is_avatar_sync_packet(damaged, 55U), "corrupt packet accepted");
            }
            auto shorter = *packet;
            shorter.pop_back();
            check(!is_avatar_sync_packet(shorter, 55U), "truncated packet accepted");
            auto longer = *packet;
            longer.push_back(0U);
            check(!is_avatar_sync_packet(longer, 55U), "padded packet accepted");
        }
    check(!encode_avatar_sync_payload(sample_message(AvatarPacketKind::chunk, 3001U)).has_value(),
          "a payload over 3000 bytes is never written");
    // A length field over 3000 is refused even when the bytes are all there
    // ("Invalid avatar chunk length." in the add-on's reader).
    auto oversized = *encode_avatar_sync_payload(sample_message(AvatarPacketKind::chunk, 3000U));
    oversized.push_back(0U);
    oversized[46] = 0xB9U; // 3001 little-endian
    oversized[47] = 0x0BU;
    AvatarSyncMessage ignored;
    check(!decode_avatar_sync_payload(oversized, ignored), "over-long chunk accepted");
    check(avatar_chunk_count(0U) == 0U && avatar_chunk_count(1U) == 1U &&
              avatar_chunk_count(3000U) == 1U && avatar_chunk_count(3001U) == 2U &&
              avatar_chunk_count(avatar_maximum_asset_bytes) == 1399U,
          "chunk count");
}

// -------------------------------------------------------- sync state machine

struct Endpoint final
{
    std::uint8_t gid{};
    AvatarSyncSession session;
    std::map<std::uint8_t, std::shared_ptr<const std::vector<std::uint8_t>>> avatars;
    std::size_t rejected{};
};

struct Mesh final
{
    // Every endpoint speaks one id here; mixed ids are the session classes' job.
    std::uint8_t id{55U};
    double now{100.0};
    std::map<std::uint8_t, Endpoint> endpoints;
    std::size_t packets{};
    std::function<bool(std::uint8_t from, std::uint8_t to, std::vector<std::uint8_t> &packet)> filter;

    Endpoint &add(std::uint8_t gid, std::shared_ptr<const std::vector<std::uint8_t>> avatar)
    {
        Endpoint &endpoint = endpoints[gid];
        endpoint.gid = gid;
        endpoint.session.reset(ProtocolVersion::classic_v3);
        if (avatar != nullptr)
            endpoint.session.set_local_avatar(avatar, xbox_avatar::sha256(*avatar), now);
        endpoint.session.set_advertised(true);
        return endpoint;
    }
    void introduce(std::uint8_t first, std::uint8_t second)
    {
        endpoints.at(first).session.peer_capable(second, id, now);
        endpoints.at(second).session.peer_capable(first, id, now);
    }
    void step(double seconds = 1.0 / 60.0)
    {
        now += seconds;
        for (auto &[gid, endpoint] : endpoints)
        {
            endpoint.session.update(now);
            for (AvatarSyncSend &send : endpoint.session.take_sends())
            {
                ++packets;
                check(send.packet.front() == id, "a packet left under the wrong id");
                if (filter && !filter(gid, send.recipient_gid, send.packet))
                    continue;
                const auto target = endpoints.find(send.recipient_gid);
                if (target != endpoints.end())
                    target->second.session.receive(gid, send.packet, now);
            }
        }
        for (auto &[gid, endpoint] : endpoints)
            for (AvatarReceived &done : endpoint.session.take_received())
            {
                const bool valid = done.bytes != nullptr && xbox_avatar::sha256(*done.bytes) == done.hash;
                endpoint.session.transfer_verified(done.gid, done.hash, valid);
                if (valid)
                    endpoint.avatars[done.gid] = done.bytes;
                else
                    ++endpoint.rejected;
            }
    }
    void run(double seconds)
    {
        for (const double end = now + seconds; now < end;)
            step();
    }
};

std::shared_ptr<const std::vector<std::uint8_t>> blob(std::size_t size, std::uint8_t seed)
{
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
    std::uint32_t state = 0x9E3779B9U * (seed + 1U);
    for (std::uint8_t &byte : *bytes)
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::uint8_t>(state);
    }
    return bytes;
}

void sync_exchange()
{
    // Two players, each with an avatar of an awkward size, see each other.
    for (const std::size_t size : {std::size_t{1}, std::size_t{2999}, std::size_t{3000}, std::size_t{3001},
                                   std::size_t{250'000}})
    {
        Mesh mesh;
        const auto first = blob(size, 1U);
        const auto second = blob(size + 17U, 2U);
        mesh.add(1U, first);
        mesh.add(2U, second);
        mesh.introduce(1U, 2U);
        mesh.run(30.0);
        check(mesh.endpoints.at(1U).avatars.count(2U) == 1U && *mesh.endpoints.at(1U).avatars.at(2U) == *second,
              "the first player never received the second's avatar");
        check(mesh.endpoints.at(2U).avatars.count(1U) == 1U && *mesh.endpoints.at(2U).avatars.at(1U) == *first,
              "the second player never received the first's avatar");
        for (const auto &[gid, endpoint] : mesh.endpoints)
            check(endpoint.session.incoming_transfers() == 0U && endpoint.session.outgoing_transfers() == 0U &&
                      endpoint.session.buffered_bytes() == 0U && endpoint.rejected == 0U,
                  "a finished exchange left state behind");
    }

    // Nothing at all is produced for a peer that has not announced the add-on,
    // whatever arrives claiming to be from it.
    {
        Mesh mesh;
        mesh.add(1U, blob(5000U, 1U));
        mesh.add(2U, blob(5000U, 2U));
        // 2 knows 1 is capable; 1 has never heard a marker from 2.
        mesh.endpoints.at(2U).session.peer_capable(1U, mesh.id, mesh.now);
        std::size_t from_first{};
        mesh.filter = [&](std::uint8_t from, std::uint8_t, std::vector<std::uint8_t> &) {
            from_first += from == 1U ? 1U : 0U;
            return true;
        };
        mesh.run(20.0);
        check(from_first == 0U, "a packet was produced for a peer that never announced the add-on");
        check(mesh.endpoints.at(1U).avatars.empty() && mesh.endpoints.at(2U).avatars.empty(),
              "an avatar crossed without mutual capability");
        check(mesh.endpoints.at(1U).session.counters().packets_ignored > 0U,
              "packets from an unannounced peer must be ignored, and counted");
    }

    // Nothing is sent before the local marker has gone out.
    {
        Mesh mesh;
        mesh.add(1U, blob(100U, 1U)).session.set_advertised(false);
        mesh.add(2U, blob(100U, 2U));
        mesh.introduce(1U, 2U);
        std::size_t from_first{};
        mesh.filter = [&](std::uint8_t from, std::uint8_t, std::vector<std::uint8_t> &) {
            from_first += from == 1U ? 1U : 0U;
            return true;
        };
        mesh.run(3.0);
        check(from_first == 0U, "a packet left before the capability marker");
        mesh.endpoints.at(1U).session.set_advertised(true);
        mesh.run(20.0);
        check(mesh.endpoints.at(1U).avatars.count(2U) == 1U && mesh.endpoints.at(2U).avatars.count(1U) == 1U,
              "the exchange did not start once the marker was out");
    }

    // The add-on's ordering hazard: a manifest that arrives before the peer has
    // said hello is ignored by it, and its hello then finds an offer already
    // pending. The offer is repeated, so the exchange still completes.
    {
        Mesh mesh;
        const auto avatar = blob(9000U, 3U);
        mesh.add(1U, avatar);
        Endpoint &late = mesh.add(2U, nullptr);
        late.session.set_advertised(false); // has not greeted anybody yet
        mesh.introduce(1U, 2U);
        mesh.run(2.0); // 1 offers; 2 ignores the manifest (it has not asked)
        check(late.avatars.empty() && late.session.incoming_transfers() == 0U,
              "an unsolicited manifest was accepted");
        late.session.set_advertised(true); // now it greets
        mesh.run(20.0);
        check(late.avatars.count(1U) == 1U && *late.avatars.at(1U) == *avatar,
              "a hello that met a pending offer was left unanswered");
    }

    // Live update: a changed local avatar reaches a peer that already has the
    // old one, in place, and the old transfer state is gone.
    {
        Mesh mesh;
        mesh.add(1U, blob(40'000U, 4U));
        mesh.add(2U, nullptr);
        mesh.introduce(1U, 2U);
        mesh.run(20.0);
        check(mesh.endpoints.at(2U).avatars.count(1U) == 1U, "first avatar missing");
        const auto changed = blob(41'000U, 5U);
        mesh.endpoints.at(1U).session.set_local_avatar(changed, xbox_avatar::sha256(*changed), mesh.now);
        mesh.run(20.0);
        check(*mesh.endpoints.at(2U).avatars.at(1U) == *changed, "the changed avatar never arrived");
        // Changing it while a transfer of the previous one is in flight.
        const auto large = blob(600'000U, 6U);
        mesh.endpoints.at(1U).session.set_local_avatar(large, xbox_avatar::sha256(*large), mesh.now);
        mesh.run(0.8);
        check(mesh.endpoints.at(2U).session.incoming_transfers() == 1U &&
                  mesh.endpoints.at(1U).session.outgoing_transfers() == 1U,
              "the large avatar is not in flight");
        const auto final_avatar = blob(30'000U, 7U);
        mesh.endpoints.at(1U).session.set_local_avatar(final_avatar, xbox_avatar::sha256(*final_avatar), mesh.now);
        mesh.run(60.0);
        check(*mesh.endpoints.at(2U).avatars.at(1U) == *final_avatar,
              "an avatar changed mid-transfer did not converge on the newest");
        check(mesh.endpoints.at(2U).rejected == 0U, "a superseded transfer was mistaken for the new one");
    }

    // A late joiner gets everyone's, and everyone gets the late joiner's, with
    // transfers served round-robin.
    {
        Mesh mesh;
        for (std::uint8_t gid = 1U; gid <= 4U; ++gid)
            mesh.add(gid, blob(60'000U + gid, gid));
        for (std::uint8_t first = 1U; first <= 4U; ++first)
            for (std::uint8_t second = static_cast<std::uint8_t>(first + 1U); second <= 4U; ++second)
                mesh.introduce(first, second);
        mesh.run(40.0);
        mesh.add(9U, blob(70'000U, 9U));
        for (std::uint8_t gid = 1U; gid <= 4U; ++gid)
            mesh.introduce(gid, 9U);
        mesh.run(60.0);
        for (const auto &[gid, endpoint] : mesh.endpoints)
            check(endpoint.avatars.size() == mesh.endpoints.size() - 1U,
                  "a player is missing somebody's avatar after a late join");
    }

    // A player who leaves is forgotten at once, mid-transfer included, and the
    // gamer id can be reused by somebody who gets nothing they did not ask for.
    {
        Mesh mesh;
        mesh.add(1U, blob(900'000U, 1U));
        mesh.add(2U, blob(900'000U, 2U));
        mesh.introduce(1U, 2U);
        mesh.run(1.0);
        check(mesh.endpoints.at(1U).session.incoming_transfers() == 1U &&
                  mesh.endpoints.at(1U).session.buffered_bytes() == 900'000U,
              "transfer not in flight");
        mesh.endpoints.at(1U).session.peer_left(2U);
        mesh.endpoints.erase(2U);
        check(!mesh.endpoints.at(1U).session.peer_ready(2U) &&
                  mesh.endpoints.at(1U).session.incoming_transfers() == 0U &&
                  mesh.endpoints.at(1U).session.outgoing_transfers() == 0U &&
                  mesh.endpoints.at(1U).session.pending_offers() == 0U &&
                  mesh.endpoints.at(1U).session.buffered_bytes() == 0U,
              "a departed player's transfers were kept");
        mesh.endpoints.at(1U).session.update(mesh.now + 1.0);
        check(mesh.endpoints.at(1U).session.take_sends().empty(), "a packet was produced for a departed player");
    }
}

void sync_hostile()
{
    const std::uint8_t id = 55U;
    const auto packet = [&](AvatarSyncMessage message) { return *encode_avatar_sync_packet(id, message); };
    const auto session_with_peer = [&](AvatarSyncSession &session) {
        session.reset(ProtocolVersion::classic_v3);
        session.set_advertised(true);
        session.peer_capable(7U, id, 0.0);
        session.update(0.0); // says hello to 7
        static_cast<void>(session.take_sends());
    };
    AvatarSyncMessage manifest;
    manifest.kind = static_cast<std::uint8_t>(AvatarPacketKind::manifest);
    manifest.transfer_id = 5U;
    manifest.total_length = 9000;
    manifest.chunk_count = 3U;

    // Size and count lies.
    struct SizeLie final
    {
        std::int32_t length{};
        std::uint16_t count{};
    };
    for (const auto &[length, count] :
         {SizeLie{0, 0U}, SizeLie{-1, 1U},
          SizeLie{static_cast<std::int32_t>(avatar_maximum_asset_bytes) + 1, 1399U}, SizeLie{9000, 2U},
          SizeLie{9000, 4U}, SizeLie{std::numeric_limits<std::int32_t>::max(), 65535U}})
    {
        AvatarSyncSession session;
        session_with_peer(session);
        AvatarSyncMessage lie = manifest;
        lie.total_length = length;
        lie.chunk_count = count;
        session.receive(7U, packet(lie), 1.0);
        check(session.incoming_transfers() == 0U && session.buffered_bytes() == 0U &&
                  session.take_sends().empty(),
              "a manifest with an impossible size was accepted");
    }
    // The largest avatar allowed is accepted and costs exactly its size.
    {
        AvatarSyncSession session;
        session_with_peer(session);
        AvatarSyncMessage largest = manifest;
        largest.total_length = static_cast<std::int32_t>(avatar_maximum_asset_bytes);
        largest.chunk_count = 1399U;
        session.receive(7U, packet(largest), 1.0);
        check(session.incoming_transfers() == 1U && session.buffered_bytes() == avatar_maximum_asset_bytes,
              "the largest allowed avatar was refused");
        // A second manifest inside five seconds is not even looked at; after
        // five it replaces the first instead of adding to it.
        AvatarSyncMessage again = largest;
        again.transfer_id = 6U;
        session.receive(7U, packet(again), 3.0);
        check(session.incoming_transfers() == 1U, "manifest rate limit");
        session.receive(7U, packet(again), 7.0);
        check(session.incoming_transfers() == 1U && session.buffered_bytes() == avatar_maximum_asset_bytes,
              "a peer held two transfers at once");
    }
    // The newest manifest inside the window is not lost, though: it is answered
    // when the window closes. (The add-on drops it, so a second `/avatar
    // reload` within five seconds is never seen by another add-on client.)
    {
        AvatarSyncSession session;
        session_with_peer(session);
        session.receive(7U, packet(manifest), 1.0);
        check(session.take_sends().size() == 1U, "the first manifest is requested at once");
        AvatarSyncMessage second = manifest;
        second.transfer_id = 6U;
        second.hash[0] = 9U;
        AvatarSyncMessage third = second;
        third.transfer_id = 7U;
        third.hash[0] = 10U;
        session.receive(7U, packet(second), 2.0);
        session.receive(7U, packet(third), 3.0);
        session.update(5.9);
        check(session.take_sends().empty() && session.incoming_transfers() == 1U,
              "a manifest inside the window was answered early");
        session.update(6.1);
        const auto sends = session.take_sends();
        AvatarSyncMessage request;
        std::uint8_t request_id{};
        check(sends.size() == 1U && decode_avatar_sync_packet(sends[0].packet, request_id, request) &&
                  request.kind == static_cast<std::uint8_t>(AvatarPacketKind::request) &&
                  request.transfer_id == 7U && request.hash == third.hash && session.incoming_transfers() == 1U,
              "the newest deferred manifest was not answered when the window closed");
        session.update(20.0);
        check(session.take_sends().empty(), "a deferred manifest was answered twice");
    }
    // At most eight transfers in flight, whoever sends them: memory is bounded
    // at eight avatars.
    {
        AvatarSyncSession session;
        session.reset(ProtocolVersion::classic_v3);
        session.set_advertised(true);
        for (std::uint8_t gid = 1U; gid <= 20U; ++gid)
            session.peer_capable(gid, id, 0.0);
        session.update(0.0);
        static_cast<void>(session.take_sends());
        AvatarSyncMessage largest = manifest;
        largest.total_length = static_cast<std::int32_t>(avatar_maximum_asset_bytes);
        largest.chunk_count = 1399U;
        for (std::uint8_t gid = 1U; gid <= 20U; ++gid)
            session.receive(gid, packet(largest), 1.0);
        check(session.incoming_transfers() == avatar_maximum_incoming_transfers &&
                  session.buffered_bytes() == avatar_maximum_incoming_transfers * avatar_maximum_asset_bytes,
              "incoming transfers are not capped at eight");
        // They time out: 180 s for a first chunk that never comes.
        session.update(100.0);
        check(session.incoming_transfers() == avatar_maximum_incoming_transfers, "expired too early");
        session.update(190.0);
        check(session.incoming_transfers() == 0U && session.buffered_bytes() == 0U,
              "a stalled transfer was kept forever");
    }
    // A manifest from a peer that was never asked is ignored.
    {
        AvatarSyncSession session;
        session.reset(ProtocolVersion::classic_v3);
        session.set_policy(true, false, 0.0); // never asks
        session.set_advertised(true);
        session.peer_capable(7U, id, 0.0);
        session.update(0.0);
        check(session.take_sends().empty(), "a hello was sent although nothing is requested");
        session.receive(7U, packet(manifest), 1.0);
        check(session.incoming_transfers() == 0U && session.take_sends().empty(),
              "an unrequested manifest was accepted");
    }
    // Chunks: wrong transfer, wrong hash, wrong length, out of range, duplicate.
    {
        AvatarSyncSession session;
        session_with_peer(session);
        const auto data = blob(9000U, 1U);
        AvatarSyncMessage good = manifest;
        good.hash = xbox_avatar::sha256(*data);
        session.receive(7U, packet(good), 1.0);
        check(session.incoming_transfers() == 1U, "manifest refused");
        static_cast<void>(session.take_sends());
        const auto chunk_for = [&](std::uint16_t index) {
            AvatarSyncMessage chunk = good;
            chunk.kind = static_cast<std::uint8_t>(AvatarPacketKind::chunk);
            chunk.chunk_index = index;
            chunk.payload.assign(data->begin() + index * 3000, data->begin() + (index + 1) * 3000);
            return chunk;
        };
        AvatarSyncMessage wrong = chunk_for(0U);
        wrong.transfer_id = 99U;
        session.receive(7U, packet(wrong), 2.0);
        wrong = chunk_for(0U);
        wrong.hash[0] ^= 1U;
        session.receive(7U, packet(wrong), 2.0);
        wrong = chunk_for(0U);
        wrong.payload.pop_back();
        session.receive(7U, packet(wrong), 2.0);
        wrong = chunk_for(0U);
        wrong.chunk_index = 3U;
        session.receive(7U, packet(wrong), 2.0);
        wrong = chunk_for(0U);
        wrong.total_length = 9001;
        session.receive(7U, packet(wrong), 2.0);
        check(session.take_received().empty(), "a bad chunk completed a transfer");
        for (int repeat = 0; repeat < 3; ++repeat)
            session.receive(7U, packet(chunk_for(0U)), 2.0);
        session.receive(7U, packet(chunk_for(1U)), 2.0);
        check(session.take_received().empty(), "duplicates counted as progress");
        session.receive(7U, packet(chunk_for(2U)), 2.0);
        const auto done = session.take_received();
        check(done.size() == 1U && *done[0].bytes == *data && session.incoming_transfers() == 0U,
              "the transfer did not complete on its last chunk");
        // A lie about the content is caught by whoever verifies the hash.
        session.receive(7U, packet(chunk_for(2U)), 2.0);
        check(session.take_received().empty(), "a chunk after completion did something");
    }
    // Corrupt content: every chunk arrives, the hash does not match.
    {
        Mesh mesh;
        mesh.add(1U, blob(20'000U, 1U));
        mesh.add(2U, nullptr);
        mesh.introduce(1U, 2U);
        mesh.filter = [&](std::uint8_t, std::uint8_t, std::vector<std::uint8_t> &sent) {
            std::uint8_t packet_id{};
            AvatarSyncMessage message;
            if (decode_avatar_sync_packet(sent, packet_id, message) &&
                message.kind == static_cast<std::uint8_t>(AvatarPacketKind::chunk) && message.chunk_index == 3U)
            {
                message.payload[10] ^= 0x55U;
                sent = *encode_avatar_sync_packet(packet_id, message);
            }
            return true;
        };
        mesh.run(20.0);
        check(mesh.endpoints.at(2U).avatars.empty() && mesh.endpoints.at(2U).rejected == 1U,
              "a corrupted avatar was accepted");
    }
    // A flood of hellos produces one offer, not one per hello.
    {
        AvatarSyncSession session;
        session.reset(ProtocolVersion::classic_v3);
        const auto avatar = blob(50'000U, 1U);
        session.set_local_avatar(avatar, xbox_avatar::sha256(*avatar), 0.0);
        session.set_advertised(true);
        session.peer_capable(7U, id, 0.0);
        session.update(0.0);
        static_cast<void>(session.take_sends());
        AvatarSyncMessage hello;
        std::size_t manifests{};
        for (int repeat = 0; repeat < 10'000; ++repeat)
        {
            session.receive(7U, packet(hello), 0.5 + repeat * 0.0001);
            manifests += session.take_sends().size();
        }
        check(manifests <= 2U && session.pending_offers() == 1U && session.outgoing_transfers() == 0U,
              "hello flood amplified");
    }
    // Garbage under the right id is counted and dropped.
    {
        AvatarSyncSession session;
        session_with_peer(session);
        std::uint32_t state = 12345U;
        for (int round = 0; round < 20'000; ++round)
        {
            std::vector<std::uint8_t> noise(1U + round % 3100U);
            for (std::uint8_t &byte : noise)
            {
                state = state * 1664525U + 1013904223U;
                byte = static_cast<std::uint8_t>(state >> 24U);
            }
            noise[0] = id;
            session.receive(7U, noise, 1.0 + round);
        }
        check(session.buffered_bytes() <= avatar_maximum_asset_bytes, "noise allocated without bound");
    }
}

// ------------------------------------------------------------------ the file

void avatar_file()
{
    const test::SyntheticAvatar avatar;
    const std::vector<std::uint8_t> file = test::build_synthetic_avatar(avatar);
    std::string error;
    const auto skin = xbox_avatar::load(file, decoder, error);
    check(skin != nullptr, "the synthetic avatar did not load: " + error);
    check(skin->file_version == 4 && skin->source_batches == 8U, "header");
    // Head, face layer (frame 0 only), body palette overlay, outfit, shell.
    // Dropped: face frame 1 (not the neutral expression), the body base (the
    // opaque overlay hides it), the body decal (fully transparent).
    check(skin->layers.size() == 5U, "which layers are drawn: " + std::to_string(skin->layers.size()));
    check(skin->geometry.size() == 4U, "layers over one surface share its skinning");
    const auto layer_named = [&](std::string_view suffix) -> const xbox_avatar::Layer & {
        for (const xbox_avatar::Layer &layer : skin->layers)
            if (layer.name.ends_with(suffix))
                return layer;
        throw std::runtime_error{"missing layer " + std::string{suffix}};
    };
    const xbox_avatar::Layer &head = layer_named(":head:0");
    const xbox_avatar::Layer &face = layer_named(":face-layer-9-frame-0:0");
    const xbox_avatar::Layer &body = layer_named(":body:0:material-overlay-palette");
    const xbox_avatar::Layer &outfit = layer_named("00000218-0000-0001-0000-000000000000:model:0");
    const xbox_avatar::Layer &shell = layer_named("00000080-0000-0001-0000-000000000000:model:0");
    check(head.geometry == face.geometry && !head.blended && face.blended && face.clamp && !head.clamp,
          "a face layer is painted, clamped, over its head");
    check(head.texture < 0 && std::abs(head.color[0][0] - 199.0F / 255.0F) < 1.0e-6F, "untextured head colour");
    check(skin->geometry[head.geometry].head_only && !skin->geometry[body.geometry].head_only, "head-only surfaces");
    check(!body.blended && !outfit.blended, "a solid surface writes depth");
    check(std::abs(outfit.color[1][0] - 121.0F / 255.0F) < 1.0e-6F && outfit.color[1][3] == 1.0F,
          "vertex colour times diffuse");
    // The bare-hand shell wears the BODY's material and ignores its own masks.
    check(shell.texture == layer_named(":body:0:material-overlay-palette").texture ||
              shell.texture >= 0,
          "shell texture");
    check(shell.color[0][0] == 1.0F && shell.color[0][3] == 1.0F, "a bare-hand shell ignores its vertex masks");
    {
        // ...which is the BASE body's texture, the first non-overlay body layer.
        const xbox_avatar::Image &texture = skin->textures[static_cast<std::size_t>(shell.texture)];
        check(texture.width == 4 && texture.rgba[0] == 247U, "the shell wears the base body's texture");
    }
    // Handedness, once: the export's +Z is the rig's -Z; X and Y are untouched.
    const auto batches = test::synthetic_batches(avatar);
    const xbox_avatar::Geometry &head_geometry = skin->geometry[head.geometry];
    for (std::size_t index = 0; index < head_geometry.position.size(); ++index)
    {
        const auto &source = batches[0].vertices[index];
        check(head_geometry.position[index][0] == source.position[0] &&
                  head_geometry.position[index][1] == source.position[1] &&
                  head_geometry.position[index][2] == -source.position[2] &&
                  head_geometry.normal[index][2] == -source.normal[2],
              "positions and normals reflect in Z only");
    }
    for (std::size_t bone = 0; bone < skin->bones.size(); ++bone)
        check(skin->bones[bone].translation.x == avatar.offsets[bone][0] &&
                  skin->bones[bone].translation.y == avatar.offsets[bone][1] &&
                  skin->bones[bone].translation.z == -avatar.offsets[bone][2],
              "bone offsets reflect in Z only");
    check(skin->build == 1.0F, "a unit-build avatar");

    // Build: per-bone scale, accumulated down the hierarchy and bounded.
    {
        test::SyntheticAvatar tall;
        tall.scales[0] = {1.1F, 1.1F, 1.1F};
        tall.scales[19] = {0.9F, 0.9F, 0.9F};
        tall.scales[33] = {900.0F, 0.0001F, 1.0F}; // nonsense: keeps the stock size
        const auto scaled = xbox_avatar::load(test::build_synthetic_avatar(tall), decoder, error);
        check(scaled != nullptr, "scaled avatar: " + error);
        check(std::abs(scaled->bones[0].scale.x - 1.1F) < 1.0e-6F &&
                  std::abs(scaled->bones[19].cumulative_scale.y - 0.99F) < 1.0e-5F &&
                  std::abs(scaled->bones[36].cumulative_scale.x - 1.1F) < 1.0e-6F,
              "build accumulates from the root");
        check(scaled->bones[33].scale.x == 1.0F && scaled->bones[33].scale.y == 1.0F,
              "a collapsed or exploded bone keeps the stock size");
        check(std::abs(scaled->build - 1.1F) < 1.0e-6F, "build");
    }
    // Older file versions: 1 has no source pose, 2 no material header, and
    // anything before 4 stores V flipped (except face layers).
    for (const std::int32_t version : {1, 2, 3})
    {
        test::SyntheticAvatar older;
        older.version = version;
        const auto loaded = xbox_avatar::load(test::build_synthetic_avatar(older), decoder, error);
        check(loaded != nullptr, "version " + std::to_string(version) + ": " + error);
        check(std::abs(loaded->bones[5].translation.z + older.offsets[5][2]) < 1.0e-6F &&
                  std::abs(loaded->bones[5].translation.x - older.offsets[5][0]) < 1.0e-6F,
              "older skeleton");
        for (const xbox_avatar::Layer &layer : loaded->layers)
            if (layer.name.ends_with(":head:0"))
                check(layer.uv[0][1] == 1.0F - batches[0].vertices[0].uv[1], "V is corrected for old files");
            else if (layer.clamp)
                check(std::abs(layer.uv[0][1] - (batches[1].vertices[0].uv[1])) < 1.0e-6F,
                      "face layers were never flipped");
    }
}

void avatar_file_hostile()
{
    const test::SyntheticAvatar avatar;
    const std::vector<std::uint8_t> file = test::build_synthetic_avatar(avatar);
    std::string error;
    const auto rejects = [&](std::vector<std::uint8_t> bytes, std::string_view what,
                             const xbox_avatar::Limits &limits = {}) {
        check(xbox_avatar::load(bytes, decoder, error, limits) == nullptr,
              "accepted: " + std::string{what});
        check(!error.empty(), "a refusal must say why");
    };
    const auto sound = [](const xbox_avatar::Skin &skin) {
        for (const xbox_avatar::Layer &layer : skin.layers)
        {
            if (layer.geometry >= skin.geometry.size() ||
                (layer.texture >= 0 && static_cast<std::size_t>(layer.texture) >= skin.textures.size()))
                return false;
            const xbox_avatar::Geometry &geometry = skin.geometry[layer.geometry];
            if (layer.uv.size() != geometry.position.size() || layer.color.size() != geometry.position.size() ||
                layer.indices.size() % 3U != 0U)
                return false;
            for (const std::uint16_t index : layer.indices)
                if (index >= geometry.position.size())
                    return false;
            for (const auto &uv : layer.uv)
                if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
                    return false;
        }
        for (const xbox_avatar::Geometry &geometry : skin.geometry)
            for (std::size_t index = 0; index < geometry.position.size(); ++index)
            {
                float total{};
                for (std::size_t influence = 0; influence < 4U; ++influence)
                {
                    if (geometry.bones[index][influence] >= xbox_avatar::bone_count)
                        return false;
                    total += geometry.weights[index][influence];
                }
                if (std::abs(total - 1.0F) > 1.0e-4F || !std::isfinite(geometry.position[index][0]))
                    return false;
            }
        for (const xbox_avatar::Bone &bone : skin.bones)
            for (const float value : bone.inverse_bind)
                if (!std::isfinite(value))
                    return false;
        return true;
    };

    rejects({}, "an empty file");
    rejects({file.begin(), file.begin() + 3}, "three bytes");
    {
        auto bytes = file;
        bytes[0] ^= 1U;
        rejects(bytes, "a wrong signature");
        bytes = file;
        bytes[4] = 5U;
        rejects(bytes, "a future version");
        bytes[4] = 0U;
        rejects(bytes, "version zero");
        bytes = file;
        bytes[8] = 70U;
        rejects(bytes, "seventy bones");
        bytes = file;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(bytes.data() + 12U, &nan, 4U);
        rejects(bytes, "a NaN bind matrix");
        const float huge = 1.0e30F;
        std::memcpy(bytes.data() + 12U, &huge, 4U);
        rejects(bytes, "an enormous bind matrix");
    }
    // Every truncation: a file cut anywhere is refused, never read past.
    for (std::size_t length = 0; length < file.size(); length += 1U + length / 97U)
        check(xbox_avatar::load(std::span{file}.first(length), decoder, error) == nullptr,
              "a truncated file loaded at " + std::to_string(length));

    const auto with_batches = [&](auto &&edit) {
        auto batches = test::synthetic_batches(avatar);
        edit(batches);
        return test::write_synthetic_avatar(avatar, batches);
    };
    rejects(with_batches([](auto &batches) { batches[3].indices[5] = 60'000U; }), "an index past the vertices");
    rejects(with_batches([](auto &batches) { batches[3].indices.pop_back(); }), "a broken triangle list");
    rejects(with_batches([](auto &batches) { batches[3].indices.clear(); }), "a batch with no triangles");
    rejects(with_batches([](auto &batches) { batches[3].vertices.clear(); batches[3].indices.clear(); }),
            "a batch with no vertices");
    rejects(with_batches([](auto &batches) { batches[0].vertices[2].position[1] = std::numeric_limits<float>::infinity(); }),
            "an infinite vertex");
    rejects(with_batches([](auto &batches) { batches[0].vertices[2].position[0] = 9000.0F; }), "a vertex a mile away");
    rejects(with_batches([](auto &batches) { batches[3].png = {1U, 2U, 3U, 4U}; }), "a texture that is not a PNG");
    rejects(with_batches([](auto &batches) {
                // A real PNG header that claims 8192 x 8192: refused before any decode.
                batches[3].png = test::solid_png(1, 1, {0U, 0U, 0U, 255U});
                batches[3].png[18] = 0x20U;
                batches[3].png[22] = 0x20U;
            }),
            "an 8192-square texture");
    {
        // Textures within the per-image limit that together exceed the budget.
        xbox_avatar::Limits limits;
        limits.texture_pixels = 40U;
        rejects(file, "textures over the pixel budget", limits);
        limits = {};
        limits.drawn_triangles = 100U;
        rejects(file, "more triangles than may be drawn", limits);
        limits = {};
        limits.total_vertices = 100U;
        rejects(file, "more vertices than allowed", limits);
        limits = {};
        limits.file_bytes = file.size() - 1U;
        rejects(file, "a file over the size limit", limits);
        limits = {};
        limits.batches = 3U;
        rejects(file, "too many batches", limits);
    }
    {
        // Counts that promise more than the file holds must not allocate it.
        auto bytes = file;
        const std::int32_t many = 100'000'000;
        // The batch count sits right after the header and both pose tables.
        std::memcpy(bytes.data() + 12U + 71U * 64U * 2U, &many, 4U);
        rejects(bytes, "a hundred million batches");
    }
    // Tolerated, exactly as the add-on tolerates them: an image that does not
    // decode leaves that one surface untextured; an influence on a bone that
    // does not exist is skipped; an unweighted vertex is carried by the root.
    {
        const auto loaded = xbox_avatar::load(file, [](std::span<const std::uint8_t>, xbox_avatar::Image &) { return false; },
                                              error);
        check(loaded != nullptr && loaded->textures.empty() && sound(*loaded), "a failed image cost the whole avatar");
        const auto odd = xbox_avatar::load(with_batches([](auto &batches) {
                                               batches[0].vertices[0].bones = {200U, 19U, 0U, 0U};
                                               batches[0].vertices[0].weights = {100U, 100U, 0U, 0U};
                                               batches[0].vertices[1].weights = {0U, 0U, 0U, 0U};
                                               batches[0].vertices[2].uv = {std::numeric_limits<float>::quiet_NaN(), 1.0e20F};
                                           }),
                                           decoder, error);
        check(odd != nullptr && sound(*odd), "tolerated oddities: " + error);
        const xbox_avatar::Geometry &geometry = odd->geometry[odd->layers.front().geometry];
        check(geometry.weights[0][0] == 0.0F && geometry.weights[0][1] == 1.0F && geometry.bones[1][0] == 0U &&
                  geometry.weights[1][0] == 1.0F && !geometry.head_only,
              "weights were not repaired");
    }
    // Noise: thousands of damaged copies. Each either loads into something
    // sound or is refused; none may crash, hang or read out of bounds.
    std::uint32_t state = 0xC0FFEEU;
    const auto random = [&]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    std::size_t loaded{};
    for (int round = 0; round < 4000; ++round)
    {
        auto bytes = file;
        const int edits = 1 + static_cast<int>(random() % 6U);
        for (int edit = 0; edit < edits; ++edit)
        {
            const std::size_t at = random() % bytes.size();
            switch (random() % 4U)
            {
            case 0U:
                bytes[at] = static_cast<std::uint8_t>(random());
                break;
            case 1U:
                bytes[at] ^= static_cast<std::uint8_t>(1U << (random() % 8U));
                break;
            case 2U:
                if (at + 4U <= bytes.size())
                {
                    const std::uint32_t value = random() % 3U == 0U ? 0x7FFFFFFFU : random();
                    std::memcpy(bytes.data() + at, &value, 4U);
                }
                break;
            default:
                bytes.resize(at);
                break;
            }
            if (bytes.empty())
                break;
        }
        const auto skin = xbox_avatar::load(bytes, decoder, error);
        if (skin != nullptr)
        {
            ++loaded;
            check(sound(*skin), "a damaged file loaded into something unsound");
        }
    }
    check(loaded > 100U && loaded < 3900U, "the noise test must exercise both outcomes: " + std::to_string(loaded));
}

// ------------------------------------------------------------- the skeleton

rig::Matrix translation(float x, float y, float z)
{
    rig::Matrix matrix = rig::identity();
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    return matrix;
}

rig::Quat axis_angle(float x, float y, float z, float degrees)
{
    const float length = std::sqrt(x * x + y * y + z * z);
    const float half = degrees * std::numbers::pi_v<float> / 360.0F;
    const float sine = std::sin(half) / length;
    return {x * sine, y * sine, z * sine, std::cos(half)};
}

// A rig built by hand on the synthetic avatar's skeleton: the stock skeletons
// have the avatar's joints, so a unit-build avatar must land exactly on them.
struct HandRig final
{
    rig::RetailRig rig;
    explicit HandRig(const test::SyntheticAvatar &avatar, bool animated)
    {
        // Avatar.OnUpdate's constant: a half turn about Y, lifted to the hips.
        rig.root_adjust = rig::identity();
        rig.root_adjust[0] = -1.0F;
        rig.root_adjust[10] = -1.0F;
        rig.root_adjust[12] = avatar.offsets[0][0];
        rig.root_adjust[13] = avatar.offsets[0][1];
        rig.root_adjust[14] = -avatar.offsets[0][2];
        rig.eye_to_head = rig::identity();
        rig.body.resize(71U);
        rig::Clip clip;
        clip.asset = "test";
        clip.duration_ticks = rig::ticks_per_second;
        for (std::size_t bone = 0; bone < 71U; ++bone)
        {
            const std::array<float, 3> offset{avatar.offsets[bone][0], avatar.offsets[bone][1], -avatar.offsets[bone][2]};
            rig::AttachmentBone &attachment = rig.attachment[bone];
            attachment.name = "bone" + std::to_string(bone);
            attachment.native_name = attachment.name;
            attachment.parent = test::avatar_parents[bone];
            attachment.bind = bone == 0U ? rig.root_adjust : translation(offset[0], offset[1], offset[2]);
            rig::BodyBone &body = rig.body[bone];
            body.name = attachment.name;
            body.parent = attachment.parent;
            body.source = static_cast<int>(bone);
            body.bind_local = attachment.bind;
            body.inverse_bind = rig::identity();
            rig.body_order.push_back(static_cast<int>(bone));
            rig::BoneTransform key;
            if (animated)
            {
                key.rotation = axis_angle(1.0F + static_cast<float>(bone % 3U), static_cast<float>(bone % 5U) - 2.0F,
                                          0.5F, 7.0F * static_cast<float>(bone % 11U) - 30.0F);
                if (bone == 0U)
                    key.translation = {0.05F, -0.2F, 0.1F}; // root motion, as in Die and Run
            }
            clip.positions[bone] = {key.translation};
            clip.rotations[bone] = {key.rotation};
            clip.scales[bone] = {key.scale};
        }
        rig.clips.push_back(std::move(clip));
        rig::Registration registration;
        registration.name = "Test";
        registration.clip = 0;
        registration.influence.fill(true);
        rig.registrations.push_back(std::move(registration));
    }
};

float distance(const rig::Matrix &first, const rig::Matrix &second)
{
    float worst{};
    for (std::size_t index = 0; index < first.size(); ++index)
        worst = std::max(worst, std::abs(first[index] - second[index]));
    return worst;
}

void skeleton()
{
    std::string error;
    for (const bool animated : {false, true})
    {
        const test::SyntheticAvatar avatar;
        const HandRig hand{avatar, animated};
        rig::CharacterPose character{hand.rig};
        check(character.animations.play("Test", 0, 0.0) != nullptr, "test clip");
        character.update(0);
        const auto skin = xbox_avatar::load(test::build_synthetic_avatar(avatar), decoder, error);
        check(skin != nullptr, error);

        // A unit-build avatar stands exactly on the attachment skeleton - the
        // third skeleton under the engine's one rule - in every pose, root
        // motion and root rotation included.
        xbox_avatar::Pose own;
        xbox_avatar::pose(*skin, character, xbox_avatar::Placement::own_proportions, own);
        for (std::size_t bone = 0; bone < own.world.size(); ++bone)
            check(distance(own.world[bone], character.attachment()[bone]) < 2.0e-5F,
                  "own-proportions bone " + std::to_string(bone) + " is off the attachment skeleton");
        // ...and on the body's joints when placed there.
        xbox_avatar::Pose stock;
        xbox_avatar::pose(*skin, character, xbox_avatar::Placement::stock_joints, stock);
        for (std::size_t bone = 0; bone < stock.world.size(); ++bone)
            check(distance(stock.world[bone], character.body_world()[bone]) < 2.0e-5F,
                  "stock-joints bone " + std::to_string(bone) + " is off the body skeleton");
        // The held item hangs from PropRight either way.
        check(distance(xbox_avatar::held_item_parent(character, own, xbox_avatar::Placement::own_proportions),
                       character.right_hand()) < 2.0e-5F &&
                  distance(xbox_avatar::held_item_parent(character, stock, xbox_avatar::Placement::stock_joints),
                           character.right_hand()) == 0.0F,
              "held-item parent");

        // Skinning: a vertex rigidly bound to a bone goes where that bone's
        // attachment transform takes it from the bone's own frame.
        const auto batches = test::synthetic_batches(avatar);
        for (const xbox_avatar::Layer &layer : skin->layers)
        {
            if (!layer.name.ends_with(":head:0"))
                continue;
            xbox_avatar::SkinnedGeometry skinned;
            xbox_avatar::skin_geometry(skin->geometry[layer.geometry], own.palette, skinned);
            std::array<float, 3> head{};
            for (int bone = 19; bone >= 0; bone = test::avatar_parents[static_cast<std::size_t>(bone)])
                for (std::size_t axis = 0; axis < 3U; ++axis)
                    head[axis] += avatar.offsets[static_cast<std::size_t>(bone)][axis] * (axis == 2U ? -1.0F : 1.0F);
            for (std::size_t index = 0; index < skinned.position.size(); ++index)
            {
                const auto &source = batches[0].vertices[index].position;
                const std::array<float, 3> local{source[0] - head[0], source[1] - head[1], -source[2] - head[2]};
                const auto expected = rig::transform_point(local.data(), character.attachment()[19]);
                for (std::size_t axis = 0; axis < 3U; ++axis)
                    check(std::abs(skinned.position[index][axis] - expected[axis]) < 2.0e-5F, "skinned head vertex");
            }
        }
        // First person: the head collapses with the animated head scale.
        character.hide_head = true;
        character.update(0);
        xbox_avatar::pose(*skin, character, xbox_avatar::Placement::stock_joints, stock);
        const float head_size = std::abs(stock.world[19][0]) + std::abs(stock.world[19][1]) + std::abs(stock.world[19][2]);
        check(head_size < 0.01F, "the hidden head did not collapse");
    }

    // A taller build. Own proportions: every joint further from the hips by the
    // build, the hips at the avatar's own height, root motion grown with it.
    // Stock joints: every joint exactly where the stock body has it, the build
    // applied about it.
    {
        test::SyntheticAvatar avatar;
        const HandRig hand{avatar, true};
        rig::CharacterPose character{hand.rig};
        check(character.animations.play("Test", 0, 0.0) != nullptr, "test clip");
        character.update(0);
        test::SyntheticAvatar tall = avatar;
        tall.scales[0] = {1.25F, 1.25F, 1.25F};
        tall.offsets[0][1] = avatar.offsets[0][1] * 1.25F; // the export lifts the hips with the build
        const auto skin = xbox_avatar::load(test::build_synthetic_avatar(tall), decoder, error);
        check(skin != nullptr, error);
        xbox_avatar::Pose own;
        xbox_avatar::pose(*skin, character, xbox_avatar::Placement::own_proportions, own);
        const rig::Matrix &stock_root = character.attachment()[0];
        // The animated root moved the stock hips by (0.05,-0.2,0.1) through the
        // half turn; the tall avatar's move 1.25 times as far from 1.25 times
        // the height.
        check(std::abs(own.world[0][13] - (avatar.offsets[0][1] * 1.25F - 0.2F * 1.25F)) < 2.0e-5F, "tall hips");
        for (std::size_t bone = 1; bone < own.world.size(); ++bone)
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                const float stock_reach = character.attachment()[bone][12U + axis] - stock_root[12U + axis];
                const float tall_reach = own.world[bone][12U + axis] - own.world[0][12U + axis];
                check(std::abs(tall_reach - stock_reach * 1.25F) < 5.0e-5F,
                      "a tall avatar's joints are not 1.25 times as far out");
            }
        xbox_avatar::Pose stock;
        xbox_avatar::pose(*skin, character, xbox_avatar::Placement::stock_joints, stock);
        for (std::size_t bone = 0; bone < stock.world.size(); ++bone)
        {
            const bool hand_bone = bone >= 37U; // everything below a wrist
            if (hand_bone)
                continue;
            for (std::size_t axis = 0; axis < 3U; ++axis)
                check(std::abs(stock.world[bone][12U + axis] - character.body_world()[bone][12U + axis]) < 2.0e-5F,
                      "a stock-joints bone left its stock joint");
            check(std::abs(stock.world[bone][0] - character.body_world()[bone][0] * 1.25F) < 2.0e-5F,
                  "the build was not applied about the stock joint");
        }
        // The hand stays whole: each finger joint sits at the avatar's own
        // offset, grown by the build, from the STOCK wrist.
        for (const std::size_t finger : {std::size_t{44}, std::size_t{50}})
        {
            const std::array<float, 3> offset{tall.offsets[finger][0] * 1.25F, tall.offsets[finger][1] * 1.25F,
                                              -tall.offsets[finger][2] * 1.25F};
            const auto expected = rig::transform_point(offset.data(), character.body_world()[36]);
            for (std::size_t axis = 0; axis < 3U; ++axis)
                check(std::abs(stock.world[finger][12U + axis] - expected[axis]) < 2.0e-5F,
                      "a finger is not chained from the stock wrist");
        }
        check(distance(xbox_avatar::held_item_parent(character, stock, xbox_avatar::Placement::stock_joints),
                       character.right_hand()) == 0.0F,
              "first person must keep the stock held-item parent");
        check(distance(xbox_avatar::held_item_parent(character, own, xbox_avatar::Placement::own_proportions),
                       own.world[rig::bone_prop_right]) == 0.0F,
              "seen from outside, the item hangs from the avatar's own PropRight");
    }
}

// ------------------------------------------------- fixtures for the add-on

void write_file(const std::filesystem::path &path, std::span<const std::uint8_t> bytes)
{
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "fixture export failed: " + path.string());
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    check(static_cast<bool>(input), "missing probe output: " + path.string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

constexpr std::array<std::size_t, 5> fixture_payload_sizes{0U, 1U, 1500U, 2999U, 3000U};

void write_fixtures(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    const std::vector<std::uint8_t> description(10U, 0U);
    write_file(directory / "description.bin", description);
    write_file(directory / "marker.form1.bin", append_avatar_capability_marker(description, std::nullopt));
    for (const std::uint8_t id : {std::uint8_t{53}, std::uint8_t{55}, std::uint8_t{57}})
        write_file(directory / ("marker.form2." + std::to_string(id) + ".bin"),
                   append_avatar_capability_marker(description, id));
    // The duplicate presence a stock client must read as an ordinary one.
    PlayerExistsPayload presence;
    presence.avatar_description = append_avatar_capability_marker(description, std::nullopt);
    presence.gamer_tag = "OpenMinerZ";
    presence.player_id.assign(16U, 7U);
    PayloadError payload_error{};
    const auto exists = encode_player_exists_payload(presence, payload_error);
    check(exists.has_value(), "presence fixture");
    write_file(directory / "PlayerExistsMessage.marker.bin", *exists);
    for (const auto kind : {AvatarPacketKind::hello, AvatarPacketKind::manifest, AvatarPacketKind::request,
                            AvatarPacketKind::chunk})
        for (const std::size_t size : fixture_payload_sizes)
        {
            const AvatarSyncMessage message = sample_message(kind, size);
            const std::string stem = "sync." + std::to_string(static_cast<int>(kind)) + "." + std::to_string(size);
            write_file(directory / (stem + ".payload.bin"), *encode_avatar_sync_payload(message));
        }
    write_file(directory / "synthetic.ocavatar", test::build_synthetic_avatar());
    // What the two sides trade in the live exchange: three avatars of very
    // different sizes (the outfit's texture is what grows; a stored PNG is as
    // large as its pixels), and bytes of every length around one chunk.
    int texture = 4;
    for (const std::string_view name : {"avatar.small", "avatar.medium", "avatar.large"})
    {
        test::SyntheticAvatar avatar;
        avatar.cloth = {static_cast<std::uint8_t>(texture), 90U, 160U, 255U};
        std::vector<test::SyntheticBatch> batches = test::synthetic_batches(avatar);
        for (test::SyntheticBatch &batch : batches)
            if (batch.category == 0x00000218U)
                batch.png = test::solid_png(texture, texture, avatar.cloth);
        const std::vector<std::uint8_t> file = test::write_synthetic_avatar(avatar, batches);
        std::string error;
        check(xbox_avatar::load(file, decoder, error) != nullptr, "exchange fixture: " + error);
        write_file(directory / (std::string{name} + ".ocavatar"), file);
        texture *= 12;
    }
    for (const std::size_t size : {std::size_t{1}, std::size_t{2999}, std::size_t{3000}, std::size_t{3001}})
        write_file(directory / ("blob." + std::to_string(size) + ".bin"),
                   *blob(size, static_cast<std::uint8_t>(size)));
}

// What the probe wrote with the add-on's own code: its marker bytes, its
// ZZAvatarSyncMessage.SendData output, and complete frames from the engine's
// Message writer under the id the add-on registered.
void check_probe(const std::filesystem::path &directory)
{
    const std::vector<std::uint8_t> description(10U, 0U);
    const auto registered = read_file(directory / "addon.message-id.bin");
    check(registered.size() == 1U, "the add-on's message id");
    const std::uint8_t id = registered[0];
    // Its marker, appended by AvatarNetworkBridge.AppendCapabilityMarker.
    const auto marker = read_file(directory / "addon.marker.bin");
    const AvatarCapabilityMarker read = read_avatar_capability_marker(marker);
    check(read.present && read.description_length == description.size() && read.message_id == id,
          "the add-on's marker was not read as form 2 with its id");
    check(marker == append_avatar_capability_marker(description, id), "our form-2 writer differs from the add-on's");
    std::size_t frames{};
    for (const auto kind : {AvatarPacketKind::hello, AvatarPacketKind::manifest, AvatarPacketKind::request,
                            AvatarPacketKind::chunk})
        for (const std::size_t size : fixture_payload_sizes)
        {
            const AvatarSyncMessage message = sample_message(kind, size);
            const std::string stem = "sync." + std::to_string(static_cast<int>(kind)) + "." + std::to_string(size);
            // Its bytes into our reader.
            const auto theirs = read_file(directory / ("addon." + stem + ".payload.bin"));
            AvatarSyncMessage decoded;
            check(decode_avatar_sync_payload(theirs, decoded) && decoded == message,
                  "the add-on's " + stem + " payload did not decode to what it was given");
            check(theirs == *encode_avatar_sync_payload(message), "payload bytes differ: " + stem);
            const auto frame = read_file(directory / ("addon." + stem + ".frame.bin"));
            std::uint8_t frame_id{};
            check(decode_avatar_sync_packet(frame, frame_id, decoded) && frame_id == id && decoded == message,
                  "the engine's frame for " + stem + " did not decode");
            check(frame == *encode_avatar_sync_packet(id, message), "frame bytes differ: " + stem);
            ++frames;
        }
    std::cout << "PASS: the add-on's marker and " << frames << " payloads and frames (message id "
              << static_cast<int>(id) << ") read by the native codec, byte-identical to its own\n";
}

// ------------------------------------------------ a live peer for the add-on
//
// `--peer`: this client's side of one session, driven line by line over stdin
// and stdout by AvatarAddonProbe, which hosts the add-on's own
// AvatarNetworkBridge inside the original engine. Every byte that crosses is an
// application packet exactly as each side's production code made it; nothing
// is translated in between.
//
//   protocol 3|4          a new session of that generation
//   local <file>|none     this client's avatar (re-issue it to change it live)
//   policy <offer> <ask>  the two switches of the Add-Ons screen
//   packet <gid> <hex>    an application packet from that gamer
//   tick <seconds>        run that long at sixty updates a second
//   advertise             announce again (a newcomer's presence arrived)
//   left <gid>            that gamer left
//   quit
// and answers with zero or more of
//   send <gid|*> <hex>    a packet to that gamer, or a broadcast
//   capable <gid> <id> <reported>
//   accepted | refused <reason>
//   avatar <gid> <sha256> <bytes> <hash ok> <loads>
// followed by `ok`.

std::string hex_of(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const std::uint8_t byte : bytes)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

int peer()
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    AvatarSyncSession session;
    // The gate, held the way LegacySteamHostSession and LegacySteamClientSession
    // hold it: only an announcement opens it, for that gamer, under that id.
    AvatarCapabilityRegistry registry;
    const std::vector<std::uint8_t> player_id(16U, 7U);
    double now{};
    bool advertised{};
    session.reset(protocol);
    const auto advertise = [&] {
        const auto presence = make_avatar_capability_presence(protocol, "OpenMinerZ", player_id);
        check(presence.has_value(), "presence");
        std::cout << "send * " << hex_of(*presence) << '\n';
        advertised = true;
        session.set_advertised(true);
    };
    const auto flush = [&] {
        for (const AvatarSyncSend &send : session.take_sends())
        {
            // Nothing may leave for a gamer that never announced, or under an
            // id that gamer does not read: a stock client throws on it.
            check(registry.capable(send.recipient_gid) &&
                      registry.message_id(send.recipient_gid) == send.packet.front() &&
                      is_avatar_sync_packet(send.packet, send.packet.front()),
                  "THE GATE: a packet was produced for a gamer that cannot read it");
            std::cout << "send " << static_cast<int>(send.recipient_gid) << ' ' << hex_of(send.packet) << '\n';
        }
        for (const AvatarReceived &done : session.take_received())
        {
            const bool valid = done.bytes != nullptr && xbox_avatar::sha256(*done.bytes) == done.hash;
            session.transfer_verified(done.gid, done.hash, valid);
            std::string error;
            const bool loads = valid && xbox_avatar::load(*done.bytes, decoder, error) != nullptr;
            std::cout << "avatar " << static_cast<int>(done.gid) << ' ' << xbox_avatar::digest_text(done.hash)
                      << ' ' << (done.bytes != nullptr ? done.bytes->size() : 0U) << ' ' << valid << ' ' << loads
                      << '\n';
        }
    };
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // A .NET parent's redirected input begins with a byte-order mark when
        // its console code page is UTF-8.
        if (line.starts_with("\xEF\xBB\xBF"))
            line.erase(0U, 3U);
        std::istringstream words{line};
        std::string command;
        words >> command;
        if (command == "quit")
            break;
        if (command == "protocol")
        {
            int version{};
            words >> version;
            protocol = version == 4 ? ProtocolVersion::modern_v4 : ProtocolVersion::classic_v3;
            session.reset(protocol);
            registry.clear();
            advertised = false;
            now = 0.0;
        }
        else if (command == "local")
        {
            std::string file;
            std::getline(words >> std::ws, file);
            if (file == "none")
                session.set_local_avatar(nullptr, {}, now);
            else
            {
                auto bytes = std::make_shared<const std::vector<std::uint8_t>>(read_file(file));
                session.set_local_avatar(bytes, xbox_avatar::sha256(*bytes), now);
            }
        }
        else if (command == "policy")
        {
            int offer{};
            int ask{};
            words >> offer >> ask;
            session.set_policy(offer != 0, ask != 0, now);
        }
        else if (command == "packet")
        {
            int gid{};
            std::string hex;
            words >> gid >> hex;
            const std::vector<std::uint8_t> packet = bytes_of(hex);
            const auto sender = static_cast<std::uint8_t>(gid);
            const AvatarCapabilityAnnouncement announcement = read_avatar_capability_announcement(protocol, packet);
            DecodedPacket stock;
            PacketError packet_error{};
            if (announcement.present)
            {
                registry.announce(sender, announcement.message_id);
                session.peer_capable(sender, announcement.message_id, now);
                std::cout << "capable " << gid << ' ' << static_cast<int>(announcement.message_id) << ' '
                          << announcement.reported_id << '\n';
            }
            else if (decode_packet(protocol, packet, stock, packet_error))
                std::cout << "refused stock-message\n";
            else if (!registry.capable(sender))
                std::cout << "refused unannounced\n";
            else if (!is_avatar_sync_packet(packet, *registry.message_id(sender)))
                std::cout << "refused not-an-avatar-message\n";
            else
            {
                session.receive(sender, packet, now);
                std::cout << "accepted\n";
            }
        }
        else if (command == "tick")
        {
            double seconds{};
            words >> seconds;
            if (!advertised)
                advertise();
            for (const double end = now + seconds; now < end;)
            {
                now += 1.0 / 60.0;
                session.update(now);
                flush();
            }
        }
        else if (command == "advertise")
            advertise();
        else if (command == "left")
        {
            int gid{};
            words >> gid;
            registry.forget(static_cast<std::uint8_t>(gid));
            session.peer_left(static_cast<std::uint8_t>(gid));
        }
        else
        {
            std::cout << "error unknown command " << hex_of({reinterpret_cast<const std::uint8_t *>(line.data()),
                                                             std::min<std::size_t>(line.size(), 24U)})
                      << '\n'
                      << std::flush;
            return 2;
        }
        flush();
        std::cout << "ok" << std::endl;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view{argv[1]} == "--write-fixtures")
        {
            write_fixtures(argv[2]);
            return 0;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--check-probe")
        {
            check_probe(argv[2]);
            return 0;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--peer")
            return peer();
        digest();
        capability_marker();
        sync_codec();
        sync_exchange();
        sync_hostile();
        avatar_file();
        avatar_file_hostile();
        skeleton();
        std::cout << "PASS: Xbox avatar file, skeleton and add-on protocol\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
