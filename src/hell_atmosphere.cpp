#include "hell_atmosphere.hpp"

#include "openminerz/world/hell_atmosphere.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>

#if defined(_WIN32)
#include <SDL3/SDL.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "shader_cache.hpp"
#endif

namespace openminerz
{

std::vector<world::LavaColumn> scan_lava_columns(const std::vector<world::WorldFace> &faces)
{
    std::vector<world::LavaColumn> columns;
    for (const world::WorldFace &face : faces)
        if (face.block == world::BlockType::surface_lava ||
            face.block == world::BlockType::deep_lava)
            columns.push_back({face.x, face.z, static_cast<std::int16_t>(face.y + 1)});
    std::sort(columns.begin(), columns.end(),
              [](const world::LavaColumn &left, const world::LavaColumn &right) {
                  if (left.x != right.x)
                      return left.x < right.x;
                  if (left.z != right.z)
                      return left.z < right.z;
                  return left.top > right.top;
              });
    columns.erase(std::unique(columns.begin(), columns.end(),
                              [](const world::LavaColumn &left, const world::LavaColumn &right) {
                                  return left.x == right.x && left.z == right.z;
                              }),
                  columns.end());
    return columns;
}

#if defined(_WIN32)
namespace
{
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Tuning. Every number the shaders use is written into their text at load from
// the constants in openminerz/world/hell_atmosphere.hpp and the table below.
// OPENMINERZ_HELL_TUNE="name=value,name=r/g/b" replaces any of them for one
// run (diagnostic only; it is how the look was judged from captures).
// ---------------------------------------------------------------------------
struct Tuning final
{
    std::map<std::string, std::array<float, 3>, std::less<>> overrides;

    Tuning()
    {
        const char *text = SDL_getenv("OPENMINERZ_HELL_TUNE");
        if (text == nullptr)
            return;
        std::string_view remaining{text};
        while (!remaining.empty())
        {
            const std::size_t comma = remaining.find(',');
            const std::string_view entry = remaining.substr(0U, comma);
            remaining = comma == std::string_view::npos ? std::string_view{}
                                                        : remaining.substr(comma + 1U);
            const std::size_t equals = entry.find('=');
            if (equals == std::string_view::npos)
                continue;
            std::array<float, 3> value{};
            std::string_view numbers = entry.substr(equals + 1U);
            std::size_t count{};
            while (count < 3U && !numbers.empty())
            {
                const std::size_t slash = numbers.find('/');
                value[count++] = static_cast<float>(
                    std::atof(std::string{numbers.substr(0U, slash)}.c_str()));
                numbers = slash == std::string_view::npos ? std::string_view{}
                                                          : numbers.substr(slash + 1U);
            }
            if (count == 1U)
                value[1] = value[2] = value[0];
            overrides.insert_or_assign(std::string{entry.substr(0U, equals)}, value);
        }
    }

    [[nodiscard]] float scalar(std::string_view name, float fallback) const
    {
        const auto found = overrides.find(name);
        return found == overrides.end() ? fallback : found->second[0];
    }

