#include "post_processing.hpp"

#include "atmosphere_shader.hpp"
#include "hell_atmosphere.hpp"
#include "openminerz/post_math.hpp"
#include "vfx/vfx_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "shader_cache.hpp"
#endif

namespace openminerz
{

// OPENMINERZ_POSTFX="name=value,name=value". Diagnostic only: lets one binary
// measure and capture each effect with and without the others. Names:
//   chain   0 falls back to the renderer's original post chain
//   bloom   0|1            bloom_strength  number
//   ao      0..4 quality   contact 0|1
//   vol     0..4 quality   clouds  0..4
//   dof     0 off, 1 aiming, 2 always     dof_strength 0..1
//   mb      0..1           fxaa 0|1       sharpen 0..1    flare 0|1
//   contrast, saturation, temperature, vignette, grain, ca   numbers
//   softness 0..1          detail -1..1
void apply_post_effect_overrides(PostEffects &effects)
{
    const char *text = SDL_getenv("OPENMINERZ_POSTFX");
    if (text == nullptr || *text == '\0')
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
        const std::string_view name = entry.substr(0U, equals);
        const float value =
            static_cast<float>(std::atof(std::string{entry.substr(equals + 1U)}.c_str()));
        const int level = std::clamp(static_cast<int>(std::lround(value)), 0, 4);
        if (name == "chain")
            effects.enabled = effects.enabled && value > 0.5F;
        else if (name == "bloom")
            effects.bloom = value > 0.5F;
        else if (name == "bloom_strength")
            effects.bloom_strength = value;
        else if (name == "ao")
        {
            constexpr std::array<std::array<int, 2>, 5> table{
                {{0, 0}, {1, 3}, {2, 3}, {2, 5}, {4, 6}}};
            effects.ao_slices = table[static_cast<std::size_t>(level)][0];
            effects.ao_steps = table[static_cast<std::size_t>(level)][1];
        }
        else if (name == "contact")
            effects.contact_shadows = value > 0.5F;
        else if (name == "vol")
        {
            constexpr std::array<int, 5> steps{0, 6, 10, 16, 32};
            effects.volumetric_steps = steps[static_cast<std::size_t>(level)];
        }
        else if (name == "clouds")
            effects.cloud_quality = level;
        else if (name == "dof")
            effects.depth_of_field = static_cast<DepthOfFieldMode>(std::min(level, 2));
        else if (name == "dof_strength")
            effects.depth_of_field_strength = value;
        else if (name == "mb")
            effects.motion_blur = value;
        else if (name == "fxaa")
            effects.fxaa = value > 0.5F;
        else if (name == "sharpen")
            effects.sharpening = value;
        else if (name == "flare")
            effects.lens_flare = value > 0.5F;
        else if (name == "contrast")
            effects.contrast = value;
        else if (name == "saturation")
            effects.saturation = value;
        else if (name == "temperature")
            effects.temperature = value;
        else if (name == "vignette")
            effects.vignette = value;
        else if (name == "grain")
            effects.film_grain = value;
        else if (name == "ca")
            effects.chromatic_aberration = value;
        else if (name == "softness")
            effects.shadow_softness = value;
        else if (name == "detail")
            effects.texture_detail = value;
        // The deep underground (hell_atmosphere.hpp): deep = light strength
        // (0 falls back to the older flat fill), air = steps of the glow march
        // (0 skips the pass), haze 0|1, embers = how many.
        else if (name == "deep")
            effects.deep_light = value;
        else if (name == "air")
            effects.deep_air_steps = static_cast<int>(std::lround(value));
        else if (name == "haze")
            effects.heat_haze = value > 0.5F;
        else if (name == "embers")
            effects.ember_count = static_cast<int>(std::lround(value));
    }
}

#if defined(_WIN32)
namespace
{
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Shared shader text. Every pass is a full-screen triangle.
// ---------------------------------------------------------------------------
constexpr char vertex_source[] = R"hlsl(
struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PostPixelInput main(uint vertex_id : SV_VertexID)
{
    PostPixelInput output;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    // z = w: the triangle sits exactly on the far plane, so a LESS_EQUAL depth
    // test passes only where nothing opaque was drawn (the sky pass).
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    output.uv = uv;
    return output;
}
)hlsl";

constexpr char common_source[] = R"hlsl(
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
    float4 SunAndOptions;
    float4 RenderOptions;
    float4 TorchAndEye;
    float4 AmbientSun;
    float4 ShadowX;
    float4 ShadowY;
    float4 ShadowZ;
    float4 ShadowParams;
    float4 PostSun;
    float4 PostParams;
    float4 TargetSize;
    float4 GradeParams;
    float4 ShadowX2;
    float4 ShadowY2;
    float4 ShadowZ2;
    float4 ShadowX3;
    float4 ShadowY3;
    float4 ShadowZ3;
    float4 ShadowCascades;
    float4 DynamicRadianceOrigin;
    float4 DynamicRadianceParams;
    float4 BiomeTint;
    float4 BiomeParams;
};

cbuffer PerPass : register(b1)
{
    float4 PassA;
    float4 PassB;
};

cbuffer Post : register(b2)
{
    float4 PostSize;       // xy = scene pixels, zw = 1 / pixels
    float4 PostTime;       // x = seconds, y = frame seconds, z = frame index, w = reduced motion
    float4 PrevEye;
    float4 PrevRight;
    float4 PrevUp;
    float4 PrevForward;
    float4 PrevProjection;
    float4 SkyZenith;      // rgb = linear zenith, w = daylight
    float4 SkyGround;      // rgb = unused, w = storminess
    float4 SkyParams;      // x = hell, y = lightning, z = cloud quality, w = painted sky weight
    float4 SkyMoon;        // xyz = moon direction, w = night
    float4 SkyWind;        // xy = cloud drift, z = cloud coverage, w = sun disc visibility
    float4 SkyPlates;      // x = second painted sky weight, y = painted skies bound, z = gain
    float4 AoParams;       // x = radius, y = strength, z = slices, w = steps
    float4 ContactParams;  // x = length, y = steps, z = strength, w = thickness
    float4 VolumeParams;   // x = steps, y = extinction, z = far, w = eye sky light
    float4 VolumeLight;    // x = sun gain, y = ambient gain, z = anisotropy, w = 1 shadows, 2 + leaves
    float4 FocusParams;    // x = lens constant, y = limit (share of height), z = taps, w = unused
    float4 BloomParams;    // x = mix, y = scatter, z = unused, w = sun glare
    float4 GradeUser;      // x = contrast, y = saturation, z = temperature, w = colour fringing
    float4 MotionParams;   // x = scale, y = taps, z = longest streak (uv), w = unused
    float4 SharpenParams;  // x = sharpening, yzw = unused
};

SamplerState LinearClamp : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState PointClamp : register(s2);
SamplerState LinearWrap : register(s3);

struct PostPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

static const float PI = 3.14159265f;

// Camera distance along the view axis for a depth-buffer value.
float linear_depth(float depth)
{
    return Projection.w / min(depth - Projection.z, -0.0000001f);
}

// World-space ray through a screen position; its component along the camera's
// forward axis is exactly one, so `Eye + ray * camera_depth` is the world point.
float3 camera_ray(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return CameraForward.xyz + CameraRight.xyz * (ndc.x / Projection.x) +
           CameraUp.xyz * (ndc.y / Projection.y);
}

// Camera-space position: x right, y up, z ahead.
float3 view_position(float2 uv, float camera_depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return float3(ndc.x / Projection.x, ndc.y / Projection.y, 1.0f) * camera_depth;
}

float2 view_to_uv(float3 position)
{
    return float2(0.5f + 0.5f * position.x * Projection.x / position.z,
                  0.5f - 0.5f * position.y * Projection.y / position.z);
}

// Jimenez' interleaved gradient noise: every 3x3 window holds nine evenly
// spread values, so a small blur turns the dither into extra samples.
float gradient_noise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 3: the sky.
// ---------------------------------------------------------------------------
// The parts of the sky that distant terrain fades into: shared by the sky pass
// and by the resolve pass, which re-aims the terrain's distance fog at the sky
// actually behind each pixel.
constexpr char sky_common_source[] = R"hlsl(
Texture2D PlateA0 : register(t4);
Texture2D PlateA1 : register(t5);
Texture2D PlateA2 : register(t6);
Texture2D PlateA3 : register(t7);
Texture2D PlateA4 : register(t8);
Texture2D PlateA5 : register(t9);
Texture2D PlateB0 : register(t10);
Texture2D PlateB1 : register(t11);
Texture2D PlateB2 : register(t12);
Texture2D PlateB3 : register(t13);
Texture2D PlateB4 : register(t14);
Texture2D PlateB5 : register(t15);

// The imported sky cube exactly as the Original presentation maps it.
void plate_face(float3 d, out int face, out float2 uv)
{
    float3 a = abs(d);
    if (a.z >= a.x && a.z >= a.y)
    {
        if (d.z < 0.0f) { face = 0; uv = float2(-d.x, -d.y) / a.z; }
        else            { face = 2; uv = float2(d.x, -d.y) / a.z; }
    }
    else if (a.x >= a.y)
    {
        if (d.x > 0.0f) { face = 1; uv = float2(-d.z, -d.y) / a.x; }
        else            { face = 3; uv = float2(d.z, -d.y) / a.x; }
    }
    else
    {
        if (d.y > 0.0f) { face = 4; uv = float2(d.x, d.z) / a.y; }
        else            { face = 5; uv = float2(d.x, -d.z) / a.y; }
    }
    uv = uv * 0.5f + 0.5f;
}

float3 plate_a(float3 d)
{
    int face;
    float2 uv;
    plate_face(d, face, uv);
    float3 color;
    if (face == 0)      color = PlateA0.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 1) color = PlateA1.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 2) color = PlateA2.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 3) color = PlateA3.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 4) color = PlateA4.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else                color = PlateA5.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    return color;
}

float3 plate_b(float3 d)
{
    int face;
    float2 uv;
    plate_face(d, face, uv);
    float3 color;
    if (face == 0)      color = PlateB0.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 1) color = PlateB1.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 2) color = PlateB2.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 3) color = PlateB3.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else if (face == 4) color = PlateB4.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    else                color = PlateB5.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    return color;
}

// Clear-sky gradient (deepest overhead, brightening toward the horizon) under
// the painted sky of the installed texture pack.
float3 sky_backdrop(float3 ray)
{
    float3 color = lerp(FogAndEnd.rgb, SkyZenith.rgb, pow(saturate(ray.y), 0.42f));
    if (SkyPlates.y > 0.5f && SkyParams.w > 0.001f)
    {
        float3 plate = plate_a(ray);
        if (SkyPlates.x > 0.001f)
            plate = lerp(plate, plate_b(ray), SkyPlates.x);
        // Display-referred artwork: decode it like every other texture.
        plate *= plate;
        color = lerp(color, plate * SkyPlates.z, SkyParams.w);
    }
    return color;
}

// Haze: the band of air along the horizon and, under it, the far ground lost
// in that air. `mist` is a slow noise in 0..1 (0.5 for none).
float3 sky_haze(float3 color, float3 ray, float mist)
{
    // Using the very colour the terrain fog uses is what lets a fogged ridge
    // dissolve into the sky behind it.
    float3 haze = FogAndEnd.rgb;
    float elevation = ray.y;
    float band = elevation > 0.045f ? saturate((0.34f - elevation) / 0.295f) : 1.0f;
    band = band * band * (3.0f - 2.0f * band);
    color = lerp(color, haze, band * 0.94f);
    if (elevation < 0.045f)
    {
        float depth = saturate((0.045f - elevation) / 0.75f);
        // The same air, deeper and a little cooler the further down one looks,
        // with slow banks of mist so it reads as distance rather than a wall.
        float3 ground = lerp(haze, haze * float3(0.46f, 0.52f, 0.60f), smoothstep(0.0f, 1.0f, depth));
        ground *= 1.0f - (mist - 0.5f) * 0.30f * smoothstep(0.02f, 0.30f, depth);
        color = lerp(color, ground, smoothstep(0.045f, -0.02f, elevation));
    }
    return color;
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 3: the sky.
// ---------------------------------------------------------------------------
constexpr char sky_source[] = R"hlsl(
Texture2D Noise : register(t0);

float hash21(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031f);
    q += dot(q, q.yzx + 33.33f);
    return frac((q.x + q.y) * q.z);
}

