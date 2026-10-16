// The Enhanced presentation's effect renderer (vfx_renderer.hpp).

#include "vfx/vfx_renderer.hpp"

#include "atmosphere_shader.hpp"

#if defined(_WIN32)

#include <SDL3/SDL.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace openminerz
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr int flip_cells = 8;
constexpr int flip_cell_pixels = 128;
constexpr int flip_extent = flip_cells * flip_cell_pixels;
constexpr unsigned noise_extent = 64U;
constexpr std::size_t instance_capacity = 16384U;

// The renderer's scene constants (the first rows of SceneConstants in
// depth_voxel_renderer.cpp, which is all the effects read) and this module's
// own. Shared by every shader below.
constexpr char scene_source[] = R"hlsl(
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
};
cbuffer VfxFrame : register(b1)
{
    // x = time, y = the atmosphere is bound, z = sun cascades are bound,
    // w = flipbook slice being baked.
    float4 VfxParams;
    // Full-resolution and reduced-resolution target: xy = pixels, zw = 1 / pixels.
    float4 VfxSize;
    float4 VfxLowSize;
    // rgb = colour of the smoke round the eye, a = its optical depth per block.
    float4 VfxImmersion;
    // x = how far it reaches, y = skylight, z = block light on it.
    float4 VfxImmersionLight;
    // rgb = colour, a = amount of the flash.
    float4 VfxFlash;
    // x = refraction buffer drawn, y = resolution divisor, z = gas buffer drawn,
    // w = diagnostic view of the flipbook.
    float4 VfxOptions;
};
cbuffer VfxLights : register(b9)
{
    float4 VfxLightCount;
    float4 VfxLightPosRadius[16];
    float4 VfxLightColor[16];
};
float vfx_linear_depth(float raw)
{
    // Projection.z is a little above one and Projection.w negative: both the
    // numerator and the denominator are negative for every depth in range.
    return Projection.w / min(raw - Projection.z, -1.0e-6f);
}
)hlsl";

// Light on a particle, per vertex: sky and ground ambient, the sun through its
// cascades, torches, the deep underground's fill and the effect lights.
constexpr char lighting_source[] = R"hlsl(
Texture2DArray VfxShadowMap : register(t3);
SamplerComparisonState VfxShadowSampler : register(s1);

float vfx_sun_shadow(float3 world)
{
    if (VfxParams.z < 0.5f || ShadowParams.x <= 0.0f || ShadowParams.z <= 0.0f)
        return 1.0f;
    float depth = max(dot(world - Eye.xyz, CameraForward.xyz), 0.0f);
    if (depth >= ShadowCascades.z)
        return 1.0f;
    float4 sx = ShadowX;
    float4 sy = ShadowY;
    float4 sz = ShadowZ;
    float cascade = 0.0f;
    if (depth >= ShadowCascades.y)
    {
        sx = ShadowX3; sy = ShadowY3; sz = ShadowZ3; cascade = 2.0f;
    }
    else if (depth >= ShadowCascades.x)
    {
        sx = ShadowX2; sy = ShadowY2; sz = ShadowZ2; cascade = 1.0f;
    }
    float2 uv = float2(dot(world, sx.xyz) + sx.w, dot(world, sy.xyz) + sy.w);
    if (any(uv < 0.002f) || any(uv > 0.998f))
        return 1.0f;
    float texel_world = ShadowParams.y / max(length(sx.xyz), 0.0001f);
    float receiver = dot(world, sz.xyz) + sz.w - length(sz.xyz) * texel_world * 1.5f;
    float visibility =
        VfxShadowMap.SampleCmpLevelZero(VfxShadowSampler, float3(uv, cascade), receiver);
    float fade = saturate((depth - ShadowCascades.z * 0.88f) / max(ShadowCascades.z * 0.12f, 0.001f));
    visibility = lerp(visibility, 1.0f, fade);
    return lerp(1.0f, visibility, lerp(0.70f, 0.94f, saturate(ShadowParams.x)) * ShadowParams.z);
}

void vfx_light(float3 world, float sky, float block, out float3 ambient_hi,
               out float3 ambient_lo, out float3 sun, out float3 point_color,
               out float3 point_direction)
{
    float daylight = 1.0f + WaterAndTime.w * 0.18f;
    ambient_lo = (AmbientSun.rgb * 0.62f + FogAndEnd.rgb * 0.08f) * sky * daylight;
    ambient_hi = (AmbientSun.rgb * 1.34f + FogAndEnd.rgb * 0.13f) * sky * daylight;
    float3 torch_color = TorchAndEye.rgb * TorchAndEye.rgb;
    float3 torch = torch_color * block * 0.78f * (1.0f - sky * LightAndFogStart.g * 0.85f);
    // The deep underground glows from below.
    float deep = smoothstep(24.0f, 36.0f, -world.y);
    float3 glow = float3(1.10f, 0.20f, 0.075f) * deep;
    ambient_lo += torch + glow * 0.50f;
    ambient_hi += torch + glow * 0.22f;
    sun = LightAndFogStart.rgb * pow(saturate(sky), 4.0f) * vfx_sun_shadow(world);
    point_color = 0.0f;
    point_direction = 0.0f;
    // The held torch.
    float3 to_eye = Eye.xyz - world;
    float held = TorchAndEye.w * pow(saturate(1.0f - length(to_eye) / 9.0f), 2.0f);
    point_color += torch_color * held;
    point_direction += normalize(to_eye + float3(0.0f, 1.0e-4f, 0.0f)) * held;
    int count = (int)VfxLightCount.x;
    [loop]
    for (int index = 0; index < count; ++index)
    {
        float3 offset = VfxLightPosRadius[index].xyz - world;
        float radius = VfxLightPosRadius[index].w;
        float range = length(offset);
        float share = saturate(1.0f - range / max(radius, 0.001f));
        share *= share;
        float3 colour = VfxLightColor[index].rgb * share;
        point_color += colour;
        point_direction += offset / max(range, 0.05f) * dot(colour, float3(0.3f, 0.6f, 0.1f));
    }
}

// What the air between the eye and `world` does to a particle there: a = the
// light that survives, rgb = the light the air adds.
float4 vfx_air(float3 world, float sky)
{
    float3 offset = world - Eye.xyz;
    float range = length(offset);
    float3 direction = offset / max(range, 1.0e-4f);
    float distance_fog = saturate((length(offset.xz) - LightAndFogStart.w) /
                                  max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    distance_fog = distance_fog * distance_fog * (3.0f - 2.0f * distance_fog);
    if (VfxParams.y < 0.5f)
        return float4(FogAndEnd.rgb * distance_fog, 1.0f - distance_fog);
    float transmittance;
    float3 inscatter;
    atmosphere(direction, range, atmosphere_exposure(AtmosMist.w, sky), FogAndEnd.rgb,
               transmittance, inscatter);
    // Underground the air is the cave's own fog.
    float cave = distance_fog * (1.0f - saturate(AtmosMist.w));
    return float4(inscatter * (1.0f - cave) + FogAndEnd.rgb * cave, transmittance * (1.0f - cave));
}
)hlsl";