    [[nodiscard]] std::array<float, 3> color(std::string_view name,
                                             std::array<float, 3> fallback) const
    {
        const auto found = overrides.find(name);
        return found == overrides.end() ? fallback : found->second;
    }
};

const Tuning &tuning()
{
    static const Tuning instance;
    return instance;
}

void append_scalar(std::string &text, std::string_view name, float fallback)
{
    char line[160];
    std::snprintf(line, sizeof(line), "static const float %.*s = %.9g;\n",
                  static_cast<int>(name.size()), name.data(),
                  static_cast<double>(tuning().scalar(name, fallback)));
    text += line;
}

void append_color(std::string &text, std::string_view name, std::array<float, 3> fallback)
{
    const std::array<float, 3> value = tuning().color(name, fallback);
    char line[200];
    std::snprintf(line, sizeof(line), "static const float3 %.*s = float3(%.9g, %.9g, %.9g);\n",
                  static_cast<int>(name.size()), name.data(), static_cast<double>(value[0]),
                  static_cast<double>(value[1]), static_cast<double>(value[2]));
    text += line;
}

[[nodiscard]] std::array<float, 3> rgb(world::SkyColor color) noexcept
{
    return {color.red, color.green, color.blue};
}

// ---------------------------------------------------------------------------
// The shader library: constants, resources and the functions that light a
// surface in the deep layer. Shared by the terrain and creature shaders and by
// this file's own passes.
// ---------------------------------------------------------------------------
constexpr char library_declarations[] = R"hlsl(
// ===== Deep underground (src/hell_atmosphere.cpp). Presentation only. =====
// The shaders this text is compiled into keep their own additions inside
// `#if defined(OPENMINERZ_DEEP_UNDERGROUND)`: without this text they are the
// shaders they were.
#define OPENMINERZ_DEEP_UNDERGROUND 1
cbuffer HellConstants : register(b3)
{
    // x = 1 / lava map extent (blocks), y = height range, z = height floor,
    // w = 1 while the lava map is bound.
    float4 HellField;
    // xy = centre of the map's window (world x, z), z = distance from it at
    // which the map has faded out, w = 1 / width of that fade.
    float4 HellWindow;
    // x = 1 while the deep-underground model lights this frame, y = the "Deep
    // underground light" setting, z = the lava light's slow pulse (about 1),
    // w = how much of the air pass applies (0..1).
    float4 HellModel;
    // x = how deep the eye is (0 above Hell .. 1 in it), y = how close the eye
    // is to lava (0..1), z = seconds (frozen with Reduced motion), w = share of
    // the lava's glow the open sky leaves in the air (1 underground).
    float4 HellEye;
    // x = march steps, y = heat haze strength, z = unused, w = how far the
    // march reaches (blocks).
    float4 HellAirParams;
    // xy = target pixels, zw = 1 / pixels.
    float4 HellTarget;
};

Texture2D HellLavaMap : register(t20);
Texture2D HellNoise : register(t21);
SamplerState HellMapSampler : register(s5);
SamplerState HellNoiseSampler : register(s6);
)hlsl";

constexpr char library_functions[] = R"hlsl(
float hell_ease(float value)
{
    float t = saturate(value);
    return t * t * (3.0f - 2.0f * t);
}

// world::hell_surface_presence: 0 above the deep layer .. 1 inside it.
float hell_depth_presence(float world_y)
{
    return hell_ease((HELL_FADE_TOP - world_y) / (HELL_FADE_TOP - HELL_FADE_BOTTOM));
}

// The lava map around a point: x = share of the footprint that is the deep
// lake, y = that lava's surface (world y), z and w the same for lava above it.
float4 hell_lava_map(float2 xz, float level)
{
    float4 texel = HellLavaMap.SampleLevel(HellMapSampler, xz * HellField.x, level);
    float2 away = abs(xz - HellWindow.xy);
    float inside = saturate((HellWindow.z - max(away.x, away.y)) * HellWindow.w) * HellField.w;
    float2 top = float2(texel.g / max(texel.r, 0.00001f), texel.a / max(texel.b, 0.00001f)) *
                     HellField.y + HellField.z;
    return float4(texel.r * inside, top.x, texel.b * inside, top.y);
}

// world::lava_air_share: what rises from lava thins out with height.
float hell_air_share(float height, float scale_height)
{
    return hell_ease((height + 1.5f) / 1.5f) * exp(-max(height, 0.0f) / scale_height);
}

// world::lava_far_light: the lake as an area light. The footprint read from
// the map is as wide as the surface is high above the lava, and leans the way
// the surface faces, so a wall sees the lava in front of it and a roof the
// lake under it.
float hell_lava_far(float3 position, float3 normal)
{
    float light = 0.0f;
    // (Real branches: a surface with no lava near it must not pay for the
    // second read of the map.)
    [branch]
    if (HellField.w > 0.5f)
    {
        float4 around = hell_lava_map(position.xz, 5.0f);
        float height = position.y - around.y;
        float taking_over = hell_ease((height - LAVA_FAR_FADE_START) /
                                      (LAVA_FAR_FADE_END - LAVA_FAR_FADE_START));
        [branch]
        if (around.x > 0.0004f && taking_over > 0.0f)
        {
            height = max(height, 0.0f);
            float2 toward = normal.xz * (height * LAVA_FAR_LEAN + 1.0f);
            float level = min(log2(height * LAVA_FOOTPRINT + 1.0f), LAVA_FAR_TOP_LEVEL);
            float coverage = hell_lava_map(position.xz + toward, level).x;
            float below = saturate(0.5f - 0.5f * normal.y);
            light = coverage * below * taking_over /
                    (1.0f + height * height * LAVA_FAR_INV_REACH);
        }
    }
    return light;
}

// What the deep layer adds to a surface's irradiance: rgb, and in a how deep
// the surface is (0..1). `occlusion` is the baked corner shading.
float4 hell_surface_light(float3 position, float3 normal, float occlusion)
{
    float presence = HellModel.x * hell_depth_presence(position.y);
    float4 result = 0.0f.xxxx;
    // (A real branch: everything above the deep layer skips all of this.)
    [branch]
    if (presence > 0.0005f)
    {
        float below = saturate(0.5f - 0.5f * normal.y);
        float3 glow = lerp(HELL_GLOW_ABOVE, HELL_GLOW_BELOW, below);
        float far_light = hell_lava_far(position, normal) * HellModel.z;
        float3 light = (glow + HELL_LAVA_LIGHT * far_light) * HellModel.y;
        result = float4(light * presence * lerp(1.0f, occlusion, HELL_OCCLUSION), presence);
    }
    return result;
}

// Colour of the flood-filled lava light: hotter, so yellower, right at the
// lava. `presence` is how deep the lit surface is: deep in Hell the eye is
// opened (world::hell_exposure_limit), so the same lava light is turned down
// there by about as much, or pale rock beside a lake would burn out.
float3 hell_lava_near_color(float level, float presence)
{
    return lerp(HELL_LAVA_NEAR_COOL, HELL_LAVA_NEAR_HOT, level * level) *
           lerp(1.0f, HELL_LAVA_NEAR_DEEP, presence);
}

// Molten rock breathes: three slow, unrelated swells that drift across the
// cavern, so no two walls pulse together. Still with Reduced motion.
float hell_lava_flicker(float3 position)
{
    float t = HellEye.z;
    return 1.0f + 0.060f * sin(t * 0.83f + dot(position, float3(0.11f, 0.05f, 0.09f))) +
           0.040f * sin(t * 2.10f + dot(position, float3(-0.07f, 0.13f, 0.05f)) + 1.3f) +
           0.022f * sin(t * 5.30f + dot(position, float3(0.19f, -0.03f, 0.23f)) + 4.0f);
}

// Rock within a few blocks of lava is hot enough to glow by itself, most in
// its recesses. `level` is the flood-filled lava light, `relief` 0 in a recess
// .. 1 on a ridge of the rock's own texture.
float3 hell_incandescence(float level, float relief, float presence)
{
    float hot = saturate((level - HELL_INCANDESCENT_FROM) / (1.0f - HELL_INCANDESCENT_FROM));
    return HELL_INCANDESCENT * hot * hot * lerp(1.0f, 0.30f, saturate(relief)) *
           lerp(1.0f, HELL_LAVA_NEAR_DEEP, presence);
}

// Blood stone under the deep layer's light: brick red that keeps a little
// green and blue, deepest in its recesses, so it can read brown in the dark
// and orange near lava instead of one flat red at every light level.
float3 hell_blood_stone(float mineral)
{
    return lerp(HELL_STONE_RECESS, HELL_STONE_RIDGE, saturate(mineral * 1.6f));
}

// Coordinates across a lava face that do not reset at block edges. A wall of
// lava runs downward.
float2 hell_lava_face(float3 position, float3 normal)
{
    if (abs(normal.y) > 0.5f)
        return position.xz;
    float2 face = abs(normal.x) > 0.5f ? position.zy : position.xy;
    face.y += HellEye.z * 0.22f;
    return face;
}

// How much the first and the second layer of lava artwork are magnified.
static const float HELL_LAVA_FIRST_SCALE = 0.43f;
static const float HELL_LAVA_SECOND_SCALE = 0.19f;

// Where to read the pack's own lava artwork for two layers that drift across
// each other: xy and zw. `face_dx` and `face_dy` are the screen-space
// derivatives of `face`, taken by the caller OUTSIDE its lava branch: every
// read below names its own gradients, so the whole of this stays inside a real
// branch that only lava pixels take.
float4 hell_lava_layers(float2 face, float2 face_dx, float2 face_dy)
{
    float t = HellEye.z;
    float2 swirl = HellNoise.SampleGrad(HellNoiseSampler,
                                        face * 0.021f + float2(t * 0.0031f, -t * 0.0023f),
                                        face_dx * 0.021f, face_dy * 0.021f).rg - 0.5f;
    float2 first = face * HELL_LAVA_FIRST_SCALE + swirl * 0.55f + float2(t * 0.011f, t * 0.007f);
    float2 second = face * HELL_LAVA_SECOND_SCALE - swirl * 0.38f +
                    float2(-t * 0.006f, t * 0.009f) + 0.37f;
    return float4(first, second);
}

// The colour of rock that hot: black crust, dull red, orange, yellow and, only
// in the thinnest cores, nearly white.
float3 hell_heat_color(float heat)
{
    float3 color = lerp(HELL_MOLTEN_CRUST, HELL_MOLTEN_RED, smoothstep(0.06f, 0.30f, heat));
    color = lerp(color, HELL_MOLTEN_ORANGE, smoothstep(0.26f, 0.55f, heat));
    color = lerp(color, HELL_MOLTEN_YELLOW, smoothstep(0.52f, 0.82f, heat));
    return lerp(color, HELL_MOLTEN_CORE, smoothstep(0.86f, 1.0f, heat));
}

// Lava's surface. `art_first` and `art_second` are how bright the pack's lava
// artwork is at hell_lava_layers(): its own blobs of melt stay recognisable,
// while plates of crust drift over them and crack.
float3 hell_molten(float2 face, float2 face_dx, float2 face_dy, float art_first,
                   float art_second)
{
    float t = HellEye.z;
    float4 slow = HellNoise.SampleGrad(HellNoiseSampler,
                                       face * 0.037f + float2(-t * 0.0042f, t * 0.0028f),
                                       face_dx * 0.037f, face_dy * 0.037f);
    float4 fine = HellNoise.SampleGrad(HellNoiseSampler,
                                       face * 0.17f + slow.rg * 0.35f +
                                           float2(t * 0.006f, t * 0.004f),
                                       face_dx * 0.17f, face_dy * 0.17f);
    float crack = pow(saturate(1.0f - abs(fine.b * 2.0f - 1.0f)), HELL_MOLTEN_CRACK_WIDTH);
    // Nonlinear heat cannot be evaluated on averaged noise: at a coarse mip
    // noise converges on 0.5, which used to turn EVERY texel into a hot crack.
    // Integrate unresolved cracks toward their coverage, not peak temperature.
    float footprint = max(length(face_dx), length(face_dy));
    float unresolved = smoothstep(0.08f, 0.65f, footprint);
    crack = lerp(crack, 1.0f / (HELL_MOLTEN_CRACK_WIDTH + 1.0f), unresolved);
    float plates = smoothstep(0.30f, 0.72f, slow.a);
    float heat = max(art_first * lerp(0.50f, 1.0f, plates),
                     art_second * 0.74f * lerp(1.0f, 0.45f, plates));
    heat = max(heat, crack * lerp(0.40f, 0.92f, plates));
    heat *= 0.90f + 0.10f * sin(t * 0.7f + slow.g * 6.2831853f);
    // Even the crust is hot: never black, a dull mottled red.
    heat = max(heat * HELL_MOLTEN_HEAT, HELL_MOLTEN_FLOOR * (0.55f + 0.90f * slow.r));
    return lerp(hell_heat_color(saturate(heat)),
                float3(0.72f, 0.035f, 0.003f) * (0.85f + slow.r * 0.3f), unresolved);
}
)hlsl";

