#include "mod_light_gpu.hpp"

#include "shader_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

namespace openminerz
{

// The cluster grid's constants, as both shaders read them (b10 in the lit
// pixel shaders, b0 in the cluster build; the lights follow in b11 / b1).
// modding::graphics::cluster_of and cluster_box (lights.cpp) are the
// reference of everything done with them.
//
//   ModLightEye         xyz the eye, w one when there are lights this frame
//   ModLightRight/Up    the camera's axes; w the tiles across / down
//   ModLightForward     the camera's axis into the screen; w the light count
//   ModLightProjection  xy NDC per unit at depth one, z ClusterGrid::first_depth,
//                       w one over ClusterGrid::log2_growth()
//   ModLightRange       x ClusterGrid::far_depth, y ClusterGrid::ndc_margin,
//                       z ClusterGrid::log2_growth()

const char *mod_light_hlsl() noexcept
{
    // The maths per light must match modding::graphics::light_at (lights.cpp),
    // which the tests check and the first-person body uses.
    return R"hlsl(
cbuffer ModLightClusters : register(b10)
{
    float4 ModLightEye;
    float4 ModLightRight;
    float4 ModLightUp;
    float4 ModLightForward;
    float4 ModLightProjection;
    float4 ModLightRange;
};
// Three float4 per light (ModLightGpu::begin_frame); after the last light one
// that reaches nothing, which pads the cluster lists to groups of four.
cbuffer ModLightData : register(b11)
{
    float4 ModLights[1539];
};
// modding::graphics::ClusterGrid::slices and max_per_cluster.
static const uint ModLightSlices = 24u;
static const uint ModLightPerCluster = 64u;
Buffer<uint> ModLightCount : register(t26);
// Four light indices per element.
Buffer<uint4> ModLightIndex : register(t27);

float3 mod_point_light(uint light, float3 world_position, float3 normal, float wrap)
{
    uint base = light * 3u;
    // w is one over the radius squared.
    float4 position_reach = ModLights[base];
    float3 offset = position_reach.xyz - world_position;
    float range_squared = dot(offset, offset);
    float reached = range_squared * position_reach.w;
    if (reached >= 1.0f)
        return 0.0f.xxx;
    float4 color_cone = ModLights[base + 1u];
    float inverse_range = rsqrt(max(range_squared, 1.0e-6f));
    float share = 1.0f - sqrt(reached);
    share *= share;
    float3 to_light = offset * inverse_range;
    share *= saturate(dot(normal, to_light) * (1.0f - wrap) + wrap);
    if (color_cone.w > -1.5f)
    {
        float4 axis_inner = ModLights[base + 2u];
        float cone = saturate((dot(-to_light, axis_inner.xyz) - color_cone.w) /
                              max(axis_inner.w - color_cone.w, 0.0001f));
        share *= cone * cone * (3.0f - 2.0f * cone);
    }
    return color_cone.rgb * share;
}

// The light mod lighting props put on a surface: `wrap` is how much light a
// surface facing away still receives (0 none .. 1 all).
float3 mod_point_lights(float3 world_position, float3 normal, float wrap)
{
    float3 total = 0.0f.xxx;
    if (ModLightEye.w < 0.5f)
        return total;
    float3 view = world_position - ModLightEye.xyz;
    float depth = dot(view, ModLightForward.xyz);
    // Deeper than any light reaches.
    if (depth > ModLightRange.x)
        return total;
    float2 ndc = float2(dot(view, ModLightRight.xyz) * ModLightProjection.x,
                        dot(view, ModLightUp.xyz) * ModLightProjection.y) / max(depth, 1.0e-6f);
    if (depth > 0.0f && all(abs(ndc) <= ModLightRange.yy))
    {
        int2 tiles = int2(ModLightRight.w, ModLightUp.w);
        int2 tile = clamp(int2(floor((ndc * 0.5f + 0.5f) * float2(tiles))), int2(0, 0), tiles - 1);
        int slice = depth < ModLightProjection.z
                        ? 0
                        : min(int(ModLightSlices) - 1,
                              1 + int(floor(log2(depth / ModLightProjection.z) *
                                            ModLightProjection.w)));
        uint cluster = (uint(slice) * uint(tiles.y) + uint(tile.y)) * uint(tiles.x) + uint(tile.x);
        uint groups = (min(ModLightCount.Load(cluster), ModLightPerCluster) + 3u) / 4u;
        uint first = cluster * (ModLightPerCluster / 4u);
        // Four lights a step: their loads go out together.
        [loop]
        for (uint group = 0u; group < groups; ++group)
        {
            uint4 lights = ModLightIndex.Load(first + group);
            total += mod_point_light(lights.x, world_position, normal, wrap) +
                     mod_point_light(lights.y, world_position, normal, wrap) +
                     mod_point_light(lights.z, world_position, normal, wrap) +
                     mod_point_light(lights.w, world_position, normal, wrap);
        }
        return total;
    }
    // Off the grid (behind the eye, or off the screen): every light.
    uint lights = uint(ModLightForward.w);
    [loop]
    for (uint light = 0u; light < lights; ++light)
        total += mod_point_light(light, world_position, normal, wrap);
    return total;
}
)hlsl";
}

