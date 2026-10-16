#pragma once

#include "openminerz/settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Retail is the default; everything retail does not have is opt-in.
//
// A stock Castle Miner Z host validates almost nothing and relays whatever a
// client sends, and every peer simulates a great deal locally. A place where
// this client BEHAVES differently from the shipped game therefore
// desynchronises a session with real Steam players even when every message is
// byte-perfect. This header is the one gate for such behaviour:
//
//   * `Enhancement` names each gameplay feature the shipped game does not
//     have. Presentation (renderer, HUD, fonts, smoothing) does not belong
//     here: it changes neither simulation state nor anything sent on the wire.
//   * The player's choice is stored in `Settings`, default OFF for every one.
//   * `enhancement_active` answers the only question gameplay code may ask:
//     "is this enhancement in effect right now?".
//
// The full register of differences from retail, matched or not, is
// docs/retail-parity.md. Add a row there with every enhancement added here.
namespace openminerz
{

enum class Enhancement : std::uint8_t
{
    // The Lagoon band's sea: a water plane at world::water_level with swim
    // physics, oxygen, drowning, the underwater tint and muffled ambience.
    // Retail (1.6.3.8 and 1.9.9.8) ships that code dead: BlockTerrain.IsWaterWorld
    // is only ever assigned false and no WaterPlane is ever constructed, so a
    // stock player walks on the dry lagoon floor.
    lagoon_water,
    // Surface lava (block 12) stops blocking movement, so players, enemies,
    // pickups and grenades sink through it. Retail: BlockType SurfaceLava is
    // `blockPlayer: true`, a solid block you stand on; only DeepLava (13) is
    // pass-through, and lava damage comes from the global plane below Y=-60.
    sinking_lava,
    // The flight this project used to give every unrestricted world: F toggles
    // it, the heading alone steers it at the "Creative flight speed" setting,
    // Space rises and Shift sinks. Retail (executed, SimulationParityProbe
    // fly_*): 1.6.3.8 binds a FlyMode key that nothing reads, so no world of
    // that generation can fly at all; 1.9.9.8 toggles it in Creative only and
    // flies at a fixed 24 along the CAMERA (48 sprinting), +15 up while Jump
    // is held. A stock host believes any position it is sent, but every stock
    // player then sees a body moving in a way their game cannot produce.
    free_flight,
};

inline constexpr std::size_t enhancement_count = 3U;

struct EnhancementInfo final
{
    Enhancement id{};
    // Key in the settings file.
    std::string_view key;
    // Shown verbatim on the settings page.
    std::string_view label;
    std::string_view description;
    // Where the player's stored choice lives.
    bool Settings::*enabled{};
};

inline constexpr std::array<EnhancementInfo, enhancement_count> enhancement_table{{
    {Enhancement::lagoon_water, "enhancement_lagoon_water", "Lagoon water",
     "Fills the Lagoon with a sea you can swim in, with air to hold and drowning. The original "
     "Lagoon is dry land. Single-player only.",
     &Settings::enhancement_lagoon_water},
    {Enhancement::sinking_lava, "enhancement_sinking_lava", "Sinking lava",
     "Lets you, creatures and dropped items sink through surface lava. In the original it is a "
     "solid block you can stand on. Single-player only.",
     &Settings::enhancement_sinking_lava},
    {Enhancement::free_flight, "enhancement_free_flight", "Free flight",
     "F flies in Creative and infinite-resource worlds at your chosen flight speed. The "
     "original Classic game cannot fly at all. Single-player only.",
     &Settings::enhancement_free_flight},
}};

[[nodiscard]] constexpr std::span<const EnhancementInfo> all_enhancements() noexcept
{
    return enhancement_table;
}

[[nodiscard]] constexpr const EnhancementInfo &enhancement_info(Enhancement id) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    return enhancement_table[index < enhancement_count ? index : 0U];
}

