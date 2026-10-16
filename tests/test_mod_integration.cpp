// Modding integration: the five areas as one system.
//
// A listen host (server VM + client VM in one process, like offline play) and
// joined clients (client VM only), each with the real runtime, gameplay, UI and
// network services, talk over the production LoopbackSession: admission, the
// extension hello, the control channel. One server mod with shared/, server/
// and client/ scripts exercises every area together: one player identity
// across net.*, replicated.*, ui.*(player), players.* and invoke_as; stats,
// replicated state and RPC both ways; an NPC dialogue answered by the client;
// a mod block and a mod creature seen by the client with their identities;
// mod items keeping their identity in crates and network pickups; a client
// leaving and a new one joining.
//
// Then the wire guarantees: a stock (capability-silent) peer is never handed
// an extension frame, and a host without server mods - its whole modding
// stack running a client-scope script mod - produces exactly the stock
// traffic of a host without modding. And InventoryStack::mod_item: no save
// byte or stock packet byte depends on it, and nothing a vanilla session does
// can make it non-zero.

#include "modding_test_harness.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/gameplay_net.hpp"
#include "openminerz/modding/net.hpp"
#include "openminerz/modding/ui.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_channel.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/world_save.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using modding::Handle;
using modding::HandleKind;
using modding::Side;
using modding::Value;
namespace gameplay = openminerz::modding::gameplay;
namespace ui = openminerz::modding::ui;

void expect(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error{std::string{message}};
}

// --------------------------------------------------------------------- fakes

// The game, as each process's gameplay service sees it: a flat world (rock
// below y 1), this process's player, the remote players it knows, crates.
class FakeGame final : public gameplay::GameplayBackend
{
  public:
    std::uint8_t local_gid{};
    std::string local_name{"Host"};
    modding::Vec3 position{0.5, 1.0, 0.5};
    std::vector<gameplay::PlayerSnapshot> remotes;
    std::map<gameplay::BlockPos, std::uint8_t> edits;
    std::map<std::pair<gameplay::BlockPos, std::size_t>, PlayerInventory::Slot> crates;
    std::vector<std::pair<modding::Vec3, gameplay::StackView>> pickups;
    std::vector<std::string> chat_lines;
    PlayerInventory inventory;

    FakeGame()
    {
        inventory.set_profile(InventoryProfile::classic);
        for (PlayerInventory::Slot &slot : inventory.storage())
            slot.reset();
        for (PlayerInventory::Tray &tray : inventory.trays())
            for (PlayerInventory::Slot &slot : tray)
                slot.reset();
    }

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return 777;
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return false;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        return local_gid;
    }
    void players(std::vector<gameplay::PlayerSnapshot> &out) const override
    {
        out.clear();
        out.push_back({local_gid, local_name, true, true, position, 0.0F, 100.0F});
        out.insert(out.end(), remotes.begin(), remotes.end());
    }
    [[nodiscard]] std::optional<gameplay::LocalView> local_view() const override
    {
        gameplay::LocalView view;
        view.eye = {position.x, position.y + 1.62, position.z};
        view.look = {0.0, -1.0, 0.0};
        view.target = gameplay::BlockPos{0, 0, 0};
        view.target_adjacent = gameplay::BlockPos{0, 1, 0};
        return view;
    }
    void local_set_health(float) override
    {
    }
    // Like apply_player_damage: the service is asked first.
    gameplay::GameplayService *service{};
    void local_damage(float amount, modding::Vec3 source, gameplay::DamageCause cause) override
    {
        if (service != nullptr && service->running())
            static_cast<void>(service->local_player_damaging(amount, cause, source));
    }
    void local_teleport(modding::Vec3 feet) override
    {
        position = feet;
    }
    std::uint16_t local_give(gameplay::StackView stack, bool) override
    {
        InventoryStack given{static_cast<InventoryItemId>(stack.item), stack.count};
        given.mod_item = stack.mod_item;
        return inventory.add_stack(given);
    }
    std::uint32_t local_take(std::uint16_t, std::uint32_t, std::uint32_t) override
    {
        return 0U;
    }
    [[nodiscard]] std::uint32_t local_count(std::uint16_t item, std::uint32_t mod_item) const override
    {
        std::uint32_t total{};
        for (const PlayerInventory::Slot &slot : inventory.storage())
            if (slot.has_value() && static_cast<std::uint16_t>(slot->item) == item && slot->mod_item == mod_item)
                total += slot->count;
        for (const PlayerInventory::Tray &tray : inventory.trays())
            for (const PlayerInventory::Slot &slot : tray)
                if (slot.has_value() && static_cast<std::uint16_t>(slot->item) == item &&
                    slot->mod_item == mod_item)
                    total += slot->count;
        return total;
    }
    void local_clear_inventory() override
    {
    }
    [[nodiscard]] std::optional<gameplay::StackView> local_held() const override
    {
        return std::nullopt;
    }
    void local_slots(std::vector<std::pair<std::uint16_t, gameplay::StackView>> &out) const override
    {
        out.clear();
    }
    bool local_tag_slot(std::uint16_t, std::uint16_t, std::uint32_t) override
    {
        return false;
    }
    void local_set_speed(float) override
    {
    }
    [[nodiscard]] std::optional<std::uint8_t> block(gameplay::BlockPos cell) const override
    {
        if (cell.y < -64 || cell.y > 63)
            return std::nullopt;
        const auto found = edits.find(cell);
        return found != edits.end() ? found->second : std::uint8_t{cell.y < 1 ? 6U : 0U};
    }
    bool set_block(gameplay::BlockPos cell, std::uint8_t value) override
    {
        edits[cell] = value;
        return true;
    }
    [[nodiscard]] int biome(int, int) const override
    {
        return 0;
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(gameplay::BlockPos) const override
    {
        return std::array<float, 2>{1.0F, 0.0F};
    }
    bool set_crate_slot(gameplay::BlockPos crate, std::size_t index,
                        std::optional<gameplay::StackView> stack) override
    {
        PlayerInventory::Slot slot;
        if (stack.has_value())
        {
            slot = InventoryStack{static_cast<InventoryItemId>(stack->item), stack->count};
            slot->mod_item = stack->mod_item;
        }
        crates[{crate, index}] = slot;
        return true;
    }
    bool tag_crate_slot(gameplay::BlockPos crate, std::size_t index, std::uint16_t item,
                        std::uint32_t mod_item) override
    {
        const auto found = crates.find({crate, index});
        if (found == crates.end() || !found->second.has_value() ||
            static_cast<std::uint16_t>(found->second->item) != item)
            return false;
        found->second->mod_item = mod_item;
        return true;
    }
    bool spawn_pickup(modding::Vec3 at, gameplay::StackView stack) override
    {
        pickups.emplace_back(at, stack);
        return true;
    }
    [[nodiscard]] double day() const override
    {
        return 0.5;
    }
    void set_time_of_day(float) override
    {
    }
    bool spawn_enemy(int, modding::Vec3) override
    {
        return false;
    }
    void chat(std::optional<std::uint8_t>, std::string_view text) override
    {
        chat_lines.emplace_back(text);
    }
    [[nodiscard]] std::filesystem::path sidecar_directory() const override
    {
        return {};
    }
};

