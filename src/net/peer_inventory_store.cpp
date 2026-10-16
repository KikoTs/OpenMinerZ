#include "openminerz/net/peer_inventory_store.hpp"
#include "openminerz/atomic_file.hpp"
#include "openminerz/net/steam_bridge_protocol.hpp"

#include <fstream>
#include <string>

namespace openminerz::net
{
namespace
{
constexpr std::size_t maximum_record_size = maximum_payload_size + 28U;
constexpr std::uint32_t magic = 0x49504D4FU; // OMPI

std::filesystem::path record_path(const std::filesystem::path &directory, ProtocolVersion protocol,
                                  PeerId peer)
{
    return directory /
           (std::to_string(static_cast<unsigned>(protocol)) + "-" + std::to_string(peer) + ".ompi");
}
} // namespace

bool write_peer_inventory(const std::filesystem::path &directory, ProtocolVersion protocol,
                          PeerId peer, const PlayerInventoryPayload &inventory)
{
    if (directory.empty() || peer == 0U)
        return false;
    PayloadError error{};
    const auto payload = encode_player_inventory_payload(inventory, protocol, error);
    if (!payload || payload->size() > maximum_record_size - 28U)
        return false;
    BoundedWriter writer{maximum_record_size};
    if (!writer.write_u32(magic) || !writer.write_u32(1U) ||
        !writer.write_u32(static_cast<std::uint32_t>(protocol)) || !writer.write_u64(peer) ||
        !writer.write_u32(static_cast<std::uint32_t>(payload->size())) ||
        !writer.write_bytes(*payload))
        return false;
    const auto checksum = steam_bridge_crc32(writer.bytes());
    if (!writer.write_u32(checksum))
        return false;
    return write_file_atomically(record_path(directory, protocol, peer), writer.bytes());
}

bool read_peer_inventory(const std::filesystem::path &directory, ProtocolVersion protocol,
                         PeerId peer, std::optional<PlayerInventoryPayload> &inventory)
{
    if (directory.empty() || peer == 0U)
        return false;
    const auto path = record_path(directory, protocol, peer);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error)
        return false;
    if (!exists)
    {
        inventory.reset();
        return true;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 28U || size > maximum_record_size)
        return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream file{path, std::ios::binary};
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        return false;
    BoundedReader reader{bytes};
    std::uint32_t header{}, version{}, stored_protocol{}, length{}, checksum{};
    std::uint64_t stored_peer{};
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(size) - 28U);
    if (!reader.read_u32(header) || header != magic || !reader.read_u32(version) || version != 1U ||
        !reader.read_u32(stored_protocol) ||
        stored_protocol != static_cast<std::uint32_t>(protocol) || !reader.read_u64(stored_peer) ||
        stored_peer != peer || !reader.read_u32(length) || length != size - 28U ||
        !reader.read_bytes(payload) || !reader.read_u32(checksum) || reader.remaining() != 0U ||
        checksum != steam_bridge_crc32(std::span{bytes}.first(bytes.size() - 4U)))
        return false;
    PlayerInventoryPayload decoded;
    PayloadError payload_error{};
    if (!decode_player_inventory_payload(payload, protocol, decoded, payload_error))
        return false;
    inventory = std::move(decoded);
    return true;
}
} // namespace openminerz::net
