#pragma once

// The mod channel's framing: how one mod message (RPC, replicated state, the
// content registry; see openminerz/modding/net.hpp) travels in OpenMinerZ
// extension packets.
//
// A message larger than one 4096-byte extension packet is split into
// fragments. Every fragment is one `mod_data` extension packet:
//
//   "OMZE" u8 extension_version u8 kind(16)
//   u32 sequence      message number, strictly increasing per direction
//   u16 index         fragment number, 0 .. count-1
//   u16 count         fragments in this message
//   u32 total         message length (1 .. receiver's limit)
//   u16 length        bytes in this fragment: mod_frame_max_chunk for every
//                     fragment but the last, which carries the remainder
//   ... chunk ...
//   u32 crc32         over everything before it
//
// The packets ride the existing reliable-ordered path (Steam P2P channel 0
// beside the extension hello; the loopback's control channel), so fragments
// arrive in order and complete. The receiver therefore accepts them strictly
// in order, holds at most one partial message per peer, never trusts a
// declared length beyond its own limit, and drops a partial message that
// stalls. Anything else is a protocol violation by the sender.

#include "openminerz/net/mod_handshake.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace openminerz::net
{

inline constexpr std::size_t mod_frame_header_bytes = 20U;
inline constexpr std::size_t mod_frame_overhead = mod_frame_header_bytes + 4U;
inline constexpr std::size_t mod_frame_max_chunk =
    maximum_extension_packet_size - mod_frame_overhead;
// The largest message any receiver may accept, whatever its own limit says.
inline constexpr std::size_t maximum_mod_message_bytes = 4U * 1024U * 1024U;

enum class ModFrameError : std::uint8_t
{
    none,
    too_small,
    too_large,
    wrong_magic,
    wrong_kind,
    checksum_mismatch,
    invalid_fragment,
    trailing_data,
    // Assembly:
    out_of_order,
    message_too_large,
    rate_limited
};

[[nodiscard]] const char *mod_frame_error_name(ModFrameError error) noexcept;

struct ModFrame final
{
    std::uint32_t sequence{};
    std::uint16_t index{};
    std::uint16_t count{};
    std::uint32_t total{};
    std::span<const std::uint8_t> chunk;
};

// Validates one packet completely: magic, version, kind, CRC, fragment
// geometry (count matches total; chunk length matches index) and length.
[[nodiscard]] bool decode_mod_frame(std::span<const std::uint8_t> packet, ModFrame &frame,
                                    ModFrameError &error) noexcept;

// Splits `message` (1 .. maximum_mod_message_bytes) into the packets of
// message `sequence`. Returns no packets for an empty or oversized message.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> encode_mod_message(
    std::uint32_t sequence, std::span<const std::uint8_t> message);

[[nodiscard]] constexpr std::size_t mod_fragment_count(std::size_t total) noexcept
{
    return (total + mod_frame_max_chunk - 1U) / mod_frame_max_chunk;
}

// Reassembles one peer's messages. Strictly in order; one partial at a time.
class ModMessageAssembler final
{
  public:
    enum class Result : std::uint8_t
    {
        pending,
        complete,
        rejected
    };

    explicit ModMessageAssembler(std::size_t maximum_message_bytes = 64U * 1024U,
                                 double timeout_seconds = 10.0) noexcept;

    // On `complete`, `message` holds the whole message. On `rejected`, any
    // partial message is discarded and `error` says why.
    Result accept(const ModFrame &frame, double now, std::vector<std::uint8_t> &message,
                  ModFrameError &error);
    // Drops a partial message older than the timeout; true when one was dropped.
    bool expire(double now) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept
    {
        return partial_.size();
    }
    [[nodiscard]] std::uint32_t last_sequence() const noexcept
    {
        return last_sequence_;
    }

  private:
    std::size_t maximum_message_bytes_{};
    double timeout_seconds_{};
    std::vector<std::uint8_t> partial_;
    bool assembling_{};
    std::uint32_t sequence_{};
    std::uint16_t next_index_{};
    std::uint16_t count_{};
    std::uint32_t total_{};
    double started_{};
    std::uint32_t last_sequence_{};
};

// A classic token bucket: `rate` tokens per second up to `burst`. Starts full.
class TokenBucket final
{
  public:
    TokenBucket() noexcept = default;
    TokenBucket(double rate, double burst) noexcept;
    // Takes `amount` when that many tokens are available.
    bool take(double amount, double now) noexcept;
    // Takes `amount` even when that goes negative (a large item that would
    // otherwise never fit), as long as the bucket is not already in debt.
    bool take_allowing_debt(double amount, double now) noexcept;
    // Takes `amount` unconditionally (it was already budgeted).
    void consume(double amount, double now) noexcept;
    [[nodiscard]] double available(double now) noexcept;

  private:
    void refill(double now) noexcept;

    double rate_{};
    double burst_{};
    double tokens_{};
    double last_{};
    bool started_{};
};

} // namespace openminerz::net
