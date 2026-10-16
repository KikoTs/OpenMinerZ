#include "far_terrain_renderer.hpp"

#include "atmosphere_shader.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/surface_atmosphere.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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

#if defined(_WIN32)
namespace
{
using Microsoft::WRL::ComPtr;

// Quads of one quadrant: a top and two side faces for each of its 32 x 32
// cells, then a skirt along each of its four edges.
constexpr UINT quadrant_cell_quads = world::far_quadrant_cells * world::far_quadrant_cells * 3;
constexpr UINT quadrant_quads = quadrant_cell_quads + 4U * world::far_quadrant_cells;
constexpr UINT quadrant_indices = quadrant_quads * 6U;
// A further layer of a quadrant with floating columns is drawn in blocks of
// 16 x 16 cells, three quads a cell (FarSelection::blocks).
constexpr UINT block_quads = (world::far_quadrant_cells / 2) * (world::far_quadrant_cells / 2) * 3;
constexpr UINT block_indices = block_quads * 6U;
// Quadrants and blocks share one instance buffer.
constexpr std::size_t maximum_quadrants = 8192U;
constexpr std::size_t maximum_boxes = 48'000U;
// The far pass's own near plane. Far geometry is never nearer than the
// hand-over, so this only has to be in front of that.
constexpr float far_near_plane = 1.0F;
// Registers of its own, clear of everything bound around these passes: the
// renderer's b0-b2 / s0-s2 / t0-t8, the post chain's b1-b2 / s0-s3 / t0-t17
// and the deep underground's b3 / s5-s6 / t20-t21 all stay as they were.
constexpr UINT far_constant_slot = 6U;
constexpr UINT far_sampler_slot = 7U; // s7 atlas, s8 shadow field
// What the detailed terrain's own shader reads of the far shadow field
// (depth_voxel_renderer.cpp, far_field_shadow()).
constexpr UINT near_shadow_constant_slot = 7U;
constexpr UINT near_shadow_texture_slot = 22U; // t22 older, t23 newer
constexpr UINT near_shadow_sampler_slot = 8U;

struct NearShadowConstants final
{
    // Per field: xy = eye - field origin (x, z), z = 1 / field width, w = present.
    std::array<std::array<float, 4>, 2> fields{};
    // x = weight of the newer field, y = softness in blocks, z = 1 when on,
    // w = how much of the sun a shadow takes.
    std::array<float, 4> parameters{};
};

// The renderer's scene constants (register b0), as far as these shaders read
// them. depth_voxel_renderer.cpp owns the layout; every shader there and in
// post_processing.cpp declares this same prefix.
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
};

cbuffer Far : register(b6)
{
    int4 EyeBlock;        // xyz = floor(eye)
    float4 EyeFraction;   // xyz = eye - floor(eye), w = the far pass's near plane
    float4 FarRange;      // x = far distance, y = where the fade into the sky starts,
                          // z = hand-over clip radius, w = presence (0 hidden .. 1)
    float4 WorldOrigin;   // xy = eye x, z relative to the world's origin, z = seconds,
                          // w = pixels one block covers at one block's distance
    float4 ShadowField[4];// fine older, fine newer, coarse older, coarse newer:
                          // xy = eye - field origin, z = 1 / field width, w = present
    float4 ShadowBlend;   // x = fine newer weight, y = coarse newer weight, z = opacity
    int4 BoxOrigin;
    // Per BlockType: x top tile, y side tile, z tile under the first block,
    // w = bits 0-7 cells of soil before rock (0 = none), 8 snow line, 9-10
    // glow (1 lava, 2 lantern).
    uint4 Blocks[256]; // mod-world: 96 and up are generated mod blocks
};

static const float WorldFloor = -64.0f;
)hlsl";

// A column is solid from the world's floor to its top. A column whose top span
// floats (a Lagoon overhang, an island; FarTile::lower) is solid from the floor
// to its `ground`, open air up to `under`, and solid again up to its top. Its
// quadrant is drawn as every quadrant is, and the 16 x 16 blocks of it that
// hold such columns twice more, over the same kind of grid of quads:
//   layer 0  the tops, and the side of each top span down to the neighbouring
//            top or to its own underside, whichever comes first;
//   layer 1  the ground under the air, and the walls between grounds;
//   layer 2  the undersides, and the steps between neighbouring undersides.
// A quadrant with no such column is layer 0 alone, exactly as before.
constexpr char terrain_vertex_source[] = R"hlsl(
Texture2DArray<uint4> Cells : register(t0);
Texture2DArray<uint4> Lower : register(t1);

struct FarVertex
{
    uint id : SV_VertexID;
    int2 origin : ORIGIN;
    uint packed : PACKED;
    float4 morph : MORPH;
};

struct FarPixel
{
    float4 position : SV_POSITION;
    float3 relative : TEXCOORD0;
    float3 surface : TEXCOORD1;
    nointerpolation float4 face : TEXCOORD2;
    nointerpolation uint2 blocks : TEXCOORD3;
    float4 shade : TEXCOORD4;
    float3 slope : TEXCOORD5;
};

uint4 cell_at(int2 cell, uint slice)
{
    return Cells.Load(int4(cell.x + 2, cell.y + 2, (int)slice, 0));
}

float3 cell_slope(uint4 data)
{
    float px = (float)(((data.z >> 4) << 2) | (data.w & 3u));
    float pz = (float)(data.w >> 2);
    float2 n = (float2(px, pz) - 32.0f) / 31.0f;
    return float3(n.x, sqrt(saturate(1.0f - dot(n, n))), n.y);
}

float cell_open(uint4 data)
{
    return (float)(data.z & 15u) / 15.0f;
}

// What is under a cell's top span: x the height of its underside, y of the
// ground beneath the air, z the block on that ground, w 1 when there is air.
float4 cell_lower(int2 cell, uint slice, bool layered, float least)
{
    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [branch]
    if (layered)
    {
        uint4 data = Lower.Load(int4(cell.x + 2, cell.y + 2, (int)slice, 0));
        if (data.x > 0u && (float)data.x - (float)data.y >= least)
            result = float4((float)data.x, (float)data.y, (float)data.z, 1.0f);
    }
    return result;
}

// One cell at one vertex, part way through the geomorph onto its parent.
struct FarCell
{
    uint4 own;
    uint4 parent;
    float top;
    float under;
    float ground;
    uint2 ground_blocks;
    float open;
    // The cell or its parent has air under its top span: the same answer at
    // every vertex of a quad, so a quad is drawn or dropped as a whole.
    bool ever;
    // At this vertex there is air between `ground` and `under`.
    bool floats;
};

FarCell far_cell(int2 at, uint slice, float size, float morph, bool still, bool layered)
{
    FarCell cell;
    cell.own = cell_at(at, slice);
    cell.parent = cell.own;
    float4 lower = cell_lower(at, slice, layered, 0.5f);
    float4 lower_parent = lower;
    [branch]
    if (!still)
    {
        int2 coarse = at & int2(-2, -2);
        cell.parent = cell_at(coarse, slice);
        // The coarser level keeps a gap only when it is a cell of its own tall.
        lower_parent = cell_lower(coarse, slice, layered, size * 2.0f);
    }
    // A solid column morphing onto (or off) one with air in it: its underside
    // and its ground start together, at the other's ground or at its own top
    // if that is lower, so the air opens and closes without a jump.
    if (lower.w < 0.5f && lower_parent.w > 0.5f)
    {
        float meet = min(lower_parent.y, (float)cell.own.x);
        lower.xyz = float3(meet, meet, lower_parent.z);
    }
    if (lower_parent.w < 0.5f && lower.w > 0.5f)
    {
        float meet = min(lower.y, (float)cell.parent.x);
        lower_parent.xyz = float3(meet, meet, lower.z);
    }
    cell.top = lerp((float)cell.own.x, (float)cell.parent.x, morph);
    cell.under = lerp(lower.x, lower_parent.x, morph);
    cell.ground = lerp(lower.y, lower_parent.y, morph);
    cell.ground_blocks = uint2((uint)lower.z, (uint)lower_parent.z);
    cell.open = lerp(cell_open(cell.own), cell_open(cell.parent), morph);
    cell.ever = lower.w + lower_parent.w > 0.5f;
    cell.floats = cell.ever && cell.under - cell.ground > 0.01f;
    return cell;
}