// The player's stored choice. NOT whether the enhancement is in effect: use
// enhancement_active for anything that changes behaviour.
[[nodiscard]] constexpr bool enhancement_enabled(const Settings &settings, Enhancement id) noexcept
{
    return settings.*(enhancement_info(id).enabled);
}

constexpr void set_enhancement_enabled(Settings &settings, Enhancement id, bool enabled) noexcept
{
    settings.*(enhancement_info(id).enabled) = enabled;
}

[[nodiscard]] constexpr bool any_enhancement_enabled(const Settings &settings) noexcept
{
    for (const EnhancementInfo &info : enhancement_table)
        if (settings.*(info.enabled))
            return true;
    return false;
}

// What the gate needs to know about the session this process is in.
struct ParitySession final
{
    enum class Role : std::uint8_t
    {
        // No network session at all. The offline multiplayer-rendering
        // diagnostic counts as offline: nobody else simulates it.
        offline,
        // This process hosts the session.
        host,
        // This process joined somebody else's session.
        guest,
    };

    Role role{Role::offline};
    // The only positive capability signal that exists: a completed OpenMinerZ
    // extension hello (net::ExtensionMessage). Unknown counts as stock.
    //   host:  LegacySteamRuntimeHost::all_peers_extension_capable()
    //          (an empty session is trivially capable);
    //   guest: LegacySteamRuntimeClient::host_extension_capable().
    bool peers_confirmed_openminerz{};

    [[nodiscard]] static constexpr ParitySession offline() noexcept
    {
        return {};
    }
    [[nodiscard]] static constexpr ParitySession hosting(bool all_peers_extension_capable) noexcept
    {
        return {Role::host, all_peers_extension_capable};
    }
    [[nodiscard]] static constexpr ParitySession joined(bool host_extension_capable) noexcept
    {
        return {Role::guest, host_extension_capable};
    }

    [[nodiscard]] constexpr bool online() const noexcept
    {
        return role != Role::offline;
    }
};

// Enhancements are OFFLINE-ONLY today.
//
// Knowing that every peer is an OpenMinerZ client is necessary but not
// sufficient: the peers would also have to agree on the SAME enhancement set,
// and nothing carries that. The private extension hello exchanges a mod
// manifest that the host compares once at admission; it has no field for an
// enhancement set, it is never renegotiated when a setting changes or a peer
// joins, and a guest only ever learns about its HOST, never the other guests.
// Until a private message does all of that, an online session gets retail
// behaviour whatever its peers are. Adding a message to the STOCK protocol for
// it is not an option. Flip this in the same change that adds the negotiation.
inline constexpr bool enhancement_sets_are_negotiated = false;

// The session half of the gate. `negotiated` exists so the capability rule
// that applies once negotiation lands can be pinned by tests today; gameplay
// code must use the one-argument form.
[[nodiscard]] constexpr bool session_permits_enhancements(ParitySession session,
                                                          bool negotiated) noexcept
{
    if (!session.online())
        return true;
    // A session containing a stock peer, or a peer whose capability is not
    // positively confirmed, always plays by retail's rules.
    return negotiated && session.peers_confirmed_openminerz;
}

[[nodiscard]] constexpr bool session_permits_enhancements(ParitySession session) noexcept
{
    return session_permits_enhancements(session, enhancement_sets_are_negotiated);
}

// THE question: is this enhancement in effect right now? It is re-asked every
// tick, so an enhancement switches off the moment a session starts.
[[nodiscard]] constexpr bool enhancement_active(const Settings &settings, Enhancement id,
                                                ParitySession session) noexcept
{
    return enhancement_enabled(settings, id) && session_permits_enhancements(session);
}

// Why an enhancement the player switched on is not in effect, for the
// settings page. Empty while the session permits enhancements.
[[nodiscard]] constexpr std::string_view enhancement_blocked_reason(ParitySession session) noexcept
{
    if (session_permits_enhancements(session))
        return {};
    return "Off while you are online, so your game matches everyone else's.";
}

} // namespace openminerz