std::string build_library()
{
    using namespace world;
    std::string text{library_declarations};
    append_scalar(text, "HELL_FADE_TOP", hell_surface_fade_top);
    append_scalar(text, "HELL_FADE_BOTTOM", hell_surface_fade_bottom);
    append_scalar(text, "LAVA_FOOTPRINT", lava_footprint_scale);
    append_scalar(text, "LAVA_FAR_TOP_LEVEL", lava_far_top_level);
    append_scalar(text, "LAVA_FAR_INV_REACH",
                  1.0F / (tuning().scalar("LAVA_FAR_HALF_HEIGHT", lava_far_half_height) *
                          tuning().scalar("LAVA_FAR_HALF_HEIGHT", lava_far_half_height)));
    append_scalar(text, "LAVA_FAR_FADE_START", lava_far_fade_in_start);
    append_scalar(text, "LAVA_FAR_FADE_END", lava_far_fade_in_end);
    // How far a wall's footprint leans toward what it faces, per block of height.
    append_scalar(text, "LAVA_FAR_LEAN", 0.75F);
    append_scalar(text, "LAVA_GLOW_HEIGHT", lava_glow_height);
    append_scalar(text, "LAVA_HEAT_HEIGHT", lava_heat_height);
    append_scalar(text, "HEAT_SATURATION", heat_haze_saturation);
    append_scalar(text, "HEAT_REACH", heat_haze_reach);
    append_color(text, "HELL_GLOW_ABOVE", rgb(hell_glow_from_above));
    append_color(text, "HELL_GLOW_BELOW", rgb(hell_glow_from_below));
    append_color(text, "HELL_LAVA_LIGHT", rgb(hell_lava_light));
    // How much of the baked corner shading the deep layer's light takes on top
    // of what every light takes: contact is what gives dim rock its shape.
    append_scalar(text, "HELL_OCCLUSION", 0.55F);
    append_color(text, "HELL_LAVA_NEAR_COOL", {2.30F, 0.46F, 0.050F});
    append_color(text, "HELL_LAVA_NEAR_HOT", {3.40F, 1.20F, 0.180F});
    append_scalar(text, "HELL_LAVA_NEAR_DEEP", 0.55F);
    append_scalar(text, "HELL_INCANDESCENT_FROM", 0.80F);
    append_color(text, "HELL_INCANDESCENT", {0.42F, 0.070F, 0.008F});
    append_color(text, "HELL_STONE_RECESS", {0.50F, 0.265F, 0.215F});
    append_color(text, "HELL_STONE_RIDGE", {0.84F, 0.420F, 0.285F});
    append_color(text, "HELL_MOLTEN_CRUST", {0.030F, 0.0060F, 0.0030F});
    append_color(text, "HELL_MOLTEN_RED", {0.85F, 0.060F, 0.004F});
    append_color(text, "HELL_MOLTEN_ORANGE", {3.00F, 0.62F, 0.035F});
    append_color(text, "HELL_MOLTEN_YELLOW", {5.60F, 2.30F, 0.26F});
    append_color(text, "HELL_MOLTEN_CORE", {7.50F, 4.40F, 1.15F});
    append_scalar(text, "HELL_MOLTEN_CRACK_WIDTH", 9.0F);
    append_scalar(text, "HELL_MOLTEN_HEAT", 0.80F);
    append_scalar(text, "HELL_MOLTEN_FLOOR", 0.11F);
    // Under an open daytime sky the eye is closed down and lava would read as
    // cold rock; there it is this much brighter, so it is still plainly lava.
    append_scalar(text, "HELL_MOLTEN_DAYLIGHT", 0.70F);
    text += library_functions;
    return text;
}

// ---------------------------------------------------------------------------
// The air pass.
// ---------------------------------------------------------------------------
constexpr char scene_prefix[] = R"hlsl(
cbuffer Scene : register(b0)
{
    float4 Eye;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraForward;
    float4 Projection;
    float4 LightAndFogStart;
    float4 FogAndEnd;
    float4 WaterAndTime;
};
)hlsl";

constexpr char screen_vertex_source[] = R"hlsl(
struct ScreenInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

ScreenInput main(uint vertex_id : SV_VertexID)
{
    ScreenInput output;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}
)hlsl";

constexpr char air_pixel_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D<float> Depth : register(t1);
SamplerState LinearClamp : register(s0);

struct ScreenInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float linear_depth(float depth)
{
    return Projection.w / min(depth - Projection.z, -0.0000001f);
}

