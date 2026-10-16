#include "mod_output.hpp"

#include "shader_cache.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace openminerz
{

// --- Audio -----------------------------------------------------------------

ModAudioDevice::ModAudioDevice()
{
    constexpr int rate = 48000;
    mixer_ = std::make_unique<modding::graphics::Mixer>(rate);
    initialized_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!initialized_)
    {
        error_ = SDL_GetError();
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &feed, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_))
    {
        error_ = SDL_GetError();
        if (stream_ != nullptr)
            SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

ModAudioDevice::~ModAudioDevice()
{
    if (stream_ != nullptr)
        SDL_DestroyAudioStream(stream_);
    // The audio thread has stopped: what was recorded so far can be written.
    finish_dump();
    if (initialized_)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void ModAudioDevice::start_dump(std::filesystem::path wav, double seconds)
{
    if (dump_ != nullptr || stream_ == nullptr)
        return;
    dump_ = std::make_unique<modding::graphics::MixRecorder>(mixer_->output_rate(), seconds);
    dump_path_ = std::move(wav);
    mixer_->set_recorder(dump_.get());
    mixer_->mark("dump started");
    std::cerr << "Mod audio dump: recording " << seconds << " s to " << dump_path_.string()
              << '\n';
}

void ModAudioDevice::poll_dump()
{
    if (dump_ != nullptr && dump_->full())
        finish_dump();
}

void ModAudioDevice::finish_dump()
{
    if (dump_ == nullptr)
        return;
    mixer_->set_recorder(nullptr);
    std::string error;
    if (dump_->write(dump_path_, error))
        std::cerr << "Mod audio dump: wrote " << dump_->frames() << " frames and "
                  << dump_->marks().size() << " marks to " << dump_path_.string() << '\n';
    else
        std::cerr << "Mod audio dump failed: " << error << '\n';
    dump_.reset();
}

void SDLCALL ModAudioDevice::feed(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)total;
    auto *device = static_cast<ModAudioDevice *>(user);
    if (device == nullptr || additional <= 0)
        return;
    const auto frames = static_cast<std::size_t>(additional) / (2U * sizeof(float));
    if (frames == 0U)
        return;
    device->scratch_.resize(frames * 2U);
    device->mixer_->mix(device->scratch_.data(), frames);
    SDL_PutAudioStreamData(stream, device->scratch_.data(),
                           static_cast<int>(frames * 2U * sizeof(float)));
}

// --- Screen pass -------------------------------------------------------------

#if defined(_WIN32)

class ModScreenPass::Impl final
{
  public:
    explicit Impl(SDL_Renderer *renderer)
    {
        auto *device = static_cast<ID3D11Device *>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
        if (device == nullptr)
            return;
        device_ = device;
        ID3D11DeviceContext *context{};
        device_->GetImmediateContext(&context);
        context_.Attach(context);
        static constexpr char vertex_source[] = R"hlsl(
struct Output
{
    float4 position : SV_POSITION;
};
Output main(uint id : SV_VertexID)
{
    Output output;
    float2 corner = float2((id << 1) & 2, id & 2);
    output.position = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)hlsl";
        // The LUT strip: N tiles of N x N side by side, red across a tile,
        // green down, blue from tile to tile (modding::graphics::sample_lut).
        static constexpr char pixel_source[] = R"hlsl(
Texture2D Scene : register(t0);
Texture2D Lut : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer Grade : register(b0)
{
    float4 Params; // x strength, y LUT size
};
float3 lut_at(float3 color)
{
    float size = Params.y;
    color = saturate(color);
    float blue = color.b * (size - 1.0f);
    float first = floor(blue);
    float second = min(first + 1.0f, size - 1.0f);
    float2 inner = (color.rg * (size - 1.0f) + 0.5f) / float2(size * size, size);
    float3 a = Lut.SampleLevel(LinearClamp, inner + float2(first / size, 0.0f), 0).rgb;
    float3 b = Lut.SampleLevel(LinearClamp, inner + float2(second / size, 0.0f), 0).rgb;
    return lerp(a, b, blue - first);
}
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    float4 scene = Scene.Load(int3(position.xy, 0));
    scene.rgb = lerp(scene.rgb, lut_at(scene.rgb), Params.x);
    return scene;
}
)hlsl";
        Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
        Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (FAILED(compile_hlsl_cached(vertex_source, sizeof(vertex_source) - 1U, nullptr, nullptr,
                                       nullptr, "main", "vs_4_0", 0U, 0U, &vertex_blob, &errors)) ||
            FAILED(compile_hlsl_cached(pixel_source, sizeof(pixel_source) - 1U, nullptr, nullptr,
                                       nullptr, "main", "ps_4_0", 0U, 0U, &pixel_blob, &errors)) ||
            FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr, &vertex_)) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr, &pixel_)))
        {
            std::cerr << "Mod colour grading unavailable\n";
            return;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16U;
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ready_ = SUCCEEDED(device_->CreateSamplerState(&sampler, &sampler_)) &&
                 SUCCEEDED(device_->CreateBuffer(&constants, nullptr, &constants_)) &&
                 SUCCEEDED(device_->CreateRasterizerState(&raster, &raster_)) &&
                 SUCCEEDED(device_->CreateDepthStencilState(&depth, &depth_)) &&
                 SUCCEEDED(device_->CreateBlendState(&blend, &blend_));
    }

    // Grades `texture` (the finished world image) in place: copy it, draw the
    // LUT from the copy into a scratch target, copy that back.
    void grade(ID3D11Texture2D *texture, const modding::graphics::Image &lut, std::string_view name,
               float strength)
    {
        if (!ready_ || texture == nullptr)
            return;
        if (lut_name_ != name || lut_view_ == nullptr)
        {
            lut_view_.Reset();
            lut_name_ = name;
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(lut.width);
            description.Height = static_cast<UINT>(lut.height);
            description.MipLevels = 1U;
            description.ArraySize = 1U;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1U;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{lut.rgba.data(), static_cast<UINT>(lut.width) * 4U,
                                              0U};
            Microsoft::WRL::ComPtr<ID3D11Texture2D> lut_texture;
            if (FAILED(device_->CreateTexture2D(&description, &data, &lut_texture)) ||
                FAILED(device_->CreateShaderResourceView(lut_texture.Get(), nullptr, &lut_view_)))
                return;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.SampleDesc.Count != 1U || description.MipLevels != 1U ||
            description.ArraySize != 1U)
            return;
        if (copy_ == nullptr || copy_width_ != description.Width ||
            copy_height_ != description.Height || copy_format_ != description.Format)
        {
            copy_.Reset();
            copy_view_.Reset();
            scratch_.Reset();
            scratch_target_.Reset();
            D3D11_TEXTURE2D_DESC copy = description;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = 0U;
            copy.MiscFlags = 0U;
            D3D11_TEXTURE2D_DESC scratch = copy;
            scratch.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&copy, nullptr, &copy_)) ||
                FAILED(device_->CreateShaderResourceView(copy_.Get(), nullptr, &copy_view_)) ||
                FAILED(device_->CreateTexture2D(&scratch, nullptr, &scratch_)) ||
                FAILED(device_->CreateRenderTargetView(scratch_.Get(), nullptr, &scratch_target_)))
            {
                copy_.Reset();
                copy_view_.Reset();
                scratch_.Reset();
                scratch_target_.Reset();
                return;
            }
            copy_width_ = description.Width;
            copy_height_ = description.Height;
            copy_format_ = description.Format;
        }
        context_->CopyResource(copy_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(constants_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped)))
            return;
        const float params[4]{std::clamp(strength, 0.0F, 1.0F), static_cast<float>(lut.height),
                              0.0F, 0.0F};
        std::memcpy(mapped.pData, params, sizeof(params));
        context_->Unmap(constants_.Get(), 0U);
        const D3D11_VIEWPORT viewport{0.0F,
                                      0.0F,
                                      static_cast<float>(description.Width),
                                      static_cast<float>(description.Height),
                                      0.0F,
                                      1.0F};
        context_->RSSetViewports(1U, &viewport);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0U);
        context_->GSSetShader(nullptr, nullptr, 0U);
        context_->PSSetShader(pixel_.Get(), nullptr, 0U);
        context_->RSSetState(raster_.Get());
        context_->OMSetDepthStencilState(depth_.Get(), 0U);
        context_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFU);
        ID3D11RenderTargetView *const targets[1]{scratch_target_.Get()};
        context_->OMSetRenderTargets(1U, targets, nullptr);
        ID3D11ShaderResourceView *const views[2]{copy_view_.Get(), lut_view_.Get()};
        context_->PSSetShaderResources(0U, 2U, views);
        ID3D11SamplerState *const samplers[1]{sampler_.Get()};
        context_->PSSetSamplers(0U, 1U, samplers);
        ID3D11Buffer *const buffers[1]{constants_.Get()};
        context_->PSSetConstantBuffers(0U, 1U, buffers);
        context_->Draw(3U, 0U);
        ID3D11ShaderResourceView *const none[2]{};
        context_->PSSetShaderResources(0U, 2U, none);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
        context_->CopyResource(texture, scratch_.Get());
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scratch_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> scratch_target_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lut_view_;
    std::string lut_name_;
    UINT copy_width_{};
    UINT copy_height_{};
    DXGI_FORMAT copy_format_{};
    bool ready_{};
};

