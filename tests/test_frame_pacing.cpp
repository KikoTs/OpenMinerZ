#include "openminerz/frame_pacing.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/world/player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace openminerz;

namespace
{

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double left, double right, double tolerance)
{
    return std::abs(left - right) <= tolerance;
}

constexpr float fixed_delta = 1.0F / 60.0F;

world::PlayerState state_at(float x, float y, float z, float offset = 0.0F)
{
    world::PlayerState state;
    state.position = {x, y, z};
    state.step_view_offset = offset;
    return state;
}

void alpha_is_the_clamped_unconsumed_tick_fraction()
{
    check(pacing::interpolation_alpha(0.0F, fixed_delta) == 0.0F, "empty accumulator is alpha 0");
    check(near(pacing::interpolation_alpha(fixed_delta * 0.25F, fixed_delta), 0.25, 1e-6),
          "quarter tick is alpha 0.25");
    check(pacing::interpolation_alpha(fixed_delta, fixed_delta) == 1.0F, "full tick is alpha 1");
    check(pacing::interpolation_alpha(0.25F, fixed_delta) == 1.0F,
          "a clamped backlog never extrapolates past the current tick");
    check(pacing::interpolation_alpha(-0.01F, fixed_delta) == 0.0F, "negative remainder clamps");
    check(pacing::interpolation_alpha(0.01F, 0.0F) == 1.0F, "degenerate step shows the live state");
    check(pacing::interpolation_alpha(std::nanf(""), fixed_delta) == 1.0F,
          "a non-finite accumulator shows the live state");
}

void camera_blends_between_the_last_two_ticks()
{
    pacing::FixedStepCamera camera;
    world::PlayerState state = state_at(10.0F, 5.0F, -3.0F, 0.2F);
    camera.begin_tick(state);
    state.position.x += 5.0F * fixed_delta;
    state.position.y += 0.03F;
    state.step_view_offset = 0.1F;
    camera.end_tick(state);

    const auto start = camera.present(state, 0.0F);
    check(!start.snapped && near(start.position.x, 10.0, 1e-5) &&
              near(start.position.y, 5.0, 1e-5) && near(start.step_view_offset, 0.2, 1e-6),
          "alpha 0 shows the previous tick");
    const auto middle = camera.present(state, 0.5F);
    check(near(middle.position.x, 10.0 + 2.5 * fixed_delta, 1e-5) &&
              near(middle.position.y, 5.015, 1e-5) && near(middle.step_view_offset, 0.15, 1e-6),
          "alpha 0.5 is halfway, including the step-view offset");
    const auto end = camera.present(state, 1.0F);
    check(near(end.position.x, state.position.x, 1e-6) &&
              near(end.step_view_offset, 0.1, 1e-6),
          "alpha 1 shows the current tick");
    const auto beyond = camera.present(state, 4.0F);
    check(near(beyond.position.x, state.position.x, 1e-6), "alpha is clamped, never extrapolated");
}

void steady_walking_presents_evenly_at_any_refresh_rate()
{
    // The property the user sees: a camera walking at constant speed must
    // cover the same distance every presented frame no matter how the display
    // rate relates to the 60 Hz simulation.
    for (const double refresh : {48.0, 60.0, 75.0, 144.0, 165.0, 239.76, 360.0})
    {
        pacing::FixedStepCamera camera;
        world::PlayerState state = state_at(0.0F, 0.0F, 0.0F);
        float accumulator{};
        const float frame = static_cast<float>(1.0 / refresh);
        std::vector<pacing::MotionSample> interpolated;
        std::vector<pacing::MotionSample> stepped;
        world::Vec3 last_interpolated{};
        world::Vec3 last_stepped{};
        for (int index = 0; index < 2400; ++index)
        {
            accumulator += frame;
            while (accumulator >= fixed_delta)
            {
                camera.begin_tick(state);
                state.position.x += 5.0F * fixed_delta;
                camera.end_tick(state);
                accumulator -= fixed_delta;
            }
            const auto sample =
                camera.present(state, pacing::interpolation_alpha(accumulator, fixed_delta));
            if (index > 8)
            {
                interpolated.push_back({frame, pacing::distance(sample.position, last_interpolated)});
                stepped.push_back({frame, pacing::distance(state.position, last_stepped)});
            }
            last_interpolated = sample.position;
            last_stepped = state.position;
        }
        const auto smooth = pacing::summarize_motion(interpolated);
        const auto raw = pacing::summarize_motion(stepped);
        check(near(smooth.mean_speed, 5.0, 0.01), "interpolation preserves the simulated speed");
        check(smooth.speed_variation < 0.01, "interpolated walking has no per-frame speed variation");
        check(smooth.stalled_fraction == 0.0, "interpolated walking never repeats a camera position");
        check(smooth.rms_step_error < 0.0005, "interpolated walking stays within half a millimetre");
        if (std::abs(refresh - 60.0) > 0.5)
        {
            check(raw.speed_variation > 0.2,
                  "the metric must expose un-interpolated 60 Hz stepping at this refresh rate");
            check(raw.rms_step_error > smooth.rms_step_error * 20.0,
                  "raw tick positions are at least twenty times less even than the blend");
        }
    }
}