const char *mod_light_cluster_hlsl() noexcept
{
    // One thread per cluster: its view-space box (cluster_box), then every
    // light in importance order, kept when its sphere touches the box. The
    // count is every such light, so a full cluster's overflow can be read
    // back; the pixel shaders read at most ModLightPerCluster of them.
    return R"hlsl(
cbuffer ModLightClusters : register(b0)
{
    float4 ModLightEye;
    float4 ModLightRight;
    float4 ModLightUp;
    float4 ModLightForward;
    float4 ModLightProjection;
    float4 ModLightRange;
};
cbuffer ModLightData : register(b1)
{
    float4 ModLights[1539];
};
static const uint ModLightSlices = 24u;
static const uint ModLightPerCluster = 64u;
RWBuffer<uint> ModLightCountOut : register(u0);
RWBuffer<uint> ModLightIndexOut : register(u1);

float slice_start(uint slice)
{
    return slice == 0u ? 0.0f
                       : ModLightProjection.z * exp2(float(slice - 1u) * ModLightRange.z);
}

float tile_edge(uint edge, uint tiles)
{
    return edge == 0u ? -ModLightRange.y
                      : (edge == tiles ? ModLightRange.y
                                       : -1.0f + 2.0f * float(edge) / float(tiles));
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    uint tiles_x = uint(ModLightRight.w);
    uint tiles_y = uint(ModLightUp.w);
    uint cluster = thread.x;
    if (cluster >= tiles_x * tiles_y * ModLightSlices)
        return;
    uint tile_x = cluster % tiles_x;
    uint tile_y = (cluster / tiles_x) % tiles_y;
    uint slice = cluster / (tiles_x * tiles_y);
    float2 low = float2(tile_edge(tile_x, tiles_x), tile_edge(tile_y, tiles_y));
    float2 high = float2(tile_edge(tile_x + 1u, tiles_x), tile_edge(tile_y + 1u, tiles_y));
    float near_depth = slice_start(slice);
    float far_depth = slice + 1u == ModLightSlices ? max(ModLightRange.x, near_depth)
                                                   : slice_start(slice + 1u);
    float2 scale = max(ModLightProjection.xy, 1.0e-6f);
    float2 a = low * near_depth;
    float2 b = low * far_depth;
    float2 c = high * near_depth;
    float2 d = high * far_depth;
    float pad = 0.01f + 0.002f * far_depth;
    float3 box_min = float3(min(min(a, b), min(c, d)) / scale, near_depth) - pad;
    float3 box_max = float3(max(max(a, b), max(c, d)) / scale, far_depth) + pad;
    uint lights = uint(ModLightForward.w);
    uint first = cluster * ModLightPerCluster;
    uint count = 0u;
    [loop]
    for (uint light = 0u; light < lights; ++light)
    {
        // w is one over the radius squared.
        float4 position_reach = ModLights[light * 3u];
        float3 offset = position_reach.xyz - ModLightEye.xyz;
        float3 center = float3(dot(offset, ModLightRight.xyz), dot(offset, ModLightUp.xyz),
                               dot(offset, ModLightForward.xyz));
        float3 gap = clamp(center, box_min, box_max) - center;
        if (dot(gap, gap) * position_reach.w > 1.0f)
            continue;
        if (count < ModLightPerCluster)
            ModLightIndexOut[first + count] = light;
        ++count;
    }
    // The last group of four is filled up with the light that reaches nothing.
    uint kept = min(count, ModLightPerCluster);
    for (uint slot = kept; slot < ((kept + 3u) & ~3u); ++slot)
        ModLightIndexOut[first + slot] = lights;
    ModLightCountOut[cluster] = count;
}
)hlsl";
}