FarPixel main(FarVertex input)
{
    uint quad = input.id >> 2;
    uint corner = input.id & 3u;
    float2 c = float2((float)(corner & 1u), (float)(corner >> 1));
    float size = (float)(input.packed & 0x7FFu);
    uint slice = (input.packed >> 12) & 0xFFFu;
    int2 base = int2((int)((input.packed >> 24) & 1u), (int)((input.packed >> 25) & 1u)) * 32;
    // (Bits 28-29: which layer this instance is; bit 30: the tile has a
    // second layer to read.)
    uint layer = (input.packed >> 28) & 3u;
    bool layered = (input.packed & 0x40000000u) != 0u;

    // Which cell and which of its faces this quad is.
    int2 cell;
    uint kind;
    if (layer != 0u)
    {
        // A further layer covers one 16 x 16 block of the quadrant (bits 11
        // and 31 say which), three quads a cell and no skirt.
        uint index = quad / 3u;
        kind = quad - index * 3u;
        cell = int2((int)(index & 15u), (int)(index >> 4)) +
               int2((int)((input.packed >> 11) & 1u), (int)(input.packed >> 31)) * 16;
    }
    else if (quad < 3072u)
    {
        uint index = quad / 3u;
        kind = quad - index * 3u;
        cell = int2((int)(index & 31u), (int)(index >> 5));
    }
    else
    {
        uint skirt = quad - 3072u;
        uint edge = skirt >> 5;
        int along = (int)(skirt & 31u);
        kind = 3u + edge;
        cell = edge == 0u ? int2(0, along)
                          : (edge == 1u ? int2(31, along)
                                        : (edge == 2u ? int2(along, 0) : int2(along, 31)));
    }
    int2 a = base + cell;
    int2 b = a;
    float2 local = (float2)cell;
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    bool lower = c.y > 0.5f;
    if (kind == 0u)
    {
        local += c;
        lower = false;
    }
    else if (kind == 1u)
    {
        b = a + int2(1, 0);
        local += float2(1.0f, c.x);
        normal = float3(1.0f, 0.0f, 0.0f);
    }
    else if (kind == 2u)
    {
        b = a + int2(0, 1);
        local += float2(c.x, 1.0f);
        normal = float3(0.0f, 0.0f, 1.0f);
    }
    else if (kind == 3u)
    {
        local += float2(0.0f, c.x);
        normal = float3(-1.0f, 0.0f, 0.0f);
    }
    else if (kind == 4u)
    {
        local += float2(1.0f, c.x);
        normal = float3(1.0f, 0.0f, 0.0f);
    }
    else if (kind == 5u)
    {
        local += float2(c.x, 0.0f);
        normal = float3(0.0f, 0.0f, -1.0f);
    }
    else
    {
        local += float2(c.x, 1.0f);
        normal = float3(0.0f, 0.0f, 1.0f);
    }

    float2 blocks_xz = local * size;
    float3 relative;
    // The whole-block part is subtracted as integers, so a world a hundred
    // thousand blocks from the origin is as steady as one at it.
    relative.xz = (float2)(input.origin - EyeBlock.xz) + blocks_xz - EyeFraction.xz;
    float horizontal = length(relative.xz);
    // The geomorph: toward the far end of its level's range a cell takes the
    // height of the coarser cell it is part of, which is its own tile's cell
    // at the even coordinates below it.
    float morph = max(saturate((horizontal - input.morph.x) * input.morph.y), input.morph.z);
    // (Bit 27: the whole quadrant is short of where its level starts to
    // morph, so the parent cells are never needed.)
    bool still = (input.packed & 0x8000000u) != 0u;

    bool side = kind == 1u || kind == 2u;
    FarCell first = far_cell(a, slice, size, morph, still, layered);
    FarCell second = first;
    [branch]
    if (side)
        second = far_cell(b, slice, size, morph, still, layered);

    float height = first.top;
    float top = first.top;
    float open = first.open;
    uint2 blocks = uint2(first.own.y, first.parent.y);
    float3 slope = float3(0.0f, 1.0f, 0.0f);
    bool level = kind == 0u;
    bool alive = true;
    if (layer == 0u)
    {
        FarCell high = first;
        FarCell low = second;
        if (side)
        {
            // A side face runs from the higher of two neighbouring cells down
            // to the lower -- or only to the higher one's own underside, where
            // its top span floats -- and shows the higher one's material. How
            // much sky it sees goes from the higher cell's share at the top to
            // the lower cell's at the foot, so the wall of a narrow shaft
            // darkens on the way down while an open cliff stays lit.
            if (second.top > first.top)
            {
                high = second;
                low = first;
                normal = -normal;
            }
            // (A floating span is rock under its one block of turf, not the
            // soil of ordinary ground: its side is textured as if that deep.)
            top = high.top + (high.floats ? size * 3.0f : 0.0f);
            height = lower ? max(low.top, high.floats ? high.under : 0.0f) : high.top;
            open = lower ? low.open : high.open;
            blocks = uint2(high.own.y, high.parent.y);
        }
        else if (kind >= 3u && lower)
        {
            // A skirt: hangs from the quadrant's edge so that a neighbour of
            // another level, whose edge cells differ slightly, never shows
            // sky. Under a floating span it stops at the underside.
            height = max(first.top - (size * 2.0f + 2.0f), first.floats ? first.under : 0.0f);
        }
        slope = normalize(lerp(cell_slope(high.own), cell_slope(high.parent), morph));
    }
    else if (layer == 1u)
    {
        // The ground under the air. Where a column has none, its "ground" for
        // the walls between is its top.
        alive = first.ever;
        height = first.ground;
        top = first.ground;
        blocks = first.ground_blocks;
        if (side)
        {
            alive = first.ever || second.ever;
            FarCell high = first;
            FarCell low = second;
            float ground_high = first.floats ? first.ground : first.top;
            float ground_low = second.floats ? second.ground : second.top;
            if (ground_low > ground_high)
            {
                high = second;
                low = first;
                float swapped = ground_high;
                ground_high = ground_low;
                ground_low = swapped;
                normal = -normal;
            }
            // The wall of the higher ground, seen through the lower column's
            // air. A solid column's wall above the other's top is layer 0's.
            height = lower ? ground_low
                           : (high.floats ? ground_high : min(ground_high, low.top));
            top = ground_high;
            if (!high.floats)
                blocks = uint2(high.own.y, high.parent.y);
            else
                blocks = high.ground_blocks;
        }
        else if (kind >= 3u && lower)
        {
            height = first.ground - (size * 2.0f + 2.0f);
        }
        slope = level ? slope : normal;
        // Under an overhang some of the sky is gone.
        open = min(open, 0.6f);
    }
    else
    {
        // The underside of a floating span, and where two neighbouring spans
        // end at different heights the side of the one that reaches lower.
        alive = first.ever && kind < 3u;
        level = false;
        height = first.under;
        // (Textured as the rock a floating span is made of, like its sides.)
        top = first.top + size * 3.0f;
        open = min(open, 0.5f);
        if (side)
        {
            alive = first.ever && second.ever;
            FarCell deep = first;
            FarCell other = second;
            if (second.under < first.under)
            {
                deep = second;
                other = first;
                normal = -normal;
            }
            float step_top = min(other.under, deep.top);
            height = lower ? (deep.floats ? deep.under : step_top) : step_top;
            top = deep.top + size * 3.0f;
            blocks = uint2(deep.own.y, deep.parent.y);
            // A flank of the span, open to the side like any cliff.
            open = deep.open;
        }
        else
        {
            normal = float3(0.0f, -1.0f, 0.0f);
        }
        slope = normal;
    }

    relative.y = height + WorldFloor - Eye.y;
    float3 camera = float3(dot(relative, CameraRight.xyz), dot(relative, CameraUp.xyz),
                           dot(relative, CameraForward.xyz));
    FarPixel output;
    // Reversed, infinite depth: near / distance. A quad with nothing to show
    // is put behind the near plane, all four corners alike.
    output.position = alive ? float4(camera.x * Projection.x, camera.y * Projection.y,
                                     EyeFraction.w, camera.z)
                            : float4(0.0f, 0.0f, -1.0f, 1.0f);
    output.relative = relative;
    output.surface = float3(blocks_xz.x, height, blocks_xz.y);
    output.face = float4(normal, level ? 0.0f : 1.0f);
    output.blocks = blocks;
    output.shade = float4(morph, top, open, size);
    output.slope = slope;
    return output;
}
)hlsl";

constexpr char box_vertex_source[] = R"hlsl(
struct BoxVertex
{
    uint id : SV_VertexID;
    int4 box_minimum : BOXMIN;  // xyz, w = where it starts to shrink
    uint4 box_size : BOXSIZE;   // xyz, w = side tile
    uint4 box_tile : BOXTILE;   // top tile, flags, then where it has shrunk away (16 bits)
};

struct FarPixel
{
    float4 position : SV_POSITION;
    float3 relative : TEXCOORD0;
    float3 surface : TEXCOORD1;
    nointerpolation float4 face : TEXCOORD2;
    nointerpolation uint2 blocks : TEXCOORD3;
    float4 shade : TEXCOORD4;
    float3 slope : TEXCOORD5;
};

FarPixel main(BoxVertex input)
{
    uint face = input.id >> 2;
    uint corner = input.id & 3u;
    float2 c = float2((float)(corner & 1u), (float)(corner >> 1));
    float3 size = (float3)input.box_size.xyz;
    float3 unit;
    float3 normal;
    if (face == 0u)      { unit = float3(1.0f, c.y, c.x); normal = float3(1.0f, 0.0f, 0.0f); }
    else if (face == 1u) { unit = float3(0.0f, c.y, c.x); normal = float3(-1.0f, 0.0f, 0.0f); }
    else if (face == 2u) { unit = float3(c.x, c.y, 1.0f); normal = float3(0.0f, 0.0f, 1.0f); }
    else if (face == 3u) { unit = float3(c.x, c.y, 0.0f); normal = float3(0.0f, 0.0f, -1.0f); }
    else if (face == 4u) { unit = float3(c.x, 1.0f, c.y); normal = float3(0.0f, 1.0f, 0.0f); }
    else                 { unit = float3(c.x, 0.0f, c.y); normal = float3(0.0f, -1.0f, 0.0f); }
    float3 local = unit * size;
    float3 minimum = (float3)input.box_minimum.xyz;
    float2 offset = (float2)(BoxOrigin.xz - EyeBlock.xz) - EyeFraction.xz;
    // A tree shrinks into its own foot over the last stretch it is drawn.
    float2 foot = minimum.xz + size.xz * 0.5f + offset;
    float scale = 1.0f;
    uint fade_end = input.box_tile.z | (input.box_tile.w << 8);
    if (fade_end > 0u)
        scale = 1.0f - smoothstep((float)input.box_minimum.w, (float)fade_end, length(foot));
    float3 position = minimum + local;
    float3 relative;
    relative.xz = lerp(foot, position.xz + offset, scale);
    float anchor_y = fade_end > 0u ? (float)(input.box_tile.y >> 1) : minimum.y;
    float height = lerp(anchor_y, position.y, scale);
    relative.y = height + (float)BoxOrigin.y - Eye.y;
    float3 camera = float3(dot(relative, CameraRight.xyz), dot(relative, CameraUp.xyz),
                           dot(relative, CameraForward.xyz));
    FarPixel output;
    output.position = float4(camera.x * Projection.x, camera.y * Projection.y, EyeFraction.w,
                             camera.z);
    output.relative = relative;
    output.surface = float3(position.x, position.y, position.z);
    output.face = float4(normal, face == 4u ? 0.0f : 1.0f);
    uint tiles = 0x10000u | input.box_size.w | (input.box_tile.x << 8) |
                 ((input.box_tile.y & 1u) != 0u ? 0x20000u : 0u);
    output.blocks = uint2(tiles, tiles);
    output.shade = float4(0.0f, minimum.y + size.y, 1.0f, 1.0f);
    output.slope = float3(0.0f, 1.0f, 0.0f);
    return output;
}
)hlsl";

// Lights a far surface the way the detailed terrain's shader lights the same
// block (depth_voxel_renderer.cpp, shade()): gamma-two albedo from the block's
// own texture, hemisphere sky light, rough Oren-Nayar sun, and a cast shadow.
constexpr char terrain_pixel_source[] = R"hlsl(
Texture2DArray DiffuseAtlas : register(t0);
Texture2D<float2> ShadowFineOlder : register(t1);
Texture2D<float2> ShadowFineNewer : register(t2);
Texture2D<float2> ShadowCoarseOlder : register(t3);
Texture2D<float2> ShadowCoarseNewer : register(t4);
SamplerState AtlasSampler : register(s7);
SamplerState FieldSampler : register(s8);

struct FarPixel
{
    float4 position : SV_POSITION;
    float3 relative : TEXCOORD0;
    float3 surface : TEXCOORD1;
    nointerpolation float4 face : TEXCOORD2;
    nointerpolation uint2 blocks : TEXCOORD3;
    float4 shade : TEXCOORD4;
    float3 slope : TEXCOORD5;
};