constexpr char particle_vertex_source[] = R"hlsl(
struct VertexInput
{
    float4 i0 : INST0;
    float4 i1 : INST1;
    float4 i2 : INST2;
    float4 i3 : INST3;
    float4 i4 : INST4;
    uint vertex : SV_VertexID;
};
struct VertexOutput
{
    float4 position : SV_POSITION;
    // xy = corner (-1 .. 1), zw = cosine and sine of the billboard's rotation
    // (z = 1 marks a streak for sparks).
    float4 corner : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
    // frame, temperature, erosion, emissive
    nointerpolation float4 params : TEXCOORD2;
    // material, view depth, size, cold palette
    nointerpolation float4 misc : TEXCOORD3;
    float3 world : TEXCOORD4;
    float3 ambient_hi : TEXCOORD5;
    float3 ambient_lo : TEXCOORD6;
    float3 sun : TEXCOORD7;
    float3 point_color : TEXCOORD8;
    float3 point_direction : TEXCOORD9;
    float4 air : TEXCOORD10;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    float3 center = input.i0.xyz;
    float size = input.i0.w;
    float material = input.i4.z;
    float cold = material > 15.5f ? 1.0f : 0.0f;
    material -= cold * 16.0f;
    float2 corner = float2((input.vertex & 1U) ? 1.0f : -1.0f, (input.vertex & 2U) ? 1.0f : -1.0f);
    float3 relative = center - Eye.xyz;
    float center_depth = dot(relative, CameraForward.xyz);
    float cosine = cos(input.i3.x);
    float sine = sin(input.i3.x);
    float alpha = input.i1.a;
    float3 world;
    output.corner = float4(corner, cosine, sine);
    if (material > 1.5f && material < 2.5f)
    {
        // A spark never gets thinner than most of a pixel; it gets dimmer.
        float pixels = size * Projection.y / max(center_depth, 0.05f) * VfxSize.y * 0.5f;
        float widen = max(1.0f, 0.8f / max(pixels, 1.0e-4f));
        float stretch = input.i2.w;
        float3 axis = input.i2.xyz;
        float3 side = cross(axis, normalize(relative));
        float side_length = length(side);
        if (stretch > 1.0e-4f && side_length > 1.0e-3f)
        {
            side /= side_length;
            world = center + side * (corner.x * size * widen) + axis * (corner.y * (size + stretch));
            alpha /= widen;
            output.corner.zw = float2(1.0f, 0.0f);
        }
        else
        {
            world = center + (CameraRight.xyz * corner.x + CameraUp.xyz * corner.y) * (size * widen);
            alpha /= widen * widen;
            output.corner.zw = float2(0.0f, 0.0f);
        }
        alpha = max(alpha, 0.0f);
    }
    else
    {
        float2 turned = float2(corner.x * cosine - corner.y * sine,
                               corner.x * sine + corner.y * cosine);
        world = center + (CameraRight.xyz * turned.x + CameraUp.xyz * turned.y) * size;
    }
    float3 view = world - Eye.xyz;
    float depth = dot(view, CameraForward.xyz);
    output.position = float4(dot(view, CameraRight.xyz) * Projection.x,
                             dot(view, CameraUp.xyz) * Projection.y,
                             depth * Projection.z + Projection.w, depth);
    output.color = float4(input.i1.rgb, alpha);
    output.params = float4(input.i3.y, input.i3.z, input.i3.w, input.i4.w);
    output.misc = float4(material, center_depth, size, cold);
    output.world = world;
    output.ambient_hi = 0.0f;
    output.ambient_lo = 0.0f;
    output.sun = 0.0f;
    output.point_color = 0.0f;
    output.point_direction = 0.0f;
    if (material < 1.5f)
        vfx_light(world, input.i4.x, input.i4.y, output.ambient_hi, output.ambient_lo,
                  output.sun, output.point_color, output.point_direction);
    output.air = vfx_air(center, input.i4.x);
    return output;
}
)hlsl";

constexpr char particle_pixel_input[] = R"hlsl(
struct PixelInput
{
    float4 position : SV_POSITION;
    float4 corner : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
    nointerpolation float4 params : TEXCOORD2;
    nointerpolation float4 misc : TEXCOORD3;
    float3 world : TEXCOORD4;
    float3 ambient_hi : TEXCOORD5;
    float3 ambient_lo : TEXCOORD6;
    float3 sun : TEXCOORD7;
    float3 point_color : TEXCOORD8;
    float3 point_direction : TEXCOORD9;
    float4 air : TEXCOORD10;
};
)hlsl";

// Gas (smoke and fire from the flipbooks) and glows: premultiplied output, so
// one sorted pass composites smoke over fire over smoke correctly.
constexpr char soft_pixel_source[] = R"hlsl(
Texture2DArray Flip : register(t0);
Texture3D<float> Noise : register(t1);
Texture2D<float> LowDepth : register(t2);
SamplerState FlipSampler : register(s0);
SamplerState WrapSampler : register(s1);

float4 flip_sample(float2 uv, float2 across, float2 down, float frame, float slice)
{
    float index = fmod(floor(frame), 64.0f);
    float2 cell = float2(fmod(index, 8.0f), floor(index / 8.0f));
    return Flip.SampleGrad(FlipSampler, float3((cell + uv) / 8.0f, slice), across, down);
}