float4 main(ScreenInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float camera_depth = linear_depth(Depth.Load(int3(pixel, 0)));
    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float3 ray = CameraForward.xyz + CameraRight.xyz * (ndc.x / Projection.x) +
                 CameraUp.xyz * (ndc.y / Projection.y);
    float ray_length = length(ray);
    float3 direction = ray / ray_length;
    float end = min(camera_depth * ray_length, HellAirParams.w);
    int steps = (int)(HellAirParams.x + 0.5f);
    float step_length = end / (float)steps;
    // A step's footprint on the map is its own length: long steps read the
    // coarse levels, so a few of them still see every lake they cross.
    float level = clamp(log2(max(step_length, 1.0f)) + 0.5f, 1.5f, 5.0f);
    // The map's coarse levels make what is integrated smooth, so a few evenly
    // spaced steps do not band. Each pixel still starts a fraction of a step
    // differently (interleaved gradient noise): just enough to break up the
    // contours a long, shallow view across a lake would otherwise show, too
    // little to read as grain.
    float jitter = frac(52.9829189f * frac(dot(input.position.xy, float2(0.06711056f, 0.00583715f))));

    float3 glow = 0.0f.xxx;
    float transmittance = 1.0f;
    float heat = 0.0f;
    [loop]
    for (int index = 0; index < steps; ++index)
    {
        float travelled = ((float)index + 0.5f + (jitter - 0.5f) * HELL_AIR_JITTER) * step_length;
        float3 position = Eye.xyz + direction * travelled;
        float4 lava = hell_lava_map(position.xz, level);
        float deep_height = position.y - lava.y;
        float shallow_height = position.y - lava.w;
        float glowing = lava.x * hell_air_share(deep_height, LAVA_GLOW_HEIGHT) +
                        lava.z * hell_air_share(shallow_height, LAVA_GLOW_HEIGHT) * HellEye.w;
        float hot = lava.x * hell_air_share(deep_height, LAVA_HEAT_HEIGHT) +
                    lava.z * hell_air_share(shallow_height, LAVA_HEAT_HEIGHT);
        // Thin warm haze through the whole deep layer, thick glowing air
        // above lava.
        float thin = hell_depth_presence(position.y) * HELL_AIR_DENSITY;
        float thick = glowing * HELL_LAVA_AIR_DENSITY;
        float density = thin + thick;
        float through = exp(-density * step_length);
        // Energy conserving: a step passes on only what it did not absorb,
        // and adds its own glow in the same proportion.
        glow += transmittance * (1.0f - through) *
                (HELL_AIR_GLOW * thin + HELL_LAVA_AIR_GLOW * thick) / max(density, 0.000001f);
        transmittance *= through;
        // The first few blocks hardly count: what the player is working on
        // stays where it is.
        heat += hot * step_length * saturate((travelled - 2.0f) / 10.0f);
    }
    float applied = HellModel.w;
    // The haze right at the eye: with the eye deep in it, even the darkest
    // corner is a dark warm brown, never a hole in the picture.
    glow += HELL_AIR_VEIL * HellEye.x;
    glow *= HellModel.y * applied;
    transmittance = lerp(1.0f, transmittance, applied);

    // Heat haze: the picture shimmers where the sight line crossed hot air.
    float2 uv = input.uv;
    float haze = HellAirParams.y * applied * (1.0f - exp(-heat * HEAT_SATURATION));
    float lens = HellAirParams.y * HellEye.y;
    if (haze + lens > 0.004f)
    {
        float aspect = HellTarget.x * HellTarget.w;
        float t = HellEye.z;
        float2 cell = float2(input.uv.x * aspect, input.uv.y);
        // Two layers of rising air, taller than they are wide.
        float2 broad = HellNoise.SampleLevel(HellNoiseSampler,
                                             cell * float2(4.6f, 2.5f) + float2(0.0f, t * 0.19f),
                                             0.0f).rg;
        float2 fine = HellNoise.SampleLevel(HellNoiseSampler,
                                            cell * float2(10.3f, 5.9f) + float2(0.37f, t * 0.41f),
                                            0.0f).ba;
        float2 wobble = (broad - 0.5f) * 1.5f + (fine - 0.5f) * 0.75f;
        // Standing right at lava, the air at the lens itself ripples, toward
        // the edges of the picture and never at the crosshair.
        float2 centred = input.uv * 2.0f - 1.0f;
        float edge = smoothstep(0.50f, 1.0f, max(abs(centred.x), abs(centred.y)));
        float2 shift = wobble * float2(1.0f, 0.55f) * HEAT_REACH * (haze + lens * edge * 1.4f);
        shift.x /= aspect;
        float2 shifted = saturate(input.uv + shift);
        // The shimmer is in the air in front of what is seen: it may move a
        // far thing, never drag a near one over it.
        float shifted_depth = linear_depth(Depth.Load(int3(int2(shifted * HellTarget.xy), 0)));
        if (shifted_depth > camera_depth * 0.6f)
            uv = shifted;
    }
    float3 color = Scene.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    return float4(color * transmittance + glow, 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Embers and ash: one draw of camera-facing specks whose places are a pure
// function of their number, the clock and the lava map. No vertex buffer, no
// CPU work, no state from frame to frame.
// ---------------------------------------------------------------------------
constexpr char ember_vertex_source[] = R"hlsl(
struct EmberVertex
{
    float4 position : SV_POSITION;
    float2 corner : TEXCOORD0;
    // rgb = light, a = strength.
    float4 color : COLOR0;
    // 0 = pure glow (an ember), up to 1 = also covers what is behind (ash).
    float cover : TEXCOORD1;
};

uint ember_hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

float ember_random(uint seed)
{
    return (float)(ember_hash(seed) & 0xFFFFFFu) / 16777216.0f;
}

EmberVertex main(uint vertex_id : SV_VertexID)
{
    EmberVertex output;
    static const float2 corners[6] = {float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
                                      float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f)};
    uint particle = vertex_id / 6u;
    float2 corner = corners[vertex_id % 6u];
    uint seed = particle * 9u + 17u;
    float3 a = float3(ember_random(seed), ember_random(seed + 1u), ember_random(seed + 2u));
    float3 b = float3(ember_random(seed + 3u), ember_random(seed + 4u), ember_random(seed + 5u));
    float t = HellEye.z;

    float3 world;
    float3 light;
    float strength;
    float size;
    float cover = 0.0f;
    if ((particle & 3u) != 3u)
    {
        // An ember: born on the lava under it, it rises, cools and dies, and
        // the next one starts somewhere else.
        float life_seconds = lerp(5.0f, 9.5f, b.x);
        float clock = t / life_seconds + a.y;
        float age = frac(clock);
        uint life = (uint)floor(clock);
        float2 start = float2(a.x, a.z) * EMBER_BOX +
                       float2(ember_random(seed + 7u + life * 131u),
                              ember_random(seed + 8u + life * 197u)) * 9.0f;
        float2 relative = frac((start - Eye.xz) / EMBER_BOX + 0.5f) * EMBER_BOX - 0.5f * EMBER_BOX;
        float2 xz = Eye.xz + relative;
        float4 lava = hell_lava_map(xz, 1.0f);
        bool shallow = lava.z > lava.x;
        float coverage = shallow ? lava.z : lava.x;
        float top = shallow ? lava.w : lava.y;
        float rise = lerp(6.0f, 14.0f, b.y);
        float sway = age * lerp(0.25f, 0.85f, b.z);
        world = float3(xz.x + sin(t * (0.5f + b.z) + a.x * 40.0f) * sway,
                       top + 0.10f + age * rise * (1.0f - 0.30f * age),
                       xz.y + cos(t * (0.4f + b.x) + a.z * 40.0f) * sway);
        float heat = 1.0f - age;
        light = lerp(EMBER_COOL, EMBER_HOT, heat * heat);
        float flicker = 0.78f + 0.22f * sin(t * (7.0f + b.x * 9.0f) + a.z * 50.0f);
        strength = smoothstep(0.0f, 0.05f, age) * (1.0f - smoothstep(0.55f, 1.0f, age)) *
                   smoothstep(0.30f, 0.62f, coverage) * flicker;
        // Fade toward the faces of the box the embers wrap in.
        float2 border = 1.0f - abs(relative) / (0.5f * EMBER_BOX);
        strength *= saturate(min(border.x, border.y) * 5.0f);
        size = lerp(0.030f, 0.058f, b.z) * (0.65f + 0.35f * heat);
    }
    else
    {
        // Ash: slow flakes anywhere in the deep layer, caught by the glow.
        const float3 box = float3(EMBER_BOX * 0.7f, 22.0f, EMBER_BOX * 0.7f);
        float3 start = a * box +
                       float3(sin(t * 0.13f + b.x * 6.2831853f) * 1.6f, t * lerp(0.16f, 0.50f, b.y),
                              cos(t * 0.11f + b.z * 6.2831853f) * 1.6f);
        float3 relative = frac((start - Eye.xyz) / box + 0.5f) * box - 0.5f * box;
        world = Eye.xyz + relative;
        float3 border = 1.0f - abs(relative) / (0.5f * box);
        light = EMBER_ASH;
        strength = hell_depth_presence(world.y) *
                   saturate(min(border.x, min(border.y, border.z)) * 5.0f) * 0.60f;
        size = lerp(0.020f, 0.042f, b.z);
        cover = 0.85f;
    }

    float3 from_eye = world - Eye.xyz;
    float camera_z = dot(from_eye, CameraForward.xyz);
    // Never narrower than a pixel and a half: a far ember dims instead of
    // flickering between pixels.
    float pixel = camera_z * 2.0f / (Projection.y * HellTarget.y);
    float drawn = max(size, pixel * 1.5f);
    strength *= (size * size) / (drawn * drawn);
    // Far ones dim with the haze; one right at the lens fades out instead of
    // crossing the picture as a glowing disc.
    strength *= HellModel.w * exp(-length(from_eye) * 0.018f) *
                saturate((camera_z - 0.6f) / 1.8f);
    float3 placed = from_eye + (CameraRight.xyz * corner.x + CameraUp.xyz * corner.y) * drawn;
    float x = dot(placed, CameraRight.xyz);
    float y = dot(placed, CameraUp.xyz);
    output.position = float4(x * Projection.x, y * Projection.y,
                             camera_z * Projection.z + Projection.w, camera_z);
    if (camera_z < 0.15f || strength < 0.004f)
        output.position = float4(0.0f, 0.0f, -1.0f, 1.0f);
    output.corner = corner;
    output.color = float4(light, strength);
    output.cover = cover;
    return output;
}
)hlsl";

