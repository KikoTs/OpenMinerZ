#include "present_control.hpp"

#include <array>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace openminerz
{

struct PresentControl::Impl final
{
    SDL_Renderer *renderer{};
    std::vector<double> latency;
    std::uint64_t missed{};
    double refresh_milliseconds{};

#if defined(_WIN32)
    struct Pending final
    {
        UINT present_count{};
        std::uint64_t input_counter{};
        bool valid{};
    };
    std::array<Pending, 64> pending{};
    bool have_statistics{};
    DXGI_FRAME_STATISTICS previous{};
    double refresh_ticks{};
    double counter_frequency{};

    // SDL owns both objects and may rebuild them (device loss, mode switch),
    // so they are looked up per call instead of being cached.
    [[nodiscard]] ID3D11Device *device() const
    {
        return static_cast<ID3D11Device *>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    }

    [[nodiscard]] IDXGISwapChain1 *swap_chain() const
    {
        return static_cast<IDXGISwapChain1 *>(
            SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                                   SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr));
    }
#endif
};

PresentControl::PresentControl(SDL_Renderer *renderer) : impl_(std::make_unique<Impl>())
{
    impl_->renderer = renderer;
#if defined(_WIN32)
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    impl_->counter_frequency = static_cast<double>(frequency.QuadPart);
#endif
}

PresentControl::~PresentControl() = default;

PresentQueueInfo PresentControl::info() const
{
    PresentQueueInfo result;
#if defined(_WIN32)
    ID3D11Device *device = impl_->device();
    IDXGISwapChain1 *swap_chain = impl_->swap_chain();
    if (device == nullptr || swap_chain == nullptr)
        return result;
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
    UINT latency{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetMaximumFrameLatency(&latency)))
        return result;
    DXGI_SWAP_CHAIN_DESC1 description{};
    if (FAILED(swap_chain->GetDesc1(&description)))
        return result;
    result.available = true;
    result.maximum_frame_latency = latency;
    result.buffer_count = description.BufferCount;
    result.swap_effect = static_cast<unsigned>(description.SwapEffect);
    result.flags = description.Flags;
    result.flip_model = description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                        description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; spelled numerically because the
    // enumerator only exists in newer SDK headers than this file requires.
    result.tearing_allowed = (description.Flags & 2048U) != 0U;
    result.width = description.Width;
    result.height = description.Height;
#endif
    return result;
}

bool PresentControl::set_maximum_frame_latency(unsigned frames)
{
#if defined(_WIN32)
    ID3D11Device *device = impl_->device();
    if (device == nullptr)
        return false;
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))))
        return false;
    return SUCCEEDED(dxgi_device->SetMaximumFrameLatency(frames));
#else
    (void)frames;
    return false;
#endif
}

void PresentControl::note_present(std::uint64_t input_counter)
{
#if defined(_WIN32)
    IDXGISwapChain1 *swap_chain = impl_->swap_chain();
    if (swap_chain == nullptr)
        return;
    UINT last_present{};
    if (FAILED(swap_chain->GetLastPresentCount(&last_present)))
        return;
    impl_->pending[last_present % impl_->pending.size()] = {last_present, input_counter, true};

    DXGI_FRAME_STATISTICS statistics{};
    if (FAILED(swap_chain->GetFrameStatistics(&statistics)))
    {
        // Disjoint statistics (mode change, occlusion): start a fresh series.
        impl_->have_statistics = false;
        return;
    }
    if (statistics.SyncRefreshCount == 0U && statistics.SyncQPCTime.QuadPart == 0)
        return;
    if (!impl_->have_statistics)
    {
        impl_->have_statistics = true;
        impl_->previous = statistics;
        return;
    }
    const DXGI_FRAME_STATISTICS &previous = impl_->previous;
    if (statistics.SyncRefreshCount > previous.SyncRefreshCount)
    {
        const double ticks =
            static_cast<double>(statistics.SyncQPCTime.QuadPart - previous.SyncQPCTime.QuadPart) /
            static_cast<double>(statistics.SyncRefreshCount - previous.SyncRefreshCount);
        if (ticks > 0.0)
        {
            impl_->refresh_ticks =
                impl_->refresh_ticks > 0.0 ? impl_->refresh_ticks * 0.9 + ticks * 0.1 : ticks;
            impl_->refresh_milliseconds = impl_->refresh_ticks * 1000.0 / impl_->counter_frequency;
        }
    }
    if (statistics.PresentCount != previous.PresentCount)
    {
        const UINT presents = statistics.PresentCount - previous.PresentCount;
        const UINT refreshes = statistics.PresentRefreshCount - previous.PresentRefreshCount;
        if (refreshes > presents && refreshes < 1000U)
            impl_->missed += refreshes - presents;
        Impl::Pending &entry = impl_->pending[statistics.PresentCount % impl_->pending.size()];
        if (entry.valid && entry.present_count == statistics.PresentCount &&
            impl_->refresh_ticks > 0.0)
        {
            // SyncQPCTime belongs to SyncRefreshCount; step back to the refresh
            // in which this present actually reached the screen.
            const double displayed =
                static_cast<double>(statistics.SyncQPCTime.QuadPart) -
                static_cast<double>(statistics.SyncRefreshCount - statistics.PresentRefreshCount) *
                    impl_->refresh_ticks;
            const double milliseconds = (displayed - static_cast<double>(entry.input_counter)) *
                                        1000.0 / impl_->counter_frequency;
            if (milliseconds > 0.0 && milliseconds < 500.0)
                impl_->latency.push_back(milliseconds);
            entry.valid = false;
        }
    }
    impl_->previous = statistics;
#else
    (void)input_counter;
#endif
}

const std::vector<double> &PresentControl::latency_milliseconds() const noexcept
{
    return impl_->latency;
}

std::uint64_t PresentControl::missed_refreshes() const noexcept
{
    return impl_->missed;
}

double PresentControl::refresh_interval_milliseconds() const noexcept
{
    return impl_->refresh_milliseconds;
}

} // namespace openminerz
