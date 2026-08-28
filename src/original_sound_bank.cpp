#include "original_sound_bank.hpp"

#include "openminerz/asset_paths.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#endif

namespace openminerz
{
namespace
{
#if defined(_WIN32)
const PROPERTYKEY device_friendly_name_key{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string utf8_from_wide(const wchar_t *value)
{
    if (value == nullptr)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr) <= 0)
        return {};
    result.pop_back();
    return result;
}

std::wstring wide_from_utf8(std::string_view value)
{
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size) <= 0)
        return {};
    return result;
}
#endif
} // namespace

std::vector<AudioOutputDevice> enumerate_audio_output_devices()
{
    std::vector<AudioOutputDevice> result{{{}, "System default"}};
#if defined(_WIN32)
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return result;

    IMMDeviceEnumerator *enumerator{};
    IMMDeviceCollection *collection{};
    const HRESULT created = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             IID_PPV_ARGS(&enumerator));
    HRESULT enumerated = created;
    if (SUCCEEDED(enumerated))
        enumerated = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    UINT count{};
    if (SUCCEEDED(enumerated))
        enumerated = collection->GetCount(&count);
    for (UINT index = 0U; SUCCEEDED(enumerated) && index < count; ++index)
    {
        IMMDevice *device{};
        IPropertyStore *properties{};
        LPWSTR id{};
        PROPVARIANT friendly_name{};
        PropVariantInit(&friendly_name);
        HRESULT item = collection->Item(index, &device);
        if (SUCCEEDED(item))
            item = device->GetId(&id);
        if (SUCCEEDED(item))
            item = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(item))
            item = properties->GetValue(device_friendly_name_key, &friendly_name);
        if (SUCCEEDED(item) && id != nullptr && friendly_name.vt == VT_LPWSTR &&
            friendly_name.pwszVal != nullptr)
        {
            AudioOutputDevice output{utf8_from_wide(id), utf8_from_wide(friendly_name.pwszVal)};
            if (!output.id.empty() && !output.name.empty())
            {
                result.push_back(std::move(output));
            }
        }
        PropVariantClear(&friendly_name);
        if (id != nullptr)
            CoTaskMemFree(id);
        if (properties != nullptr)
            properties->Release();
        if (device != nullptr)
            device->Release();
    }
    if (enumerator != nullptr)
    {
        IMMDevice *default_device{};
        LPWSTR default_id{};
        HRESULT selected =
            enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_device);
        if (SUCCEEDED(selected))
            selected = default_device->GetId(&default_id);
        if (SUCCEEDED(selected))
        {
            const std::string id_value = utf8_from_wide(default_id);
            const auto found = std::find_if(
                result.begin() + 1, result.end(),
                [&](const AudioOutputDevice &device) { return device.id == id_value; });
            if (found != result.end())
                result.front().name = std::format("System default ({})", found->name);
        }
        if (default_id != nullptr)
            CoTaskMemFree(default_id);
        if (default_device != nullptr)
            default_device->Release();
    }
    if (collection != nullptr)
        collection->Release();
    if (enumerator != nullptr)
        enumerator->Release();
    if (uninitialize)
        CoUninitialize();
#endif
    return result;
}

class OriginalSoundBank::Impl final
{
  public:
    explicit Impl(std::string_view output_id)
    {
        renderer_id_.assign(output_id.begin(), output_id.end());
#if defined(_WIN32)
        std::wstring module_path(32768U, L'\0');
        const DWORD length =
            GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
        if (length == 0U || length >= module_path.size())
        {
            return;
        }
        module_path.resize(length);
        const std::filesystem::path bridge = std::filesystem::path{module_path}.parent_path() /
                                             "AudioBridge" / "OpenMinerZ.AudioRuntime.exe";
        const std::filesystem::path game_dir = active_content_pack_path();
        if (!std::filesystem::exists(bridge) ||
            !std::filesystem::exists(game_dir / "Content" / "Sounds.xgs"))
        {
            return;
        }

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE child_input{};
        if (!CreatePipe(&child_input, &input_, &attributes, 0U))
        {
            return;
        }
        if (!SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0U))
        {
            CloseHandle(child_input);
            CloseHandle(input_);
            input_ = nullptr;
            return;
        }