void teleports_cut_instead_of_smearing()
{
    pacing::FixedStepCamera camera;
    world::PlayerState state = state_at(0.0F, 10.0F, 0.0F);
    check(camera.present(state, 0.3F).snapped, "the first presented frame cuts to the live state");
    camera.begin_tick(state);
    state.position.x += 0.08F;
    camera.end_tick(state);
    check(!camera.present(state, 0.5F).snapped, "ordinary motion blends");

    // A teleport, respawn or load writes the position outside any tick.
    state.position = {500.0F, 20.0F, -300.0F};
    const auto cut = camera.present(state, 0.5F);
    check(cut.snapped && cut.position.x == 500.0F && cut.position.y == 20.0F &&
              cut.position.z == -300.0F,
          "an external move is shown exactly, with no blend from the old place");
    const auto after = camera.present(state, 0.1F);
    check(after.position.x == 500.0F && after.position.z == -300.0F,
          "later frames do not drift back toward the pre-teleport position");

    // The next tick blends from the teleport destination, not the origin.
    camera.begin_tick(state);
    state.position.x += 0.08F;
    camera.end_tick(state);
    const auto resumed = camera.present(state, 0.5F);
    check(!resumed.snapped && near(resumed.position.x, 500.04, 1e-3),
          "motion resumes smoothly from the destination");

    // A respawn executed inside a tick shows up as an impossible step.
    camera.begin_tick(state);
    state.position = {8.0F, 64.0F, -8.0F};
    camera.end_tick(state);
    const auto respawn = camera.present(state, 0.25F);
    check(respawn.snapped && respawn.position.x == 8.0F && respawn.position.y == 64.0F,
          "a step longer than the snap distance cuts to the new state");
    check(camera.present(state, 0.25F).position.x == 8.0F, "and stays cut on the following frames");

    // Fast but legitimate motion (terminal fall, top flight speed) still blends.
    camera.begin_tick(state);
    state.position.y -= 1.2F;
    state.position.x += 32.0F * fixed_delta;
    camera.end_tick(state);
    check(!camera.present(state, 0.5F).snapped, "fast legitimate motion is interpolated");
    check(pacing::teleport_snap_distance > 32.0F * fixed_delta * 2.0F,
          "the snap distance leaves headroom over the fastest simulated movement");

    // Only the step-view offset changing outside a tick is also an external edit.
    state.step_view_offset = 0.4F;
    check(camera.present(state, 0.5F).snapped, "an externally edited view offset is adopted");

    camera.reset();
    check(camera.present(state, 0.5F).snapped, "reset forgets the blend history");
}

void angle_interpolation_takes_the_short_way_round()
{
    constexpr float pi = 3.14159265358979323846F;
    check(near(pacing::lerp_angle(0.0F, 1.0F, 0.5F), 0.5, 1e-6), "plain blend");
    const float wrapped = pacing::lerp_angle(pi - 0.1F, -pi + 0.1F, 0.5F);
    check(near(std::abs(wrapped), pi, 1e-4), "crossing the seam does not swing through zero");
    check(near(pacing::lerp_angle(-pi + 0.1F, pi - 0.1F, 0.25F), -pi + 0.05, 1e-4),
          "the short arc is used in both directions");
    check(near(pacing::lerp_angle(1.0F, 1.0F, 0.7F), 1.0, 1e-6), "equal headings are stable");
}