#if defined(_WIN32)

namespace
{

struct ClusterConstants final
{
    float eye[4]{};
    float right[4]{};
    float up[4]{};
    float forward[4]{};
    float projection[4]{};
    float range[4]{};
};

constexpr UINT cluster_group_size = 64U;

Microsoft::WRL::ComPtr<ID3D11Buffer> dynamic_buffer(ID3D11Device *device, UINT bytes, UINT bind)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = bind;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&description, nullptr, &buffer)))
        return {};
    return buffer;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> typed_view(ID3D11Device *device,
                                                            ID3D11Buffer *buffer,
                                                            DXGI_FORMAT format, UINT elements)
{
    if (buffer == nullptr)
        return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = format;
    description.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0U;
    description.Buffer.NumElements = elements;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(buffer, &description, &view)))
        return {};
    return view;
}

Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> typed_writer(ID3D11Device *device,
                                                               ID3D11Buffer *buffer,
                                                               DXGI_FORMAT format, UINT elements)
{
    if (buffer == nullptr)
        return {};
    D3D11_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = format;
    description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0U;
    description.Buffer.NumElements = elements;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> view;
    if (FAILED(device->CreateUnorderedAccessView(buffer, &description, &view)))
        return {};
    return view;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> gpu_buffer(ID3D11Device *device, UINT bytes)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&description, nullptr, &buffer)))
        return {};
    return buffer;
}

template <typename Item>
bool upload(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const Item *items,
            std::size_t count)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
        return false;
    if (count > 0U)
        std::memcpy(mapped.pData, items, count * sizeof(Item));
    context->Unmap(buffer, 0U);
    return true;
}

} // namespace

ModLightGpu::ModLightGpu(ID3D11Device *device, ID3D11DeviceContext *context)
    : device_(device), context_(context)
{
    static_assert(mod_light_slots == 1539U, "the shaders declare ModLights[1539]");
    static_assert(mod_light_slots * 16U <= 65536U, "a constant buffer holds at most 64 KiB");
    constants_ = dynamic_buffer(device, sizeof(ClusterConstants), D3D11_BIND_CONSTANT_BUFFER);
    lights_ = dynamic_buffer(device, static_cast<UINT>(mod_light_slots * 16U),
                             D3D11_BIND_CONSTANT_BUFFER);
    ready_ = constants_ != nullptr && lights_ != nullptr;
}

void ModLightGpu::submit(const modding::graphics::LightFrame &frame)
{
    frame_.lights = frame.lights;
    frame_.considered = frame.considered;
    frame_.culled = frame.culled;
    submitted_ = true;
}

bool ModLightGpu::reserve_clusters(std::size_t clusters)
{
    using modding::graphics::ClusterGrid;
    if (clusters <= cluster_capacity_ && counts_ != nullptr)
        return true;
    // A little room, so a window resized by a few pixels keeps its buffers.
    const std::size_t capacity = clusters + clusters / 4U;
    const auto entries = static_cast<UINT>(capacity);
    const auto slots = static_cast<UINT>(capacity * ClusterGrid::max_per_cluster);
    counts_ = gpu_buffer(device_.Get(), entries * 4U);
    indices_ = gpu_buffer(device_.Get(), slots * 2U);
    count_view_ = typed_view(device_.Get(), counts_.Get(), DXGI_FORMAT_R32_UINT, entries);
    // The pixel shaders read four indices at a time; the cluster build writes one.
    index_view_ = typed_view(device_.Get(), indices_.Get(), DXGI_FORMAT_R16G16B16A16_UINT,
                             slots / 4U);
    count_writer_ = typed_writer(device_.Get(), counts_.Get(), DXGI_FORMAT_R32_UINT, entries);
    index_writer_ = typed_writer(device_.Get(), indices_.Get(), DXGI_FORMAT_R16_UINT, slots);
    if (count_view_ == nullptr || index_view_ == nullptr || count_writer_ == nullptr ||
        index_writer_ == nullptr)
    {
        std::cerr << "Mod lights: the cluster buffers could not be created\n";
        counts_.Reset();
        indices_.Reset();
        cluster_capacity_ = 0U;
        return false;
    }
    cluster_capacity_ = capacity;
    return true;
}