float4 main(PixelInput input) : SV_TARGET
{
    float material = input.misc.x;
    float size = input.misc.z;
    float2 corner = input.corner.xy;
    // (Taken before any branch: the flipbook's mip level.)
    float2 across = ddx(corner) * float2(0.0625f, -0.0625f);
    float2 down = ddy(corner) * float2(0.0625f, -0.0625f);
    // Soft against the scene: fade over a share of the particle's own size.
    float scene_depth = LowDepth.Load(int3(int2(input.position.xy), 0));
    float soft = saturate((scene_depth - input.misc.y) / max(size * 0.6f, 0.10f));
    clip(soft - 0.001f);
    float transmittance = input.air.a;
    if (material > 2.5f)
    {
        float glow = pow(saturate(1.0f - length(corner)), 2.2f);
        float3 light = input.color.rgb * (input.params.w * glow * input.color.a * soft);
        if (input.misc.w > 0.5f)
            light = light.bgr;
        return float4(light * transmittance, 0.0f);
    }
    float2 uv = float2(corner.x, -corner.y) * 0.5f + 0.5f;
    float frame = input.params.x;
    float4 flip = lerp(flip_sample(uv, across, down, frame, material),
                       flip_sample(uv, across, down, frame + 1.0f, material), frac(frame));
    // Noise fixed in the world that the puff drifts through eats its edge, so
    // it churns and tears instead of sliding about as a picture.
    float detail = Noise.SampleLevel(WrapSampler, input.world / (52.0f * max(size, 0.08f)) +
                                                      float3(0.0f, frame * 0.004f, 0.0f), 0.0f);
    if (VfxOptions.w > 0.5f)
    {
        // Diagnostic (OPENMINERZ_VFX_DEBUG=flip): the flipbook itself.
        float shown = flip.a * soft;
        return float4(flip.rgb * shown, shown);
    }
    float density = flip.a * (0.84f + 0.36f * detail);
    float erosion = input.params.z;
    float alpha = saturate((density - erosion) / max(1.0f - erosion, 0.1f));
    alpha *= input.color.a * soft;
    clip(alpha - 0.002f);

    float3 flat_normal;
    flat_normal.xy = flip.xy * 2.0f - 1.0f;
    flat_normal.z = sqrt(saturate(1.0f - dot(flat_normal.xy, flat_normal.xy)));
    float cosine = input.corner.z;
    float sine = input.corner.w;
    float3 normal = normalize(
        CameraRight.xyz * (flat_normal.x * cosine - flat_normal.y * sine) +
        CameraUp.xyz * (flat_normal.x * sine + flat_normal.y * cosine) -
        CameraForward.xyz * flat_normal.z);
    float thickness = flip.b;
    float3 sun_direction = normalize(SunAndOptions.xyz);
    float3 to_eye = normalize(Eye.xyz - input.world);
    float facing = saturate((dot(normal, sun_direction) + 0.25f) / 1.25f);
    // Thin smoke with the sun behind it lights up.
    float rim = pow(saturate(dot(-to_eye, sun_direction)), 5.0f) * (1.0f - thickness) * 0.9f;
    float up = normal.y * 0.5f + 0.5f;
    float3 light = lerp(input.ambient_lo, input.ambient_hi, up) *
                   lerp(1.0f, 0.62f, thickness * (1.0f - up));
    light += input.sun * (facing * facing * 1.15f + rim);
    float point_length = length(input.point_direction);
    float point_facing = point_length > 1.0e-4f
        ? saturate((dot(normal, input.point_direction / point_length) + 0.7f) / 1.7f)
        : 1.0f;
    light += input.point_color * point_facing;
    float3 colour = input.color.rgb * light * alpha;

    float temperature = input.params.y;
    if (temperature > 0.001f)
    {
        // The core of a puff burns hottest, and a finer noise breaks its
        // surface into bright cells with dark soot between them: fire that
        // rolls, not a yellow cloud.
        float fine = Noise.SampleLevel(WrapSampler,
                                       input.world / (11.0f * max(size, 0.08f)) +
                                           float3(0.31f, frame * 0.011f, 0.57f), 0.0f);
        float cells = saturate((detail * 0.62f + fine * 0.38f) * 2.3f - 0.55f);
        float core = saturate(flip.a * 1.1f + thickness * 0.6f - 0.25f);
        float heat = temperature * core * (0.22f + 1.30f * cells);
        float3 fire = float3(1.0f, 0.16f, 0.02f) * saturate(heat * 1.6f) +
                      float3(1.0f, 0.50f, 0.06f) * saturate(heat - 0.5f) * 2.0f +
                      float3(0.9f, 0.85f, 0.55f) * saturate(heat - 1.3f) * 2.6f;
        if (input.misc.w > 0.5f)
            fire = fire.bgr * float3(0.75f, 1.0f, 1.25f);
        float hot = saturate(heat * 1.2f);
        colour = colour * (1.0f - 0.6f * hot) + fire * (input.params.w * alpha);
        alpha *= lerp(1.0f, 0.55f, hot);
    }
    return float4(colour * transmittance + input.air.rgb * alpha, alpha);
}
)hlsl";

// Sparks and embers: additive, full resolution.
constexpr char sharp_pixel_source[] = R"hlsl(
Texture2D<float> Depth : register(t2);

float4 main(PixelInput input) : SV_TARGET
{
    float scene_depth = vfx_linear_depth(Depth.Load(int3(int2(input.position.xy), 0)));
    float soft = saturate((scene_depth - input.misc.y) / 0.08f);
    clip(soft - 0.001f);
    float2 corner = input.corner.xy;
    float shape;
    if (input.corner.z > 0.5f)
    {
        float head = corner.y * 0.5f + 0.5f;
        shape = exp(-corner.x * corner.x * 5.0f) * head * head *
                saturate((1.0f - abs(corner.y)) * 8.0f);
    }
    else
    {
        float radius = length(corner);
        shape = exp(-radius * radius * 4.5f) * saturate((1.0f - radius) * 4.0f);
    }
    float amount = input.params.w * input.color.a * soft;
    float3 light = input.color.rgb * (shape * amount) + (shape * shape * shape) * amount * 0.25f;
    if (input.misc.w > 0.5f)
        light = light.bgr;
    return float4(light * input.air.a, 0.0f);
}
)hlsl";

// Shockwaves and hot air: screen-space offsets, accumulated additively.
constexpr char distort_pixel_source[] = R"hlsl(
Texture3D<float> Noise : register(t1);
Texture2D<float> LowDepth : register(t2);
SamplerState WrapSampler : register(s1);

float4 main(PixelInput input) : SV_TARGET
{
    float size = input.misc.z;
    float scene_depth = LowDepth.Load(int3(int2(input.position.xy), 0));
    float soft = saturate((scene_depth - input.misc.y) / max(size * 0.3f, 0.2f));
    clip(soft - 0.001f);
    float2 corner = input.corner.xy;
    float radius = length(corner);
    float2 offset;
    if (input.misc.x < 4.5f)
    {
        float ring = smoothstep(0.50f, 0.86f, radius) * (1.0f - smoothstep(0.86f, 1.0f, radius));
        offset = corner / max(radius, 1.0e-3f) * ring;
    }
    else
    {
        float3 point_ = input.world * 0.045f + float3(0.0f, -VfxParams.x * 0.09f, 0.0f);
        float mask = saturate(1.0f - radius * radius);
        offset = float2(Noise.SampleLevel(WrapSampler, point_, 0.0f) - 0.5f,
                        Noise.SampleLevel(WrapSampler, point_ + 0.37f, 0.0f) - 0.5f) *
                 (mask * mask * 2.0f);
    }
    // No stronger on screen than the thing itself is large there.
    float apparent = saturate(size * Projection.y / max(input.misc.y, 0.1f) * 0.6f);
    offset *= input.params.w * soft * apparent;
    return float4(offset.x * VfxSize.y / VfxSize.x, -offset.y, 0.0f, 0.0f);
}
)hlsl";

constexpr char fullscreen_vertex_source[] = R"hlsl(
struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 immersion : TEXCOORD1;
};
FullscreenOutput main(uint vertex : SV_VertexID)
{
    FullscreenOutput output;
    float2 uv = float2((vertex << 1U) & 2U, vertex & 2U);
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    output.immersion = 0.0f;
#if defined(VFX_IMMERSION)
    // The smoke the eye stands in, lit once where the eye is.
    float3 ambient_hi;
    float3 ambient_lo;
    float3 sun;
    float3 point_color;
    float3 point_direction;
    vfx_light(Eye.xyz, VfxImmersionLight.y, VfxImmersionLight.z, ambient_hi, ambient_lo, sun,
              point_color, point_direction);
    output.immersion = VfxImmersion.rgb *
                       ((ambient_hi + ambient_lo) * 0.5f + sun * 0.40f + point_color);
#endif
    return output;
}
)hlsl";

constexpr char fullscreen_pixel_input[] = R"hlsl(
struct FullscreenInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 immersion : TEXCOORD1;
};
)hlsl";

// Scene depth to linear view depth at the gas buffer's resolution: the
// farthest of each block, so gas behind an edge is drawn whole and the
// upsample decides per pixel who shows it.
constexpr char depth_pixel_source[] = R"hlsl(
Texture2D<float> Depth : register(t0);
float main(FullscreenInput input) : SV_TARGET
{
    int divisor = (int)VfxOptions.y;
    int2 base = int2(input.position.xy) * divisor;
    int2 limit = int2(VfxSize.xy) - 1;
    float farthest = 0.0f;
    for (int row = 0; row < divisor; ++row)
        for (int column = 0; column < divisor; ++column)
            farthest = max(farthest, vfx_linear_depth(
                Depth.Load(int3(min(base + int2(column, row), limit), 0))));
    return farthest;
}
)hlsl";