float3 surface_color(uint id, bool top, float cells_below, float height, float2 uv, float2 dx,
                     float2 dy, out float glow)
{
    uint tile;
    glow = 0.0f;
    if (id >= 0x10000u)
    {
        // A box (tree, tower): the tiles themselves.
        tile = top ? ((id >> 8) & 255u) : (id & 255u);
        glow = (id & 0x20000u) != 0u ? 2.0f : 0.0f;
    }
    else
    {
        uint4 entry = Blocks[min(id, 255u)];
        tile = top ? entry.x : entry.y;
        if (!top)
        {
            // Down the side of a column: the top block's own side, the soil
            // or snow under it, then rock. Counted in cells, not blocks: a
            // coarse cell stands for a staircase of blocks that each show
            // their own top layer.
            uint soil = entry.w & 255u;
            if (cells_below >= 1.0f)
                tile = entry.z;
            if (soil > 0u && cells_below >= (float)soil)
                tile = 5u;
        }
        glow = (float)((entry.w >> 9) & 3u);
    }
    float3 texel = DiffuseAtlas.SampleGrad(AtlasSampler, float3(uv, (float)tile), dx, dy).rgb;
    // Display-referred artwork into the scene's linear light, as everywhere.
    return texel * texel;
}

float field_light(Texture2D<float2> field, float4 placement, float2 relative_xz, float height,
                  float softness, out float inside)
{
    float2 uv = (relative_xz + placement.xy) * placement.z;
    float2 margin = min(uv, 1.0f - uv);
    inside = saturate(min(margin.x, margin.y) * 24.0f) * placement.w;
    // Heights are stored in 1/64 block.
    float shadow_top = field.SampleLevel(FieldSampler, uv, 0.0f).x * (65535.0f / 64.0f);
    return saturate((height - shadow_top + softness * 0.25f) / softness);
}

// How much of the sun reaches a point `height` blocks above the world's floor.
float far_shadow(float2 relative_xz, float height)
{
    if (ShadowBlend.z <= 0.0f)
        return 1.0f;
    float inside_older;
    float inside_newer;
    float light = 1.0f;
    // The fine field where it reaches; the coarse one beyond and across the
    // fine field's edge, where the two are blended.
    float2 fine_uv = (relative_xz + ShadowField[1].xy) * ShadowField[1].z;
    float2 fine_margin = min(fine_uv, 1.0f - fine_uv);
    bool well_inside = ShadowField[0].w > 0.5f && ShadowField[1].w > 0.5f &&
                       min(fine_margin.x, fine_margin.y) > 0.06f;
    [branch]
    if (!well_inside)
    {
        float older = field_light(ShadowCoarseOlder, ShadowField[2], relative_xz, height, 10.0f,
                                  inside_older);
        float newer = field_light(ShadowCoarseNewer, ShadowField[3], relative_xz, height, 10.0f,
                                  inside_newer);
        light = lerp(lerp(1.0f, older, inside_older), lerp(1.0f, newer, inside_newer),
                     ShadowBlend.y);
    }
    [branch]
    if (ShadowField[1].w > 0.5f && min(fine_margin.x, fine_margin.y) > 0.0f)
    {
        float older = field_light(ShadowFineOlder, ShadowField[0], relative_xz, height, 3.0f,
                                  inside_older);
        float newer = field_light(ShadowFineNewer, ShadowField[1], relative_xz, height, 3.0f,
                                  inside_newer);
        light = lerp(lerp(light, older, inside_older), lerp(light, newer, inside_newer),
                     ShadowBlend.x);
    }
    return lerp(1.0f, light, ShadowBlend.z);
}

float4 main(FarPixel input) : SV_TARGET
{
    float horizontal = length(input.relative.xz);
    // Not inside the detailed window, not beyond the far distance.
    clip(min(horizontal - FarRange.z, FarRange.x - horizontal));
    bool top = input.face.w < 0.5f;
    float3 face_normal = input.face.xyz;
    // (The underside of a floating span is level too, though it is not a top.)
    float2 uv = top || abs(face_normal.y) > 0.5f
                    ? input.surface.xz
                    : (abs(face_normal.x) > 0.5f ? float2(input.surface.z, -input.surface.y)
                                                 : float2(input.surface.x, -input.surface.y));
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float cells_below = max(input.shade.y - input.surface.y, 0.0f) / input.shade.w;
    float morph = input.shade.x;
    float glow;
    float3 albedo = surface_color(input.blocks.x, top, cells_below, input.surface.y, uv, dx, dy,
                                  glow);
    if (morph > 0.004f && input.blocks.y != input.blocks.x)
    {
        float parent_glow;
        float3 parent = surface_color(input.blocks.y, top, cells_below, input.surface.y, uv, dx,
                                      dy, parent_glow);
        albedo = lerp(albedo, parent, morph);
        glow = lerp(glow, parent_glow, morph);
    }

    float3 sun = normalize(SunAndOptions.xyz);
    float distance_to_eye = length(input.relative);
    float3 view = -input.relative / max(distance_to_eye, 0.001f);
    // Close up a column is lit as the flat-faced block it is. Once its cells
    // are only a few pixels wide the faces would sparkle against each other,
    // so the light eases onto the slope of the ground they stand for.
    float cell_pixels = input.shade.w * WorldOrigin.w / max(distance_to_eye, 1.0f);
    float soften = 1.0f - smoothstep(3.0f, 10.0f, cell_pixels);
    float3 normal = normalize(lerp(face_normal, normalize(input.slope), soften * 0.8f));
    float open = saturate(input.shade.z);

    float hemisphere = saturate(normal.y * 0.5f + 0.5f);
    float sky_fill = 1.0f + WaterAndTime.w * 0.25f;
    float3 ground_bounce = AmbientSun.rgb * 0.62f + FogAndEnd.rgb * 0.10f;
    float3 sky_irradiance = AmbientSun.rgb * 1.42f + FogAndEnd.rgb * 0.16f;
    // A hillside sees nearly the whole sky; a pit that sees a sliver of it is
    // nearly dark.
    float sky_exposure = lerp(0.10f, 1.0f, smoothstep(0.0f, 0.85f, open));
    float3 ambient = lerp(ground_bounce, sky_irradiance, hemisphere) * sky_exposure * sky_fill;
    ambient += (AmbientSun.rgb * 1.18f + FogAndEnd.rgb * 0.08f) * 0.30f * sky_fill *
               (top ? 0.06f : 0.16f) * sky_exposure;
    // The deep pit beyond the arctic is lit by what is in it, like Hell.
    float world_y = input.surface.y + WorldFloor;
    float hell_surface = Eye.w * smoothstep(24.0f, 36.0f, -world_y);
    ambient = lerp(ambient,
                   float3(1.10f, 0.20f, 0.075f) * (0.56f + 0.44f * saturate(0.5f - normal.y * 0.5f)),
                   hell_surface);

    float n_dot_l = saturate(dot(normal, sun));
    float n_dot_v = saturate(dot(normal, view));
    float azimuth = max(dot(sun, view) - n_dot_l * n_dot_v, 0.0f);
    float face_shade = n_dot_l * (0.70f + 0.38f * azimuth / max(max(n_dot_l, n_dot_v), 0.001f));
    float shadow = far_shadow(input.relative.xz, input.surface.y);
    // The sun does not reach the bottom of a shaft either.
    float3 direct = LightAndFogStart.rgb * face_shade * shadow * (1.0f - hell_surface) *
                    smoothstep(0.05f, 0.45f, open);

    // What the detailed terrain's baked corner shading does to a hillside.
    float contact = top ? lerp(0.78f, 0.93f, open) : 0.78f;
    float3 lit = albedo * (ambient + direct) * contact;
    if (glow > 1.5f)
        lit = albedo * 2.8f;
    else if (glow > 0.5f)
        // Unresolved cracks cover a small fraction of the surface. Preserve
        // their red mean radiance instead of painting the entire pool orange.
        lit = float3(0.72f, 0.035f, 0.003f) * (0.85f + albedo.r * 0.3f);
    // OPENMINERZ_FAR_DEBUG: one term alone (1 shadow, 2 sky seen, 3 morph).
    if (ShadowBlend.w > 0.5f)
        lit = ShadowBlend.w < 1.5f ? shadow.xxx * 0.5f
                                   : (ShadowBlend.w < 2.5f ? open.xxx * 0.5f : morph.xxx * 0.5f);
    return float4(max(lit, 0.0f), 1.0f);
}
)hlsl";

constexpr char screen_vertex_source[] = R"hlsl(
float4 main(uint vertex_id : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)hlsl";

// Pass 3b. Writes with dual-source blending: target = target * keep + add.
constexpr char composite_pixel_source[] = R"hlsl(
Texture2D<float> SceneDepth : register(t0);
Texture2D FarColor : register(t1);
Texture2D<float> FarDepth : register(t2);
Texture2D Surface1 : register(t3);

// The view ray through a pixel; its component along the camera's forward axis
// is exactly one, so a camera depth times its length is a distance.
float3 view_ray(float4 position)
{
    float2 uv = position.xy * TargetSize.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return CameraForward.xyz + CameraRight.xyz * (ndc.x / Projection.x) +
           CameraUp.xyz * (ndc.y / Projection.y);
}

// The far landscape on a pixel, seen through the air in front of it. Returns
// how much of the pixel it covers (it dissolves over the last of its range).
float far_landscape(int2 pixel, float3 ray, float ray_length, out float3 color)
{
    color = 0.0f.xxx;
    float far_depth = FarDepth.Load(int3(pixel, 0));
    if (far_depth <= 0.0f || FarRange.w <= 0.001f)
        return 0.0f;
    float camera_depth = EyeFraction.w / far_depth;
    float transmittance;
    float3 inscatter;
    atmosphere(ray / ray_length, camera_depth * ray_length, 1.0f, FogAndEnd.rgb, transmittance,
               inscatter);
    color = FarColor.Load(int3(pixel, 0)).rgb * transmittance + inscatter;
    // OPENMINERZ_FAR_DEBUG: the air alone (1 what survives, 2 what it adds).
    if (AtmosHaze.w > 0.5f)
        color = AtmosHaze.w < 1.5f ? transmittance.xxx : inscatter;
    float horizontal = length(ray.xz) * camera_depth;
    return FarRange.w * (1.0f - smoothstep(FarRange.y, FarRange.x, horizontal));
}

