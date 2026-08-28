#include "openminerz/net/steam_bridge_protocol.hpp"

#include "steam_native.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

using namespace openminerz::net;
using openminerz::steam_bridge::SteamNative;
using Clock = std::chrono::steady_clock;

constexpr std::wstring_view pipe_prefix = LR"(\\.\pipe\OpenMinerZ-SteamBridge-)";
constexpr std::size_t guid_text_length = 36U;
constexpr std::uint32_t maximum_drain_size = 1024U * 1024U;
constexpr std::size_t maximum_packets_per_poll = 256U;
constexpr std::int32_t native_unreliable_send = 0;
constexpr std::int32_t native_reliable_send = 2;

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

struct Arguments final
{
    bool self_test{};
    bool protocol_test{};
    std::wstring pipe_name;
    std::filesystem::path steam_api_path;
    DWORD parent_process_id{};
};

[[nodiscard]] bool valid_guid_suffix(std::wstring_view value) noexcept
{
    if (value.size() != guid_text_length)
        return false;
    for (std::size_t index{}; index < value.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            if (value[index] != L'-')
                return false;
            continue;
        }
        const wchar_t character = value[index];
        if (!((character >= L'0' && character <= L'9') ||
              (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F')))
            return false;
    }
    return true;
}

[[nodiscard]] bool valid_pipe_name(std::wstring_view value) noexcept
{
    return value.starts_with(pipe_prefix) && valid_guid_suffix(value.substr(pipe_prefix.size()));
}

[[nodiscard]] DWORD parse_process_id(std::wstring_view value)
{
    if (value.empty())
        throw std::runtime_error{"Missing parent process ID"};
    std::string ascii;
    ascii.reserve(value.size());
    for (const wchar_t character : value)
    {
        if (character < L'0' || character > L'9')
            throw std::runtime_error{"Invalid parent process ID"};
        ascii.push_back(static_cast<char>(character));
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != ascii.data() + ascii.size() || parsed == 0U ||
        parsed > std::numeric_limits<DWORD>::max())
        throw std::runtime_error{"Invalid parent process ID"};
    return static_cast<DWORD>(parsed);
}

[[nodiscard]] Arguments parse_arguments(int count, wchar_t **values)
{
    Arguments arguments;
    if (count == 2 && std::wstring_view{values[1]} == L"--self-test")
    {
        arguments.self_test = true;
        return arguments;
    }
    for (int index = 1; index < count; ++index)
    {
        const std::wstring_view argument{values[index]};
        if (argument == L"--protocol-test" && !arguments.protocol_test)
        {
            arguments.protocol_test = true;
            continue;
        }
        if (index + 1 >= count)
            throw std::runtime_error{"Bridge arguments must be key/value pairs"};
        const std::wstring_view value{values[++index]};
        if (argument == L"--pipe" && arguments.pipe_name.empty())
            arguments.pipe_name = value;
        else if (argument == L"--parent-pid" && arguments.parent_process_id == 0U)
            arguments.parent_process_id = parse_process_id(value);
        else if (argument == L"--steam-api" && arguments.steam_api_path.empty())
            arguments.steam_api_path = value;
        else
            throw std::runtime_error{"Unknown or duplicate bridge argument"};
    }
    if (!valid_pipe_name(arguments.pipe_name) || arguments.parent_process_id == 0U ||
        (!arguments.protocol_test && arguments.steam_api_path.empty()))
        throw std::runtime_error{"Required bridge arguments are missing or malformed"};
    if (arguments.parent_process_id == GetCurrentProcessId())
        throw std::runtime_error{"The bridge cannot supervise itself"};
    return arguments;
}

[[nodiscard]] std::filesystem::path normalized_library_path(const std::filesystem::path &path)
{
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::canonical(path, error);
    if (error || normalized.filename() != L"steam_api.dll" ||
        !std::filesystem::is_regular_file(normalized, error) || error)
        throw std::runtime_error{"The steam_api.dll path is not a canonical regular file"};
    return normalized;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4U)
        throw std::runtime_error{"Truncated bridge frame header"};
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] bool parent_alive(HANDLE parent) noexcept
{
    return WaitForSingleObject(parent, 0U) == WAIT_TIMEOUT;
}

void write_exact(HANDLE pipe, std::span<const std::uint8_t> bytes)
{
    std::size_t offset{};
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (!WriteFile(pipe, bytes.data() + offset, request, &written, nullptr) || written == 0U)
            throw std::runtime_error{"The bridge pipe write failed"};
        offset += written;
    }
}

void read_exact(HANDLE pipe, std::span<std::uint8_t> bytes)
{
    std::size_t offset{};
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD received{};
        if (!ReadFile(pipe, bytes.data() + offset, request, &received, nullptr) || received == 0U)
            throw std::runtime_error{"The bridge pipe read failed"};
        offset += received;
    }
}