constexpr char composite_pixel_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D Low : register(t2);
Texture2D<float> LowDepth : register(t3);
Texture2D Distort : register(t4);
Texture3D<float> Noise : register(t5);
SamplerState LinearSampler : register(s0);
SamplerState WrapSampler : register(s1);

float4 main(FullscreenInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float2 uv = input.position.xy * VfxSize.zw;
    float depth = vfx_linear_depth(Depth.Load(int3(pixel, 0)));
    float2 offset = 0.0f;
    if (VfxOptions.x > 0.5f)
    {
        offset = Distort.SampleLevel(LinearSampler, uv, 0.0f).xy;
        if (dot(offset, offset) > 1.0e-10f)
        {
            // Never drag something much nearer than this pixel across it.
            float2 bent = saturate(uv + offset);
            float there = vfx_linear_depth(Depth.Load(int3(int2(bent * VfxSize.xy), 0)));
            if (there < depth * 0.6f - 0.3f)
                offset = 0.0f;
        }
    }
    float3 scene = Scene.SampleLevel(LinearSampler, uv + offset, 0.0f).rgb;

    if (VfxImmersion.a > 0.0f)
    {
        // Inside smoke: the world dims and loses colour with distance behind
        // one soft fog of the smoke's own lit colour.
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float3 direction = normalize(CameraForward.xyz + CameraRight.xyz * (ndc.x / Projection.x) +
                                     CameraUp.xyz * (ndc.y / Projection.y));
        // The smoke is not even: three looks at a drifting noise along the
        // sight line give it wisps that slide past as the eye moves.
        float span = min(depth, VfxImmersionLight.x);
        float3 drift = float3(VfxParams.x * 0.006f, -VfxParams.x * 0.012f, 0.0f);
        float wisps = 0.0f;
        for (int look = 0; look < 3; ++look)
        {
            float3 point_ = Eye.xyz + direction * (span * ((float)look + 0.5f) / 3.0f);
            float broad = Noise.SampleLevel(WrapSampler, point_ * 0.011f + drift, 0.0f);
            float fine = Noise.SampleLevel(WrapSampler, point_ * 0.034f - drift * 1.7f, 0.0f);
            wisps += (0.30f + 1.40f * broad) * (0.55f + 0.90f * fine);
        }
        // Bounded: however thick the smoke, what stands within arm's reach
        // stays readable through it and the far side never goes fully flat.
        float optical = min(VfxImmersion.a * span * wisps / 3.0f, 1.9f);
        float clear = exp(-optical);
        float grey = dot(scene, float3(0.2126f, 0.7152f, 0.0722f));
        scene = lerp(scene, lerp(scene, grey.xxx, 0.75f), 1.0f - clear);
        scene = scene * clear + input.immersion * (1.0f - clear);
    }

    if (VfxOptions.z > 0.5f)
    {
        // Depth-aware upsample of the gas buffer.
        float2 low = uv * VfxLowSize.xy - 0.5f;
        float2 base = floor(low);
        float2 share = low - base;
        int2 limit = int2(VfxLowSize.xy) - 1;
        float4 gas = 0.0f;
        float total = 0.0f;
        for (int row = 0; row < 2; ++row)
            for (int column = 0; column < 2; ++column)
            {
                int2 tap = clamp(int2(base) + int2(column, row), int2(0, 0), limit);
                float tap_depth = LowDepth.Load(int3(tap, 0));
                float weight = (column ? share.x : 1.0f - share.x) * (row ? share.y : 1.0f - share.y);
                weight = (weight + 0.02f) /
                         (0.02f + abs(tap_depth - depth) / max(depth, 0.5f) * 30.0f);
                gas += Low.Load(int3(tap, 0)) * weight;
                total += weight;
            }
        gas /= total;
        scene = scene * (1.0f - gas.a) + gas.rgb;
    }
    // The flash opens the picture up like an over-exposed frame (and adds a
    // touch of its own colour), not a veil laid over it.
    scene = scene * (1.0f + VfxFlash.a * 1.1f) + VfxFlash.rgb * (VfxFlash.a * 0.10f);
    return float4(scene, 1.0f);
}
)hlsl";

// Flipbook bake, first pass: march an animated noise density for every frame
// and keep where its surface is (x), how thick it is (y) and its opacity (z).
constexpr char bake_density_source[] = R"hlsl(
Texture3D<float> Noise : register(t1);
SamplerState WrapSampler : register(s1);

float noise3(float3 point_)
{
    return Noise.SampleLevel(WrapSampler, point_ / 64.0f, 0.0f);
}
float fbm2(float3 point_)
{
    return noise3(point_) * 0.62f + noise3(point_ * 2.03f + 17.1f) * 0.31f;
}
float fbm4(float3 point_)
{
    return noise3(point_) * 0.5f + noise3(point_ * 2.03f + 17.1f) * 0.25f +
           noise3(point_ * 4.01f + 41.3f) * 0.125f + noise3(point_ * 8.07f + 7.7f) * 0.0625f;
}
float smoke_density(float3 point_, float drift)
{
    // A few large, gently warped lobes make the outline (a billow, not a ball
    // of fluff or a bundle of streaks); the fine octaves only vary how dense
    // the inside is.
    float3 lattice = point_ * 2.6f + float3(5.3f, 9.1f, drift);
    float3 warp = float3(fbm2(lattice + 31.7f), fbm2(lattice + 67.3f), fbm2(lattice + 11.9f)) - 0.43f;
    lattice += warp * 0.9f;
    float body = 1.0f - smoothstep(0.30f, 0.95f, length(point_) + (fbm2(lattice) - 0.43f) * 1.15f);
    return body * (0.70f + 0.45f * fbm4(lattice * 2.3f + 5.0f));
}
float flame_density(float3 point_, float drift)
{
    float height = point_.y * 0.5f + 0.5f;
    float3 lattice = float3(point_.x * 3.0f, point_.y * 1.7f - drift, point_.z * 3.0f) + 3.7f;
    float sway = fbm2(lattice * 0.7f + 13.0f) - 0.47f;
    float2 across = point_.xz + sway * 0.36f * height;
    float width = lerp(0.60f, 0.09f, pow(height, 0.8f));
    float body = 1.0f - smoothstep(0.30f, 1.0f, length(across) / width +
                                                      (fbm4(lattice) - 0.47f) * 1.4f * (0.4f + height));
    float tip = 1.0f - smoothstep(0.72f, 0.97f, height + (fbm2(lattice * 1.3f + 3.0f) - 0.47f) * 0.5f);
    return body * tip * smoothstep(0.0f, 0.10f, height);
}
float density_at(float3 point_, float time)
{
    // Two copies of the drifting field cross-faded, so frame 63 runs into 0.
    if (VfxParams.w < 0.5f)
        return lerp(smoke_density(point_, time * 5.0f + 2.0f),
                    smoke_density(point_, (time - 1.0f) * 5.0f + 2.0f), time);
    return lerp(flame_density(point_, time * 9.0f), flame_density(point_, (time - 1.0f) * 9.0f), time);
}

