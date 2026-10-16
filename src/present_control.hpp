#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace openminerz
{

// What the display queue behind SDL's Direct3D 11 renderer looks like. Filled
// from the live device and swap chain, not from assumptions about SDL.
struct PresentQueueInfo final
{
    bool available{};
    // Frames DXGI lets the application queue ahead of the display.
    unsigned maximum_frame_latency{};
    unsigned buffer_count{};
    // DXGI_SWAP_EFFECT value and DXGI_SWAP_CHAIN_FLAG bits as created by SDL.
    unsigned swap_effect{};
    unsigned flags{};
    bool flip_model{};
    bool tearing_allowed{};
    unsigned width{};
    unsigned height{};
};

// Thin, optional access to the DXGI objects SDL already owns. Every call is a
// no-op on a renderer that is not Direct3D 11.
class PresentControl final
{
  public:
    explicit PresentControl(SDL_Renderer *renderer);
    ~PresentControl();

    PresentControl(const PresentControl &) = delete;
    PresentControl &operator=(const PresentControl &) = delete;

    [[nodiscard]] PresentQueueInfo info() const;
    // Limits how many finished frames may wait for the display. One is the
    // lowest-latency value; three is the DXGI default.
    bool set_maximum_frame_latency(unsigned frames);

    // Diagnostics: call straight after a present with the performance-counter
    // value at which that frame's input was sampled. Once DXGI reports the
    // frame on screen, the input-to-scanout time lands in latency samples.
    void note_present(std::uint64_t input_counter);
    [[nodiscard]] const std::vector<double> &latency_milliseconds() const noexcept;
    // Refreshes in which the display repeated a frame while the game was
    // presenting, as counted by DXGI frame statistics.
    [[nodiscard]] std::uint64_t missed_refreshes() const noexcept;
    // Measured display refresh interval in milliseconds (0 until known).
    [[nodiscard]] double refresh_interval_milliseconds() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