// Cloud cover at a point of the cloud deck. Two octaves of drifting shape and,
// from Medium up, a finer one that erodes the edges.
float cloud_cover(float2 p, float coverage, int quality)
{
    float2 drift = SkyWind.xy;
    float shape = Noise.SampleLevel(LinearWrap, p * 0.000072f + drift, 0.0f).r * 0.62f +
                  Noise.SampleLevel(LinearWrap, p * 0.00023f + drift * 2.6f, 0.0f).r * 0.38f;
    if (quality >= 2)
    {
        float fine = Noise.SampleLevel(LinearWrap, p * 0.00081f + drift * 6.5f, 0.0f).g;
        shape -= (1.0f - fine) * 0.085f;
        if (quality >= 4)
            shape -= (1.0f - Noise.SampleLevel(LinearWrap, p * 0.0029f + drift * 15.0f, 0.0f).g) *
                     0.035f;
    }
    float softness = quality >= 3 ? 0.13f : 0.18f;
    return smoothstep(0.0f, 1.0f, saturate((shape - (1.0f - coverage)) / softness));
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float3 ray = normalize(camera_ray(input.uv));
    float3 sun = normalize(SunAndOptions.xyz);
    float daylight = SkyZenith.w;
    float storm = SkyGround.w;
    float night = SkyMoon.w;
    float elevation = ray.y;
    float3 haze = FogAndEnd.rgb;
    float3 zenith = SkyZenith.rgb;
    float3 color = sky_backdrop(ray);
    float cos_sun = dot(ray, sun);
    float sun_up = saturate(sun.y * 6.0f + 0.35f);

    // Stars and the moon, behind the clouds.
    if (night > 0.01f && elevation > -0.05f)
    {
        float3 a = abs(ray);
        float major = max(a.x, max(a.y, a.z));
        float3 cube = ray / major;
        float2 plane = a.x >= major ? cube.yz : (a.y >= major ? cube.xz : cube.xy);
        float2 cell_position = plane * 150.0f + (a.x >= major ? 17.0f : (a.y >= major ? 53.0f : 91.0f));
        float2 cell = floor(cell_position);
        float seed = hash21(cell);
        if (seed > 0.90f)
        {
            float2 centre = float2(hash21(cell + 3.1f), hash21(cell + 7.7f)) * 0.6f + 0.2f;
            float distance_to_star = length(frac(cell_position) - centre);
            float magnitude = pow((seed - 0.90f) * 10.0f, 3.0f);
            float size = 0.11f + magnitude * 0.10f;
            float twinkle = PostTime.w > 0.5f
                                ? 1.0f
                                : 0.80f + 0.20f * sin(PostTime.x * (1.3f + seed * 3.0f) + seed * 80.0f);
            float glow = saturate(1.0f - distance_to_star / size);
            float3 tint = lerp(float3(1.0f, 0.86f, 0.72f), float3(0.72f, 0.84f, 1.0f),
                               hash21(cell + 11.3f));
            color += tint * glow * glow * (0.35f + magnitude * 3.2f) * twinkle * night *
                     saturate(elevation * 5.0f + 0.25f);
        }
    }
    float3 moon_direction = SkyMoon.xyz;
    float cos_moon = dot(ray, moon_direction);
    if (night > 0.01f && cos_moon > 0.985f)
    {
        // A lit sphere: the limb darkens and the terminator follows the sun.
        const float moon_radius = 0.030f;
        float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), moon_direction));
        float3 up = cross(moon_direction, right);
        float2 disc = float2(dot(ray, right), dot(ray, up)) / moon_radius;
        float r2 = dot(disc, disc);
        if (r2 < 1.0f)
        {
            float3 normal = float3(disc, -sqrt(1.0f - r2));
            float3 light = normalize(float3(0.55f, 0.35f, -0.75f));
            float lit = saturate(dot(normal, light) * 1.6f + 0.25f);
            float maria = Noise.SampleLevel(LinearWrap, disc * 0.11f + 0.37f, 0.0f).r;
            float3 surface = float3(0.92f, 0.94f, 1.0f) * (0.62f + 0.38f * smoothstep(0.35f, 0.7f, maria));
            float edge = smoothstep(1.0f, 0.90f, r2);
            color = lerp(color, surface * lit * 2.6f, edge * night);
        }
        float halo = pow(saturate((cos_moon - 0.985f) / 0.015f), 3.0f);
        color += float3(0.40f, 0.50f, 0.72f) * halo * 0.22f * night;
    }

    // Drifting cloud deck.
    int quality = (int)(SkyParams.z + 0.5f);
    float cover = 0.0f;
    if (quality > 0 && elevation > 0.012f)
    {
        // A flat deck seen from below, bent down toward the horizon so it
        // recedes instead of stretching to infinity.
        const float deck_height = 1450.0f;
        float reach = deck_height / (elevation + 0.085f);
        float2 p = Eye.xz + ray.xz * reach;
        float coverage = SkyWind.z;
        cover = cloud_cover(p, coverage, quality);
        if (cover > 0.002f)
        {
            float3 light_direction = sun.y > -0.06f ? sun : moon_direction;
            float2 toward = normalize(light_direction.xz + 0.0001f);
            float toward_light = cloud_cover(p + toward * 310.0f, coverage, min(quality, 2));
            float self_shadow = exp(-toward_light * 2.1f) * (1.0f - cover * 0.42f);
            if (quality >= 3)
                self_shadow *= exp(-cloud_cover(p + toward * 760.0f, coverage, 1) * 0.9f);
            float cos_light = dot(ray, light_direction);
            // Thin edges facing the light glow; the body shades itself.
            float rim = pow(saturate(cos_light), 10.0f) * (1.0f - cover) * 2.2f;
            float3 direct_color = sun.y > -0.06f
                                      ? LightAndFogStart.rgb * (0.30f + 0.70f * sun_up)
                                      : float3(0.030f, 0.042f, 0.070f) * night;
            // Heavy weather: the sun reaches the underside through a thick deck.
            float thickness = lerp(1.0f, 0.34f, storm);
            float3 lit = direct_color * (self_shadow * 1.05f + rim) * thickness;
            float3 sky_fill = lerp(haze, zenith, 0.55f) * lerp(1.15f, 0.62f, storm) +
                              AmbientSun.rgb * 0.16f;
            float3 cloud = lit + sky_fill;
            // Distance swallows the deck: it fades into the haze near the horizon.
            float fade = smoothstep(0.012f, 0.26f, elevation);
            color = lerp(color, cloud, cover * fade);
            cover *= fade;
        }
    }

    // The sun: a hot disc the bloom spreads, and the air glowing around it.
    if (sun.y > -0.12f && SkyParams.x < 0.999f)
    {
        float visibility = SkyWind.w * (1.0f - cover * 0.94f) * smoothstep(-0.10f, 0.02f, sun.y);
        float3 sun_color = LightAndFogStart.rgb / max(max(LightAndFogStart.r, LightAndFogStart.g),
                                                      max(LightAndFogStart.b, 0.05f));
        float disc = smoothstep(0.99975f, 0.99990f, cos_sun);
        color += sun_color * disc * 30.0f * visibility;
        // A low sun shines through far more air, so its glow spreads wider.
        float low = 1.0f - saturate(sun.y * 3.5f);
        float aureole = pow(saturate(cos_sun), 420.0f) * 1.6f +
                        pow(saturate(cos_sun), 48.0f) * (0.24f + 0.36f * low) +
                        pow(saturate(cos_sun), 6.0f) * (0.07f + 0.11f * low);
        // (No aureole while this light is the moon, which has its own halo.)
        color += sun_color * aureole * (0.25f + 0.75f * visibility) * lerp(1.0f, 0.42f, storm) *
                 (0.35f + 0.65f * daylight) * step(0.0001f, SkyWind.w);
    }

    // Haze: the band the terrain fades into, and under the horizon the far
    // ground lost in it.
    float mist = 0.5f;
    if (elevation < 0.045f)
    {
        float2 q = ray.xz / max(0.06f - elevation, 0.06f);
        mist = Noise.SampleLevel(LinearWrap, q * 0.035f + SkyWind.xy * 3.0f, 0.0f).r;
    }
    color = sky_haze(color, ray, mist);

    // Lightning lights the deck from inside.
    color += float3(0.42f, 0.52f, 0.78f) * SkyParams.y * (0.22f + 0.55f * cover + 0.25f * storm);
    // Underground there is no sky, only the cavern's own air.
    color = lerp(color, haze, SkyParams.x);
    return float4(max(color, 0.0f), 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 4: ground-truth ambient occlusion at half resolution.
// ---------------------------------------------------------------------------
constexpr char ao_source[] = R"hlsl(
Texture2D<float> Depth : register(t0);
Texture2D Surface0 : register(t1);

float fast_acos(float x)
{
    float ax = abs(x);
    float result = (-0.156583f * ax + 1.570796f) * sqrt(1.0f - ax);
    return x >= 0.0f ? result : PI - result;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    int2 full = pixel * 2;
    float depth = Depth.Load(int3(full, 0));
    if (depth >= 0.999999f)
        return 1.0f.xxxx;
    float2 uv = (float2(full) + 0.5f) * PostSize.zw;
    float camera_depth = linear_depth(depth);
    float3 position = view_position(uv, camera_depth);
    float3 world_normal = normalize(Surface0.Load(int3(full, 0)).xyz * 2.0f - 1.0f);
    float3 normal = float3(dot(world_normal, CameraRight.xyz), dot(world_normal, CameraUp.xyz),
                           dot(world_normal, CameraForward.xyz));
    float3 view = normalize(-position);
    // Start just off the surface so a flat wall never occludes itself.
    position += normal * (0.004f + camera_depth * 0.0012f);

    float radius = AoParams.x;
    // Radius on screen, in pixels of the full-resolution depth it samples.
    float radius_pixels = radius * Projection.y * 0.5f / camera_depth * PostSize.y;
    if (radius_pixels < 1.5f)
        return 1.0f.xxxx;
    radius_pixels = min(radius_pixels, 110.0f);

    int slices = (int)(AoParams.z + 0.5f);
    int steps = (int)(AoParams.w + 0.5f);
    // 4x4 interleaved pattern: the blur pass that follows averages exactly one
    // period, so the result is as if every pixel had taken 16x the samples.
    float direction_noise = (float)((((pixel.x + pixel.y) & 3) << 2) + (pixel.x & 3)) / 16.0f;
    float step_noise = (float)((pixel.y - pixel.x) & 3) * 0.25f;
    float falloff_start = radius * 0.35f;
    float falloff_range = radius - falloff_start;

    float visibility = 0.0f;
    [loop]
    for (int slice = 0; slice < slices; ++slice)
    {
        float phi = ((float)slice + direction_noise) * PI / (float)slices;
        float2 omega = float2(cos(phi), sin(phi));
        float3 direction = float3(omega.x, omega.y, 0.0f);
        float3 ortho = direction - dot(direction, view) * view;
        float3 axis = normalize(cross(ortho, view));
        float3 projected = normal - axis * dot(normal, axis);
        float projected_length = max(length(projected), 0.0001f);
        float sign_normal = sign(dot(ortho, projected));
        float cos_normal = saturate(dot(projected, view) / projected_length);
        float n = sign_normal * fast_acos(cos_normal);
        float low_cos0 = cos(n + PI * 0.5f);
        float low_cos1 = cos(n - PI * 0.5f);
        float horizon0 = low_cos0;
        float horizon1 = low_cos1;
        // Screen y points down while the view-space y points up.
        float2 screen_step = float2(omega.x, -omega.y) * PostSize.zw * radius_pixels;
        [loop]
        for (int step = 0; step < steps; ++step)
        {
            float s = ((float)step + step_noise + 0.35f) / (float)steps;
            s *= s;
            float2 offset = screen_step * max(s, 1.2f / radius_pixels);

            float2 uv0 = uv + offset;
            float3 delta0 = view_position(uv0, linear_depth(Depth.SampleLevel(PointClamp, uv0, 0.0f))) -
                            position;
            float distance0 = length(delta0);
            float weight0 = saturate(1.0f - (distance0 - falloff_start) / falloff_range);
            horizon0 = max(horizon0, lerp(low_cos0, dot(delta0 / max(distance0, 0.0001f), view), weight0));

            float2 uv1 = uv - offset;
            float3 delta1 = view_position(uv1, linear_depth(Depth.SampleLevel(PointClamp, uv1, 0.0f))) -
                            position;
            float distance1 = length(delta1);
            float weight1 = saturate(1.0f - (distance1 - falloff_start) / falloff_range);
            horizon1 = max(horizon1, lerp(low_cos1, dot(delta1 / max(distance1, 0.0001f), view), weight1));
        }
        float h0 = -fast_acos(clamp(horizon1, -1.0f, 1.0f));
        float h1 = fast_acos(clamp(horizon0, -1.0f, 1.0f));
        h0 = n + clamp(h0 - n, -PI * 0.5f, PI * 0.5f);
        h1 = n + clamp(h1 - n, -PI * 0.5f, PI * 0.5f);
        float sin_n = sin(n);
        float arc0 = (cos_normal + 2.0f * h0 * sin_n - cos(2.0f * h0 - n)) * 0.25f;
        float arc1 = (cos_normal + 2.0f * h1 * sin_n - cos(2.0f * h1 - n)) * 0.25f;
        visibility += projected_length * (arc0 + arc1);
    }
    visibility = saturate(visibility / (float)slices);
    // Far away a block is a pixel: fade the term out rather than let it crawl.
    float distance_fade = saturate((camera_depth - 70.0f) / 50.0f);
    return lerp(visibility, 1.0f, distance_fade).xxxx;
}
)hlsl";

// Depth-aware 4x4 blur shared by the half-resolution passes. With VOLUME
// defined it filters four channels over 3x3 (the fog's dither period).
constexpr char half_blur_source[] = R"hlsl(
Texture2D Source : register(t0);
Texture2D<float> Depth : register(t1);

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float centre = linear_depth(Depth.Load(int3(pixel * 2, 0)));
    float4 total = 0.0f.xxxx;
    float weight_total = 0.0f;
#if defined(VOLUME)
    const int first = -1;
    const int last = 1;
#else
    const int first = -2;
    const int last = 1;
#endif
    [unroll]
    for (int y = first; y <= last; ++y)
    {
        [unroll]
        for (int x = first; x <= last; ++x)
        {
            int2 tap = pixel + int2(x, y);
            float tap_depth = linear_depth(Depth.Load(int3(tap * 2, 0)));
            float difference = abs(tap_depth - centre) / max(centre, 0.01f);
            float weight = saturate(1.0f - difference * PassA.x);
            weight = weight * weight + 0.0005f;
            total += Source.Load(int3(tap, 0)) * weight;
            weight_total += weight;
        }
    }
    return total / weight_total;
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 5: shadow in the air. The air itself is the one atmosphere of
// atmosphere_shader.hpp, which pass 2b has already laid over every pixel as if
// the sun reached all of it; this pass marches the same field along each
// pixel's ray and returns the sunlight that terrain shadows keep out of it,
// to be taken back off. That is what a light shaft is: lit air beside
// shadowed air. (Compiled with atmosphere_hlsl in front of it.)
// ---------------------------------------------------------------------------
constexpr char volume_source[] = R"hlsl(
Texture2D<float> Depth : register(t0);
Texture2D Surface1 : register(t1);
Texture2DArray ShadowMap : register(t2);
Texture2DArray ShadowTransmission : register(t3);
// The far landscape's shadow field and depth, as far_terrain_renderer.cpp
// leaves them bound (bind_near_shadow, draw); all zero without far terrain.
cbuffer FarShadow : register(b7)
{
    float4 FarShadowField[2];
    float4 FarShadowParams;
};
Texture2D<float2> FarShadowOlder : register(t22);
Texture2D<float2> FarShadowNewer : register(t23);
Texture2D<float> FarLandscapeDepth : register(t24);
SamplerState FarShadowSampler : register(s8);

float air_field_one(Texture2D<float2> field, float4 placement, float2 relative_xz, float height)
{
    float2 uv = (relative_xz + placement.xy) * placement.z;
    float2 margin = min(uv, 1.0f - uv);
    float inside = saturate(min(margin.x, margin.y) * 24.0f) * placement.w;
    // Heights are stored in 1/64 block above the world's floor.
    float top = field.SampleLevel(FarShadowSampler, uv, 0.0f).x * (65535.0f / 64.0f);
    return lerp(1.0f, saturate((height - top + 1.5f) / 6.0f), inside);
}

// How much of the sun reaches a point of the air `offset` from the eye, past
// the far landscape (mountains beyond the reach of the sun cascades).
float air_field_light(float3 offset)
{
    if (FarShadowParams.z < 0.5f)
        return 1.0f;
    float height = Eye.y + offset.y + 64.0f;
    float light = lerp(air_field_one(FarShadowOlder, FarShadowField[0], offset.xz, height),
                       air_field_one(FarShadowNewer, FarShadowField[1], offset.xz, height),
                       FarShadowParams.x);
    return lerp(1.0f, light, FarShadowParams.w);
}