class FakeUi final : public ui::UiBackend
{
  public:
    Handle local;

    [[nodiscard]] ui::Style style() const override
    {
        return ui::Style::remastered;
    }
    [[nodiscard]] std::uint32_t load_order(std::string_view) const override
    {
        return 0U;
    }
    [[nodiscard]] bool asset_exists(const ui::AssetRef &) const override
    {
        return false;
    }
    [[nodiscard]] std::optional<std::string> read_mod_text(std::string_view, std::string_view,
                                                           std::size_t) const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> read_local(std::string_view) const override
    {
        return std::nullopt;
    }
    void write_local(std::string_view, std::string_view) override
    {
    }
    [[nodiscard]] bool item_exists(int item) const override
    {
        return item > 0 && item < 300;
    }
    [[nodiscard]] std::string item_name(int item) const override
    {
        return std::format("Item {}", item);
    }
    [[nodiscard]] std::string canonical_key(std::string_view key) const override
    {
        return std::string{key};
    }
    [[nodiscard]] bool offline() const override
    {
        return false;
    }
    [[nodiscard]] Handle local_player() const override
    {
        return local;
    }
    void play(ui::SoundKey) override
    {
    }
};

struct NetObjects final : modding::NetBackend
{
    modding::ModNetServer *server_ptr{};
    modding::ModNetClient *client_ptr{};
    [[nodiscard]] modding::ModNetServer *server() noexcept override
    {
        return server_ptr;
    }
    [[nodiscard]] modding::ModNetClient *client() noexcept override
    {
        return client_ptr;
    }
};

// ------------------------------------------------------------------ the mod

constexpr std::string_view state_module = R"(return {})";

constexpr std::string_view shared_definitions = R"(
blocks.define("moss", { fallback = "rock", name = "Moss", texture = "moss.png" })
items.define("relic", { fallback = "stick", name = "Relic" })
creatures.define("deer", { behaviour = "passive", health = 4, model = "deer.gltf" })
stats.define("thirst", { max = 100, initial = 80, hud = { label = "Thirst" } })
dialogue.define("guide", { title = "Guide", start = "hello", nodes = {
    hello = { speaker = "Guide", text = "Which way?", choices = {
        { text = "Left", action = function(player)
            require("shared/state").action_player = player
        end },
        { text = "Right" },
    } },
} })
)";

constexpr std::string_view server_main = R"(
local state = require("shared/state")
state.joined = {}
state.ready = {}
events.on("player.joined", function(e)
    local p = e.player
    table.insert(state.joined, p)
    -- Everything below names the player by the one handle.
    ui.toast(p, "Welcome", "to the quest")
    stats.set(p, "thirst", 42)
    replicated.set(p, "coins", 7)
    replicated.set("world", "phase", "start")
    net.send_to(p, "hello", { n = #state.joined })
end)
events.on("net.player_ready", function(e) table.insert(state.ready, e.player) end)
events.on("player.left", function(e) state.left = e.player end)
events.on("dialogue.choice", function(e) state.choice_player = e.player end)
net.on("ping", function(player, payload)
    state.ping_from = player
    net.send_to(player, "pong", { n = payload.n + 1 })
end, { n = "integer" })
)";

constexpr std::string_view client_main = R"(
local state = require("shared/state")
net.on("hello", function(payload) state.hello = payload.n end)
net.on("pong", function(payload) state.pong = payload.n end)
replicated.on_change("player", "coins", function(value, old, key, owner)
    state.coins = value
    state.coins_owner = owner
end)
events.on("net.ready", function(e) state.ready_as = e.player end)
)";

modding::ModSource quest_mod()
{
    modding::ModSource source;
    source.id = "quest";
    source.scope = ModScope::server;
    source.files["shared/state.luau"] = std::string{state_module};
    source.files["shared/defs.luau"] = std::string{shared_definitions};
    source.files["server/main.luau"] = std::string{server_main};
    source.files["client/main.luau"] = std::string{client_main};
    return source;
}

ModDigest digest_of(std::uint8_t seed)
{
    ModDigest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index)
        digest[index] = static_cast<std::uint8_t>(seed + index * 5U + 3U);
    return digest;
}

ModManifest quest_manifest()
{
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    manifest.mods.push_back({"quest", "1", digest_of(1U), ModScope::server});
    return manifest;
}

HostSessionSettings settings_for(const ModManifest &manifest)
{
    HostSessionSettings settings;
    settings.server_name = "Integration";
    settings.protocol = ProtocolVersion::classic_v3;
    settings.maximum_players = 8;
    settings.visibility = HostVisibility::public_lobby;
    settings.mod_manifest = manifest;
    return settings;
}

