#include "openminerz/world/legacy_noise.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace openminerz::world
{
namespace
{

constexpr std::int32_t big = 2'147'483'647;
constexpr std::int32_t seed_magic = 161'803'398;

constexpr std::array<std::array<int, 4>, 32> gradients{{
    {{0, -1, -1, -1}}, {{-1, 0, -1, -1}}, {{1, 0, -1, -1}}, {{0, 1, -1, -1}}, {{-1, -1, 0, -1}},
    {{1, -1, 0, -1}},  {{-1, 1, 0, -1}},  {{1, 1, 0, -1}},  {{0, -1, 1, -1}}, {{-1, 0, 1, -1}},
    {{1, 0, 1, -1}},   {{0, 1, 1, -1}},   {{1, 1, 0, 1}},   {{-1, 1, 0, 1}},  {{0, -1, -1, 1}},
    {{0, -1, 1, 1}},   {{-1, -1, -1, 0}}, {{1, -1, -1, 0}}, {{-1, 1, -1, 0}}, {{1, 1, -1, 0}},
    {{-1, -1, 1, 0}},  {{1, -1, 1, 0}},   {{-1, 1, 1, 0}},  {{1, 1, 1, 0}},   {{-1, 0, -1, 1}},
    {{1, 0, -1, 1}},   {{0, 1, -1, 1}},   {{-1, -1, 0, 1}}, {{1, -1, 0, 1}},  {{-1, 0, 1, 1}},
    {{1, 0, 1, 1}},    {{0, 1, 1, 1}},
}};

int fast_floor(float value) noexcept
{
    return value <= 0.0F ? static_cast<int>(value) - 1 : static_cast<int>(value);
}

// Everything below reproduces what the legacy 32-bit JIT emits for
// DNA.Drawing.Noise.PerlinNoise: x87 arithmetic at 53-bit precision, narrowed
// to float only where the JIT stores a value to a 4-byte slot. `double` here is
// that 53-bit register; every static_cast<float> is one of those stores. The
// operation order is the JIT's, not the C# source's, wherever they differ.
// Contraction must stay off: a fused multiply-add rounds once where x87 rounds
// twice.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

// PerlinNoise.Fade, and the identical inlined copies in ComputeNoise(Vector3):
// ((t*t)*t) * ((t*((t*6)-15))+10), entirely on the FPU stack.
double fade_x87(double value) noexcept
{
    const double cube = (value * value) * value;
    const double inner = (value * ((value * 6.0) - 15.0)) + 10.0;
    return cube * inner;
}

// PerlinNoise.Lerp(t, a, b): a + ((b - a) * t), returned unrounded.
double lerp_x87(double amount, double left, double right) noexcept
{
    return left + ((right - left) * amount);
}

template <typename Table> void initialize_permutation(Table &table, std::int32_t seed) noexcept
{
    LegacyRandom random{seed};
    for (int index = 0; index < 256; ++index)
    {
        table[static_cast<std::size_t>(index)] = random.next(256);
        table[static_cast<std::size_t>(256 + index)] = table[static_cast<std::size_t>(index)];
    }
    for (int index = 0; index < 512; ++index)
    {
        table[static_cast<std::size_t>(512 + index)] = table[static_cast<std::size_t>(index)];
    }
}

// The first three components of the sixteen gradients the 2D and 3D noise can
// select, as the 53-bit values the FPU loads (-1, 0 and 1 are exact). Held as
// doubles only so the hot path does not convert an int on every use.
constexpr std::array<std::array<double, 3>, 16> gradient_components = [] {
    std::array<std::array<double, 3>, 16> table{};
    for (std::size_t index = 0; index < table.size(); ++index)
        for (std::size_t axis = 0; axis < 3; ++axis)
            table[index][axis] = static_cast<double>(gradients[index][axis]);
    return table;
}();

// PerlinNoise.Grad(hash, x, y): (g0 * x) + (g1 * y) on the FPU stack. Both
// arguments arrive as floats; the caller narrows the result.
double gradient2_x87(int hash, float x, float y) noexcept
{
    const auto &gradient = gradient_components[static_cast<std::size_t>(hash & 7)];
    return (gradient[0] * static_cast<double>(x)) + (gradient[1] * static_cast<double>(y));
}

// The inlined three-term gradient of ComputeNoise(Vector3):
// ((g0 * x) + (g1 * y)) + (g2 * z) with 53-bit operands.
double gradient3_x87(int hash, double x, double y, double z) noexcept
{
    const auto &gradient = gradient_components[static_cast<std::size_t>(hash & 15)];
    return ((gradient[0] * x) + (gradient[1] * y)) + (gradient[2] * z);
}

// The `(x > 0f) ? (int)x : (int)x - 1` written out in ComputeNoise(Vector3).
int floor_x87(float value) noexcept
{
    return value > 0.0F ? static_cast<int>(value) : static_cast<int>(value) - 1;
}

} // namespace

LegacyRandom::LegacyRandom(std::int32_t seed) noexcept
{
    reset(seed);
}

void LegacyRandom::reset(std::int32_t seed) noexcept
{
    seed_array_.fill(0);
    inext_ = 0;
    inextp_ = 21;
    const std::int32_t subtraction =
        seed == std::numeric_limits<std::int32_t>::min() ? big : (seed < 0 ? -seed : seed);
    std::int32_t mj = seed_magic - subtraction;
    if (mj < 0)
    {
        mj += big;
    }
    seed_array_[55] = mj;
    std::int32_t mk = 1;
    for (int index = 1; index < 55; ++index)
    {
        const int ii = (21 * index) % 55;
        seed_array_[static_cast<std::size_t>(ii)] = mk;
        mk = mj - mk;
        if (mk < 0)
        {
            mk += big;
        }
        mj = seed_array_[static_cast<std::size_t>(ii)];
    }
    for (int pass = 0; pass < 4; ++pass)
    {
        for (int index = 1; index < 56; ++index)
        {
            seed_array_[static_cast<std::size_t>(index)] -=
                seed_array_[static_cast<std::size_t>(1 + (index + 30) % 55)];
            if (seed_array_[static_cast<std::size_t>(index)] < 0)
            {
                seed_array_[static_cast<std::size_t>(index)] += big;
            }
        }
    }
}

std::int32_t LegacyRandom::internal_sample() noexcept
{
    if (++inext_ >= 56)
    {
        inext_ = 1;
    }
    if (++inextp_ >= 56)
    {
        inextp_ = 1;
    }
    std::int32_t result = seed_array_[static_cast<std::size_t>(inext_)] -
                          seed_array_[static_cast<std::size_t>(inextp_)];
    if (result == big)
    {
        --result;
    }
    if (result < 0)
    {
        result += big;
    }
    seed_array_[static_cast<std::size_t>(inext_)] = result;
    return result;
}

std::int32_t LegacyRandom::next(std::int32_t maximum) noexcept
{
    return static_cast<std::int32_t>(static_cast<double>(internal_sample()) *
                                     (1.0 / static_cast<double>(big)) * maximum);
}

std::int32_t LegacyRandom::next(std::int32_t minimum, std::int32_t maximum_exclusive) noexcept
{
    // System.Random.Next(min, max) still samples when min == max. Loot quantity
    // rolls depend on preserving that otherwise invisible RNG advance.
    return minimum + next(maximum_exclusive - minimum);
}

float LegacyRandom::next_float() noexcept
{
    // DNA.MathTools.RandomFloat converts Random.NextDouble to float before any
    // interpolation or magnitude multiplication.
    return static_cast<float>(static_cast<double>(internal_sample()) *
                              (1.0 / static_cast<double>(big)));
}

PerlinNoise::PerlinNoise(std::int32_t seed) noexcept
{
    initialize_permutation(permutation_, seed);
}

double PerlinNoise::noise_x87(float x, float y) const noexcept
{
    // PerlinNoise.ComputeNoise(float, float) as JIT-compiled in retail. x and y
    // are written back to their 4-byte argument slots after the floor is
    // removed, Fade and Grad results are stored to float locals, the first two
    // Lerps are narrowed, and the last Lerp is returned on the FPU stack.
    const int floor_x = fast_floor(x);
    const int floor_y = fast_floor(y);
    const int cell_x = floor_x & 255;
    const int cell_y = floor_y & 255;
    x = static_cast<float>(static_cast<double>(x) - static_cast<double>(static_cast<float>(floor_x)));
    y = static_cast<float>(static_cast<double>(y) - static_cast<double>(static_cast<float>(floor_y)));
    const float u = static_cast<float>(fade_x87(x));
    const float v = static_cast<float>(fade_x87(y));
    const int a = permutation_[static_cast<std::size_t>(cell_x)] + cell_y;
    const int b = permutation_[static_cast<std::size_t>(cell_x + 1)] + cell_y;
    const float x_minus_one = static_cast<float>(static_cast<double>(x) - 1.0);
    const float y_minus_one = static_cast<float>(static_cast<double>(y) - 1.0);
    const float g00 =
        static_cast<float>(gradient2_x87(permutation_[static_cast<std::size_t>(a)], x, y));
    const float g10 = static_cast<float>(
        gradient2_x87(permutation_[static_cast<std::size_t>(b)], x_minus_one, y));
    const float g01 = static_cast<float>(
        gradient2_x87(permutation_[static_cast<std::size_t>(a + 1)], x, y_minus_one));
    const float g11 = static_cast<float>(
        gradient2_x87(permutation_[static_cast<std::size_t>(b + 1)], x_minus_one, y_minus_one));
    const float lower = static_cast<float>(lerp_x87(u, g00, g10));
    const float upper = static_cast<float>(lerp_x87(u, g01, g11));
    return lerp_x87(v, lower, upper);
}

float PerlinNoise::noise(float x, float y) const noexcept
{
    return static_cast<float>(noise_x87(x, y));
}

double PerlinNoise::noise_x87(float x, float y, float z) const noexcept
{
    // PerlinNoise.ComputeNoise(Vector3) as JIT-compiled in retail. Here the JIT
    // enregisters x, y and z on the FPU stack, so the fractional coordinates
    // and the three `- 1` terms stay at 53 bits (they are NOT floats whenever
    // the coordinate is negative). The fades and the first seven gradients are
    // stored to float locals; the eighth gradient and every interpolation stay
    // on the stack, as does the result.
    const int floor_x = floor_x87(x);
    const int floor_y = floor_x87(y);
    const int floor_z = floor_x87(z);
    const int cell_x = floor_x & 255;
    const int cell_y = floor_y & 255;
    const int cell_z = floor_z & 255;
    const double fx = static_cast<double>(x) - static_cast<double>(static_cast<float>(floor_x));
    const double fy = static_cast<double>(y) - static_cast<double>(static_cast<float>(floor_y));
    const double fz = static_cast<double>(z) - static_cast<double>(static_cast<float>(floor_z));
    const float u = static_cast<float>(fade_x87(fx));
    const float v = static_cast<float>(fade_x87(fy));
    const float w = static_cast<float>(fade_x87(fz));
    const int a = permutation_[static_cast<std::size_t>(cell_x)] + cell_y;
    const int aa = permutation_[static_cast<std::size_t>(a)] + cell_z;
    const int ab = permutation_[static_cast<std::size_t>(a + 1)] + cell_z;
    const int b = permutation_[static_cast<std::size_t>(cell_x + 1)] + cell_y;
    const int ba = permutation_[static_cast<std::size_t>(b)] + cell_z;
    const int bb = permutation_[static_cast<std::size_t>(b + 1)] + cell_z;
    const double fx1 = fx - 1.0;
    const double fy1 = fy - 1.0;
    const double fz1 = fz - 1.0;
    const float g000 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(aa)], fx, fy, fz));
    const float g100 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(ba)], fx1, fy, fz));
    const float g010 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(ab)], fx, fy1, fz));
    const float g110 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(bb)], fx1, fy1, fz));
    const float g001 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(aa + 1)], fx, fy, fz1));
    const float g101 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(ba + 1)], fx1, fy, fz1));
    const float g011 = static_cast<float>(
        gradient3_x87(permutation_[static_cast<std::size_t>(ab + 1)], fx, fy1, fz1));
    // Left on the FPU stack by the JIT: never narrowed.
    const double g111 =
        gradient3_x87(permutation_[static_cast<std::size_t>(bb + 1)], fx1, fy1, fz1);
    const double x00 = lerp_x87(u, g000, g100);
    const double x10 = lerp_x87(u, g010, g110);
    const double x01 = lerp_x87(u, g001, g101);
    const double x11 = lerp_x87(u, g011, g111);
    const double y0 = lerp_x87(v, x00, x10);
    const double y1 = lerp_x87(v, x01, x11);
    return lerp_x87(w, y0, y1);
}

float PerlinNoise::noise(float x, float y, float z) const noexcept
{
    return static_cast<float>(noise_x87(x, y, z));
}

IntegerNoise::IntegerNoise(std::int32_t seed) noexcept
{
    initialize_permutation(permutation_, seed);
}

std::int32_t IntegerNoise::noise(std::int32_t x, std::int32_t y) const noexcept
{
    const int a = permutation_[static_cast<std::size_t>(x & 255)] + (y & 255);
    return permutation_[static_cast<std::size_t>(a)];
}

std::int32_t IntegerNoise::noise(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
{
    const int a = permutation_[static_cast<std::size_t>(x & 255)] + (y & 255);
    const int b = permutation_[static_cast<std::size_t>(a)] + (z & 255);
    return permutation_[static_cast<std::size_t>(b)];
}

} // namespace openminerz::world