float sun_visibility(float3 world, float camera_depth)
{
    if (camera_depth >= ShadowCascades.z)
        return 1.0f;
    float4 sx = ShadowX;
    float4 sy = ShadowY;
    float4 sz = ShadowZ;
    float cascade = 0.0f;
    if (camera_depth >= ShadowCascades.y)
    {
        sx = ShadowX3; sy = ShadowY3; sz = ShadowZ3; cascade = 2.0f;
    }
    else if (camera_depth >= ShadowCascades.x)
    {
        sx = ShadowX2; sy = ShadowY2; sz = ShadowZ2; cascade = 1.0f;
    }
    float2 uv = float2(dot(world, sx.xyz) + sx.w, dot(world, sy.xyz) + sy.w);
    if (any(uv < 0.002f) || any(uv > 0.998f))
        return 1.0f;
    float depth = dot(world, sz.xyz) + sz.w;
    float lit = ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(uv, cascade), depth - 0.0012f);
    // Ultra: leaves let a share of the sun through, in the air as on the
    // ground. Below that a canopy simply shades the air under it.
    if (VolumeLight.w > 1.5f)
        lit = lerp(lit, 1.0f,
                   ShadowTransmission.SampleLevel(LinearClamp, float3(uv, cascade), 0.0f).r);
    return lit;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    int2 full = pixel * 2;
    float2 uv = (float2(full) + 0.5f) * PostSize.zw;
    float depth = Depth.Load(int3(full, 0));
    bool is_sky = depth >= 0.999999f;
    float3 ray = camera_ray(uv);
    float ray_length = length(ray);
    float3 direction = ray / ray_length;

    int steps = (int)(VolumeParams.x + 0.5f);
    float jitter = gradient_noise(float2(pixel));
    // The path this pixel's light came along: from the surface the scene
    // holds, from the far landscape where there is one, else from beyond the
    // atmosphere. Camera depths; a distance is one times the ray's length.
    float surface_sky = is_sky ? 1.0f : Surface1.Load(int3(full, 0)).g;
    float exposure = atmosphere_exposure(VolumeParams.w, surface_sky);
    float path = is_sky ? 1.0e6f : linear_depth(depth);
    if (is_sky && AtmosEye.w > 0.0f)
    {
        float far_depth = FarLandscapeDepth.Load(int3(full, 0));
        if (far_depth > 0.0f)
            path = AtmosEye.w / far_depth;
    }
    bool shadows = VolumeLight.w > 0.5f;
    float cascades = max(ShadowCascades.z, 1.0f);

    // The share of the path's scattering that lies in shadow: x in the clear
    // air, y in mist. First through the reach of the sun cascades; toward
    // their end the far landscape's shadow field takes over from them.
    float2 lost = 0.0f.xx;
    float transmittance = 1.0f;
    float step_depth = min(path, cascades) / (float)steps;
    [loop]
    for (int index = 0; index < steps; ++index)
    {
        float camera_depth = ((float)index + jitter) * step_depth;
        float3 offset = ray * camera_depth;
        float2 density = atmosphere_density(offset, exposure);
        float total = density.x + density.y;
        float through = exp(-total * step_depth * ray_length);
        // Energy conserving: a step can only pass on what it did not absorb.
        float share = transmittance * (1.0f - through);
        float lit = shadows ? sun_visibility(Eye.xyz + offset, camera_depth) : 1.0f;
        float beyond = smoothstep(0.70f, 1.0f, camera_depth / cascades);
        if (shadows && beyond > 0.0f)
            lit = lerp(lit, air_field_light(offset), beyond);
        lost += share * (1.0f - lit) * density / max(total, 1.0e-8f);
        transmittance *= through;
    }
    // Then on through the far landscape's own shadows, as far as its fine
    // field reaches. Lit air further along only dilutes what was lost, so a
    // shaft thins with distance and never stops at an edge.
    float reach = min(path, 1000.0f);
    [branch]
    if (shadows && FarShadowParams.z > 0.5f && reach > cascades)
    {
        const int far_steps = 6;
        float far_step = (reach - cascades) / (float)far_steps;
        [unroll]
        for (int far_index = 0; far_index < far_steps; ++far_index)
        {
            float3 offset = ray * (cascades + ((float)far_index + jitter) * far_step);
            float2 density = atmosphere_density(offset, exposure);
            float total = density.x + density.y;
            float through = exp(-total * far_step * ray_length);
            float share = transmittance * (1.0f - through);
            lost += share * (1.0f - air_field_light(offset)) * density / max(total, 1.0e-8f);
            transmittance *= through;
        }
    }
    // What that takes from the light pass 2b added to this pixel (negative).
    float3 sunlit_air;
    float3 sunlit_mist;
    atmosphere_light(direction, 0.0f, FogAndEnd.rgb, sunlit_air);
    atmosphere_light(direction, 1.0f, FogAndEnd.rgb, sunlit_mist);
    return float4(-(sunlit_air * lost.x + sunlit_mist * lost.y), 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 6: apply occlusion, contact shadows and the air to the lit scene.
// ---------------------------------------------------------------------------
constexpr char resolve_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D Surface0 : register(t2);
Texture2D Surface1 : register(t3);
// (t4-t15 are the painted sky, shared with the sky pass.)
Texture2D Occlusion : register(t16);
Texture2D Volume : register(t17);

// The four half-resolution texels around a full-resolution pixel, weighted by
// how close their depth is to this pixel's: a soft effect never bleeds across
// a silhouette.
void half_taps(int2 pixel, float camera_depth, out int2 taps[4], out float weights[4])
{
    int2 base = pixel / 2;
    int2 side = (pixel & 1) * 2 - 1;
    taps[0] = base;
    taps[1] = base + int2(side.x, 0);
    taps[2] = base + int2(0, side.y);
    taps[3] = base + side;
    const float bilinear[4] = {0.5625f, 0.1875f, 0.1875f, 0.0625f};
    float total = 0.0f;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        float tap_depth = linear_depth(Depth.Load(int3(taps[index] * 2, 0)));
        float difference = abs(tap_depth - camera_depth) / max(camera_depth, 0.01f);
        weights[index] = bilinear[index] / (0.0008f + difference * difference * 900.0f);
        total += weights[index];
    }
    [unroll]
    for (int normalise = 0; normalise < 4; ++normalise)
        weights[normalise] /= total;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float3 color = Scene.Load(int3(pixel, 0)).rgb;
    float depth = Depth.Load(int3(pixel, 0));
    bool is_sky = depth >= 0.999999f;
    float camera_depth = linear_depth(depth);
    int2 taps[4];
    float weights[4];
    half_taps(pixel, camera_depth, taps, weights);

    float debug_occlusion = 1.0f;
    float debug_contact = 0.0f;
    if (!is_sky)
    {
        float4 surface0 = Surface0.Load(int3(pixel, 0));
        float4 surface1 = Surface1.Load(int3(pixel, 0));
        float baked = surface0.a;
        float ambient_share = surface1.r;
        if (PassA.x > 3.5f && PassA.x < 4.5f)
            return float4(surface0.xyz, 1.0f);
        if (PassA.x > 4.5f && PassA.x < 5.5f)
            return float4(ambient_share, baked, surface1.g, 1.0f);
        // The atmosphere (pass 2b) is already on this pixel. What shades the
        // surface must not shade the air in front of it, or leaves stand as
        // dark blobs in a fog that has whitened the ground beside them: both
        // terms below act only on the share of the pixel that is still the
        // surface, its transmittance.
        float surface_share = 1.0f;
        if (PassA.y > 0.5f)
        {
            float3 air_ray = camera_ray(input.uv);
            float2 air = atmosphere_path(normalize(air_ray), camera_depth * length(air_ray),
                                         atmosphere_exposure(VolumeParams.w, surface1.g));
            surface_share = exp(-(air.x + air.y));
        }
        // How much of the term this pixel takes: none where it glows or is
        // lost in fog, half on leaves, all on an ordinary surface.
        float occludable = surface1.a * surface_share;
        if (AoParams.z > 0.5f && occludable > 0.01f)
        {
            float occlusion = 0.0f;
            [unroll]
            for (int index = 0; index < 4; ++index)
                occlusion += Occlusion.Load(int3(taps[index], 0)).r * weights[index];
            debug_occlusion = occlusion;
            occlusion = lerp(1.0f, pow(saturate(occlusion), 1.35f), AoParams.y);
            // The baked corner shading already darkened this pixel by `baked`.
            // Together they may only reach the darker of the two.
            float applied = max(baked, 0.05f);
            float residual = saturate(min(applied, occlusion) / applied);
            // Occlusion is about light from every direction; the sun is one
            // direction and has its own shadows.
            color *= lerp(1.0f, residual, occludable * lerp(0.30f, 1.0f, ambient_share));
        }
        float direct_share = 1.0f - ambient_share;
        if (ContactParams.y > 0.5f && direct_share > 0.04f && camera_depth < 72.0f)
        {
            float3 sun = normalize(SunAndOptions.xyz);
            float3 light = float3(dot(sun, CameraRight.xyz), dot(sun, CameraUp.xyz),
                                  dot(sun, CameraForward.xyz));
            float3 world_normal = normalize(surface0.xyz * 2.0f - 1.0f);
            float3 normal = float3(dot(world_normal, CameraRight.xyz),
                                   dot(world_normal, CameraUp.xyz),
                                   dot(world_normal, CameraForward.xyz));
            float facing = dot(world_normal, sun);
            if (facing > 0.02f)
            {
                float3 origin = view_position(input.uv, camera_depth) +
                                normal * (0.012f + camera_depth * 0.0030f);
                int steps = (int)(ContactParams.y + 0.5f);
                // Long enough to cross a few pixels at any distance.
                float reach = ContactParams.x * (1.0f + camera_depth * 0.045f);
                float jitter = gradient_noise(input.position.xy);
                float thickness = ContactParams.w * (1.0f + camera_depth * 0.03f);
                float shadow = 0.0f;
                [loop]
                for (int step = 0; step < steps; ++step)
                {
                    float travel = ((float)step + jitter) / (float)steps;
                    float3 point_on_ray = origin + light * (reach * travel);
                    if (point_on_ray.z <= 0.07f)
                        break;
                    float2 tap_uv = view_to_uv(point_on_ray);
                    if (any(tap_uv < 0.0f) || any(tap_uv > 1.0f))
                        break;
                    float scene_depth = linear_depth(Depth.SampleLevel(PointClamp, tap_uv, 0.0f));
                    float behind = point_on_ray.z - scene_depth;
                    if (behind > 0.004f + camera_depth * 0.0012f && behind < thickness)
                    {
                        shadow = 1.0f - travel * travel * 0.7f;
                        break;
                    }
                }
                float distance_fade = 1.0f - saturate((camera_depth - 48.0f) / 24.0f);
                debug_contact = shadow * distance_fade;
                color *= 1.0f - shadow * direct_share * ContactParams.z * distance_fade * surface_share;
            }
        }
    }
    // Diagnostic views (OPENMINERZ_POSTFX_DEBUG): what each term alone holds.
    if (PassA.x > 0.5f && PassA.x < 1.5f)
        return float4(debug_occlusion.xxx, 1.0f);
    if (PassA.x > 1.5f && PassA.x < 2.5f)
        return float4((1.0f - debug_contact).xxx, 1.0f);
    if (!is_sky)
    {
        // The lit pass faded this pixel toward one flat fog colour. Re-aim
        // that same amount of fog at the sky actually behind the pixel, so a
        // far ridge dissolves into the sky instead of standing in front of it
        // as a pale cut-out. Same distances and curve as the lit pass.
        float3 ray = camera_ray(input.uv);
        float horizontal = length(ray.xz) * camera_depth;
        float fog = saturate((horizontal - LightAndFogStart.w) /
                             max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
        fog = fog * fog * (3.0f - 2.0f * fog);
        float hell_depth = Eye.w * smoothstep(24.0f, 36.0f, -Eye.y);
        fog = max(fog, hell_depth * (1.0f - exp(-horizontal * 0.025f)) * 0.38f);
        // Entities cap their fog, as the original never fogged the dragon.
        if (Surface1.Load(int3(pixel, 0)).b > 0.5f)
            fog = min(fog, 0.55f);
        // (PassA.y: pass 2b has laid the atmosphere over this frame. The lit
        // pass's fade is then no fog at all above ground, and nothing is
        // re-aimed here.)
        if (fog > 0.004f && SkyParams.x < 0.999f && PassA.y < 0.5f)
        {
            float3 direction = normalize(ray);
            float3 behind = sky_haze(sky_backdrop(direction), direction, 0.5f);
            behind = lerp(behind, FogAndEnd.rgb, SkyParams.x);
            // Not quite all the way: a fully fogged ridge keeps a faint outline
            // instead of vanishing, which is what gives the distance its depth.
            color = max(color + fog * 0.72f * (behind - FogAndEnd.rgb), 0.0f);
        }
    }
    if (VolumeParams.x > 0.5f)
    {
        float4 volume = 0.0f.xxxx;
        [unroll]
        for (int index = 0; index < 4; ++index)
            volume += Volume.Load(int3(taps[index], 0)) * weights[index];
        // (volume.rgb is negative: the sunlight that shadows keep out of the
        // air, which pass 2b added as if the sun reached all of it. It can
        // dim a pixel by the air's own share of it, never blacken or tint it.)
        color = max(color + volume.rgb, color * 0.35f);
        if (PassA.x > 2.5f && PassA.x < 3.5f)
            return float4(volume.rgb * 4.0f + 0.0f, 1.0f);
    }
    return float4(color, 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 7: motion blur along the camera's and each creature's motion.
// ---------------------------------------------------------------------------
constexpr char motion_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D Velocity : register(t2);

// Screen-space motion of whatever is at `uv`, in uv per exposure.
float2 pixel_motion(float2 uv, float depth)
{
    float4 stored = Velocity.SampleLevel(PointClamp, uv, 0.0f);
    float2 motion;
    if (stored.a > 0.5f)
    {
        // A creature: its own motion was written when it was drawn.
        motion = stored.xy;
    }
    else
    {
        float3 world = Eye.xyz + camera_ray(uv) * linear_depth(depth);
        float3 relative = world - PrevEye.xyz;
        float previous_depth = max(dot(relative, PrevForward.xyz), 0.01f);
        float2 previous_uv = float2(
            0.5f + 0.5f * dot(relative, PrevRight.xyz) * PrevProjection.x / previous_depth,
            0.5f - 0.5f * dot(relative, PrevUp.xyz) * PrevProjection.y / previous_depth);
        motion = uv - previous_uv;
    }
    motion *= MotionParams.x;
    float size = length(motion);
    return size > MotionParams.z ? motion * (MotionParams.z / size) : motion;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 uv = input.uv;
    float depth = Depth.SampleLevel(PointClamp, uv, 0.0f);
    float2 motion = pixel_motion(uv, depth);
    float3 centre = Scene.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    float pixels = length(motion * PostSize.xy);
    if (pixels < 0.75f)
        return float4(centre, 1.0f);
    int taps = (int)(MotionParams.y + 0.5f);
    float jitter = gradient_noise(input.position.xy) - 0.5f;
    float centre_depth = linear_depth(depth);
    float3 total = centre;
    float weight_total = 1.0f;
    [loop]
    for (int tap = 0; tap < taps; ++tap)
    {
        float along = ((float)tap + 0.5f + jitter) / (float)taps - 0.5f;
        float2 tap_uv = saturate(uv + motion * along);
        float tap_raw = Depth.SampleLevel(PointClamp, tap_uv, 0.0f);
        float tap_depth = linear_depth(tap_raw);
        // A sharp thing in front of this pixel does not smear across it: a
        // nearer sample counts only if it is itself moving that far.
        float weight = 1.0f;
        if (tap_depth < centre_depth * 0.97f - 0.05f)
        {
            float tap_pixels = length(pixel_motion(tap_uv, tap_raw) * PostSize.xy);
            weight = saturate(tap_pixels / max(abs(along) * pixels, 0.5f));
        }
        total += Scene.SampleLevel(LinearClamp, tap_uv, 0.0f).rgb * weight;
        weight_total += weight;
    }
    return float4(total / weight_total, 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 8: depth of field.
// ---------------------------------------------------------------------------
// Autofocus: the focus distance, in dioptres, eased toward what the crosshair
// is on. A 1x1 target carried from frame to frame; no read-back to the CPU.
constexpr char focus_source[] = R"hlsl(
Texture2D<float> Depth : register(t0);
Texture2D<float> Previous : register(t1);

float4 main(PostPixelInput input) : SV_TARGET
{
    // The crosshair and a small ring around it, averaged in dioptres so a
    // creature in front of a far hillside wins the focus.
    const float2 ring[5] = {float2(0.0f, 0.0f), float2(0.012f, 0.0f), float2(-0.012f, 0.0f),
                            float2(0.0f, 0.02f), float2(0.0f, -0.02f)};
    const float weights[5] = {0.44f, 0.14f, 0.14f, 0.14f, 0.14f};
    float target = 0.0f;
    [unroll]
    for (int index = 0; index < 5; ++index)
        target += weights[index] /
                  max(linear_depth(Depth.SampleLevel(PointClamp, 0.5f + ring[index], 0.0f)), 0.35f);
    float previous = Previous.Load(int3(0, 0, 0));
    if (PassA.x > 0.5f || previous <= 0.0f)
        return target.xxxx;
    float rate = target > previous ? 9.0f : 5.0f;
    float blend = saturate(1.0f - exp(-max(PostTime.y, 0.0f) * rate));
    return (previous + (target - previous) * blend).xxxx;
}
)hlsl";

// Half-resolution colour with the signed blur diameter (in half-resolution
// pixels) in alpha: negative in front of the focus plane.
constexpr char dof_prefilter_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float> Focus : register(t2);

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 full = int2(input.position.xy) * 2;
    float focus = Focus.Load(int3(0, 0, 0));
    float3 total = 0.0f.xxx;
    float weight_total = 0.0f;
    float nearest = 100000.0f;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        int2 tap = full + int2(index & 1, index >> 1);
        float3 color = Scene.Load(int3(tap, 0)).rgb;
        // Karis average keeps one very bright texel from becoming a firefly.
        float weight = 1.0f / (1.0f + luminance(color));
        total += color * weight;
        weight_total += weight;
        nearest = min(nearest, linear_depth(Depth.Load(int3(tap, 0))));
    }
    float coc = clamp(FocusParams.x * (focus - 1.0f / max(nearest, 0.01f)), -FocusParams.y,
                      FocusParams.y);
    return float4(total / weight_total, coc * PostSize.y * 0.5f);
}
)hlsl";

// One-pass scatter-as-gather bokeh on a golden-angle spiral.
constexpr char dof_gather_source[] = R"hlsl(
Texture2D Source : register(t0);

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 texel = PostSize.zw * 2.0f;
    float4 centre = Source.SampleLevel(PointClamp, input.uv, 0.0f);
    float centre_size = abs(centre.a);
    float longest = FocusParams.y * PostSize.y * 0.5f;
    float spacing = max(longest * longest / (2.0f * max(FocusParams.z, 8.0f)), 0.35f);
    float3 total = centre.rgb;
    float count = 1.0f;
    float spread = centre_size;
    float radius = spacing;
    const float golden_angle = 2.39996323f;
    float angle = gradient_noise(input.position.xy) * 6.2831853f;
    [loop]
    for (int tap = 0; tap < 128 && radius < longest; ++tap)
    {
        float2 offset = float2(cos(angle), sin(angle)) * radius;
        float4 sample_value = Source.SampleLevel(LinearClamp, input.uv + offset * texel, 0.0f);
        float sample_size = abs(sample_value.a);
        // Something behind this pixel cannot spread its blur over it further
        // than this pixel is itself out of focus: sharp edges stay clean.
        if (sample_value.a > centre.a)
            sample_size = clamp(sample_size, 0.0f, centre_size * 2.0f);
        float covers = smoothstep(radius - 0.5f, radius + 0.5f, sample_size);
        total += lerp(total / count, sample_value.rgb, covers);
        spread = max(spread, sample_size * covers);
        count += 1.0f;
        radius += spacing / radius;
        angle += golden_angle;
    }
    return float4(total / count, spread);
}
)hlsl";