AdmissionStatus admit(LoopbackSession &session, const HostSessionSettings &settings, PeerId peer,
                      const ModManifest &client_manifest, bool extension = true)
{
    PeerAdmissionRequest request;
    request.peer = peer;
    request.properties = make_session_properties(settings, false, 1, 1, false);
    request.advertised_extension = extension;
    const AdmissionDecision decision = session.connect(request);
    if (!extension || !decision.accepted())
        return decision.status;
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 0x5150U + peer;
    hello.manifest = client_manifest;
    static_cast<void>(session.submit_from_peer(peer, encode_extension_message(hello),
                                               MessageDeliveryPolicy::reliable_ordered,
                                               openminerz_control_channel));
    static_cast<void>(session.receive_for_peer(peer)); // the accept/reject reply
    return session.admission().status(peer);
}

// --------------------------------------------------------------- a process

// One machine's modding stack, wired the way the app wires it
// (app_mod_runtime.inc, app_mod_net.inc, app_mod_gameplay.inc, app_mod_ui.inc).
struct Process final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    FakeUi ui_backend;
    std::unique_ptr<gameplay::GameplayService> gameplay;
    std::unique_ptr<ui::UiService> ui;
    NetObjects net;
    std::unique_ptr<modding::ModNetServer> server;
    std::unique_ptr<modding::ModNetClient> client;
    std::unique_ptr<gameplay::ModNetGameplaySync> link;
    std::unique_ptr<ui::ModNetUiChannel> ui_channel;
    double now{};

    Process()
    {
        // register_mod_apis' order: gameplay, (graphics), ui, net.
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        ui = std::make_unique<ui::UiService>(ui_backend);
        ui->register_api(runtime.registry(), runtime);
        modding::register_net_api(runtime.registry(), runtime, net);
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
    }
    ~Process()
    {
        stop();
    }
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // attach_mod_net_objects.
    void attach(const ModManifest &manifest, bool authority)
    {
        if (authority)
            server = std::make_unique<modding::ModNetServer>(manifest);
        client = std::make_unique<modding::ModNetClient>(manifest);
        net.server_ptr = server.get();
        net.client_ptr = client.get();
        modding::connect_net_events(runtime, server.get(), client.get());
        link = std::make_unique<gameplay::ModNetGameplaySync>(*gameplay);
        link->attach(server.get(), server != nullptr ? nullptr : client.get());
        gameplay->loopback().set_remote(link.get());
        ui_channel = std::make_unique<ui::ModNetUiChannel>(*ui, server.get(), client.get(),
                                                           [this] { return now; });
        ui->set_channel(ui_channel.get());
        if (!authority)
            client->add_event_listener([this](const modding::ModNetEvent &event) {
                if (event.kind == modding::ModNetEventKind::welcome && gameplay->client_running())
                    gameplay->client_channel_ready();
            });
    }

    // start_mod_net_session(true) + start_mod_session(mods, true).
    Handle host(const ModManifest &manifest)
    {
        attach(manifest, true);
        const Handle local = gameplay->admit_player(game.local_gid, game.local_name, true);
        expect(server->add_peer(modding::local_mod_peer, local, now), "the host's own player gets a channel");
        publish_directory(local, game.local_gid, game.local_name);
        start({Side::server, Side::client});
        return local;
    }

    // mod_net_join_finished + start_mod_session(mods, false).
    void join(const ModManifest &manifest)
    {
        attach(manifest, false);
        start({Side::client});
    }

    void start(std::initializer_list<Side> sides)
    {
        for (const Side side : sides)
        {
            std::vector<modding::ModSource> mods{quest_mod()};
            expect(runtime.start(side, std::move(mods), {}), "the quest mod's VM starts");
        }
        if (runtime.running(Side::server))
            gameplay->start_server();
        gameplay->start_client();
        const Value world{modding::Map{{"name", "Integration"}, {"seed", 777}, {"mode", "host"}}};
        runtime.world_loaded(Side::server, world);
        runtime.world_loaded(Side::client, world);
        for (const modding::ModStatus &status : runtime.status())
            expect(status.state == modding::ModState::running,
                   std::format("{} [{}] runs: {}", status.id, modding::side_name(status.side), status.error));
    }

    // mod_net_publish_player.
    void publish_directory(Handle player, std::uint8_t gid, std::string_view name)
    {
        const modding::StoreRef store = modding::StoreRef::player(player);
        const modding::ReplicatedKeyOptions options{true, modding::ReplicationPriority::critical};
        server->set(store, "", "gid", Value{static_cast<std::int64_t>(gid)}, options);
        server->set(store, "", "name", Value{std::string{name}}, options);
    }

    // mod_net_peer_connected: admit, open the channel, then player.joined.
    Handle admit_remote(PeerId peer, std::uint8_t gid, std::string_view name)
    {
        const Handle player = gameplay->admit_player(gid, name, false);
        expect(server->add_peer(peer, player, now), "an admitted extension peer gets a channel");
        publish_directory(player, gid, name);
        gameplay->remote_player_joined(gid, name);
        return player;
    }

    // mod_net_peer_disconnected.
    void remove_remote(PeerId peer, std::uint8_t gid)
    {
        const std::optional<Handle> player = server->player_of(peer);
        server->remove_peer(peer);
        gameplay->remote_player_left(gid);
        if (player.has_value())
        {
            server->drop_store(modding::StoreRef::player(*player));
            ui->forget_player(*player);
        }
    }

    // One fixed step and one frame.
    void step(double seconds)
    {
        now += seconds;
        if (runtime.running(Side::server))
            runtime.tick(seconds);
        gameplay->tick(static_cast<float>(seconds));
        runtime.frame(seconds);
        ui->update(seconds);
        if (client != nullptr && runtime.running(Side::client))
            client->set_ready(now);
        if (server != nullptr)
            server->update(now);
    }

    void stop()
    {
        runtime.stop_all();
        ui->set_channel(nullptr);
        ui_channel.reset();
        if (link != nullptr)
            link->attach(nullptr, nullptr);
        gameplay->loopback().set_remote(nullptr);
        link.reset();
        net.server_ptr = nullptr;
        net.client_ptr = nullptr;
        client.reset();
        server.reset();
    }

    // A script value of the mod's shared state on one side.
    Value probe(Side side, std::string_view expression)
    {
        const modding::RunResult result = runtime.run(
            side, "quest", "probe", std::format("local state = require('shared/state') return {}", expression));
        expect(result.ok, std::format("probe '{}': {}", expression, result.error));
        return result.value;
    }
    void run(Side side, std::string_view source)
    {
        const modding::RunResult result = runtime.run(side, "quest", "chunk", source);
        expect(result.ok, std::format("chunk failed: {}", result.error));
    }
};

