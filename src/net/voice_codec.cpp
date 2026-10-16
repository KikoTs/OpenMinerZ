#include "openminerz/net/voice_codec.hpp"

#include <algorithm>

namespace openminerz::net
{
int voice_packet_sample_rate(std::size_t samples) noexcept
{
    // The original message has no rate field. Its 100 ms buffer length gives
    // the rate for standard XNA device formats; short/nonstandard frames use
    // the native PC format. No hardware format is changed by this inference.
    for (const int rate : {8000, 11025, 16000, 22050, 32000, 44100, 48000})
        if (samples == static_cast<std::size_t>(rate / 10))
            return rate;
    return voice_sample_rate;
}

std::uint8_t encode_voice_sample(std::int16_t sample) noexcept
{
    int pcm = sample;
    const int sign = pcm < 0 ? 0x80 : 0;
    if (sign != 0)
        pcm = -pcm;
    pcm = std::min(pcm, 32767);
    int exponent = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; mask >>= 1)
        --exponent;
    const int mantissa = (pcm >> (exponent == 0 ? 4 : exponent + 3)) & 15;
    return static_cast<std::uint8_t>((sign | (exponent << 4) | mantissa) ^ 0xD5);
}

std::int16_t decode_voice_sample(std::uint8_t code) noexcept
{
    const unsigned value = code ^ 0xD5U;
    const unsigned exponent = (value & 0x70U) >> 4U;
    int pcm = static_cast<int>((value & 15U) << 4U) + 8;
    if (exponent != 0U)
        pcm += 256;
    if (exponent > 1U)
        pcm <<= exponent - 1U;
    return static_cast<std::int16_t>((value & 0x80U) == 0U ? pcm : -pcm);
}
} // namespace openminerz::net
