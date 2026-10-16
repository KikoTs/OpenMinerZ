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
        // Commands are written from the frame thread, so the write must never
        // wait for the helper (see play). If this fails the pipe stays as it
        // was: blocking, and correct for a helper that keeps reading.
        DWORD no_wait = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        static_cast<void>(SetNamedPipeHandleState(input_, &no_wait, nullptr, nullptr));

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
        // A blocking write here froze the whole game for as long as the helper
        // was alive but not reading: stalled in the audio stack, suspended, or
        // sitting behind a crash dialog. A few seconds of cues fill the pipe
        // and WriteFile then never returns (measured: the window went "Not
        // Responding" 12 s after the helper was suspended and stayed so). The
        // pipe is in no-wait mode instead; what does not fit waits in pending_
        // and goes out with the next command. A helper that stays behind
        // loses cues, never the frame.
        if (pending_.size() + cue.size() + 1U > maximum_pending_bytes)
        {
            // Stale by now. Only the rest of a command whose first bytes are
            // already in the pipe has to follow them.
            const std::size_t line_end = command_begun_ ? pending_.find('\n') : std::string::npos;
            pending_.erase(line_end == std::string::npos ? 0U : line_end + 1U);
        }
        pending_.append(cue);
        pending_.push_back('\n');
        while (!pending_.empty())
        {
            // One command per write. A full no-wait pipe may take as much as
            // fits or nothing at all; a single command always fits an empty
            // pipe, so neither answer can leave the queue stuck behind a
            // write larger than the pipe itself.
            const std::size_t line_end = pending_.find('\n');
            const std::size_t count =
                line_end == std::string::npos ? pending_.size() : line_end + 1U;
            DWORD written{};
            if (!WriteFile(input_, pending_.data(), static_cast<DWORD>(count), &written, nullptr))
            {
                CloseHandle(input_);
                input_ = nullptr;
                pending_.clear();
                break;
            }
            if (written == 0U)
                break; // The pipe is full: the helper has not caught up yet.
            command_begun_ = pending_[written - 1U] != '\n';
            pending_.erase(0U, written);
        }
#else
        (void)cue;
#endif
    }

  private:
#if defined(_WIN32)
    // About a second of commands at their busiest; more than that is history.
    static constexpr std::size_t maximum_pending_bytes = 16U * 1024U;
    HANDLE input_{};
    HANDLE output_{};
    HANDLE process_{};
    bool ready_{};
    // Commands not yet accepted by the pipe, and whether the pipe holds the
    // beginning of the one at the front.
    std::string pending_;
    bool command_begun_{};
#endif
    std::string renderer_id_;
    std::string renderer_name_;
};

namespace
{
// (Graphics area) the installed sound pack's cue replacements (see set_cue_override).
OriginalSoundBank::CueOverride &cue_override_hook()
{
    static OriginalSoundBank::CueOverride hook;
    return hook;
}

bool cue_replaced(std::string_view cue, const world::Vec3 *position = nullptr)
{
    const auto &hook = cue_override_hook();
    return hook && hook(cue, position);
}
} // namespace

void OriginalSoundBank::set_cue_override(CueOverride hook)
{
    cue_override_hook() = std::move(hook);
}

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
        if (!cue_replaced("Sand"))
            impl_->play("Sand");
        break;
    case world::BlockType::leaves:
        if (!cue_replaced("leaves"))
            impl_->play("leaves");
        break;
    default:
        if (!cue_replaced("punch"))
            impl_->play("punch");
        break;
    }
}

void OriginalSoundBank::play_place()
{
    if (!cue_replaced("Place"))
        impl_->play("Place");
}

void OriginalSoundBank::play_footstep()
{
    if (!cue_replaced("FootStep"))
        impl_->play("FootStep");
}

void OriginalSoundBank::play_pickup()
{
    if (!cue_replaced("pickupitem"))
        impl_->play("pickupitem");
}

void OriginalSoundBank::play_click()
{
    if (!cue_replaced("Click"))
        impl_->play("Click");
}

void OriginalSoundBank::play_drop()
{
    if (!cue_replaced("dropitem"))
        impl_->play("dropitem");
}

void OriginalSoundBank::play_pistol()
{
    if (!cue_replaced("GunShot4"))
        impl_->play("GunShot4");
}

void OriginalSoundBank::play_reload()
{
    if (!cue_replaced("Reload"))
        impl_->play("Reload");
}

void OriginalSoundBank::play_cue(std::string_view cue)
{
    if (!cue.empty() && !cue_replaced(cue))
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
    if (!cue.empty() && !cue_replaced(cue, &position))
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