        HANDLE child_output{};
        if (!CreatePipe(&output_, &child_output, &attributes, 0U) ||
            !SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0U))
        {
            CloseHandle(child_input);
            CloseHandle(input_);
            input_ = nullptr;
            if (child_output != nullptr)
                CloseHandle(child_output);
            if (output_ != nullptr)
                CloseHandle(output_);
            output_ = nullptr;
            return;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_input;
        startup.hStdOutput = child_output;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command = L"\"" + bridge.wstring() + L"\" \"" + game_dir.wstring() + L"\"";
        if (!renderer_id_.empty())
        {
            const std::wstring wide_id = wide_from_utf8(renderer_id_);
            if (!wide_id.empty())
                command += L" \"" + wide_id + L"\"";
        }
        PROCESS_INFORMATION process{};
        const BOOL launched =
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, game_dir.c_str(), &startup, &process);
        CloseHandle(child_input);
        CloseHandle(child_output);
        if (!launched)
        {
            CloseHandle(input_);
            input_ = nullptr;
            CloseHandle(output_);
            output_ = nullptr;
            return;
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);

        std::string response;
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            DWORD exit_code{};
            if (!GetExitCodeProcess(process_, &exit_code) || exit_code != STILL_ACTIVE)
                break;
            DWORD pending{};
            if (PeekNamedPipe(output_, nullptr, 0U, nullptr, &pending, nullptr) && pending > 0U)
            {
                char buffer[128]{};
                DWORD bytes_read{};
                if (ReadFile(output_, buffer,
                             std::min<DWORD>(pending, static_cast<DWORD>(sizeof(buffer))),
                             &bytes_read, nullptr))
                {
                    response.append(buffer, bytes_read);
                    const auto ready = response.find("READY\t");
                    const auto end =
                        ready == std::string::npos ? std::string::npos : response.find('\n', ready);
                    if (end != std::string::npos)
                    {
                        const auto id_end = response.find('\t', ready + 6U);
                        if (id_end != std::string::npos && id_end < end)
                        {
                            renderer_id_ = response.substr(ready + 6U, id_end - (ready + 6U));
                            renderer_name_ = response.substr(id_end + 1U, end - (id_end + 1U));
                            if (!renderer_name_.empty() && renderer_name_.back() == '\r')
                                renderer_name_.pop_back();
                        }
                        ready_ = true;
                        break;
                    }
                }
            }
            Sleep(10U);
        }
#endif
    }

    ~Impl()
    {
#if defined(_WIN32)
        play("quit");
        if (input_ != nullptr)
        {
            CloseHandle(input_);
        }
        if (output_ != nullptr)
        {
            CloseHandle(output_);
        }
        if (process_ != nullptr)
        {
            WaitForSingleObject(process_, 1500U);
            CloseHandle(process_);
        }
#endif
    }

    [[nodiscard]] bool available() const noexcept
    {
#if defined(_WIN32)
        if (!ready_ || input_ == nullptr || process_ == nullptr)
        {
            return false;
        }
        DWORD exit_code{};
        return GetExitCodeProcess(process_, &exit_code) && exit_code == STILL_ACTIVE;
#else
        return false;
#endif
    }

    [[nodiscard]] bool output_muted() const noexcept
    {
#if defined(_WIN32)
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            return false;

        IMMDeviceEnumerator *enumerator{};
        IMMDevice *device{};
        IAudioEndpointVolume *endpoint{};
        BOOL muted{};
        const HRESULT created = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                 IID_PPV_ARGS(&enumerator));
        HRESULT queried = created;
        if (SUCCEEDED(queried))
        {
            if (renderer_id_.empty())
                queried = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
            else
            {
                const std::wstring wide_id = wide_from_utf8(renderer_id_);
                queried = wide_id.empty() ? E_INVALIDARG
                                          : enumerator->GetDevice(wide_id.c_str(), &device);
            }
        }
        if (SUCCEEDED(queried))
            queried = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void **>(&endpoint));
        if (SUCCEEDED(queried))
            queried = endpoint->GetMute(&muted);
        if (endpoint != nullptr)
            endpoint->Release();
        if (device != nullptr)
            device->Release();
        if (enumerator != nullptr)
            enumerator->Release();
        if (uninitialize)
            CoUninitialize();
        return SUCCEEDED(queried) && muted != FALSE;