constexpr char dof_composite_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D Blurred : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float> Focus : register(t3);

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float3 sharp = Scene.Load(int3(pixel, 0)).rgb;
    float4 blurred = Blurred.SampleLevel(LinearClamp, input.uv, 0.0f);
    float focus = Focus.Load(int3(0, 0, 0));
    float camera_depth = linear_depth(Depth.Load(int3(pixel, 0)));
    float coc = abs(clamp(FocusParams.x * (focus - 1.0f / max(camera_depth, 0.01f)),
                          -FocusParams.y, FocusParams.y)) * PostSize.y;
    // Blur diameter in full-resolution pixels: this pixel's own, or the blur
    // of a nearer thing that the gather spread over it.
    float size = max(coc, blurred.a * 2.0f);
    float amount = smoothstep(1.0f, 3.2f, size);
    return float4(lerp(sharp, blurred.rgb, amount), 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 9: bloom.
// ---------------------------------------------------------------------------
// 13-tap down-sample (Jimenez, "Next Generation Post Processing in Call of
// Duty"). PassA.xy = source texel; PassA.z > 0.5 on the first level, where the
// five groups are Karis-weighted against fireflies.
constexpr char bloom_down_source[] = R"hlsl(
Texture2D Source : register(t0);

float3 tap(float2 uv, float x, float y)
{
    return min(Source.SampleLevel(LinearClamp, uv + float2(x, y) * PassA.xy, 0.0f).rgb, 96.0f);
}

float karis(float3 color)
{
    return 1.0f / (1.0f + luminance(color));
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 uv = input.uv;
    float3 a = tap(uv, -2.0f, -2.0f);
    float3 b = tap(uv, 0.0f, -2.0f);
    float3 c = tap(uv, 2.0f, -2.0f);
    float3 d = tap(uv, -2.0f, 0.0f);
    float3 e = tap(uv, 0.0f, 0.0f);
    float3 f = tap(uv, 2.0f, 0.0f);
    float3 g = tap(uv, -2.0f, 2.0f);
    float3 h = tap(uv, 0.0f, 2.0f);
    float3 i = tap(uv, 2.0f, 2.0f);
    float3 j = tap(uv, -1.0f, -1.0f);
    float3 k = tap(uv, 1.0f, -1.0f);
    float3 l = tap(uv, -1.0f, 1.0f);
    float3 m = tap(uv, 1.0f, 1.0f);
    float3 centre = (j + k + l + m) * 0.25f;
    float3 top_left = (a + b + d + e) * 0.25f;
    float3 top_right = (b + c + e + f) * 0.25f;
    float3 bottom_left = (d + e + g + h) * 0.25f;
    float3 bottom_right = (e + f + h + i) * 0.25f;
    if (PassA.z > 0.5f)
    {
        float w0 = karis(centre) * 0.5f;
        float w1 = karis(top_left) * 0.125f;
        float w2 = karis(top_right) * 0.125f;
        float w3 = karis(bottom_left) * 0.125f;
        float w4 = karis(bottom_right) * 0.125f;
        return float4((centre * w0 + top_left * w1 + top_right * w2 + bottom_left * w3 +
                       bottom_right * w4) / (w0 + w1 + w2 + w3 + w4), 1.0f);
    }
    return float4(centre * 0.5f + (top_left + top_right + bottom_left + bottom_right) * 0.125f,
                  1.0f);
}
)hlsl";

