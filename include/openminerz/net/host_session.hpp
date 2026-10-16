#pragma once

#include "openminerz/net/mod_handshake.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace openminerz::net
{

enum class HostVisibility : std::uint8_t
{
    friends_only,
    invite_only,
    public_lobby,
    lan
};

enum class PlayerVersusPlayer : std::int32_t
{
    off = 0,
    everyone = 1,
    not_friends = 2
};

enum class HostJoinPolicy : std::uint8_t
{
    anyone,
    allow_list,
    invite_only
};

struct HostSessionSettings final
{
    std::string server_name{"OPENMINERZ HOST"};
    std::string server_message{"WELCOME TO THE FRONTIER"};
    std::string password;
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    int maximum_players{8};
    HostVisibility visibility{HostVisibility::friends_only};
    HostJoinPolicy join_policy{HostJoinPolicy::anyone};
    PlayerVersusPlayer player_versus_player{PlayerVersusPlayer::off};
    bool anti_cheat{true};
    bool dedicated_join_safety{true};
    int spawn_protection_radius{8};
    ModManifest mod_manifest;

    [[nodiscard]] bool validate(std::string &error) const noexcept;
};

using SessionProperties = std::array<std::optional<std::int32_t>, 8U>;

// Slot 1 is the original "no longer joinable" flag. An active, joinable game
// advertises zero; it is not a game-started flag.
[[nodiscard]] SessionProperties make_session_properties(const HostSessionSettings &settings,
                                                        bool join_closed, std::int32_t game_mode,
                                                        std::int32_t difficulty,
                                                        bool infinite_resources);

[[nodiscard]] const char *protocol_label(ProtocolVersion protocol) noexcept;
[[nodiscard]] const char *visibility_label(HostVisibility visibility) noexcept;
[[nodiscard]] const char *join_policy_label(HostJoinPolicy policy) noexcept;
[[nodiscard]] const char *pvp_label(PlayerVersusPlayer policy) noexcept;

} // namespace openminerz::net