#else
        return false;
#endif
    }

    [[nodiscard]] std::string_view output_name() const noexcept
    {
        return renderer_name_;
    }

    [[nodiscard]] float output_volume() const noexcept
    {
#if defined(_WIN32)
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            return -1.0F;

        IMMDeviceEnumerator *enumerator{};
        IMMDevice *device{};
        IAudioEndpointVolume *endpoint{};
        float volume{-1.0F};
        const HRESULT created = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                 IID_PPV_ARGS(&enumerator));
        HRESULT queried = created;
        if (SUCCEEDED(queried))
        {
            if (renderer_id_.empty())
                queried = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
            else
            {
                const std::wstring wide_id = wide_from_utf8(renderer_id_);
                queried = wide_id.empty() ? E_INVALIDARG
                                          : enumerator->GetDevice(wide_id.c_str(), &device);
            }
        }
        if (SUCCEEDED(queried))
            queried = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void **>(&endpoint));
        if (SUCCEEDED(queried))
            queried = endpoint->GetMasterVolumeLevelScalar(&volume);
        if (endpoint != nullptr)
            endpoint->Release();
        if (device != nullptr)
            device->Release();
        if (enumerator != nullptr)
            enumerator->Release();
        if (uninitialize)
            CoUninitialize();
        return SUCCEEDED(queried) ? std::clamp(volume, 0.0F, 1.0F) : -1.0F;
#else
        return -1.0F;
#endif
    }

    void play(std::string_view cue)
    {
#if defined(_WIN32)
        if (!available())
        {
            return;
        }
        std::string command{cue};
        command.push_back('\n');
        DWORD written{};
        if (!WriteFile(input_, command.data(), static_cast<DWORD>(command.size()), &written,
                       nullptr))
        {
            CloseHandle(input_);
            input_ = nullptr;
        }
#else
        (void)cue;
#endif
    }

  private:
#if defined(_WIN32)
    HANDLE input_{};
    HANDLE output_{};
    HANDLE process_{};
    bool ready_{};
#endif
    std::string renderer_id_;
    std::string renderer_name_;
};

OriginalSoundBank::OriginalSoundBank(std::string_view output_id)
    : impl_(std::make_unique<Impl>(output_id))
{
}

OriginalSoundBank::~OriginalSoundBank() = default;

bool OriginalSoundBank::available() const noexcept
{
    return impl_->available();
}

std::string_view OriginalSoundBank::output_name() const noexcept
{
    return impl_->output_name();
}

bool OriginalSoundBank::output_muted() const noexcept
{
    return impl_->output_muted();
}

float OriginalSoundBank::output_volume() const noexcept
{
    return impl_->output_volume();
}

void OriginalSoundBank::set_levels(float master, float music, float effects, float ambience,
                                   bool muted)
{
    const float output = muted ? 0.0F : std::clamp(master, 0.0F, 1.0F);
    impl_->play(std::format("volume {:.3f} {:.3f} {:.3f}", output * std::clamp(effects, 0.0F, 1.0F),
                            output * std::clamp(music, 0.0F, 1.0F),
                            output * std::clamp(ambience, 0.0F, 1.0F)));
}

