#include "openminerz/frame_pacing.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace openminerz::pacing
{

float interpolation_alpha(float accumulator_seconds, float fixed_delta_seconds) noexcept
{
    if (!(fixed_delta_seconds > 0.0F) || !std::isfinite(accumulator_seconds))
        return 1.0F;
    return std::clamp(accumulator_seconds / fixed_delta_seconds, 0.0F, 1.0F);
}

world::Vec3 lerp(world::Vec3 from, world::Vec3 to, float alpha) noexcept
{
    return {from.x + (to.x - from.x) * alpha, from.y + (to.y - from.y) * alpha,
            from.z + (to.z - from.z) * alpha};
}

float distance(world::Vec3 left, world::Vec3 right) noexcept
{
    const float x = left.x - right.x;
    const float y = left.y - right.y;
    const float z = left.z - right.z;
    return std::sqrt(x * x + y * y + z * z);
}

float lerp_angle(float from, float to, float alpha) noexcept
{
    constexpr float pi = 3.14159265358979323846F;
    float difference = std::fmod(to - from, 2.0F * pi);
    if (difference > pi)
        difference -= 2.0F * pi;
    else if (difference < -pi)
        difference += 2.0F * pi;
    return from + difference * alpha;
}

void FixedStepCamera::begin_tick(const world::PlayerState &state) noexcept
{
    previous_position_ = state.position;
    previous_offset_ = state.step_view_offset;
}

void FixedStepCamera::end_tick(const world::PlayerState &state) noexcept
{
    current_position_ = state.position;
    current_offset_ = state.step_view_offset;
    valid_ = true;
}

CameraSample FixedStepCamera::present(const world::PlayerState &live, float alpha) noexcept
{
    const bool moved_outside_tick =
        !valid_ || live.position.x != current_position_.x ||
        live.position.y != current_position_.y || live.position.z != current_position_.z ||
        live.step_view_offset != current_offset_;
    if (moved_outside_tick)
    {
        // Something other than a simulation tick placed the player here. Adopt
        // it on both sides so the following frames do not blend from the old
        // location either.
        previous_position_ = current_position_ = live.position;
        previous_offset_ = current_offset_ = live.step_view_offset;
        valid_ = true;
        return {live.position, live.step_view_offset, true};
    }
    if (distance(previous_position_, current_position_) > teleport_snap_distance)
    {
        previous_position_ = current_position_;
        previous_offset_ = current_offset_;
        return {current_position_, current_offset_, true};
    }
    const float blend = std::clamp(alpha, 0.0F, 1.0F);
    return {lerp(previous_position_, current_position_, blend),
            previous_offset_ + (current_offset_ - previous_offset_) * blend, false};
}

void FixedStepCamera::reset() noexcept
{
    *this = FixedStepCamera{};
}

std::int64_t FrameLimiter::next_deadline(std::int64_t now_ns, std::int64_t interval_ns) noexcept
{
    if (interval_ns <= 0)
    {
        armed_ = false;
        return now_ns;
    }
    if (!armed_ || interval_ns != interval_ns_)
    {
        // First frame at this rate: the schedule starts here.
        armed_ = true;
        interval_ns_ = interval_ns;
        deadline_ns_ = now_ns;
        return now_ns;
    }
    deadline_ns_ += interval_ns;
    if (now_ns >= deadline_ns_)
    {
        // The frame overran its slot. A small overrun keeps the schedule so
        // the average rate stays exact; a large one restarts it rather than
        // racing through a backlog of short frames.
        if (now_ns - deadline_ns_ > interval_ns)
            deadline_ns_ = now_ns;
        return now_ns;
    }
    return deadline_ns_;
}

std::int64_t FrameLimiter::coarse_sleep_ns(std::int64_t now_ns,
                                           std::int64_t deadline_ns) const noexcept
{
    const std::int64_t remaining = deadline_ns - now_ns;
    return remaining > spin_margin_ns_ ? remaining - spin_margin_ns_ : 0;
}

void FrameLimiter::record_sleep(std::int64_t requested_ns, std::int64_t actual_ns) noexcept
{
    const std::int64_t overshoot = std::max<std::int64_t>(0, actual_ns - requested_ns);
    // Cover one and a half times the worst recent oversleep, and relax by a
    // sixteenth per sleep so one scheduler hiccup does not cost CPU forever.
    const std::int64_t wanted = overshoot + overshoot / 2 + minimum_spin_ns;
    const std::int64_t relaxed = spin_margin_ns_ - spin_margin_ns_ / 16;
    spin_margin_ns_ = std::clamp(std::max(wanted, relaxed), minimum_spin_ns, maximum_spin_ns);
}

void FrameLimiter::reset() noexcept
{
    armed_ = false;
    deadline_ns_ = 0;
    interval_ns_ = 0;
}

std::int64_t PresentLatencyPacer::update(std::int64_t block_ns, std::int64_t interval_ns) noexcept
{
    if (interval_ns <= target_block_ns || block_ns < 0)
    {
        delay_ns_ = 0;
        return 0;
    }
    if (block_ns >= target_block_ns)
    {
        // Present is still holding a finished frame: claim a quarter of the
        // surplus per frame so one long block cannot overshoot.
        delay_ns_ += (block_ns - target_block_ns) / 4;
    }
    else
    {
        // The frame arrived with less slack than wanted. Give back at least
        // the shortfall, and at least a quarter of the delay, straight away.
        delay_ns_ -= std::max(target_block_ns - block_ns, delay_ns_ / 4);
    }
    delay_ns_ = std::clamp<std::int64_t>(delay_ns_, 0, interval_ns - target_block_ns);
    return delay_ns_;
}

void PresentLatencyPacer::reset() noexcept
{
    delay_ns_ = 0;
}

FrameTimeSummary summarize_frame_times(std::span<const double> milliseconds)
{
    FrameTimeSummary summary;
    summary.frames = milliseconds.size();
    if (milliseconds.empty())
        return summary;
    std::vector<double> sorted{milliseconds.begin(), milliseconds.end()};
    std::sort(sorted.begin(), sorted.end());
    double total{};
    for (const double sample : sorted)
        total += sample;
    const auto percentile = [&](double fraction) {
        return sorted[static_cast<std::size_t>(static_cast<double>(sorted.size() - 1U) *
                                               fraction)];
    };
    summary.average_ms = total / static_cast<double>(sorted.size());
    summary.median_ms = sorted[sorted.size() / 2U];
    summary.p95_ms = percentile(0.95);
    summary.p99_ms = percentile(0.99);
    summary.minimum_ms = sorted.front();
    summary.maximum_ms = sorted.back();
    double variance{};
    constexpr std::array<double, 7> edges{1.0, 2.0, 4.0, 8.0, 16.0, 33.0, 66.0};
    for (const double sample : sorted)
    {
        variance += (sample - summary.average_ms) * (sample - summary.average_ms);
        if (sample > summary.median_ms * 2.0)
            ++summary.hitches;
        if (sample > 1000.0 / 60.0)
            ++summary.slow_frames;
        std::size_t bucket{};
        while (bucket < edges.size() && sample >= edges[bucket])
            ++bucket;
        ++summary.histogram[bucket];
    }
    summary.deviation_ms = std::sqrt(variance / static_cast<double>(sorted.size()));
    return summary;
}

MotionSummary summarize_motion(std::span<const MotionSample> samples)
{
    MotionSummary summary;
    double total_seconds{};
    double total_distance{};
    for (const MotionSample &sample : samples)
    {
        if (!(sample.seconds > 0.0))
            continue;
        ++summary.frames;
        total_seconds += sample.seconds;
        total_distance += sample.displacement;
    }
    if (summary.frames == 0U || !(total_seconds > 0.0))
        return summary;
    summary.mean_speed = total_distance / total_seconds;
    if (!(summary.mean_speed > 0.0))
        return summary;
    double speed_variance{};
    double error_squares{};
    std::size_t stalled{};
    for (const MotionSample &sample : samples)
    {
        if (!(sample.seconds > 0.0))
            continue;
        const double speed = sample.displacement / sample.seconds;
        speed_variance += (speed - summary.mean_speed) * (speed - summary.mean_speed);
        const double expected = summary.mean_speed * sample.seconds;
        const double error = sample.displacement - expected;
        error_squares += error * error;
        summary.worst_step_error = std::max(summary.worst_step_error, std::abs(error));
        if (sample.displacement < expected * 0.05)
            ++stalled;
    }
    const double count = static_cast<double>(summary.frames);
    summary.speed_variation = std::sqrt(speed_variance / count) / summary.mean_speed;
    summary.rms_step_error = std::sqrt(error_squares / count);
    summary.stalled_fraction = static_cast<double>(stalled) / count;
    return summary;
}

} // namespace openminerz::pacing