#else

class ModScreenPass::Impl final
{
  public:
    explicit Impl(SDL_Renderer *)
    {
    }
    void grade(ID3D11Texture2D *, const modding::graphics::Image &, std::string_view, float)
    {
    }
};

#endif

ModScreenPass::ModScreenPass(SDL_Renderer *renderer) : renderer_(renderer)
{
}

ModScreenPass::~ModScreenPass() = default;

void ModScreenPass::grade(ID3D11Texture2D *texture, const modding::graphics::Image *lut,
                          std::string_view lut_name, float strength)
{
    if (texture == nullptr || lut == nullptr || strength <= 0.001F ||
        !modding::graphics::valid_lut(*lut))
        return;
    if (impl_ == nullptr)
        impl_ = std::make_unique<Impl>(renderer_);
    impl_->grade(texture, *lut, lut_name, strength);
}

void ModScreenPass::fade(modding::graphics::Color color, float alpha, int width, int height)
{
    if (alpha <= 0.001F)
        return;
    SDL_BlendMode previous{};
    SDL_GetRenderDrawBlendMode(renderer_, &previous);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(renderer_, std::clamp(color.r, 0.0F, 1.0F),
                                std::clamp(color.g, 0.0F, 1.0F), std::clamp(color.b, 0.0F, 1.0F),
                                std::clamp(alpha, 0.0F, 1.0F));
    const SDL_FRect rect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    SDL_RenderFillRect(renderer_, &rect);
    SDL_SetRenderDrawBlendMode(renderer_, previous);
}

} // namespace openminerz