void OriginalSoundBank::play_menu_music()
{
    impl_->play("music start");
}

void OriginalSoundBank::stop_music()
{
    impl_->play("music stop");
}

void OriginalSoundBank::play_break(world::BlockType block)
{
    switch (block)
    {
    case world::BlockType::sand:
    case world::BlockType::snow:
        impl_->play("Sand");
        break;
    case world::BlockType::leaves:
        impl_->play("leaves");
        break;
    default:
        impl_->play("punch");
        break;
    }
}

void OriginalSoundBank::play_place()
{
    impl_->play("Place");
}

void OriginalSoundBank::play_footstep()
{
    impl_->play("FootStep");
}

void OriginalSoundBank::play_pickup()
{
    impl_->play("pickupitem");
}

void OriginalSoundBank::play_click()
{
    impl_->play("Click");
}

void OriginalSoundBank::play_drop()
{
    impl_->play("dropitem");
}

void OriginalSoundBank::play_pistol()
{
    impl_->play("GunShot4");
}

void OriginalSoundBank::play_reload()
{
    impl_->play("Reload");
}

void OriginalSoundBank::play_cue(std::string_view cue)
{
    if (!cue.empty())
        impl_->play(cue);
}

void OriginalSoundBank::start_looping_cue(std::string_view cue)
{
    if (!cue.empty())
        impl_->play(std::format("loop start {}", cue));
}

void OriginalSoundBank::stop_looping_cue(std::string_view cue)
{
    if (!cue.empty())
        impl_->play(std::format("loop stop {}", cue));
}

void OriginalSoundBank::play_song(std::string_view name)
{
    if (!name.empty())
        impl_->play(std::format("song {}", name));
}

void OriginalSoundBank::update_listener(world::Vec3 position, float yaw)
{
    // Player.UpdateAudio: the ear sits 1.8 above the feet.
    impl_->play(std::format("listener {:.2f} {:.2f} {:.2f} {:.3f} {:.3f} {:.3f}", position.x,
                            position.y + 1.8F, position.z, std::sin(yaw), 0.0F, -std::cos(yaw)));
}

void OriginalSoundBank::play_cue_at(std::string_view cue, world::Vec3 position)
{
    if (!cue.empty())
        impl_->play(
            std::format("cue3d {} {:.2f} {:.2f} {:.2f}", cue, position.x, position.y, position.z));
}

void OriginalSoundBank::begin_enemy_voice_frame()
{
    impl_->play("voices begin");
}

void OriginalSoundBank::sync_enemy_voice(std::uint32_t id, std::string_view cue,
                                         world::Vec3 position)
{
    if (!cue.empty())
        impl_->play(std::format("voice {} {} {:.2f} {:.2f} {:.2f}", id, cue, position.x, position.y,
                                position.z));
}

void OriginalSoundBank::end_enemy_voice_frame()
{
    impl_->play("voices end");
}

void OriginalSoundBank::clear_enemy_voices()
{
    impl_->play("voices clear");
}

void OriginalSoundBank::sync_fireball_cue(std::uint32_t id, std::string_view cue,
                                          world::Vec3 position, world::Vec3 velocity)
{
    if (!cue.empty())
        impl_->play(std::format("fireball {} {} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}", id, cue,
                                position.x, position.y, position.z, velocity.x, velocity.y,
                                velocity.z));
}

void OriginalSoundBank::stop_fireball_cue(std::uint32_t id)
{
    impl_->play(std::format("fireball stop {}", id));
}

void OriginalSoundBank::clear_fireball_cues()
{
    impl_->play("fireballs clear");
}

void OriginalSoundBank::set_ambience(float day, float night, float cave, float hell, float outdoors)
{
    impl_->play(std::format("ambient {:.3f} {:.3f} {:.3f} {:.3f}", day, night, cave, hell));
    impl_->play(std::format("outdoors {:.3f}", outdoors));
}

} // namespace openminerz
