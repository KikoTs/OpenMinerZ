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

float fade(float value) noexcept
{
    return value * value * value * (value * (value * 6.0F - 15.0F) + 10.0F);
}

float lerp(float amount, float left, float right) noexcept
{
    return left + amount * (right - left);
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

float gradient2(int hash, float x, float y) noexcept
{
    const auto &gradient = gradients[static_cast<std::size_t>(hash & 7)];
    return x * static_cast<float>(gradient[0]) + y * static_cast<float>(gradient[1]);
}

float gradient3(int hash, float x, float y, float z) noexcept
{
    const auto &gradient = gradients[static_cast<std::size_t>(hash & 15)];
    return x * static_cast<float>(gradient[0]) + y * static_cast<float>(gradient[1]) +
           z * static_cast<float>(gradient[2]);
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

float PerlinNoise::noise(float x, float y) const noexcept
{
    const int floor_x = fast_floor(x);
    const int floor_y = fast_floor(y);
    const int cell_x = floor_x & 255;
    const int cell_y = floor_y & 255;
    x -= static_cast<float>(floor_x);
    y -= static_cast<float>(floor_y);
    const float u = fade(x);
    const float v = fade(y);
    const int a = permutation_[static_cast<std::size_t>(cell_x)] + cell_y;
    const int b = permutation_[static_cast<std::size_t>(cell_x + 1)] + cell_y;
    const float lower = lerp(u, gradient2(permutation_[static_cast<std::size_t>(a)], x, y),
                             gradient2(permutation_[static_cast<std::size_t>(b)], x - 1.0F, y));
    const float upper =
        lerp(u, gradient2(permutation_[static_cast<std::size_t>(a + 1)], x, y - 1.0F),
             gradient2(permutation_[static_cast<std::size_t>(b + 1)], x - 1.0F, y - 1.0F));
    return lerp(v, lower, upper);
}

float PerlinNoise::noise(float x, float y, float z) const noexcept
{
    const int floor_x = x > 0.0F ? static_cast<int>(x) : static_cast<int>(x) - 1;
    const int floor_y = y > 0.0F ? static_cast<int>(y) : static_cast<int>(y) - 1;
    const int floor_z = z > 0.0F ? static_cast<int>(z) : static_cast<int>(z) - 1;
    const int cell_x = floor_x & 255;
    const int cell_y = floor_y & 255;
    const int cell_z = floor_z & 255;
    x -= static_cast<float>(floor_x);
    y -= static_cast<float>(floor_y);
    z -= static_cast<float>(floor_z);
    const float u = fade(x);
    const float v = fade(y);
    const float w = fade(z);
    const int a = permutation_[static_cast<std::size_t>(cell_x)] + cell_y;
    const int aa = permutation_[static_cast<std::size_t>(a)] + cell_z;
    const int ab = permutation_[static_cast<std::size_t>(a + 1)] + cell_z;
    const int b = permutation_[static_cast<std::size_t>(cell_x + 1)] + cell_y;
    const int ba = permutation_[static_cast<std::size_t>(b)] + cell_z;
    const int bb = permutation_[static_cast<std::size_t>(b + 1)] + cell_z;
    const float x00 = lerp(u, gradient3(permutation_[static_cast<std::size_t>(aa)], x, y, z),
                           gradient3(permutation_[static_cast<std::size_t>(ba)], x - 1.0F, y, z));
    const float x10 =
        lerp(u, gradient3(permutation_[static_cast<std::size_t>(ab)], x, y - 1.0F, z),
             gradient3(permutation_[static_cast<std::size_t>(bb)], x - 1.0F, y - 1.0F, z));
    const float x01 =
        lerp(u, gradient3(permutation_[static_cast<std::size_t>(aa + 1)], x, y, z - 1.0F),
             gradient3(permutation_[static_cast<std::size_t>(ba + 1)], x - 1.0F, y, z - 1.0F));
    const float x11 = lerp(
        u, gradient3(permutation_[static_cast<std::size_t>(ab + 1)], x, y - 1.0F, z - 1.0F),
        gradient3(permutation_[static_cast<std::size_t>(bb + 1)], x - 1.0F, y - 1.0F, z - 1.0F));
    return lerp(w, lerp(v, x00, x10), lerp(v, x01, x11));
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
