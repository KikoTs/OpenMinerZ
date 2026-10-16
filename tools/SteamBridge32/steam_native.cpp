#include "steam_native.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace openminerz::steam_bridge
{
namespace
{

template <typename Function> Function require_export(HMODULE library, const char *name)
{
    const FARPROC address = GetProcAddress(library, name);
    if (address == nullptr)
        throw std::runtime_error{std::string{"Missing legacy Steam export: "} + name};
    return reinterpret_cast<Function>(address);
}

void *require_interface(void *interface_pointer, const char *name)
{
    if (interface_pointer == nullptr)
        throw std::runtime_error{std::string{"Legacy Steam did not expose "} + name};
    return interface_pointer;
}

using NoArgumentInterface = void *(__cdecl *)();
using GetAppId = std::uint32_t(__thiscall *)(void *);
using NoArgumentBoolean = std::uint8_t(__thiscall *)(void *);
using GetSteamId = void *(__thiscall *)(void *, std::uint64_t *);
using NoArgumentString = const char *(__thiscall *)(void *);
using CreateLobby = std::uint64_t(__thiscall *)(void *, std::int32_t, std::int32_t);
using AddLobbyStringFilter = void(__thiscall *)(void *, const char *, const char *, std::int32_t);
using AddLobbyIntegerFilter = void(__thiscall *)(void *, std::int32_t);
using RequestLobbyList = std::uint64_t(__thiscall *)(void *);
using IsApiCallCompleted = std::uint8_t(__thiscall *)(void *, std::uint64_t, std::uint8_t *);
using GetApiCallResult = std::uint8_t(__thiscall *)(void *, std::uint64_t, void *, std::int32_t,
                                                    std::int32_t, std::uint8_t *);
using GetLobbyByIndex = void *(__thiscall *)(void *, std::uint64_t *, std::int32_t);
using RequestLobbyData = std::uint8_t(__thiscall *)(void *, std::uint64_t);
using GetLobbyData = const char *(__thiscall *)(void *, std::uint64_t, const char *);
using GetLobbyMemberLimit = std::int32_t(__thiscall *)(void *, std::uint64_t);
using GetLobbyOwner = void *(__thiscall *)(void *, std::uint64_t *, std::uint64_t);
using JoinLobby = std::uint64_t(__thiscall *)(void *, std::uint64_t);
using GetLobbyMemberCount = std::int32_t(__thiscall *)(void *, std::uint64_t);
using GetLobbyMember = void *(__thiscall *)(void *, std::uint64_t *, std::uint64_t, std::int32_t);
using SetLobbyData = std::uint8_t(__thiscall *)(void *, std::uint64_t, const char *, const char *);
using SetLobbyType = std::uint8_t(__thiscall *)(void *, std::uint64_t, std::int32_t);
using LeaveLobby = void(__thiscall *)(void *, std::uint64_t);
using GetFriendCount = std::int32_t(__thiscall *)(void *, std::int32_t);
using GetFriendByIndex = void *(__thiscall *)(void *, std::uint64_t *, std::int32_t, std::int32_t);
using GetFriendRelationship = std::int32_t(__thiscall *)(void *, std::uint64_t);
using GetFriendGamePlayed = std::uint8_t(__thiscall *)(void *, std::uint64_t, void *);
using GetFriendPersonaName = const char *(__thiscall *)(void *, std::uint64_t);
using PeerBoolean = std::uint8_t(__thiscall *)(void *, std::uint64_t);
using PacketAvailable = std::uint8_t(__thiscall *)(void *, std::uint32_t *, std::int32_t);
using ReadPacket = std::uint8_t(__thiscall *)(void *, void *, std::uint32_t, std::uint32_t *,
                                              std::uint64_t *, std::int32_t);
using SendPacket = std::uint8_t(__thiscall *)(void *, std::uint64_t, const void *, std::uint32_t,
                                              std::int32_t, std::int32_t);

constexpr std::int32_t lobby_created_callback = 513;
constexpr std::int32_t lobby_created_result_size = 16;
constexpr std::int32_t lobby_match_list_callback = 510;
constexpr std::int32_t lobby_match_list_result_size = 4;
constexpr std::int32_t lobby_enter_callback = 504;
constexpr std::int32_t lobby_enter_result_size = 20;
constexpr std::size_t set_lobby_type_vtable_index = 33U;

bool is_x86_pe32_library(const std::filesystem::path &path)
{
    std::ifstream stream{path, std::ios::binary};
    IMAGE_DOS_HEADER dos{};
    stream.read(reinterpret_cast<char *>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return false;
    stream.seekg(dos.e_lfanew, std::ios::beg);
    DWORD signature{};
    IMAGE_FILE_HEADER file{};
    WORD optional_magic{};
    stream.read(reinterpret_cast<char *>(&signature), sizeof(signature));
    stream.read(reinterpret_cast<char *>(&file), sizeof(file));
    stream.read(reinterpret_cast<char *>(&optional_magic), sizeof(optional_magic));
    return stream && signature == IMAGE_NT_SIGNATURE && file.Machine == IMAGE_FILE_MACHINE_I386 &&
           (file.Characteristics & IMAGE_FILE_DLL) != 0U &&
           optional_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
}

} // namespace

SteamNative::SteamNative(const std::filesystem::path &library_path)
{
    if (!is_x86_pe32_library(library_path))
        throw std::runtime_error{"The legacy Steam library is not a Windows x86 binary"};
    if (!SetEnvironmentVariableW(L"SteamAppId", L"253430") ||
        !SetEnvironmentVariableW(L"SteamGameId", L"253430"))
        throw std::runtime_error{"The helper could not establish the CastleMiner Z app context"};

    library_ = LoadLibraryExW(library_path.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library_ == nullptr)
        throw std::runtime_error{"The legacy Steam library could not be loaded"};

    try
    {
        const auto initialize =
            require_export<std::uint8_t(__cdecl *)()>(library_, "SteamAPI_Init");
        shutdown_ = require_export<void(__cdecl *)()>(library_, "SteamAPI_Shutdown");
        run_callbacks_ = require_export<void(__cdecl *)()>(library_, "SteamAPI_RunCallbacks");
        const auto steam_user = require_export<NoArgumentInterface>(library_, "SteamUser");
        const auto steam_friends = require_export<NoArgumentInterface>(library_, "SteamFriends");
        const auto steam_matchmaking =
            require_export<NoArgumentInterface>(library_, "SteamMatchmaking");
        const auto steam_networking =
            require_export<NoArgumentInterface>(library_, "SteamNetworking");
        const auto steam_utils = require_export<NoArgumentInterface>(library_, "SteamUtils");

        if (initialize() == 0U)
            throw std::runtime_error{"SteamAPI_Init returned false"};
        initialized_ = true;
        user_ = require_interface(steam_user(), "SteamUser");
        friends_ = require_interface(steam_friends(), "SteamFriends");
        matchmaking_ = require_interface(steam_matchmaking(), "SteamMatchmaking");
        networking_ = require_interface(steam_networking(), "SteamNetworking");
        utils_ = require_interface(steam_utils(), "SteamUtils");
    }
    catch (...)
    {
        if (initialized_ && shutdown_ != nullptr)
            shutdown_();
        FreeLibrary(library_);
        library_ = nullptr;
        initialized_ = false;
        throw;
    }
}

SteamNative::~SteamNative()
{
    if (initialized_ && shutdown_ != nullptr)
        shutdown_();
    if (library_ != nullptr)
        FreeLibrary(library_);
}

std::uint32_t SteamNative::app_id() const
{
    return vtable_method<GetAppId>(utils_, 9U)(utils_);
}

bool SteamNative::is_logged_on() const
{
    return vtable_method<NoArgumentBoolean>(user_, 1U)(user_) != 0U;
}

std::uint64_t SteamNative::local_steam_id() const
{
    std::uint64_t value{};
    static_cast<void>(vtable_method<GetSteamId>(user_, 2U)(user_, &value));
    return value;
}

std::string SteamNative::persona_name() const
{
    const char *value = vtable_method<NoArgumentString>(friends_, 0U)(friends_);
    if (value == nullptr)
        throw std::runtime_error{"Steam returned a null persona name"};
    const std::size_t length = strnlen_s(value, 129U);
    if (length == 0U || length > 128U)
        throw std::runtime_error{"Steam returned an invalid persona name"};
    return {value, length};
}

std::uint64_t SteamNative::create_public_lobby(std::int32_t maximum_members) const
{
    return vtable_method<CreateLobby>(matchmaking_, 13U)(matchmaking_, 2, maximum_members);
}

void SteamNative::add_lobby_string_filter(const char *key, const char *value) const
{
    vtable_method<AddLobbyStringFilter>(matchmaking_, 5U)(matchmaking_, key, value, 0);
}

void SteamNative::add_lobby_distance_filter(std::int32_t distance) const
{
    vtable_method<AddLobbyIntegerFilter>(matchmaking_, 9U)(matchmaking_, distance);
}

void SteamNative::add_lobby_result_count_filter(std::int32_t maximum_results) const
{
    vtable_method<AddLobbyIntegerFilter>(matchmaking_, 10U)(matchmaking_, maximum_results);
}

std::uint64_t SteamNative::request_lobby_list() const
{
    return vtable_method<RequestLobbyList>(matchmaking_, 4U)(matchmaking_);
}

bool SteamNative::get_lobby_match_count(std::uint64_t api_call, std::uint32_t &count,
                                        bool &failed) const
{
    std::array<std::uint8_t, lobby_match_list_result_size> buffer{};
    std::uint8_t native_failed{};
    const bool succeeded = vtable_method<GetApiCallResult>(utils_, 13U)(
                               utils_, api_call, buffer.data(), lobby_match_list_result_size,
                               lobby_match_list_callback, &native_failed) != 0U;
    failed = native_failed != 0U;
    if (succeeded)
        std::memcpy(&count, buffer.data(), sizeof(count));
    return succeeded;
}

std::uint64_t SteamNative::lobby_by_index(std::int32_t index) const
{
    std::uint64_t value{};
    static_cast<void>(
        vtable_method<GetLobbyByIndex>(matchmaking_, 12U)(matchmaking_, &value, index));
    return value;
}

bool SteamNative::request_lobby_data(std::uint64_t lobby_id) const
{
    return vtable_method<RequestLobbyData>(matchmaking_, 28U)(matchmaking_, lobby_id) != 0U;
}

std::string SteamNative::lobby_data(std::uint64_t lobby_id, const char *key,
                                    std::size_t maximum_bytes) const
{
    const char *value = vtable_method<GetLobbyData>(matchmaking_, 19U)(matchmaking_, lobby_id, key);
    if (value == nullptr)
        return {};
    const std::size_t length = strnlen_s(value, maximum_bytes + 1U);
    if (length > maximum_bytes)
        throw std::runtime_error{"Steam returned oversized lobby metadata"};
    return {value, length};
}

std::int32_t SteamNative::lobby_member_limit(std::uint64_t lobby_id) const
{
    return vtable_method<GetLobbyMemberLimit>(matchmaking_, 32U)(matchmaking_, lobby_id);
}

std::uint64_t SteamNative::lobby_owner(std::uint64_t lobby_id) const
{
    std::uint64_t value{};
    static_cast<void>(
        vtable_method<GetLobbyOwner>(matchmaking_, 35U)(matchmaking_, &value, lobby_id));
    return value;
}

std::uint64_t SteamNative::join_lobby(std::uint64_t lobby_id) const
{
    return vtable_method<JoinLobby>(matchmaking_, 14U)(matchmaking_, lobby_id);
}

bool SteamNative::get_lobby_enter_result(std::uint64_t api_call, std::uint64_t &lobby_id,
                                         std::uint32_t &response, bool &failed) const
{
    std::array<std::uint8_t, lobby_enter_result_size> buffer{};
    std::uint8_t native_failed{};
    const bool succeeded = vtable_method<GetApiCallResult>(utils_, 13U)(
                               utils_, api_call, buffer.data(), lobby_enter_result_size,
                               lobby_enter_callback, &native_failed) != 0U;
    failed = native_failed != 0U;
    if (!succeeded)
        return false;
    std::memcpy(&lobby_id, buffer.data(), sizeof(lobby_id));
    std::memcpy(&response, buffer.data() + 16U, sizeof(response));
    return true;
}

bool SteamNative::is_api_call_completed(std::uint64_t api_call, bool &failed) const
{
    std::uint8_t native_failed{};
    const bool completed =
        vtable_method<IsApiCallCompleted>(utils_, 11U)(utils_, api_call, &native_failed) != 0U;
    failed = native_failed != 0U;
    return completed;
}

bool SteamNative::get_lobby_created_result(std::uint64_t api_call, std::int32_t &result,
                                           std::uint64_t &lobby_id, bool &failed) const
{
    std::array<std::uint8_t, lobby_created_result_size> buffer{};
    std::uint8_t native_failed{};
    const bool succeeded = vtable_method<GetApiCallResult>(utils_, 13U)(
                               utils_, api_call, buffer.data(), lobby_created_result_size,
                               lobby_created_callback, &native_failed) != 0U;
    failed = native_failed != 0U;
    if (!succeeded)
        return false;
    std::memcpy(&result, buffer.data(), sizeof(result));
    std::memcpy(&lobby_id, buffer.data() + 8U, sizeof(lobby_id));
    return true;
}

void SteamNative::run_callbacks() const
{
    run_callbacks_();
}

std::int32_t SteamNative::lobby_member_count(std::uint64_t lobby_id) const
{
    return vtable_method<GetLobbyMemberCount>(matchmaking_, 17U)(matchmaking_, lobby_id);
}

std::uint64_t SteamNative::lobby_member(std::uint64_t lobby_id, std::int32_t member_index) const
{
    std::uint64_t value{};
    static_cast<void>(vtable_method<GetLobbyMember>(matchmaking_, 18U)(matchmaking_, &value,
                                                                       lobby_id, member_index));
    return value;
}

bool SteamNative::set_lobby_data(std::uint64_t lobby_id, const char *key, const char *value) const
{
    return vtable_method<SetLobbyData>(matchmaking_, 20U)(matchmaking_, lobby_id, key, value) != 0U;
}

bool SteamNative::set_lobby_type(std::uint64_t lobby_id, std::int32_t lobby_type) const
{
    return vtable_method<SetLobbyType>(matchmaking_, set_lobby_type_vtable_index)(
               matchmaking_, lobby_id, lobby_type) != 0U;
}

void SteamNative::leave_lobby(std::uint64_t lobby_id) const
{
    vtable_method<LeaveLobby>(matchmaking_, 15U)(matchmaking_, lobby_id);
}

std::int32_t SteamNative::friend_count(std::int32_t flags) const
{
    return vtable_method<GetFriendCount>(friends_, 3U)(friends_, flags);
}

std::uint64_t SteamNative::friend_by_index(std::int32_t index, std::int32_t flags) const
{
    std::uint64_t value{};
    static_cast<void>(
        vtable_method<GetFriendByIndex>(friends_, 4U)(friends_, &value, index, flags));
    return value;
}

std::int32_t SteamNative::friend_relationship(std::uint64_t friend_id) const
{
    return vtable_method<GetFriendRelationship>(friends_, 5U)(friends_, friend_id);
}

std::uint64_t SteamNative::friend_game_lobby(std::uint64_t friend_id) const
{
    std::array<std::uint8_t, 24U> game_info{};
    if (vtable_method<GetFriendGamePlayed>(friends_, 8U)(friends_, friend_id, game_info.data()) ==
        0U)
        return 0U;
    // FriendGameInfo_t starts with the CGameID whose low 24 bits are the app id.
    // A friend's lobby in another game can never be read through this app, and
    // waiting for its data stalls every discovery for the full deadline.
    std::uint64_t game_id{};
    std::memcpy(&game_id, game_info.data(), sizeof(game_id));
    if (static_cast<std::uint32_t>(game_id & 0x00FF'FFFFU) != app_id())
        return 0U;
    std::uint64_t lobby{};
    std::memcpy(&lobby, game_info.data() + 16U, sizeof(lobby));
    return lobby;
}

std::string SteamNative::friend_persona_name(std::uint64_t friend_id) const
{
    const char *value = vtable_method<GetFriendPersonaName>(friends_, 7U)(friends_, friend_id);
    if (value == nullptr)
        return {};
    const std::size_t length = strnlen_s(value, 129U);
    if (length > 128U)
        throw std::runtime_error{"Steam returned an oversized friend persona name"};
    return {value, length};
}

bool SteamNative::accept_p2p_session(std::uint64_t remote_steam_id) const
{
    return vtable_method<PeerBoolean>(networking_, 3U)(networking_, remote_steam_id) != 0U;
}

bool SteamNative::close_p2p_session(std::uint64_t remote_steam_id) const
{
    return vtable_method<PeerBoolean>(networking_, 4U)(networking_, remote_steam_id) != 0U;
}

bool SteamNative::packet_available(std::uint32_t &available, std::int32_t channel) const
{
    return vtable_method<PacketAvailable>(networking_, 1U)(networking_, &available, channel) != 0U;
}

bool SteamNative::read_packet(std::span<std::uint8_t> destination, std::uint32_t &received,
                              std::uint64_t &remote_steam_id, std::int32_t channel) const
{
    if (destination.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    return vtable_method<ReadPacket>(networking_, 2U)(
               networking_, destination.data(), static_cast<std::uint32_t>(destination.size()),
               &received, &remote_steam_id, channel) != 0U;
}

bool SteamNative::send_packet(std::uint64_t remote_steam_id, std::span<const std::uint8_t> payload,
                              std::int32_t send_type, std::int32_t channel) const
{
    if (payload.empty() || payload.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    return vtable_method<SendPacket>(networking_, 0U)(networking_, remote_steam_id, payload.data(),
                                                      static_cast<std::uint32_t>(payload.size()),
                                                      send_type, channel) != 0U;
}

} // namespace openminerz::steam_bridge
