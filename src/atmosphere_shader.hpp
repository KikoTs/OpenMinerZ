#pragma once

// The Enhanced presentation's one atmosphere, as HLSL.
//
// PRESENTATION ONLY. One density field over the world and one function that
// integrates it along a view ray in closed form; every pixel of a frame is
// fogged by it and by nothing else above ground:
//
//   * the far-terrain composite (far_terrain_renderer.cpp, pass 2b) applies it
//     to everything the scene holds - detailed terrain, entities, the far
//     landscape and the sky down to the horizon - from each pixel's own depth;
//   * the light-shaft pass (post_processing.cpp) marches the SAME field and
//     takes away the sunlight that terrain shadows keep out of it.
//
// The field (openminerz/world/surface_atmosphere.hpp is the tested copy of the
// arithmetic) is a sum of layers, each full strength at and below its floor
// and thinning exponentially above it:
//
//   density(p) = air(p.y) + first_biome(p.xz) * (mist(p.y) + haze(p.y))
//
// `first_biome` is 1 across the plains round the world's origin and falls
// smoothly to 0 by the next band, so the parameters change with the biome a
// ray passes through and never with which renderer drew the pixel. A density
// that is never negative gives a transmittance that can only fall along a
// ray: nothing is ever clearer than what stands in front of it.
//
// The far-terrain renderer owns and fills the constant buffer (register b8)
// and leaves it bound for the frame; the Scene cbuffer (register b0) must be
// declared before this text. All zero - no atmosphere - in the
// Original presentation and deep underground.

