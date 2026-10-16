#include "openminerz/net/voice_codec.hpp"
#include "voice_chat_audio.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error{message};
}
} // namespace

int main()
{
    try
    {
        using namespace openminerz;
        using namespace openminerz::net;
        // Golden G.711 / DNA VoiceChat sample values, including both signs,
        // silence, exponent boundaries and saturation.
        struct Sample
        {
            std::int16_t pcm;
            std::uint8_t code;
        };
        const std::array<Sample, 10> golden{{{0, 0xD5},
                                             {8, 0xD5},
                                             {-8, 0x55},
                                             {256, 0xC5},
                                             {-256, 0x45},
                                             {1024, 0xE5},
                                             {-1024, 0x65},
                                             {32767, 0xAA},
                                             {-32768, 0x2A},
                                             {4096, 0x85}}};
        for (const auto &[pcm, code] : golden)
            check(encode_voice_sample(pcm) == code,
                  "A-law encoder differs from original sample values");
        check(decode_voice_sample(0xD5) == 8 && decode_voice_sample(0x55) == -8 &&
                  decode_voice_sample(0xAA) == 32256 && decode_voice_sample(0x2A) == -32256,
              "A-law decoder sign or magnitude differs");
        for (unsigned code = 0; code < 256U; ++code)
            check(encode_voice_sample(decode_voice_sample(static_cast<std::uint8_t>(code))) == code,
                  "A-law codebook is not stable after decode/encode");
        for (int pcm = -32768; pcm <= 32767; ++pcm)
            check(std::abs(pcm - decode_voice_sample(
                                     encode_voice_sample(static_cast<std::int16_t>(pcm)))) <= 512,
                  "voice quantization exceeds the original companding error");
        // No hardware output or microphone is opened by this test.
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        VoiceChatAudio audio;
        check(voice_packet_sample_rate(1600) == 16000 && voice_packet_sample_rate(4410) == 44100 &&
                  voice_packet_sample_rate(4800) == 48000,
              "original 100 ms voice device rates differ");
        std::vector<std::uint8_t> silence(voice_packet_samples, 0xD5);
        check(audio.capture(false).empty(), "microphone captured without push-to-talk");
        for (int packet = 0; packet < 100; ++packet)
        {
            check(audio.receive(1, silence), "dummy playback rejected speaker 1");
            check(audio.receive(2, silence), "dummy playback rejected simultaneous speaker 2");
        }
        audio.set_volume(0.0F);
        check(audio.receive(1, silence), "muted playback rejected a valid packet");
        audio.forget(1);
        audio.set_volume(0.5F);
        check(audio.receive(1, silence), "reconnecting speaker could not recreate playback");
        check(!audio.receive(3, std::vector<std::uint8_t>(voice_sample_rate)),
              "oversized audio queue accepted");
        audio.clear();
        check(audio.capture(false).empty(), "cleared microphone produced audio");
        std::cout << "PASS: original A-law codebook, all 65536 PCM samples, simultaneous playback, "
                     "mute and reconnect\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