class PipeEndpoint final
{
  public:
    PipeEndpoint(const std::wstring &pipe_name, DWORD parent_process_id)
    {
        parent_.reset(
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_process_id));
        if (!parent_.valid() || !parent_alive(parent_.get()))
            throw std::runtime_error{"The OpenMinerZ parent process is unavailable"};
        DWORD parent_session{};
        DWORD local_session{};
        if (!ProcessIdToSessionId(parent_process_id, &parent_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &local_session) ||
            parent_session != local_session)
            throw std::runtime_error{"The bridge and parent must share a Windows session"};
        if (!WaitNamedPipeW(pipe_name.c_str(), 10'000U))
            throw std::runtime_error{"The secured OpenMinerZ bridge pipe was not available"};
        pipe_.reset(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr,
                                OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                nullptr));
        if (!pipe_.valid())
            throw std::runtime_error{"The secured OpenMinerZ bridge pipe could not be opened"};
        ULONG server_process_id{};
        if (!GetNamedPipeServerProcessId(pipe_.get(), &server_process_id) ||
            server_process_id != parent_process_id)
            throw std::runtime_error{"The bridge pipe server is not the declared parent process"};
    }

    [[nodiscard]] bool parent_is_alive() const noexcept
    {
        return parent_alive(parent_.get());
    }

    void send(SteamBridgeMessageType type, std::span<const std::uint8_t> payload = {})
    {
        SteamBridgeProtocolError error{};
        const auto frame = encode_steam_bridge_frame(type, outgoing_sequence_++, payload, error);
        if (!frame.has_value())
            throw std::runtime_error{"The helper generated an invalid bridge frame"};
        write_exact(pipe_.get(), *frame);
    }

    [[nodiscard]] std::optional<SteamBridgeFrame> receive()
    {
        std::array<std::uint8_t, steam_bridge_header_size> header{};
        DWORD peeked{};
        DWORD available{};
        if (!PeekNamedPipe(pipe_.get(), header.data(), static_cast<DWORD>(header.size()), &peeked,
                           &available, nullptr))
            throw std::runtime_error{"The bridge pipe peek failed"};
        if (available == 0U)
        {
            partial_since_.reset();
            return std::nullopt;
        }
        if (peeked < header.size())
        {
            note_partial();
            return std::nullopt;
        }
        const std::uint32_t payload_length = read_u32(header, 8U);
        if (payload_length > steam_bridge_maximum_payload_size)
            throw std::runtime_error{"The parent advertised an oversized bridge frame"};
        const std::size_t frame_size = steam_bridge_header_size + payload_length;
        if (available < frame_size)
        {
            note_partial();
            return std::nullopt;
        }
        partial_since_.reset();
        std::vector<std::uint8_t> encoded(frame_size);
        read_exact(pipe_.get(), encoded);
        SteamBridgeFrame frame;
        SteamBridgeProtocolError error{};
        if (!decode_steam_bridge_frame(encoded, frame, error))
            throw std::runtime_error{"The parent sent an invalid bridge frame"};
        if (frame.sequence != incoming_sequence_++)
            throw std::runtime_error{"The parent bridge sequence is missing or replayed"};
        return frame;
    }

  private:
    void note_partial()
    {
        const auto now = Clock::now();
        if (!partial_since_.has_value())
            partial_since_ = now;
        else if (now - *partial_since_ > std::chrono::seconds{5})
            throw std::runtime_error{"The parent left a partial bridge frame in the pipe"};
    }

    Handle parent_;
    Handle pipe_;
    std::uint32_t incoming_sequence_{1U};
    std::uint32_t outgoing_sequence_{1U};
    std::optional<Clock::time_point> partial_since_;
};