// 3x3 tent up-sample. The blend state mixes it into the next finer level by
// the scatter factor, so the levels always sum to one.
constexpr char bloom_up_source[] = R"hlsl(
Texture2D Source : register(t0);

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 uv = input.uv;
    float2 d = PassA.xy;
    float3 total = Source.SampleLevel(LinearClamp, uv, 0.0f).rgb * 4.0f;
    total += Source.SampleLevel(LinearClamp, uv + float2(-d.x, 0.0f), 0.0f).rgb * 2.0f;
    total += Source.SampleLevel(LinearClamp, uv + float2(d.x, 0.0f), 0.0f).rgb * 2.0f;
    total += Source.SampleLevel(LinearClamp, uv + float2(0.0f, -d.y), 0.0f).rgb * 2.0f;
    total += Source.SampleLevel(LinearClamp, uv + float2(0.0f, d.y), 0.0f).rgb * 2.0f;
    total += Source.SampleLevel(LinearClamp, uv + float2(-d.x, -d.y), 0.0f).rgb;
    total += Source.SampleLevel(LinearClamp, uv + float2(d.x, -d.y), 0.0f).rgb;
    total += Source.SampleLevel(LinearClamp, uv + float2(-d.x, d.y), 0.0f).rgb;
    total += Source.SampleLevel(LinearClamp, uv + float2(d.x, d.y), 0.0f).rgb;
    return float4(total / 16.0f, 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 10: from scene light to the display.
// ---------------------------------------------------------------------------
constexpr char composite_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D Bloom : register(t1);
Texture2D Adapted : register(t2);
Texture2D<float> Depth : register(t3);

float scene_exposure()
{
    float manual = GradeParams.x;
    if (GradeParams.w < 0.5f)
        return manual;
    float average = exp(Adapted.SampleLevel(LinearClamp, float2(0.5f, 0.5f), 0.0f).r);
    // Closes down for bright snow and sand; never opens up a dark scene. The
    // one exception is deep in Hell, where GradeParams.w carries how far the
    // eye may open (world::hell_exposure_limit; exactly 1 everywhere else).
    return clamp(0.20f / max(average, 0.0008f), 0.354f, max(GradeParams.w, 1.0f)) * manual;
}

float3 tonemap_aces(float3 color)
{
    color *= 0.725f;
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 tonemap_reinhard_jodie(float3 color)
{
    float l = luminance(color);
    float3 per_channel = color / (1.0f + color);
    return saturate(lerp(color / (1.0f + l), per_channel, per_channel));
}

float3 hable_curve(float3 x)
{
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 tonemap_filmic(float3 color)
{
    const float white = 11.2f;
    return saturate(hable_curve(color * 2.0f) / hable_curve(float3(white, white, white)));
}

float3 tonemap_legacy(float3 color)
{
    float l = dot(color, float3(0.299f, 0.587f, 0.114f));
    if (l > 0.0001f)
        color *= (l / (1.0f + max(0.0f, l - 1.0f) * 0.45f)) / l;
    return color;
}

float3 apply_tonemap(float3 color)
{
    int curve = (int)(PostParams.z + 0.5f);
    if (curve == 0)
        return tonemap_legacy(color);
    if (curve == 1)
        return tonemap_reinhard_jodie(color);
    if (curve == 2)
        return tonemap_filmic(color);
    return tonemap_aces(color);
}

// White balance against the colour of the air, bounded so dawn and dusk keep
// their warmth.
float3 white_balance(float3 color)
{
    float3 tint = 1.0f / (FogAndEnd.rgb + 0.5f);
    tint = lerp(tint, float3(1.0f, 1.0f, 1.0f), 0.2f);
    tint = min(tint / max(min(tint.r, min(tint.g, tint.b)), 0.0001f), 1.25f);
    return lerp(color, color * tint, saturate(GradeParams.y));
}

float3 apply_grade(float3 color, float strength)
{
    if (strength <= 0.001f)
        return color;
    float l = luminance(color);
    float3 graded = lerp(float3(l, l, l), color, 0.79f);
    const float k = 1.18f;
    float3 low = 0.5f * pow(saturate(2.0f * graded), k);
    float3 high = 1.0f - 0.5f * pow(saturate(2.0f - 2.0f * graded), k);
    graded = lerp(low, high, step(0.5f, graded));
    return saturate(lerp(color, graded, saturate(strength)));
}

float3 display_color(float3 color)
{
    color = white_balance(max(color, 0.0f));
    color = apply_tonemap(color);
    color = sqrt(saturate(color));
    color = apply_grade(color, GradeParams.y);
    float l = luminance(color);
    float3 biome = lerp(l.xxx, color, BiomeParams.x) * BiomeTint.rgb;
    return saturate(lerp(color, biome, BiomeTint.w));
}

// How much of the sun's disc is clear of terrain, from the depth buffer.
float sun_clearance(float2 sun_uv)
{
    const float2 ring[5] = {float2(0.0f, 0.0f), float2(0.006f, 0.0f), float2(-0.006f, 0.0f),
                            float2(0.0f, 0.010f), float2(0.0f, -0.010f)};
    float clear = 0.0f;
    [unroll]
    for (int index = 0; index < 5; ++index)
    {
        float2 uv = sun_uv + ring[index];
        float inside = step(0.0f, uv.x) * step(uv.x, 1.0f) * step(0.0f, uv.y) * step(uv.y, 1.0f);
        clear += inside * step(0.999999f, Depth.SampleLevel(PointClamp, saturate(uv), 0.0f));
    }
    return clear * 0.2f;
}

// What a real lens adds when the sun is in frame: a streak, a ring and a few
// faint reflections walking across the picture.
float3 sun_glare(float2 uv)
{
    if (BloomParams.w < 0.001f || PostSun.z < 0.02f)
        return 0.0f.xxx;
    float2 sun_uv = PostSun.xy;
    float clear = sun_clearance(sun_uv);
    if (clear <= 0.0f)
        return 0.0f.xxx;
    float aspect = PostSize.x * PostSize.w;
    float2 to_pixel = (uv - sun_uv) * float2(aspect, 1.0f);
    float distance_to_sun = length(to_pixel);
    float2 outside = max(float2(0.0f, 0.0f), max(-sun_uv, sun_uv - 1.0f));
    float edge = saturate(1.0f - (outside.x + outside.y) * 4.0f);
    float3 sun_color = LightAndFogStart.rgb / max(max(LightAndFogStart.r, LightAndFogStart.g),
                                                  max(LightAndFogStart.b, 0.05f));
    float3 glare = sun_color * (exp(-abs(to_pixel.y) * 110.0f) * exp(-abs(to_pixel.x) * 5.5f) * 0.10f +
                                exp(-distance_to_sun * 9.0f) * 0.10f);
    float ring_offset = (distance_to_sun - 0.34f) * 26.0f;
    glare += float3(0.9f, 0.6f, 1.0f) * exp(-ring_offset * ring_offset) * 0.012f;
    float2 axis = (float2(0.5f, 0.5f) - sun_uv) * float2(aspect, 1.0f);
    float2 from_centre = (uv - 0.5f) * float2(aspect, 1.0f);
    const float placements[4] = {0.45f, 0.85f, 1.35f, 1.9f};
    const float sizes[4] = {0.035f, 0.060f, 0.022f, 0.090f};
    const float3 tints[4] = {float3(0.55f, 0.85f, 1.0f), float3(1.0f, 0.75f, 0.45f),
                             float3(0.7f, 1.0f, 0.65f), float3(0.8f, 0.65f, 1.0f)};
    [unroll]
    for (int ghost = 0; ghost < 4; ++ghost)
    {
        float2 centre = axis * (placements[ghost] - 1.0f);
        float falloff = saturate(1.0f - length(from_centre - centre) / sizes[ghost]);
        glare += tints[ghost] * falloff * falloff * 0.020f;
    }
    return glare * clear * edge * BloomParams.w * SkyWind.w * (0.25f + 0.75f * PostSun.w);
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 uv = input.uv;
    float3 color;
    if (GradeUser.w > 0.0005f)
    {
        // Colour fringing grows with the square of the distance from centre,
        // so the middle of the picture, where the crosshair is, stays clean.
        float2 centred = uv - 0.5f;
        float2 shift = centred * dot(centred, centred) * GradeUser.w * 0.035f;
        color.r = Scene.SampleLevel(LinearClamp, uv - shift, 0.0f).r;
        color.g = Scene.SampleLevel(LinearClamp, uv, 0.0f).g;
        color.b = Scene.SampleLevel(LinearClamp, uv + shift, 0.0f).b;
    }
    else
    {
        color = Scene.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    }
    if (BloomParams.x > 0.0001f)
        color = lerp(color, Bloom.SampleLevel(LinearClamp, uv, 0.0f).rgb, BloomParams.x);
    color += sun_glare(uv);
    color = display_color(color * scene_exposure());
    // Dither of one 8-bit step with a triangular distribution (two unlike
    // patterns summed): the smooth gradients of sky and fog would band on
    // 8-bit output, and a rectangular dither of half that leaves the bands'
    // edges showing as stripes of noisier and quieter pixels.
    float3 first = frac(dot(float2(171.0f, 231.0f), input.position.xy) /
                        float3(103.0f, 71.0f, 97.0f));
    float3 second = frac(52.9829189f * frac(dot(input.position.xy + float2(47.0f, 17.0f),
                                                float2(0.06711056f, 0.00583715f))) +
                         float3(0.0f, 0.37f, 0.71f));
    color += (first + second - 1.0f) / 255.0f;
    // Luma rides in alpha for the edge-smoothing pass; the last pass of the
    // chain writes one, so the finished frame is opaque.
    return float4(color, PassA.x > 0.5f ? 1.0f : dot(saturate(color), float3(0.299f, 0.587f, 0.114f)));
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 11: FXAA 3.11 (quality path) on the display-encoded frame.
// ---------------------------------------------------------------------------
constexpr char fxaa_source[] = R"hlsl(
Texture2D Source : register(t0);

float luma_at(float2 uv)
{
    return Source.SampleLevel(LinearClamp, uv, 0.0f).a;
}

float4 main(PostPixelInput input) : SV_TARGET
{
    float2 uv = input.uv;
    float2 rcp = PostSize.zw;
    float4 centre = Source.SampleLevel(LinearClamp, uv, 0.0f);
    float alpha = PassA.x > 0.5f ? 1.0f : centre.a;
    float lumaM = centre.a;
    float lumaS = luma_at(uv + float2(0.0f, rcp.y));
    float lumaE = luma_at(uv + float2(rcp.x, 0.0f));
    float lumaN = luma_at(uv + float2(0.0f, -rcp.y));
    float lumaW = luma_at(uv + float2(-rcp.x, 0.0f));
    float range_max = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float range_min = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = range_max - range_min;
    if (range < max(0.0312f, range_max * 0.125f))
        return float4(centre.rgb, alpha);

    float lumaNW = luma_at(uv + float2(-rcp.x, -rcp.y));
    float lumaSE = luma_at(uv + float2(rcp.x, rcp.y));
    float lumaNE = luma_at(uv + float2(rcp.x, -rcp.y));
    float lumaSW = luma_at(uv + float2(-rcp.x, rcp.y));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpix_rcp_range = 1.0f / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edge_horz1 = -2.0f * lumaM + lumaNS;
    float edge_vert1 = -2.0f * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edge_horz2 = -2.0f * lumaE + lumaNESE;
    float edge_vert2 = -2.0f * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edge_horz4 = abs(edge_horz1) * 2.0f + abs(edge_horz2);
    float edge_vert4 = abs(edge_vert1) * 2.0f + abs(edge_vert2);
    float edge_horz3 = -2.0f * lumaW + lumaNWSW;
    float edge_vert3 = -2.0f * lumaS + lumaSWSE;
    float edge_horz = abs(edge_horz3) + edge_horz4;
    float edge_vert = abs(edge_vert3) + edge_vert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float length_sign = rcp.x;
    bool horizontal = edge_horz >= edge_vert;
    float subpixA = subpixNSWE * 2.0f + subpixNWSWNESE;
    if (!horizontal)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    else
    {
        length_sign = rcp.y;
    }
    float subpixB = subpixA * (1.0f / 12.0f) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        length_sign = -length_sign;
    float subpixC = saturate(abs(subpixB) * subpix_rcp_range);
    float2 posB = uv;
    float2 off = horizontal ? float2(rcp.x, 0.0f) : float2(0.0f, rcp.y);
    if (!horizontal)
        posB.x += length_sign * 0.5f;
    else
        posB.y += length_sign * 0.5f;
    float2 posN = posB - off;
    float2 posP = posB + off;
    float subpixD = -2.0f * subpixC + 3.0f;
    float lumaEndN = luma_at(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = luma_at(posP);
    if (!pairN)
        lumaNN = lumaSS;
    float gradient_scaled = gradient * 0.25f;
    float lumaMM = lumaM - lumaNN * 0.5f;
    float subpixF = subpixD * subpixE;
    bool luma_below = lumaMM < 0.0f;
    lumaEndN -= lumaNN * 0.5f;
    lumaEndP -= lumaNN * 0.5f;
    bool doneN = abs(lumaEndN) >= gradient_scaled;
    bool doneP = abs(lumaEndP) >= gradient_scaled;
    const float advance[7] = {1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f};
    [unroll]
    for (int search = 0; search < 7; ++search)
    {
        if (!doneN)
        {
            posN -= off * advance[search];
            lumaEndN = luma_at(posN) - lumaNN * 0.5f;
            doneN = abs(lumaEndN) >= gradient_scaled;
        }
        if (!doneP)
        {
            posP += off * advance[search];
            lumaEndP = luma_at(posP) - lumaNN * 0.5f;
            doneP = abs(lumaEndP) >= gradient_scaled;
        }
    }
    float dstN = horizontal ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horizontal ? posP.x - uv.x : posP.y - uv.y;
    bool good_spanN = (lumaEndN < 0.0f) != luma_below;
    bool good_spanP = (lumaEndP < 0.0f) != luma_below;
    float span_length = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool good_span = directionN ? good_spanN : good_spanP;
    float subpixG = subpixF * subpixF;
    float pixel_offset = dst * (-1.0f / span_length) + 0.5f;
    // 0.5 instead of the usual 0.75: less texture softening, and sharpening
    // follows this pass.
    float subpixH = subpixG * 0.5f;
    float pixel_offset_good = good_span ? pixel_offset : 0.0f;
    float pixel_offset_subpix = max(pixel_offset_good, subpixH);
    if (!horizontal)
        uv.x += pixel_offset_subpix * length_sign;
    else
        uv.y += pixel_offset_subpix * length_sign;
    return float4(Source.SampleLevel(LinearClamp, uv, 0.0f).rgb, alpha);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 12: contrast-adaptive sharpening (after AMD FidelityFX CAS). Sharpens
// soft texture detail most and already-contrasty edges least, so it restores
// what edge smoothing softened without ringing.
// ---------------------------------------------------------------------------
constexpr char sharpen_source[] = R"hlsl(
Texture2D Source : register(t0);

float4 main(PostPixelInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float3 e = Source.Load(int3(pixel, 0)).rgb;
    float3 b = Source.Load(int3(pixel + int2(0, -1), 0)).rgb;
    float3 d = Source.Load(int3(pixel + int2(-1, 0), 0)).rgb;
    float3 f = Source.Load(int3(pixel + int2(1, 0), 0)).rgb;
    float3 h = Source.Load(int3(pixel + int2(0, 1), 0)).rgb;
    float3 lowest = min(min(min(d, e), min(f, b)), h);
    float3 highest = max(max(max(d, e), max(f, b)), h);
    float3 amplitude = sqrt(saturate(min(lowest, 1.0f - highest) / max(highest, 0.0001f)));
    float peak = -1.0f / lerp(8.0f, 5.0f, saturate(SharpenParams.x));
    float3 weight = amplitude * peak;
    float3 sharpened = (b * weight + d * weight + f * weight + h * weight + e) /
                       (1.0f + 4.0f * weight);
    return float4(saturate(lerp(e, sharpened, saturate(SharpenParams.x * 2.0f))), 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Pass 14: the lens and the player's colour controls, over the finished world
// AND the first-person body composited on it, at output resolution. Running
// here rather than in the world chain is what keeps a held weapon the same
// colour temperature, contrast and saturation as the world behind it.
// PassA = vignette, grain, grain seed, unused. PassB = contrast, saturation,
// temperature, unused.
// ---------------------------------------------------------------------------
constexpr char lens_source[] = R"hlsl(
Texture2D Source : register(t0);

float4 main(PostPixelInput input) : SV_TARGET
{
    float3 color = Source.Load(int3(int2(input.position.xy), 0)).rgb;
    // Colour temperature: a gain on red against blue, at constant luminance.
    float warm = PassB.z;
    float3 trim = float3(1.0f + warm * 0.14f, 1.0f, 1.0f - warm * 0.18f);
    color = saturate(color * trim / dot(trim, float3(0.2126f, 0.7152f, 0.0722f)));
    // Contrast about middle grey: black and white stay where they are, so it
    // can neither crush shadows nor clip highlights.
    float contrast = PassB.x;
    float3 low = 0.5f * pow(saturate(2.0f * color), contrast);
    float3 high = 1.0f - 0.5f * pow(saturate(2.0f - 2.0f * color), contrast);
    color = lerp(low, high, step(0.5f, color));
    float l = luminance(color);
    color = saturate(lerp(l.xxx, color, PassB.y));
    // Vignette: flat through the middle of the picture, where the crosshair
    // is, falling off toward the corners (openminerz::post::vignette).
    float2 centred = input.uv * 2.0f - 1.0f;
    float radius = sqrt(dot(centred, centred) * 0.5f);
    float edge = saturate((radius - 0.35f) / 0.65f);
    color *= 1.0f - PassA.x * 0.55f * edge * edge * (3.0f - 2.0f * edge);
    // Film grain: zero-mean, strongest in the midtones, a new pattern every
    // frame (or one still pattern with Reduced motion).
    if (PassA.y > 0.0005f)
    {
        float2 p = input.position.xy + PassA.z * float2(37.0f, 17.0f);
        float3 q = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
        q += dot(q, q.yzx + 33.33f);
        float noise = frac((q.x + q.y) * q.z) + frac((q.y + q.z) * q.x) - 1.0f;
        float midtone = 1.0f - abs(luminance(color) * 2.0f - 1.0f);
        color = saturate(color + noise * PassA.y * 0.085f * (0.35f + 0.65f * midtone));
    }
    return float4(color, 1.0f);
}
)hlsl";

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};
};

struct alignas(16) PostConstants final
{
    Float4 size;
    Float4 time;
    Float4 prev_eye;
    Float4 prev_right;
    Float4 prev_up;
    Float4 prev_forward;
    Float4 prev_projection;
    Float4 sky_zenith;
    Float4 sky_ground;
    Float4 sky_params;
    Float4 sky_moon;
    Float4 sky_wind;
    Float4 sky_plates;
    Float4 ao;
    Float4 contact;
    Float4 volume;
    Float4 volume_light;
    Float4 focus;
    Float4 bloom;
    Float4 grade_user;
    Float4 motion;
    Float4 sharpen;
};

struct PassConstants final
{
    Float4 a;
    Float4 b;
};

struct Target final
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    int width{};
    int height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};

    void reset() noexcept
    {
        texture.Reset();
        target.Reset();
        view.Reset();
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }
};

// Tileable value noise, several octaves, as one small texture: r = broad
// cloud shapes, g = fine detail, b = billows, a = white noise.
std::vector<std::uint8_t> build_noise(int size)
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
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
            const float broad = fbm(u, v, 4, 5, 11);
            const float fine = fbm(u, v, 16, 4, 71);
            const float billow = 1.0F - std::abs(fbm(u, v, 8, 4, 131) * 2.0F - 1.0F);
            const std::size_t index = (static_cast<std::size_t>(y) * size + x) * 4U;
            const auto pack = [](float channel) {
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
            };
            // Stretch the fbm, which crowds around one half, across the range.
            texels[index] = pack((broad - 0.5F) * 1.9F + 0.5F);
            texels[index + 1U] = pack((fine - 0.5F) * 1.9F + 0.5F);
            texels[index + 2U] = pack(billow);
            texels[index + 3U] = pack(hash(x, y, 977));
        }
    return texels;
}

} // namespace

class PostProcessor::Impl final
{
  public:
    Impl(ID3D11Device *device, ID3D11DeviceContext *context) : device_(device), context_(context)
    {
        trace_ = SDL_getenv("OPENMINERZ_POSTFX_TRACE") != nullptr;
        // OPENMINERZ_POSTFX_DEBUG=ao|contact|volume|normal|share shows one
        // term of the resolve pass alone, for judging it without the rest.
        if (const char *debug = SDL_getenv("OPENMINERZ_POSTFX_DEBUG"))
        {
            constexpr std::array<std::string_view, 5> names{"ao", "contact", "volume", "normal",
                                                            "share"};
            for (std::size_t index = 0U; index < names.size(); ++index)
                if (names[index] == debug)
                    debug_view_ = static_cast<float>(index + 1U);
        }
        const Uint64 started = SDL_GetTicksNS();
        ready_ = create_shaders() && create_states() && create_noise();
        if (!ready_)
            SDL_Log("Post-processing chain unavailable: %s", failure_.c_str());
        else if (trace_)
            SDL_Log("POSTFX shaders and resources ready in %.1f ms (at load, never during play)",
                    static_cast<double>(SDL_GetTicksNS() - started) / 1.0e6);
    }

    ~Impl()
    {
        report_timings(true);
    }

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

    void set_effects(const PostEffects &effects) noexcept
    {
        // Which targets exist depends on which effects are on; a change is
        // picked up by the next resize(), never in the middle of a frame.
        targets_stale_ = targets_stale_ || effects.bloom != effects_.bloom ||
                         effects.ao_slices != effects_.ao_slices ||
                         effects.volumetric_steps != effects_.volumetric_steps ||
                         effects.depth_of_field != effects_.depth_of_field ||
                         (effects.motion_blur > 0.001F) != (effects_.motion_blur > 0.001F) ||
                         effects.fxaa != effects_.fxaa ||
                         (effects.sharpening > 0.001F) != (effects_.sharpening > 0.001F) ||
                         effects.contact_shadows != effects_.contact_shadows ||
                         deep_air_possible(effects) != deep_air_possible(effects_);
        effects_ = effects;
    }

    [[nodiscard]] const PostEffects &effects() const noexcept
    {
        return effects_;
    }