float4 main(FullscreenInput input) : SV_TARGET
{
    float2 cell = floor(input.position.xy / 128.0f);
    float2 inside = frac(input.position.xy / 128.0f) * 2.0f - 1.0f;
    float time = (cell.y * 8.0f + cell.x) / 64.0f;
    float2 plane = float2(inside.x, -inside.y);
    const int steps = 16;
    float clear = 1.0f;
    float surface = 0.0f;
    float thickness = 0.0f;
    for (int index = 0; index < steps; ++index)
    {
        float along = 1.0f - ((float)index + 0.5f) * 2.0f / (float)steps;
        float density = density_at(float3(plane, along), time);
        float opacity = 1.0f - exp(-density * (2.0f / (float)steps) * 5.0f);
        surface += clear * opacity * along;
        thickness += density * (2.0f / (float)steps);
        clear *= 1.0f - opacity;
    }
    float alpha = 1.0f - clear;
    // Nothing may reach the cell's edge, or neighbouring frames would bleed.
    alpha *= saturate((1.0f - max(abs(inside.x), abs(inside.y))) * 10.0f);
    return float4(alpha > 1.0e-4f ? surface / (1.0f - clear + 1.0e-4f) : -1.0f,
                  saturate(thickness * 0.75f), alpha, 1.0f);
}
)hlsl";

// Second pass: a normal from the slope of that surface.
constexpr char bake_normal_source[] = R"hlsl(
Texture2D Baked : register(t0);

float surface_at(int2 pixel, int2 cell_origin)
{
    pixel = clamp(pixel, cell_origin, cell_origin + 127);
    float4 baked = Baked.Load(int3(pixel, 0));
    // Where the puff thins out its surface falls away, which rounds the rim.
    return lerp(-1.0f, baked.x, baked.z);
}

float4 main(FullscreenInput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    int2 cell_origin = (pixel / 128) * 128;
    float4 baked = Baked.Load(int3(pixel, 0));
    // The slope of the surface over two widths, so the relief is the puff's
    // lobes and not the grain of the march. (A texel is 1 / 64 of the puff's
    // half-width.)
    float2 slope = 0.0f;
    slope.x = (surface_at(pixel + int2(4, 0), cell_origin) -
               surface_at(pixel + int2(-4, 0), cell_origin)) / 8.0f +
              (surface_at(pixel + int2(9, 0), cell_origin) -
               surface_at(pixel + int2(-9, 0), cell_origin)) / 18.0f;
    slope.y = (surface_at(pixel + int2(0, -4), cell_origin) -
               surface_at(pixel + int2(0, 4), cell_origin)) / 8.0f +
              (surface_at(pixel + int2(0, -9), cell_origin) -
               surface_at(pixel + int2(0, 9), cell_origin)) / 18.0f;
    float3 relief = normalize(float3(-slope * (32.0f * 0.55f), 1.0f));
    // Over the round of a ball, which is what a puff is from any side.
    float2 inside = (float2(pixel - cell_origin) + 0.5f) / 64.0f - 1.0f;
    float3 ball = normalize(float3(inside.x, -inside.y,
                                   sqrt(max(1.0f - dot(inside, inside), 0.06f))));
    float3 normal = normalize(lerp(ball, relief, 0.68f));
    return float4(normal.xy * 0.5f + 0.5f, baked.y, baked.z);
}
)hlsl";

struct FrameConstants final
{
    float params[4];
    float size[4];
    float low_size[4];
    float immersion[4];
    float immersion_light[4];
    float flash[4];
    float options[4];
};
static_assert(sizeof(FrameConstants) % 16U == 0U);

struct LightConstants final
{
    float count[4];
    float position_radius[vfx_max_lights][4];
    float color[vfx_max_lights][4];
};
static_assert(sizeof(LightConstants) % 16U == 0U);

struct Surface final
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};

} // namespace

class VfxRenderer::Impl final
{
  public:
    Impl(ID3D11Device *device, ID3D11DeviceContext *context) : device_(device), context_(context)
    {
        frame_.soft.reserve(instance_capacity);
        frame_.sharp.reserve(instance_capacity / 2U);
        frame_.distortion.reserve(256U);
        frame_.debris.reserve(1024U);
        frame_.lights.reserve(64U);
        ready_ = create_shaders() && create_states() && create_buffers() && create_noise() &&
                 bake_flipbooks();
        if (!ready_)
            SDL_Log("Effects renderer unavailable: %s", failure_.c_str());
    }

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    [[nodiscard]] const char *failure() const noexcept
    {
        return failure_.c_str();
    }
    [[nodiscard]] const vfx::Frame &frame() const noexcept
    {
        return frame_;
    }

    void submit(vfx::Frame &frame, const VfxOptions &options)
    {
        std::swap(frame_, frame);
        options_ = options;
        submitted_ = true;
    }

