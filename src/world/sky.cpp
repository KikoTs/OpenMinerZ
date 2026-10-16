#include "openminerz/world/sky.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::world
{
namespace
{

constexpr float pi = 3.14159265358979323846F;

float wrap01(float value) noexcept
{
    value -= std::floor(value);
    return value < 0.0F ? value + 1.0F : value;
}

SkyColor color(int red, int green, int blue) noexcept
{
    return {static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F,
            static_cast<float>(blue) / 255.0F};
}

SkyColor mix(SkyColor left, SkyColor right, float amount) noexcept
{
    amount = std::clamp(amount, 0.0F, 1.0F);
    return {left.red + (right.red - left.red) * amount,
            left.green + (right.green - left.green) * amount,
            left.blue + (right.blue - left.blue) * amount};
}

float smooth_step(float edge0, float edge1, float value) noexcept
{
    const float amount = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return amount * amount * (3.0F - 2.0F * amount);
}

SkyColor scale(SkyColor value, float amount) noexcept
{
    return {value.red * amount, value.green * amount, value.blue * amount};
}

struct LightPack final
{
    SkyColor fog;
    SkyColor ambient;
    SkyColor direct;
    SkyColor specular;
    SkyColor zenith;
    SkyColor horizon;
    const char *name;
};

constexpr LightPack night{{10.0F / 255.0F, 10.0F / 255.0F, 10.0F / 255.0F},
                          {50.0F / 255.0F, 62.0F / 255.0F, 107.0F / 255.0F},
                          {75.0F / 255.0F, 93.0F / 255.0F, 160.0F / 255.0F},
                          {100.0F / 255.0F, 124.0F / 255.0F, 214.0F / 255.0F},
                          {5.0F / 255.0F, 9.0F / 255.0F, 24.0F / 255.0F},
                          {20.0F / 255.0F, 27.0F / 255.0F, 46.0F / 255.0F},
                          "NIGHT"};
constexpr LightPack dawn{{94.0F / 255.0F, 112.0F / 255.0F, 129.0F / 255.0F},
                         {183.0F / 255.0F, 134.0F / 255.0F, 107.0F / 255.0F},
                         {254.0F / 255.0F, 215.0F / 255.0F, 158.0F / 255.0F},
                         {254.0F / 255.0F, 215.0F / 255.0F, 158.0F / 255.0F},
                         {48.0F / 255.0F, 65.0F / 255.0F, 92.0F / 255.0F},
                         {218.0F / 255.0F, 123.0F / 255.0F, 76.0F / 255.0F},
                         "DAWN"};
constexpr LightPack day{{74.0F / 255.0F, 78.0F / 255.0F, 74.0F / 255.0F},
                        {0.4F, 0.4F, 0.4F},
                        {1.0F, 1.0F, 1.0F},
                        {1.0F, 1.0F, 1.0F},
                        {54.0F / 255.0F, 96.0F / 255.0F, 137.0F / 255.0F},
                        {157.0F / 255.0F, 180.0F / 255.0F, 192.0F / 255.0F},
                        "DAY"};
constexpr LightPack dusk{{145.0F / 255.0F, 100.0F / 255.0F, 57.0F / 255.0F},
                         {0.4F, 0.4F, 0.3F},
                         {254.0F / 255.0F, 215.0F / 255.0F, 158.0F / 255.0F},
                         {254.0F / 255.0F, 215.0F / 255.0F, 158.0F / 255.0F},
                         {45.0F / 255.0F, 42.0F / 255.0F, 67.0F / 255.0F},
                         {223.0F / 255.0F, 91.0F / 255.0F, 45.0F / 255.0F},
                         "SUNSET"};

LightPack blend_pack(const LightPack &left, const LightPack &right, float amount,
                     const char *name) noexcept
{
    return {mix(left.fog, right.fog, amount),
            mix(left.ambient, right.ambient, amount),
            mix(left.direct, right.direct, amount),
            mix(left.specular, right.specular, amount),
            mix(left.zenith, right.zenith, amount),
            mix(left.horizon, right.horizon, amount),
            name};
}

LightPack lighting(float time) noexcept
{
    const float hours = time * 24.0F;
    const int hour = static_cast<int>(hours);
    const float blend = hours - static_cast<float>(hour);
    if (hour <= 5 || hour >= 21)
    {
        return night;
    }
    if (hour == 6)
    {
        return blend_pack(night, dawn, blend, "DAWN");
    }
    if (hour == 7)
    {
        return dawn;
    }
    if (hour == 8)
    {
        return blend_pack(dawn, day, blend, "MORNING");
    }
    if (hour <= 17)
    {
        return day;
    }
    if (hour == 18)
    {
        return blend_pack(day, dusk, blend, "GOLDEN HOUR");
    }
    if (hour == 19)
    {
        return dusk;
    }
    return blend_pack(dusk, night, blend, "DUSK");
}

SkyState rebirth_lighting(float time) noexcept
{
    // A fixed 68-degree maximum elevation gives the blocks useful directional
    // relief at noon while still crossing the horizon at 06:00 and 18:00.
    // The tilted great-circle path keeps the direction normalized and makes
    // sunrise/sunset shadows rotate continuously instead of flipping between
    // the original always-positive arcs.
    constexpr float maximum_elevation = 68.0F * pi / 180.0F;
    const float solar_phase = (time - 0.25F) * 2.0F * pi;
    const float sine_phase = std::sin(solar_phase);
    const float cosine_phase = std::cos(solar_phase);
    const float sun_x = cosine_phase;
    const float sun_y = sine_phase * std::sin(maximum_elevation);
    const float sun_z = -sine_phase * std::cos(maximum_elevation);

    // Civil twilight begins while the sun is just below the horizon. Direct
    // illumination, unlike indirect sky light, reaches exactly zero at night.
    const float daylight = smooth_step(-0.18F, 0.16F, sun_y);
    const float direct_amount = smooth_step(-0.005F, 0.14F, sun_y);
    const float twilight =
        smooth_step(-0.22F, -0.015F, sun_y) * (1.0F - smooth_step(0.02F, 0.42F, std::abs(sun_y)));
    const bool morning = time < 0.5F;

    constexpr SkyColor night_zenith{0.008F, 0.016F, 0.050F};
    constexpr SkyColor night_horizon{0.030F, 0.050F, 0.095F};
    constexpr SkyColor night_fog{0.016F, 0.023F, 0.040F};
    constexpr SkyColor night_ambient{0.080F, 0.105F, 0.180F};
    constexpr SkyColor day_zenith{0.120F, 0.310F, 0.580F};
    constexpr SkyColor day_horizon{0.560F, 0.720F, 0.820F};
    constexpr SkyColor day_fog{0.290F, 0.340F, 0.360F};
    constexpr SkyColor day_ambient{0.380F, 0.400F, 0.420F};
    constexpr SkyColor dawn_zenith{0.110F, 0.160F, 0.300F};
    constexpr SkyColor dawn_horizon{0.740F, 0.500F, 0.360F};
    constexpr SkyColor dawn_fog{0.340F, 0.300F, 0.270F};
    constexpr SkyColor dawn_ambient{0.260F, 0.210F, 0.190F};
    constexpr SkyColor dusk_zenith{0.100F, 0.090F, 0.180F};
    constexpr SkyColor dusk_horizon{0.660F, 0.360F, 0.260F};
    constexpr SkyColor dusk_fog{0.320F, 0.250F, 0.240F};
    constexpr SkyColor dusk_ambient{0.270F, 0.190F, 0.150F};

    const SkyColor twilight_zenith = morning ? dawn_zenith : dusk_zenith;
    const SkyColor twilight_horizon = morning ? dawn_horizon : dusk_horizon;
    const SkyColor twilight_fog = morning ? dawn_fog : dusk_fog;
    const SkyColor twilight_ambient = morning ? dawn_ambient : dusk_ambient;
    SkyColor zenith = mix(night_zenith, day_zenith, daylight);
    SkyColor horizon = mix(night_horizon, day_horizon, daylight);
    SkyColor fog = mix(night_fog, day_fog, daylight);
    SkyColor ambient = mix(night_ambient, day_ambient, daylight);
    zenith = mix(zenith, twilight_zenith, twilight * 0.72F);
    horizon = mix(horizon, twilight_horizon, twilight * 0.88F);
    fog = mix(fog, twilight_fog, twilight * 0.70F);
    // The sun is already below the horizon during civil twilight, but the sky
    // dome still illuminates exposed terrain.  Keep that indirect component
    // strong enough for voxel faces to remain readable; direct and specular
    // light below stay at zero, so this cannot create night-time cast shadows.
    ambient = mix(ambient, twilight_ambient, twilight * 0.68F);

    const SkyColor low_sun = morning ? SkyColor{1.0F, 0.62F, 0.34F} : SkyColor{1.0F, 0.46F, 0.20F};
    constexpr SkyColor high_sun{1.0F, 0.98F, 0.92F};
    const float neutral_sun = smooth_step(0.03F, 0.58F, sun_y);
    const SkyColor direct = scale(mix(low_sun, high_sun, neutral_sun), direct_amount);
    const SkyColor specular = scale(direct, 0.92F);

    const char *period = sun_y < -0.18F  ? "NIGHT"
                         : sun_y < 0.14F ? (morning ? "DAWN" : "DUSK")
                         : sun_y < 0.42F ? (morning ? "MORNING" : "SUNSET")
                                         : "DAY";
    return {time, sun_x,   sun_y,  sun_z,    daylight, zenith, horizon,
            fog,  ambient, direct, specular, period,   false};
}

} // namespace

SkyBlend rebirth_sky_blend(float time_of_day) noexcept
{
    const float time = wrap01(time_of_day);
    const float elevation = std::sin((time - 0.25F) * 2.0F * pi) * std::sin(68.0F * pi / 180.0F);
    const auto twilight = time < 0.5F ? SkyTexture::dawn : SkyTexture::sunset;
    if (elevation < 0.02F)
        return {SkyTexture::night, twilight, smooth_step(-0.18F, 0.02F, elevation)};
    return {twilight, SkyTexture::day, smooth_step(0.02F, 0.42F, elevation)};
}

DayNightCycle::DayNightCycle(float day) noexcept : day_(day)
{
}

float lightning_envelope(float previous, bool flashing, float elapsed_seconds, float cloud_cover,
                         bool exposed) noexcept
{
    if (!exposed)
        return 0.0F;
    const float target = flashing ? std::clamp(cloud_cover, 0.0F, 1.0F) : 0.0F;
    const float response =
        1.0F - std::exp(-std::max(0.0F, elapsed_seconds) / (flashing ? 0.035F : 0.18F));
    return std::lerp(std::clamp(previous, 0.0F, 1.0F), target, response);
}

void apply_lightning_illumination(SkyState &sky) noexcept
{
    const float flash = std::clamp(sky.lightning_strength, 0.0F, 1.0F) * (1.0F - sky.hell);
    sky.ambient.red += 0.42F * flash;
    sky.ambient.green += 0.50F * flash;
    sky.ambient.blue += 0.66F * flash;
    sky.horizon = mix(sky.horizon, {0.70F, 0.76F, 0.88F}, flash * 0.35F);
    sky.fog = mix(sky.fog, {0.52F, 0.59F, 0.72F}, flash * 0.22F);
}

void DayNightCycle::update(float elapsed_seconds, float speed) noexcept
{
    day_ += elapsed_seconds * speed / (minutes_per_day * 60.0F);
}

void DayNightCycle::set_time(float time_of_day) noexcept
{
    day_ = std::floor(day_) + wrap01(time_of_day);
}

float DayNightCycle::day() const noexcept
{
    return day_;
}

float DayNightCycle::time_of_day() const noexcept
{
    return wrap01(day_);
}

SkyState DayNightCycle::state(SkyModel model) const
{
    const float time = time_of_day();
    const auto pack = lighting(time);
    if (model == SkyModel::rebirth)
        return rebirth_lighting(time);
    float phase = wrap01(time + 1.625F) * 2.0F * pi;
    const float sun_y = 0.2F + 0.8F * std::abs(std::sin(phase));
    const float radial = std::sqrt(std::max(0.0F, 1.0F - sun_y * sun_y));
    phase -= 0.236F;
    const float distance_to_noon = std::abs(time - 0.5F) * 2.0F;
    return {time,
            -std::sin(phase) * radial,
            sun_y,
            std::cos(phase) * radial,
            std::clamp(1.0F - distance_to_noon * 0.72F, 0.18F, 1.0F),
            pack.zenith,
            pack.horizon,
            pack.fog,
            pack.ambient,
            pack.direct,
            pack.specular,
            pack.name,
            false};
}

std::string DayNightCycle::clock_text() const
{
    const float hours = time_of_day() * 24.0F;
    const int hour24 = static_cast<int>(hours) % 24;
    const int minute = (static_cast<int>((hours - static_cast<float>(hour24)) * 60.0F) / 15) * 15;
    int hour12 = hour24 % 12;
    if (hour12 == 0)
    {
        hour12 = 12;
    }
    return std::format("{}:{:02} {}", hour12, minute, hour24 < 12 ? "AM" : "PM");
}

LightningCycle::LightningCycle(std::uint64_t seed) noexcept
{
    reset(seed);
}

void LightningCycle::reset(std::uint64_t seed) noexcept
{
    rng_state_ = seed != 0U ? seed : 0xC45A71E2D39B5601ULL;
    visible_ = false;
    schedule_initial();
}

std::uint64_t LightningCycle::next() noexcept
{
    rng_state_ ^= rng_state_ << 13U;
    rng_state_ ^= rng_state_ >> 7U;
    rng_state_ ^= rng_state_ << 17U;
    return rng_state_;
}

float LightningCycle::next01() noexcept
{
    return static_cast<float>(next() >> 40U) / 16777216.0F;
}

int LightningCycle::next_int(int minimum, int maximum_exclusive) noexcept
{
    if (maximum_exclusive <= minimum)
        return minimum;
    return minimum +
           static_cast<int>(next() % static_cast<std::uint64_t>(maximum_exclusive - minimum));
}

void LightningCycle::schedule_initial() noexcept
{
    time_to_lightning_ = static_cast<float>(next_int(5, 10));
    time_to_thunder_ = next01() * 2.0F;
    flash_count_ = next_int(0, 4);
}

LightningUpdate LightningCycle::update(float elapsed_seconds, int depth_under_ground,
                                       float player_y) noexcept
{
    const float delta = std::max(0.0F, elapsed_seconds);
    if (flash_count_ <= 0 || depth_under_ground > 2 || player_y <= -32.0F)
        visible_ = false;

    bool thunder_low{};
    bool thunder_high{};
    if (time_to_lightning_ <= 0.0F)
    {
        if (flash_count_ > 0 && !visible_)
        {
            visible_ = true;
            --flash_count_;
            time_to_lightning_ = 0.1F + next01() * 0.25F;
        }
        else if (flash_count_ > 0 && visible_)
        {
            // No timer reset: this is the original one-update dark gap.
            visible_ = false;
        }
        else if (time_to_thunder_ <= 0.0F)
        {
            // flash_count_ must be zero here, making thunderHigh unreachable
            // in the shipped state machine; keep the branch for exactness.
            thunder_low = flash_count_ < 3;
            thunder_high = !thunder_low;
            time_to_thunder_ = next01() * 2.0F;
            time_to_lightning_ = static_cast<float>(next_int(10, 40));
            flash_count_ = next_int(0, 4);
        }
        else
        {
            time_to_thunder_ -= delta;
        }
    }
    else if (depth_under_ground <= 2 && player_y > -32.0F)
    {
        time_to_lightning_ -= delta;
        visible_ = false;
    }
    return {visible_, thunder_low, thunder_high};
}

} // namespace openminerz::world