void limiter_keeps_an_exact_cadence_without_drift_or_bursts()
{
    pacing::FrameLimiter limiter;
    const std::int64_t interval = 1'000'000'000LL / 144;
    std::int64_t now = 5'000'000'000LL;
    check(limiter.next_deadline(now, interval) == now, "the first frame starts the schedule");

    // Frames that finish early wait for evenly spaced deadlines even when the
    // wake-up itself is late by a varying amount.
    const std::int64_t origin = now;
    for (int frame = 1; frame <= 1000; ++frame)
    {
        now += 2'000'000 + (frame % 7) * 100'000; // work
        const std::int64_t deadline = limiter.next_deadline(now, interval);
        check(deadline == origin + interval * frame, "deadlines advance by exactly one interval");
        check(deadline >= now, "an early frame always waits");
        now = deadline + (frame % 5) * 40'000; // imperfect wake-up
    }
    check(limiter.next_deadline(now + 1'000'000, interval) == origin + interval * 1001,
          "wake-up error never accumulates into the schedule");

    // A small overrun does not wait and does not move the schedule.
    now = origin + interval * 1002 + interval / 2;
    check(limiter.next_deadline(now, interval) == now, "an overrun frame does not wait");
    check(limiter.next_deadline(now + 1'000'000, interval) == origin + interval * 1003,
          "a small overrun is absorbed by the next slot");

    // A long hitch drops the debt instead of racing through short frames.
    now = origin + interval * 1003 + 250'000'000;
    check(limiter.next_deadline(now, interval) == now, "a hitch frame does not wait");
    const std::int64_t resumed = limiter.next_deadline(now + 1'000'000, interval);
    check(resumed == now + interval, "the schedule restarts at the hitch, with no catch-up burst");

    // Changing the cap restarts the schedule at the new rate.
    const std::int64_t slower = 1'000'000'000LL / 60;
    now = resumed + 10'000'000;
    check(limiter.next_deadline(now, slower) == now, "a new rate begins a new schedule");
    check(limiter.next_deadline(now + 3'000'000, slower) == now + slower,
          "and then holds that rate");

    check(limiter.next_deadline(now, 0) == now, "an uncapped interval never waits");
    limiter.reset();
    check(limiter.next_deadline(123, interval) == 123, "reset forgets the schedule");
}

void limiter_sleeps_most_of_the_wait_and_spins_the_rest()
{
    pacing::FrameLimiter limiter;
    const std::int64_t margin = limiter.spin_margin_ns();
    check(margin == pacing::FrameLimiter::default_spin_ns, "default spin margin");
    check(limiter.coarse_sleep_ns(0, 6'000'000) == 6'000'000 - margin,
          "the OS sleep stops one spin margin short of the deadline");
    check(limiter.coarse_sleep_ns(0, margin) == 0, "a wait within the margin is spun entirely");
    check(limiter.coarse_sleep_ns(10, 5) == 0, "a passed deadline never sleeps");

    // An oversleeping timer widens the margin at once...
    limiter.record_sleep(4'000'000, 5'800'000);
    check(limiter.spin_margin_ns() >= 1'800'000 + 900'000,
          "the margin covers one and a half times the observed oversleep");
    check(limiter.spin_margin_ns() <= pacing::FrameLimiter::maximum_spin_ns, "bounded above");
    // ...an absurd one is capped...
    limiter.record_sleep(1'000'000, 60'000'000);
    check(limiter.spin_margin_ns() == pacing::FrameLimiter::maximum_spin_ns,
          "the spin never exceeds its ceiling");
    // ...and accurate sleeps relax it gradually back to the floor.
    std::int64_t previous = limiter.spin_margin_ns();
    for (int index = 0; index < 200; ++index)
    {
        limiter.record_sleep(4'000'000, 4'020'000);
        check(limiter.spin_margin_ns() <= previous, "the margin relaxes monotonically");
        previous = limiter.spin_margin_ns();
    }
    check(limiter.spin_margin_ns() < 400'000 &&
              limiter.spin_margin_ns() >= pacing::FrameLimiter::minimum_spin_ns,
          "a precise timer earns a short spin");
    limiter.record_sleep(4'000'000, 3'900'000);
    check(limiter.spin_margin_ns() >= pacing::FrameLimiter::minimum_spin_ns,
          "an early wake-up never drives the margin below the floor");
}