// The session between one host and its joined clients: mod frames ride the
// LoopbackSession's control channel exactly as Steam's channel 0 carries them.
struct Session final
{
    LoopbackSession &session;
    Process &host;
    std::map<PeerId, Process *> clients;
    std::size_t refused{};
    std::size_t delivered{};

    void pump()
    {
        for (int round = 0; round < 64; ++round)
        {
            bool moved = false;
            host.client->update(host.now);
            for (const auto &frame : host.client->take_sends())
            {
                host.server->receive(modding::local_mod_peer, frame, host.now);
                moved = true;
            }
            for (modding::ModNetSend &send : host.server->take_sends())
            {
                if (send.peer == modding::local_mod_peer)
                {
                    host.client->receive(send.frame, host.now);
                    moved = true;
                    continue;
                }
                refused += session
                               .submit_from_host(send.peer, send.frame, MessageDeliveryPolicy::reliable_ordered,
                                                 openminerz_control_channel)
                               .accepted()
                               ? 0U
                               : 1U;
            }
            for (auto &[peer, process] : clients)
            {
                process->client->update(process->now);
                for (const auto &frame : process->client->take_sends())
                    refused += session
                                   .submit_from_peer(peer, frame, MessageDeliveryPolicy::reliable_ordered,
                                                     openminerz_control_channel)
                                   .accepted()
                                   ? 0U
                                   : 1U;
            }
            while (auto datagram = session.receive_for_host())
            {
                expect(datagram->channel == openminerz_control_channel, "mod frames stay on the control channel");
                host.server->receive(datagram->sender, datagram->payload, host.now);
                ++delivered;
                moved = true;
            }
            for (auto &[peer, process] : clients)
                while (auto datagram = session.receive_for_peer(peer))
                {
                    process->client->receive(datagram->payload, process->now);
                    ++delivered;
                    moved = true;
                }
            if (!moved)
                return;
        }
    }

    void steps(int count = 1)
    {
        for (int step = 0; step < count; ++step)
        {
            host.step(0.05);
            for (auto &[peer, process] : clients)
                process->step(0.05);
            pump();
        }
    }
};

std::uint64_t handle_id(const Value &value, std::string_view what)
{
    expect(value.type() == modding::ValueType::handle,
           std::format("{} is a handle (got {})", what, modding::value_type_name(value.type())));
    return value.as_handle().id;
}

// ------------------------------------------------------------- the session