    void begin_frame(bool enabled)
    {
        if (!ready_)
            return;
        active_ = enabled && submitted_;
        LightConstants constants{};
        if (active_)
        {
            const std::size_t count =
                std::min<std::size_t>({frame_.lights.size(), options_.max_lights, vfx_max_lights});
            constants.count[0] = static_cast<float>(count);
            for (std::size_t index = 0U; index < count; ++index)
            {
                const vfx::PointLight &light = frame_.lights[index];
                constants.position_radius[index][0] = light.position.x;
                constants.position_radius[index][1] = light.position.y;
                constants.position_radius[index][2] = light.position.z;
                constants.position_radius[index][3] = light.radius;
                constants.color[index][0] = light.color.r;
                constants.color[index][1] = light.color.g;
                constants.color[index][2] = light.color.b;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(light_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
        {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context_->Unmap(light_buffer_.Get(), 0U);
        }
        ID3D11Buffer *buffer = light_buffer_.Get();
        context_->PSSetConstantBuffers(vfx_light_slot, 1U, &buffer);
        context_->VSSetConstantBuffers(vfx_light_slot, 1U, &buffer);
    }

    [[nodiscard]] bool wants_draw() const noexcept
    {
        return ready_ && active_ &&
               (!frame_.soft.empty() || !frame_.sharp.empty() ||
                (options_.distortion && !frame_.distortion.empty()) || frame_.immersion > 0.002F ||
                frame_.flash * options_.flash > 0.002F);
    }

    void draw(const VfxPass &pass)
    {
        if (!wants_draw() || pass.target == nullptr || pass.scene == nullptr ||
            pass.depth == nullptr)
            return;
        const int divisor = std::clamp(options_.resolution_divisor, 1, 2);
        const int low_width = std::max(1, pass.width / divisor);
        const int low_height = std::max(1, pass.height / divisor);
        if (!ensure_targets(low_width, low_height))
            return;
        const bool gas = !frame_.soft.empty();
        const bool bend = options_.distortion && !frame_.distortion.empty();

        FrameConstants constants{};
        constants.params[0] = pass.elapsed_seconds;
        constants.params[1] = pass.atmosphere ? 1.0F : 0.0F;
        constants.params[2] = pass.shadow != nullptr ? 1.0F : 0.0F;
        constants.size[0] = static_cast<float>(pass.width);
        constants.size[1] = static_cast<float>(pass.height);
        constants.size[2] = 1.0F / static_cast<float>(pass.width);
        constants.size[3] = 1.0F / static_cast<float>(pass.height);
        constants.low_size[0] = static_cast<float>(low_width);
        constants.low_size[1] = static_cast<float>(low_height);
        constants.low_size[2] = 1.0F / static_cast<float>(low_width);
        constants.low_size[3] = 1.0F / static_cast<float>(low_height);
        constants.immersion[0] = frame_.immersion_color.r;
        constants.immersion[1] = frame_.immersion_color.g;
        constants.immersion[2] = frame_.immersion_color.b;
        constants.immersion[3] = frame_.immersion > 0.002F ? std::min(frame_.immersion, 3.0F) : 0.0F;
        constants.immersion_light[0] = std::max(frame_.immersion_reach, 0.5F);
        constants.immersion_light[1] = frame_.immersion_light.sky;
        constants.immersion_light[2] = frame_.immersion_light.block;
        const float flash = std::clamp(frame_.flash * options_.flash, 0.0F, 1.0F);
        constants.flash[0] = frame_.flash_color.r;
        constants.flash[1] = frame_.flash_color.g;
        constants.flash[2] = frame_.flash_color.b;
        constants.flash[3] = flash * 1.6F;
        constants.options[0] = bend ? 1.0F : 0.0F;
        constants.options[1] = static_cast<float>(divisor);
        constants.options[2] = gas ? 1.0F : 0.0F;
        constants.options[3] = debug_flip_ ? 1.0F : 0.0F;
        write_constants(constants);

        ID3D11Buffer *const buffers[2]{pass.scene_constants, frame_buffer_.Get()};
        context_->VSSetConstantBuffers(0U, 2U, buffers);
        context_->PSSetConstantBuffers(0U, 2U, buffers);
        ID3D11Buffer *lights = light_buffer_.Get();
        context_->VSSetConstantBuffers(vfx_light_slot, 1U, &lights);
        // The atmosphere's constants are bound for pixel shaders; the effects
        // fog per vertex.
        ID3D11Buffer *atmosphere{};
        context_->PSGetConstantBuffers(atmosphere_constant_slot, 1U, &atmosphere);
        context_->VSSetConstantBuffers(atmosphere_constant_slot, 1U, &atmosphere);
        if (atmosphere != nullptr)
            atmosphere->Release();
        context_->VSSetShaderResources(3U, 1U, &pass.shadow);
        context_->VSSetSamplers(1U, 1U, &pass.shadow_sampler);
        ID3D11SamplerState *const samplers[2]{flip_sampler_.Get(), wrap_sampler_.Get()};
        context_->PSSetSamplers(0U, 2U, samplers);
        context_->RSSetState(raster_.Get());
        context_->OMSetDepthStencilState(no_depth_.Get(), 0U);
        context_->GSSetShader(nullptr, nullptr, 0U);

        // 1) Linear depth at the gas buffer's resolution.
        set_viewport(low_width, low_height);
        fullscreen(depth_shader_.Get(), fullscreen_vertex_.Get(), low_depth_.target.Get(),
                   {pass.depth});

        // 2) Gas and glows.
        const float clear[4]{0.0F, 0.0F, 0.0F, 0.0F};
        if (gas)
        {
            context_->ClearRenderTargetView(low_color_.target.Get(), clear);
            ID3D11RenderTargetView *target = low_color_.target.Get();
            context_->OMSetRenderTargets(1U, &target, nullptr);
            context_->OMSetBlendState(premultiplied_.Get(), nullptr, 0xFFFFFFFFU);
            ID3D11ShaderResourceView *const views[3]{flip_view_.Get(), noise_view_.Get(),
                                                     low_depth_.view.Get()};
            context_->PSSetShaderResources(0U, 3U, views);
            draw_instances(frame_.soft, soft_shader_.Get());
            unbind_pixel_views();
        }
        // 3) Refraction.
        if (bend)
        {
            context_->ClearRenderTargetView(distort_.target.Get(), clear);
            ID3D11RenderTargetView *target = distort_.target.Get();
            context_->OMSetRenderTargets(1U, &target, nullptr);
            context_->OMSetBlendState(additive_.Get(), nullptr, 0xFFFFFFFFU);
            ID3D11ShaderResourceView *const views[3]{nullptr, noise_view_.Get(),
                                                     low_depth_.view.Get()};
            context_->PSSetShaderResources(0U, 3U, views);
            draw_instances(frame_.distortion, distort_shader_.Get());
            unbind_pixel_views();
        }
        // 4) Composite over the scene.
        set_viewport(pass.width, pass.height);
        ID3D11SamplerState *const composite_samplers[2]{clamp_sampler_.Get(), wrap_sampler_.Get()};
        context_->PSSetSamplers(0U, 2U, composite_samplers);
        fullscreen(composite_shader_.Get(), composite_vertex_.Get(), pass.target,
                   {pass.scene, pass.depth, low_color_.view.Get(), low_depth_.view.Get(),
                    distort_.view.Get(), noise_view_.Get()});
        // 5) Sparks and embers.
        if (!frame_.sharp.empty())
        {
            context_->OMSetRenderTargets(1U, &pass.target, nullptr);
            context_->OMSetBlendState(additive_.Get(), nullptr, 0xFFFFFFFFU);
            ID3D11ShaderResourceView *const views[3]{nullptr, nullptr, pass.depth};
            context_->PSSetShaderResources(0U, 3U, views);
            draw_instances(frame_.sharp, sharp_shader_.Get());
            unbind_pixel_views();
        }
        ID3D11ShaderResourceView *none{};
        context_->VSSetShaderResources(3U, 1U, &none);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void unbind()
    {
        ID3D11Buffer *none{};
        context_->PSSetConstantBuffers(vfx_light_slot, 1U, &none);
        context_->VSSetConstantBuffers(vfx_light_slot, 1U, &none);
    }

  private:
    [[nodiscard]] ComPtr<ID3DBlob> compile(const char *name, const std::string &source,
                                           const char *target)
    {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main", target,
                              D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U,
                              &shader, &errors)))
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

    [[nodiscard]] bool pixel(const char *name, const std::string &source,
                             ComPtr<ID3D11PixelShader> &shader)
    {
        const ComPtr<ID3DBlob> blob = compile(name, source, "ps_5_0");
        return blob != nullptr &&
               SUCCEEDED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    nullptr, &shader));
    }

