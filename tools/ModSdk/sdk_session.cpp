// The modding SDK's stand-in for the game (sdk_session.hpp).

#include "sdk_session.hpp"

#include "openminerz/modding/package.hpp"
#include "openminerz/net/mod_handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>

namespace openminerz::mod_sdk
{

using modding::Handle;
using modding::HandleKind;
using modding::Side;
using modding::Value;
using modding::Vec3;
using modding::gameplay::BlockPos;
using modding::gameplay::StackView;

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------

SandboxWorld::SandboxWorld()
{
    // An empty inventory instead of the starter loadout, so a given item lands
    // in the hand.
    inventory.set_profile(InventoryProfile::modern);
    visit([](PlayerInventory::Slot &slot, std::uint16_t) { slot.reset(); });
}

template <typename Visit>
void SandboxWorld::visit(Visit &&function)
{
    std::uint16_t index{};
    for (PlayerInventory::Slot &slot : inventory.storage())
        function(slot, index++);
    for (PlayerInventory::Tray &tray : inventory.trays())
        for (PlayerInventory::Slot &slot : tray)
            function(slot, index++);
}

std::uint8_t SandboxWorld::terrain(BlockPos cell) noexcept
{
    if (cell.y > 0)
        return 0U; // empty
    if (cell.y == 0)
        return 2U; // grass
    if (cell.y >= -3)
        return 1U; // dirt
    if (cell.y >= -60)
        return 6U; // rock
    return 14U;    // bedrock
}

std::optional<modding::gameplay::LocalView> SandboxWorld::local_view() const
{
    modding::gameplay::LocalView view;
    view.eye = {position.x, position.y + 1.6, position.z};
    const double cos_pitch = std::cos(static_cast<double>(pitch));
    view.look = {std::sin(static_cast<double>(yaw)) * cos_pitch, std::sin(static_cast<double>(pitch)),
                 -std::cos(static_cast<double>(yaw)) * cos_pitch};
    view.yaw = yaw;
    view.pitch = pitch;
    view.target = aimed;
    if (aimed)
        view.target_adjacent = modding::gameplay::BlockPos{aimed->x, aimed->y + 1, aimed->z};
    return view;
}

std::int32_t SandboxWorld::world_seed() const
{
    return seed;
}

bool SandboxWorld::modern_rules() const
{
    return modern;
}

std::optional<std::uint8_t> SandboxWorld::local_player() const
{
    return std::uint8_t{0U};
}

void SandboxWorld::players(std::vector<modding::gameplay::PlayerSnapshot> &out) const
{
    out.clear();
    out.push_back({0U, local_name, true, alive, position, yaw, health});
}

void SandboxWorld::local_set_health(float value)
{
    health = std::clamp(value, 0.0F, 100.0F);
    if (health <= 0.0F)
        alive = false;
}

void SandboxWorld::local_damage(float amount, Vec3 source, modding::gameplay::DamageCause cause)
{
    if (!alive)
        return;
    float applied = amount;
    if (service != nullptr && service->running())
        applied = service->local_player_damaging(amount, cause, source);
    if (applied <= 0.0F)
        return;
    damage_taken.emplace_back(applied, cause);
    health -= applied;
    if (health > 0.0F)
        return;
    health = 0.0F;
    alive = false;
    if (service != nullptr)
        service->local_player_died(position, cause);
    // The game respawns the player where they started, at full health.
    health = 100.0F;
    alive = true;
    position = {8.5, 1.0, 8.5};
    if (service != nullptr)
        service->local_player_respawned(position);
}

void SandboxWorld::local_teleport(Vec3 feet)
{
    position = feet;
}

std::uint16_t SandboxWorld::local_give(StackView stack, bool drop_rest)
{
    InventoryStack given{static_cast<InventoryItemId>(stack.item), stack.count};
    given.mod_item = stack.mod_item;
    const std::uint16_t left = inventory.add_stack(given);
    if (left != 0U && drop_rest)
        pickups.emplace_back(position, StackView{stack.item, left, stack.mod_item});
    return drop_rest ? std::uint16_t{} : left;
}

std::uint32_t SandboxWorld::local_take(std::uint16_t item, std::uint32_t mod_item, std::uint32_t count)
{
    std::uint32_t taken{};
    visit([&](PlayerInventory::Slot &slot, std::uint16_t) {
        if (taken >= count || !slot.has_value() || static_cast<std::uint16_t>(slot->item) != item ||
            slot->mod_item != mod_item)
            return;
        const std::uint32_t here = std::min<std::uint32_t>(slot->count, count - taken);
        slot->count = static_cast<std::uint16_t>(slot->count - here);
        taken += here;
        if (slot->count == 0U)
            slot.reset();
    });
    return taken;
}

std::uint32_t SandboxWorld::local_count(std::uint16_t item, std::uint32_t mod_item) const
{
    std::uint32_t total{};
    const_cast<SandboxWorld *>(this)->visit([&](PlayerInventory::Slot &slot, std::uint16_t) {
        if (slot.has_value() && static_cast<std::uint16_t>(slot->item) == item && slot->mod_item == mod_item)
            total += slot->count;
    });
    return total;
}

void SandboxWorld::local_clear_inventory()
{
    visit([](PlayerInventory::Slot &slot, std::uint16_t) { slot.reset(); });
}

std::optional<StackView> SandboxWorld::local_held() const
{
    const PlayerInventory::Slot &held = inventory.selected();
    if (!held.has_value())
        return std::nullopt;
    return StackView{static_cast<std::uint16_t>(held->item), held->count, held->mod_item};
}

void SandboxWorld::local_slots(std::vector<std::pair<std::uint16_t, StackView>> &out) const
{
    out.clear();
    const_cast<SandboxWorld *>(this)->visit([&](PlayerInventory::Slot &slot, std::uint16_t index) {
        if (slot.has_value())
            out.emplace_back(index, StackView{static_cast<std::uint16_t>(slot->item), slot->count, slot->mod_item});
    });
}

bool SandboxWorld::local_tag_slot(std::uint16_t index, std::uint16_t item, std::uint32_t mod_item)
{
    bool tagged{};
    visit([&](PlayerInventory::Slot &slot, std::uint16_t at) {
        if (at == index && slot.has_value() && static_cast<std::uint16_t>(slot->item) == item)
        {
            slot->mod_item = mod_item;
            tagged = true;
        }
    });
    return tagged;
}

void SandboxWorld::local_set_speed(float multiplier)
{
    speed = multiplier;
}

std::optional<std::uint8_t> SandboxWorld::block(BlockPos cell) const
{
    if (cell.y < -64 || cell.y > 63)
        return std::nullopt;
    const auto found = edits.find(cell);
    return found != edits.end() ? found->second : terrain(cell);
}

bool SandboxWorld::set_block(BlockPos cell, std::uint8_t value)
{
    if (cell.y < -64 || cell.y > 63)
        return false;
    edits[cell] = value;
    return true;
}

int SandboxWorld::biome(int x, int z) const
{
    // mod-world: as the game answers where a mod world generator runs.
    if (generator != nullptr && generator->custom_terrain() != nullptr)
    {
        const auto &custom = generator->custom_terrain();
        const world::TerrainBiome &biome = custom->definition().biomes[custom->biome_at(*generator, x, z)];
        return biome.retail != world::RetailReuse::none ? static_cast<int>(generator->biome_at(x, z))
                                                        : world::biome_id(biome.key);
    }
    // Bands along X, so biome rules see more than one biome: 0 classic, 1
    // lagoon, 2 desert, 3 mountain, 4 arctic, 5 descent.
    return std::clamp(std::abs(x) / 400, 0, 5);
}

std::optional<std::array<float, 2>> SandboxWorld::light(BlockPos cell) const
{
    const std::optional<std::uint8_t> here = block(cell);
    if (!here)
        return std::nullopt;
    if (*here != 0U)
        return std::array<float, 2>{-1.0F, 0.0F};
    return std::array<float, 2>{cell.y >= 1 ? 1.0F : 0.0F, 0.0F};
}

bool SandboxWorld::set_crate_slot(BlockPos, std::size_t, std::optional<StackView>)
{
    return true;
}

bool SandboxWorld::spawn_pickup(Vec3 at, StackView stack)
{
    pickups.emplace_back(at, stack);
    return true;
}

double SandboxWorld::day() const
{
    return clock;
}

void SandboxWorld::set_time_of_day(float time_of_day)
{
    clock = std::floor(clock) + static_cast<double>(time_of_day);
}

bool SandboxWorld::spawn_enemy(int type, Vec3)
{
    spawned_enemies.push_back(type);
    return true;
}

void SandboxWorld::chat(std::optional<std::uint8_t>, std::string_view text)
{
    chat_lines.emplace_back(text);
}

std::filesystem::path SandboxWorld::sidecar_directory() const
{
    return sidecar;
}

namespace
{

// ---------------------------------------------------------------------------
// The other backends
// ---------------------------------------------------------------------------

std::optional<std::string> read_text(const std::filesystem::path &path, std::size_t limit)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > limit)
        return std::nullopt;
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string lowercase(std::string_view text)
{
    std::string result{text};
    for (char &character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

// Every glyph is half its capital height wide (close to the game's text face).
class FixedMeasure final : public modding::ui::TextMeasure
{
  public:
    [[nodiscard]] float width(modding::ui::Face, float cap, std::string_view text) const override
    {
        std::size_t points{};
        for (const char character : text)
            points += (static_cast<unsigned char>(character) & 0xC0U) != 0x80U ? 1U : 0U;
        return cap * 0.55F * static_cast<float>(points);
    }
};

} // namespace

class Sandbox::Impl final
{
  public:
    class UiFake final : public modding::ui::UiBackend
    {
      public:
        explicit UiFake(Impl &owner) : owner_{owner} {}
        [[nodiscard]] modding::ui::Style style() const override
        {
            return modding::ui::Style::remastered;
        }
        [[nodiscard]] std::uint32_t load_order(std::string_view mod) const override
        {
            const net::LoadedMod *package = owner_.package(mod);
            return package == nullptr ? 0U : package->load_order;
        }
        [[nodiscard]] bool asset_exists(const modding::ui::AssetRef &asset) const override
        {
            const std::optional<std::filesystem::path> file = owner_.mod_file(asset.mod, asset.path);
            std::error_code ignored;
            return file.has_value() && std::filesystem::is_regular_file(*file, ignored);
        }
        [[nodiscard]] std::optional<std::string> read_mod_text(std::string_view mod, std::string_view path,
                                                               std::size_t limit) const override
        {
            const std::optional<std::filesystem::path> file = owner_.mod_file(mod, path);
            return file ? read_text(*file, limit) : std::nullopt;
        }
        [[nodiscard]] std::optional<std::string> read_local(std::string_view name) const override
        {
            const auto found = local_.find(std::string{name});
            return found == local_.end() ? std::nullopt : std::optional<std::string>{found->second};
        }
        void write_local(std::string_view name, std::string_view text) override
        {
            local_[std::string{name}] = std::string{text};
        }
        [[nodiscard]] bool item_exists(int item) const override
        {
            return item > 0 && item < static_cast<int>(InventoryItemId::item_count);
        }
        [[nodiscard]] std::string item_name(int item) const override
        {
            return std::string{modding::gameplay::retail_item_name(static_cast<std::uint16_t>(item))};
        }
        [[nodiscard]] std::string canonical_key(std::string_view key) const override
        {
            if (key.empty() || key.size() > 24U)
                return {};
            std::string result{key};
            result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
            return result;
        }
        [[nodiscard]] bool offline() const override
        {
            return true;
        }
        [[nodiscard]] Handle local_player() const override
        {
            return owner_.gameplay->local_player_handle();
        }
        void play(modding::ui::SoundKey) override {}

      private:
        Impl &owner_;
        std::map<std::string, std::string> local_;
    };

    class GraphicsFake final : public modding::graphics::GraphicsBackend
    {
      public:
        explicit GraphicsFake(Impl &owner) : owner_{owner} {}
        [[nodiscard]] bool locate(Handle target, modding::graphics::Vec3 &position, float &yaw) const override
        {
            if (target.kind == HandleKind::player && target == owner_.gameplay->local_player_handle())
            {
                position = {static_cast<float>(owner_.world.position.x),
                            static_cast<float>(owner_.world.position.y),
                            static_cast<float>(owner_.world.position.z)};
                yaw = owner_.world.yaw;
                return true;
            }
            if (target.kind != HandleKind::entity)
                return false;
            for (const modding::gameplay::ClientEntity &entity : owner_.gameplay->client_entities())
                if (entity.id == target.id)
                {
                    position = {static_cast<float>(entity.position.x), static_cast<float>(entity.position.y),
                                static_cast<float>(entity.position.z)};
                    yaw = entity.yaw;
                    return true;
                }
            return false;
        }
        [[nodiscard]] bool effects_available() const override
        {
            return true;
        }
        vfx::EffectHandle spawn_effect(const vfx::EffectDesc &, const vfx::SpawnParams &) override
        {
            ++spawned_;
            return {static_cast<std::uint16_t>(spawned_ & 0xFFFFU), 1U};
        }
        [[nodiscard]] bool effect_alive(vfx::EffectHandle) const override
        {
            return false;
        }
        void move_effect(vfx::EffectHandle, modding::graphics::Vec3, modding::graphics::Vec3) override {}
        void kill_effect(vfx::EffectHandle, bool) override {}
        [[nodiscard]] modding::graphics::Mixer *mixer(bool) override
        {
            return nullptr; // no audio device: sounds are refused quietly
        }

      private:
        Impl &owner_;
        std::uint32_t spawned_{};
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

    class Stats final : public modding::ui::StatSource
    {
      public:
        explicit Stats(Impl &owner) : owner_{owner} {}
        [[nodiscard]] std::optional<modding::ui::StatValue> stat(std::string_view name) const override
        {
            if (!owner_.gameplay->client_running())
                return std::nullopt;
            for (const modding::gameplay::ClientStat &stat : owner_.gameplay->client_stats())
                if (stat.name == name)
                    return modding::ui::StatValue{stat.value - stat.minimum, stat.maximum - stat.minimum};
            return std::nullopt;
        }

      private:
        Impl &owner_;
    };

    class Config final : public modding::ui::ConfigSource
    {
      public:
        explicit Config(Impl &owner) : owner_{owner} {}
        [[nodiscard]] std::vector<modding::ConfigSetting> settings() const override
        {
            return owner_.runtime.config_settings();
        }
        bool set(std::string_view mod, std::string_view key, const Value &value, std::string &error) override
        {
            return owner_.runtime.set_config(mod, key, value, error);
        }

      private:
        Impl &owner_;
    };

    explicit Impl(SandboxOptions options)
        : options_{std::move(options)}, runtime{options_.limits}, ui_backend{*this}, graphics_backend{*this},
          ui{ui_backend}, graphics{graphics_backend}, stats{*this}, config{*this}
    {
        runtime.set_log_sink([this](const modding::LogEntry &entry) {
            log.push_back(entry);
            if (echo)
            {
                std::ostream &out = entry.level >= modding::LogLevel::warning ? std::cerr : std::cout;
                out << "mod." << modding::log_level_name(entry.level) << " side=" << modding::side_name(entry.side)
                    << " mod=" << (entry.mod_id.empty() ? std::string{"runtime"} : entry.mod_id) << ' '
                    << entry.message << '\n';
            }
        });
        // The game's order (app_mod_runtime.inc: register_mod_apis).
        modding::ApiRegistry &api = runtime.registry();
        gameplay = modding::register_gameplay_api(api, runtime, world);
        world.service = gameplay.get();
        ui.set_stats(&stats);
        modding::graphics::register_graphics_api(api, runtime, graphics);
        ui.set_config(&config);
        ui.set_language("en");
        ui.register_api(api, runtime);
        modding::register_net_api(api, runtime, net_backend);
        // mod-world: the world libraries, last as in register_mod_apis.
        worldgen = modding::worldgen::register_worldgen_api(api, runtime, worldgen_backend);
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
    }

    // mod-world: the game's WorldgenBackend (app_mod_world.inc) on the sandbox.
    class WorldBackend final : public modding::worldgen::WorldgenBackend
    {
      public:
        explicit WorldBackend(Impl &owner) : owner_{owner} {}
        [[nodiscard]] std::optional<std::uint8_t> block_fallback(Side side, std::string_view key) const override
        {
            return owner_.gameplay != nullptr ? owner_.gameplay->block_fallback(side, key) : std::nullopt;
        }
        [[nodiscard]] const world::WorldGenerator *generator() const override
        {
            return &owner_.terrain_generator;
        }
        [[nodiscard]] std::int32_t world_seed() const override
        {
            return owner_.world.seed;
        }

      private:
        Impl &owner_;
    };

    // mod-world: once the scripts ran, the world they define (as the game
    // builds it when a session starts).
    void build_world(bool server_vm)
    {
        terrain.reset();
        terrain_generator = world::WorldGenerator{
            world.seed, world.modern ? world::TerrainProfile::modern : world::TerrainProfile::classic};
        const Side side = server_vm ? Side::server : Side::client;
        if (worldgen == nullptr || !worldgen->defines_world(side))
            return;
        std::string error;
        terrain = worldgen->build(side, error);
        if (!error.empty())
            runtime.log(worldgen->world_mod(side).first, modding::LogLevel::error, error);
        terrain_generator.set_custom_terrain(terrain);
        worldgen->set_active(terrain);
        world.generator = &terrain_generator;
    }

    ~Impl()
    {
        stop();
    }

    [[nodiscard]] const net::LoadedMod *package(std::string_view id) const
    {
        const std::string key = lowercase(id);
        for (const net::LoadedMod &mod : mods)
            if (lowercase(mod.descriptor.id) == key)
                return &mod;
        return nullptr;
    }

    [[nodiscard]] std::optional<std::filesystem::path> mod_file(std::string_view id, std::string_view path) const
    {
        const std::optional<std::string> normalized = modding::ui::normalize_asset_path(path);
        const net::LoadedMod *mod = package(id);
        if (!normalized || mod == nullptr)
            return std::nullopt;
        return mod->root / std::filesystem::path{*normalized};
    }

    [[nodiscard]] double now() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
    }

    bool start(std::span<const net::LoadedMod> list, std::string &error)
    {
        stop();
        mods.assign(list.begin(), list.end());
        modding::PackagePlan plan = modding::plan_packages(mods);
        refused = plan.refused;
        if (plan.mods.empty())
        {
            error = refused.empty() ? "no mods to load" : refused.front().first + ": " + refused.front().second;
            return false;
        }

        // Storage beside a world save, like the game's "<world>.mods".
        modding::SessionInfo info;
        if (!options_.data_dir.empty())
        {
            info.world_dir = options_.data_dir / "world.omw.mods";
            info.local_dir = options_.data_dir / "ModData";
            world.sidecar = info.world_dir;
        }
        info.world_seed = static_cast<std::uint32_t>(world.seed);

        // Assets: every mod's assets/ folder, in load order.
        std::vector<modding::graphics::AssetSource> sources;
        for (const net::LoadedMod &mod : mods)
        {
            std::error_code ignored;
            if (std::filesystem::is_directory(mod.root / "assets", ignored))
                sources.push_back({mod.descriptor.id, mod.root / "assets"});
        }
        graphics.set_sources(std::move(sources));

        // The mod channel, before the VMs (app_mod_net.inc).
        net::ModCatalog catalog;
        catalog.mods = mods;
        net::ModManifest manifest;
        std::string manifest_error;
        if (net::make_mod_manifest(catalog, net::ProtocolVersion::modern_v4, manifest, manifest_error) &&
            net::requires_extension(manifest))
        {
            server = std::make_unique<modding::ModNetServer>(manifest);
            client = std::make_unique<modding::ModNetClient>(manifest);
            net_backend.server_ptr = server.get();
            net_backend.client_ptr = client.get();
            modding::connect_net_events(runtime, server.get(), client.get());
            gameplay_link = std::make_unique<modding::gameplay::ModNetGameplaySync>(*gameplay);
            gameplay_link->attach(server.get(), nullptr);
            gameplay->loopback().set_remote(gameplay_link.get());
            ui_channel = std::make_unique<modding::ui::ModNetUiChannel>(ui, server.get(), client.get(),
                                                                       [this] { return now(); });
            ui.set_channel(ui_channel.get());
        }

        const bool server_vm = runtime.start(Side::server, plan.mods, info);
        const bool client_vm = runtime.start(Side::client, std::move(plan.mods), info);
        if (!server_vm && !client_vm)
        {
            error = "no VM started (the mods have no scripts)";
            stop();
            return false;
        }
        if (server_vm)
            gameplay->start_server();
        if (client_vm)
            gameplay->start_client();
        build_world(server_vm); // mod-world
        // This process's own player is a peer like any other, named by the
        // handle Gameplay gives it.
        if (server != nullptr)
            static_cast<void>(server->add_peer(modding::local_mod_peer, gameplay->local_player_handle(), now()));
        const Value payload{modding::Map{{"name", "sandbox"},
                                         {"seed", static_cast<std::int64_t>(world.seed)},
                                         {"mode", "offline"}}};
        runtime.world_loaded(Side::server, payload);
        runtime.world_loaded(Side::client, payload);
        if (gameplay->running())
            gameplay->local_player_spawned(world.position);
        active = true;
        return true;
    }

    void pump()
    {
        if (server == nullptr || client == nullptr)
            return;
        if (runtime.running(Side::client))
            client->set_ready(now());
        modding::pump_mod_net_loopback(*server, *client, modding::local_mod_peer, now());
        static_cast<void>(server->take_events());
        static_cast<void>(client->take_events());
    }

    void sync_ui()
    {
        if (!gameplay->client_running())
            return;
        std::vector<modding::ui::UiService::HudStat> bars;
        for (const modding::gameplay::ClientStat &stat : gameplay->client_stats())
            if (stat.hud)
                bars.push_back({stat.name, stat.label, modding::ui::parse_color(Value{stat.color}), stat.value,
                                stat.minimum, stat.maximum});
        ui.set_hud_stats(std::move(bars));
        const std::optional<modding::gameplay::ClientDialogue> &dialogue = gameplay->client_dialogue();
        if (dialogue.has_value() && dialogue->session != ui.native_dialogue_session())
        {
            modding::ui::UiService::NativeDialogue shown;
            shown.session = dialogue->session;
            shown.speaker = dialogue->speaker.empty() ? dialogue->title : dialogue->speaker;
            shown.text = dialogue->text;
            shown.choices = dialogue->choices;
            const std::uint64_t session = dialogue->session;
            shown.choose = [this, session](std::size_t choice) { gameplay->client_choose(session, choice); };
            ui.show_native_dialogue(std::move(shown));
        }
        else if (!dialogue.has_value() && ui.native_dialogue_session() != 0U)
            ui.close_native_dialogue(ui.native_dialogue_session());
    }

    void step(double seconds)
    {
        const float dt = static_cast<float>(seconds);
        world.advance(seconds);
        runtime.tick(seconds);
        gameplay->tick(dt);
        if (server != nullptr)
            server->update(now());
        pump();
        runtime.frame(seconds);
        sync_ui();
        ui.update(seconds);
        ui.flush();
        graphics.step(dt, {static_cast<float>(world.position.x), static_cast<float>(world.position.y) + 1.6F,
                           static_cast<float>(world.position.z)});
        modding::ui::FrameContext frame;
        frame.time = clock_;
        modding::ui::DrawList hud;
        modding::ui::DrawList screens;
        ui.draw_hud(frame, measure, hud);
        ui.draw_notices(frame, measure, hud);
        ui.draw_screens(frame, measure, screens);
        hud_commands = hud.size();
        screen_commands = screens.size();
        clock_ += seconds;
    }

    void stop()
    {
        if (!active && server == nullptr)
            return;
        runtime.stop_all();
        if (gameplay->server_running())
            gameplay->stop_server();
        if (gameplay->client_running())
            gameplay->stop_client();
        ui.set_channel(nullptr);
        ui_channel.reset();
        if (gameplay_link != nullptr)
        {
            gameplay_link->attach(nullptr, nullptr);
            gameplay->loopback().set_remote(nullptr);
            gameplay_link.reset();
        }
        net_backend.server_ptr = nullptr;
        net_backend.client_ptr = nullptr;
        client.reset();
        server.reset();
        ui.clear_all();
        graphics.reset();
        active = false;
    }

    SandboxOptions options_;
    modding::ModRuntime runtime;
    SandboxWorld world;
    UiFake ui_backend;
    GraphicsFake graphics_backend;
    NetObjects net_backend;
    modding::ui::UiService ui;
    modding::graphics::GraphicsService graphics;
    Stats stats;
    Config config;
    std::unique_ptr<modding::gameplay::GameplayService> gameplay;
    std::unique_ptr<modding::ModNetServer> server;
    std::unique_ptr<modding::ModNetClient> client;
    std::unique_ptr<modding::gameplay::ModNetGameplaySync> gameplay_link;
    std::unique_ptr<modding::ui::ModNetUiChannel> ui_channel;
    // mod-world
    WorldBackend worldgen_backend{*this};
    std::unique_ptr<modding::worldgen::WorldgenService> worldgen;
    std::shared_ptr<const world::CustomTerrain> terrain;
    world::WorldGenerator terrain_generator{20261014};
    std::vector<net::LoadedMod> mods;
    std::vector<std::pair<std::string, std::string>> refused;
    std::vector<modding::LogEntry> log;
    FixedMeasure measure;
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    double clock_{};
    std::size_t hud_commands{};
    std::size_t screen_commands{};
    bool echo{};
    bool active{};
};

Sandbox::Sandbox(SandboxOptions options) : impl_{std::make_unique<Impl>(std::move(options))} {}

Sandbox::~Sandbox() = default;

modding::ApiRegistry &Sandbox::registry() noexcept
{
    return impl_->runtime.registry();
}

modding::ModRuntime &Sandbox::runtime() noexcept
{
    return impl_->runtime;
}

modding::gameplay::GameplayService &Sandbox::gameplay() noexcept
{
    return *impl_->gameplay;
}

modding::ui::UiService &Sandbox::ui() noexcept
{
    return impl_->ui;
}

modding::graphics::GraphicsService &Sandbox::graphics() noexcept
{
    return impl_->graphics;
}

SandboxWorld &Sandbox::world() noexcept
{
    return impl_->world;
}

modding::ModNetServer *Sandbox::net_server() noexcept
{
    return impl_->server.get();
}

modding::ModNetClient *Sandbox::net_client() noexcept
{
    return impl_->client.get();
}

modding::worldgen::WorldgenService &Sandbox::worldgen() noexcept
{
    return *impl_->worldgen;
}

const std::shared_ptr<const world::CustomTerrain> &Sandbox::terrain() const noexcept
{
    return impl_->terrain;
}

const world::WorldGenerator &Sandbox::terrain_generator() const noexcept
{
    return impl_->terrain_generator;
}

bool Sandbox::start(std::span<const net::LoadedMod> mods, std::string &error)
{
    return impl_->start(mods, error);
}

void Sandbox::run(double seconds)
{
    constexpr double step = 1.0 / 60.0;
    for (double done = 0.0; done + step * 0.5 < seconds; done += step)
        impl_->step(step);
}

void Sandbox::stop()
{
    impl_->stop();
}

bool Sandbox::running() const noexcept
{
    return impl_->active;
}

const std::vector<modding::LogEntry> &Sandbox::log() const noexcept
{
    return impl_->log;
}

void Sandbox::set_echo(bool echo) noexcept
{
    impl_->echo = echo;
}

const std::vector<std::pair<std::string, std::string>> &Sandbox::refused() const noexcept
{
    return impl_->refused;
}

SandboxReport Sandbox::report() const
{
    SandboxReport report;
    for (const modding::LogEntry &entry : impl_->log)
    {
        report.errors += entry.level == modding::LogLevel::error ? 1U : 0U;
        report.warnings += entry.level == modding::LogLevel::warning ? 1U : 0U;
    }
    report.hud_commands = impl_->hud_commands;
    report.screen_commands = impl_->screen_commands;
    report.lights = impl_->graphics.lights().size();
    report.atmosphere_layers = impl_->graphics.atmosphere().size();
    report.models = impl_->graphics.models().entries().size();
    report.entities = impl_->gameplay->client_entities().size();
    return report;
}

modding::gameplay::ChatVerdict Sandbox::chat(std::string_view line)
{
    if (!impl_->gameplay->running())
        return modding::gameplay::ChatVerdict::pass;
    return impl_->gameplay->local_chat(line);
}

// ---------------------------------------------------------------------------
// One mod folder
// ---------------------------------------------------------------------------

bool load_mod_folder(const std::filesystem::path &folder, net::LoadedMod &mod, std::string &error)
{
    std::error_code fs_error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(folder, fs_error);
    if (fs_error || !std::filesystem::is_directory(root, fs_error))
    {
        error = folder.generic_string() + " is not a folder";
        return false;
    }
    if (!std::filesystem::is_regular_file(root / net::mod_descriptor_filename, fs_error))
    {
        error = "no openminerz.mod in " + root.generic_string();
        return false;
    }
    // Links and budgets first, exactly as the catalog checks them.
    std::vector<net::ModContentFile> files;
    if (!net::list_mod_content(root, files, error))
        return false;
    const auto pick = [&](const net::ModCatalog &catalog, const std::filesystem::path &wanted) {
        for (const net::LoadedMod &loaded : catalog.mods)
            if (std::filesystem::equivalent(loaded.root, wanted, fs_error))
            {
                mod = loaded;
                return true;
            }
        return false;
    };
    net::ModCatalog catalog;
    if (net::load_mod_catalog(root.parent_path(), catalog, error) && pick(catalog, root))
        return true;

    // A sibling broke the shared catalog: load a private copy instead (the
    // digest covers relative paths and bytes only, so it is the same).
    std::random_device seed;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path(fs_error) / ("openminerz-mod-sdk-" + std::to_string(seed()));
    const std::filesystem::path copy = scratch / root.filename();
    std::filesystem::create_directories(copy, fs_error);
    for (const net::ModContentFile &file : files)
    {
        const std::filesystem::path target = copy / std::filesystem::path{file.source}.lexically_relative(root);
        std::filesystem::create_directories(target.parent_path(), fs_error);
        std::filesystem::copy_file(file.source, target, std::filesystem::copy_options::overwrite_existing,
                                   fs_error);
    }
    bool loaded = net::load_mod_catalog(scratch, catalog, error) && pick(catalog, copy);
    if (loaded)
        mod.root = root;
    else if (error.empty())
        error = "the mod could not be read";
    std::filesystem::remove_all(scratch, fs_error);
    return loaded;
}

} // namespace openminerz::mod_sdk