constexpr char ember_pixel_source[] = R"hlsl(
struct EmberVertex
{
    float4 position : SV_POSITION;
    float2 corner : TEXCOORD0;
    float4 color : COLOR0;
    float cover : TEXCOORD1;
};

float4 main(EmberVertex input) : SV_TARGET
{
    float distance_squared = dot(input.corner, input.corner);
    clip(1.0f - distance_squared);
    float core = exp(-distance_squared * 3.2f) * input.color.a;
    // Premultiplied: an ember only adds light, a flake of ash also covers.
    return float4(input.color.rgb * core, core * input.cover);
}
)hlsl";

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};
};

struct alignas(16) HellConstants final
{
    Float4 field;
    Float4 window;
    Float4 model;
    Float4 eye;
    Float4 air;
    Float4 target;
};

// IEEE half from float, round to nearest; the map's values are small and
// positive, so the general cases cost nothing here.
[[nodiscard]] std::uint16_t to_half(float value) noexcept
{
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0)
    {
        if (exponent < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa = (mantissa | 0x800000U) >> static_cast<unsigned>(1 - exponent);
        return static_cast<std::uint16_t>(sign | ((mantissa + 0x1000U) >> 13U));
    }
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    return static_cast<std::uint16_t>(
        (sign | (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U)) +
        ((mantissa & 0x1000U) != 0U ? 1U : 0U));
}

// Smooth, tileable value noise: r and g drive drift and shimmer, b the cracks
// in lava's crust, a its plates. Four unrelated fields in one small texture.
[[nodiscard]] std::vector<std::uint8_t> build_noise(int size)
{
    const auto hash = [](int x, int y, int seed) {
        auto h = static_cast<std::uint32_t>(x) * 374761393U +
                 static_cast<std::uint32_t>(y) * 668265263U +
                 static_cast<std::uint32_t>(seed) * 2246822519U;
        h = (h ^ (h >> 13U)) * 1274126177U;
        h ^= h >> 16U;
        return static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0xFFFFFF);
    };
    const auto value = [&](float x, float y, int period, int seed) {
        const float fx = std::floor(x);
        const float fy = std::floor(y);
        const int x0 = ((static_cast<int>(fx) % period) + period) % period;
        const int y0 = ((static_cast<int>(fy) % period) + period) % period;
        const int x1 = (x0 + 1) % period;
        const int y1 = (y0 + 1) % period;
        float tx = x - fx;
        float ty = y - fy;
        tx = tx * tx * tx * (tx * (tx * 6.0F - 15.0F) + 10.0F);
        ty = ty * ty * ty * (ty * (ty * 6.0F - 15.0F) + 10.0F);
        const float top = std::lerp(hash(x0, y0, seed), hash(x1, y0, seed), tx);
        const float bottom = std::lerp(hash(x0, y1, seed), hash(x1, y1, seed), tx);
        return std::lerp(top, bottom, ty);
    };
    const auto fbm = [&](float u, float v, int base_period, int octaves, int seed) {
        float total{};
        float amplitude = 0.5F;
        float normaliser{};
        int period = base_period;
        for (int octave = 0; octave < octaves; ++octave)
        {
            total += value(u * static_cast<float>(period), v * static_cast<float>(period), period,
                           seed + octave * 17) *
                     amplitude;
            normaliser += amplitude;
            amplitude *= 0.5F;
            period *= 2;
        }
        return total / normaliser;
    };
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(size) * size * 4U);
    const auto pack = [](float channel) {
        // The sum of octaves crowds around one half; spread it over the range.
        return static_cast<std::uint8_t>(
            std::lround(std::clamp((channel - 0.5F) * 1.9F + 0.5F, 0.0F, 1.0F) * 255.0F));
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
            const std::size_t index = (static_cast<std::size_t>(y) * size + x) * 4U;
            texels[index] = pack(fbm(u, v, 8, 3, 211));
            texels[index + 1U] = pack(fbm(u, v, 8, 3, 337));
            texels[index + 2U] = pack(fbm(u, v, 8, 4, 59));
            texels[index + 3U] = pack(fbm(u, v, 4, 3, 463));
        }
    return texels;
}

} // namespace

class HellAtmosphere::Impl final
{
  public:
    Impl(ID3D11Device *device, ID3D11DeviceContext *context) : device_(device), context_(context)
    {
        if (const char *value = SDL_getenv("OPENMINERZ_HELL_HEAT"))
            disabled_ = std::string_view{value} == "0";
        ready_ = create_shaders() && create_states() && create_textures();
        if (!ready_)
            SDL_Log("Deep-underground effects unavailable: %s", failure_.c_str());
    }

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

    [[nodiscard]] world::LavaField &lava() noexcept
    {
        return lava_;
    }

