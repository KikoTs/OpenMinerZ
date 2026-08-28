#include "openminerz/net/steam_bridge_client.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Aclapi.h>
#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace openminerz::net
{
namespace
{

using Clock = std::chrono::steady_clock;
constexpr std::wstring_view pipe_prefix = LR"(\\.\pipe\OpenMinerZ-SteamBridge-)";

class Handle final
{
  public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_{value}
    {
    }
    ~Handle()
    {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_{std::exchange(other.value_, nullptr)}
    {
    }
    Handle &operator=(Handle &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept
    {
        return value_;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE value = nullptr) noexcept
    {
        if (valid())
            CloseHandle(value_);
        value_ = value;
    }

  private:
    HANDLE value_{};
};

class LocalMemory final
{
  public:
    ~LocalMemory()
    {
        if (value_ != nullptr)
            LocalFree(value_);
    }
    [[nodiscard]] void **address() noexcept
    {
        return &value_;
    }
    [[nodiscard]] void *get() const noexcept
    {
        return value_;
    }

  private:
    void *value_{};
};

[[nodiscard]] std::string windows_error(std::string_view operation, DWORD code = GetLastError())
{
    return std::string{operation} + " failed with Windows error " + std::to_string(code);
}

[[nodiscard]] std::filesystem::path canonical_regular_file(const std::filesystem::path &path,
                                                           std::wstring_view required_name)
{
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::canonical(path, error);
    if (error || normalized.filename() != required_name ||
        !std::filesystem::is_regular_file(normalized, error) || error)
        throw std::runtime_error{"A required bridge file path is invalid"};
    return normalized;
}

[[nodiscard]] std::wstring make_pipe_name()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error{"Windows could not generate a bridge endpoint identity"};
    std::array<wchar_t, 39> encoded{};
    if (StringFromGUID2(guid, encoded.data(), static_cast<int>(encoded.size())) != 39)
        throw std::runtime_error{"Windows could not format a bridge endpoint identity"};
    return std::wstring{pipe_prefix} + std::wstring{encoded.data() + 1U, 36U};
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring quoted{L"\""};
    std::size_t backslashes{};
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(character);
            backslashes = 0U;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4U)
        throw std::runtime_error{"The helper returned a truncated frame header"};
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] bool process_alive(HANDLE process) noexcept
{
    return process != nullptr && WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
}

void cancel_and_throw(HANDLE pipe, OVERLAPPED &overlapped, std::string_view message)
{
    static_cast<void>(CancelIoEx(pipe, &overlapped));
    static_cast<void>(WaitForSingleObject(overlapped.hEvent, 100U));
    throw std::runtime_error{std::string{message}};
}

[[nodiscard]] DWORD transfer(HANDLE pipe, HANDLE process, bool write, void *buffer, DWORD count,
                             std::chrono::milliseconds timeout)
{
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid())
        throw std::runtime_error{windows_error("CreateEvent")};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred{};
    const BOOL started = write ? WriteFile(pipe, buffer, count, &transferred, &overlapped)
                               : ReadFile(pipe, buffer, count, &transferred, &overlapped);
    if (started != FALSE)
        return transferred;
    if (GetLastError() != ERROR_IO_PENDING)
        throw std::runtime_error{windows_error(write ? "WriteFile" : "ReadFile")};
    const std::array<HANDLE, 2> waits{event.get(), process};
    const DWORD wait =
        WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE,
                               static_cast<DWORD>(std::clamp<std::int64_t>(
                                   timeout.count(), 1, std::numeric_limits<DWORD>::max())));
    if (wait == WAIT_OBJECT_0 + 1U)
        cancel_and_throw(pipe, overlapped, "The Steam bridge process exited during pipe IO");
    if (wait != WAIT_OBJECT_0)
        cancel_and_throw(pipe, overlapped, "The Steam bridge pipe IO timed out");
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
        throw std::runtime_error{windows_error("GetOverlappedResult")};
    return transferred;
}