#ifdef ATMOSPHERE_UNDER
// Where the scene holds no depth: the far landscape, and over what sky is left
// the air between the eye and the end of the atmosphere. Blended UNDER what is
// there (dst + src * (1 - dst.a)), so particles and effects in front of the
// sky stay in front; the sky pass that follows fills what this leaves of the
// alpha, which is how the sky itself is fogged down to the horizon.
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    int2 pixel = int2(position.xy);
    if (SceneDepth.Load(int3(pixel, 0)) < 0.999999f)
        discard;
    float3 ray = view_ray(position);
    float ray_length = length(ray);
    float3 far_color;
    float far_alpha = far_landscape(pixel, ray, ray_length, far_color);
    float transmittance;
    float3 inscatter;
    atmosphere(ray / ray_length, 60000.0f, 1.0f, FogAndEnd.rgb, transmittance, inscatter);
    if (AtmosHaze.w > 0.5f)
        return float4(lerp(AtmosHaze.w < 1.5f ? transmittance.xxx : inscatter, far_color,
                           far_alpha), 1.0f);
    return float4(far_color * far_alpha + inscatter * (1.0f - far_alpha),
                  far_alpha + (1.0f - far_alpha) * (1.0f - transmittance));
}
#else
// Where the scene holds something: the air in front of it, from its own
// depth. target = target * keep + add.
struct CompositeOutput
{
    float4 add : SV_TARGET0;
    float4 keep : SV_TARGET1;
};

CompositeOutput main(float4 position : SV_POSITION)
{
    int2 pixel = int2(position.xy);
    float depth = SceneDepth.Load(int3(pixel, 0));
    if (depth >= 0.999999f)
        discard;
    float3 ray = view_ray(position);
    float ray_length = length(ray);
    float camera_depth = Projection.w / min(depth - Projection.z, -0.0000001f);
    float4 surface = Surface1.Load(int3(pixel, 0));
    float eye_sky = AtmosMist.w;
    float transmittance;
    float3 inscatter;
    atmosphere(ray / ray_length, camera_depth * ray_length,
               atmosphere_exposure(eye_sky, surface.g), FogAndEnd.rgb, transmittance, inscatter);

    // The hand-over to the far landscape is geometry, not air: over the last
    // of the detailed window the lit pass blends the terrain toward one flat
    // colour by `fade` (the curve of depth_voxel_renderer.cpp's shade()), and
    // that share is given here to the far landscape standing in the same
    // place. A cave wall has no landscape behind it and keeps its fog.
    float horizontal = length(ray.xz) * camera_depth;
    float fade = saturate((horizontal - LightAndFogStart.w) /
                          max(FogAndEnd.w - LightAndFogStart.w, 0.001f));
    fade = fade * fade * (3.0f - 2.0f * fade);
    float open = smoothstep(0.03f, 0.20f, max(surface.g, eye_sky)) * AtmosAir.w;
    CompositeOutput output;
    if (surface.b > 0.5f)
    {
        // A creature is not handed over to anything: take the fade back out.
        float undo = min(fade, 0.55f) * open;
        float scale = transmittance / (1.0f - undo);
        output.keep = float4(scale.xxx, 1.0f);
        output.add = float4(inscatter - FogAndEnd.rgb * (undo * scale), 0.0f);
    }
    else
    {
        float handed = 0.0f;
        float3 far_color = 0.0f.xxx;
        [branch]
        if (fade * open > 0.0f)
            handed = fade * open * far_landscape(pixel, ray, ray_length, far_color);
        output.keep = float4(transmittance.xxx, 1.0f);
        output.add = float4(inscatter * (1.0f - handed) +
                                handed * (far_color - FogAndEnd.rgb * transmittance),
                            0.0f);
    }
    if (AtmosHaze.w > 0.5f)
    {
        output.keep = float4(0.0f, 0.0f, 0.0f, 1.0f);
        output.add = float4(AtmosHaze.w < 1.5f ? transmittance.xxx : inscatter, 0.0f);
    }
    return output;
}
#endif
)hlsl";

ComPtr<ID3DBlob> compile(const std::string &source, const char *target, std::string &failure)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    // Cached for the process (shader_cache.hpp): built again with every world.
    const HRESULT result = compile_hlsl_cached(source.data(), source.size(), "far_terrain",
                                               nullptr, nullptr, "main", target,
                                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &blob, &errors);
    if (FAILED(result))
    {
        failure = std::string{"far terrain shader ("} + target + "): ";
        if (errors != nullptr)
            failure.append(static_cast<const char *>(errors->GetBufferPointer()),
                           errors->GetBufferSize());
        return nullptr;
    }
    return blob;
}

struct FarConstants final
{
    std::array<std::int32_t, 4> eye_block{};
    std::array<float, 4> eye_fraction{};
    std::array<float, 4> far_range{};
    std::array<float, 4> world_origin{};
    std::array<std::array<float, 4>, 4> shadow_field{};
    std::array<float, 4> shadow_blend{};
    std::array<std::int32_t, 4> box_origin{};
    std::array<std::array<std::uint32_t, 4>, 256> blocks{};
};
static_assert(sizeof(FarConstants) % 16U == 0U);

// The Atmosphere cbuffer of atmosphere_shader.hpp.
struct AtmosphereConstants final
{
    std::array<float, 4> air{};
    std::array<float, 4> mist{};
    std::array<float, 4> haze{};
    std::array<std::array<float, 4>, 4> discs{};
    std::array<float, 4> eye{};
    std::array<float, 4> sun{};
    std::array<float, 4> sun_direction{};
};
static_assert(sizeof(AtmosphereConstants) % 16U == 0U);

// The block table of the Far cbuffer: the retail ids, then (mod-world) the
// generated mod blocks, which until set_mod_palette are rock.
std::array<std::array<std::uint32_t, 4>, 256> block_table()
{
    std::array<std::array<std::uint32_t, 4>, 256> table{};
    constexpr std::uint32_t dirt_tile = 0U;
    for (std::uint32_t id = 0U; id < static_cast<std::uint32_t>(world::far_mod_block_base); ++id)
    {
        const auto type = static_cast<world::BlockType>(id);
        const std::uint32_t top = world::atlas_tile(type, world::FaceDirection::positive_y);
        const std::uint32_t side = world::atlas_tile(type, world::FaceDirection::positive_x);
        std::uint32_t under = side;
        std::uint32_t flags{};
        switch (type)
        {
        case world::BlockType::grass:
            under = dirt_tile;
            flags = 4U;
            break;
        case world::BlockType::dirt:
            flags = 4U;
            break;
        case world::BlockType::sand:
            flags = 3U;
            break;
        case world::BlockType::snow:
            flags = 4U | 0x100U;
            break;
        case world::BlockType::ice:
            flags = 2U;
            break;
        case world::BlockType::surface_lava:
        case world::BlockType::deep_lava:
            flags = 1U << 9U;
            break;
        case world::BlockType::lantern:
        case world::BlockType::fixed_lantern:
        case world::BlockType::lantern_fancy:
            flags = 2U << 9U;
            break;
        default:
            break;
        }
        table[id] = {top, side, under, flags};
    }
    for (std::size_t id = world::far_mod_block_base; id < table.size(); ++id)
        table[id] = table[static_cast<std::size_t>(world::BlockType::rock)];
    return table;
}

} // namespace

class FarTerrainRenderer::Impl final
{
  public:
    Impl(ID3D11Device *device, ID3D11DeviceContext *context) : device_(device), context_(context)
    {
        trace_ = SDL_getenv("OPENMINERZ_FAR_TRACE") != nullptr ||
                 SDL_getenv("OPENMINERZ_POSTFX_TRACE") != nullptr;
        ready_ = create_shaders() && create_states() && create_buffers();
        if (!ready_)
            SDL_Log("Far terrain unavailable: %s", failure_.c_str());
    }

    // mod-world: the block table's generated mod blocks.
    void set_mod_palette(const world::ModPaletteLooks &looks, std::span<const world::BlockType> fallbacks,
                         std::span<const world::FarSurfaceProfile> profiles)
    {
        const auto retail = block_table();
        constexpr auto retail_last = static_cast<std::size_t>(world::far_mod_block_base) - 1U;
        for (std::size_t index = 0U; index < static_cast<std::size_t>(world::far_mod_block_count); ++index)
        {
            const std::size_t id = static_cast<std::size_t>(world::far_mod_block_base) + index;
            const std::size_t fallback =
                index < fallbacks.size() ? static_cast<std::size_t>(fallbacks[index]) : 6U;
            blocks_[id] = retail[std::min<std::size_t>(fallback, retail_last)];
            if (index < looks.size() && looks[index].valid)
            {
                blocks_[id][0] = looks[index].tiles[0];
                blocks_[id][1] = looks[index].tiles[1];
                blocks_[id][2] = looks[index].tiles[1];
            }
        }
        // (fix, wave 5) A mod biome's own layering: its top block's faces, the
        // side of the block under it, and rock where its layers end, instead
        // of what retail's layering of that block would show.
        const auto row = [&](const world::TerrainBlock &block) {
            return block.mod != 0U && block.mod <= world::far_mod_block_count
                       ? blocks_[static_cast<std::size_t>(world::far_mod_block_base) + block.mod - 1U]
                       : retail[std::min<std::size_t>(static_cast<std::size_t>(block.block), retail_last)];
        };
        for (std::size_t index = 0U; index < profiles.size(); ++index)
        {
            const std::size_t id = world::far_profile_byte(static_cast<std::uint8_t>(index + 1U));
            // (Never over the palette's bytes; the generator keeps clear of them.)
            if (index >= 255U || id < static_cast<std::size_t>(world::far_mod_block_base) + fallbacks.size())
                break;
            const world::FarSurfaceProfile &profile = profiles[index];
            std::array<std::uint32_t, 4> entry = row(profile.top);
            entry[2] = row(profile.under)[1];
            entry[3] = (entry[3] & ~0xFFU) | profile.soil;
            blocks_[id] = entry;
        }
    }

    ~Impl()
    {
        if (trace_ && stats_.frames > 0U)
            SDL_Log("FARTERRAIN gpu geometry avg %.3f ms peak %.3f ms, composite avg %.3f ms peak "
                    "%.3f ms over %zu frames; %zu uploads, %.3f ms total, %.3f ms peak; targets "
                    "%.1f MB, cell maps %.1f MB",
                    stats_.geometry_milliseconds, stats_.geometry_peak_milliseconds,
                    stats_.composite_milliseconds, stats_.composite_peak_milliseconds,
                    stats_.frames, stats_.uploads, stats_.upload_milliseconds_total,
                    stats_.upload_milliseconds_peak,
                    static_cast<double>(stats_.target_bytes) / 1048576.0,
                    static_cast<double>(stats_.array_bytes) / 1048576.0);
    }

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

