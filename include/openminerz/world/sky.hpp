#pragma once

#include <cstdint>
#include <string>

namespace openminerz::world
{

struct SkyColor final
{
    float red{};
    float green{};
    float blue{};
};

struct SkyState final
{
    float time_of_day{};
    float sun_x{};
    float sun_y{};
    float sun_z{};
    float daylight{};
    SkyColor zenith{};
    SkyColor horizon{};
    SkyColor fog{};
    SkyColor ambient{};
    SkyColor direct{};
    SkyColor specular{};
    std::string period;
    bool lightning{};
    float hell{};
    float lightning_strength{};
};

struct LightningUpdate final
{
    bool visible{};
    bool thunder_low{};
    bool thunder_high{};
};

// Classic keeps the shipped sun/light packs bit-for-bit. Rebirth retains the
// same 16-minute clock and named periods but uses a horizon-crossing solar path
// plus continuous modern lighting, so night no longer has a daytime sun vector.
enum class SkyModel
{
    classic,
    rebirth,
};

enum class SkyTexture
{
    night,
    dawn,
    day,
    sunset
};
struct SkyBlend final
{
    SkyTexture first{SkyTexture::night};
    SkyTexture second{SkyTexture::night};
    float amount{};
};
[[nodiscard]] SkyBlend rebirth_sky_blend(float time_of_day) noexcept;
[[nodiscard]] float lightning_envelope(float previous, bool flashing, float elapsed_seconds,
                                       float cloud_cover, bool exposed) noexcept;
void apply_lightning_illumination(SkyState &sky) noexcept;

// InGameHUD's recurring surface-lightning timer. It intentionally preserves
// the shipped one-frame dark gaps and the effectively unreachable high-thunder
// branch instead of smoothing the sequence into a modern weather effect.
class LightningCycle final
{
  public:
    explicit LightningCycle(std::uint64_t seed = 0xC45A71E2D39B5601ULL) noexcept;
    void reset(std::uint64_t seed) noexcept;
    [[nodiscard]] LightningUpdate update(float elapsed_seconds, int depth_under_ground,
                                         float player_y) noexcept;

  private:
    [[nodiscard]] std::uint64_t next() noexcept;
    [[nodiscard]] float next01() noexcept;
    [[nodiscard]] int next_int(int minimum, int maximum_exclusive) noexcept;
    void schedule_initial() noexcept;

    std::uint64_t rng_state_{};
    float time_to_lightning_{};
    float time_to_thunder_{};
    int flash_count_{};
    bool visible_{};
};

class DayNightCycle final
{
  public:
    static constexpr float minutes_per_day = 16.0F;

    explicit DayNightCycle(float day = 0.41F) noexcept;
    void update(float elapsed_seconds, float speed = 1.0F) noexcept;
    void set_time(float time_of_day) noexcept;
    [[nodiscard]] float day() const noexcept;
    [[nodiscard]] float time_of_day() const noexcept;
    [[nodiscard]] SkyState state(SkyModel model = SkyModel::classic) const;
    [[nodiscard]] std::string clock_text() const;

  private:
    float day_{};
};

} // namespace openminerz::world
