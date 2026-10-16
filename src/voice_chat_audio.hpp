#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace openminerz
{
class VoiceChatAudio final
{
  public:
    ~VoiceChatAudio();
    void set_volume(float volume) noexcept;
    [[nodiscard]] bool receive(std::uint8_t gid, std::span<const std::uint8_t> audio);
    // The microphone is opened only while push-to-talk is held in focused
    // gameplay. Releasing it closes the device and discards untransmitted audio.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> capture(bool push_to_talk);
    void forget(std::uint8_t gid) noexcept;
    void clear() noexcept;
    [[nodiscard]] const std::string &error() const noexcept
    {
        return error_;
    }

  private:
    [[nodiscard]] bool initialize();
    struct Speaker
    {
        SDL_AudioStream *stream{};
        int sample_rate{};
    };
    std::map<std::uint8_t, Speaker> playback_;
    SDL_AudioStream *microphone_{};
    bool initialized_{};
    bool capturing_{};
    float volume_{1.0F};
    std::string error_;
};
} // namespace openminerz
