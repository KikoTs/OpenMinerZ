#pragma once

// The GPU side of mod lighting props (openminerz/modding/graphics.hpp) in the
// depth renderer's lit passes.
//
// PRESENTATION ONLY, and only while a mod has lights. Each frame's lights go
// into a constant buffer, a compute shader lists them per view-space cluster
// (modding::graphics::ClusterGrid: screen tiles times depth slices) into two
// typed buffers, and one more constant buffer describes the grid; the terrain
// and creature pixel shaders are used in a variant compiled with
// OPENMINERZ_MOD_LIGHTS, which reads them. A frame without mod lights
// dispatches nothing, binds none of this and draws with the shaders exactly as
// they are, so an unmodded game is untouched pixel for pixel.
//
// Per pixel the variant finds its cluster from its world position and loops
// over the lights listed there, four at a time, so its cost follows how many
// lights overlap a pixel, not how many exist, and neighbouring pixels read the
// same list. The compute shader and the variants are compiled the first time a
// frame has mod lights (once per process: shader_cache.hpp).

#include "openminerz/modding/graphics.hpp"

#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace openminerz
{

// b10 the cluster grid, b11 the lights (three float4 each, then one that
// reaches nothing, which pads the index lists to whole groups of four).
inline constexpr unsigned mod_light_constant_slot = 10U;
inline constexpr unsigned mod_light_data_slot = 11U;
// t26 per-cluster counts, t27 light indices.
inline constexpr unsigned mod_light_buffer_slot = 26U;
// The most lights the buffers hold; the CPU keeps the most important.
inline constexpr std::size_t mod_light_capacity = 512U;
inline constexpr std::size_t mod_light_slots = (mod_light_capacity + 1U) * 3U;

// The HLSL the variants prepend: mod_point_lights(position, normal, wrap).
[[nodiscard]] const char *mod_light_hlsl() noexcept;
// The compute shader that fills the cluster lists.
[[nodiscard]] const char *mod_light_cluster_hlsl() noexcept;

#if defined(_WIN32)

class ModLightGpu final
{
  public:
    ModLightGpu(ID3D11Device *device, ID3D11DeviceContext *context);

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    // The frame's lights (copied).
    void submit(const modding::graphics::LightFrame &frame);
    // Uploads the lights, builds their cluster lists for `view` and binds
    // them for the lit passes. True when this frame has lights and the
    // variant shaders below should be used.
    [[nodiscard]] bool begin_frame(const modding::graphics::ClusterView &view);
    void unbind();
    // What the last built lists held. Reads them back from the GPU (a stall):
    // diagnostics only.
    [[nodiscard]] modding::graphics::LightClusterStats statistics();

    // Variants of the lit pixel shaders, compiled from `source` (the exact
    // text of the base shader) on first request; nullptr when that failed,
    // and the frame then draws without mod lights.
    enum class Variant : std::uint8_t
    {
        terrain,
        entity,
        entity_alpha_lit,
        count
    };
    [[nodiscard]] ID3D11PixelShader *variant(Variant which, const std::string &source);
    [[nodiscard]] const modding::graphics::LightFrame &frame() const noexcept
    {
        return frame_;
    }

  private:
    [[nodiscard]] bool reserve_clusters(std::size_t clusters);
    [[nodiscard]] ID3D11ComputeShader *cluster_shader();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lights_;
    // The cluster lists, sized for the largest grid so far.
    Microsoft::WRL::ComPtr<ID3D11Buffer> counts_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indices_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> count_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> index_view_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> count_writer_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> index_writer_;
    std::size_t cluster_capacity_{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cluster_shader_;
    bool cluster_attempted_{};
    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, static_cast<std::size_t>(Variant::count)>
        variants_{};
    std::array<bool, static_cast<std::size_t>(Variant::count)> attempted_{};
    modding::graphics::LightFrame frame_;
    // frame_.lights as uploaded (see begin_frame).
    std::vector<modding::graphics::GpuLight> packed_;
    // The grid of the last built lists.
    modding::graphics::ClusterGrid grid_;
    bool built_{};
    bool submitted_{};
    bool bound_{};
    bool ready_{};
};

#endif

} // namespace openminerz
