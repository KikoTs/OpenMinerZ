#pragma once

// The gameplay service's replication over the mod channel (modding/net.hpp).
//
// Everything travels as one engine-level RPC: mod id "" (the channel's engine
// slot, which belongs to no mod) and message name "gameplay", whose payload is
// the service's own Value message ("t" = its kind; the list is at the top of
// src/modding/gameplay/service.cpp). The channel already guarantees what the
// service relies on: nothing is sent unless the session's manifest carries a
// server/core mod, only peers that completed the extension hello are
// addressed, and every inbound frame is rate limited, bounded and attributed
// to its transport peer before the service sees it.
//
// Wiring (the app, once per session):
//
//   ModNetGameplaySync link{service};
//   link.attach(mod_net_server, mod_net_client);   // either may be null
//   service.loopback().set_remote(&link);
//   // and when a peer is admitted (one path, in this order):
//   const Handle player = service.admit_player(gid, tag, false);
//   mod_net_server->add_peer(peer, player, now);
//   service.remote_player_joined(gid, tag);         // player.joined
//   // when it leaves: remove_peer, then remote_player_left(gid).
//
// The loopback keeps delivering to this process's own client half directly;
// this link only carries what is addressed to other machines.
//
// The session's content (blocks, items, creatures, recipes, stats) reaches
// each client as the gameplay "registry" message, which carries the full
// definitions with their session ids. The channel's generic content registry
// (ModNetServer::set_content_registry) is not used for it: one mechanism.

#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/net.hpp"

namespace openminerz::modding::gameplay
{

inline constexpr std::string_view gameplay_message_name = "gameplay";

class ModNetGameplaySync final : public GameplaySync
{
  public:
    explicit ModNetGameplaySync(GameplayService &service) : service_{service}
    {
    }
    ~ModNetGameplaySync() override;
    ModNetGameplaySync(const ModNetGameplaySync &) = delete;
    ModNetGameplaySync &operator=(const ModNetGameplaySync &) = delete;

    // Registers the inbound handlers on whichever side exists here and keeps
    // the pointers for sending. Call again with nulls before they go away.
    void attach(ModNetServer *server, ModNetClient *client);

    void to_player(std::uint8_t gid, const Value &message, bool reliable) override;
    void to_all(const Value &message, bool reliable) override;
    void to_server(const Value &message) override;

    // Messages the channel refused (no channel yet, too large, ...).
    [[nodiscard]] std::uint64_t dropped() const noexcept
    {
        return dropped_;
    }

  private:
    GameplayService &service_;
    ModNetServer *server_{};
    ModNetClient *client_{};
    std::uint64_t dropped_{};
};

} // namespace openminerz::modding::gameplay