    [[nodiscard]] bool vertex(const char *name, const std::string &source,
                              ComPtr<ID3D11VertexShader> &shader, ComPtr<ID3DBlob> *keep = nullptr)
    {
        const ComPtr<ID3DBlob> blob = compile(name, source, "vs_5_0");
        if (blob == nullptr ||
            FAILED(device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                               nullptr, &shader)))
            return false;
        if (keep != nullptr)
            *keep = blob;
        return true;
    }

    [[nodiscard]] bool create_shaders()
    {
        const std::string scene{scene_source};
        const std::string lit = scene + atmosphere_hlsl + lighting_source;
        const std::string particle_pixel = scene + particle_pixel_input;
        const std::string screen_pixel = scene + fullscreen_pixel_input;
        ComPtr<ID3DBlob> particle_blob;
        if (!vertex("vfx particle vertex", lit + particle_vertex_source, particle_vertex_,
                    &particle_blob) ||
            !vertex("vfx fullscreen vertex", scene + fullscreen_vertex_source, fullscreen_vertex_) ||
            !vertex("vfx composite vertex",
                    std::string{"#define VFX_IMMERSION 1\n"} + lit + fullscreen_vertex_source,
                    composite_vertex_) ||
            !pixel("vfx gas", particle_pixel + soft_pixel_source, soft_shader_) ||
            !pixel("vfx sparks", particle_pixel + sharp_pixel_source, sharp_shader_) ||
            !pixel("vfx refraction", particle_pixel + distort_pixel_source, distort_shader_) ||
            !pixel("vfx depth", screen_pixel + depth_pixel_source, depth_shader_) ||
            !pixel("vfx composite", screen_pixel + composite_pixel_source, composite_shader_) ||
            !pixel("vfx bake density", screen_pixel + bake_density_source, bake_density_shader_) ||
            !pixel("vfx bake normal", screen_pixel + bake_normal_source, bake_normal_shader_))
            return false;
        std::array<D3D11_INPUT_ELEMENT_DESC, 5> elements{};
        for (UINT index = 0U; index < elements.size(); ++index)
            elements[index] = {"INST", index, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, index * 16U,
                               D3D11_INPUT_PER_INSTANCE_DATA, 1U};
        if (FAILED(device_->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                              particle_blob->GetBufferPointer(),
                                              particle_blob->GetBufferSize(), &layout_)))
        {
            failure_ = "could not create the particle input layout";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_states()
    {
        D3D11_BLEND_DESC blend{};
        auto &target = blend.RenderTarget[0];
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_ZERO;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_ZERO;
        if (FAILED(device_->CreateBlendState(&blend, &opaque_)))
            return false;
        target.BlendEnable = TRUE;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (FAILED(device_->CreateBlendState(&blend, &premultiplied_)))
            return false;
        target.DestBlend = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device_->CreateBlendState(&blend, &additive_)))
            return false;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC depth{};
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateRasterizerState(&raster, &raster_)) ||
            FAILED(device_->CreateDepthStencilState(&depth, &no_depth_)) ||
            FAILED(device_->CreateSamplerState(&sampler, &clamp_sampler_)))
            return false;
        // Past the third mip neighbouring flipbook frames would blur together.
        sampler.MaxLOD = 2.0F;
        if (FAILED(device_->CreateSamplerState(&sampler, &flip_sampler_)))
            return false;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        if (FAILED(device_->CreateSamplerState(&sampler, &wrap_sampler_)))
        {
            failure_ = "could not create the effect pipeline states";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_buffers()
    {
        D3D11_BUFFER_DESC description{};
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        description.ByteWidth = sizeof(FrameConstants);
        if (FAILED(device_->CreateBuffer(&description, nullptr, &frame_buffer_)))
            return false;
        description.ByteWidth = sizeof(LightConstants);
        if (FAILED(device_->CreateBuffer(&description, nullptr, &light_buffer_)))
            return false;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.ByteWidth = static_cast<UINT>(instance_capacity * sizeof(vfx::Instance));
        if (FAILED(device_->CreateBuffer(&description, nullptr, &instance_buffer_)))
        {
            failure_ = "could not allocate the particle instance buffer";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_noise()
    {
        std::vector<std::uint8_t> noise(static_cast<std::size_t>(noise_extent) * noise_extent *
                                        noise_extent);
        std::uint32_t state = 0x2F6E2B1DU;
        for (std::uint8_t &value : noise)
        {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            value = static_cast<std::uint8_t>(state >> 24U);
        }
        D3D11_TEXTURE3D_DESC description{};
        description.Width = description.Height = description.Depth = noise_extent;
        description.MipLevels = 1U;
        description.Format = DXGI_FORMAT_R8_UNORM;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{noise.data(), noise_extent, noise_extent * noise_extent};
        ComPtr<ID3D11Texture3D> texture;
        if (FAILED(device_->CreateTexture3D(&description, &data, &texture)) ||
            FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &noise_view_)))
        {
            failure_ = "could not create the effect noise volume";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_surface(Surface &surface, int width, int height, DXGI_FORMAT format)
    {
        surface = {};
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = format;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device_->CreateTexture2D(&description, nullptr, &surface.texture)) &&
               SUCCEEDED(device_->CreateRenderTargetView(surface.texture.Get(), nullptr,
                                                         &surface.target)) &&
               SUCCEEDED(device_->CreateShaderResourceView(surface.texture.Get(), nullptr,
                                                           &surface.view));
    }

    [[nodiscard]] bool ensure_targets(int width, int height)
    {
        if (low_color_.view != nullptr && low_width_ == width && low_height_ == height)
            return true;
        low_width_ = 0;
        low_height_ = 0;
        if (!create_surface(low_color_, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !create_surface(low_depth_, width, height, DXGI_FORMAT_R32_FLOAT) ||
            !create_surface(distort_, width, height, DXGI_FORMAT_R16G16_FLOAT))
            return false;
        low_width_ = width;
        low_height_ = height;
        return true;
    }

    // The two flipbooks, 64 frames each, drawn once from noise.
    [[nodiscard]] bool bake_flipbooks()
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = flip_extent;
        description.MipLevels = 0U;
        description.ArraySize = 2U;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> flip;
        Surface scratch;
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &flip)) ||
            FAILED(device_->CreateShaderResourceView(flip.Get(), nullptr, &flip_view_)) ||
            !create_surface(scratch, flip_extent, flip_extent, DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            failure_ = "could not allocate the effect flipbooks";
            return false;
        }
        // The device context belongs to whoever is drawing; put back what the
        // bake changes.
        struct Saved final
        {
            ComPtr<ID3D11RenderTargetView> target;
            ComPtr<ID3D11DepthStencilView> depth;
            ComPtr<ID3D11InputLayout> layout;
            ComPtr<ID3D11VertexShader> vertex;
            ComPtr<ID3D11PixelShader> pixel;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth_state;
            ComPtr<ID3D11RasterizerState> raster;
            ComPtr<ID3D11Buffer> constants;
            ComPtr<ID3D11SamplerState> sampler;
            std::array<ComPtr<ID3D11ShaderResourceView>, 2> views;
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            D3D11_VIEWPORT viewport{};
            UINT viewports{1U};
            FLOAT factor[4]{};
            UINT mask{};
            UINT stencil{};
        } saved;
        context_->OMGetRenderTargets(1U, &saved.target, &saved.depth);
        context_->IAGetInputLayout(&saved.layout);
        context_->IAGetPrimitiveTopology(&saved.topology);
        context_->VSGetShader(&saved.vertex, nullptr, nullptr);
        context_->PSGetShader(&saved.pixel, nullptr, nullptr);
        context_->OMGetBlendState(&saved.blend, saved.factor, &saved.mask);
        context_->OMGetDepthStencilState(&saved.depth_state, &saved.stencil);
        context_->RSGetState(&saved.raster);
        context_->RSGetViewports(&saved.viewports, &saved.viewport);
        context_->PSGetConstantBuffers(1U, 1U, &saved.constants);
        context_->PSGetSamplers(1U, 1U, &saved.sampler);
        context_->PSGetShaderResources(0U, 2U, saved.views[0].GetAddressOf());

        context_->RSSetState(raster_.Get());
        context_->OMSetDepthStencilState(no_depth_.Get(), 0U);
        ID3D11Buffer *constants_buffer = frame_buffer_.Get();
        context_->PSSetConstantBuffers(1U, 1U, &constants_buffer);
        ID3D11SamplerState *wrap = wrap_sampler_.Get();
        context_->PSSetSamplers(1U, 1U, &wrap);
        set_viewport(flip_extent, flip_extent);
        bool complete = true;
        for (UINT slice = 0U; slice < 2U && complete; ++slice)
        {
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.Format = description.Format;
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.ArraySize = 1U;
            ComPtr<ID3D11RenderTargetView> slice_target;
            if (FAILED(device_->CreateRenderTargetView(flip.Get(), &view, &slice_target)))
            {
                complete = false;
                break;
            }
            FrameConstants constants{};
            constants.params[3] = static_cast<float>(slice);
            write_constants(constants);
            fullscreen(bake_density_shader_.Get(), fullscreen_vertex_.Get(), scratch.target.Get(),
                       {nullptr, noise_view_.Get()});
            fullscreen(bake_normal_shader_.Get(), fullscreen_vertex_.Get(), slice_target.Get(),
                       {scratch.view.Get()});
        }
        context_->GenerateMips(flip_view_.Get());

        ID3D11RenderTargetView *target = saved.target.Get();
        context_->OMSetRenderTargets(1U, &target, saved.depth.Get());
        context_->IASetInputLayout(saved.layout.Get());
        context_->IASetPrimitiveTopology(saved.topology);
        context_->VSSetShader(saved.vertex.Get(), nullptr, 0U);
        context_->PSSetShader(saved.pixel.Get(), nullptr, 0U);
        context_->OMSetBlendState(saved.blend.Get(), saved.factor, saved.mask);
        context_->OMSetDepthStencilState(saved.depth_state.Get(), saved.stencil);
        context_->RSSetState(saved.raster.Get());
        if (saved.viewports > 0U)
            context_->RSSetViewports(1U, &saved.viewport);
        ID3D11Buffer *previous_constants = saved.constants.Get();
        context_->PSSetConstantBuffers(1U, 1U, &previous_constants);
        ID3D11SamplerState *previous_sampler = saved.sampler.Get();
        context_->PSSetSamplers(1U, 1U, &previous_sampler);
        ID3D11ShaderResourceView *const previous_views[2]{saved.views[0].Get(),
                                                          saved.views[1].Get()};
        context_->PSSetShaderResources(0U, 2U, previous_views);
        if (!complete)
            failure_ = "could not bake the effect flipbooks";
        return complete;
    }

    void write_constants(const FrameConstants &constants)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(frame_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
        {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context_->Unmap(frame_buffer_.Get(), 0U);
        }
    }

    void set_viewport(int width, int height)
    {
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width),
                                      static_cast<float>(height), 0.0F, 1.0F};
        context_->RSSetViewports(1U, &viewport);
    }

    void unbind_pixel_views()
    {
        ID3D11ShaderResourceView *const none[6]{};
        context_->PSSetShaderResources(0U, 6U, none);
    }

    void fullscreen(ID3D11PixelShader *shader, ID3D11VertexShader *vertex_shader,
                    ID3D11RenderTargetView *target,
                    std::initializer_list<ID3D11ShaderResourceView *> views)
    {
        context_->OMSetRenderTargets(1U, &target, nullptr);
        context_->OMSetBlendState(opaque_.Get(), nullptr, 0xFFFFFFFFU);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader, nullptr, 0U);
        context_->PSSetShader(shader, nullptr, 0U);
        context_->PSSetShaderResources(0U, static_cast<UINT>(views.size()), views.begin());
        context_->Draw(3U, 0U);
        unbind_pixel_views();
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }

    void draw_instances(const std::vector<vfx::Instance> &instances, ID3D11PixelShader *shader)
    {
        const std::size_t count = std::min(instances.size(), instance_capacity);
        if (count == 0U)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(instance_buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return;
        // When more were submitted than fit, keep the nearest (the list is
        // sorted farthest first).
        std::memcpy(mapped.pData, instances.data() + (instances.size() - count),
                    count * sizeof(vfx::Instance));
        context_->Unmap(instance_buffer_.Get(), 0U);
        const UINT stride = sizeof(vfx::Instance);
        const UINT offset = 0U;
        ID3D11Buffer *buffer = instance_buffer_.Get();
        context_->IASetInputLayout(layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->IASetVertexBuffers(0U, 1U, &buffer, &stride, &offset);
        context_->VSSetShader(particle_vertex_.Get(), nullptr, 0U);
        context_->PSSetShader(shader, nullptr, 0U);
        context_->DrawInstanced(4U, static_cast<UINT>(count), 0U, 0U);
    }

    ID3D11Device *device_{};
    ID3D11DeviceContext *context_{};
    bool ready_{};
    bool submitted_{};
    bool active_{};
    // OPENMINERZ_VFX_DEBUG=flip: draw gas as its raw flipbook texels.
    bool debug_flip_{SDL_getenv("OPENMINERZ_VFX_DEBUG") != nullptr};
    std::string failure_;
    vfx::Frame frame_;
    VfxOptions options_{};
    ComPtr<ID3D11VertexShader> particle_vertex_;
    ComPtr<ID3D11VertexShader> fullscreen_vertex_;
    ComPtr<ID3D11VertexShader> composite_vertex_;
    ComPtr<ID3D11PixelShader> soft_shader_;
    ComPtr<ID3D11PixelShader> sharp_shader_;
    ComPtr<ID3D11PixelShader> distort_shader_;
    ComPtr<ID3D11PixelShader> depth_shader_;
    ComPtr<ID3D11PixelShader> composite_shader_;
    ComPtr<ID3D11PixelShader> bake_density_shader_;
    ComPtr<ID3D11PixelShader> bake_normal_shader_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11BlendState> opaque_;
    ComPtr<ID3D11BlendState> premultiplied_;
    ComPtr<ID3D11BlendState> additive_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> no_depth_;
    ComPtr<ID3D11SamplerState> clamp_sampler_;
    ComPtr<ID3D11SamplerState> flip_sampler_;
    ComPtr<ID3D11SamplerState> wrap_sampler_;
    ComPtr<ID3D11Buffer> frame_buffer_;
    ComPtr<ID3D11Buffer> light_buffer_;
    ComPtr<ID3D11Buffer> instance_buffer_;
    ComPtr<ID3D11ShaderResourceView> noise_view_;
    ComPtr<ID3D11ShaderResourceView> flip_view_;
    Surface low_color_;
    Surface low_depth_;
    Surface distort_;
    int low_width_{};
    int low_height_{};
};

VfxRenderer::VfxRenderer(ID3D11Device *device, ID3D11DeviceContext *context)
    : impl_(std::make_unique<Impl>(device, context))
{
}

VfxRenderer::~VfxRenderer() = default;

bool VfxRenderer::ready() const noexcept
{
    return impl_->ready();
}

const char *VfxRenderer::failure() const noexcept
{
    return impl_->failure();
}

void VfxRenderer::submit(vfx::Frame &frame, const VfxOptions &options)
{
    impl_->submit(frame, options);
}

void VfxRenderer::begin_frame(bool enabled)
{
    impl_->begin_frame(enabled);
}

bool VfxRenderer::wants_draw() const noexcept
{
    return impl_->wants_draw();
}

void VfxRenderer::draw(const VfxPass &pass)
{
    impl_->draw(pass);
}

void VfxRenderer::unbind()
{
    impl_->unbind();
}

const vfx::Frame &VfxRenderer::frame() const noexcept
{
    return impl_->frame();
}

} // namespace openminerz

#endif