void present_pacer_only_spends_slack_and_backs_off_at_once()
{
    using Pacer = pacing::PresentLatencyPacer;
    const std::int64_t interval = 1'000'000'000LL / 60;
    const std::int64_t cost = 3'500'000;

    // Closed loop against an ideal vsync queue: Present blocks for whatever
    // part of the interval the delay and the frame cost did not use.
    Pacer pacer;
    std::int64_t delay{};
    std::int64_t block{};
    for (int frame = 0; frame < 200; ++frame)
    {
        block = std::max<std::int64_t>(0, interval - cost - delay);
        const std::int64_t next = pacer.update(block, interval);
        check(next >= 0 && next <= interval - Pacer::target_block_ns, "the delay stays bounded");
        check(next + cost <= interval, "the delay never pushes a steady frame past its slot");
        delay = next;
    }
    check(near(static_cast<double>(interval - cost - delay),
               static_cast<double>(Pacer::target_block_ns), 50'000.0),
          "the loop converges on the target block time");
    check(delay > 10'000'000, "at 60 Hz most of the idle interval becomes fresher input");

    // The frame cost jumps: Present stops blocking and the delay collapses.
    const std::int64_t before = delay;
    delay = pacer.update(0, interval);
    check(delay <= before - before / 4, "a late frame gives back at least a quarter immediately");
    int frames_to_recover{};
    while (delay > 0 && frames_to_recover < 64)
    {
        delay = pacer.update(0, interval);
        ++frames_to_recover;
    }
    check(delay == 0 && frames_to_recover <= 32, "sustained lateness removes the delay entirely");

    // A display that leaves no idle time gets no delay at all.
    Pacer fast;
    const std::int64_t fast_interval = 1'000'000'000LL / 240;
    for (int frame = 0; frame < 100; ++frame)
        delay = fast.update(std::max<std::int64_t>(0, fast_interval - cost - fast.delay_ns()),
                            fast_interval);
    check(delay == 0, "no slack means no inserted delay");

    // Unknown refresh interval or an invalid measurement disables the pacer.
    Pacer unknown;
    check(unknown.update(8'000'000, 0) == 0, "an unknown refresh interval disables pacing");
    check(pacer.update(-1, interval) == 0, "a negative block time is rejected");
    pacer.reset();
    check(pacer.delay_ns() == 0, "reset clears the delay");
}

void frame_time_summary_counts_hitches_and_buckets()
{
    std::vector<double> samples(98, 4.0);
    samples.push_back(9.0);
    samples.push_back(40.0);
    const auto summary = pacing::summarize_frame_times(samples);
    check(summary.frames == 100U && near(summary.median_ms, 4.0, 1e-9) &&
              near(summary.average_ms, 4.41, 1e-9),
          "average and median");
    check(near(summary.p95_ms, 4.0, 1e-9) && near(summary.p99_ms, 9.0, 1e-9) &&
              near(summary.maximum_ms, 40.0, 1e-9) && near(summary.minimum_ms, 4.0, 1e-9),
          "percentiles");
    check(summary.hitches == 2U, "frames above twice the median are hitches");
    check(summary.slow_frames == 1U, "frames above 16.7 ms are slow frames");
    check(summary.histogram[3] == 98U && summary.histogram[4] == 1U &&
              summary.histogram[6] == 1U,
          "histogram buckets are <1 <2 <4 <8 <16 <33 <66 >=66");
    check(summary.deviation_ms > 3.0 && summary.deviation_ms < 4.0, "standard deviation");
    check(pacing::summarize_frame_times({}).frames == 0U, "an empty run is all zeros");
}

void motion_summary_measures_judder()
{
    // 60 Hz steps shown at 240 Hz: three stalled frames, then a quadruple step.
    std::vector<pacing::MotionSample> stepped;
    std::vector<pacing::MotionSample> even;
    const double frame = 1.0 / 240.0;
    for (int index = 0; index < 960; ++index)
    {
        stepped.push_back({frame, index % 4 == 3 ? 5.0 / 60.0 : 0.0});
        even.push_back({frame, 5.0 * frame});
    }
    const auto judder = pacing::summarize_motion(stepped);
    check(near(judder.mean_speed, 5.0, 1e-9), "both runs travel at five units per second");
    check(near(judder.stalled_fraction, 0.75, 1e-9), "three of every four frames do not move");
    check(near(judder.speed_variation, std::sqrt(3.0), 1e-6),
          "60 Hz steps at 240 Hz vary by root three of the mean speed");
    check(near(judder.worst_step_error, 5.0 / 60.0 - 5.0 * frame, 1e-9),
          "the step frame overshoots uniform motion by three frames of travel");
    const auto smooth = pacing::summarize_motion(even);
    check(near(smooth.mean_speed, 5.0, 1e-9) && smooth.speed_variation < 1e-9 &&
              smooth.rms_step_error < 1e-12 && smooth.stalled_fraction == 0.0,
          "uniform motion scores zero on every judder measure");

    // Uneven frame times are not judder as long as distance tracks time.
    std::vector<pacing::MotionSample> uneven;
    for (int index = 0; index < 500; ++index)
    {
        const double seconds = index % 2 == 0 ? 0.003 : 0.009;
        uneven.push_back({seconds, 5.0 * seconds});
    }
    check(pacing::summarize_motion(uneven).speed_variation < 1e-9,
          "distance proportional to frame time is smooth");
    std::vector<pacing::MotionSample> still(100, pacing::MotionSample{frame, 0.0});
    const auto resting = pacing::summarize_motion(still);
    check(resting.mean_speed == 0.0 && resting.stalled_fraction == 0.0,
          "a camera at rest is not reported as stalled");
    check(pacing::summarize_motion({}).frames == 0U, "an empty run is all zeros");
}

void pacing_settings_default_on_and_round_trip()
{
    Settings defaults;
    check(defaults.motion_smoothing, "motion smoothing defaults on");
    check(defaults.low_latency == LowLatencyMode::on, "low latency defaults on");

    const auto path = std::filesystem::temp_directory_path() /
                      ("openminerz_frame_pacing_" +
                       std::to_string(static_cast<long long>(
                           std::hash<std::string>{}(std::filesystem::current_path().string()) &
                           0xFFFFFF)) +
                       ".cfg");
    const SettingsStore store{path};
    Settings saved;
    saved.motion_smoothing = false;
    saved.low_latency = LowLatencyMode::off;
    store.save(saved);
    Settings loaded = store.load();
    check(!loaded.motion_smoothing && loaded.low_latency == LowLatencyMode::off,
          "pacing settings survive a save and load");
    saved.motion_smoothing = true;
    saved.low_latency = LowLatencyMode::on;
    store.save(saved);
    loaded = store.load();
    check(loaded.motion_smoothing && loaded.low_latency == LowLatencyMode::on,
          "re-enabled pacing settings survive a save and load");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    check(SettingsStore{path}.load().motion_smoothing &&
              SettingsStore{path}.load().low_latency == LowLatencyMode::on,
          "a settings file written before these fields existed keeps the defaults");
    check(to_string(LowLatencyMode::off) == "off" && to_string(LowLatencyMode::on) == "on",
          "low latency modes have stable names");
}

} // namespace