    void submit(world::FarTerrainFrame frame)
    {
        if (!ready_)
            return;
        const auto started = std::chrono::steady_clock::now();
        bool uploaded{};
        if (frame.active)
        {
            if (frame.generation != generation_ || frame.slice_capacity != capacity_ ||
                cells_texture_ == nullptr)
            {
                if (!create_cells(frame.slice_capacity))
                {
                    frame_ = {};
                    return;
                }
                generation_ = frame.generation;
            }
            for (const auto &upload : frame.uploads)
            {
                if (upload.tile == nullptr || upload.slice >= capacity_ ||
                    upload.tile->cells.size() !=
                        static_cast<std::size_t>(world::far_tile_grid * world::far_tile_grid) * 4U)
                    continue;
                context_->UpdateSubresource(cells_texture_.Get(),
                                            D3D11CalcSubresource(0U, upload.slice, 1U), nullptr,
                                            upload.tile->cells.data(),
                                            static_cast<UINT>(world::far_tile_grid) * 4U, 0U);
                // The second layer, for a tile with floating columns. (A tile
                // without one never reads its slice of this array.)
                if (upload.tile->lower.size() == upload.tile->cells.size())
                    context_->UpdateSubresource(lower_texture_.Get(),
                                                D3D11CalcSubresource(0U, upload.slice, 1U), nullptr,
                                                upload.tile->lower.data(),
                                                static_cast<UINT>(world::far_tile_grid) * 4U, 0U);
                ++stats_.uploads;
                uploaded = true;
            }
            // One shadow field per frame: each is a megabyte.
            for (std::size_t cascade = 0U; cascade < 2U; ++cascade)
            {
                const auto &map = frame.shadows[cascade];
                if (map == nullptr || map->serial == shadows_[cascade].serial)
                    continue;
                upload_shadow(cascade, *map);
                uploaded = true;
                break;
            }
        }
        frame.uploads.clear();
        frame_ = std::move(frame);
        if (uploaded)
        {
            const double elapsed = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            stats_.upload_milliseconds_total += elapsed;
            stats_.upload_milliseconds_peak = std::max(stats_.upload_milliseconds_peak, elapsed);
        }
    }

    [[nodiscard]] bool active() const noexcept
    {
        return ready_ && frame_.active && cells_texture_ != nullptr;
    }

    [[nodiscard]] world::FarHandover handover() const noexcept
    {
        return frame_.handover;
    }

