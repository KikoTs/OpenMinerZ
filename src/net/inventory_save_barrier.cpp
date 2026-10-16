#include "openminerz/net/inventory_save_barrier.hpp"

#include <utility>

namespace openminerz::net
{
InventorySaveBarrier::InventorySaveBarrier(ProtocolVersion protocol, std::uint8_t player_gid,
                                           PlayerInventoryPayload inventory)
    : protocol_{protocol}, player_gid_{player_gid}, inventory_{std::move(inventory)}
{
}

std::optional<std::array<std::vector<std::uint8_t>, 2>> InventorySaveBarrier::packets() const
{
    PayloadError payload_error{};
    PacketError packet_error{};
    const auto store = encode_inventory_store_payload({inventory_, true}, protocol_, payload_error);
    const auto *store_type = message_by_name(protocol_, "InventoryStoreOnServerMessage");
    const auto *request_type = message_by_name(protocol_, "RequestInventoryMessage");
    if (!store || !store_type || !request_type || player_gid_ == 0U)
        return std::nullopt;
    const auto store_packet = encode_packet(protocol_, store_type->id, *store, packet_error);
    const auto request_packet = encode_packet(protocol_, request_type->id, {}, packet_error);
    if (!store_packet || !request_packet)
        return std::nullopt;
    return std::array{*store_packet, *request_packet};
}

bool InventorySaveBarrier::accepts(std::uint8_t sender_gid,
                                   std::span<const std::uint8_t> packet) const
{
    DecodedPacket decoded;
    PacketError packet_error{};
    InventoryRetrievePayload reply;
    PayloadError payload_error{};
    return sender_gid == 0U && decode_packet(protocol_, packet, decoded, packet_error) &&
           decoded.message && decoded.message->name == "InventoryRetrieveFromServerMessage" &&
           decode_inventory_retrieve_payload(decoded.payload, protocol_, reply, payload_error) &&
           reply.player_id == player_gid_ && !reply.default_inventory &&
           reply.inventory == inventory_;
}
} // namespace openminerz::net