void host_and_client_play_one_mod_together()
{
    const ModManifest manifest = quest_manifest();
    const HostSessionSettings settings = settings_for(manifest);
    LoopbackSession wire{settings, false, 1, 1, false};

    Process host;
    host.game.local_gid = 0U;
    host.game.local_name = "Host";
    const Handle host_player = host.host(manifest);
    Session session{wire, host};
    session.steps(2);
    expect(host.client->welcomed() && host.client->local_player() == host_player,
           "the host's own client half is welcomed as the admitted local player");
    expect(host.gameplay->local_player_handle() == host_player, "players.* names the host the same way");
    expect(host.ui->toast_count() == 1U && host.probe(Side::client, "state.hello") == Value{1},
           "the host's own player is reached through its handle like any other");

    // --- A client joins through admission and the extension hello.
    expect(admit(wire, settings, 101U, manifest) == AdmissionStatus::admitted,
           "an OpenMinerZ peer with the same manifest is admitted");
    // Its PlayerExists has not arrived: the game does not list it yet.
    const Handle joiner = host.admit_remote(101U, 5U, "Joiner");
    expect(joiner.kind == HandleKind::player && joiner != host_player, "the joiner gets its own handle");
    expect(host.gameplay->player_handle(5U) == joiner && host.server->player_of(101U) == joiner &&
               host.gameplay->player_gid(joiner) == std::optional<std::uint8_t>{std::uint8_t{5U}},
           "gameplay and the channel agree on the joiner's handle");
    Process client;
    client.game.local_gid = 5U;
    client.game.local_name = "Joiner";
    client.join(manifest);
    session.clients[101U] = &client;
    session.steps(4);
    // The game's own list catches up later; the admitted record stays put.
    host.game.remotes.push_back({5U, "Joiner", false, true, {2.5, 1.0, 3.5}, 0.0F, 100.0F});
    session.steps(4);

    // --- One identity everywhere.
    const Value joined = host.probe(Side::server, "state.joined");
    expect(joined.type() == modding::ValueType::array && joined.as_array().size() == 2U,
           "player.joined reached the server mod once for each player");
    expect(handle_id(joined.as_array()[0], "the host in player.joined") == host_player.id &&
               handle_id(joined.as_array()[1], "the joiner in player.joined") == joiner.id,
           "player.joined carries the admitted handles");
    expect(host.probe(Side::server, "#state.ready") == Value{2},
           "net.player_ready fired once per player, after player.joined");
    expect(handle_id(host.probe(Side::server, "state.ready[2]"), "net.player_ready") == joiner.id,
           "net.player_ready names the joiner by the same handle");
    const Value listed = host.probe(Side::server, "net.players()");
    expect(listed.type() == modding::ValueType::array && listed.as_array().size() == 2U &&
               handle_id(listed.as_array()[1], "net.players") == joiner.id,
           "net.players() lists the same handles");
    expect(client.client->welcomed() && client.client->local_player() == joiner,
           "the joined client is welcomed as the same handle");
    expect(handle_id(client.probe(Side::client, "net.local_player()"), "net.local_player") == joiner.id,
           "net.local_player() on the client is the host's handle for it");
    expect(handle_id(client.probe(Side::client, "state.ready_as"), "net.ready") == joiner.id,
           "net.ready names the client by the same handle");
    // The directory lets the app find a handle's player on any peer.
    const Value *directory = client.client->get(modding::StoreRef::player(host_player), "", "gid");
    expect(directory != nullptr && *directory == Value{0}, "every client can map a handle to its game player");

    // --- What player.joined did with that handle reached the client.
    expect(client.probe(Side::client, "state.hello") == Value{2}, "net.send_to(p) from player.joined arrived");
    expect(client.ui->toast_count() == 1U, "ui.toast(p) from player.joined arrived");
    const auto stats = client.gameplay->client_stats();
    const auto thirst = std::ranges::find(stats, "thirst", &gameplay::ClientStat::name);
    expect(thirst != stats.end() && thirst->value == 42.0 && thirst->hud,
           "stats.set(p) reached that player's HUD stats");
    expect(client.probe(Side::client, "state.coins") == Value{7} &&
               handle_id(client.probe(Side::client, "state.coins_owner"), "the coins' owner") == joiner.id,
           "replicated.set(p) reached the client under the same handle");
    expect(client.probe(Side::client, "replicated.get('world', 'phase')") == Value{"start"},
           "world state reached the client");

    // --- RPC both ways; the server handler runs as that player.
    client.run(Side::client, "net.send('ping', { n = 5 })");
    session.steps(3);
    expect(handle_id(host.probe(Side::server, "state.ping_from"), "the RPC sender") == joiner.id,
           "the server handler gets the sender's handle from the connection");
    expect(client.probe(Side::client, "state.pong") == Value{6}, "the reply came back");
    client.run(Side::client, "net.send('ping', { n = 'not a number' })");
    session.steps(2);
    expect(client.probe(Side::client, "state.pong") == Value{6}, "a payload against the schema never runs");

    // --- A dialogue the client answers; its action runs as that player.
    host.run(Side::server, "dialogue.open(players.by_name('Joiner'), 'guide')");
    session.steps(2);
    const auto &shown = client.gameplay->client_dialogue();
    expect(shown.has_value() && shown->text == "Which way?" && shown->choices.size() == 2U,
           "the dialogue reached the client");
    client.gameplay->client_choose(shown->session, 0U);
    session.steps(2);
    expect(handle_id(host.probe(Side::server, "state.choice_player"), "dialogue.choice") == joiner.id,
           "dialogue.choice names the client by the same handle");
    expect(handle_id(host.probe(Side::server, "state.action_player"), "the choice's action") == joiner.id,
           "the choice's action ran for that player (invoke_as)");

    // --- A mod block and a mod creature, seen with their identities.
    host.run(Side::server, "world.set_block(vector.create(4, 1, 4), 'quest:moss')");
    host.run(Side::server, "entities.spawn('deer', vector.create(6, 1, 6))");
    session.steps(6);
    const std::uint8_t rock = gameplay::retail_block_from_name("rock").value();
    expect(host.game.edits[{4, 1, 4}] == rock, "the world holds the mod block's retail fallback");
    const gameplay::ClientBlockType *moss = client.gameplay->client_block_at({4, 1, 4}, rock);
    expect(moss != nullptr && moss->key == "quest:moss", "the client knows the cell is the mod block");
    expect(client.gameplay->client_block_at({4, 1, 4}, 0U) == nullptr,
           "a cell no longer holding the fallback is not the mod block");
    const auto &entities = client.gameplay->client_entities();
    expect(entities.size() == 1U && entities[0].kind == "quest:deer" && entities[0].model == "deer.gltf",
           "the mod creature was replicated to the client");

    // --- Mod item identity in crates and network pickups.
    const std::uint16_t stick = gameplay::retail_item_from_name("stick").value();
    const auto relic_on = [](const gameplay::GameplayService &service) {
        for (const gameplay::ClientItemType &item : service.client_item_types())
            if (item.key == "quest:relic")
                return item.id;
        return std::uint32_t{};
    };
    const std::uint32_t relic = relic_on(*client.gameplay);
    expect(relic != 0U && relic == relic_on(*host.gameplay), "both peers number the mod item the same");
    const gameplay::BlockPos crate{2, 1, 2};
    // The client puts a relic in slot 0; the stock ItemCrateMessage brings the
    // host its fallback (a stick) and the mod channel its identity.
    host.game.crates[{crate, 0U}] = InventoryStack{InventoryItemId::stick, 1U};
    client.game.crates[{crate, 0U}] = InventoryStack{InventoryItemId::stick, 1U};
    client.game.crates[{crate, 0U}]->mod_item = relic;
    client.gameplay->local_crate_slot(crate, 0U, {stick, 1U, relic});
    session.steps(2);
    expect(host.gameplay->crate_slot_identity(crate, 0U, stick) == relic &&
               host.game.crates[{crate, 0U}]->mod_item == relic,
           "the host's crate keeps the client's mod item");
    expect(host.gameplay->crate_slot_identity(crate, 0U, static_cast<std::uint16_t>(InventoryItemId::torch)) == 0U,
           "an identity never applies to a different retail item");
    // The host's player puts one in slot 1; the client's copy learns it.
    client.game.crates[{crate, 1U}] = InventoryStack{InventoryItemId::stick, 3U};
    host.game.crates[{crate, 1U}] = InventoryStack{InventoryItemId::stick, 3U};
    host.gameplay->local_crate_slot(crate, 1U, {stick, 3U, relic});
    session.steps(2);
    expect(client.gameplay->crate_slot_identity(crate, 1U, stick) == relic &&
               client.game.crates[{crate, 1U}]->mod_item == relic,
           "the client's crate keeps the host's mod item");
    // A plain stick replacing it clears the identity everywhere.
    client.gameplay->local_crate_slot(crate, 1U, {stick, 3U, 0U});
    session.steps(2);
    expect(host.gameplay->crate_slot_identity(crate, 1U, stick) == 0U, "a retail stack clears the identity");
    // A pickup the client drops; whoever picks it up gets the relic.
    client.gameplay->local_pickup_published(5U, 77, {stick, 1U, relic});
    session.steps(2);
    expect(host.gameplay->take_pickup_identity(5U, 77, stick) == relic,
           "the host learns the pickup's identity (spawner taken from the channel)");
    expect(client.gameplay->take_pickup_identity(5U, 77, stick) == relic,
           "every client learns it");
    expect(client.gameplay->take_pickup_identity(5U, 77, stick) == 0U, "an identity is used once");
    host.gameplay->crate_destroyed(crate);
    session.steps(2);
    expect(host.gameplay->crate_slot_identity(crate, 0U, stick) == 0U &&
               client.gameplay->crate_slot_identity(crate, 0U, stick) == 0U,
           "a destroyed crate's identities go on every peer");

    // --- The client leaves; a new one joins with the same gid.
    host.remove_remote(101U, 5U);
    wire.disconnect(101U);
    session.clients.erase(101U);
    host.game.remotes.clear();
    session.steps(2);
    expect(handle_id(host.probe(Side::server, "state.left"), "player.left") == joiner.id,
           "player.left names the departed player by its handle");
    expect(host.probe(Side::server, "#net.players()") == Value{1}, "the channel forgot it");
    expect(host.server->get(modding::StoreRef::player(joiner), "quest", "coins") == nullptr,
           "its replicated store went with it");
    client.stop();

    expect(admit(wire, settings, 102U, manifest) == AdmissionStatus::admitted, "a new client is admitted");
    const Handle rejoiner = host.admit_remote(102U, 5U, "Joiner");
    expect(rejoiner != joiner && rejoiner.id > joiner.id, "a handle is never reused in a session");
    Process again;
    again.game.local_gid = 5U;
    again.game.local_name = "Joiner";
    again.join(manifest);
    session.clients[102U] = &again;
    session.steps(6);
    expect(again.client->local_player() == rejoiner &&
               handle_id(again.probe(Side::client, "net.local_player()"), "the rejoiner") == rejoiner.id,
           "the new client is its new handle");
    expect(again.probe(Side::client, "state.hello") == Value{3} && again.ui->toast_count() == 1U,
           "player.joined reached the new client through its handle");
    expect(again.gameplay->client_item_types().size() == host.gameplay->client_item_types().size(),
           "the registry reached it");
    expect(session.refused == 0U, "every mod frame took the real session path");
    expect(host.runtime.recent_log().empty() ||
               std::ranges::none_of(host.runtime.recent_log(),
                                    [](const modding::LogEntry &entry) {
                                        return entry.level == modding::LogLevel::error;
                                    }),
           "no script error on the host");

    // --- A stock peer next to it is never handed an extension frame.
    expect(admit(wire, settings, 303U, {}, false) == AdmissionStatus::rejected_extension_required,
           "a stock client cannot enter a modded session");
    const auto frame = encode_mod_message(1U, std::vector<std::uint8_t>{2U, 1U});
    expect(!wire.submit_from_host(303U, frame[0], MessageDeliveryPolicy::reliable_ordered,
                                  openminerz_control_channel)
                .accepted(),
           "the session refuses a mod frame for it");
    session.steps(2);
    expect(!wire.receive_for_peer(303U).has_value(), "and it received nothing");
}

