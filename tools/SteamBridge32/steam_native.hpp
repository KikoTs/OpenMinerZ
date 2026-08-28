#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

namespace openminerz::steam_bridge
{

class SteamNative final
{
  public:
    explicit SteamNative(const std::filesystem::path &library_path);
    ~SteamNative();

    SteamNative(const SteamNative &) = delete;
    SteamNative &operator=(const SteamNative &) = delete;

    [[nodiscard]] std::uint32_t app_id() const;
    [[nodiscard]] bool is_logged_on() const;
    [[nodiscard]] std::uint64_t local_steam_id() const;
    [[nodiscard]] std::string persona_name() const;

    [[nodiscard]] std::uint64_t create_public_lobby(std::int32_t maximum_members) const;
    void add_lobby_string_filter(const char *key, const char *value) const;
    void add_lobby_distance_filter(std::int32_t distance) const;
    void add_lobby_result_count_filter(std::int32_t maximum_results) const;
    [[nodiscard]] std::uint64_t request_lobby_list() const;
    [[nodiscard]] bool get_lobby_match_count(std::uint64_t api_call, std::uint32_t &count,
                                             bool &failed) const;
    [[nodiscard]] std::uint64_t lobby_by_index(std::int32_t index) const;
    [[nodiscard]] bool request_lobby_data(std::uint64_t lobby_id) const;
    [[nodiscard]] std::string lobby_data(std::uint64_t lobby_id, const char *key,
                                         std::size_t maximum_bytes) const;
    [[nodiscard]] std::int32_t lobby_member_limit(std::uint64_t lobby_id) const;
    [[nodiscard]] std::uint64_t lobby_owner(std::uint64_t lobby_id) const;
    [[nodiscard]] std::uint64_t join_lobby(std::uint64_t lobby_id) const;
    [[nodiscard]] bool get_lobby_enter_result(std::uint64_t api_call, std::uint64_t &lobby_id,
                                              std::uint32_t &response, bool &failed) const;
    [[nodiscard]] bool is_api_call_completed(std::uint64_t api_call, bool &failed) const;
    [[nodiscard]] bool get_lobby_created_result(std::uint64_t api_call, std::int32_t &result,
                                                std::uint64_t &lobby_id, bool &failed) const;
    void run_callbacks() const;

    [[nodiscard]] std::int32_t lobby_member_count(std::uint64_t lobby_id) const;
    [[nodiscard]] std::uint64_t lobby_member(std::uint64_t lobby_id,
                                             std::int32_t member_index) const;
    [[nodiscard]] bool set_lobby_data(std::uint64_t lobby_id, const char *key,
                                      const char *value) const;
    [[nodiscard]] bool set_lobby_type(std::uint64_t lobby_id, std::int32_t lobby_type) const;
    void leave_lobby(std::uint64_t lobby_id) const;
    [[nodiscard]] std::int32_t friend_count(std::int32_t flags) const;
    [[nodiscard]] std::uint64_t friend_by_index(std::int32_t index, std::int32_t flags) const;
    [[nodiscard]] std::int32_t friend_relationship(std::uint64_t friend_id) const;
    [[nodiscard]] std::uint64_t friend_game_lobby(std::uint64_t friend_id) const;
    [[nodiscard]] std::string friend_persona_name(std::uint64_t friend_id) const;

    [[nodiscard]] bool accept_p2p_session(std::uint64_t remote_steam_id) const;
    [[nodiscard]] bool close_p2p_session(std::uint64_t remote_steam_id) const;
    [[nodiscard]] bool packet_available(std::uint32_t &available, std::int32_t channel) const;
    [[nodiscard]] bool read_packet(std::span<std::uint8_t> destination, std::uint32_t &received,
                                   std::uint64_t &remote_steam_id, std::int32_t channel) const;
    [[nodiscard]] bool send_packet(std::uint64_t remote_steam_id,
                                   std::span<const std::uint8_t> payload, std::int32_t send_type,
                                   std::int32_t channel) const;

  private:
    template <typename Function>
    [[nodiscard]] static Function vtable_method(void *instance, std::size_t index)
    {
        if (instance == nullptr)
            throw std::runtime_error{"Steam interface pointer is null"};
        auto **vtable = *reinterpret_cast<void ***>(instance);
        if (vtable == nullptr || vtable[index] == nullptr)
            throw std::runtime_error{"Steam interface vtable entry is null"};
        return reinterpret_cast<Function>(vtable[index]);
    }

    HMODULE library_{};
    void *user_{};
    void *friends_{};
    void *matchmaking_{};
    void *networking_{};
    void *utils_{};
    void(__cdecl *shutdown_)(){};
    void(__cdecl *run_callbacks_)(){};
    bool initialized_{};
};

} // namespace openminerz::steam_bridge