void write_exact(HANDLE pipe, HANDLE process, std::span<const std::uint8_t> bytes)
{
    std::size_t offset{};
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        const DWORD written =
            transfer(pipe, process, true, const_cast<std::uint8_t *>(bytes.data() + offset),
                     request, std::chrono::seconds{5});
        if (written == 0U)
            throw std::runtime_error{"The Steam bridge pipe write returned zero bytes"};
        offset += written;
    }
}

void read_exact(HANDLE pipe, HANDLE process, std::span<std::uint8_t> bytes)
{
    std::size_t offset{};
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        const DWORD received =
            transfer(pipe, process, false, bytes.data() + offset, request, std::chrono::seconds{5});
        if (received == 0U)
            throw std::runtime_error{"The Steam bridge pipe read returned zero bytes"};
        offset += received;
    }
}

} // namespace

class SteamBridgeClient::Implementation final
{
    enum class Mode : std::uint8_t
    {
        stopped,
        browsing,
        hosting,
        joined
    };

  public:
    ~Implementation()
    {
        std::string ignored;
        static_cast<void>(stop(ignored));
    }

    [[nodiscard]] bool protocol_self_test(const std::filesystem::path &helper_path,
                                          std::chrono::milliseconds timeout, std::string &error)
    {
        try
        {
            const std::filesystem::path helper =
                canonical_regular_file(helper_path, L"OpenMinerZ.SteamBridge32.exe");
            create_pipe();
            create_job();
            launch(helper, std::nullopt);
            connect(timeout);
            const SteamBridgeFrame hello = wait_for_frame(timeout);
            if (hello.type != SteamBridgeMessageType::hello || !hello.payload.empty())
                throw std::runtime_error{"The x86 protocol test did not send hello"};
            send_frame(SteamBridgeMessageType::heartbeat, {});
            const SteamBridgeFrame heartbeat = wait_for_frame(timeout);
            if (heartbeat.type != SteamBridgeMessageType::heartbeat || !heartbeat.payload.empty())
                throw std::runtime_error{"The x86 protocol test did not echo heartbeat"};
            send_frame(SteamBridgeMessageType::stop, {});
            const SteamBridgeFrame stopped = wait_for_frame(timeout);
            if (stopped.type != SteamBridgeMessageType::stopped || !stopped.payload.empty())
                throw std::runtime_error{"The x86 protocol test did not stop cleanly"};
            teardown();
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool start_host(const SteamBridgeClientOptions &options,
                                  const SteamBridgeHostConfig &config, SteamBridgeHostReady &ready,
                                  std::string &error)
    {
        try
        {
            SteamBridgeProtocolError protocol_error{};
            const auto host_payload = encode_steam_bridge_host_config(config, protocol_error);
            if (!host_payload.has_value())
                throw std::runtime_error{"The host configuration is invalid for the Steam bridge"};
            start_helper(options);
            send_frame(SteamBridgeMessageType::start_host, *host_payload);
            const SteamBridgeFrame started = wait_for_frame(options.host_timeout);
            if (started.type == SteamBridgeMessageType::failure)
                throw std::runtime_error{decode_failure_detail(started.payload)};
            if (started.type != SteamBridgeMessageType::host_ready ||
                !decode_steam_bridge_host_ready(started.payload, ready, protocol_error))
                throw std::runtime_error{"The x86 helper did not return valid host readiness"};
            running_ = true;
            mode_ = Mode::hosting;
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool start_discovery(const SteamBridgeClientOptions &options,
                                       const SteamBridgeDiscoveryRequest &request,
                                       SteamBridgeDiscoveryResults &results, std::string &error)
    {
        try
        {
            SteamBridgeProtocolError protocol_error{};
            const auto payload = encode_steam_bridge_discovery_request(request, protocol_error);
            if (!payload.has_value())
                throw std::runtime_error{"The discovery request is invalid for the Steam bridge"};
            start_helper(options);
            discovery_timeout_ = options.discovery_timeout;
            join_timeout_ = options.join_timeout;
            send_frame(SteamBridgeMessageType::discover_request, *payload);
            const SteamBridgeFrame response = wait_for_frame(discovery_timeout_);
            if (response.type == SteamBridgeMessageType::failure)
                throw std::runtime_error{decode_failure_detail(response.payload)};
            if (response.type != SteamBridgeMessageType::discovery_results ||
                !decode_steam_bridge_discovery_results(response.payload, results, protocol_error))
                throw std::runtime_error{"The x86 helper returned invalid discovery results"};
            running_ = true;
            mode_ = Mode::browsing;
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool refresh_discovery(const SteamBridgeDiscoveryRequest &request,
                                         SteamBridgeDiscoveryResults &results, std::string &error)
    {
        try
        {
            require_mode(Mode::browsing, "Discovery refresh requires browser state");
            SteamBridgeProtocolError protocol_error{};
            const auto payload = encode_steam_bridge_discovery_request(request, protocol_error);
            if (!payload.has_value())
                throw std::runtime_error{"The discovery refresh request is invalid"};
            send_frame(SteamBridgeMessageType::discover_request, *payload);
            const SteamBridgeFrame response = wait_for_frame(discovery_timeout_);
            if (response.type == SteamBridgeMessageType::failure)
                throw std::runtime_error{decode_failure_detail(response.payload)};
            if (response.type != SteamBridgeMessageType::discovery_results ||
                !decode_steam_bridge_discovery_results(response.payload, results, protocol_error))
                throw std::runtime_error{"The x86 helper returned invalid discovery results"};
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool join(const SteamBridgeJoinRequest &request, SteamBridgeJoinReady &ready,
                            std::string &error)
    {
        try
        {
            require_mode(Mode::browsing, "Lobby join requires browser state");
            SteamBridgeProtocolError protocol_error{};
            const auto payload = encode_steam_bridge_join_request(request, protocol_error);
            if (!payload.has_value())
                throw std::runtime_error{"The Steam bridge join request is invalid"};
            send_frame(SteamBridgeMessageType::join_request, *payload);
            const SteamBridgeFrame response = wait_for_frame(join_timeout_);
            if (response.type == SteamBridgeMessageType::failure)
                throw std::runtime_error{decode_failure_detail(response.payload)};
            if (response.type != SteamBridgeMessageType::join_ready ||
                !decode_steam_bridge_join_ready(response.payload, ready, protocol_error) ||
                ready.lobby.lobby_id != request.lobby_id)
                throw std::runtime_error{"The x86 helper returned invalid join readiness"};
            mode_ = Mode::joined;
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool send(const SteamBridgeOutboundWire &datagram, std::string &error)
    {
        try
        {
            require_gameplay_mode();
            SteamBridgeProtocolError protocol_error{};
            const auto payload = encode_steam_bridge_outbound_wire(datagram, protocol_error);
            if (!payload.has_value())
                throw std::runtime_error{"The outbound Steam bridge datagram is invalid"};
            send_frame(SteamBridgeMessageType::outbound_wire, *payload);
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }
    }

    [[nodiscard]] bool update(const SteamBridgeHostConfig &config, std::string &error)
    {
        try
        {
            require_mode(Mode::hosting, "Host update requires hosting state");
            SteamBridgeProtocolError protocol_error{};
            const auto payload = encode_steam_bridge_host_config(config, protocol_error);
            if (!payload.has_value())
                throw std::runtime_error{"The updated Steam host configuration is invalid"};
            send_frame(SteamBridgeMessageType::update_host, *payload);
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }
    }

    [[nodiscard]] bool poll(std::optional<SteamBridgeClientEvent> &event, std::string &error)
    {
        event.reset();
        try
        {
            require_running();
            const auto frame = receive_frame();
            if (!frame.has_value())
            {
                error.clear();
                return true;
            }
            SteamBridgeProtocolError protocol_error{};
            SteamBridgeClientEvent decoded;
            switch (frame->type)
            {
            case SteamBridgeMessageType::inbound_wire:
                decoded.kind = SteamBridgeClientEventKind::inbound_wire;
                if (!decode_steam_bridge_inbound_wire(frame->payload, decoded.inbound,
                                                      protocol_error))
                    throw std::runtime_error{"The helper returned an invalid inbound datagram"};
                break;
            case SteamBridgeMessageType::peer_left:
                decoded.kind = SteamBridgeClientEventKind::peer_left;
                if (!decode_steam_bridge_peer(frame->payload, decoded.peer, protocol_error))
                    throw std::runtime_error{"The helper returned an invalid departed peer"};
                break;
            case SteamBridgeMessageType::failure:
                decoded.kind = SteamBridgeClientEventKind::failure;
                if (!decode_steam_bridge_failure(frame->payload, decoded.failure, protocol_error))
                    throw std::runtime_error{"The helper returned a malformed failure"};
                running_ = false;
                break;
            case SteamBridgeMessageType::heartbeat:
                decoded.kind = SteamBridgeClientEventKind::heartbeat;
                require_empty(*frame);
                break;
            case SteamBridgeMessageType::stopped:
                decoded.kind = SteamBridgeClientEventKind::stopped;
                require_empty(*frame);
                running_ = false;
                break;
            default:
                throw std::runtime_error{"The helper returned a message invalid while running"};
            }
            event = std::move(decoded);
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }
    }

    [[nodiscard]] bool stop(std::string &error) noexcept
    {
        try
        {
            if (running_ && process_alive(process_.get()))
            {
                send_frame(SteamBridgeMessageType::stop, {});
                const auto deadline = Clock::now() + std::chrono::seconds{2};
                while (Clock::now() < deadline && process_alive(process_.get()))
                {
                    if (const auto frame = receive_frame();
                        frame.has_value() && frame->type == SteamBridgeMessageType::stopped)
                    {
                        require_empty(*frame);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
            }
            teardown();
            error.clear();
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            teardown();
            return false;
        }
        catch (...)
        {
            error = "Unknown Steam bridge shutdown error";
            teardown();
            return false;
        }
    }

    [[nodiscard]] bool running() const noexcept
    {
        return running_ && process_alive(process_.get());
    }

  private:
    void start_helper(const SteamBridgeClientOptions &options)
    {
        if (running_ || process_.valid() || pipe_.valid() || mode_ != Mode::stopped)
            throw std::runtime_error{"The Steam bridge client can only be started once"};
        if (options.connect_timeout <= std::chrono::milliseconds::zero() ||
            options.host_timeout <= std::chrono::milliseconds::zero() ||
            options.discovery_timeout <= std::chrono::milliseconds::zero() ||
            options.join_timeout <= std::chrono::milliseconds::zero())
            throw std::runtime_error{"Steam bridge deadlines must be positive"};
        const std::filesystem::path helper =
            canonical_regular_file(options.helper_path, L"OpenMinerZ.SteamBridge32.exe");
        const std::filesystem::path steam_api =
            canonical_regular_file(options.steam_api_path, L"steam_api.dll");
        create_pipe();
        create_job();
        launch(helper, steam_api);
        connect(options.connect_timeout);
        const SteamBridgeFrame hello = wait_for_frame(options.connect_timeout);
        if (hello.type != SteamBridgeMessageType::hello || !hello.payload.empty())
            throw std::runtime_error{"The x86 helper did not send the expected hello frame"};
    }

    void create_pipe()
    {
        pipe_name_ = make_pipe_name();
        Handle token;
        HANDLE raw_token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            throw std::runtime_error{windows_error("OpenProcessToken")};
        token.reset(raw_token);
        DWORD required{};
        static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0U, &required));
        if (required == 0U)
            throw std::runtime_error{windows_error("GetTokenInformation")};
        std::vector<std::uint8_t> token_user_storage(required);
        if (!GetTokenInformation(token.get(), TokenUser, token_user_storage.data(), required,
                                 &required))
            throw std::runtime_error{windows_error("GetTokenInformation")};
        const auto *token_user = reinterpret_cast<const TOKEN_USER *>(token_user_storage.data());
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<wchar_t *>(token_user->User.Sid);
        LocalMemory acl;
        const DWORD acl_result =
            SetEntriesInAclW(1U, &access, nullptr, reinterpret_cast<PACL *>(acl.address()));
        if (acl_result != ERROR_SUCCESS)
            throw std::runtime_error{windows_error("SetEntriesInAcl", acl_result)};
        SECURITY_DESCRIPTOR descriptor{};
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, static_cast<PACL>(acl.get()), FALSE))
            throw std::runtime_error{windows_error("SetSecurityDescriptorDacl")};
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
        pipe_.reset(CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1U,
            static_cast<DWORD>(steam_bridge_maximum_frame_size),
            static_cast<DWORD>(steam_bridge_maximum_frame_size), 0U, &attributes));
        if (!pipe_.valid())
            throw std::runtime_error{windows_error("CreateNamedPipe")};
    }

    void create_job()
    {
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_.valid())
            throw std::runtime_error{windows_error("CreateJobObject")};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)))
            throw std::runtime_error{windows_error("SetInformationJobObject")};
    }

    void launch(const std::filesystem::path &helper,
                const std::optional<std::filesystem::path> &steam_api)
    {
        std::wstring command = quote_argument(helper.native());
        if (!steam_api.has_value())
            command += L" --protocol-test";
        command += L" --pipe " + quote_argument(pipe_name_) + L" --parent-pid " +
                   std::to_wstring(GetCurrentProcessId());
        if (steam_api.has_value())
            command += L" --steam-api " + quote_argument(steam_api->native());
        std::vector<wchar_t> writable(command.begin(), command.end());
        writable.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process_info{};
        const std::wstring working_directory = helper.parent_path().native();
        if (!CreateProcessW(helper.c_str(), writable.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, working_directory.c_str(),
                            &startup, &process_info))
            throw std::runtime_error{windows_error("CreateProcess")};
        Handle thread{process_info.hThread};
        process_.reset(process_info.hProcess);
        if (!AssignProcessToJobObject(job_.get(), process_.get()))
            throw std::runtime_error{windows_error("AssignProcessToJobObject")};
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
            throw std::runtime_error{windows_error("ResumeThread")};
    }

    void connect(std::chrono::milliseconds timeout)
    {
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event.valid())
            throw std::runtime_error{windows_error("CreateEvent")};
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        if (ConnectNamedPipe(pipe_.get(), &overlapped) != FALSE)
            return;
        const DWORD connect_error = GetLastError();
        if (connect_error == ERROR_PIPE_CONNECTED)
            return;
        if (connect_error != ERROR_IO_PENDING)
            throw std::runtime_error{windows_error("ConnectNamedPipe", connect_error)};
        const std::array<HANDLE, 2> waits{event.get(), process_.get()};
        const DWORD wait =
            WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE,
                                   static_cast<DWORD>(std::clamp<std::int64_t>(
                                       timeout.count(), 1, std::numeric_limits<DWORD>::max())));
        if (wait == WAIT_OBJECT_0 + 1U)
            cancel_and_throw(pipe_.get(), overlapped,
                             "The x86 helper exited before connecting its pipe");
        if (wait != WAIT_OBJECT_0)
            cancel_and_throw(pipe_.get(), overlapped,
                             "The x86 helper did not connect before the deadline");
        DWORD transferred{};
        if (!GetOverlappedResult(pipe_.get(), &overlapped, &transferred, FALSE))
            throw std::runtime_error{windows_error("GetOverlappedResult")};
    }

    void send_frame(SteamBridgeMessageType type, std::span<const std::uint8_t> payload)
    {
        if (!process_alive(process_.get()))
            throw std::runtime_error{"The Steam bridge process is not running"};
        SteamBridgeProtocolError protocol_error{};
        const auto encoded =
            encode_steam_bridge_frame(type, outgoing_sequence_++, payload, protocol_error);
        if (!encoded.has_value())
            throw std::runtime_error{"OpenMinerZ attempted to send an invalid bridge frame"};
        write_exact(pipe_.get(), process_.get(), *encoded);
    }

    [[nodiscard]] std::optional<SteamBridgeFrame> receive_frame()
    {
        std::array<std::uint8_t, steam_bridge_header_size> header{};
        DWORD peeked{};
        DWORD available{};
        if (!PeekNamedPipe(pipe_.get(), header.data(), static_cast<DWORD>(header.size()), &peeked,
                           &available, nullptr))
            throw std::runtime_error{windows_error("PeekNamedPipe")};
        if (available == 0U)
        {
            partial_since_.reset();
            if (!process_alive(process_.get()))
                throw std::runtime_error{"The Steam bridge process exited"};
            return std::nullopt;
        }
        if (peeked < header.size())
        {
            note_partial();
            return std::nullopt;
        }
        const std::uint32_t payload_length = read_u32(header, 8U);
        if (payload_length > steam_bridge_maximum_payload_size)
            throw std::runtime_error{"The helper advertised an oversized bridge frame"};
        const std::size_t frame_size = steam_bridge_header_size + payload_length;
        if (available < frame_size)
        {
            note_partial();
            return std::nullopt;
        }
        partial_since_.reset();
        std::vector<std::uint8_t> encoded(frame_size);
        read_exact(pipe_.get(), process_.get(), encoded);
        SteamBridgeFrame decoded;
        SteamBridgeProtocolError protocol_error{};
        if (!decode_steam_bridge_frame(encoded, decoded, protocol_error))
            throw std::runtime_error{"The helper returned a corrupt bridge frame"};
        if (decoded.sequence != incoming_sequence_++)
            throw std::runtime_error{"The helper bridge sequence is missing or replayed"};
        return decoded;
    }

    [[nodiscard]] SteamBridgeFrame wait_for_frame(std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline)
        {
            if (const auto frame = receive_frame(); frame.has_value())
                return *frame;
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        throw std::runtime_error{"The x86 helper response timed out"};
    }

    [[nodiscard]] static std::string decode_failure_detail(std::span<const std::uint8_t> payload)
    {
        SteamBridgeFailure failure;
        SteamBridgeProtocolError protocol_error{};
        if (!decode_steam_bridge_failure(payload, failure, protocol_error))
            return "The x86 helper returned a malformed startup failure";
        return failure.detail;
    }

    static void require_empty(const SteamBridgeFrame &frame)
    {
        if (!frame.payload.empty())
            throw std::runtime_error{"The helper returned payload data on a control frame"};
    }

    void note_partial()
    {
        const auto now = Clock::now();
        if (!partial_since_.has_value())
            partial_since_ = now;
        else if (now - *partial_since_ > std::chrono::seconds{5})
            throw std::runtime_error{"The helper left a partial frame in the bridge pipe"};
    }

    void require_running() const
    {
        if (!running_ || !process_alive(process_.get()))
            throw std::runtime_error{"The Steam bridge is not running"};
    }

    void require_mode(Mode expected, std::string_view message) const
    {
        require_running();
        if (mode_ != expected)
            throw std::runtime_error{std::string{message}};
    }

    void require_gameplay_mode() const
    {
        require_running();
        if (mode_ != Mode::hosting && mode_ != Mode::joined)
            throw std::runtime_error{"Gameplay transport requires a joined or hosted lobby"};
    }

    void teardown() noexcept
    {
        running_ = false;
        mode_ = Mode::stopped;
        if (pipe_.valid())
        {
            static_cast<void>(CancelIoEx(pipe_.get(), nullptr));
            static_cast<void>(DisconnectNamedPipe(pipe_.get()));
        }
        pipe_.reset();
        // Closing the job is the final recovery path and terminates any surviving helper.
        job_.reset();
        if (process_.valid())
            static_cast<void>(WaitForSingleObject(process_.get(), 2'000U));
        process_.reset();
        pipe_name_.clear();
        partial_since_.reset();
        incoming_sequence_ = 1U;
        outgoing_sequence_ = 1U;
    }

    Handle pipe_;
    Handle process_;
    Handle job_;
    std::wstring pipe_name_;
    std::optional<Clock::time_point> partial_since_;
    std::uint32_t incoming_sequence_{1U};
    std::uint32_t outgoing_sequence_{1U};
    std::chrono::milliseconds discovery_timeout_{20'000};
    std::chrono::milliseconds join_timeout_{20'000};
    Mode mode_{Mode::stopped};
    bool running_{};
};

SteamBridgeClient::SteamBridgeClient() : implementation_{std::make_unique<Implementation>()}
{
}

SteamBridgeClient::~SteamBridgeClient() = default;
SteamBridgeClient::SteamBridgeClient(SteamBridgeClient &&) noexcept = default;
SteamBridgeClient &SteamBridgeClient::operator=(SteamBridgeClient &&) noexcept = default;

bool SteamBridgeClient::protocol_self_test(const std::filesystem::path &helper_path,
                                           std::chrono::milliseconds timeout, std::string &error)
{
    Implementation implementation;
    return implementation.protocol_self_test(helper_path, timeout, error);
}

bool SteamBridgeClient::start_host(const SteamBridgeClientOptions &options,
                                   const SteamBridgeHostConfig &config, SteamBridgeHostReady &ready,
                                   std::string &error)
{
    return implementation_->start_host(options, config, ready, error);
}

bool SteamBridgeClient::start_discovery(const SteamBridgeClientOptions &options,
                                        const SteamBridgeDiscoveryRequest &request,
                                        SteamBridgeDiscoveryResults &results, std::string &error)
{
    return implementation_->start_discovery(options, request, results, error);
}

bool SteamBridgeClient::refresh_discovery(const SteamBridgeDiscoveryRequest &request,
                                          SteamBridgeDiscoveryResults &results, std::string &error)
{
    return implementation_->refresh_discovery(request, results, error);
}

bool SteamBridgeClient::join(const SteamBridgeJoinRequest &request, SteamBridgeJoinReady &ready,
                             std::string &error)
{
    return implementation_->join(request, ready, error);
}

bool SteamBridgeClient::send(const SteamBridgeOutboundWire &datagram, std::string &error)
{
    return implementation_->send(datagram, error);
}

bool SteamBridgeClient::update_host(const SteamBridgeHostConfig &config, std::string &error)
{
    return implementation_->update(config, error);
}

bool SteamBridgeClient::poll(std::optional<SteamBridgeClientEvent> &event, std::string &error)
{
    return implementation_->poll(event, error);
}

bool SteamBridgeClient::stop(std::string &error) noexcept
{
    return implementation_->stop(error);
}

bool SteamBridgeClient::running() const noexcept
{
    return implementation_->running();
}

} // namespace openminerz::net

#else

namespace openminerz::net
{

class SteamBridgeClient::Implementation final
{
};

SteamBridgeClient::SteamBridgeClient() : implementation_{std::make_unique<Implementation>()}
{
}
SteamBridgeClient::~SteamBridgeClient() = default;
SteamBridgeClient::SteamBridgeClient(SteamBridgeClient &&) noexcept = default;
SteamBridgeClient &SteamBridgeClient::operator=(SteamBridgeClient &&) noexcept = default;
bool SteamBridgeClient::protocol_self_test(const std::filesystem::path &, std::chrono::milliseconds,
                                           std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::start_host(const SteamBridgeClientOptions &, const SteamBridgeHostConfig &,
                                   SteamBridgeHostReady &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::start_discovery(const SteamBridgeClientOptions &,
                                        const SteamBridgeDiscoveryRequest &,
                                        SteamBridgeDiscoveryResults &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::refresh_discovery(const SteamBridgeDiscoveryRequest &,
                                          SteamBridgeDiscoveryResults &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::join(const SteamBridgeJoinRequest &, SteamBridgeJoinReady &,
                             std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::send(const SteamBridgeOutboundWire &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::update_host(const SteamBridgeHostConfig &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::poll(std::optional<SteamBridgeClientEvent> &, std::string &error)
{
    error = "The legacy Steam bridge is available only on Windows";
    return false;
}
bool SteamBridgeClient::stop(std::string &error) noexcept
{
    error.clear();
    return true;
}
bool SteamBridgeClient::running() const noexcept
{
    return false;
}

} // namespace openminerz::net

#endif
