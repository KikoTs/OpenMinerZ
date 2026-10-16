#include "openminerz/net/host_session.hpp"

#include <algorithm>
#include <stdexcept>

namespace openminerz::net
{
namespace
{

bool printable_text(std::string_view value) noexcept
{
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
    });
}

int maximum_game_mode(ProtocolVersion protocol) noexcept
{
    return protocol == ProtocolVersion::modern_v4 ? 5 : 3;
}

} // namespace

bool HostSessionSettings::validate(std::string &error) const noexcept
{
    if (server_name.empty() || server_name.size() > 128U || !printable_text(server_name))
    {
        error = "Server name must contain 1-128 printable bytes.";
        return false;
    }
    if (server_message.empty() || server_message.size() > 128U || !printable_text(server_message))
    {
        error = "Server message must contain 1-128 printable bytes.";
        return false;
    }
    if (password.size() > 256U || !printable_text(password))
    {
        error = "Password must contain at most 256 printable bytes.";
        return false;
    }
    if (maximum_players < 1 || maximum_players > 16)
    {
        error = "Maximum players must be between 1 and 16.";
        return false;
    }
    if (spawn_protection_radius < 0 || spawn_protection_radius > 128)
    {
        error = "Spawn protection must be between 0 and 128 blocks.";
        return false;
    }
    if (protocol != ProtocolVersion::classic_v3 && protocol != ProtocolVersion::modern_v4)
    {
        error = "Unsupported CastleMiner Z protocol profile.";
        return false;
    }
    if (visibility != HostVisibility::friends_only && visibility != HostVisibility::invite_only &&
        visibility != HostVisibility::public_lobby && visibility != HostVisibility::lan)
    {
        error = "Unsupported host visibility.";
        return false;
    }
    if (join_policy != HostJoinPolicy::anyone && join_policy != HostJoinPolicy::allow_list &&
        join_policy != HostJoinPolicy::invite_only)
    {
        error = "Unsupported join policy.";
        return false;
    }
    if (player_versus_player != PlayerVersusPlayer::off &&
        player_versus_player != PlayerVersusPlayer::everyone &&
        player_versus_player != PlayerVersusPlayer::not_friends)
    {
        error = "Unsupported player-versus-player policy.";
        return false;
    }
    if (mod_manifest.protocol != protocol)
    {
        error = "Host protocol and mod-manifest protocol must match.";
        return false;
    }
    if (!validate_mod_manifest(mod_manifest, error))
        return false;
    error.clear();
    return true;
}

SessionProperties make_session_properties(const HostSessionSettings &settings, bool join_closed,
                                          std::int32_t game_mode, std::int32_t difficulty,
                                          bool infinite_resources)
{
    std::string error;
    if (!settings.validate(error))
        throw std::invalid_argument("invalid host settings: " + error);
    if (game_mode < 0 || game_mode > maximum_game_mode(settings.protocol))
        throw std::out_of_range("game mode is unavailable for the selected protocol");
    if (difficulty < 0 || difficulty > 3)
        throw std::out_of_range("difficulty must be between 0 and 3");

    SessionProperties properties{};
    properties[0] = static_cast<std::int32_t>(settings.protocol);
    properties[1] = join_closed ? 1 : 0;
    properties[2] = game_mode;
    properties[3] = difficulty;
    properties[4] = infinite_resources ? 1 : 0;
    properties[5] = static_cast<std::int32_t>(settings.player_versus_player);
    properties[6] = std::nullopt;
    properties[7] = std::nullopt;
    return properties;
}

const char *protocol_label(ProtocolVersion protocol) noexcept
{
    switch (protocol)
    {
    case ProtocolVersion::classic_v3:
        return "Classic";
    case ProtocolVersion::modern_v4:
        return "Modern";
    }
    return "UNKNOWN";
}

const char *visibility_label(HostVisibility visibility) noexcept
{
    switch (visibility)
    {
    case HostVisibility::friends_only:
        return "FRIENDS";
    case HostVisibility::invite_only:
        return "INVITE ONLY";
    case HostVisibility::public_lobby:
        return "PUBLIC";
    case HostVisibility::lan:
        return "LAN";
    }
    return "UNKNOWN";
}

const char *join_policy_label(HostJoinPolicy policy) noexcept
{
    switch (policy)
    {
    case HostJoinPolicy::anyone:
        return "ANYONE";
    case HostJoinPolicy::allow_list:
        return "ALLOW LIST";
    case HostJoinPolicy::invite_only:
        return "INVITED";
    }
    return "UNKNOWN";
}

const char *pvp_label(PlayerVersusPlayer policy) noexcept
{
    switch (policy)
    {
    case PlayerVersusPlayer::off:
        return "OFF";
    case PlayerVersusPlayer::everyone:
        return "EVERYONE";
    case PlayerVersusPlayer::not_friends:
        return "NOT FRIENDS";
    }
    return "UNKNOWN";
}

} // namespace openminerz::net