    // Binds the fine shadow field for the detailed terrain's lit pass, so a
    // mountain beyond the reach of its shadow maps still shades it. With
    // nothing to bind the constants are zero and the shader's term is off.
    void bind_near_shadow(const std::array<double, 3> &eye, float elapsed_seconds, float opacity)
    {
        if (!ready_)
            return;
        NearShadowConstants constants{};
        const ShadowCascade &fine = shadows_[0];
        const bool on = active() && fine.views[1] != nullptr && opacity > 0.0F &&
                        SDL_getenv("OPENMINERZ_FAR_NO_NEAR_SHADOW") == nullptr;
        if (on)
        {
            constants.fields = {field_placement(fine, 0U, eye), field_placement(fine, 1U, eye)};
            constants.parameters = {shadow_weight(fine, elapsed_seconds), 6.0F, 1.0F,
                                    std::clamp(opacity, 0.0F, 1.0F) *
                                        std::clamp(frame_.opacity, 0.0F, 1.0F)};
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(near_shadow_constants_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                    &mapped)))
        {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context_->Unmap(near_shadow_constants_.Get(), 0U);
        }
        ID3D11Buffer *buffer = near_shadow_constants_.Get();
        context_->PSSetConstantBuffers(near_shadow_constant_slot, 1U, &buffer);
        const std::array<ID3D11ShaderResourceView *, 2> views{on ? fine.views[0].Get() : nullptr,
                                                              on ? fine.views[1].Get() : nullptr};
        context_->PSSetShaderResources(near_shadow_texture_slot, static_cast<UINT>(views.size()),
                                       views.data());
        ID3D11SamplerState *sampler = field_sampler_.Get();
        context_->PSSetSamplers(near_shadow_sampler_slot, 1U, &sampler);
    }

    void unbind_near_shadow()
    {
        if (!ready_)
            return;
        constexpr std::array<ID3D11ShaderResourceView *, 2> none{};
        context_->PSSetShaderResources(near_shadow_texture_slot, static_cast<UINT>(none.size()),
                                       none.data());
        context_->PSSetShaderResources(atmosphere_far_depth_slot, 1U, none.data());
    }

    void draw(const FarTerrainPass &pass)
    {
        atmosphere_drawn_ = false;
        if (!ready_ || pass.scene_target == nullptr || pass.depth_view == nullptr ||
            pass.surface_view == nullptr || pass.width <= 0 || pass.height <= 0 ||
            !ensure_targets(pass.width, pass.height))
            return;
        // The atmosphere is drawn with or without a far landscape under it.
        const bool landscape = active() && pass.atlas_view != nullptr;
        begin_timing();
        write_constants(pass, landscape);

        // --- 2b (i): far geometry into its own colour and depth.
        constexpr std::array<float, 4> nothing{0.0F, 0.0F, 0.0F, 0.0F};
        context_->ClearRenderTargetView(color_target_.Get(), nothing.data());
        // Reversed depth: zero is infinitely far.
        context_->ClearDepthStencilView(depth_target_.Get(), D3D11_CLEAR_DEPTH, 0.0F, 0U);
        const std::size_t quadrant_count =
            landscape ? std::min(frame_.quadrants.size(), maximum_quadrants) : 0U;
        const std::size_t box_count = landscape ? std::min(frame_.boxes.size(), maximum_boxes) : 0U;
        const D3D11_VIEWPORT viewport{0.0F,
                                      0.0F,
                                      static_cast<float>(pass.width),
                                      static_cast<float>(pass.height),
                                      0.0F,
                                      1.0F};
        context_->RSSetViewports(1U, &viewport);
        context_->RSSetState(rasterizer_.Get());
        const std::array<ID3D11Buffer *, 1> scene_buffer{pass.scene_constants};
        const std::array<ID3D11Buffer *, 1> far_buffer{constants_.Get()};
        context_->VSSetConstantBuffers(0U, 1U, scene_buffer.data());
        context_->VSSetConstantBuffers(far_constant_slot, 1U, far_buffer.data());
        context_->PSSetConstantBuffers(0U, 1U, scene_buffer.data());
        context_->PSSetConstantBuffers(far_constant_slot, 1U, far_buffer.data());
        const std::array<ID3D11SamplerState *, 2> samplers{atlas_sampler_.Get(),
                                                           field_sampler_.Get()};
        context_->PSSetSamplers(far_sampler_slot, static_cast<UINT>(samplers.size()), samplers.data());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (quadrant_count + box_count > 0U)
        {
            ID3D11RenderTargetView *target = color_target_.Get();
            context_->OMSetRenderTargets(1U, &target, depth_target_.Get());
            context_->OMSetDepthStencilState(depth_state_.Get(), 0U);
            context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
            const std::array<ID3D11ShaderResourceView *, 5> pixel_views{
                pass.atlas_view, shadow_view(0U, 0U), shadow_view(0U, 1U), shadow_view(1U, 0U),
                shadow_view(1U, 1U)};
            context_->PSSetShaderResources(0U, static_cast<UINT>(pixel_views.size()),
                                           pixel_views.data());
            context_->PSSetShader(terrain_pixel_shader_.Get(), nullptr, 0U);
            constexpr UINT offset = 0U;
            // OPENMINERZ_FAR_DEBUG=still: measure the vertex shader without its
            // parent-cell reads (the picture is wrong where levels morph).
            static const bool force_still = [] {
                const char *text = SDL_getenv("OPENMINERZ_FAR_DEBUG");
                return text != nullptr && std::string_view{text} == "still";
            }();
            if (force_still)
                for (world::FarQuadrantInstance &instance : frame_.quadrants)
                    instance.packed |= 1U << 27U;
            // The quadrants, then the blocks that draw the further layers of
            // those with floating columns, in one buffer.
            const std::size_t block_count =
                std::min(frame_.blocks.size(), maximum_quadrants - quadrant_count);
            instances_.assign(frame_.quadrants.begin(),
                              frame_.quadrants.begin() +
                                  static_cast<std::ptrdiff_t>(quadrant_count));
            instances_.insert(instances_.end(), frame_.blocks.begin(),
                              frame_.blocks.begin() + static_cast<std::ptrdiff_t>(block_count));
            if (quadrant_count > 0U &&
                fill(quadrant_buffer_.Get(), instances_.data(),
                     instances_.size() * sizeof(world::FarQuadrantInstance)))
            {
                const std::array<ID3D11ShaderResourceView *, 2> cells{cells_view_.Get(),
                                                                      lower_view_.Get()};
                context_->VSSetShaderResources(0U, static_cast<UINT>(cells.size()), cells.data());
                context_->VSSetShader(terrain_vertex_shader_.Get(), nullptr, 0U);
                context_->IASetInputLayout(quadrant_layout_.Get());
                ID3D11Buffer *instances = quadrant_buffer_.Get();
                constexpr UINT stride = sizeof(world::FarQuadrantInstance);
                context_->IASetVertexBuffers(0U, 1U, &instances, &stride, &offset);
                context_->IASetIndexBuffer(quad_indices_.Get(), DXGI_FORMAT_R16_UINT, 0U);
                context_->DrawIndexedInstanced(quadrant_indices, static_cast<UINT>(quadrant_count),
                                               0U, 0, 0U);
                if (block_count > 0U)
                    context_->DrawIndexedInstanced(block_indices, static_cast<UINT>(block_count),
                                                   0U, 0, static_cast<UINT>(quadrant_count));
                stats_.triangles_submitted =
                    quadrant_count * quadrant_quads * 2U + block_count * block_quads * 2U;
            }
            if (box_count > 0U &&
                fill(box_buffer_.Get(), frame_.boxes.data(), box_count * sizeof(world::FarBoxInstance)))
            {
                context_->VSSetShader(box_vertex_shader_.Get(), nullptr, 0U);
                context_->IASetInputLayout(box_layout_.Get());
                ID3D11Buffer *instances = box_buffer_.Get();
                constexpr UINT stride = sizeof(world::FarBoxInstance);
                context_->IASetVertexBuffers(0U, 1U, &instances, &stride, &offset);
                context_->IASetIndexBuffer(quad_indices_.Get(), DXGI_FORMAT_R16_UINT, 0U);
                context_->DrawIndexedInstanced(36U, static_cast<UINT>(box_count), 0U, 0, 0U);
                stats_.triangles_submitted += box_count * 12U;
            }
            constexpr std::array<ID3D11ShaderResourceView *, 5> empty{};
            context_->PSSetShaderResources(0U, static_cast<UINT>(empty.size()), empty.data());
            context_->VSSetShaderResources(0U, 2U, empty.data());
            context_->OMSetRenderTargets(0U, nullptr, nullptr);
        }
        mark_timing(1U);

        // --- 2b (ii): the atmosphere over the scene, the far landscape under
        // it. Two draws over disjoint pixels: where the scene holds depth the
        // air is laid over it; where it holds none the landscape and the air
        // of the open sky go UNDER whatever is there.
        const std::array<ID3D11Buffer *, 1> atmosphere_buffer{atmosphere_constants_.Get()};
        context_->PSSetConstantBuffers(atmosphere_constant_slot, 1U, atmosphere_buffer.data());
        context_->PSSetConstantBuffers(0U, 1U, scene_buffer.data());
        context_->PSSetConstantBuffers(far_constant_slot, 1U, far_buffer.data());
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(screen_vertex_shader_.Get(), nullptr, 0U);
        ID3D11RenderTargetView *scene = pass.scene_target;
        context_->OMSetRenderTargets(1U, &scene, nullptr);
        context_->OMSetDepthStencilState(no_depth_state_.Get(), 0U);
        const std::array<ID3D11ShaderResourceView *, 4> views{
            pass.depth_view, color_view_.Get(), depth_view_.Get(), pass.surface_view};
        context_->PSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
        context_->PSSetShader(composite_pixel_shader_.Get(), nullptr, 0U);
        context_->OMSetBlendState(composite_blend_.Get(), nullptr, 0xFFFFFFFFU);
        context_->Draw(3U, 0U);
        context_->PSSetShader(under_pixel_shader_.Get(), nullptr, 0U);
        context_->OMSetBlendState(under_blend_.Get(), nullptr, 0xFFFFFFFFU);
        context_->Draw(3U, 0U);
        constexpr std::array<ID3D11ShaderResourceView *, 4> empty{};
        context_->PSSetShaderResources(0U, static_cast<UINT>(empty.size()), empty.data());
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->OMSetBlendState(opaque_blend_.Get(), nullptr, 0xFFFFFFFFU);
        // For the light-shaft pass that follows (post_processing.cpp): the
        // atmosphere's constants stay bound, and the far landscape's depth.
        ID3D11ShaderResourceView *far_depth = depth_view_.Get();
        context_->PSSetShaderResources(atmosphere_far_depth_slot, 1U, &far_depth);
        atmosphere_drawn_ = true;
        mark_timing(2U);
        end_timing();
    }

    // True for a frame whose scene went through draw(): the atmosphere was
    // applied and its constants are bound.
    [[nodiscard]] bool atmosphere_drawn() const noexcept
    {
        return atmosphere_drawn_;
    }

    [[nodiscard]] FarTerrainGpuStats stats() const noexcept
    {
        return stats_;
    }

  private:
    struct ShadowCascade final
    {
        // [0] the older field, [1] the newer one.
        std::array<ComPtr<ID3D11Texture2D>, 2> textures;
        std::array<ComPtr<ID3D11ShaderResourceView>, 2> views;
        std::array<std::array<int, 3>, 2> placement{}; // origin x, origin z, width in blocks
        std::uint32_t serial{};
        // When the newer field arrived and how long the older one had stood.
        float arrived_seconds{-1.0F};
        float interval_seconds{0.5F};
        bool pending_arrival{};
    };

    [[nodiscard]] static std::array<float, 4> field_placement(const ShadowCascade &cascade,
                                                              std::size_t which,
                                                              const std::array<double, 3> &eye) noexcept
    {
        if (cascade.views[which] == nullptr)
            return {};
        const auto &placement = cascade.placement[which];
        return {static_cast<float>(eye[0] - static_cast<double>(placement[0])),
                static_cast<float>(eye[2] - static_cast<double>(placement[1])),
                1.0F / static_cast<float>(std::max(placement[2], 1)), 1.0F};
    }

    // Weight of the newer of a cascade's two fields: it takes over across the
    // time the field before it had been showing, so the shadows of a moving
    // sun slide instead of stepping.
    [[nodiscard]] static float shadow_weight(const ShadowCascade &cascade,
                                             float elapsed_seconds) noexcept
    {
        if (cascade.arrived_seconds < 0.0F)
            return 1.0F;
        return std::clamp((elapsed_seconds - cascade.arrived_seconds) / cascade.interval_seconds,
                          0.0F, 1.0F);
    }

    [[nodiscard]] ID3D11ShaderResourceView *shadow_view(std::size_t cascade,
                                                        std::size_t which) const noexcept
    {
        return shadows_[cascade].views[which].Get();
    }

    void upload_shadow(std::size_t index, const world::FarShadowMap &map)
    {
        ShadowCascade &cascade = shadows_[index];
        if (map.heights.size() != static_cast<std::size_t>(map.size * map.size) * 2U)
            return;
        // The newer field becomes the older one; the texture that held the
        // older one is reused for what just arrived.
        std::swap(cascade.textures[0], cascade.textures[1]);
        std::swap(cascade.views[0], cascade.views[1]);
        std::swap(cascade.placement[0], cascade.placement[1]);
        if (cascade.textures[1] == nullptr || cascade.textures[1] == cascade.textures[0])
        {
            cascade.textures[1].Reset();
            cascade.views[1].Reset();
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(map.size);
            description.Height = static_cast<UINT>(map.size);
            description.MipLevels = 1U;
            description.ArraySize = 1U;
            description.Format = DXGI_FORMAT_R16G16_UNORM;
            description.SampleDesc.Count = 1U;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device_->CreateTexture2D(&description, nullptr, &cascade.textures[1])) ||
                FAILED(device_->CreateShaderResourceView(cascade.textures[1].Get(), nullptr,
                                                         &cascade.views[1])))
            {
                cascade.textures[1].Reset();
                cascade.views[1].Reset();
                return;
            }
        }
        context_->UpdateSubresource(cascade.textures[1].Get(), 0U, nullptr, map.heights.data(),
                                    static_cast<UINT>(map.size) * 4U, 0U);
        cascade.placement[1] = {map.origin_x, map.origin_z, map.size * map.texel};
        cascade.serial = map.serial;
        cascade.pending_arrival = true;
        if (cascade.views[0] == nullptr)
        {
            // The very first field stands for both.
            cascade.textures[0] = cascade.textures[1];
            cascade.views[0] = cascade.views[1];
            cascade.placement[0] = cascade.placement[1];
        }
    }

    void write_constants(const FarTerrainPass &pass, bool landscape)
    {
        FarConstants constants{};
        AtmosphereConstants atmosphere{};
        const std::array<double, 3> floor{std::floor(pass.eye[0]), std::floor(pass.eye[1]),
                                          std::floor(pass.eye[2])};
        constants.eye_block = {static_cast<std::int32_t>(floor[0]),
                               static_cast<std::int32_t>(floor[1]),
                               static_cast<std::int32_t>(floor[2]), 0};
        constants.eye_fraction = {static_cast<float>(pass.eye[0] - floor[0]),
                                  static_cast<float>(pass.eye[1] - floor[1]),
                                  static_cast<float>(pass.eye[2] - floor[2]), far_near_plane};
        const float distance = landscape ? static_cast<float>(frame_.settings.distance) : 0.0F;
        const float hell = std::clamp(pass.sky.hell, 0.0F, 1.0F);
        // (Deep underground there is no landscape overhead to show.)
        const float presence = landscape ? std::clamp(frame_.opacity, 0.0F, 1.0F) : 0.0F;
        constants.far_range = {distance, distance * 0.84F, frame_.handover.clip,
                               presence * (1.0F - hell)};

        // The one atmosphere (atmosphere_shader.hpp). Its clear air is as
        // thick as the view is short: to the far landscape's end, or to the
        // edge of the detailed window where there is none.
        const float daylight = std::clamp(pass.sky.daylight, 0.0F, 1.0F);
        const float low_sun = 1.0F - std::clamp(std::abs(pass.sky.sun_y) / 0.42F, 0.0F, 1.0F);
        const double eye_radius = std::hypot(pass.eye[0], pass.eye[2]);
        const world::SurfaceAtmosphere air = world::surface_atmosphere(
            static_cast<float>(eye_radius), std::max(pass.visibility, 32.0F), distance, presence);
        // Mist gathers at dawn and dusk and thins a little at night.
        const float gathering = 0.80F + 0.20F * daylight + 0.35F * low_sun * daylight;
        float mist_density = air.mist.density * gathering;
        float haze_density = air.haze.density * gathering;
        // Mist that no ray of this frame can reach costs nothing: the band
        // (or its nearest repeat) is further away than anything is drawn.
        {
            // How far outside the band's outer edge the eye is and, for a
            // repeat of the band (a ring), how far inside its inner edge.
            float outside = 1.0e9F;
            float inside = 1.0e9F;
            bool ring{};
            for (const world::MistDisc &disc : air.discs)
            {
                if (disc.weight > 0.0F)
                    outside = std::min(outside, static_cast<float>(eye_radius) - disc.radius);
                else if (disc.weight < 0.0F)
                {
                    ring = true;
                    inside = std::min(inside, disc.radius - static_cast<float>(eye_radius));
                }
            }
            const float reach = std::max(distance, pass.visibility) + 64.0F;
            if (outside > reach || (ring && inside > reach))
            {
                mist_density = 0.0F;
                haze_density = 0.0F;
            }
        }
        // OPENMINERZ_FAR_DEBUG=transmittance|inscatter: the air alone, as the
        // light that survives it or the light it adds.
        static const float debug_air = [] {
            const char *text = SDL_getenv("OPENMINERZ_FAR_DEBUG");
            const std::string_view view{text != nullptr ? text : ""};
            return view == "transmittance" ? 1.0F : (view == "inscatter" ? 2.0F : 0.0F);
        }();
        atmosphere.air = {air.air.density, air.air.floor, 1.0F / air.air.height, 1.0F - hell};
        atmosphere.mist = {mist_density, air.mist.floor, 1.0F / air.mist.height,
                           std::clamp(pass.eye_sky_light, 0.0F, 1.0F)};
        atmosphere.haze = {haze_density, air.haze.floor, 1.0F / air.haze.height, debug_air};
        for (std::size_t index = 0U; index < air.discs.size(); ++index)
        {
            // |eye|^2 - radius^2 in double: two large numbers that nearly cancel.
            const double radius = static_cast<double>(air.discs[index].radius);
            atmosphere.discs[index] = {
                static_cast<float>((eye_radius - radius) * (eye_radius + radius)),
                air.discs[index].weight, static_cast<float>(radius * radius), 0.0F};
        }
        atmosphere.eye = {static_cast<float>(pass.eye[0]), static_cast<float>(pass.eye[2]),
                          static_cast<float>(pass.eye[1]), landscape ? far_near_plane : 0.0F};
        const float pixel_scale = 0.5F * static_cast<float>(pass.height);
        constants.world_origin = {static_cast<float>(pass.eye[0]), static_cast<float>(pass.eye[2]),
                                  pass.elapsed_seconds, pixel_scale};

        // Sunlight scattered forward through the air: the glow round a sun
        // that stands in the sky (or has only just left it), in the
        // sunlight's own colour. Little of it is left under a storm.
        const float storm = std::clamp(pass.storminess, 0.0F, 1.0F);
        if (hell < 0.999F && pass.sky.sun_y > -0.12F)
        {
            const float rise = std::clamp((pass.sky.sun_y + 0.10F) / 0.12F, 0.0F, 1.0F);
            const float visibility = (1.0F - hell) * rise * rise * (3.0F - 2.0F * rise);
            // The sunlight that reaches the air round the eye: what the air
            // above it lets through on the sun's slant.
            world::SurfaceAtmosphere above = air;
            above.mist.density = mist_density;
            above.haze.density = haze_density;
            const float slant =
                world::overhead_depth(above, static_cast<float>(pass.eye[0]),
                                      static_cast<float>(pass.eye[1]),
                                      static_cast<float>(pass.eye[2])) /
                std::max(pass.sky.sun_y, 0.12F);
            // (The scene's own sunlight, LightAndFogStart.rgb, and how much of
            // it a unit of air sends on; heavy cloud takes some of it.)
            const float gain =
                0.45F * visibility * std::lerp(1.0F, 0.60F, storm) * std::exp(-slant);
            atmosphere.sun = {pass.sky.direct.red * gain, pass.sky.direct.green * gain,
                              pass.sky.direct.blue * gain,
                              (1.0F - std::clamp(pass.sky.sun_y * 3.5F, 0.0F, 1.0F)) * visibility};
        }
        // About half of what clear daytime air sends the eye is sunlight it
        // scattered; the rest is the sky's. A terrain shadow removes the first.
        atmosphere.sun_direction = {
            air.edge_depth, 1.0F / air.edge_reach,
            world::closing_air_ceiling(static_cast<float>(pass.eye[1])),
            0.50F * daylight * (1.0F - 0.6F * storm)};
        if (trace_ && ++trace_frames_ == 240U)
            SDL_Log("FARTERRAIN air density %.6f floor %.1f height %.1f, mist %.5f haze %.5f, "
                    "sun %.3f %.3f %.3f low %.2f (sun_y %.3f storm %.2f direct %.2f %.2f %.2f), "
                    "sunlit share %.2f, strength %.2f",
                    static_cast<double>(air.air.density), static_cast<double>(air.air.floor),
                    static_cast<double>(air.air.height), static_cast<double>(mist_density),
                    static_cast<double>(haze_density), static_cast<double>(atmosphere.sun[0]),
                    static_cast<double>(atmosphere.sun[1]), static_cast<double>(atmosphere.sun[2]),
                    static_cast<double>(atmosphere.sun[3]), static_cast<double>(pass.sky.sun_y),
                    static_cast<double>(storm), static_cast<double>(pass.sky.direct.red),
                    static_cast<double>(pass.sky.direct.green),
                    static_cast<double>(pass.sky.direct.blue),
                    static_cast<double>(atmosphere.sun_direction[3]),
                    static_cast<double>(atmosphere.air[3]));
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(atmosphere_constants_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                        &mapped)))
            {
                std::memcpy(mapped.pData, &atmosphere, sizeof(atmosphere));
                context_->Unmap(atmosphere_constants_.Get(), 0U);
            }
        }

        for (std::size_t cascade = 0U; cascade < 2U; ++cascade)
        {
            ShadowCascade &shadow = shadows_[cascade];
            if (shadow.pending_arrival)
            {
                // Timed from the first frame that draws it.
                shadow.interval_seconds =
                    shadow.arrived_seconds < 0.0F
                        ? 0.001F
                        : std::clamp(pass.elapsed_seconds - shadow.arrived_seconds, 0.12F, 1.5F);
                shadow.arrived_seconds = pass.elapsed_seconds;
                shadow.pending_arrival = false;
            }
            constants.shadow_field[cascade * 2U] = field_placement(shadow, 0U, pass.eye);
            constants.shadow_field[cascade * 2U + 1U] = field_placement(shadow, 1U, pass.eye);
            constants.shadow_blend[cascade] = shadow_weight(shadow, pass.elapsed_seconds);
        }
        constants.shadow_blend[2] = std::clamp(pass.shadow_opacity, 0.0F, 1.0F);
        static const float debug_view = [] {
            const char *text = SDL_getenv("OPENMINERZ_FAR_DEBUG");
            const std::string_view name{text != nullptr ? text : ""};
            return name == "shadow" ? 1.0F : name == "sky" ? 2.0F : name == "morph" ? 3.0F : 0.0F;
        }();
        constants.shadow_blend[3] = debug_view;
        constants.box_origin = {frame_.box_origin[0], frame_.box_origin[1], frame_.box_origin[2],
                                0};
        constants.blocks = blocks_;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(constants_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return;
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constants_.Get(), 0U);
    }

    [[nodiscard]] bool fill(ID3D11Buffer *buffer, const void *data, std::size_t bytes)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(buffer, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return false;
        std::memcpy(mapped.pData, data, bytes);
        context_->Unmap(buffer, 0U);
        return true;
    }

    [[nodiscard]] bool fail(const char *what)
    {
        failure_ = std::string{"could not create the "} + what;
        return false;
    }

    [[nodiscard]] bool create_shaders()
    {
        const std::string scene{scene_source};
        const auto vertex = compile(scene + terrain_vertex_source, "vs_5_0", failure_);
        const auto box = compile(scene + box_vertex_source, "vs_5_0", failure_);
        const auto pixel = compile(scene + terrain_pixel_source, "ps_5_0", failure_);
        const auto screen = compile(screen_vertex_source, "vs_5_0", failure_);
        const auto composite =
            compile(scene + atmosphere_hlsl + composite_pixel_source, "ps_5_0", failure_);
        const auto under = compile(scene + "#define ATMOSPHERE_UNDER 1\n" + atmosphere_hlsl +
                                       composite_pixel_source,
                                   "ps_5_0", failure_);
        if (vertex == nullptr || box == nullptr || pixel == nullptr || screen == nullptr ||
            composite == nullptr || under == nullptr)
            return false;
        if (FAILED(device_->CreatePixelShader(under->GetBufferPointer(), under->GetBufferSize(),
                                              nullptr, &under_pixel_shader_)))
            return fail("atmosphere shader");
        if (FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
                                               nullptr, &terrain_vertex_shader_)) ||
            FAILED(device_->CreateVertexShader(box->GetBufferPointer(), box->GetBufferSize(),
                                               nullptr, &box_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
                                              nullptr, &terrain_pixel_shader_)) ||
            FAILED(device_->CreateVertexShader(screen->GetBufferPointer(), screen->GetBufferSize(),
                                               nullptr, &screen_vertex_shader_)) ||
            FAILED(device_->CreatePixelShader(composite->GetBufferPointer(),
                                              composite->GetBufferSize(), nullptr,
                                              &composite_pixel_shader_)))
            return fail("shaders");
        const std::array<D3D11_INPUT_ELEMENT_DESC, 3> quadrant_elements{{
            {"ORIGIN", 0U, DXGI_FORMAT_R32G32_SINT, 0U, 0U, D3D11_INPUT_PER_INSTANCE_DATA, 1U},
            {"PACKED", 0U, DXGI_FORMAT_R32_UINT, 0U, 8U, D3D11_INPUT_PER_INSTANCE_DATA, 1U},
            {"MORPH", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 12U, D3D11_INPUT_PER_INSTANCE_DATA,
             1U},
        }};
        const std::array<D3D11_INPUT_ELEMENT_DESC, 3> box_elements{{
            {"BOXMIN", 0U, DXGI_FORMAT_R16G16B16A16_SINT, 0U, 0U, D3D11_INPUT_PER_INSTANCE_DATA,
             1U},
            {"BOXSIZE", 0U, DXGI_FORMAT_R8G8B8A8_UINT, 0U, 8U, D3D11_INPUT_PER_INSTANCE_DATA, 1U},
            {"BOXTILE", 0U, DXGI_FORMAT_R8G8B8A8_UINT, 0U, 12U, D3D11_INPUT_PER_INSTANCE_DATA, 1U},
        }};
        if (FAILED(device_->CreateInputLayout(quadrant_elements.data(),
                                              static_cast<UINT>(quadrant_elements.size()),
                                              vertex->GetBufferPointer(), vertex->GetBufferSize(),
                                              &quadrant_layout_)) ||
            FAILED(device_->CreateInputLayout(box_elements.data(),
                                              static_cast<UINT>(box_elements.size()),
                                              box->GetBufferPointer(), box->GetBufferSize(),
                                              &box_layout_)))
            return fail("input layouts");
        return true;
    }

    [[nodiscard]] bool create_states()
    {
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_GREATER;
        if (FAILED(device_->CreateDepthStencilState(&depth, &depth_state_)))
            return fail("depth state");
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device_->CreateDepthStencilState(&depth, &no_depth_state_)))
            return fail("composite depth state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        // A side face points whichever way its taller column is, which the
        // vertex shader decides, so nothing can be culled by winding.
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rasterizer, &rasterizer_)))
            return fail("rasterizer state");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend, &opaque_blend_)))
            return fail("blend state");
        // target = target * keep + add, with `keep` the second colour the
        // pixel shader writes. Alpha is left as the scene had it.
        auto &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_SRC1_COLOR;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device_->CreateBlendState(&blend, &composite_blend_)))
            return fail("composite blend state");
        // dst + src * (1 - dst.a), colour and alpha alike: under what is there.
        target.SrcBlend = D3D11_BLEND_INV_DEST_ALPHA;
        target.DestBlend = D3D11_BLEND_ONE;
        target.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device_->CreateBlendState(&blend, &under_blend_)))
            return fail("under blend state");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_ANISOTROPIC;
        sampler.MaxAnisotropy = 4U;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler, &atlas_sampler_)))
            return fail("atlas sampler");
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.MaxAnisotropy = 1U;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device_->CreateSamplerState(&sampler, &field_sampler_)))
            return fail("shadow field sampler");

        if (trace_)
        {
            const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0U};
            const D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0U};
            for (TimingFrame &frame : timing_)
            {
                if (FAILED(device_->CreateQuery(&disjoint, &frame.disjoint)))
                    trace_ = false;
                for (auto &query : frame.stamps)
                    if (FAILED(device_->CreateQuery(&stamp, &query)))
                        trace_ = false;
            }
        }
        return true;
    }

    [[nodiscard]] bool create_buffers()
    {
        blocks_ = block_table();
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(FarConstants);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &constants_)))
            return fail("constant buffer");
        buffer.ByteWidth = sizeof(NearShadowConstants);
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &near_shadow_constants_)))
            return fail("near shadow constant buffer");
        buffer.ByteWidth = sizeof(AtmosphereConstants);
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &atmosphere_constants_)))
            return fail("atmosphere constant buffer");
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.ByteWidth = static_cast<UINT>(maximum_quadrants * sizeof(world::FarQuadrantInstance));
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &quadrant_buffer_)))
            return fail("quadrant buffer");
        buffer.ByteWidth = static_cast<UINT>(maximum_boxes * sizeof(world::FarBoxInstance));
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &box_buffer_)))
            return fail("box buffer");

        // Two triangles per quad over four vertices whose number is the quad's
        // times four plus the corner; the vertex shaders work out the rest. A
        // box is the first six quads of the same buffer.
        std::vector<std::uint16_t> indices(quadrant_indices);
        for (UINT quad = 0U; quad < quadrant_quads; ++quad)
        {
            const auto first = static_cast<std::uint16_t>(quad * 4U);
            const std::array<std::uint16_t, 6> order{0U, 1U, 2U, 2U, 1U, 3U};
            for (std::size_t index = 0U; index < order.size(); ++index)
                indices[quad * 6U + index] = static_cast<std::uint16_t>(first + order[index]);
        }
        D3D11_BUFFER_DESC index_buffer{};
        index_buffer.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        index_buffer.Usage = D3D11_USAGE_IMMUTABLE;
        index_buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{indices.data(), 0U, 0U};
        if (FAILED(device_->CreateBuffer(&index_buffer, &data, &quad_indices_)))
            return fail("index buffer");
        return true;
    }

    [[nodiscard]] bool create_cells(std::uint32_t capacity)
    {
        cells_texture_.Reset();
        cells_view_.Reset();
        lower_texture_.Reset();
        lower_view_.Reset();
        capacity_ = 0U;
        if (capacity == 0U)
            return false;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(world::far_tile_grid);
        description.Height = static_cast<UINT>(world::far_tile_grid);
        description.MipLevels = 1U;
        description.ArraySize = capacity;
        description.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // Two arrays of the same shape: the cell maps, and the second layer of
        // the tiles that have floating columns, slice for slice.
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &cells_texture_)) ||
            FAILED(device_->CreateShaderResourceView(cells_texture_.Get(), nullptr, &cells_view_)) ||
            FAILED(device_->CreateTexture2D(&description, nullptr, &lower_texture_)) ||
            FAILED(device_->CreateShaderResourceView(lower_texture_.Get(), nullptr, &lower_view_)))
        {
            cells_texture_.Reset();
            cells_view_.Reset();
            lower_texture_.Reset();
            lower_view_.Reset();
            return false;
        }
        capacity_ = capacity;
        stats_.array_bytes = static_cast<std::size_t>(capacity) *
                             static_cast<std::size_t>(world::far_tile_grid * world::far_tile_grid) *
                             4U * 2U;
        return true;
    }

    [[nodiscard]] bool ensure_targets(int width, int height)
    {
        if (color_texture_ != nullptr && width == width_ && height == height_)
            return true;
        color_texture_.Reset();
        color_target_.Reset();
        color_view_.Reset();
        depth_texture_.Reset();
        depth_target_.Reset();
        depth_view_.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        // Lit terrain only: no sign, no alpha, and the smooth gradients (the
        // air) are added in the composite at the scene's own precision. Four
        // bytes a pixel instead of eight matters at 5120 x 1440.
        description.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bool created =
            SUCCEEDED(device_->CreateTexture2D(&description, nullptr, &color_texture_)) &&
            SUCCEEDED(device_->CreateRenderTargetView(color_texture_.Get(), nullptr,
                                                      &color_target_)) &&
            SUCCEEDED(device_->CreateShaderResourceView(color_texture_.Get(), nullptr, &color_view_));
        description.Format = DXGI_FORMAT_R32_TYPELESS;
        description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC target{};
        target.Format = DXGI_FORMAT_D32_FLOAT;
        target.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1U;
        created = created &&
                  SUCCEEDED(device_->CreateTexture2D(&description, nullptr, &depth_texture_)) &&
                  SUCCEEDED(device_->CreateDepthStencilView(depth_texture_.Get(), &target,
                                                            &depth_target_)) &&
                  SUCCEEDED(device_->CreateShaderResourceView(depth_texture_.Get(), &view,
                                                              &depth_view_));
        if (!created)
        {
            color_texture_.Reset();
            width_ = 0;
            height_ = 0;
            return false;
        }
        width_ = width;
        height_ = height;
        stats_.target_bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * (4U + 4U);
        return true;
    }

    // --- GPU timing ----------------------------------------------------------
    struct TimingFrame final
    {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>, 3> stamps;
        bool pending{};
    };

    void begin_timing()
    {
        if (!trace_)
            return;
        // Collect a frame issued a few frames ago; never wait for the GPU.
        TimingFrame &oldest = timing_[(timing_index_ + 1U) % timing_.size()];
        if (oldest.pending)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            std::array<UINT64, 3> stamps{};
            bool complete = context_->GetData(oldest.disjoint.Get(), &disjoint, sizeof(disjoint),
                                              D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            for (std::size_t index = 0U; complete && index < stamps.size(); ++index)
                complete = context_->GetData(oldest.stamps[index].Get(), &stamps[index],
                                             sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (complete)
            {
                oldest.pending = false;
                if (disjoint.Disjoint == FALSE && disjoint.Frequency != 0U)
                {
                    const double scale = 1000.0 / static_cast<double>(disjoint.Frequency);
                    const double geometry = static_cast<double>(stamps[1] - stamps[0]) * scale;
                    const double composite = static_cast<double>(stamps[2] - stamps[1]) * scale;
                    const auto count = static_cast<double>(stats_.frames);
                    stats_.geometry_milliseconds =
                        (stats_.geometry_milliseconds * count + geometry) / (count + 1.0);
                    stats_.composite_milliseconds =
                        (stats_.composite_milliseconds * count + composite) / (count + 1.0);
                    // (The first frames compile state objects in the driver.)
                    if (stats_.frames > 8U)
                    {
                        stats_.geometry_peak_milliseconds =
                            std::max(stats_.geometry_peak_milliseconds, geometry);
                        stats_.composite_peak_milliseconds =
                            std::max(stats_.composite_peak_milliseconds, composite);
                    }
                    ++stats_.frames;
                }
            }
        }
        timing_index_ = (timing_index_ + 1U) % timing_.size();
        TimingFrame &frame = timing_[timing_index_];
        timing_active_ = !frame.pending;
        if (!timing_active_)
            return;
        context_->Begin(frame.disjoint.Get());
        context_->End(frame.stamps[0].Get());
    }

    void mark_timing(std::size_t index)
    {
        if (trace_ && timing_active_)
            context_->End(timing_[timing_index_].stamps[index].Get());
    }

    void end_timing()
    {
        if (!trace_ || !timing_active_)
            return;
        context_->End(timing_[timing_index_].disjoint.Get());
        timing_[timing_index_].pending = true;
    }

    ID3D11Device *device_{};
    ID3D11DeviceContext *context_{};
    bool ready_{};
    bool trace_{};
    std::string failure_;
    world::FarTerrainFrame frame_{};
    // The instance buffer's contents for the frame being drawn.
    std::vector<world::FarQuadrantInstance> instances_;
    std::uint32_t generation_{};
    std::uint32_t capacity_{};
    int width_{};
    int height_{};
    std::array<std::array<std::uint32_t, 4>, 256> blocks_{};
    std::array<ShadowCascade, 2> shadows_{};
    FarTerrainGpuStats stats_{};
    std::array<TimingFrame, 5> timing_{};
    std::size_t timing_index_{};
    bool timing_active_{};

    ComPtr<ID3D11VertexShader> terrain_vertex_shader_;
    ComPtr<ID3D11VertexShader> box_vertex_shader_;
    ComPtr<ID3D11PixelShader> terrain_pixel_shader_;
    ComPtr<ID3D11VertexShader> screen_vertex_shader_;
    ComPtr<ID3D11PixelShader> composite_pixel_shader_;
    ComPtr<ID3D11PixelShader> under_pixel_shader_;
    ComPtr<ID3D11BlendState> under_blend_;
    ComPtr<ID3D11Buffer> atmosphere_constants_;
    bool atmosphere_drawn_{};
    std::uint32_t trace_frames_{};
    ComPtr<ID3D11InputLayout> quadrant_layout_;
    ComPtr<ID3D11InputLayout> box_layout_;
    ComPtr<ID3D11DepthStencilState> depth_state_;
    ComPtr<ID3D11DepthStencilState> no_depth_state_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> opaque_blend_;
    ComPtr<ID3D11BlendState> composite_blend_;
    ComPtr<ID3D11SamplerState> atlas_sampler_;
    ComPtr<ID3D11SamplerState> field_sampler_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Buffer> near_shadow_constants_;
    ComPtr<ID3D11Buffer> quadrant_buffer_;
    ComPtr<ID3D11Buffer> box_buffer_;
    ComPtr<ID3D11Buffer> quad_indices_;
    ComPtr<ID3D11Texture2D> cells_texture_;
    ComPtr<ID3D11ShaderResourceView> cells_view_;
    ComPtr<ID3D11Texture2D> lower_texture_;
    ComPtr<ID3D11ShaderResourceView> lower_view_;
    ComPtr<ID3D11Texture2D> color_texture_;
    ComPtr<ID3D11RenderTargetView> color_target_;
    ComPtr<ID3D11ShaderResourceView> color_view_;
    ComPtr<ID3D11Texture2D> depth_texture_;
    ComPtr<ID3D11DepthStencilView> depth_target_;
    ComPtr<ID3D11ShaderResourceView> depth_view_;
};

