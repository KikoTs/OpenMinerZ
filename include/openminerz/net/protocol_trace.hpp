#pragma once

#include "openminerz/net/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace openminerz::net
{

enum class ProtocolTraceRole : std::uint8_t
{
    host,
    client
};

enum class ProtocolTraceDirection : std::uint8_t
{
    inbound,
    outbound,
    internal
};

enum class ProtocolTraceResult : std::uint8_t
{
    accepted,
    rejected,
    failure
};

// A certification record intentionally contains no Steam/lobby IDs, gamer
// tags, passwords, endpoints, or packet bytes. The optional reason is emitted
// only as a one-way fingerprint so failures can be correlated without leaking
// untrusted text.
struct ProtocolTraceRecord final
{
    ProtocolTraceRole role{ProtocolTraceRole::client};
    ProtocolTraceDirection direction{ProtocolTraceDirection::internal};
    ProtocolTraceResult result{ProtocolTraceResult::accepted};
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    std::string event;
    std::uint8_t sender_gid{};
    std::optional<std::uint8_t> recipient_gid;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
    std::uint8_t channel{};
    std::size_t packet_bytes{};
    std::string reason;
};

[[nodiscard]] std::string encode_protocol_trace_record_json(
    std::uint64_t sequence, std::int64_t unix_milliseconds, std::uint64_t elapsed_microseconds,
    const ProtocolTraceRecord &record);

class ProtocolTraceWriter final
{
  public:
    ProtocolTraceWriter() = default;
    ~ProtocolTraceWriter() = default;

    ProtocolTraceWriter(const ProtocolTraceWriter &) = delete;
    ProtocolTraceWriter &operator=(const ProtocolTraceWriter &) = delete;
    ProtocolTraceWriter(ProtocolTraceWriter &&) noexcept = default;
    ProtocolTraceWriter &operator=(ProtocolTraceWriter &&) noexcept = default;

    [[nodiscard]] bool open(const std::filesystem::path &path, ProtocolTraceRole role,
                            ProtocolVersion protocol, std::string &error);
    void set_protocol(ProtocolVersion protocol) noexcept;
    [[nodiscard]] bool record(ProtocolTraceDirection direction, ProtocolTraceResult result,
                              std::string_view event, std::uint8_t sender_gid = 0U,
                              std::optional<std::uint8_t> recipient_gid = std::nullopt,
                              MessageDeliveryPolicy delivery =
                                  MessageDeliveryPolicy::reliable_ordered,
                              std::uint8_t channel = 0U, std::size_t packet_bytes = 0U,
                              std::string_view reason = {}) noexcept;
    [[nodiscard]] bool record_packet(
        ProtocolTraceDirection direction, ProtocolTraceResult result,
        std::span<const std::uint8_t> application_packet, std::uint8_t sender_gid,
        std::optional<std::uint8_t> recipient_gid, MessageDeliveryPolicy delivery,
        std::uint8_t channel, std::string_view reason = {}) noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    static constexpr std::uint64_t maximum_records = 2'000'000U;

    ProtocolTraceRole role_{ProtocolTraceRole::client};
    ProtocolVersion protocol_{ProtocolVersion::classic_v3};
    std::ofstream output_;
    std::chrono::steady_clock::time_point started_{};
    std::uint64_t sequence_{};
};

} // namespace openminerz::net