// ----------------------------------------------- no server mods, no new bytes

// A client-scope mod whose client script tries every library: the host runs
// the whole modding stack (runtime, gameplay, UI, net bindings) for it, as the
// app does for a host whose manifest has no server mod.
constexpr std::string_view client_only_script = R"(
ui.toast("hello", "from a client mod")
local ok_send = pcall(function() net.send("x", {}) end)
local ok_rep = pcall(function() return replicated.get("world", "k") end)
events.on("frame", function() ui.title("tick") end)
return ok_send, ok_rep
)";

std::vector<std::vector<std::uint8_t>> stock_transcript(bool with_modding)
{
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    manifest.mods.push_back({"pretty", "1", digest_of(9U), ModScope::client});
    const HostSessionSettings settings = settings_for(manifest);
    LoopbackSession wire{settings, false, 1, 1, false};
    std::vector<std::vector<std::uint8_t>> transcript;
    const auto record = [&](char tag, PeerId peer) {
        for (;;)
        {
            std::optional<Datagram> datagram = tag == 'h' ? wire.receive_for_host() : wire.receive_for_peer(peer);
            if (!datagram.has_value())
                return;
            std::vector<std::uint8_t> entry{static_cast<std::uint8_t>(tag), datagram->channel,
                                            static_cast<std::uint8_t>(datagram->delivery)};
            entry.insert(entry.end(), datagram->payload.begin(), datagram->payload.end());
            transcript.push_back(std::move(entry));
        }
    };
    expect(admit(wire, settings, 401U, manifest) == AdmissionStatus::admitted,
           "an OpenMinerZ peer joins a host without server mods");
    expect(admit(wire, settings, 402U, {}, false) == AdmissionStatus::admitted,
           "a stock peer joins a host without server mods");
    std::unique_ptr<Process> host;
    if (with_modding)
    {
        host = std::make_unique<Process>();
        // No server mod: the channel objects are disabled (the app does not
        // even build them), and only the client VM runs.
        host->attach(manifest, true);
        expect(!host->server->enabled() && !host->client->enabled(), "no server mod: the channel is off");
        expect(!host->server->add_peer(401U, Handle{HandleKind::player, 1U}, 0.0), "and admits nobody");
        modding::ModSource pretty;
        pretty.id = "pretty";
        pretty.scope = ModScope::client;
        pretty.files["client/main.luau"] = "events.on('frame', function() ui.title('x') end)";
        std::vector<modding::ModSource> mods{std::move(pretty)};
        expect(host->runtime.start(Side::client, std::move(mods), {}), "the client mod's VM runs");
        modding::ModSource client_scope;
        client_scope.id = "pretty";
        client_scope.scope = ModScope::client;
        std::vector<modding::ModSource> server_mods{std::move(client_scope)};
        expect(!host->runtime.start(Side::server, std::move(server_mods), {}), "and no server VM");
        const modding::RunResult result = host->runtime.run(Side::client, "pretty", "probe", client_only_script);
        expect(result.ok, "the client-only script runs: " + result.error);
        expect(!host->gameplay->running(), "gameplay stays off without a server mod");
    }
    PacketError packet_error{};
    const MessageDescriptor *chat = message_by_name(ProtocolVersion::classic_v3, "ChatMessage");
    const std::vector<std::uint8_t> chat_payload{5U, 'h', 'e', 'l', 'l', 'o'};
    const auto packet = encode_packet(ProtocolVersion::classic_v3, chat->id, chat_payload, packet_error);
    expect(packet.has_value(), "the stock chat packet encodes");
    for (int step = 0; step < 4; ++step)
    {
        static_cast<void>(wire.submit_from_peer(401U, *packet, chat->transport.delivery, stock_gameplay_channel));
        static_cast<void>(wire.submit_from_peer(402U, *packet, chat->transport.delivery, stock_gameplay_channel));
        static_cast<void>(wire.submit_from_host(401U, *packet, chat->transport.delivery));
        static_cast<void>(wire.submit_from_host(402U, *packet, chat->transport.delivery));
        if (host != nullptr)
        {
            host->step(0.05);
            expect(host->server->take_sends().empty() && host->client->take_sends().empty(),
                   "the disabled channel never produces a frame");
        }
        record('h', 0U);
        record('p', 401U);
        record('p', 402U);
    }
    return transcript;
}

