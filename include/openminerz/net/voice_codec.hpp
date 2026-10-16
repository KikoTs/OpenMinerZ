#pragma once

#include <cstddef>
#include <cstdint>

namespace openminerz::net
{
// DNA.Net.VoiceChat uses one G.711 A-law byte per mono sample, with the
// XNA microphone's device rate and 100 ms packet duration. The installed PC
// XNA runtime reports 44.1 kHz; 16 kHz is not universal across XNA platforms.
inline constexpr int voice_sample_rate = 44'100;
inline constexpr int voice_packet_samples = voice_sample_rate / 10;
[[nodiscard]] int voice_packet_sample_rate(std::size_t samples) noexcept;
[[nodiscard]] std::uint8_t encode_voice_sample(std::int16_t pcm) noexcept;
[[nodiscard]] std::int16_t decode_voice_sample(std::uint8_t alaw) noexcept;
} // namespace openminerz::net