ID3D11ComputeShader *ModLightGpu::cluster_shader()
{
    if (cluster_shader_ != nullptr || cluster_attempted_)
        return cluster_shader_.Get();
    cluster_attempted_ = true;
    const std::string_view text = mod_light_cluster_hlsl();
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(compile_hlsl_cached(text.data(), text.size(), nullptr, nullptr, nullptr, "main",
                                   "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0U, &blob, &errors)) ||
        blob == nullptr ||
        FAILED(device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                            nullptr, &cluster_shader_)))
    {
        std::cerr << "Mod lights: the cluster shader failed"
                  << (errors != nullptr
                          ? ": " +
                                std::string{static_cast<const char *>(errors->GetBufferPointer()),
                                            errors->GetBufferSize()}
                          : std::string{})
                  << '\n';
        cluster_shader_.Reset();
    }
    return cluster_shader_.Get();
}

bool ModLightGpu::begin_frame(const modding::graphics::ClusterView &view)
{
    using modding::graphics::ClusterGrid;
    if (!ready_)
        return false;
    // Nothing of the last frame may stay bound while the lists are rewritten.
    unbind();
    if (!submitted_ || frame_.lights.empty())
        return false;
    // A light index past the buffer would read garbage: the CPU keeps at most
    // mod_light_capacity lights, and the lists only name lights it has.
    if (frame_.lights.size() > mod_light_capacity)
        frame_.lights.resize(mod_light_capacity);
    grid_ = modding::graphics::make_cluster_grid(view, frame_.lights);
    ID3D11ComputeShader *builder = cluster_shader();
    if (builder == nullptr || !reserve_clusters(grid_.cluster_count()))
        return false;
    // The shaders test reach as distance squared times one over the radius
    // squared, which they find in the radius slot. After the lights, one
    // that reaches nothing (the padding of the cluster lists).
    packed_ = frame_.lights;
    for (modding::graphics::GpuLight &light : packed_)
        light.radius = 1.0F / std::max(light.radius * light.radius, 1.0e-4F);
    modding::graphics::GpuLight nothing;
    nothing.radius = 1.0e30F;
    packed_.push_back(nothing);
    static_assert(ClusterGrid::slices == 24 && ClusterGrid::max_per_cluster == 64U,
                  "mod_light_hlsl and mod_light_cluster_hlsl hard-code the grid's slices and "
                  "slots");
    const float log2_growth = ClusterGrid::log2_growth();
    ClusterConstants constants;
    constants.eye[0] = view.eye.x;
    constants.eye[1] = view.eye.y;
    constants.eye[2] = view.eye.z;
    constants.eye[3] = 1.0F;
    const auto axis = [](float (&out)[4], const modding::graphics::Vec3 &value, float extra) {
        out[0] = value.x;
        out[1] = value.y;
        out[2] = value.z;
        out[3] = extra;
    };
    axis(constants.right, view.right, static_cast<float>(grid_.tiles_x));
    axis(constants.up, view.up, static_cast<float>(grid_.tiles_y));
    axis(constants.forward, view.forward, static_cast<float>(frame_.lights.size()));
    constants.projection[0] = view.scale_x;
    constants.projection[1] = view.scale_y;
    constants.projection[2] = ClusterGrid::first_depth;
    constants.projection[3] = 1.0F / log2_growth;
    constants.range[0] = grid_.far_depth;
    constants.range[1] = ClusterGrid::ndc_margin;
    constants.range[2] = log2_growth;
    if (!upload(context_.Get(), constants_.Get(), &constants, 1U) ||
        !upload(context_.Get(), lights_.Get(), packed_.data(), packed_.size()))
        return false;

    // The lists, one thread per cluster.
    ID3D11Buffer *const buffers[2]{constants_.Get(), lights_.Get()};
    ID3D11UnorderedAccessView *const writers[2]{count_writer_.Get(), index_writer_.Get()};
    context_->CSSetShader(builder, nullptr, 0U);
    context_->CSSetConstantBuffers(0U, 2U, buffers);
    context_->CSSetUnorderedAccessViews(0U, 2U, writers, nullptr);
    context_->Dispatch(
        static_cast<UINT>((grid_.cluster_count() + cluster_group_size - 1U) / cluster_group_size),
        1U, 1U);
    ID3D11UnorderedAccessView *const no_writers[2]{};
    ID3D11Buffer *const no_buffers[2]{};
    context_->CSSetUnorderedAccessViews(0U, 2U, no_writers, nullptr);
    context_->CSSetConstantBuffers(0U, 2U, no_buffers);
    context_->CSSetShader(nullptr, nullptr, 0U);
    built_ = true;

    static_assert(mod_light_data_slot == mod_light_constant_slot + 1U,
                  "the grid and the lights bind as one pair");
    context_->PSSetConstantBuffers(mod_light_constant_slot, 2U, buffers);
    ID3D11ShaderResourceView *const views[2]{count_view_.Get(), index_view_.Get()};
    context_->PSSetShaderResources(mod_light_buffer_slot, 2U, views);
    bound_ = true;
    return true;
}

