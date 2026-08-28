#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::net
{

enum class ProtocolVersion : std::uint8_t
{
    classic_v3 = 3,
    modern_v4 = 4
};

enum class MessageDeliveryPolicy : std::uint8_t
{
    unreliable,
    reliable_unordered,
    unreliable_sequenced,
    reliable_sequenced,
    reliable_ordered,
    chat,
    player_update_state,
    dragon_update_warmup
};

struct MessageTransportContract final
{
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::unreliable};
    bool echo{true};
};

struct MessageDescriptor final
{
    std::uint8_t id{};
    std::string_view full_name;
    std::string_view name;
    MessageTransportContract transport;
};

inline constexpr std::size_t minimum_packet_size = 2U;
inline constexpr std::size_t maximum_packet_size = 4096U;
inline constexpr std::size_t maximum_payload_size = maximum_packet_size - minimum_packet_size;

[[nodiscard]] std::span<const MessageDescriptor> message_manifest(ProtocolVersion version) noexcept;
[[nodiscard]] const MessageDescriptor *message_by_id(ProtocolVersion version,
                                                     std::uint8_t id) noexcept;
[[nodiscard]] const MessageDescriptor *message_by_name(ProtocolVersion version,
                                                       std::string_view name) noexcept;
// Messages in the original PlayerUpdate dispatch domain require the sender's
// Player object. Enemy, pickup, broadcast, and system messages do not.
[[nodiscard]] bool player_routed_message(std::string_view name) noexcept;

enum class PacketError : std::uint8_t
{
    none,
    too_small,
    too_large,
    unknown_message_id,
    checksum_mismatch,
    payload_too_large
};

struct DecodedPacket final
{
    const MessageDescriptor *message{};
    std::span<const std::uint8_t> payload;
};

[[nodiscard]] std::uint8_t xor8(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_packet(
    ProtocolVersion version, std::uint8_t message_id, std::span<const std::uint8_t> payload,
    PacketError &error);
[[nodiscard]] bool decode_packet(ProtocolVersion version, std::span<const std::uint8_t> packet,
                                 DecodedPacket &decoded, PacketError &error) noexcept;

// Bounds-aware primitives used by both stock payload codecs and the private
// OpenMinerZ extension channel. Integers are explicitly little-endian and
// strings use BinaryWriter's 7-bit byte-length prefix.
class BoundedReader final
{
  public:
    explicit BoundedReader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}
    {
    }

    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] std::size_t position() const noexcept
    {
        return offset_;
    }
    [[nodiscard]] bool exhausted() const noexcept
    {
        return offset_ == bytes_.size();
    }

    bool read_u8(std::uint8_t &value) noexcept;
    bool read_bool(bool &value) noexcept;
    bool read_i16(std::int16_t &value) noexcept;
    bool read_u16(std::uint16_t &value) noexcept;
    bool read_i32(std::int32_t &value) noexcept;
    bool read_u32(std::uint32_t &value) noexcept;
    bool read_i64(std::int64_t &value) noexcept;
    bool read_u64(std::uint64_t &value) noexcept;
    bool read_f32(float &value) noexcept;
    bool read_bytes(std::span<std::uint8_t> destination) noexcept;
    bool read_dotnet_string(std::string &value, std::size_t maximum_bytes);

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

class BoundedWriter final
{
  public:
    explicit BoundedWriter(std::size_t maximum_size = maximum_packet_size);

    [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept
    {
        return bytes_;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept
    {
        return std::move(bytes_);
    }

    bool write_u8(std::uint8_t value);
    bool write_bool(bool value);
    bool write_i16(std::int16_t value);
    bool write_u16(std::uint16_t value);
    bool write_i32(std::int32_t value);
    bool write_u32(std::uint32_t value);
    bool write_i64(std::int64_t value);
    bool write_u64(std::uint64_t value);
    bool write_f32(float value);
    bool write_bytes(std::span<const std::uint8_t> value);
    bool write_dotnet_string(std::string_view value, std::size_t maximum_bytes);

  private:
    [[nodiscard]] bool can_append(std::size_t count) const noexcept;

    std::size_t maximum_size_{};
    std::vector<std::uint8_t> bytes_;
};

} // namespace openminerz::net