    [[nodiscard]] const HellFrameState &state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] float pulse() const noexcept
    {
        return pulse_;
    }

    [[nodiscard]] float lava_light_at(float x, float y, float z, float normal_y) const noexcept
    {
        if (!state_.model)
            return 0.0F;
        const world::LavaField::Sample around = lava_.sample(x, z, 5.0F);
        if (around.deep <= 0.0004F)
            return 0.0F;
        const float height = y - around.deep_top;
        const float coverage = lava_.sample(x, z, world::lava_footprint_level(height)).deep;
        return world::lava_far_light(coverage, height, normal_y);
    }

    HellFrameState begin_frame(const HellFrameInput &input)
    {
        state_ = {};
        HellConstants constants{};
        if (!ready_)
            return state_;
        const int chunk_x =
            static_cast<int>(std::floor(input.eye[0] / static_cast<float>(world::lava_field_chunk)));
        const int chunk_z =
            static_cast<int>(std::floor(input.eye[2] / static_cast<float>(world::lava_field_chunk)));
        lava_.recenter(chunk_x, chunk_z);
        upload_map();

        const PostEffects &effects = input.effects;
        const float strength = std::clamp(effects.deep_light, 0.0F, 2.0F);
        state_.model = input.enhanced && effects.enabled && strength > 0.001F && !disabled_;
        if (state_.model)
        {
            const float presence = std::clamp(input.eye_presence, 0.0F, 1.0F);
            const float seconds = effects.reduced_motion ? 0.0F : input.elapsed_seconds;
            pulse_ = world::lava_pulse(seconds);

            // Is there lava the eye could be looking at? A wide read of the
            // map around it: the lake under Hell, and lava above it (a pocket
            // in a cave wall, lava met at the surface).
            const world::LavaField::Sample wide = lava_.sample(input.eye[0], input.eye[2], 6.0F);
            float near_lava{};
            if (wide.deep > 1.0e-6F)
                near_lava = 1.0F - world::hell_ease((input.eye[1] - wide.deep_top - 26.0F) / 18.0F);
            if (wide.shallow > 1.0e-6F)
                near_lava = std::max(
                    near_lava, 1.0F - world::hell_ease(
                                          (std::abs(input.eye[1] - wide.shallow_top) - 10.0F) /
                                          12.0F));
            const float applied = std::clamp(std::max(presence, near_lava), 0.0F, 1.0F);
            state_.air = applied > 0.002F && effects.deep_air_steps > 0;
            state_.embers = state_.air && effects.ember_count > 0 && !effects.reduced_motion;
            state_.exposure_limit =
                std::lerp(1.0F,
                          tuning().scalar("HELL_EXPOSURE_HEADROOM", world::hell_exposure_headroom),
                          presence);
            state_.deep_grade = presence;

            // Standing right at lava: the air at the lens ripples.
            const world::LavaField::Sample close = lava_.sample(input.eye[0], input.eye[2], 2.0F);
            const float lens =
                std::max(close.deep * world::lava_air_share(input.eye[1] - close.deep_top, 2.4F),
                         close.shallow *
                             world::lava_air_share(input.eye[1] - close.shallow_top, 2.4F));
            // Under an open noon sky the air above lava hardly shows its glow.
            const float open_sky =
                std::clamp(input.eye_sky_light, 0.0F, 1.0F) * std::clamp(input.daylight, 0.0F, 1.0F);

            constants.field = {1.0F / static_cast<float>(world::lava_field_extent),
                               world::lava_field_height_range, world::lava_field_height_floor,
                               map_view_ != nullptr ? 1.0F : 0.0F};
            constants.window = {lava_.center_x(), lava_.center_z(),
                                world::lava_field_fade_start + world::lava_field_fade_width,
                                1.0F / world::lava_field_fade_width};
            constants.model = {1.0F, strength, pulse_, state_.air ? applied : 0.0F};
            constants.eye = {presence, std::clamp(lens * 1.6F, 0.0F, 1.0F), seconds,
                             1.0F - 0.80F * open_sky};
            constants.air = {static_cast<float>(std::clamp(effects.deep_air_steps, 2, 24)),
                             effects.heat_haze && !effects.reduced_motion ? 1.0F : 0.0F, 0.0F,
                             tuning().scalar("HELL_AIR_REACH", 124.0F)};
            constants.target = {static_cast<float>(std::max(input.width, 1)),
                                static_cast<float>(std::max(input.height, 1)),
                                1.0F / static_cast<float>(std::max(input.width, 1)),
                                1.0F / static_cast<float>(std::max(input.height, 1))};
            ember_count_ = static_cast<UINT>(std::clamp(effects.ember_count, 0, 12000));
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(constant_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                    &mapped)))
        {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context_->Unmap(constant_buffer_.Get(), 0U);
        }
        return state_;
    }

    void bind()
    {
        if (!ready_)
            return;
        ID3D11Buffer *buffer = constant_buffer_.Get();
        context_->VSSetConstantBuffers(constant_slot, 1U, &buffer);
        context_->PSSetConstantBuffers(constant_slot, 1U, &buffer);
        const std::array<ID3D11ShaderResourceView *, 2> views{map_view_.Get(), noise_view_.Get()};
        context_->VSSetShaderResources(map_slot, static_cast<UINT>(views.size()), views.data());
        context_->PSSetShaderResources(map_slot, static_cast<UINT>(views.size()), views.data());
        const std::array<ID3D11SamplerState *, 2> samplers{map_sampler_.Get(),
                                                           noise_sampler_.Get()};
        context_->VSSetSamplers(sampler_slot, static_cast<UINT>(samplers.size()), samplers.data());
        context_->PSSetSamplers(sampler_slot, static_cast<UINT>(samplers.size()), samplers.data());
    }

    void unbind()
    {
        ID3D11Buffer *no_buffer{};
        context_->VSSetConstantBuffers(constant_slot, 1U, &no_buffer);
        context_->PSSetConstantBuffers(constant_slot, 1U, &no_buffer);
        constexpr std::array<ID3D11ShaderResourceView *, 2> no_views{};
        context_->VSSetShaderResources(map_slot, static_cast<UINT>(no_views.size()),
                                       no_views.data());
        context_->PSSetShaderResources(map_slot, static_cast<UINT>(no_views.size()),
                                       no_views.data());
        constexpr std::array<ID3D11SamplerState *, 2> no_samplers{};
        context_->VSSetSamplers(sampler_slot, static_cast<UINT>(no_samplers.size()),
                                no_samplers.data());
        context_->PSSetSamplers(sampler_slot, static_cast<UINT>(no_samplers.size()),
                                no_samplers.data());
    }

    void draw_air(ID3D11ShaderResourceView *scene, ID3D11RenderTargetView *target,
                  ID3D11ShaderResourceView *depth, ID3D11Buffer *scene_constants, int width,
                  int height)
    {
        if (!ready_ || target == nullptr || scene == nullptr || depth == nullptr)
            return;
        bind();
        set_viewport(width, height);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->RSSetState(rasterizer_.Get());
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
        context_->OMSetRenderTargets(1U, &target, nullptr);
        context_->VSSetShader(screen_vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(air_pixel_shader_.Get(), nullptr, 0U);
        context_->PSSetConstantBuffers(0U, 1U, &scene_constants);
        ID3D11SamplerState *sampler = clamp_sampler_.Get();
        context_->PSSetSamplers(0U, 1U, &sampler);
        const std::array<ID3D11ShaderResourceView *, 2> views{scene, depth};
        context_->PSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
        context_->Draw(3U, 0U);
        constexpr std::array<ID3D11ShaderResourceView *, 2> no_views{};
        context_->PSSetShaderResources(0U, static_cast<UINT>(no_views.size()), no_views.data());
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }

    void draw_embers(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth_readonly,
                     ID3D11Buffer *scene_constants, int width, int height)
    {
        if (!ready_ || target == nullptr || depth_readonly == nullptr || ember_count_ == 0U)
            return;
        bind();
        set_viewport(width, height);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->RSSetState(rasterizer_.Get());
        context_->OMSetDepthStencilState(test_depth_state_.Get(), 0U);
        context_->OMSetBlendState(ember_blend_.Get(), nullptr, 0xFFFFFFFFU);
        context_->OMSetRenderTargets(1U, &target, depth_readonly);
        context_->VSSetShader(ember_vertex_shader_.Get(), nullptr, 0U);
        context_->PSSetShader(ember_pixel_shader_.Get(), nullptr, 0U);
        context_->VSSetConstantBuffers(0U, 1U, &scene_constants);
        context_->Draw(ember_count_ * 6U, 0U);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
    }

  private:
    static constexpr UINT constant_slot = 3U;
    static constexpr UINT map_slot = 20U;
    static constexpr UINT sampler_slot = 5U;

    [[nodiscard]] ComPtr<ID3DBlob> compile(std::string_view name, const std::string &source,
                                           const char *target)
    {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        // Cached for the process (shader_cache.hpp): built again with every world.
        const HRESULT result = compile_hlsl_cached(
            source.data(), source.size(), nullptr, nullptr, nullptr, "main", target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &shader, &errors);
        if (FAILED(result))
        {
            failure_ = std::string{name} + ": ";
            failure_ += errors != nullptr
                            ? std::string{static_cast<const char *>(errors->GetBufferPointer()),
                                          errors->GetBufferSize()}
                            : std::string{"shader compilation failed"};
            return {};
        }
        return shader;
    }

    [[nodiscard]] bool create_shaders()
    {
        std::string constants;
        // The haze through the whole deep layer and the glowing air above
        // lava: how much of the light crossing one block each takes, and the
        // light a sight line through endlessly much of it would see.
        append_scalar(constants, "HELL_AIR_DENSITY", 0.0060F);
        append_color(constants, "HELL_AIR_GLOW", {0.062F, 0.0170F, 0.0100F});
        append_scalar(constants, "HELL_LAVA_AIR_DENSITY", 0.020F);
        append_color(constants, "HELL_LAVA_AIR_GLOW", {0.80F, 0.180F, 0.028F});
        append_color(constants, "HELL_AIR_VEIL", {0.0100F, 0.0048F, 0.0034F});
        // Share of one step by which a pixel's march may start early or late.
        append_scalar(constants, "HELL_AIR_JITTER", 0.20F);
        // Side of the box the embers wrap in around the eye, in blocks.
        append_scalar(constants, "EMBER_BOX", 52.0F);
        append_color(constants, "EMBER_HOT", {9.0F, 3.3F, 0.48F});
        append_color(constants, "EMBER_COOL", {2.4F, 0.26F, 0.012F});
        append_color(constants, "EMBER_ASH", {0.22F, 0.085F, 0.050F});
        const std::string library = HellAtmosphere::shader_library();
        const std::string prefix = library + scene_prefix + constants;

        const ComPtr<ID3DBlob> screen = compile("screen", screen_vertex_source, "vs_5_0");
        const ComPtr<ID3DBlob> air = compile("deep air", prefix + air_pixel_source, "ps_5_0");
        const ComPtr<ID3DBlob> ember_vertex =
            compile("embers", prefix + ember_vertex_source, "vs_5_0");
        const ComPtr<ID3DBlob> ember_pixel = compile("ember", ember_pixel_source, "ps_5_0");
        return screen != nullptr && air != nullptr && ember_vertex != nullptr &&
               ember_pixel != nullptr &&
               SUCCEEDED(device_->CreateVertexShader(screen->GetBufferPointer(),
                                                     screen->GetBufferSize(), nullptr,
                                                     &screen_vertex_shader_)) &&
               SUCCEEDED(device_->CreatePixelShader(air->GetBufferPointer(), air->GetBufferSize(),
                                                    nullptr, &air_pixel_shader_)) &&
               SUCCEEDED(device_->CreateVertexShader(ember_vertex->GetBufferPointer(),
                                                     ember_vertex->GetBufferSize(), nullptr,
                                                     &ember_vertex_shader_)) &&
               SUCCEEDED(device_->CreatePixelShader(ember_pixel->GetBufferPointer(),
                                                    ember_pixel->GetBufferSize(), nullptr,
                                                    &ember_pixel_shader_));
    }

    [[nodiscard]] bool fail(const char *what)
    {
        failure_ = std::string{"could not create the "} + what;
        return false;
    }

    [[nodiscard]] bool create_states()
    {
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buffer.ByteWidth = sizeof(HellConstants);
        const HellConstants zero{};
        const D3D11_SUBRESOURCE_DATA initial{&zero, 0U, 0U};
        if (FAILED(device_->CreateBuffer(&buffer, &initial, &constant_buffer_)))
            return fail("constant buffer");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        if (FAILED(device_->CreateDepthStencilState(&depth, &no_depth_state_)))
            return fail("depth state");
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device_->CreateDepthStencilState(&depth, &test_depth_state_)))
            return fail("ember depth state");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend, &opaque_blend_)))
            return fail("blend state");
        auto &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device_->CreateBlendState(&blend, &ember_blend_)))
            return fail("ember blend state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rasterizer, &rasterizer_)))
            return fail("rasterizer state");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler, &map_sampler_)) ||
            FAILED(device_->CreateSamplerState(&sampler, &noise_sampler_)))
            return fail("wrap sampler");
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device_->CreateSamplerState(&sampler, &clamp_sampler_)))
            return fail("clamp sampler");
        return true;
    }

    [[nodiscard]] bool create_textures()
    {
        // The lava map: empty until the first chunk that shows lava arrives.
        D3D11_TEXTURE2D_DESC map{};
        map.Width = static_cast<UINT>(world::lava_field_extent);
        map.Height = static_cast<UINT>(world::lava_field_extent);
        map.MipLevels = static_cast<UINT>(world::lava_field_levels);
        map.ArraySize = 1U;
        map.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        map.SampleDesc.Count = 1U;
        map.Usage = D3D11_USAGE_DEFAULT;
        map.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<std::uint16_t> empty(static_cast<std::size_t>(world::lava_field_extent) *
                                         world::lava_field_extent * 4U);
        std::array<D3D11_SUBRESOURCE_DATA, world::lava_field_levels> initial{};
        for (int level = 0; level < world::lava_field_levels; ++level)
            initial[static_cast<std::size_t>(level)] = {
                empty.data(), static_cast<UINT>((world::lava_field_extent >> level) * 8), 0U};
        if (FAILED(device_->CreateTexture2D(&map, initial.data(), &map_texture_)) ||
            FAILED(device_->CreateShaderResourceView(map_texture_.Get(), nullptr, &map_view_)))
            return fail("lava map");

        constexpr int noise_size = 128;
        std::vector<std::vector<std::uint8_t>> levels;
        levels.push_back(build_noise(noise_size));
        for (int side = noise_size / 2; side >= 1; side /= 2)
        {
            const std::vector<std::uint8_t> &finer = levels.back();
            std::vector<std::uint8_t> coarser(static_cast<std::size_t>(side) * side * 4U);
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        unsigned total{};
                        for (int tap = 0; tap < 4; ++tap)
                            total += finer[(static_cast<std::size_t>(y * 2 + (tap >> 1)) *
                                                static_cast<std::size_t>(side * 2) +
                                            static_cast<std::size_t>(x * 2 + (tap & 1))) *
                                               4U +
                                           static_cast<std::size_t>(channel)];
                        coarser[(static_cast<std::size_t>(y) * side + x) * 4U +
                                static_cast<std::size_t>(channel)] =
                            static_cast<std::uint8_t>((total + 2U) / 4U);
                    }
            levels.push_back(std::move(coarser));
        }
        D3D11_TEXTURE2D_DESC noise{};
        noise.Width = noise_size;
        noise.Height = noise_size;
        noise.MipLevels = static_cast<UINT>(levels.size());
        noise.ArraySize = 1U;
        noise.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        noise.SampleDesc.Count = 1U;
        noise.Usage = D3D11_USAGE_IMMUTABLE;
        noise.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> noise_data;
        int side = noise_size;
        for (const auto &level : levels)
        {
            noise_data.push_back({level.data(), static_cast<UINT>(side * 4), 0U});
            side = std::max(1, side / 2);
        }
        ComPtr<ID3D11Texture2D> noise_texture;
        if (FAILED(device_->CreateTexture2D(&noise, noise_data.data(), &noise_texture)) ||
            FAILED(device_->CreateShaderResourceView(noise_texture.Get(), nullptr, &noise_view_)))
            return fail("noise texture");
        return true;
    }

    // Copies the chunk-sized blocks of the map that changed. A block is 16x16
    // texels of level 0 and its share of the coarser levels, so a chunk
    // streaming in costs a few hundred texels, never the whole map.
    void upload_map()
    {
        for (const std::array<int, 2> &slot : lava_.take_dirty_slots())
            if (std::find(pending_slots_.begin(), pending_slots_.end(), slot) ==
                pending_slots_.end())
                pending_slots_.push_back(slot);
        if (pending_slots_.empty() || map_texture_ == nullptr)
            return;
        // Walking into a new chunk brings a whole row of chunks into the map
        // at once. They arrive at its edge, where the map has faded out, so
        // nothing shows if they are copied over a few frames: nearest first, a
        // handful a frame. A bulk change (a world loading) is copied at once.
        constexpr std::size_t slots_per_frame = 8U;
        constexpr std::size_t bulk = 64U;
        std::vector<std::array<int, 2>> slots;
        if (pending_slots_.size() > bulk)
        {
            slots.swap(pending_slots_);
        }
        else
        {
            const int centre_x = static_cast<int>(std::floor(
                lava_.center_x() / static_cast<float>(world::lava_field_chunk)));
            const int centre_z = static_cast<int>(std::floor(
                lava_.center_z() / static_cast<float>(world::lava_field_chunk)));
            const auto distance = [&](const std::array<int, 2> &slot) {
                // Slots wrap: the nearest image of the slot to the centre chunk.
                const auto wrapped = [](int slot_index, int centre) {
                    const int half = world::lava_field_slots / 2;
                    int delta = (slot_index - centre) % world::lava_field_slots;
                    if (delta < -half)
                        delta += world::lava_field_slots;
                    if (delta >= half)
                        delta -= world::lava_field_slots;
                    return std::abs(delta);
                };
                return std::max(wrapped(slot[0], centre_x), wrapped(slot[1], centre_z));
            };
            std::sort(pending_slots_.begin(), pending_slots_.end(),
                      [&](const std::array<int, 2> &left, const std::array<int, 2> &right) {
                          return distance(left) < distance(right);
                      });
            const std::size_t count = std::min(pending_slots_.size(), slots_per_frame);
            slots.assign(pending_slots_.begin(),
                         pending_slots_.begin() + static_cast<std::ptrdiff_t>(count));
            pending_slots_.erase(pending_slots_.begin(),
                                 pending_slots_.begin() + static_cast<std::ptrdiff_t>(count));
        }
        std::array<std::uint16_t, world::lava_field_chunk * world::lava_field_chunk * 4> scratch{};
        const auto copy = [&](int level, int origin_x, int origin_z, int span) {
            const auto &texels = lava_.level(level);
            const int side = world::lava_field_extent >> level;
            for (int z = 0; z < span; ++z)
                for (int x = 0; x < span; ++x)
                {
                    const world::LavaField::Texel &texel =
                        texels[static_cast<std::size_t>(origin_z + z) *
                                   static_cast<std::size_t>(side) +
                               static_cast<std::size_t>(origin_x + x)];
                    const std::size_t index = static_cast<std::size_t>(z * span + x) * 4U;
                    scratch[index] = to_half(texel.deep);
                    scratch[index + 1U] = to_half(texel.deep_height);
                    scratch[index + 2U] = to_half(texel.shallow);
                    scratch[index + 3U] = to_half(texel.shallow_height);
                }
            const D3D11_BOX box{static_cast<UINT>(origin_x),
                                static_cast<UINT>(origin_z),
                                0U,
                                static_cast<UINT>(origin_x + span),
                                static_cast<UINT>(origin_z + span),
                                1U};
            context_->UpdateSubresource(map_texture_.Get(), static_cast<UINT>(level), &box,
                                        scratch.data(), static_cast<UINT>(span * 8), 0U);
        };
        // Levels 0-4 hold a slot in 16, 8, 4, 2 and 1 texels a side.
        constexpr int shared_level = 5;
        for (const auto &slot : slots)
            for (int level = 0; level < shared_level; ++level)
                copy(level, (slot[0] * world::lava_field_chunk) >> level,
                     (slot[1] * world::lava_field_chunk) >> level,
                     world::lava_field_chunk >> level);
        // From level 5 on a texel holds several slots and a level is at most
        // 8x8: copied whole, once.
        for (int level = shared_level; level < world::lava_field_levels; ++level)
            copy(level, 0, 0, world::lava_field_extent >> level);
    }

    void set_viewport(int width, int height)
    {
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
        context_->RSSetViewports(1U, &viewport);
    }

    ID3D11Device *device_{};
    ID3D11DeviceContext *context_{};
    std::string failure_;
    bool ready_{};
    bool disabled_{};
    world::LavaField lava_;
    // Chunk slots of the map that changed and are not on the GPU yet.
    std::vector<std::array<int, 2>> pending_slots_;
    HellFrameState state_{};
    float pulse_{1.0F};
    UINT ember_count_{};

    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11Texture2D> map_texture_;
    ComPtr<ID3D11ShaderResourceView> map_view_;
    ComPtr<ID3D11ShaderResourceView> noise_view_;
    ComPtr<ID3D11SamplerState> map_sampler_;
    ComPtr<ID3D11SamplerState> noise_sampler_;
    ComPtr<ID3D11SamplerState> clamp_sampler_;
    ComPtr<ID3D11VertexShader> screen_vertex_shader_;
    ComPtr<ID3D11PixelShader> air_pixel_shader_;
    ComPtr<ID3D11VertexShader> ember_vertex_shader_;
    ComPtr<ID3D11PixelShader> ember_pixel_shader_;
    ComPtr<ID3D11DepthStencilState> no_depth_state_;
    ComPtr<ID3D11DepthStencilState> test_depth_state_;
    ComPtr<ID3D11BlendState> opaque_blend_;
    ComPtr<ID3D11BlendState> ember_blend_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
};