void no_server_mods_means_stock_bytes()
{
    const auto plain = stock_transcript(false);
    const auto modded = stock_transcript(true);
    expect(!plain.empty() && plain == modded,
           "a host running client-mod scripts sends and receives exactly the stock bytes");
    for (const auto &entry : modded)
        expect(entry[1] == stock_gameplay_channel, "nothing but stock gameplay traffic crossed the session");
}

// ------------------------------------------------- InventoryStack::mod_item

PlayerInventory tagged_inventory(bool tagged)
{
    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::classic);
    for (PlayerInventory::Slot &slot : inventory.storage())
        slot.reset();
    for (PlayerInventory::Tray &tray : inventory.trays())
        for (PlayerInventory::Slot &slot : tray)
            slot.reset();
    inventory.storage()[0] = InventoryStack{InventoryItemId::stick, 5U};
    inventory.storage()[3] = InventoryStack{InventoryItemId::torch, 2U};
    inventory.tray()[1] = InventoryStack{InventoryItemId::stick, 1U};
    if (tagged)
    {
        inventory.storage()[0]->mod_item = 1U;
        inventory.storage()[3]->mod_item = 0xFFFFFFFFU;
        inventory.tray()[1]->mod_item = 7U;
    }
    return inventory;
}

WorldSaveData save_with(bool tagged)
{
    WorldSaveData save;
    save.seed = 4242;
    save.has_player = true;
    save.inventory = tagged_inventory(tagged);
    save.held_stack = InventoryStack{InventoryItemId::stick, 1U};
    WorldSaveCrateSlots crate{};
    crate[0] = InventoryStack{InventoryItemId::stick, 9U};
    crate[31] = InventoryStack{InventoryItemId::torch, 1U};
    if (tagged)
    {
        save.held_stack->mod_item = 3U;
        crate[0]->mod_item = 2U;
        crate[31]->mod_item = 12345U;
    }
    save.crates[{1, 2, 3}] = crate;
    return save;
}

void mod_item_never_reaches_a_save_or_a_stock_packet()
{
    // The world save, byte for byte.
    std::ostringstream plain;
    std::ostringstream tagged;
    expect(write_world_save(plain, save_with(false)) && write_world_save(tagged, save_with(true)),
           "both saves write");
    expect(!plain.str().empty() && plain.str() == tagged.str(), "a tagged stack saves exactly as its fallback");
    std::istringstream input{tagged.str()};
    WorldSaveData loaded;
    expect(read_world_save(input, loaded), "the save reads back");
    const auto untagged = [](const PlayerInventory::Slot &slot) {
        return !slot.has_value() || slot->mod_item == 0U;
    };
    expect(std::ranges::all_of(loaded.inventory.storage(), untagged) && untagged(loaded.held_stack) &&
               std::ranges::all_of(loaded.crates.at({1, 2, 3}), untagged),
           "and nothing read from a save is a mod item");

    for (const ProtocolVersion protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        PayloadError error{};
        // An inventory (InventoryStoreOnServerMessage, the bootstrap).
        PlayerInventoryPayload plain_inventory;
        PlayerInventoryPayload tagged_inventory_payload;
        expect(player_inventory_to_payload(tagged_inventory(false), protocol, plain_inventory) &&
                   player_inventory_to_payload(tagged_inventory(true), protocol, tagged_inventory_payload),
               "both inventories convert");
        const auto plain_bytes = encode_player_inventory_payload(plain_inventory, protocol, error);
        const auto tagged_bytes = encode_player_inventory_payload(tagged_inventory_payload, protocol, error);
        expect(plain_bytes.has_value() && plain_bytes == tagged_bytes, "an inventory packet ignores mod items");
        // A crate slot (ItemCrateMessage) and a pickup (CreatePickupMessage).
        const InventoryStack stack{InventoryItemId::stick, 4U};
        InventoryStack mod_stack = stack;
        mod_stack.mod_item = 99U;
        InventorySlotPayload plain_slot;
        InventorySlotPayload tagged_slot;
        expect(inventory_slot_to_payload(PlayerInventory::Slot{stack}, protocol, plain_slot) &&
                   inventory_slot_to_payload(PlayerInventory::Slot{mod_stack}, protocol, tagged_slot) &&
                   plain_slot.has_value() && tagged_slot.has_value(),
               "both slots convert");
        const auto plain_crate = encode_item_crate_payload({{1, 2, 3}, 0, plain_slot}, protocol, error);
        const auto tagged_crate = encode_item_crate_payload({{1, 2, 3}, 0, tagged_slot}, protocol, error);
        expect(plain_crate.has_value() && plain_crate == tagged_crate, "a crate packet ignores mod items");
        CreatePickupPayload pickup;
        pickup.pickup_id = 5;
        pickup.item = *plain_slot;
        const auto plain_pickup = encode_create_pickup_payload(pickup, protocol, error);
        pickup.item = *tagged_slot;
        const auto tagged_pickup = encode_create_pickup_payload(pickup, protocol, error);
        expect(plain_pickup.has_value() && plain_pickup == tagged_pickup, "a pickup packet ignores mod items");
        // The world a joiner is sent (the bootstrap snapshot).
        HostSessionSettings host = settings_for({});
        host.protocol = protocol;
        host.mod_manifest.protocol = protocol;
        BootstrapWorldIdentity identity;
        AuthoritativeBootstrapSnapshot plain_snapshot;
        AuthoritativeBootstrapSnapshot tagged_snapshot;
        BootstrapSnapshotError snapshot_error{};
        WorldSaveData plain_save = save_with(false);
        WorldSaveData tagged_save = save_with(true);
        const InventoryProfile profile =
            protocol == ProtocolVersion::modern_v4 ? InventoryProfile::modern : InventoryProfile::classic;
        plain_save.inventory.set_profile(profile);
        tagged_save.inventory.set_profile(profile);
        expect(make_authoritative_bootstrap_snapshot(plain_save, host, identity, false, plain_snapshot,
                                                     snapshot_error) &&
                   make_authoritative_bootstrap_snapshot(tagged_save, host, identity, false, tagged_snapshot,
                                                         snapshot_error),
               "both bootstrap snapshots build");
        expect(plain_snapshot.inventory == tagged_snapshot.inventory &&
                   plain_snapshot.crate_replay == tagged_snapshot.crate_replay,
               "the joiner's world ignores mod items");
        // Everything a stock message decodes to is a retail stack.
        PlayerInventory::Slot decoded;
        expect(inventory_slot_to_stack(tagged_slot, protocol, decoded) && decoded.has_value() &&
                   decoded->mod_item == 0U,
               "a stack decoded from the wire is never a mod item");
    }
}