int main()
{
    struct Test final
    {
        const char *name;
        void (*body)();
    };
    const Test tests[]{
        {"alpha_is_the_clamped_unconsumed_tick_fraction",
         alpha_is_the_clamped_unconsumed_tick_fraction},
        {"camera_blends_between_the_last_two_ticks", camera_blends_between_the_last_two_ticks},
        {"steady_walking_presents_evenly_at_any_refresh_rate",
         steady_walking_presents_evenly_at_any_refresh_rate},
        {"teleports_cut_instead_of_smearing", teleports_cut_instead_of_smearing},
        {"angle_interpolation_takes_the_short_way_round",
         angle_interpolation_takes_the_short_way_round},
        {"limiter_keeps_an_exact_cadence_without_drift_or_bursts",
         limiter_keeps_an_exact_cadence_without_drift_or_bursts},
        {"limiter_sleeps_most_of_the_wait_and_spins_the_rest",
         limiter_sleeps_most_of_the_wait_and_spins_the_rest},
        {"present_pacer_only_spends_slack_and_backs_off_at_once",
         present_pacer_only_spends_slack_and_backs_off_at_once},
        {"frame_time_summary_counts_hitches_and_buckets",
         frame_time_summary_counts_hitches_and_buckets},
        {"motion_summary_measures_judder", motion_summary_measures_judder},
        {"pacing_settings_default_on_and_round_trip", pacing_settings_default_on_and_round_trip},
    };
    std::size_t failures{};
    for (const Test &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    const std::size_t count = sizeof(tests) / sizeof(tests[0]);
    std::cout << count - failures << '/' << count << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