namespace openminerz
{

inline constexpr unsigned atmosphere_constant_slot = 8U;
// The far landscape's reversed depth, for the light-shaft pass.
inline constexpr unsigned atmosphere_far_depth_slot = 24U;

inline constexpr char atmosphere_hlsl[] = R"hlsl(
cbuffer Atmosphere : register(b8)
{
    // x = density at and below the floor, y = floor (world y), z = 1 / scale
    // height. AtmosAir.w = strength of the whole atmosphere (0 none .. 1),
    // AtmosMist.w = the eye's sky exposure, AtmosHaze.w = debug view.
    float4 AtmosAir;
    float4 AtmosMist;
    float4 AtmosHaze;
    // The first biome as discs round the world's origin: x = |eye|^2 -
    // radius^2, y = weight, z = radius^2.
    float4 AtmosDiscs[4];
    // xy = the eye's x, z about the world's origin, z = the eye's world y,
    // w = the far pass's near plane (0 = no far landscape this frame).
    float4 AtmosEye;
    // rgb = forward-scattered sunlight, w = how low the sun is (0 high .. 1).
    float4 AtmosSun;
    // x, y, z = the end of sight without a far landscape (atmosphere_edge);
    // w = share of the air's light that is sunlight
    // (what a shadow takes out of it).
    float4 AtmosSunDirection;
};

// Integral over a straight path of exp(-max(y - floor, 0) * inv_height): the
// length of the path as a layer weighs it. `y0` is the height where the path
// starts and `dy` how much it climbs per block.
float atmos_layer(float y0, float dy, float len, float floor_y, float inv_height)
{
    if (len <= 0.0f)
        return 0.0f;
    float a = (y0 - floor_y) * inv_height;
    float b = dy * inv_height;
    if (abs(b) < 1.0e-5f)
        return len * exp(-max(a, 0.0f));
    float crossing = clamp(-a / b, 0.0f, len);
    float above_start = b > 0.0f ? crossing : 0.0f;
    float above_end = b > 0.0f ? len : crossing;
    float result = len - (above_end - above_start);
    if (above_end > above_start)
        result += (exp(-max(a + b * above_start, 0.0f)) - exp(-max(a + b * above_end, 0.0f))) / b;
    return result;
}

// The end of sight when there is no far landscape (AtmosSunDirection.x is
// then the optical depth reached at the edge of the detailed window, .y one
// over that distance, .z how far above the eye the closing air reaches). The
// air closes in with the fifth power of the horizontal distance: next to
// nothing over the first half of the view, everything at its end. It only
// ever grows along a ray, and a ray that climbs out of it stops gathering it,
// which is what leaves the sky overhead a sky.
float atmosphere_edge(float3 direction, float len)
{
    if (AtmosSunDirection.x <= 0.0f)
        return 0.0f;
    float level = length(direction.xz);
    float reached = len * level;
    if (direction.y > 0.0f)
        reached = min(reached, AtmosSunDirection.z * level / direction.y);
    float share = reached * AtmosSunDirection.y;
    float squared = share * share;
    return AtmosSunDirection.x * squared * squared * share;
}

// Optical depths along `len` blocks from the eye: x through the clear air, y
// through the first biome's mist. `exposure` is 0 for a path underground.
float2 atmosphere_path(float3 direction, float len, float exposure)
{
    float strength = AtmosAir.w * exposure;
    if (strength <= 0.0f || len <= 0.0f)
        return float2(0.0f, 0.0f);
    float eye_y = AtmosEye.z;
    float air = AtmosAir.x * atmos_layer(eye_y, direction.y, len, AtmosAir.y, AtmosAir.z);
    float mist = 0.0f;
    [branch]
    if (AtmosMist.x + AtmosHaze.x > 0.0f)
    {
        float a = dot(direction.xz, direction.xz);
        float b = dot(AtmosEye.xy, direction.xz);
        [unroll]
        for (int index = 0; index < 4; ++index)
        {
            float weight = AtmosDiscs[index].y;
            float c = AtmosDiscs[index].x;
            float start = 0.0f;
            float end = len;
            bool inside = c < 0.0f;
            if (a >= 1.0e-8f)
            {
                float discriminant = b * b - a * c;
                inside = discriminant > 0.0f;
                float root = sqrt(max(discriminant, 0.0f));
                start = max((-b - root) / a, 0.0f);
                end = min((-b + root) / a, len);
            }
            if (inside && end > start && weight != 0.0f)
            {
                float height = eye_y + direction.y * start;
                mist += weight *
                        (AtmosMist.x * atmos_layer(height, direction.y, end - start, AtmosMist.y,
                                                   AtmosMist.z) +
                         AtmosHaze.x * atmos_layer(height, direction.y, end - start, AtmosHaze.y,
                                                   AtmosHaze.z));
            }
        }
    }
    return float2(air + atmosphere_edge(direction, len), max(mist, 0.0f)) * strength;
}

// The same field at one point `offset` from the eye (for a march): x the
// clear air's density there, y the mist's.
float2 atmosphere_density(float3 offset, float exposure)
{
    float strength = AtmosAir.w * exposure;
    float y = AtmosEye.z + offset.y;
    float air = AtmosAir.x * exp(-max(y - AtmosAir.y, 0.0f) * AtmosAir.z);
    // (What atmosphere_edge gathers per block along a ray through here.)
    if (AtmosSunDirection.x > 0.0f && offset.y < AtmosSunDirection.z)
    {
        float reached = length(offset.xz);
        float share = reached * AtmosSunDirection.y;
        float squared = share * share;
        air += AtmosSunDirection.x * 5.0f * squared * squared * AtmosSunDirection.y * reached /
               max(length(offset), 0.001f);
    }
    float2 xz = AtmosEye.xy + offset.xz;
    float radius_squared = dot(xz, xz);
    float weight = 0.0f;
    [unroll]
    for (int index = 0; index < 4; ++index)
        weight += radius_squared < AtmosDiscs[index].z ? AtmosDiscs[index].y : 0.0f;
    float mist = max(weight, 0.0f) *
                 (AtmosMist.x * exp(-max(y - AtmosMist.y, 0.0f) * AtmosMist.z) +
                  AtmosHaze.x * exp(-max(y - AtmosHaze.y, 0.0f) * AtmosHaze.z));
    return float2(air, mist) * strength;
}

// Sunlight scattered forward through the air (AtmosSun.rgb is the sunlight's
// own colour and strength): the Henyey-Greenstein phase function for g = 0.66,
// less what it sends straight back, which the sky's own colour already
// carries. Bright and warm toward a low sun, falling off round it, nothing
// looking away.
float3 atmosphere_glow(float3 direction)
{
    float cos_sun = dot(direction, normalize(SunAndOptions.xyz));
    float phase = 0.5644f / pow(max(1.4356f - 1.32f * cos_sun, 0.0001f), 1.5f);
    return AtmosSun.rgb * (min(phase, 7.0f) - 0.1234f);
}

// The light the air sends toward the eye from `direction`, for a path whose
// optical depth is `mist_share` mist. `ambient` is the light of the sky as the
// scene's fog colour carries it (FogAndEnd.rgb). `sunlit` is the part of it a
// terrain shadow removes.
float3 atmosphere_light(float3 direction, float mist_share, float3 ambient, out float3 sunlit)
{
    // Mist is water: whiter than the sky that lights it.
    float grey = dot(ambient, float3(0.2126f, 0.7152f, 0.0722f));
    float3 air = lerp(ambient, lerp(ambient, grey.xxx * 1.06f, 0.55f), mist_share);
    // With a low sun the air opposite it is in the earth's own shade.
    float away = saturate(-dot(direction, normalize(SunAndOptions.xyz)));
    air *= 1.0f - 0.20f * AtmosSun.w * away;
    float3 glow = atmosphere_glow(direction);
    sunlit = air * AtmosSunDirection.w + glow;
    return air + glow;
}

// Light surviving `len` blocks along unit `direction` from the eye, and the
// light the air adds over that distance.
void atmosphere(float3 direction, float len, float exposure, float3 ambient,
                out float transmittance, out float3 inscatter)
{
    float2 path = atmosphere_path(direction, len, exposure);
    float total = path.x + path.y;
    transmittance = exp(-total);
    float3 sunlit;
    float3 light = atmosphere_light(direction, path.y / max(total, 1.0e-6f), ambient, sunlit);
    inscatter = light * (1.0f - transmittance);
}

// How much of a path counts as open air: all of it unless both ends are
// underground (`eye_sky` and `surface_sky` are baked skylight, 0 .. 1).
float atmosphere_exposure(float eye_sky, float surface_sky)
{
    return max(eye_sky, smoothstep(0.0f, 0.25f, surface_sky));
}
)hlsl";

} // namespace openminerz