    [[nodiscard]] bool resize(int width, int height, DXGI_FORMAT output_format)
    {
        if (!ready_)
            return false;
        if (width == width_ && height == height_ && output_format == output_format_ &&
            surface_[0].target != nullptr)
        {
            if (targets_stale_)
                prepare_targets();
            return true;
        }
        release_targets();
        width_ = width;
        height_ = height;
        output_format_ = output_format;
        // The surface targets and one HDR target are needed by every Enhanced
        // frame; everything else is created when its effect first runs.
        if (!acquire(surface_[0], width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
            !acquire(surface_[1], width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
            !acquire(hdr_[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            release_targets();
            width_ = 0;
            height_ = 0;
            return false;
        }
        focus_primed_ = false;
        history_valid_ = false;
        prepare_targets();
        return true;
    }

    // Creates every target the current effects can use, now, so the frame that
    // first needs one (raising a weapon for depth of field, say) never
    // allocates. Each is fixed in size, so this cannot grow during play.
    void prepare_targets()
    {
        targets_stale_ = false;
        const int half_width = std::max(1, width_ / 2);
        const int half_height = std::max(1, height_ / 2);
        const bool occlusion = effects_.ao_slices > 0 && effects_.ao_steps > 0;
        const bool volume = effects_.volumetric_steps > 0;
        const bool focus = effects_.depth_of_field != DepthOfFieldMode::off;
        const bool motion = effects_.motion_blur > 0.001F;
        const auto prepare = [&](std::array<Target, 2> &pair, bool wanted, int width, int height,
                                 DXGI_FORMAT format) {
            for (Target &target : pair)
                if (wanted)
                    (void)acquire(target, width, height, format);
                else
                    target.reset();
        };
        prepare(ao_, occlusion, half_width, half_height, DXGI_FORMAT_R8_UNORM);
        prepare(volume_, volume, half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        prepare(dof_, focus, half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        prepare(focus_, focus, 1, 1, DXGI_FORMAT_R32_FLOAT);
        if (!focus)
            focus_primed_ = false;
        if (motion)
            (void)acquire(velocity_, width_, height_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        else
            velocity_.reset();
        // A second HDR target only when two full-resolution passes can chain
        // (the resolve, the deep underground's air, motion blur, depth of
        // field): created here, so entering Hell never allocates.
        const int chained = (occlusion || volume || effects_.contact_shadows ? 1 : 0) +
                            (deep_air_possible(effects_) ? 1 : 0) + (focus ? 1 : 0) +
                            (motion ? 1 : 0);
        if (chained >= 2 && (deep_air_possible(effects_) ||
                             ((occlusion || volume || effects_.contact_shadows) &&
                              (focus || motion))))
            (void)acquire(hdr_[1], width_, height_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        else
            hdr_[1].reset();
        int level_width = width_;
        int level_height = height_;
        const int levels = effects_.bloom ? post::bloom_level_count(width_, height_) : 0;
        for (int level = 0; level < post::maximum_bloom_levels; ++level)
        {
            level_width = std::max(1, level_width / 2);
            level_height = std::max(1, level_height / 2);
            Target &target = bloom_[static_cast<std::size_t>(level)];
            if (level < levels)
                (void)acquire(target, level_width, level_height, DXGI_FORMAT_R11G11B10_FLOAT);
            else
                target.reset();
        }
        const int tail = (effects_.fxaa ? 1 : 0) + (effects_.sharpening > 0.001F ? 1 : 0);
        for (int index = 0; index < 2; ++index)
            if (index < tail)
                (void)acquire(ldr_[static_cast<std::size_t>(index)], width_, height_,
                              DXGI_FORMAT_R8G8B8A8_UNORM);
            else
                ldr_[static_cast<std::size_t>(index)].reset();
    }

    [[nodiscard]] ID3D11RenderTargetView *surface_target(std::size_t index) const noexcept
    {
        if (index < surface_.size())
            return surface_[index].target.Get();
        // Per-object motion, only while motion blur is on.
        return index == 2U && effects_.motion_blur > 0.001F ? velocity_.target.Get() : nullptr;
    }

    [[nodiscard]] ID3D11Buffer *camera_constants() const noexcept
    {
        return post_buffer_.Get();
    }

    // --- For the far-terrain composite (far_terrain_renderer.hpp). ---
    [[nodiscard]] ID3D11ShaderResourceView *surface_view(std::size_t index) const noexcept
    {
        return index < surface_.size() ? surface_[index].view.Get() : nullptr;
    }

    void clear_surface_targets()
    {
        // Nothing drawn: a flat, fully lit, unoccluded surface under open sky.
        constexpr std::array<float, 4> surface0{0.5F, 1.0F, 0.5F, 1.0F};
        constexpr std::array<float, 4> surface1{1.0F, 1.0F, 0.0F, 1.0F};
        // Alpha zero: "nothing wrote its own motion here; use the camera's".
        constexpr std::array<float, 4> still{0.0F, 0.0F, 0.0F, 0.0F};
        if (surface_[0].target != nullptr)
            context_->ClearRenderTargetView(surface_[0].target.Get(), surface0.data());
        if (surface_[1].target != nullptr)
            context_->ClearRenderTargetView(surface_[1].target.Get(), surface1.data());
        if (velocity_.target != nullptr && effects_.motion_blur > 0.001F)
            context_->ClearRenderTargetView(velocity_.target.Get(), still.data());
    }

    void begin_frame(const PostFrame &frame)
    {
        if (!ready_)
            return;
        update_constants(frame);
    }

    void draw_sky(const PostFrame &frame)
    {
        if (!ready_)
            return;
        begin_frame_timing();
        const TimerScope timer{*this, Pass::sky};
        bind_common(frame);
        set_viewport(frame.width, frame.height);
        ID3D11RenderTargetView *target = frame.scene_target;
        context_->OMSetRenderTargets(1U, &target, frame.depth_readonly);
        // Far-plane triangle, LESS_EQUAL, no depth write: only pixels nothing
        // opaque covered run the shader.
        context_->OMSetDepthStencilState(sky_depth_state_.Get(), 0U);
        // dst + src * (1 - dst.a): soft particles and additive effects already
        // in the scene stay in front of the sky.
        context_->OMSetBlendState(under_blend_.Get(), nullptr, 0xFFFFFFFFU);
        context_->PSSetShader(sky_shader_.Get(), nullptr, 0U);
        ID3D11ShaderResourceView *noise = noise_view_.Get();
        context_->PSSetShaderResources(0U, 1U, &noise);
        context_->PSSetShaderResources(4U, static_cast<UINT>(frame_plates_.size()),
                                       frame_plates_.data());
        context_->Draw(3U, 0U);
        unbind(16U);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
    }

    [[nodiscard]] bool lens_needed() const noexcept
    {
        return ready_ && effects_.enabled &&
               (effects_.vignette > 0.001F || effects_.film_grain > 0.001F ||
                std::abs(effects_.contrast - 1.0F) > 0.001F ||
                std::abs(effects_.saturation - 1.0F) > 0.001F ||
                std::abs(effects_.temperature) > 0.001F || deep_grade_ > 0.001F);
    }

    void lens(ID3D11RenderTargetView *target, float elapsed_seconds)
    {
        if (target == nullptr || !lens_needed())
            return;
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)))
            return;
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        target->GetDesc(&view);
        if (description.SampleDesc.Count != 1U || view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D)
            return;
        if (lens_copy_ == nullptr || lens_width_ != description.Width ||
            lens_height_ != description.Height || lens_format_ != description.Format)
        {
            // One copy of the output, re-created only when the window changes.
            lens_copy_.Reset();
            lens_view_.Reset();
            D3D11_TEXTURE2D_DESC copy = description;
            copy.MipLevels = 1U;
            copy.ArraySize = 1U;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = 0U;
            copy.MiscFlags = 0U;
            D3D11_SHADER_RESOURCE_VIEW_DESC copy_view{};
            copy_view.Format = view.Format;
            copy_view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            copy_view.Texture2D.MipLevels = 1U;
            if (FAILED(device_->CreateTexture2D(&copy, nullptr, &lens_copy_)) ||
                FAILED(device_->CreateShaderResourceView(lens_copy_.Get(), &copy_view,
                                                         &lens_view_)))
            {
                lens_copy_.Reset();
                lens_view_.Reset();
                return;
            }
            lens_width_ = description.Width;
            lens_height_ = description.Height;
            lens_format_ = description.Format;
        }
        context_->CopySubresourceRegion(lens_copy_.Get(), 0U, 0U, 0U, 0U, texture.Get(),
                                        view.Texture2D.MipSlice, nullptr);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->RSSetState(rasterizer_.Get());
        const std::array<ID3D11Buffer *, 2> buffers{pass_buffer_.Get(), post_buffer_.Get()};
        context_->PSSetConstantBuffers(1U, static_cast<UINT>(buffers.size()), buffers.data());
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
        const float seed = effects_.reduced_motion
                               ? 1.0F
                               : std::fmod(std::floor(elapsed_seconds * 60.0F), 977.0F);
        // Deep in Hell the lens closes in a little and warms a little, over
        // the world and the first-person body alike (zero everywhere else).
        const float deep = std::clamp(deep_grade_, 0.0F, 1.0F);
        write_pass({std::clamp(effects_.vignette + deep_vignette * deep, 0.0F, 1.0F),
                    std::clamp(effects_.film_grain, 0.0F, 1.0F), seed, 0.0F},
                   {std::clamp(effects_.contrast, 0.5F, 1.5F),
                    std::clamp(effects_.saturation, 0.0F, 2.0F),
                    std::clamp(effects_.temperature + deep_warmth * deep, -1.0F, 1.0F), 0.0F});
        ID3D11ShaderResourceView *source = lens_view_.Get();
        draw_to(lens_shader_.Get(), target, static_cast<int>(description.Width),
                static_cast<int>(description.Height), &source, 1U);
        unbind(1U);
    }

    void set_plates(const std::array<ID3D11ShaderResourceView *, 12> &plates, float second_weight,
                    bool bound) noexcept
    {
        frame_plates_ = plates;
        plate_second_weight_ = second_weight;
        plates_bound_ = bound;
    }

    void run(const PostFrame &frame)
    {
        if (!ready_)
            return;
        bind_common(frame);
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
        const int half_width = std::max(1, frame.width / 2);
        const int half_height = std::max(1, frame.height / 2);

        // The scene as the next pass will read it. Passes ping-pong between
        // the renderer's own target and one HDR target of ours.
        ID3D11ShaderResourceView *scene = frame.scene_view;
        const auto next_target = [&]() -> Target & {
            Target &target = scene == hdr_[0].view.Get() ? hdr_[1] : hdr_[0];
            (void)acquire(target, frame.width, frame.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
            return target;
        };

        // 4) Ambient occlusion.
        const bool occlusion = effects_.ao_slices > 0 && effects_.ao_steps > 0 &&
                               acquire(ao_[0], half_width, half_height, DXGI_FORMAT_R8_UNORM) &&
                               acquire(ao_[1], half_width, half_height, DXGI_FORMAT_R8_UNORM);
        if (occlusion)
        {
            const TimerScope timer{*this, Pass::occlusion};
            draw_pass(ao_shader_.Get(), ao_[0], {frame.depth_view, surface_[0].view.Get()});
            write_pass({24.0F, 0.0F, 0.0F, 0.0F}, {});
            draw_pass(ao_blur_shader_.Get(), ao_[1], {ao_[0].view.Get(), frame.depth_view});
        }

        // 5) Volumetric light.
        const bool volume =
            effects_.volumetric_steps > 0 &&
            acquire(volume_[0], half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            acquire(volume_[1], half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (volume)
        {
            const TimerScope timer{*this, Pass::volume};
            draw_pass(volume_shader_.Get(), volume_[0],
                      {frame.depth_view, surface_[1].view.Get(), frame.shadow_view,
                       frame.shadow_transmission_view});
            write_pass({9.0F, 0.0F, 0.0F, 0.0F}, {});
            draw_pass(volume_blur_shader_.Get(), volume_[1],
                      {volume_[0].view.Get(), frame.depth_view});
        }

        // 6) Resolve.
        if (occlusion || volume || effects_.contact_shadows)
        {
            const TimerScope timer{*this, Pass::resolve};
            Target &target = next_target();
            if (target.target != nullptr)
            {
                write_pass({debug_view_, frame.atmosphere ? 1.0F : 0.0F, 0.0F, 0.0F}, {});
                std::array<ID3D11ShaderResourceView *, 18> views{};
                views[0] = scene;
                views[1] = frame.depth_view;
                views[2] = surface_[0].view.Get();
                views[3] = surface_[1].view.Get();
                std::copy(frame_plates_.begin(), frame_plates_.end(), views.begin() + 4);
                views[16] = occlusion ? ao_[1].view.Get() : nullptr;
                views[17] = volume ? volume_[1].view.Get() : nullptr;
                draw_to(resolve_shader_.Get(), target.target.Get(), target.width, target.height,
                        views.data(), views.size());
                unbind(18U);
                scene = target.view.Get();
            }
        }

        // 6b) The deep underground's air (hell_atmosphere.hpp): the lava's
        // glow in the haze, the heat shimmer, then embers and ash over it.
        // Only where that module says it is worth drawing; above ground this
        // is not reached. Before motion blur, depth of field and bloom, so all
        // three see it, and long before the first-person body and the
        // interface, which it can therefore never touch.
        if (frame.hell != nullptr && frame.hell->state().air && deep_air_possible(effects_))
        {
            const TimerScope timer{*this, Pass::deep_air};
            Target &target = next_target();
            if (target.target != nullptr)
            {
                unbind(8U);
                frame.hell->draw_air(scene, target.target.Get(), frame.depth_view,
                                     frame.scene_constants, frame.width, frame.height);
                if (frame.hell->state().embers)
                    frame.hell->draw_embers(target.target.Get(), frame.depth_readonly,
                                            frame.scene_constants, frame.width, frame.height);
                scene = target.view.Get();
                // That module sets its own pipeline state; put this chain's back.
                bind_common(frame);
                context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
                context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
            }
        }

        // 6c) The effects layer (vfx/vfx_renderer.hpp): gas at reduced
        // resolution, refraction, the smoke round the eye, sparks. It reads
        // the resolved scene and writes the next target, so nothing before
        // this point (occlusion, contact shadows, the air's shafts) can ever
        // be laid over smoke, and everything after it sees the effects.
        if (frame.vfx != nullptr && frame.vfx->wants_draw())
        {
            const TimerScope timer{*this, Pass::effects};
            Target &target = next_target();
            if (target.target != nullptr)
            {
                unbind(8U);
                VfxPass pass;
                pass.width = frame.width;
                pass.height = frame.height;
                pass.scene = scene;
                pass.target = target.target.Get();
                pass.depth = frame.depth_view;
                pass.scene_constants = frame.scene_constants;
                pass.shadow_sampler = frame.shadow_sampler;
                pass.shadow = frame.shadow_view;
                pass.elapsed_seconds = frame.elapsed_seconds;
                pass.atmosphere = frame.atmosphere;
                pass.eye_sky_light = frame.view.eye_sky_light;
                frame.vfx->draw(pass);
                scene = target.view.Get();
                // That module sets its own pipeline state; put this chain's back.
                bind_common(frame);
                const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(frame.width),
                                              static_cast<float>(frame.height), 0.0F, 1.0F};
                context_->RSSetViewports(1U, &viewport);
                context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
                context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
            }
        }

        // 7) Motion blur.
        if (effects_.motion_blur > 0.001F && history_valid_)
        {
            const TimerScope timer{*this, Pass::motion};
            Target &target = next_target();
            if (target.target != nullptr)
            {
                draw_pass(motion_shader_.Get(), target,
                          {scene, frame.depth_view, velocity_.view.Get()});
                scene = target.view.Get();
            }
        }

        // 8) Depth of field.
        if (effects_.depth_of_field != DepthOfFieldMode::off &&
            acquire(focus_[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
            acquire(focus_[1], 1, 1, DXGI_FORMAT_R32_FLOAT))
        {
            const TimerScope timer{*this, Pass::focus};
            // The lens keeps following the crosshair even while no blur is
            // drawn, so raising a weapon never starts with a stale focus.
            const std::size_t next = 1U - focus_index_;
            write_pass({focus_primed_ ? 0.0F : 1.0F, 0.0F, 0.0F, 0.0F}, {});
            draw_pass(focus_shader_.Get(), focus_[next],
                      {frame.depth_view, focus_[focus_index_].view.Get()});
            focus_index_ = next;
            focus_primed_ = true;
            if (lens_constant_ > 1.0e-6F &&
                acquire(dof_[0], half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                acquire(dof_[1], half_width, half_height, DXGI_FORMAT_R16G16B16A16_FLOAT))
            {
                ID3D11ShaderResourceView *focus = focus_[focus_index_].view.Get();
                draw_pass(dof_prefilter_shader_.Get(), dof_[0], {scene, frame.depth_view, focus});
                draw_pass(dof_gather_shader_.Get(), dof_[1], {dof_[0].view.Get()});
                Target &target = next_target();
                if (target.target != nullptr)
                {
                    draw_pass(dof_composite_shader_.Get(), target,
                              {scene, dof_[1].view.Get(), frame.depth_view, focus});
                    scene = target.view.Get();
                }
            }
        }

        // 9) Bloom.
        ID3D11ShaderResourceView *bloom{};
        if (effects_.bloom)
        {
            const TimerScope timer{*this, Pass::bloom};
            const int levels = post::bloom_level_count(frame.width, frame.height);
            bool complete = true;
            int level_width = frame.width;
            int level_height = frame.height;
            for (int level = 0; level < levels && complete; ++level)
            {
                level_width = std::max(1, level_width / 2);
                level_height = std::max(1, level_height / 2);
                complete = acquire(bloom_[static_cast<std::size_t>(level)], level_width,
                                   level_height, DXGI_FORMAT_R11G11B10_FLOAT);
            }
            if (complete)
            {
                ID3D11ShaderResourceView *source = scene;
                int source_width = frame.width;
                int source_height = frame.height;
                for (int level = 0; level < levels; ++level)
                {
                    Target &target = bloom_[static_cast<std::size_t>(level)];
                    write_pass({1.0F / static_cast<float>(source_width),
                                1.0F / static_cast<float>(source_height),
                                level == 0 ? 1.0F : 0.0F, 0.0F},
                               {});
                    draw_pass(bloom_down_shader_.Get(), target, {source});
                    source = target.view.Get();
                    source_width = target.width;
                    source_height = target.height;
                }
                // Up the chain: each level becomes lerp(itself, the combined
                // coarser levels, scatter) through the blend factor.
                constexpr float scatter = 0.68F;
                const std::array<float, 4> factor{scatter, scatter, scatter, scatter};
                context_->OMSetBlendState(factor_blend_.Get(), factor.data(), 0xFFFFFFFFU);
                for (int level = levels - 2; level >= 0; --level)
                {
                    Target &coarse = bloom_[static_cast<std::size_t>(level + 1)];
                    write_pass({1.0F / static_cast<float>(coarse.width),
                                1.0F / static_cast<float>(coarse.height), 0.0F, 0.0F},
                               {});
                    draw_pass(bloom_up_shader_.Get(), bloom_[static_cast<std::size_t>(level)],
                              {coarse.view.Get()});
                }
                context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
                bloom = bloom_[0].view.Get();
            }
        }

        // 10-12) Composite, edge smoothing, sharpening. Whichever is last
        // writes the presentation target.
        const bool sharpen = effects_.sharpening > 0.001F;
        const bool smooth = effects_.fxaa;
        const int tail = (smooth ? 1 : 0) + (sharpen ? 1 : 0);
        const bool intermediates =
            tail == 0 ||
            (acquire(ldr_[0], frame.width, frame.height, DXGI_FORMAT_R8G8B8A8_UNORM) &&
             (tail < 2 || acquire(ldr_[1], frame.width, frame.height, DXGI_FORMAT_R8G8B8A8_UNORM)));
        {
            const TimerScope timer{*this, Pass::composite};
            const bool last = tail == 0 || !intermediates;
            write_pass({last ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F}, {});
            const std::array<ID3D11ShaderResourceView *, 4> views{scene, bloom, frame.adapted_view,
                                                                 frame.depth_view};
            if (last)
                draw_to(composite_shader_.Get(), frame.output_target, frame.width, frame.height,
                        views.data(), views.size());
            else
                draw_to(composite_shader_.Get(), ldr_[0].target.Get(), frame.width, frame.height,
                        views.data(), views.size());
        }
        if (intermediates && smooth)
        {
            const TimerScope timer{*this, Pass::smooth};
            write_pass({sharpen ? 0.0F : 1.0F, 0.0F, 0.0F, 0.0F}, {});
            ID3D11ShaderResourceView *source = ldr_[0].view.Get();
            draw_to(fxaa_shader_.Get(), sharpen ? ldr_[1].target.Get() : frame.output_target,
                    frame.width, frame.height, &source, 1U);
        }
        if (intermediates && sharpen)
        {
            const TimerScope timer{*this, Pass::sharpen};
            ID3D11ShaderResourceView *source = smooth ? ldr_[1].view.Get() : ldr_[0].view.Get();
            draw_to(sharpen_shader_.Get(), frame.output_target, frame.width, frame.height, &source,
                    1U);
        }
        unbind(16U);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);

        // This frame's camera is the next frame's "where things were".
        previous_eye_ = frame.eye;
        previous_right_ = frame.right;
        previous_up_ = frame.up;
        previous_forward_ = frame.forward;
        previous_projection_ = frame.projection;
        history_valid_ = true;
        ++frame_index_;
        end_frame_timing();
    }

  private:
    enum class Pass : std::size_t
    {
        sky,
        occlusion,
        volume,
        resolve,
        motion,
        focus,
        bloom,
        composite,
        smooth,
        sharpen,
        deep_air,
        effects,
        count
    };
    static constexpr std::size_t pass_count = static_cast<std::size_t>(Pass::count);
    static constexpr std::array<const char *, pass_count> pass_names{
        "sky",   "ao",    "volumetric", "resolve", "motion_blur", "dof",
        "bloom", "composite",  "fxaa",    "sharpen", "deep_air",    "vfx"};

    // Whether the deep underground's air pass can be asked for with these
    // effects at all (whether it runs is decided per frame, by where the eye is).
    [[nodiscard]] static bool deep_air_possible(const PostEffects &effects) noexcept
    {
        return effects.enabled && effects.deep_light > 0.001F && effects.deep_air_steps > 0;
    }

    struct TimerScope final
    {
        TimerScope(Impl &owner, Pass pass) : owner_(owner), pass_(pass)
        {
            owner_.mark(pass_, 0U);
        }
        ~TimerScope()
        {
            owner_.mark(pass_, 1U);
        }
        TimerScope(const TimerScope &) = delete;
        TimerScope &operator=(const TimerScope &) = delete;

      private:
        Impl &owner_;
        Pass pass_;
    };

    [[nodiscard]] ComPtr<ID3DBlob> compile(std::string_view name, const char *body,
                                           const char *target, const char *defines = "")
    {
        std::string source{defines};
        if (std::string_view{target}.starts_with("ps"))
            source += common_source;
        source += body;
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        // Cached for the process (shader_cache.hpp): the chain is built again
        // with every world.
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

    [[nodiscard]] bool pixel_shader(std::string_view name, const char *body,
                                    ComPtr<ID3D11PixelShader> &shader, const char *defines = "")
    {
        const ComPtr<ID3DBlob> blob = compile(name, body, "ps_5_0", defines);
        return blob != nullptr &&
               SUCCEEDED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    nullptr, &shader));
    }

    [[nodiscard]] bool create_shaders()
    {
        const ComPtr<ID3DBlob> vertex = compile("vertex", vertex_source, "vs_4_0");
        if (vertex == nullptr ||
            FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
                                               nullptr, &vertex_shader_)))
            return false;
        const std::string sky_text = std::string{sky_common_source} + sky_source;
        const std::string resolve_text =
            std::string{atmosphere_hlsl} + sky_common_source + resolve_source;
        return pixel_shader("sky", sky_text.c_str(), sky_shader_) &&
               pixel_shader("ambient occlusion", ao_source, ao_shader_) &&
               pixel_shader("occlusion blur", half_blur_source, ao_blur_shader_) &&
               pixel_shader("volumetric light",
                            (std::string{atmosphere_hlsl} + volume_source).c_str(),
                            volume_shader_) &&
               pixel_shader("volumetric blur", half_blur_source, volume_blur_shader_,
                            "#define VOLUME 1\n") &&
               pixel_shader("resolve", resolve_text.c_str(), resolve_shader_) &&
               pixel_shader("motion blur", motion_source, motion_shader_) &&
               pixel_shader("autofocus", focus_source, focus_shader_) &&
               pixel_shader("depth of field prefilter", dof_prefilter_source,
                            dof_prefilter_shader_) &&
               pixel_shader("depth of field gather", dof_gather_source, dof_gather_shader_) &&
               pixel_shader("depth of field composite", dof_composite_source,
                            dof_composite_shader_) &&
               pixel_shader("bloom down", bloom_down_source, bloom_down_shader_) &&
               pixel_shader("bloom up", bloom_up_source, bloom_up_shader_) &&
               pixel_shader("composite", composite_source, composite_shader_) &&
               pixel_shader("fxaa", fxaa_source, fxaa_shader_) &&
               pixel_shader("sharpen", sharpen_source, sharpen_shader_) &&
               pixel_shader("lens", lens_source, lens_shader_);
    }

    [[nodiscard]] bool create_states()
    {
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buffer.ByteWidth = sizeof(PostConstants);
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &post_buffer_)))
            return fail("constant buffer");
        buffer.ByteWidth = sizeof(PassConstants);
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &pass_buffer_)))
            return fail("pass buffer");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        if (FAILED(device_->CreateDepthStencilState(&depth, &no_depth_state_)))
            return fail("depth state");
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device_->CreateDepthStencilState(&depth, &sky_depth_state_)))
            return fail("sky depth state");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend, &opaque_blend_)))
            return fail("blend state");
        auto &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_INV_DEST_ALPHA;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device_->CreateBlendState(&blend, &under_blend_)))
            return fail("sky blend state");
        target.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        target.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_ZERO;
        if (FAILED(device_->CreateBlendState(&blend, &factor_blend_)))
            return fail("bloom blend state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        // The sky triangle sits exactly on the far plane.
        rasterizer.DepthClipEnable = FALSE;
        if (FAILED(device_->CreateRasterizerState(&rasterizer, &rasterizer_)))
            return fail("rasterizer state");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler, &point_sampler_)))
            return fail("point sampler");
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        if (FAILED(device_->CreateSamplerState(&sampler, &wrap_sampler_)))
            return fail("wrap sampler");

        if (trace_)
        {
            D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0U};
            D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0U};
            for (TimingFrame &frame : timing_)
            {
                if (FAILED(device_->CreateQuery(&disjoint, &frame.disjoint)))
                    trace_ = false;
                for (auto &pair : frame.stamps)
                    for (auto &query : pair)
                        if (FAILED(device_->CreateQuery(&stamp, &query)))
                            trace_ = false;
            }
        }
        return true;
    }

    [[nodiscard]] bool create_noise()
    {
        constexpr int size = 256;
        const std::vector<std::uint8_t> texels = build_noise(size);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = size;
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{texels.data(), static_cast<UINT>(size * 4), 0U};
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device_->CreateTexture2D(&description, &data, &texture)) ||
            FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &noise_view_)))
            return fail("noise texture");
        return true;
    }

    [[nodiscard]] bool fail(const char *what)
    {
        failure_ = std::string{"could not create the "} + what;
        return false;
    }

    // Creates or re-creates a target when its size or format changed. The set
    // of targets is fixed and each has exactly one size per resolution, so GPU
    // memory cannot grow during play.
    [[nodiscard]] bool acquire(Target &target, int width, int height, DXGI_FORMAT format)
    {
        if (target.target != nullptr && target.width == width && target.height == height &&
            target.format == format)
            return true;
        target.reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(std::max(width, 1));
        description.Height = static_cast<UINT>(std::max(height, 1));
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = format;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &target.texture)) ||
            FAILED(device_->CreateRenderTargetView(target.texture.Get(), nullptr, &target.target)) ||
            FAILED(device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.view)))
        {
            target.reset();
            return false;
        }
        target.width = width;
        target.height = height;
        target.format = format;
        return true;
    }

    void release_targets() noexcept
    {
        for (Target &target : surface_)
            target.reset();
        for (Target &target : hdr_)
            target.reset();
        for (Target &target : ldr_)
            target.reset();
        for (Target &target : ao_)
            target.reset();
        for (Target &target : volume_)
            target.reset();
        for (Target &target : dof_)
            target.reset();
        for (Target &target : bloom_)
            target.reset();
        velocity_.reset();
        // The focus pair is 1x1 and survives a resize; only its history resets.
    }

    void set_viewport(int width, int height)
    {
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
        context_->RSSetViewports(1U, &viewport);
    }

    void bind_common(const PostFrame &frame)
    {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->RSSetState(rasterizer_.Get());
        const std::array<ID3D11Buffer *, 3> buffers{frame.scene_constants, pass_buffer_.Get(),
                                                    post_buffer_.Get()};
        context_->PSSetConstantBuffers(0U, static_cast<UINT>(buffers.size()), buffers.data());
        const std::array<ID3D11SamplerState *, 4> samplers{
            frame.linear_sampler, frame.shadow_sampler, point_sampler_.Get(), wrap_sampler_.Get()};
        context_->PSSetSamplers(0U, static_cast<UINT>(samplers.size()), samplers.data());
    }

    void unbind(UINT count)
    {
        constexpr std::array<ID3D11ShaderResourceView *, 18> empty{};
        context_->PSSetShaderResources(0U, std::min<UINT>(count, 18U), empty.data());
    }

    void write_pass(const Float4 &a, const Float4 &b)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(pass_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return;
        const PassConstants constants{a, b};
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(pass_buffer_.Get(), 0U);
    }

    void draw_to(ID3D11PixelShader *shader, ID3D11RenderTargetView *target, int width, int height,
                 ID3D11ShaderResourceView *const *views, std::size_t view_count)
    {
        if (target == nullptr)
            return;
        // A target must never still be bound as an input of the same draw.
        unbind(8U);
        set_viewport(width, height);
        context_->OMSetRenderTargets(1U, &target, nullptr);
        context_->PSSetShader(shader, nullptr, 0U);
        context_->PSSetShaderResources(0U, static_cast<UINT>(view_count), views);
        context_->Draw(3U, 0U);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }

    void draw_pass(ID3D11PixelShader *shader, Target &target,
                   std::initializer_list<ID3D11ShaderResourceView *> views)
    {
        draw_to(shader, target.target.Get(), target.width, target.height, views.begin(),
                views.size());
    }

    void update_constants(const PostFrame &frame)
    {
        deep_grade_ = frame.deep_grade;
        PostConstants constants{};
        constants.size = {static_cast<float>(frame.width), static_cast<float>(frame.height),
                          1.0F / static_cast<float>(frame.width),
                          1.0F / static_cast<float>(frame.height)};
        constants.time = {frame.elapsed_seconds, frame.delta_seconds,
                          static_cast<float>(frame_index_ % 1024U),
                          effects_.reduced_motion ? 1.0F : 0.0F};
        const bool history = history_valid_;
        const auto &eye = history ? previous_eye_ : frame.eye;
        const auto &right = history ? previous_right_ : frame.right;
        const auto &up = history ? previous_up_ : frame.up;
        const auto &forward = history ? previous_forward_ : frame.forward;
        const auto &projection = history ? previous_projection_ : frame.projection;
        constants.prev_eye = {eye[0], eye[1], eye[2], 0.0F};
        constants.prev_right = {right[0], right[1], right[2], 0.0F};
        constants.prev_up = {up[0], up[1], up[2], 0.0F};
        constants.prev_forward = {forward[0], forward[1], forward[2], 0.0F};
        constants.prev_projection = {projection[0], projection[1], projection[2], projection[3]};

        const world::SkyState &sky = frame.sky;
        const float daylight = std::clamp(sky.daylight, 0.0F, 1.0F);
        const float storm = std::clamp(frame.view.storminess, 0.0F, 1.0F);
        const float night = std::clamp(1.0F - daylight * 1.6F, 0.0F, 1.0F);
        // The palette is display-referred; the scene is linear (gamma two, the
        // renderer's own material contract).
        const auto linear = [](world::SkyColor color) {
            return std::array<float, 3>{color.red * color.red, color.green * color.green,
                                        color.blue * color.blue};
        };
        const std::array<float, 3> zenith = linear(sky.zenith);
        constants.sky_zenith = {zenith[0], zenith[1], zenith[2], daylight};
        // (The haze along and under the horizon is the terrain's own fog
        // colour, which the shaders read from the scene constants.)
        constants.sky_ground = {0.0F, 0.0F, 0.0F, storm};
        // The painted sky is a faint detail layer in clear daylight and most of
        // the picture in heavy weather and at night, as it always was.
        const float painted = std::lerp(std::lerp(0.54F, 0.18F, daylight),
                                        std::lerp(0.94F, 0.78F, daylight), storm);
        constants.sky_params = {std::clamp(sky.hell, 0.0F, 1.0F),
                                std::clamp(sky.lightning_strength, 0.0F, 1.0F),
                                static_cast<float>(std::clamp(effects_.cloud_quality, 0, 4)),
                                plates_bound_ ? painted : 0.0F};
        // Display scale of the painted sky, matched to the gradient around it.
        const float plate_gain = std::lerp(0.92F, 1.18F, daylight) * std::lerp(1.0F, 0.86F, storm);
        // While the moon is the frame's light, the "sun" direction handed in
        // IS the moon's; otherwise the moon stands opposite the sun.
        const bool moonlit = frame.view.moonlight > 0.001F;
        const std::array<float, 3> moon =
            moonlit ? std::array<float, 3>{sky.sun_x, sky.sun_y, sky.sun_z}
                    : post::moon_direction(sky.sun_x, sky.sun_y, sky.sun_z);
        constants.sky_moon = {moon[0], moon[1], moon[2], night * (1.0F - sky.hell)};
        // Clouds drift on the world clock so they do not jump when a menu
        // pauses the frame clock; Reduced motion parks them.
        const float drift = effects_.reduced_motion ? 0.0F : frame.elapsed_seconds;
        constants.sky_wind = {drift * 0.00115F, drift * 0.00042F,
                              std::lerp(0.47F, 0.60F, storm),
                              moonlit ? 0.0F : std::lerp(1.0F, 0.10F, storm) * (1.0F - sky.hell)};
        constants.sky_plates = {plate_second_weight_, plates_bound_ ? 1.0F : 0.0F, plate_gain, 0.0F};

        constants.ao = {1.55F, 1.0F, static_cast<float>(effects_.ao_slices),
                        static_cast<float>(effects_.ao_steps)};
        // The sun's shadows only exist while it is up and outside Hell.
        const float sun_presence =
            std::clamp((sky.sun_y - 0.005F) / 0.10F, 0.0F, 1.0F) * (1.0F - sky.hell);
        constants.contact = {0.42F,
                             effects_.contact_shadows && sun_presence > 0.01F &&
                                     frame.shadow_view != nullptr
                                 ? static_cast<float>(effects_.contact_steps)
                                 : 0.0F,
                             0.80F * sun_presence, 0.34F};
        // The light-shaft pass: its step count and the eye's sky exposure. The
        // air it marches is the atmosphere's own (atmosphere_shader.hpp).
        constants.volume = {static_cast<float>(effects_.volumetric_steps), 0.0F, 0.0F,
                            std::clamp(frame.view.eye_sky_light, 0.0F, 1.0F)};
        const bool shadowed_air = frame.shadow_view != nullptr && sun_presence > 0.01F;
        constants.volume_light = {0.0F, 0.0F, 0.0F,
                                  !shadowed_air                       ? 0.0F
                                  : effects_.volumetric_steps >= 32 &&
                                          frame.shadow_transmission_view != nullptr
                                      ? 2.0F
                                      : 1.0F};

        // Depth of field. "When aiming" opens the lens with the weapon; the
        // shouldered zoom lengthens it, which is where most of the blur comes
        // from, as with a real lens.
        const float aim = std::clamp(frame.view.aim_progress, 0.0F, 1.0F);
        const float eased_aim = aim * aim * (3.0F - 2.0F * aim);
        float aperture{};
        if (effects_.depth_of_field == DepthOfFieldMode::aiming)
            aperture = eased_aim;
        else if (effects_.depth_of_field == DepthOfFieldMode::always)
            aperture = std::lerp(post::resting_aperture, 1.0F, eased_aim);
        lens_constant_ =
            post::lens_constant(frame.vertical_fov_degrees, effects_.depth_of_field_strength) *
            aperture;
        constants.focus = {lens_constant_, 0.016F,
                           static_cast<float>(std::clamp(effects_.depth_of_field_taps, 8, 120)),
                           0.0F};
        constants.bloom = {effects_.bloom ? post::bloom_mix(effects_.bloom_strength) : 0.0F, 0.68F,
                           0.0F, effects_.lens_flare ? 1.0F : 0.0F};
        constants.grade_user = {std::clamp(effects_.contrast, 0.5F, 1.5F),
                                std::clamp(effects_.saturation, 0.0F, 2.0F),
                                std::clamp(effects_.temperature, -1.0F, 1.0F),
                                std::clamp(effects_.chromatic_aberration, 0.0F, 1.0F)};
        constants.motion = {post::motion_blur_scale(effects_.motion_blur, frame.delta_seconds),
                            static_cast<float>(std::clamp(effects_.motion_blur_taps, 4, 32)),
                            0.045F, 0.0F};
        constants.sharpen = {std::clamp(effects_.sharpening, 0.0F, 1.0F), 0.0F, 0.0F, 0.0F};

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(post_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return;
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(post_buffer_.Get(), 0U);
    }

    // --- GPU timing (OPENMINERZ_POSTFX_TRACE) ---------------------------------
    struct TimingFrame final
    {
        ComPtr<ID3D11Query> disjoint;
        std::array<std::array<ComPtr<ID3D11Query>, 2>, pass_count> stamps;
        std::array<bool, pass_count> used{};
        bool pending{};
    };

    void begin_frame_timing()
    {
        if (!trace_)
            return;
        collect_timing();
        TimingFrame &frame = timing_[timing_cursor_];
        if (frame.pending)
        {
            // Results never arrived; drop the frame rather than block on it.
            timing_active_ = false;
            return;
        }
        frame.used.fill(false);
        context_->Begin(frame.disjoint.Get());
        timing_active_ = true;
    }

    void mark(Pass pass, std::size_t edge)
    {
        if (!trace_ || !timing_active_)
            return;
        TimingFrame &frame = timing_[timing_cursor_];
        const auto index = static_cast<std::size_t>(pass);
        context_->End(frame.stamps[index][edge].Get());
        frame.used[index] = true;
    }

    void end_frame_timing()
    {
        if (!trace_ || !timing_active_)
            return;
        TimingFrame &frame = timing_[timing_cursor_];
        context_->End(frame.disjoint.Get());
        frame.pending = true;
        timing_active_ = false;
        timing_cursor_ = (timing_cursor_ + 1U) % timing_.size();
        report_timings(false);
    }

    void collect_timing()
    {
        for (TimingFrame &frame : timing_)
        {
            if (!frame.pending)
                continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (context_->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            std::array<double, pass_count> milliseconds{};
            bool complete = true;
            for (std::size_t pass = 0U; pass < pass_count && complete; ++pass)
            {
                if (!frame.used[pass])
                    continue;
                UINT64 begin{};
                UINT64 end{};
                complete = context_->GetData(frame.stamps[pass][0].Get(), &begin, sizeof(begin),
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                           context_->GetData(frame.stamps[pass][1].Get(), &end, sizeof(end),
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (complete && disjoint.Frequency > 0U && end >= begin)
                    milliseconds[pass] = static_cast<double>(end - begin) * 1000.0 /
                                         static_cast<double>(disjoint.Frequency);
            }
            if (!complete)
                continue;
            frame.pending = false;
            if (disjoint.Disjoint)
                continue;
            for (std::size_t pass = 0U; pass < pass_count; ++pass)
                timing_total_[pass] += milliseconds[pass];
            ++timing_samples_;
        }
    }

    void report_timings(bool final_report)
    {
        if (!trace_ || timing_samples_ == 0U)
            return;
        if (!final_report && timing_samples_ % 600U != 0U)
            return;
        std::string line = "POSTFX gpu ms (" + std::to_string(width_) + "x" +
                           std::to_string(height_) + ", " + std::to_string(timing_samples_) +
                           " frames):";
        double total{};
        for (std::size_t pass = 0U; pass < pass_count; ++pass)
        {
            const double average = timing_total_[pass] / static_cast<double>(timing_samples_);
            total += average;
            char text[64];
            SDL_snprintf(text, sizeof(text), " %s=%.3f", pass_names[pass], average);
            line += text;
        }
        char text[64];
        SDL_snprintf(text, sizeof(text), " total=%.3f", total);
        line += text;
        SDL_Log("%s", line.c_str());
    }

    ID3D11Device *device_{};
    ID3D11DeviceContext *context_{};
    PostEffects effects_{};
    std::string failure_;
    bool ready_{};
    int width_{};
    int height_{};
    DXGI_FORMAT output_format_{DXGI_FORMAT_UNKNOWN};

    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> sky_shader_;
    ComPtr<ID3D11PixelShader> ao_shader_;
    ComPtr<ID3D11PixelShader> ao_blur_shader_;
    ComPtr<ID3D11PixelShader> volume_shader_;
    ComPtr<ID3D11PixelShader> volume_blur_shader_;
    ComPtr<ID3D11PixelShader> resolve_shader_;
    ComPtr<ID3D11PixelShader> motion_shader_;
    ComPtr<ID3D11PixelShader> focus_shader_;
    ComPtr<ID3D11PixelShader> dof_prefilter_shader_;
    ComPtr<ID3D11PixelShader> dof_gather_shader_;
    ComPtr<ID3D11PixelShader> dof_composite_shader_;
    ComPtr<ID3D11PixelShader> bloom_down_shader_;
    ComPtr<ID3D11PixelShader> bloom_up_shader_;
    ComPtr<ID3D11PixelShader> composite_shader_;
    ComPtr<ID3D11PixelShader> fxaa_shader_;
    ComPtr<ID3D11PixelShader> sharpen_shader_;
    ComPtr<ID3D11PixelShader> lens_shader_;
    ComPtr<ID3D11Texture2D> lens_copy_;
    ComPtr<ID3D11ShaderResourceView> lens_view_;
    UINT lens_width_{};
    UINT lens_height_{};
    DXGI_FORMAT lens_format_{DXGI_FORMAT_UNKNOWN};

    ComPtr<ID3D11Buffer> post_buffer_;
    ComPtr<ID3D11Buffer> pass_buffer_;
    ComPtr<ID3D11DepthStencilState> no_depth_state_;
    ComPtr<ID3D11DepthStencilState> sky_depth_state_;
    ComPtr<ID3D11BlendState> opaque_blend_;
    ComPtr<ID3D11BlendState> under_blend_;
    ComPtr<ID3D11BlendState> factor_blend_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11SamplerState> point_sampler_;
    ComPtr<ID3D11SamplerState> wrap_sampler_;
    ComPtr<ID3D11ShaderResourceView> noise_view_;

    std::array<Target, 2> surface_;
    std::array<Target, 2> hdr_;
    std::array<Target, 2> ldr_;
    std::array<Target, 2> ao_;
    std::array<Target, 2> volume_;
    std::array<Target, 2> dof_;
    std::array<Target, 2> focus_;
    Target velocity_;
    std::array<Target, post::maximum_bloom_levels> bloom_;
    std::size_t focus_index_{};
    bool focus_primed_{};
    float lens_constant_{};

    std::array<ID3D11ShaderResourceView *, 12> frame_plates_{};
    float plate_second_weight_{};
    bool plates_bound_{};

    std::array<float, 3> previous_eye_{};
    std::array<float, 3> previous_right_{};
    std::array<float, 3> previous_up_{};
    std::array<float, 3> previous_forward_{};
    std::array<float, 4> previous_projection_{};
    bool history_valid_{};
    std::uint32_t frame_index_{};

    // The deep underground's share of the lens (PostFrame::deep_grade) and how
    // much vignette and warmth all of it adds.
    static constexpr float deep_vignette = 0.16F;
    static constexpr float deep_warmth = 0.12F;
    float deep_grade_{};

    float debug_view_{};
    bool targets_stale_{};
    bool trace_{};
    bool timing_active_{};
    std::array<TimingFrame, 6> timing_;
    std::size_t timing_cursor_{};
    std::array<double, pass_count> timing_total_{};
    std::uint64_t timing_samples_{};
};

PostProcessor::PostProcessor(ID3D11Device *device, ID3D11DeviceContext *context)
    : impl_(std::make_unique<Impl>(device, context))
{
}

PostProcessor::~PostProcessor() = default;

bool PostProcessor::ready() const noexcept
{
    return impl_->ready();
}

void PostProcessor::set_effects(const PostEffects &effects) noexcept
{
    impl_->set_effects(effects);
}

const PostEffects &PostProcessor::effects() const noexcept
{
    return impl_->effects();
}

bool PostProcessor::resize(int width, int height, DXGI_FORMAT output_format)
{
    return impl_->resize(width, height, output_format);
}

ID3D11RenderTargetView *PostProcessor::surface_target(std::size_t index) const noexcept
{
    return impl_->surface_target(index);
}

void PostProcessor::clear_surface_targets()
{
    impl_->clear_surface_targets();
}

void PostProcessor::set_sky_plates(const std::array<ID3D11ShaderResourceView *, 12> &plates,
                                   float second_weight, bool bound) noexcept
{
    impl_->set_plates(plates, second_weight, bound);
}

ID3D11Buffer *PostProcessor::camera_constants() const noexcept
{
    return impl_->camera_constants();
}

ID3D11ShaderResourceView *PostProcessor::surface_view(std::size_t index) const noexcept
{
    return impl_->surface_view(index);
}

void PostProcessor::begin_frame(const PostFrame &frame)
{
    impl_->begin_frame(frame);
}

void PostProcessor::draw_sky(const PostFrame &frame)
{
    impl_->draw_sky(frame);
}

void PostProcessor::run(const PostFrame &frame)
{
    impl_->run(frame);
}

bool PostProcessor::lens_needed() const noexcept
{
    return impl_->lens_needed();
}

void PostProcessor::lens(ID3D11RenderTargetView *target, float elapsed_seconds)
{
    impl_->lens(target, elapsed_seconds);
}

#endif

} // namespace openminerz