void ModLightGpu::unbind()
{
    if (!bound_)
        return;
    ID3D11Buffer *const none[2]{};
    context_->PSSetConstantBuffers(mod_light_constant_slot, 2U, none);
    ID3D11ShaderResourceView *const views[2]{};
    context_->PSSetShaderResources(mod_light_buffer_slot, 2U, views);
    bound_ = false;
}

modding::graphics::LightClusterStats ModLightGpu::statistics()
{
    if (!built_ || counts_ == nullptr)
        return {};
    const auto bytes = static_cast<UINT>(grid_.cluster_count() * 4U);
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    if (FAILED(device_->CreateBuffer(&description, nullptr, &staging)))
        return {};
    const D3D11_BOX range{0U, 0U, 0U, bytes, 1U, 1U};
    context_->CopySubresourceRegion(staging.Get(), 0U, 0U, 0U, 0U, counts_.Get(), 0U, &range);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)))
        return {};
    const modding::graphics::LightClusterStats stats = modding::graphics::cluster_stats(
        grid_, {static_cast<const std::uint32_t *>(mapped.pData), grid_.cluster_count()});
    context_->Unmap(staging.Get(), 0U);
    return stats;
}

ID3D11PixelShader *ModLightGpu::variant(Variant which, const std::string &source)
{
    const auto index = static_cast<std::size_t>(which);
    if (index >= variants_.size())
        return nullptr;
    if (variants_[index] != nullptr || attempted_[index] || source.empty())
        return variants_[index].Get();
    attempted_[index] = true;
    const auto started = std::chrono::steady_clock::now();
    const std::string text =
        std::string{"#define OPENMINERZ_MOD_LIGHTS 1\n"} + mod_light_hlsl() + source;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(compile_hlsl_cached(text.data(), text.size(), nullptr, nullptr, nullptr, "main",
                                   "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0U, &blob, &errors)) ||
        blob == nullptr ||
        FAILED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                          &variants_[index])))
    {
        std::cerr << "Mod lights: shader variant " << index << " failed"
                  << (errors != nullptr
                          ? ": " +
                                std::string{static_cast<const char *>(errors->GetBufferPointer()),
                                            errors->GetBufferSize()}
                          : std::string{})
                  << '\n';
        variants_[index].Reset();
        return nullptr;
    }
    const auto milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (milliseconds > 50.0)
        std::cerr << "Mod lights: compiled shader variant " << index << " in "
                  << static_cast<int>(milliseconds) << " ms\n";
    return variants_[index].Get();
}

#endif

} // namespace openminerz
