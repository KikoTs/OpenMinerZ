#pragma once

// What mods add to the picture and the speakers after the world is drawn
// (openminerz/modding/graphics.hpp): the audio device that plays the mod
// mixer, and the full-screen grade (LUT) and fade over the world.
//
// PRESENTATION ONLY. Neither exists until a mod asks for it: no device is
// opened and no pass runs for a game without mods.

#include "openminerz/modding/graphics.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>

struct ID3D11Texture2D;

namespace openminerz
{

// Plays a modding::graphics::Mixer through the default output device.
class ModAudioDevice final
{
  public:
    ModAudioDevice();
    ~ModAudioDevice();
    ModAudioDevice(const ModAudioDevice &) = delete;
    ModAudioDevice &operator=(const ModAudioDevice &) = delete;

    [[nodiscard]] bool available() const noexcept
    {
        return stream_ != nullptr;
    }
    [[nodiscard]] const std::string &error() const noexcept
    {
        return error_;
    }
    [[nodiscard]] modding::graphics::Mixer &mixer() noexcept
    {
        return *mixer_;
    }

    // (Polish) Diagnostics: records the next `seconds` of the mix (and the
    // mixer's marks) and writes them to `wav` once that much was played, or
    // when the device closes, whichever is first.
    void start_dump(std::filesystem::path wav, double seconds);
    // Writes a finished dump; call once a frame while one runs.
    void poll_dump();

  private:
    static void SDLCALL feed(void *user, SDL_AudioStream *stream, int additional, int total);
    void finish_dump();
    std::unique_ptr<modding::graphics::Mixer> mixer_;
    SDL_AudioStream *stream_{};
    std::vector<float> scratch_;
    std::string error_;
    bool initialized_{};
    std::unique_ptr<modding::graphics::MixRecorder> dump_;
    std::filesystem::path dump_path_;
};

// The grade and fade over the finished world, before the HUD.
class ModScreenPass final
{
  public:
    explicit ModScreenPass(SDL_Renderer *renderer);
    ~ModScreenPass();
    ModScreenPass(const ModScreenPass &) = delete;
    ModScreenPass &operator=(const ModScreenPass &) = delete;

    // Grades `texture` (the depth renderer's finished world image, before
    // SDL draws it) in place through `lut` at `strength` (0..1). Does nothing
    // without a D3D11 device, a valid LUT or any strength.
    void grade(ID3D11Texture2D *texture, const modding::graphics::Image *lut,
               std::string_view lut_name, float strength);
    // A flat colour over the whole output.
    void fade(modding::graphics::Color color, float alpha, int width, int height);

  private:
    class Impl;
    SDL_Renderer *renderer_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
