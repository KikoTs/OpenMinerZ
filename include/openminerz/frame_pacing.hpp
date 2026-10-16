#pragma once

#include "openminerz/world/player.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Presentation-side frame pacing. Nothing in this header feeds back into the
// fixed 60 Hz simulation: it only decides which already-simulated state a
// rendered frame shows, how long the loop waits between frames, and how the
// diagnostics summarise what was presented.
namespace openminerz::pacing
{

// ---------------------------------------------------------------------------
// Fixed-step presentation
// ---------------------------------------------------------------------------

// No simulated motion covers this much ground in one 60 Hz tick (240 m/s);
// a larger step is a spawn, respawn, load or teleport and must cut, not smear.
inline constexpr float teleport_snap_distance = 4.0F;

// Fraction of the way from the previous tick to the current one that the
// unconsumed accumulator represents, clamped to [0, 1].
[[nodiscard]] float interpolation_alpha(float accumulator_seconds,
                                        float fixed_delta_seconds) noexcept;

[[nodiscard]] world::Vec3 lerp(world::Vec3 from, world::Vec3 to, float alpha) noexcept;
[[nodiscard]] float distance(world::Vec3 left, world::Vec3 right) noexcept;
// Shortest-arc interpolation between two headings in radians.
[[nodiscard]] float lerp_angle(float from, float to, float alpha) noexcept;

struct CameraSample final
{
    world::Vec3 position{};
    float step_view_offset{};
    // True when the sample cut straight to the live state.
    bool snapped{};
};

// Remembers the camera-carrying part of the player state on both sides of the
// most recent simulation tick and blends between them for presentation.
class FixedStepCamera final
{
  public:
    // Call immediately before and after every fixed simulation tick.
    void begin_tick(const world::PlayerState &state) noexcept;
    void end_tick(const world::PlayerState &state) noexcept;
    // The pose to render. The live state is authoritative: when anything moved
    // the player outside a tick (spawn, teleport, load) or the last tick
    // covered more than teleport_snap_distance, the camera cuts to it.
    [[nodiscard]] CameraSample present(const world::PlayerState &live, float alpha) noexcept;
    void reset() noexcept;

  private:
    world::Vec3 previous_position_{};
    world::Vec3 current_position_{};
    float previous_offset_{};
    float current_offset_{};
    bool valid_{};
};

// ---------------------------------------------------------------------------
// Frame limiter
// ---------------------------------------------------------------------------

// Deadline scheduler for a sleep-then-spin limiter. All times are nanoseconds
// on one monotonic clock. Deadlines advance by exactly one interval so sleep
// error never accumulates into drift; a frame that overruns by more than one
// interval drops the debt instead of bursting to catch up.
class FrameLimiter final
{
  public:
    // Longest stretch the limiter is willing to burn in a spin loop.
    static constexpr std::int64_t maximum_spin_ns = 4'000'000;
    static constexpr std::int64_t minimum_spin_ns = 200'000;
    static constexpr std::int64_t default_spin_ns = 1'500'000;

    // Deadline at which the frame that just finished should hand over to the
    // next one. Returns now_ns (no wait) when the frame already overran.
    [[nodiscard]] std::int64_t next_deadline(std::int64_t now_ns, std::int64_t interval_ns) noexcept;
    // Portion of the wait that may be handed to the OS sleep; the rest is spun.
    [[nodiscard]] std::int64_t coarse_sleep_ns(std::int64_t now_ns,
                                               std::int64_t deadline_ns) const noexcept;
    // Feed back how long an OS sleep really took so the spin margin tracks the
    // machine's actual timer behaviour.
    void record_sleep(std::int64_t requested_ns, std::int64_t actual_ns) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::int64_t spin_margin_ns() const noexcept
    {
        return spin_margin_ns_;
    }

  private:
    std::int64_t deadline_ns_{};
    std::int64_t interval_ns_{};
    std::int64_t spin_margin_ns_{default_spin_ns};
    bool armed_{};
};

// Under vsync a finished frame waits inside Present for the previous frame to
// leave the queue. Every millisecond spent blocked there is a millisecond the
// input it was built from grows stale. This controller converts the measured
// block time into a delay inserted *before* input is sampled, so the frame is
// finished just in time instead of early. It only ever uses slack that Present
// was already wasting, and backs off immediately when the slack disappears.
class PresentLatencyPacer final
{
  public:
    // Block time the pacer leaves in place as protection against a late frame.
    static constexpr std::int64_t target_block_ns = 1'500'000;

    // block_ns: time the last Present call blocked. interval_ns: display
    // refresh interval (0 when unknown). Returns the delay to apply before the
    // next frame samples input.
    [[nodiscard]] std::int64_t update(std::int64_t block_ns, std::int64_t interval_ns) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::int64_t delay_ns() const noexcept
    {
        return delay_ns_;
    }

  private:
    std::int64_t delay_ns_{};
};

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct FrameTimeSummary final
{
    std::size_t frames{};
    double average_ms{};
    double median_ms{};
    double p95_ms{};
    double p99_ms{};
    double minimum_ms{};
    double maximum_ms{};
    double deviation_ms{};
    // Frames that took more than twice the median.
    std::size_t hitches{};
    // Frames that took more than 16.7 ms, i.e. visibly dropped at 60 Hz.
    std::size_t slow_frames{};
    // Buckets: <1, <2, <4, <8, <16, <33, <66, >=66 ms.
    std::array<std::size_t, 8> histogram{};
};

[[nodiscard]] FrameTimeSummary summarize_frame_times(std::span<const double> milliseconds);

struct MotionSample final
{
    double seconds{};
    double displacement{};
};

struct MotionSummary final
{
    std::size_t frames{};
    // Total distance over total time.
    double mean_speed{};
    // Standard deviation of per-frame speed divided by the mean speed. Zero
    // for perfectly even motion; near two for 60 Hz steps shown at 240 Hz.
    double speed_variation{};
    // RMS distance between where the camera was drawn and where uniform motion
    // would have put it, per frame, in world units.
    double rms_step_error{};
    // Largest single-frame departure from uniform motion, in world units.
    double worst_step_error{};
    // Share of frames in which a moving camera did not move at all.
    double stalled_fraction{};
};

[[nodiscard]] MotionSummary summarize_motion(std::span<const MotionSample> samples);

} // namespace openminerz::pacing
