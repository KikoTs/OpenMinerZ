#pragma once

#include <array>
#include <cstdint>

namespace openminerz::world
{

// Compatibility implementation of the .NET Framework Random used by the original game.
class LegacyRandom final
{
  public:
    explicit LegacyRandom(std::int32_t seed) noexcept;
    void reset(std::int32_t seed) noexcept;
    [[nodiscard]] std::int32_t next(std::int32_t maximum) noexcept;
    [[nodiscard]] std::int32_t next(std::int32_t minimum, std::int32_t maximum_exclusive) noexcept;
    [[nodiscard]] float next_float() noexcept;

  private:
    [[nodiscard]] std::int32_t internal_sample() noexcept;

    std::array<std::int32_t, 56> seed_array_{};
    std::int32_t inext_{};
    std::int32_t inextp_{21};
};

class PerlinNoise final
{
  public:
    explicit PerlinNoise(std::int32_t seed) noexcept;
    [[nodiscard]] float noise(float x, float y) const noexcept;
    [[nodiscard]] float noise(float x, float y, float z) const noexcept;

  private:
    std::array<std::int32_t, 1024> permutation_{};
};

class IntegerNoise final
{
  public:
    explicit IntegerNoise(std::int32_t seed) noexcept;
    [[nodiscard]] std::int32_t noise(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] std::int32_t noise(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept;

  private:
    std::array<std::int32_t, 1024> permutation_{};
};

} // namespace openminerz::world
