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
    // The value retail's callers actually see. The shipped game is a 32-bit
    // process whose legacy JIT evaluates ComputeNoise on the x87 FPU at 53-bit
    // precision and returns the result on the FPU stack WITHOUT narrowing it to
    // float; the world builder then truncates or compares that unrounded value.
    // These reproduce the JIT's exact sequence of double-precision operations
    // and float stores (verified against the disassembled retail code and, block
    // for block, by TerrainParityProbe), so `(int)(noise * scale)` lands on the
    // same side of every integer boundary as it does in the original.
    [[nodiscard]] double noise_x87(float x, float y) const noexcept;
    // PerlinNoise.ComputeNoise(Vector3): unlike the 2D overload the JIT keeps
    // the three fractional coordinates and the last gradient on the FPU stack.
    [[nodiscard]] double noise_x87(float x, float y, float z) const noexcept;
    // The same values narrowed to float, as retail gets when it stores a result
    // into a float local that lives in memory.
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