FarTerrainRenderer::FarTerrainRenderer(ID3D11Device *device, ID3D11DeviceContext *context)
    : impl_(std::make_unique<Impl>(device, context))
{
}

FarTerrainRenderer::~FarTerrainRenderer() = default;

bool FarTerrainRenderer::ready() const noexcept
{
    return impl_->ready();
}

void FarTerrainRenderer::submit(world::FarTerrainFrame frame)
{
    impl_->submit(std::move(frame));
}

bool FarTerrainRenderer::active() const noexcept
{
    return impl_->active();
}

world::FarHandover FarTerrainRenderer::handover() const noexcept
{
    return impl_->handover();
}

void FarTerrainRenderer::draw(const FarTerrainPass &pass)
{
    impl_->draw(pass);
}

bool FarTerrainRenderer::atmosphere_drawn() const noexcept
{
    return impl_->atmosphere_drawn();
}

void FarTerrainRenderer::bind_near_shadow(const std::array<double, 3> &eye, float elapsed_seconds,
                                          float opacity)
{
    impl_->bind_near_shadow(eye, elapsed_seconds, opacity);
}

void FarTerrainRenderer::unbind_near_shadow()
{
    impl_->unbind_near_shadow();
}

FarTerrainGpuStats FarTerrainRenderer::stats() const noexcept
{
    return impl_->stats();
}

void FarTerrainRenderer::set_mod_palette(const world::ModPaletteLooks &looks,
                                         std::span<const world::BlockType> fallbacks,
                                         std::span<const world::FarSurfaceProfile> profiles)
{
    impl_->set_mod_palette(looks, fallbacks, profiles);
}

#endif

} // namespace openminerz