void a_vanilla_session_never_makes_a_mod_item()
{
    // Inventory operations on retail stacks keep the tag at zero.
    PlayerInventory inventory = tagged_inventory(false);
    static_cast<void>(inventory.add_stack(InventoryStack{InventoryItemId::stick, 70U}));
    static_cast<void>(inventory.add_stack(InventoryStack{InventoryItemId::torch, 3U}));
    const auto untagged = [](const PlayerInventory::Slot &slot) {
        return !slot.has_value() || slot->mod_item == 0U;
    };
    expect(std::ranges::all_of(inventory.storage(), untagged) && std::ranges::all_of(inventory.tray(), untagged),
           "adding and merging retail stacks never tags one");
    expect(InventoryStack{InventoryItemId::stick, 2U}.with_count(1U).mod_item == 0U, "nor does splitting");
    // A mod item never merges into its fallback (and so never tags it).
    InventoryStack relic{InventoryItemId::stick, 1U};
    relic.mod_item = 4U;
    expect(!relic.same_kind(InventoryStack{InventoryItemId::stick, 1U}), "a mod item is not its fallback");

    // Every identity hook of the gameplay service, with no VM running.
    FakeGame game;
    modding::testing::Harness harness;
    auto service = modding::register_gameplay_api(harness.registry(), harness.host(), game);
    expect(!service->running(), "no VM: the service is off");
    service->local_crate_slot({1, 1, 1}, 0U, {7U, 1U, 4U});
    service->local_pickup_published(0U, 1, {7U, 1U, 4U});
    service->crate_destroyed({1, 1, 1});
    expect(service->crate_slot_identity({1, 1, 1}, 0U, 7U) == 0U && service->take_pickup_identity(0U, 1, 7U) == 0U,
           "a vanilla session has no mod item identity to hand out");
    expect(game.crates.empty() && game.pickups.empty(), "and touches no crate or pickup");
}

// --------------------------------------------------------------- hardening

// A handler that fires the event it handles, directly or through the game,
// must end in a script error, never in a native stack overflow.
void reentrant_handlers_stop_cleanly()
{
    modding::testing::Harness harness;
    FakeGame game;
    auto service = modding::register_gameplay_api(harness.registry(), harness.host(), game);
    const modding::RunResult direct = harness.run(R"(
        local depth = 0
        events.on("test:ping", function() depth = depth + 1; events.emit("test:ping") end)
        events.emit("test:ping")
        return depth
    )");
    expect(direct.ok && direct.value.is_number() && direct.value.as_integer() >= 1 &&
               direct.value.as_integer() < 1000,
           "a self-emitting handler is cut off at a bounded depth: " + direct.error);
    // Through the game: a player.damaged handler that damages the player
    // goes back through the game's damage path, which asks the handlers.
    game.service = service.get();
    service->start_server();
    const modding::RunResult through_game = harness.run(R"(
        damage_depth = 0
        events.on("player.damaged", function(e)
            damage_depth = damage_depth + 1
            players.damage(e.player, 1)
        end)
    )");
    expect(through_game.ok, "the handler loads: " + through_game.error);
    static_cast<void>(service->local_player_damaging(5.0F, gameplay::DamageCause::enemy, {}));
    const modding::RunResult depth = harness.run("return damage_depth");
    expect(harness.runtime().running(Side::server) && depth.ok && depth.value.is_number() &&
               depth.value.as_integer() >= 1 && depth.value.as_integer() < 1000,
           "the loop through the game ends in a script error at a bounded depth");
}

} // namespace

int main()
{
    const std::pair<const char *, void (*)()> cases[]{
        {"host and client play one mod together", host_and_client_play_one_mod_together},
        {"no server mods means stock bytes", no_server_mods_means_stock_bytes},
        {"mod_item never reaches a save or a stock packet", mod_item_never_reaches_a_save_or_a_stock_packet},
        {"a vanilla session never makes a mod item", a_vanilla_session_never_makes_a_mod_item},
        {"re-entrant handlers stop cleanly", reentrant_handlers_stop_cleanly},
    };
    int failures = 0;
    for (const auto &[name, run] : cases)
    {
        try
        {
            run();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (std::size(cases) - static_cast<std::size_t>(failures)) << '/' << std::size(cases)
              << " integration cases passed\n";
    return failures == 0 ? 0 : 1;
}

