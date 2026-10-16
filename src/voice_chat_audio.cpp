#include "voice_chat_audio.hpp"
#include "openminerz/net/voice_codec.hpp"

#include <algorithm>
#include <array>

namespace openminerz
{
namespace
{
constexpr SDL_AudioSpec voice_spec{SDL_AUDIO_S16, 1, net::voice_sample_rate};
constexpr int maximum_queued_bytes = net::voice_sample_rate; // 500 ms of signed 16-bit PCM.
} // namespace

VoiceChatAudio::~VoiceChatAudio()
{
    clear();
    if (initialized_)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool VoiceChatAudio::initialize()
{
    if (!initialized_)
    {
        initialized_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!initialized_)
            error_ = SDL_GetError();
    }
    return initialized_;
}

void VoiceChatAudio::set_volume(float volume) noexcept
{
    volume_ = std::clamp(volume, 0.0F, 1.0F);
    for (const auto &[gid, speaker] : playback_)
        SDL_SetAudioStreamGain(speaker.stream, volume_);
}

bool VoiceChatAudio::receive(std::uint8_t gid, std::span<const std::uint8_t> audio)
{
    if (audio.empty())
        return true;
    if (audio.size() > static_cast<std::size_t>(maximum_queued_bytes / 2))
        return false;
    if (volume_ == 0.0F)
        return true;
    if (!initialize())
        return false;
    const int rate = net::voice_packet_sample_rate(audio.size());
    auto found = playback_.find(gid);
    if (found != playback_.end() && found->second.sample_rate != rate)
    {
        forget(gid);
        found = playback_.end();
    }
    if (found == playback_.end())
    {
        // Each peer gets its own stream so simultaneous speakers mix instead
        // of forming an ever-growing serial queue.
        if (playback_.size() >= 32U)
            return false;
        const SDL_AudioSpec incoming{SDL_AUDIO_S16, 1, rate};
        auto *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &incoming,
                                                 nullptr, nullptr);
        if (stream == nullptr)
        {
            error_ = SDL_GetError();
            return false;
        }
        SDL_SetAudioStreamGain(stream, volume_);
        if (!SDL_ResumeAudioStreamDevice(stream))
        {
            error_ = SDL_GetError();
            SDL_DestroyAudioStream(stream);
            return false;
        }
        found = playback_.emplace(gid, Speaker{stream, rate}).first;
    }
    std::vector<std::int16_t> pcm(audio.size());
    std::ranges::transform(audio, pcm.begin(), net::decode_voice_sample);
    auto *stream = found->second.stream;
    const int size = static_cast<int>(pcm.size() * sizeof(std::int16_t));
    if (SDL_GetAudioStreamQueued(stream) > rate - size)
        SDL_ClearAudioStream(stream);
    if (!SDL_PutAudioStreamData(stream, pcm.data(), size))
    {
        error_ = SDL_GetError();
        return false;
    }
    return true;
}

std::vector<std::vector<std::uint8_t>> VoiceChatAudio::capture(bool push_to_talk)
{
    std::vector<std::vector<std::uint8_t>> packets;
    if (!push_to_talk)
    {
        if (microphone_ != nullptr)
            SDL_DestroyAudioStream(microphone_);
        microphone_ = nullptr;
        capturing_ = false;
        return packets;
    }
    if (!capturing_)
    {
        capturing_ = true;
        error_.clear();
        if (initialize())
            microphone_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &voice_spec,
                                                    nullptr, nullptr);
        if (microphone_ == nullptr || !SDL_ResumeAudioStreamDevice(microphone_))
        {
            error_ = SDL_GetError();
            if (microphone_ != nullptr)
                SDL_DestroyAudioStream(microphone_);
            microphone_ = nullptr;
        }
    }
    if (microphone_ == nullptr)
        return packets;
    std::array<std::int16_t, net::voice_packet_samples> pcm{};
    if (SDL_GetAudioStreamAvailable(microphone_) > maximum_queued_bytes)
        SDL_ClearAudioStream(microphone_);
    for (int packet = 0;
         packet < 3 && SDL_GetAudioStreamAvailable(microphone_) >= static_cast<int>(sizeof(pcm));
         ++packet)
    {
        if (SDL_GetAudioStreamData(microphone_, pcm.data(), static_cast<int>(sizeof(pcm))) !=
            sizeof(pcm))
        {
            error_ = SDL_GetError();
            break;
        }
        auto &audio = packets.emplace_back(pcm.size());
        std::ranges::transform(pcm, audio.begin(), net::encode_voice_sample);
    }
    return packets;
}

void VoiceChatAudio::forget(std::uint8_t gid) noexcept
{
    const auto found = playback_.find(gid);
    if (found != playback_.end())
    {
        SDL_DestroyAudioStream(found->second.stream);
        playback_.erase(found);
    }
}

void VoiceChatAudio::clear() noexcept
{
    for (const auto &[gid, speaker] : playback_)
        SDL_DestroyAudioStream(speaker.stream);
    playback_.clear();
    if (microphone_ != nullptr)
        SDL_DestroyAudioStream(microphone_);
    microphone_ = nullptr;
    capturing_ = false;
    error_.clear();
}
} // namespace openminerz