[[nodiscard]] std::string creation_date()
{
    static constexpr std::array<std::string_view, 7> weekdays{"Sun", "Mon", "Tue", "Wed",
                                                              "Thu", "Fri", "Sat"};
    static constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    SYSTEMTIME value{};
    GetSystemTime(&value);
    if (value.wDayOfWeek >= weekdays.size() || value.wMonth < 1U || value.wMonth > months.size())
        throw std::runtime_error{"Windows returned an invalid UTC timestamp"};
    std::array<char, 40> encoded{};
    const int written =
        snprintf(encoded.data(), encoded.size(), "%.*s, %02u %.*s %04u %02u:%02u:%02u GMT", 3,
                 weekdays[value.wDayOfWeek].data(), value.wDay, 3, months[value.wMonth - 1U].data(),
                 value.wYear, value.wHour, value.wMinute, value.wSecond);
    if (written <= 0 || static_cast<std::size_t>(written) >= encoded.size())
        throw std::runtime_error{"The lobby creation timestamp could not be formatted"};
    return {encoded.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::int32_t native_lobby_type(SteamBridgeJoinPolicy policy)
{
    switch (policy)
    {
    case SteamBridgeJoinPolicy::anyone:
        return 2;
    case SteamBridgeJoinPolicy::friends_only:
        return 1;
    case SteamBridgeJoinPolicy::invite_only:
        return 0;
    }
    throw std::runtime_error{"Invalid lobby join policy"};
}

[[nodiscard]] std::int32_t native_send_type(SteamDeliveryMethod delivery)
{
    switch (delivery)
    {
    case SteamDeliveryMethod::unknown:
    case SteamDeliveryMethod::unreliable:
    case SteamDeliveryMethod::unreliable_sequenced:
        return native_unreliable_send;
    case SteamDeliveryMethod::reliable_unordered:
    case SteamDeliveryMethod::reliable_sequenced:
    case SteamDeliveryMethod::reliable_ordered:
        return native_reliable_send;
    }
    throw std::runtime_error{"Invalid native delivery method"};
}

void require_metadata(SteamNative &native, std::uint64_t lobby_id, const char *key,
                      const std::string &value)
{
    if (!native.set_lobby_data(lobby_id, key, value.c_str()))
        throw std::runtime_error{std::string{"Steam rejected lobby metadata key "} + key};
}

[[nodiscard]] std::optional<std::int32_t> parse_lobby_integer(const std::string &value) noexcept
{
    if (value.empty())
        return std::nullopt;
    std::int32_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return std::numeric_limits<std::int32_t>::min();
    return parsed;
}

[[nodiscard]] std::string encode_hex(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded(bytes.size() * 2U, '0');
    for (std::size_t index{}; index < bytes.size(); ++index)
    {
        encoded[index * 2U] = digits[bytes[index] >> 4U];
        encoded[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return encoded;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view text)
{
    if (text.size() > steam_bridge_maximum_extension_metadata_size * 2U || text.size() % 2U != 0U)
        return std::nullopt;
    const auto nibble = [](char character) -> std::optional<std::uint8_t> {
        if (character >= '0' && character <= '9')
            return static_cast<std::uint8_t>(character - '0');
        if (character >= 'a' && character <= 'f')
            return static_cast<std::uint8_t>(character - 'a' + 10);
        if (character >= 'A' && character <= 'F')
            return static_cast<std::uint8_t>(character - 'A' + 10);
        return std::nullopt;
    };
    std::vector<std::uint8_t> decoded(text.size() / 2U);
    for (std::size_t index{}; index < decoded.size(); ++index)
    {
        const auto high = nibble(text[index * 2U]);
        const auto low = nibble(text[index * 2U + 1U]);
        if (!high.has_value() || !low.has_value())
            return std::nullopt;
        decoded[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return decoded;
}

struct LobbyCandidate final
{
    std::uint64_t lobby_id{};
    std::uint8_t friend_count{};
};

enum class LobbyReadFailure : std::uint8_t
{
    none,
    native_shape,
    metadata,
    bridge_contract
};

class BridgeRuntime final
{
  public:
    BridgeRuntime(PipeEndpoint &pipe, std::filesystem::path steam_api_path)
        : pipe_{pipe}, steam_api_path_{std::move(steam_api_path)}
    {
    }

    ~BridgeRuntime()
    {
        cleanup();
    }

    void run()
    {
        pipe_.send(SteamBridgeMessageType::hello);
        const SteamBridgeFrame first = wait_for_first_command();
        initialize_native();
        dispatch_initial(first);
        while (pipe_.parent_is_alive())
        {
            native_->run_callbacks();
            if (mode_ == Mode::hosting || mode_ == Mode::joined)
            {
                poll_members(false);
                poll_packets();
            }
            if (const auto frame = pipe_.receive(); frame.has_value())
            {
                if (dispatch(*frame))
                    return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        throw std::runtime_error{"The OpenMinerZ parent process exited"};
    }

  private:
    enum class Mode : std::uint8_t
    {
        initial,
        browsing,
        hosting,
        joined
    };

    [[nodiscard]] SteamBridgeFrame wait_for_first_command()
    {
        const auto deadline = Clock::now() + std::chrono::seconds{30};
        while (pipe_.parent_is_alive() && Clock::now() < deadline)
        {
            const auto frame = pipe_.receive();
            if (!frame.has_value())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                continue;
            }
            if (frame->type != SteamBridgeMessageType::start_host &&
                frame->type != SteamBridgeMessageType::discover_request)
                throw std::runtime_error{
                    "The first parent command was neither start_host nor discover_request"};
            return std::move(*frame);
        }
        throw std::runtime_error{"Timed out waiting for the parent bridge command"};
    }

    void initialize_native()
    {
        native_ = std::make_unique<SteamNative>(steam_api_path_);
        if (native_->app_id() != castle_miner_z_steam_app_id)
            throw std::runtime_error{"Steam initialized the wrong app ID"};
        if (!native_->is_logged_on())
            throw std::runtime_error{"Steam initialized without a logged-on user"};
        local_steam_id_ = native_->local_steam_id();
        if (local_steam_id_ == 0U)
            throw std::runtime_error{"Steam returned an invalid local user ID"};
        persona_name_ = native_->persona_name();
    }

    void dispatch_initial(const SteamBridgeFrame &frame)
    {
        SteamBridgeProtocolError error{};
        if (frame.type == SteamBridgeMessageType::start_host)
        {
            SteamBridgeHostConfig config;
            if (!decode_steam_bridge_host_config(frame.payload, config, error))
                throw std::runtime_error{"The parent sent an invalid host configuration"};
            start_host(config);
            return;
        }
        SteamBridgeDiscoveryRequest request;
        if (!decode_steam_bridge_discovery_request(frame.payload, request, error))
            throw std::runtime_error{"The parent sent an invalid discovery request"};
        mode_ = Mode::browsing;
        discover(request);
    }

    void start_host(const SteamBridgeHostConfig &config)
    {
        const std::uint64_t api_call = native_->create_public_lobby(config.maximum_players);
        if (api_call == 0U)
            throw std::runtime_error{"Steam rejected CreateLobby before it was queued"};
        lobby_id_ = wait_for_lobby(api_call);
        initial_maximum_players_ = config.maximum_players;
        publish_metadata(config, true);
        mode_ = Mode::hosting;
        poll_members(true);
        SteamBridgeProtocolError error{};
        const auto ready = encode_steam_bridge_host_ready(
            SteamBridgeHostReady{local_steam_id_, lobby_id_, persona_name_}, error);
        if (!ready.has_value())
            throw std::runtime_error{"Steam supplied invalid host identity metadata"};
        pipe_.send(SteamBridgeMessageType::host_ready, *ready);
    }

    [[nodiscard]] std::uint64_t wait_for_lobby(std::uint64_t api_call)
    {
        const auto deadline = Clock::now() + std::chrono::seconds{15};
        while (pipe_.parent_is_alive() && Clock::now() < deadline)
        {
            native_->run_callbacks();
            bool completion_failed{};
            if (native_->is_api_call_completed(api_call, completion_failed))
            {
                if (completion_failed)
                    throw std::runtime_error{"Steam lobby completion reported an IO failure"};
                bool result_failed{};
                std::int32_t result{};
                std::uint64_t lobby{};
                if (!native_->get_lobby_created_result(api_call, result, lobby, result_failed) ||
                    result_failed || result != 1 || lobby == 0U)
                    throw std::runtime_error{"Steam lobby creation returned an invalid result"};
                return lobby;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        throw std::runtime_error{"Steam lobby creation timed out"};
    }

    [[nodiscard]] std::uint32_t wait_for_lobby_matches(std::uint64_t api_call)
    {
        const auto deadline = Clock::now() + std::chrono::seconds{15};
        while (pipe_.parent_is_alive() && Clock::now() < deadline)
        {
            native_->run_callbacks();
            bool completion_failed{};
            if (native_->is_api_call_completed(api_call, completion_failed))
            {
                if (completion_failed)
                    throw std::runtime_error{"Steam lobby search reported an IO failure"};
                bool result_failed{};
                std::uint32_t count{};
                if (!native_->get_lobby_match_count(api_call, count, result_failed) ||
                    result_failed)
                    throw std::runtime_error{"Steam lobby search returned an invalid result"};
                return count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        throw std::runtime_error{"Steam lobby search timed out"};
    }

    void wait_for_lobby_enter(std::uint64_t api_call, std::uint64_t expected_lobby)
    {
        const auto deadline = Clock::now() + std::chrono::seconds{15};
        while (pipe_.parent_is_alive() && Clock::now() < deadline)
        {
            native_->run_callbacks();
            bool completion_failed{};
            if (native_->is_api_call_completed(api_call, completion_failed))
            {
                if (completion_failed)
                    throw std::runtime_error{"Steam lobby join reported an IO failure"};
                bool result_failed{};
                std::uint32_t response{};
                std::uint64_t lobby{};
                if (!native_->get_lobby_enter_result(api_call, lobby, response, result_failed) ||
                    result_failed || response != 1U || lobby != expected_lobby)
                    throw std::runtime_error{"Steam lobby join returned an invalid result"};
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        throw std::runtime_error{"Steam lobby join timed out"};
    }

    [[nodiscard]] std::optional<SteamBridgeDiscoveredLobby> read_lobby(
        const LobbyCandidate &candidate, bool require_valid_properties,
        LobbyReadFailure *failure = nullptr)
    {
        if (failure != nullptr)
            *failure = LobbyReadFailure::none;
        SteamBridgeDiscoveredLobby lobby;
        lobby.lobby_id = candidate.lobby_id;
        lobby.friend_count = candidate.friend_count;
        lobby.host_peer = native_->lobby_owner(candidate.lobby_id);
        // EFriendRelationship::Friend is ordinal three in the legacy API.
        lobby.host_is_friend = lobby.host_peer != 0U && lobby.host_peer != local_steam_id_ &&
                               native_->friend_relationship(lobby.host_peer) == 3;
        const std::int32_t members = native_->lobby_member_count(candidate.lobby_id);
        const std::int32_t limit = native_->lobby_member_limit(candidate.lobby_id);
        if ((require_valid_properties && lobby.host_peer == 0U) || members < 0 || members > 16 ||
            limit < 1 || limit > 16 || members > limit)
        {
            last_discovery_diagnostic_ = "lobby=" + std::to_string(candidate.lobby_id) +
                                         " host=" + std::to_string(lobby.host_peer) +
                                         " members=" + std::to_string(members) +
                                         " limit=" + std::to_string(limit);
            if (failure != nullptr)
                *failure = LobbyReadFailure::native_shape;
            return std::nullopt;
        }
        lobby.current_players = static_cast<std::uint8_t>(members);
        lobby.maximum_players = static_cast<std::uint8_t>(limit);
        const auto join_policy =
            parse_lobby_integer(native_->lobby_data(candidate.lobby_id, "JoinPolicy", 8U));
        // The stock client treats a missing JoinPolicy key as Anyone. Keep the
        // same compatibility fallback so older and partially populated public
        // lobbies remain visible instead of disappearing from discovery.
        if (join_policy.has_value() && (*join_policy < 0 || *join_policy > 2))
        {
            if (failure != nullptr)
                *failure = LobbyReadFailure::metadata;
            return std::nullopt;
        }
        lobby.join_policy = join_policy.has_value()
                                ? static_cast<SteamBridgeJoinPolicy>(*join_policy)
                                : SteamBridgeJoinPolicy::anyone;
        lobby.password_protected =
            native_->lobby_data(candidate.lobby_id, "Protected", 8U) == "True";
        lobby.name = native_->lobby_data(candidate.lobby_id, "Name", 128U);
        lobby.created_at = native_->lobby_data(candidate.lobby_id, "Date", 64U);
        lobby.host_name = lobby.host_peer == local_steam_id_
                              ? persona_name_
                              : native_->friend_persona_name(lobby.host_peer);
        if (lobby.name.empty())
            lobby.name = "CASTLEMINER Z LOBBY " + std::to_string(candidate.lobby_id);
        if (lobby.created_at.empty())
            lobby.created_at = "STEAM LOBBY";
        if (lobby.host_name.empty())
            lobby.host_name = lobby.host_peer == 0U
                                  ? "STEAM HOST"
                                  : "STEAM HOST " + std::to_string(lobby.host_peer);
        for (std::size_t index{}; index < lobby.session_properties.size(); ++index)
        {
            const std::string key = std::to_string(index);
            lobby.session_properties[index] =
                parse_lobby_integer(native_->lobby_data(candidate.lobby_id, key.c_str(), 16U));
        }
        const std::string extension = native_->lobby_data(
            candidate.lobby_id, "OMZE", steam_bridge_maximum_extension_metadata_size * 2U);
        const auto decoded_extension = decode_hex(extension);
        if (!decoded_extension.has_value())
        {
            if (failure != nullptr)
                *failure = LobbyReadFailure::metadata;
            return std::nullopt;
        }
        lobby.extension_metadata = *decoded_extension;
        SteamBridgeProtocolError error{};
        if (require_valid_properties)
        {
            const SteamBridgeJoinReady ready{local_steam_id_, persona_name_, lobby};
            if (!encode_steam_bridge_join_ready(ready, error).has_value())
            {
                if (failure != nullptr)
                    *failure = LobbyReadFailure::bridge_contract;
                return std::nullopt;
            }
        }
        else
        {
            SteamBridgeDiscoveryResults validation;
            validation.local_peer = local_steam_id_;
            validation.persona_name = persona_name_;
            validation.lobbies.push_back(lobby);
            if (!encode_steam_bridge_discovery_results(validation, error).has_value())
            {
                if (failure != nullptr)
                    *failure = LobbyReadFailure::bridge_contract;
                return std::nullopt;
            }
        }
        return lobby;
    }

    [[nodiscard]] std::vector<LobbyCandidate> lobby_candidates(std::uint32_t public_count,
                                                               std::uint8_t maximum_results)
    {
        if (public_count > maximum_results)
            throw std::runtime_error{"Steam exceeded the requested lobby-result ceiling"};
        std::vector<LobbyCandidate> candidates;
        candidates.reserve(maximum_results);
        const std::int32_t friends = native_->friend_count(4);
        if (friends < 0 || friends > 10'000)
            throw std::runtime_error{"Steam returned an invalid friend count"};
        for (std::int32_t index{}; index < friends; ++index)
        {
            const std::uint64_t friend_id = native_->friend_by_index(index, 4);
            const std::uint64_t lobby_id =
                friend_id == 0U ? 0U : native_->friend_game_lobby(friend_id);
            if (lobby_id == 0U)
                continue;
            const auto existing = std::find_if(
                candidates.begin(), candidates.end(),
                [&](const LobbyCandidate &candidate) { return candidate.lobby_id == lobby_id; });
            if (existing != candidates.end())
            {
                if (existing->friend_count < 16U)
                    ++existing->friend_count;
            }
            else if (candidates.size() < maximum_results)
                candidates.push_back({lobby_id, 1U});
        }
        for (std::uint32_t index{}; index < public_count && candidates.size() < maximum_results;
             ++index)
        {
            const std::uint64_t lobby_id =
                native_->lobby_by_index(static_cast<std::int32_t>(index));
            if (lobby_id != 0U &&
                std::none_of(candidates.begin(), candidates.end(),
                             [&](const auto &candidate) { return candidate.lobby_id == lobby_id; }))
                candidates.push_back({lobby_id, 0U});
        }
        return candidates;
    }

    void discover(const SteamBridgeDiscoveryRequest &request)
    {
        last_discovery_diagnostic_.clear();
        if (request.protocol.has_value())
        {
            const std::string protocol =
                std::to_string(static_cast<std::uint8_t>(*request.protocol));
            native_->add_lobby_string_filter("0", protocol.c_str());
        }
        native_->add_lobby_distance_filter(2);
        native_->add_lobby_result_count_filter(request.maximum_results);
        const std::uint64_t call = native_->request_lobby_list();
        if (call == 0U)
            throw std::runtime_error{"Steam rejected RequestLobbyList before it was queued"};
        const std::uint32_t public_count = wait_for_lobby_matches(call);
        const std::vector<LobbyCandidate> candidates =
            lobby_candidates(public_count, request.maximum_results);
        for (const LobbyCandidate &candidate : candidates)
            static_cast<void>(native_->request_lobby_data(candidate.lobby_id));

        std::vector<std::optional<SteamBridgeDiscoveredLobby>> resolved(candidates.size());
        std::vector<LobbyReadFailure> failures(candidates.size());
        const auto deadline = Clock::now() + std::chrono::seconds{5};
        while (pipe_.parent_is_alive() && Clock::now() < deadline)
        {
            native_->run_callbacks();
            bool complete = true;
            for (std::size_t index{}; index < candidates.size(); ++index)
            {
                if (!resolved[index].has_value())
                    resolved[index] = read_lobby(candidates[index], false, &failures[index]);
                complete = complete && resolved[index].has_value();
            }
            if (complete)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        SteamBridgeDiscoveryResults results;
        results.local_peer = local_steam_id_;
        results.persona_name = persona_name_;
        results.reported_lobby_count = static_cast<std::uint16_t>(candidates.size());
        last_discovered_lobbies_.clear();
        for (std::size_t index{}; index < candidates.size(); ++index)
        {
            if (!resolved[index].has_value())
                resolved[index] = read_lobby(candidates[index], false, &failures[index]);
            if (resolved[index].has_value())
            {
                // Steam's public-lobby query applies the protocol filter, but friend-game
                // lobbies are merged in separately. Match the original client's final
                // property check so a Modern friend lobby cannot leak into a Classic query
                // (or vice versa).
                const std::optional<std::int32_t> advertised_protocol =
                    resolved[index]->session_properties[0];
                if (request.protocol.has_value() && advertised_protocol.has_value() &&
                    *advertised_protocol != static_cast<std::int32_t>(*request.protocol))
                {
                    --results.reported_lobby_count;
                    continue;
                }
                last_discovered_lobbies_.insert(resolved[index]->lobby_id);
                results.lobbies.push_back(std::move(*resolved[index]));
            }
        }
        results.rejected_lobby_count =
            static_cast<std::uint16_t>(results.reported_lobby_count - results.lobbies.size());
        results.rejected_native_count = static_cast<std::uint16_t>(
            std::count(failures.begin(), failures.end(), LobbyReadFailure::native_shape));
        results.rejected_metadata_count = static_cast<std::uint16_t>(
            std::count(failures.begin(), failures.end(), LobbyReadFailure::metadata));
        results.rejected_contract_count = static_cast<std::uint16_t>(
            std::count(failures.begin(), failures.end(), LobbyReadFailure::bridge_contract));
        results.diagnostic = last_discovery_diagnostic_;
        SteamBridgeProtocolError error{};
        const auto payload = encode_steam_bridge_discovery_results(results, error);
        if (!payload.has_value())
            throw std::runtime_error{"Bounded Steam discovery results failed bridge encoding"};
        pipe_.send(SteamBridgeMessageType::discovery_results, *payload);
    }

    void join(const SteamBridgeJoinRequest &request)
    {
        if (!last_discovered_lobbies_.contains(request.lobby_id))
            throw std::runtime_error{"The parent attempted to join an undiscovered lobby"};
        const std::uint64_t call = native_->join_lobby(request.lobby_id);
        if (call == 0U)
            throw std::runtime_error{"Steam rejected JoinLobby before it was queued"};
        wait_for_lobby_enter(call, request.lobby_id);
        lobby_id_ = request.lobby_id;
        mode_ = Mode::joined;
        static_cast<void>(native_->request_lobby_data(lobby_id_));
        poll_members(true);

        std::optional<SteamBridgeDiscoveredLobby> lobby;
        const LobbyCandidate candidate{lobby_id_, 0U};
        const auto deadline = Clock::now() + std::chrono::seconds{5};
        while (pipe_.parent_is_alive() && Clock::now() < deadline && !lobby.has_value())
        {
            native_->run_callbacks();
            poll_members(false);
            lobby = read_lobby(candidate, true);
            if (!lobby.has_value())
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (!lobby.has_value())
            throw std::runtime_error{"Joined lobby metadata did not become valid before timeout"};
        SteamBridgeProtocolError error{};
        const auto payload = encode_steam_bridge_join_ready(
            SteamBridgeJoinReady{local_steam_id_, persona_name_, std::move(*lobby)}, error);
        if (!payload.has_value())
            throw std::runtime_error{"Joined lobby readiness failed bridge encoding"};
        pipe_.send(SteamBridgeMessageType::join_ready, *payload);
    }

    void publish_metadata(const SteamBridgeHostConfig &config, bool include_creation_date)
    {
        require_metadata(*native_, lobby_id_, "Name", config.server_name);
        require_metadata(*native_, lobby_id_, "Protected",
                         config.password_protected ? "True" : "False");
        require_metadata(*native_, lobby_id_, "JoinPolicy",
                         std::to_string(static_cast<std::uint8_t>(config.join_policy)));
        if (include_creation_date)
            require_metadata(*native_, lobby_id_, "Date", creation_date());
        for (std::size_t index{}; index < config.session_properties.size(); ++index)
        {
            if (config.session_properties[index].has_value())
                require_metadata(*native_, lobby_id_, std::to_string(index).c_str(),
                                 std::to_string(*config.session_properties[index]));
        }
        if (!config.extension_metadata.empty())
            require_metadata(*native_, lobby_id_, "OMZE", encode_hex(config.extension_metadata));
        if (!native_->set_lobby_type(lobby_id_, native_lobby_type(config.join_policy)))
            throw std::runtime_error{"Steam rejected the requested lobby type"};
    }

    [[nodiscard]] bool dispatch(const SteamBridgeFrame &frame)
    {
        SteamBridgeProtocolError error{};
        switch (frame.type)
        {
        case SteamBridgeMessageType::outbound_wire: {
            if (mode_ != Mode::hosting && mode_ != Mode::joined)
                throw std::runtime_error{"The parent sent gameplay data before joining a lobby"};
            SteamBridgeOutboundWire datagram;
            if (!decode_steam_bridge_outbound_wire(frame.payload, datagram, error))
                throw std::runtime_error{"The parent sent an invalid outbound datagram"};
            if (!members_.contains(datagram.peer))
                throw std::runtime_error{"The parent targeted a peer outside the lobby"};
            const std::int32_t native_send = native_send_type(datagram.delivery);
            if (!native_->send_packet(datagram.peer, datagram.wire, native_send,
                                      datagram.p2p_channel))
                throw std::runtime_error{"Steam rejected an outbound P2P packet"};
            return false;
        }
        case SteamBridgeMessageType::update_host: {
            if (mode_ != Mode::hosting)
                throw std::runtime_error{"The parent tried to update a non-host bridge"};
            SteamBridgeHostConfig update;
            if (!decode_steam_bridge_host_config(frame.payload, update, error) ||
                update.maximum_players != initial_maximum_players_)
                throw std::runtime_error{"The parent sent an invalid host update"};
            publish_metadata(update, false);
            return false;
        }
        case SteamBridgeMessageType::discover_request: {
            if (mode_ != Mode::browsing)
                throw std::runtime_error{"The parent requested discovery outside browser state"};
            SteamBridgeDiscoveryRequest request;
            if (!decode_steam_bridge_discovery_request(frame.payload, request, error))
                throw std::runtime_error{"The parent sent an invalid discovery request"};
            discover(request);
            return false;
        }
        case SteamBridgeMessageType::join_request: {
            if (mode_ != Mode::browsing)
                throw std::runtime_error{"The parent requested a join outside browser state"};
            SteamBridgeJoinRequest request;
            if (!decode_steam_bridge_join_request(frame.payload, request, error))
                throw std::runtime_error{"The parent sent an invalid join request"};
            join(request);
            return false;
        }
        case SteamBridgeMessageType::heartbeat:
            require_empty(frame);
            pipe_.send(SteamBridgeMessageType::heartbeat);
            return false;
        case SteamBridgeMessageType::stop:
            require_empty(frame);
            cleanup();
            pipe_.send(SteamBridgeMessageType::stopped);
            return true;
        default:
            throw std::runtime_error{"The parent sent a message invalid in the running state"};
        }
    }

    static void require_empty(const SteamBridgeFrame &frame)
    {
        if (!frame.payload.empty())
            throw std::runtime_error{"A control bridge frame contained unexpected payload data"};
    }

    void poll_members(bool force)
    {
        const auto now = Clock::now();
        if (!force && now < next_membership_poll_)
            return;
        next_membership_poll_ = now + std::chrono::milliseconds{100};
        const std::int32_t member_count = native_->lobby_member_count(lobby_id_);
        if (member_count < 0 || member_count > 16)
            throw std::runtime_error{"Steam returned an invalid lobby member count"};
        std::unordered_set<std::uint64_t> current;
        current.reserve(static_cast<std::size_t>(member_count));
        for (std::int32_t index{}; index < member_count; ++index)
        {
            const std::uint64_t member = native_->lobby_member(lobby_id_, index);
            if (member == 0U)
                continue;
            current.insert(member);
            if (member != local_steam_id_)
                static_cast<void>(native_->accept_p2p_session(member));
        }
        for (const std::uint64_t previous : members_)
        {
            if (previous == local_steam_id_ || current.contains(previous))
                continue;
            static_cast<void>(native_->close_p2p_session(previous));
            SteamBridgeProtocolError error{};
            const auto payload = encode_steam_bridge_peer(previous, error);
            if (!payload.has_value())
                throw std::runtime_error{"A departed lobby member had an invalid Steam ID"};
            pipe_.send(SteamBridgeMessageType::peer_left, *payload);
        }
        members_ = std::move(current);
    }

    void poll_packets()
    {
        std::size_t count{};
        while (count < maximum_packets_per_poll)
        {
            bool found_packet{};
            for (std::uint8_t channel{}; channel <= 1U && count < maximum_packets_per_poll;
                 ++channel)
            {
                std::uint32_t available{};
                if (!native_->packet_available(available, channel))
                    continue;
                found_packet = true;
                ++count;
                if (available == 0U || available > maximum_drain_size)
                    throw std::runtime_error{"Steam advertised an undrainable P2P packet length"};
                std::vector<std::uint8_t> wire(available);
                std::uint32_t received{};
                std::uint64_t remote{};
                const bool read = native_->read_packet(wire, received, remote, channel);
                if (!read || received != available)
                    continue;
                if (!members_.contains(remote) || remote == local_steam_id_)
                {
                    if (remote != 0U)
                        static_cast<void>(native_->close_p2p_session(remote));
                    continue;
                }
                if (available > legacy_steam_maximum_wire_size)
                    continue;
                SteamBridgeProtocolError error{};
                const auto payload = encode_steam_bridge_inbound_wire(
                    SteamBridgeInboundWire{remote, channel, std::move(wire)}, error);
                if (!payload.has_value())
                    throw std::runtime_error{"A bounded native packet failed bridge encoding"};
                pipe_.send(SteamBridgeMessageType::inbound_wire, *payload);
            }
            if (!found_packet)
                break;
        }
    }

    void cleanup() noexcept
    {
        if (native_ != nullptr)
        {
            if (lobby_id_ != 0U)
            {
                for (const std::uint64_t member : members_)
                {
                    if (member != local_steam_id_)
                        static_cast<void>(native_->close_p2p_session(member));
                }
                native_->leave_lobby(lobby_id_);
            }
            members_.clear();
            last_discovered_lobbies_.clear();
            lobby_id_ = 0U;
            local_steam_id_ = 0U;
            persona_name_.clear();
            mode_ = Mode::initial;
            native_.reset();
        }
    }

    PipeEndpoint &pipe_;
    std::filesystem::path steam_api_path_;
    std::unique_ptr<SteamNative> native_;
    std::unordered_set<std::uint64_t> members_;
    std::unordered_set<std::uint64_t> last_discovered_lobbies_;
    std::uint64_t lobby_id_{};
    std::uint64_t local_steam_id_{};
    std::string persona_name_;
    std::string last_discovery_diagnostic_;
    std::uint8_t initial_maximum_players_{};
    Clock::time_point next_membership_poll_{};
    Mode mode_{Mode::initial};
};

[[nodiscard]] int run_self_test()
{
    if (sizeof(void *) != 4U ||
        !valid_pipe_name(
            LR"(\\.\pipe\OpenMinerZ-SteamBridge-00112233-4455-6677-8899-aabbccddeeff)"))
        return 1;
    const std::vector<std::uint8_t> crc_fixture{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (steam_bridge_crc32(crc_fixture) != 0xCBF43926U)
        return 1;
    SteamBridgeProtocolError error{};
    const auto encoded = encode_steam_bridge_frame(SteamBridgeMessageType::hello, 1U, {}, error);
    SteamBridgeFrame decoded;
    if (!encoded.has_value() || !decode_steam_bridge_frame(*encoded, decoded, error) ||
        decoded.type != SteamBridgeMessageType::hello || decoded.sequence != 1U ||
        !decoded.payload.empty())
        return 1;
    std::cout << "OpenMinerZ SteamBridge32 self-test passed\n";
    return 0;
}

[[nodiscard]] int run_protocol_test(PipeEndpoint &pipe)
{
    pipe.send(SteamBridgeMessageType::hello);
    const auto deadline = Clock::now() + std::chrono::seconds{30};
    bool heartbeat_seen{};
    while (pipe.parent_is_alive() && Clock::now() < deadline)
    {
        const auto frame = pipe.receive();
        if (!frame.has_value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            continue;
        }
        if (frame->type == SteamBridgeMessageType::heartbeat && frame->payload.empty() &&
            !heartbeat_seen)
        {
            heartbeat_seen = true;
            pipe.send(SteamBridgeMessageType::heartbeat);
            continue;
        }
        if (frame->type == SteamBridgeMessageType::stop && frame->payload.empty() && heartbeat_seen)
        {
            pipe.send(SteamBridgeMessageType::stopped);
            return 0;
        }
        throw std::runtime_error{"The protocol test received an invalid command sequence"};
    }
    throw std::runtime_error{"The protocol test timed out"};
}

} // namespace

int wmain(int count, wchar_t **values)
{
    try
    {
        const Arguments arguments = parse_arguments(count, values);
        if (arguments.self_test)
            return run_self_test();
        PipeEndpoint pipe{arguments.pipe_name, arguments.parent_process_id};
        if (arguments.protocol_test)
            return run_protocol_test(pipe);
        const std::filesystem::path steam_api_path =
            normalized_library_path(arguments.steam_api_path);
        try
        {
            BridgeRuntime runtime{pipe, steam_api_path};
            runtime.run();
            return 0;
        }
        catch (const std::exception &exception)
        {
            SteamBridgeProtocolError error{};
            std::string detail = exception.what();
            if (detail.size() > 512U)
                detail.resize(512U);
            const auto failure = encode_steam_bridge_failure(
                SteamBridgeFailure{SteamBridgeFailureCode::internal_error, std::move(detail)},
                error);
            if (failure.has_value())
            {
                try
                {
                    pipe.send(SteamBridgeMessageType::failure, *failure);
                }
                catch (...)
                {
                }
            }
            std::cerr << exception.what() << '\n';
            return 1;
        }
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 2;
    }
}