HellAtmosphere::HellAtmosphere(ID3D11Device *device, ID3D11DeviceContext *context)
    : impl_(std::make_unique<Impl>(device, context))
{
}

HellAtmosphere::~HellAtmosphere() = default;

bool HellAtmosphere::ready() const noexcept
{
    return impl_->ready();
}

const std::string &HellAtmosphere::shader_library()
{
    static const std::string library = build_library();
    return library;
}

world::LavaField &HellAtmosphere::lava() noexcept
{
    return impl_->lava();
}

HellFrameState HellAtmosphere::begin_frame(const HellFrameInput &input)
{
    return impl_->begin_frame(input);
}

const HellFrameState &HellAtmosphere::state() const noexcept
{
    return impl_->state();
}

void HellAtmosphere::bind()
{
    impl_->bind();
}

void HellAtmosphere::unbind()
{
    impl_->unbind();
}

void HellAtmosphere::draw_air(ID3D11ShaderResourceView *scene, ID3D11RenderTargetView *target,
                              ID3D11ShaderResourceView *depth, ID3D11Buffer *scene_constants,
                              int width, int height)
{
    impl_->draw_air(scene, target, depth, scene_constants, width, height);
}

void HellAtmosphere::draw_embers(ID3D11RenderTargetView *target,
                                 ID3D11DepthStencilView *depth_readonly,
                                 ID3D11Buffer *scene_constants, int width, int height)
{
    impl_->draw_embers(target, depth_readonly, scene_constants, width, height);
}

float HellAtmosphere::lava_light_at(float x, float y, float z, float normal_y) const noexcept
{
    return impl_->lava_light_at(x, y, z, normal_y);
}

float HellAtmosphere::pulse() const noexcept
{
    return impl_->pulse();
}

#endif

} // namespace openminerz
