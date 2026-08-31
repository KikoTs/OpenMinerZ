#include "app.hpp"

#include "font_renderer.hpp"
#include "openminerz/asset_paths.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/game_mode.hpp"
#include "openminerz/input_contract.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/interop_state.hpp"
#include "openminerz/menu_model.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/host_session.hpp"
#include "openminerz/net/legacy_steam_runtime_client.hpp"
#include "openminerz/net/legacy_steam_runtime_host.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/net/session_browser.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/explosive.hpp"
#include "openminerz/world/loot.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/spawner.hpp"
#include "openminerz/world_save.hpp"
#include "original_sound_bank.hpp"
#include "static_model_asset.hpp"
#include "world_preview_renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz
{
namespace
{

using Clock = std::chrono::steady_clock;
using ui::Rect;

constexpr SDL_Color white{239U, 243U, 244U, 255U};
constexpr SDL_Color muted{143U, 153U, 157U, 255U};
constexpr SDL_Color red{226U, 42U, 48U, 255U};
constexpr SDL_Color red_bright{255U, 70U, 74U, 255U};
constexpr SDL_Color black{5U, 7U, 8U, 255U};

SDL_Color with_alpha(SDL_Color color, float alpha)
{
    color.a = static_cast<Uint8>(std::clamp(alpha, 0.0F, 1.0F) * 255.0F);
    return color;
}

PointerButton pointer_button(Uint8 button) noexcept
{
    if (button == SDL_BUTTON_LEFT)
        return PointerButton::primary;
    if (button == SDL_BUTTON_RIGHT)
        return PointerButton::secondary;
    return PointerButton::other;
}

SDL_Texture *load_bmp_texture(SDL_Renderer *renderer, const std::filesystem::path &path)
{
    SDL_Surface *surface = SDL_LoadBMP(path.string().c_str());
    if (surface == nullptr)
        return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture != nullptr)
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

std::filesystem::path default_settings_path()
{
#if defined(_WIN32)
    char *app_data{};
    std::size_t app_data_size{};
    if (_dupenv_s(&app_data, &app_data_size, "APPDATA") == 0 && app_data != nullptr)
    {
        const std::filesystem::path path =
            std::filesystem::path{app_data} / "OpenMinerZ" / "settings.cfg";
        std::free(app_data);
        return path;
    }
#endif
    return std::filesystem::current_path() / "settings.cfg";
}

std::filesystem::path default_online_positions_path()
{
#if defined(_WIN32)
    char *app_data{};
    std::size_t app_data_size{};
    if (_dupenv_s(&app_data, &app_data_size, "APPDATA") == 0 && app_data != nullptr)
    {
        const std::filesystem::path path =
            std::filesystem::path{app_data} / "OpenMinerZ" / "online-positions.cfg";
        std::free(app_data);
        return path;
    }
#endif
    return std::filesystem::current_path() / "online-positions.cfg";
}

struct OnlineRememberedPosition final
{
    world::Vec3 position;
    float yaw{};
    float pitch{};
};

bool valid_online_position(const OnlineRememberedPosition &remembered) noexcept
{
    return std::isfinite(remembered.position.x) && std::isfinite(remembered.position.y) &&
           std::isfinite(remembered.position.z) && std::isfinite(remembered.yaw) &&
           std::isfinite(remembered.pitch) && std::abs(remembered.position.x) <= 8'000'000.0F &&
           remembered.position.y >= static_cast<float>(world::world_min_y) - 16.0F &&
           remembered.position.y <=
               static_cast<float>(world::world_min_y + world::chunk_height) + 192.0F &&
           std::abs(remembered.position.z) <= 8'000'000.0F;
}

std::string online_world_key(const net::WorldInfoPayload &world_info)
{
    if (std::ranges::any_of(world_info.world_id, [](std::uint8_t value) { return value != 0U; }))
    {
        std::string key;
        key.reserve(world_info.world_id.size() * 2U);
        for (const std::uint8_t value : world_info.world_id)
            key += std::format("{:02x}", static_cast<unsigned int>(value));
        return key;
    }

    // Some old Classic saves have an empty Guid. Keep those worlds distinct
    // using immutable manifest fields instead of falling back to one global slot.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::string_view text) {
        for (const unsigned char character : text)
            hash = (hash ^ character) * 1099511628211ULL;
    };
    mix(world_info.name);
    mix(world_info.owner_gamer_tag);
    hash = (hash ^ static_cast<std::uint32_t>(world_info.seed)) * 1099511628211ULL;
    return std::format("legacy-{:08x}-{:016x}",
                       static_cast<std::uint32_t>(world_info.seed), hash);
}

std::map<std::string, OnlineRememberedPosition> read_online_positions()
{
    std::map<std::string, OnlineRememberedPosition> positions;
    std::ifstream input{default_online_positions_path()};
    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream fields{line};
        std::string key;
        OnlineRememberedPosition remembered;
        if (fields >> key >> remembered.position.x >> remembered.position.y >>
                remembered.position.z >> remembered.yaw >> remembered.pitch &&
            key.size() <= 80U && valid_online_position(remembered))
            positions.insert_or_assign(std::move(key), remembered);
    }
    return positions;
}

void write_online_position(std::string_view key, const OnlineRememberedPosition &remembered)
{
    if (key.empty() || !valid_online_position(remembered))
        return;
    auto positions = read_online_positions();
    positions.insert_or_assign(std::string{key}, remembered);
    const std::filesystem::path path = default_online_positions_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return;
    std::ofstream output{path, std::ios::trunc};
    output.precision(std::numeric_limits<float>::max_digits10);
    for (const auto &[world_key, position] : positions)
        output << world_key << ' ' << position.position.x << ' ' << position.position.y << ' '
               << position.position.z << ' ' << position.yaw << ' ' << position.pitch << '\n';
}

float approach(float current, float target, float speed, float delta_seconds)
{
    return target + (current - target) * std::exp(-speed * delta_seconds);
}

float smoothstep(float value)
{
    value = std::clamp(value, 0.0F, 1.0F);
    return value * value * (3.0F - 2.0F * value);
}

std::string uppercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string system_profile_name()
{
#if defined(_WIN32)
    char *user_name{};
    std::size_t user_name_size{};
    if (_dupenv_s(&user_name, &user_name_size, "USERNAME") == 0 && user_name != nullptr)
    {
        std::string result{user_name};
        std::free(user_name);
        return uppercase(std::move(result));
    }
#else
    if (const char *user_name = std::getenv("USER"); user_name != nullptr)
    {
        return uppercase(user_name);
    }
#endif
    return "PLAYER";
}

enum class HitKind
{
    menu_item,
    setting_row,
    tab,
    back
};

enum class HostTextField
{
    none,
    server_name,
    password,
    join_password,
    direct_address
};

struct HitRegion final
{
    Rect rect;
    HitKind kind{};
    std::size_t index{};
};

enum class CraftHitKind
{
    recipe,       // index = cookbook recipe index
    ingredient,   // index = ingredient slot of the selected recipe
    create,       // the CREATE ITEM button
    mode_classic, // style toggle segments
    mode_modern,
    category_cycle, // classic CATEGORY button (LMB next, RMB previous)
    tab,            // modern category tab, index into craft_categories
};

struct CraftHit final
{
    Rect rect;
    CraftHitKind kind{};
    std::size_t index{};
};

struct CraftCategoryEntry final
{
    int id{};
    const char *label{};
};

// CraftingUIScreen's category cycle order: All, Tools, Special, Firearms,
// Ammo, Components, Blocks (the ints are the original Category values).
constexpr std::array<CraftCategoryEntry, 7> craft_categories{{
    {-1, "ALL"},
    {1, "TOOLS"},
    {2, "SPECIAL"},
    {3, "FIREARMS"},
    {6, "AMMO"},
    {4, "COMPONENTS"},
    {5, "BLOCKS"},
}};

struct Palette final
{
    SDL_Color panel;
    SDL_Color panel_selected;
    SDL_Color line;
    SDL_Color text;
    SDL_Color secondary;
    SDL_Color accent;
};

Palette palette(bool high_contrast)
{
    if (high_contrast)
    {
        return {{2U, 3U, 4U, 245U},       {28U, 8U, 10U, 255U},     {210U, 218U, 220U, 210U},
                {255U, 255U, 255U, 255U}, {206U, 214U, 216U, 255U}, {255U, 57U, 63U, 255U}};
    }
    return {{8U, 11U, 12U, 218U}, {31U, 17U, 18U, 238U}, {87U, 99U, 102U, 125U}, white, muted, red};
}

float terrain_shadow_strength(QualityLevel quality) noexcept
{
    switch (quality)
    {
    case QualityLevel::low:
        return 0.25F;
    case QualityLevel::medium:
        return 0.45F;
    case QualityLevel::high:
        return 0.68F;
    case QualityLevel::ultra:
        return 0.86F;
    }
    return 0.68F;
}

float particle_density(QualityLevel quality) noexcept
{
    switch (quality)
    {
    case QualityLevel::low:
        return 0.25F;
    case QualityLevel::medium:
        return 0.55F;
    case QualityLevel::high:
        return 1.0F;
    case QualityLevel::ultra:
        return 1.5F;
    }
    return 1.0F;
}

} // namespace

class Application::Impl final
{
  public:
    explicit Impl(LaunchOptions options)
        : options_(std::move(options)), store_(default_settings_path()), settings_(store_.load())
    {
        if (options_.asset_style >= 0)
            settings_.texture_style =
                options_.asset_style == 0 ? AssetStyle::classic : AssetStyle::modern;
        if (options_.presentation_profile >= 0)
            settings_.presentation = options_.presentation_profile == 0
                                         ? PresentationProfile::classic
                                         : PresentationProfile::rebirth;
        loaded_asset_style_ = settings_.texture_style;
        set_active_asset_style(loaded_asset_style_);
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, settings_.raw_mouse_input ? "0" : "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            throw std::runtime_error{std::string{"SDL initialization failed: "} + SDL_GetError()};
        }

        const int width = options_.width > 0 ? options_.width : settings_.window_width;
        const int height = options_.height > 0 ? options_.height : settings_.window_height;
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options_.screenshot_path.has_value())
        {
            flags |= SDL_WINDOW_HIDDEN;
        }
        if (!SDL_CreateWindowAndRenderer("OpenMinerZ", width, height, flags, &window_, &renderer_))
        {
            SDL_Quit();
            throw std::runtime_error{std::string{"window creation failed: "} + SDL_GetError()};
        }
        SDL_SetWindowMinimumSize(window_, 960, 540);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderVSync(renderer_, settings_.vsync ? 1 : 0);
        open_first_gamepad();
        craft_modern_ = options_.craft_modern.value_or(settings_.crafting_menu_modern);
        game_mode_ = static_cast<GameMode>(std::clamp(settings_.game_mode, 0, 5));
        difficulty_ = static_cast<GameDifficulty>(std::clamp(settings_.game_difficulty, 0, 3));
        host_settings_.server_name = profile_name_ + "'S FRONTIER";
        join_client_manifest_.protocol = net::ProtocolVersion::classic_v3;
        join_browser_.set_client(join_client_manifest_, true);
        font_ = std::make_unique<FontRenderer>(renderer_, find_default_font());
        const auto derived = original_derived_data_path();
        sniper_scope_texture_ = load_bmp_texture(renderer_, derived / "sniper_scope.bmp");
        missile_lock_texture_ = load_bmp_texture(renderer_, derived / "missile_lock.bmp");
        missile_locking_texture_ = load_bmp_texture(renderer_, derived / "missile_locking.bmp");
        pickaxe_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pickaxe.omsm");
        compass_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "compass.omsm");
        pistol_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "pistol.omsm");
        knife_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "knife.omsm");
        ammo_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ammo.omsm");
        torch_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "torch.omsm");
        spade_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "spade.omsm");
        axe_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "axe.omsm");
        ak_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ak.omsm");
        shotgun_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "shotgun.omsm");
        rifle_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rifle.omsm");
        smg_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "smg.omsm");
        lmg_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "lmg.omsm");
        rpg_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "rpg.omsm");
        grenade_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "grenade.omsm");
        ore_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "ore.omsm");
        gems_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "gems.omsm");
        bars_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "bars.omsm");
        space_ak_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "space_ak.omsm");
        space_pistol_art_ =
            std::make_unique<StaticModelAsset>(renderer_, derived / "space_pistol.omsm");
        space_shotgun_art_ =
            std::make_unique<StaticModelAsset>(renderer_, derived / "space_shotgun.omsm");
        space_rifle_art_ =
            std::make_unique<StaticModelAsset>(renderer_, derived / "space_rifle.omsm");
        space_smg_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "space_smg.omsm");
        saber_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "saber.omsm");
        locator_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "locator.omsm");
        teleporter_art_ =
            std::make_unique<StaticModelAsset>(renderer_, derived / "teleporter.omsm");
        clock_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "clock.omsm");
        gunpowder_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "gunpowder.omsm");
        grenade_icon_art_ = std::make_unique<StaticModelAsset>(renderer_, derived / "grenade.omsm");
        const int launch_view_distance =
            options_.view_distance > 0 ? options_.view_distance : settings_.view_distance;
        world_preview_ = std::make_unique<WorldPreviewRenderer>(
            renderer_, world_seed_, launch_view_distance, options_.world_biome);
        world_preview_->set_doors(&custom_doors_);
        world_preview_->set_particle_density(particle_density(settings_.particle_quality));
        if (options_.tone_mapping >= 0)
            settings_.tone_mapping = static_cast<ToneMapping>(options_.tone_mapping);
        apply_grading_settings();
        enemy_horde_.set_clip_durations([this](world::EnemyModelKind model, std::string_view clip) {
            return world_preview_ != nullptr && world_preview_->enemy_models() != nullptr
                       ? world_preview_->enemy_models()->clip_duration(model, clip)
                       : 0.0F;
        });
        audio_outputs_ = enumerate_audio_output_devices();
        auto selected_output =
            std::find_if(audio_outputs_.begin(), audio_outputs_.end(), [&](const auto &output) {
                return output.id == settings_.audio_output_id;
            });
        if (selected_output == audio_outputs_.end())
        {
            std::cerr << "Saved audio output is unavailable; using the Windows system default\n";
            settings_.audio_output_id.clear();
            selected_output = audio_outputs_.begin();
            dirty_ = true;
        }
        if (selected_output != audio_outputs_.end() &&
            settings_.audio_output_name != selected_output->name)
        {
            settings_.audio_output_name = selected_output->name;
            dirty_ = true;
        }
        sound_bank_ = std::make_unique<OriginalSoundBank>(settings_.audio_output_id);
        apply_audio_settings();
        const bool audio_ready = sound_bank_->available();
        if (audio_ready && !sound_bank_->output_name().empty())
        {
            const std::string resolved_name =
                settings_.audio_output_id.empty()
                    ? std::format("System default ({})", sound_bank_->output_name())
                    : std::string{sound_bank_->output_name()};
            if (settings_.audio_output_name != resolved_name)
            {
                settings_.audio_output_name = resolved_name;
                dirty_ = true;
            }
        }
        const bool windows_audio_muted = audio_ready && sound_bank_->output_muted();
        const float windows_audio_volume = audio_ready ? sound_bank_->output_volume() : -1.0F;
        const bool windows_audio_silent =
            windows_audio_volume >= 0.0F && windows_audio_volume <= 0.001F;
        std::cout << "Audio bank: "
                  << (!audio_ready           ? "unavailable"
                      : windows_audio_muted  ? "ready (Windows output muted)"
                      : windows_audio_silent ? "ready (Windows output volume is zero)"
                                             : "ready")
                  << (windows_audio_volume >= 0.0F
                          ? std::format(
                                "; Windows endpoint volume {}%",
                                static_cast<int>(std::lround(windows_audio_volume * 100.0F)))
                          : "")
                  << (sound_bank_->output_name().empty()
                          ? ""
                          : std::format("; output {}", sound_bank_->output_name()))
                  << '\n';
        if (windows_audio_muted)
            toast("WINDOWS AUDIO IS MUTED", "UNMUTE THE ACTIVE OUTPUT TO HEAR MUSIC AND EFFECTS");
        else if (windows_audio_silent)
            toast("WINDOWS AUDIO VOLUME IS ZERO",
                  "RAISE THE ACTIVE OUTPUT TO HEAR MUSIC AND EFFECTS");
        if (!options_.screenshot_path.has_value() && !options_.open_world)
        {
            sound_bank_->play_menu_music();
            // FrontEndScreen starts the daytime Birds bed alongside Theme.
            sound_bank_->set_ambience(1.0F, 0.0F, 0.0F, 0.0F, 1.0F);
        }
        if (options_.no_enemies)
            difficulty_ = GameDifficulty::no_enemies;
        else if (options_.spawn_enemy >= 0 && difficulty_ == GameDifficulty::no_enemies)
            // An explicit deterministic actor capture must not be erased by a
            // persisted NO ENEMIES setting on the first simulation tick.
            difficulty_ = GameDifficulty::hard;
        spawn_player();
        if (options_.preview_item.has_value())
        {
            inventory_.select(0U);
            const auto item = static_cast<InventoryItemId>(*options_.preview_item);
            if (item == InventoryItemId::bare_hands)
                inventory_.tray()[0].reset();
            else
                inventory_.tray()[0] = InventoryStack{item, 1U};
            sync_selected_item();
        }
        if (options_.preview_pickup_item.has_value())
        {
            const InventoryItemId item =
                static_cast<InventoryItemId>(*options_.preview_pickup_item);
            const world::Vec3 forward = player_.view_direction();
            world_preview_->spawn_created_stack_at(
                InventoryStack{item, 1U},
                {player_.state().position.x + forward.x * 1.25F,
                 player_.state().position.y + world::PlayerSimulation::eye_height +
                     forward.y * 1.25F,
                 player_.state().position.z + forward.z * 1.25F});
        }
        focus_.resize(16U, 0.0F);
        scan_addons();
        if (options_.open_settings)
        {
            menu_.open(ui::MenuScreen::settings);
        }
        else if (options_.open_host_settings)
        {
            menu_.open(options_.host_settings_section == 1   ? ui::MenuScreen::host_details
                       : options_.host_settings_section == 2 ? ui::MenuScreen::host_access
                       : options_.host_settings_section == 3 ? ui::MenuScreen::host_gameplay
                                                             : ui::MenuScreen::host_setup);
        }
        else if (options_.diagnostic_online_loading)
        {
            menu_.open(ui::MenuScreen::join_browser);
            online_join_stage_ = OnlineJoinStage::syncing_chunks;
            online_join_server_name_ = "KIKOTS'S SERVER";
            online_join_host_name_ = "KIRILCHOALTOV";
            online_join_detail_ = "32 OF 46 AUTHORITATIVE WORLD CHUNKS VERIFIED";
            online_sync_chunks_received_ = 32U;
            online_sync_chunks_total_ = 46U;
            online_persona_name_ = "KikoTs";
            steam_identity_verified_ = true;
            online_client_protocol_ = net::ProtocolVersion::modern_v4;
        }
        else if (options_.diagnostic_join_browser || options_.diagnostic_join_details)
        {
            menu_.open(ui::MenuScreen::join_browser);
            refresh_join_sessions(true);
            if (options_.diagnostic_join_details && !join_row_sessions_.empty())
            {
                selected_join_session_ = join_row_sessions_.front();
                menu_.open(ui::MenuScreen::join_details);
            }
        }
        else if (options_.open_join_browser || options_.open_join_details)
        {
            menu_.open(ui::MenuScreen::join_browser);
            refresh_join_sessions(false);
        }
        else if (options_.open_world_select)
        {
            refresh_world_list();
            menu_.open(ui::MenuScreen::world_select);
        }
        else if (options_.open_world)
        {
            menu_.open(ui::MenuScreen::world_preview);
            waiting_for_world_ = !world_preview_->preload_complete();
            inventory_open_ = options_.open_inventory;
            inventory_focus_ = PlayerInventory::storage_size + inventory_.selected_index();
            if (!options_.screenshot_path.has_value())
            {
                SDL_SetWindowRelativeMouseMode(window_, !waiting_for_world_ && !inventory_open_);
            }
        }
        if (options_.diagnostic_multiplayer)
        {
            online_persona_name_ = "KikoTs";
            steam_identity_verified_ = true;
            online_local_gid_ = 7U;
            const world::Vec3 origin = player_.state().position;
            const world::Vec3 forward = player_.view_direction();
            const world::Vec3 right{-forward.z, 0.0F, forward.x};
            const char *avatar_case_value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_AVATAR_CASE");
            const std::string_view avatar_case =
                avatar_case_value == nullptr ? std::string_view{} : avatar_case_value;
            const auto place_remote = [&](OnlineRemotePlayer &remote, float distance,
                                          float lateral) {
                remote.state.position = {origin.x + forward.x * distance + right.x * lateral,
                                         origin.y,
                                         origin.z + forward.z * distance + right.z * lateral};
                remote.state.yaw = std::atan2(origin.x - remote.state.position.x,
                                              -(origin.z - remote.state.position.z));
                remote.has_update = true;
                remote.last_update = elapsed_;
            };
            OnlineRemotePlayer host;
            host.gamer_tag = avatar_case.empty() ? "KirilchoAltov" : "AVATAR TEST";
            place_remote(host, avatar_case.empty() ? 2.45F : 1.65F,
                         avatar_case.empty() ? -0.72F : 0.0F);
            host.state.animation_mode = PlayerAnimationMode::fist;
            if (avatar_case == "rifle")
            {
                host.state.animation_mode = PlayerAnimationMode::assault;
                host.carried_item =
                    static_cast<std::uint16_t>(InventoryItemId::assault_rifle);
            }
            else if (avatar_case == "pick")
            {
                host.state.animation_mode = PlayerAnimationMode::pick;
                host.state.using_tool = true;
                host.carried_item =
                    static_cast<std::uint16_t>(InventoryItemId::iron_pickaxe);
            }
            online_remote_players_[0U] = host;
            if (avatar_case.empty())
            {
                OnlineRemotePlayer rifle;
                rifle.gamer_tag = "CastleForgeDev";
                place_remote(rifle, 2.65F, 0.72F);
                rifle.state.animation_mode = PlayerAnimationMode::assault;
                rifle.carried_item =
                    static_cast<std::uint16_t>(InventoryItemId::assault_rifle);
                online_remote_players_[3U] = rifle;

                OnlineRemotePlayer explorer;
                explorer.gamer_tag = "Tool Test";
                place_remote(explorer, 3.8F, 1.65F);
                explorer.state.movement_x = 0.65F;
                explorer.state.movement_y = 0.70F;
                explorer.state.animation_mode = PlayerAnimationMode::pick;
                explorer.state.using_tool = true;
                explorer.carried_item =
                    static_cast<std::uint16_t>(InventoryItemId::iron_pickaxe);
                online_remote_players_[4U] = explorer;
            }
            push_online_chat("KirilchoAltov: Welcome to the frontier.");
            push_online_chat("KikoTs: Avatar sync is online.");
            player_roster_visible_ =
                SDL_getenv("OPENMINERZ_DIAGNOSTIC_HIDE_ROSTER") == nullptr;
            refresh_online_remote_visuals();
        }
        if (options_.world_time.has_value())
        {
            day_night_.set_time(*options_.world_time);
            world_time_speed_ = 0.0F;
        }
        const bool deterministic_launch =
            options_.screenshot_path.has_value() || options_.bench_frames > 0 ||
            options_.open_world || options_.open_world_select || options_.diagnostic_join_browser ||
            options_.diagnostic_join_details || options_.diagnostic_online_loading ||
            options_.diagnostic_multiplayer;
        const bool live_browser_started = options_.open_join_browser || options_.open_join_details;
        if (!deterministic_launch && !live_browser_started)
            begin_online_discovery(true);
        last_transition_serial_ = menu_.transition_serial();
        std::cout << "OpenMinerZ frontend initialized with " << SDL_GetRendererName(renderer_)
                  << '\n';
        std::cout << "Settings: " << store_.path().string() << '\n';
        std::cout << "Content pack: "
                  << (loaded_asset_style_ == AssetStyle::classic ? "Classic" : "Modern") << " ("
                  << active_content_pack_path().string() << ")\n";
    }

    ~Impl()
    {
        shutdown_online_client();
        shutdown_online_host();
        world_slots_.clear();
        if (text_editing())
            SDL_StopTextInput(window_);
        sound_bank_.reset();
        world_preview_.reset();
        SDL_DestroyTexture(sniper_scope_texture_);
        SDL_DestroyTexture(missile_lock_texture_);
        SDL_DestroyTexture(missile_locking_texture_);
        for (auto *asset : {&pickaxe_art_,     &compass_art_,      &pistol_art_,
                            &knife_art_,       &ammo_art_,         &torch_art_,
                            &spade_art_,       &axe_art_,          &ak_art_,
                            &shotgun_art_,     &rifle_art_,        &smg_art_,
                            &lmg_art_,         &rpg_art_,          &grenade_art_,
                            &ore_art_,         &gems_art_,         &bars_art_,
                            &space_ak_art_,    &space_pistol_art_, &space_shotgun_art_,
                            &space_rifle_art_, &space_smg_art_,    &saber_art_,
                            &locator_art_,     &teleporter_art_,   &clock_art_,
                            &gunpowder_art_,   &grenade_icon_art_})
            asset->reset();
        font_.reset();
        if (gamepad_ != nullptr)
        {
            SDL_CloseGamepad(gamepad_);
            gamepad_ = nullptr;
        }
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int run()
    {
        const bool bench = options_.bench_frames > 0;
        std::size_t screenshot_frame_threshold = 2U;
        if (options_.diagnostic_multiplayer)
            if (const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_CAPTURE_FRAMES"))
                screenshot_frame_threshold = static_cast<std::size_t>(
                    std::clamp(std::atoi(value), 2, 600));
        std::vector<double> bench_samples;
        int bench_warmup = 90;
        int browser_capture_ready_frames{};
        if (bench)
        {
            bench_samples.reserve(static_cast<std::size_t>(options_.bench_frames));
            SDL_SetRenderVSync(renderer_, 0);
        }
        auto last_frame = Clock::now();
        while (running_)
        {
            const auto frame_start = Clock::now();
            float delta = std::chrono::duration<float>(frame_start - last_frame).count();
            last_frame = frame_start;
            delta = std::clamp(delta, 0.0F, 0.05F);

            SDL_Event event{};
            while (SDL_PollEvent(&event))
            {
                process_event(event);
            }
            update(delta);
            render();

            const bool live_browser_capture =
                options_.screenshot_path.has_value() &&
                (options_.open_join_browser || options_.open_join_details);
            if (live_browser_capture && join_browser_state_ != JoinBrowserState::searching)
                ++browser_capture_ready_frames;
            else
                browser_capture_ready_frames = 0;
            const bool browser_capture_ready =
                !live_browser_capture || browser_capture_ready_frames >= 2;
            if (options_.screenshot_path.has_value() &&
                frames_rendered_ >= screenshot_frame_threshold &&
                (!options_.open_world || world_ready_frames_ >= 2U) && browser_capture_ready)
            {
                capture(*options_.screenshot_path);
                running_ = false;
            }
            ++frames_rendered_;

            if (bench && menu_.screen() == ui::MenuScreen::world_preview && !waiting_for_world_)
            {
                // Sweep the camera slowly while walking so the measured frames cover
                // chunk streaming and remeshing along a wide arc, not one cached angle.
                player_.look(delta * 0.15F, 0.0F);
                if (bench_warmup > 0)
                {
                    --bench_warmup;
                }
                else
                {
                    bench_samples.push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - frame_start)
                            .count());
                    if (bench_samples.size() >= static_cast<std::size_t>(options_.bench_frames))
                    {
                        std::vector<double> sorted = bench_samples;
                        std::sort(sorted.begin(), sorted.end());
                        double total = 0.0;
                        for (const double sample : sorted)
                            total += sample;
                        const double average = total / static_cast<double>(sorted.size());
                        const double median = sorted[sorted.size() / 2U];
                        const double percentile_95 = sorted[static_cast<std::size_t>(
                            static_cast<double>(sorted.size() - 1U) * 0.95)];
                        SDL_Log("BENCH frames=%zu avg=%.3fms median=%.3fms p95=%.3fms "
                                "max=%.3fms fps=%.1f",
                                sorted.size(), average, median, percentile_95, sorted.back(),
                                1000.0 / average);
                        running_ = false;
                    }
                }
                continue; // No frame limiter while measuring.
            }

            if (!settings_.vsync && settings_.frame_limit > 0)
            {
                const double target = 1.0 / static_cast<double>(settings_.frame_limit);
                const double elapsed =
                    std::chrono::duration<double>(Clock::now() - frame_start).count();
                if (elapsed < target)
                {
                    SDL_Delay(static_cast<Uint32>((target - elapsed) * 1000.0));
                }
            }
        }
        int exit_code{};
        if (ui_join_probe_failed_)
            exit_code = 1;
        if (options_.state_dump_path.has_value())
        {
            std::string state_error;
            if (!write_interop_state_dump(*options_.state_dump_path, state_error))
            {
                std::cerr << "interop.state.failure=" << state_error << '\n';
                exit_code = 1;
            }
        }
        save_settings();
        return exit_code;
    }

  private:
    using CrateKey = WorldSaveCrateKey;
    static constexpr std::size_t crate_slot_count = world_save_crate_slot_count;
    static constexpr std::size_t crate_slot_base =
        PlayerInventory::storage_size + PlayerInventory::tray_size;
    using CrateSlots = WorldSaveCrateSlots;
    using CrateMap = WorldSaveCrateMap;
    using DoorMap = WorldSaveDoorMap;
    using SpawnerMap = WorldSaveSpawnerMap;

    struct OnlineHostStartResult final
    {
        std::unique_ptr<net::LegacySteamRuntimeHost> host;
        net::SteamBridgeHostReady ready;
        std::string error;
    };

    enum class OnlineClientOperation : std::uint8_t
    {
        none,
        discovery,
        join
    };

    enum class OnlineJoinStage : std::uint8_t
    {
        idle,
        joining_lobby,
        awaiting_admission,
        receiving_world,
        syncing_profile,
        syncing_chunks,
        preparing_world
    };

    enum class JoinBrowserState : std::uint8_t
    {
        idle,
        searching,
        ready,
        failed,
        diagnostic
    };

    struct OnlineClientOperationResult final
    {
        std::unique_ptr<net::LegacySteamRuntimeClient> client;
        net::SteamBridgeDiscoveryResults discovery;
        net::SteamBridgeJoinReady ready;
        std::string error;
    };

    struct OnlineRemotePlayer final
    {
        std::string gamer_tag;
        net::RemotePlayerReplicationState state;
        std::uint16_t carried_item{};
        std::int64_t connected_ticks{};
        net::IntVector3 focus_crate{};
        std::int32_t focus_item_x{};
        std::int32_t focus_item_y{};
        float last_update{};
        float death_animation_started{};
        bool presence_announced{};
        bool has_update{};
    };

    struct PendingPrePresencePacket final
    {
        net::LegacySteamRuntimeClientEvent event;
        std::string message_name;
    };

    struct OnlineChatLine final
    {
        std::string message;
        float received_at{};
    };

    struct OnlineMeleeTarget final
    {
        std::uint8_t gid{};
        world::Vec3 intersection;
    };

    struct LocalSpawnerRuntime final
    {
        std::unique_ptr<world::SpawnerSimulation> simulation;
        world::SpawnerTarget fallback_target;
    };

    struct PendingRemoteSpawnerTrigger final
    {
        float expires_at{};
        std::optional<CrateKey> location;
    };

    struct RemoteLootClaim final
    {
        net::PeerId owner{};
        world::BlockType block{world::BlockType::empty};
        std::uint8_t accepted_drops{};
        std::uint8_t rare_eligible_drops{};
        float expires_at{};
        bool block_removed{};
    };

    enum class NetworkProjectileKind : std::uint8_t
    {
        grenade,
        rocket
    };

    struct RecentNetworkProjectile final
    {
        NetworkProjectileKind kind{NetworkProjectileKind::grenade};
        std::uint8_t owner_gid{};
        std::uint16_t type{};
        world::Vec3 position{};
        float expires{};
    };

    [[nodiscard]] bool world_controls_active() const noexcept
    {
        return world_controls_enabled(menu_.screen() == ui::MenuScreen::world_preview,
                                      waiting_for_world_);
    }

    [[nodiscard]] bool text_editing() const noexcept
    {
        return chat_editing_ || locator_name_editing_ || host_text_field_ != HostTextField::none;
    }

    void process_event(SDL_Event event)
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            running_ = false;
            return;
        }
        if (online_join_stage_ != OnlineJoinStage::idle &&
            (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
             event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN))
        {
            return;
        }
        SDL_ConvertEventToRenderCoordinates(renderer_, &event);
        switch (event.type)
        {
        case SDL_EVENT_MOUSE_MOTION:
            note_mouse_activity();
            if (world_controls_active() && !inspector_view_ && !inventory_open_ && !text_editing())
            {
                const float sensitivity = settings_.mouse_sensitivity * 0.0022F;
                player_.look(event.motion.xrel * sensitivity,
                             -event.motion.yrel * sensitivity *
                                 (settings_.invert_y ? -1.0F : 1.0F));
                break;
            }
            mouse_x_ = event.motion.x;
            mouse_y_ = event.motion.y;
            hover_at(mouse_x_, mouse_y_);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            note_mouse_button(event.button.button, true);
            if (text_editing())
            {
                break;
            }
            if (world_controls_active() && player_life_ != PlayerLife::alive)
            {
                // KillPlayer -> the click-to-respawn contract.
                if (player_life_ == PlayerLife::dead)
                    respawn_player();
            }
            else if (world_controls_active() && inventory_open_)
            {
                inventory_pointer(event.button.x, event.button.y, event.button.button);
            }
            else if (world_controls_active() && !inspector_view_)
            {
                const WorldPointerPress press =
                    world_pointer_press(pointer_button(event.button.button));
                if (press.primary_action)
                {
                    primary_action_pressed();
                }
                else if (press.activate)
                {
                    // CastleMinerZControllerMapping maps right mouse to BOTH
                    // Activate and Shoulder. The HUD processes block activation
                    // first, then the selected item sees the same press.
                    (void)try_activate_target();
                    const InventoryItemId item = inventory_.selected_item();
                    if (selected_gun() != nullptr)
                        shouldering_ = true;
                    else if (item == InventoryItemId::teleport_gps)
                    {
                        (void)teleport_to_selected_waypoint();
                        secondary_action_down_ = false;
                    }
                    else if (item == InventoryItemId::gps)
                        secondary_action_down_ = false;
                    else if (press.secondary_action)
                        secondary_action_down_ = true;
                }
            }
            else if (menu_.screen() != ui::MenuScreen::world_preview &&
                     event.button.button == SDL_BUTTON_LEFT)
            {
                click_at(event.button.x, event.button.y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            note_mouse_button(event.button.button, false);
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                if (grenade_cooking_ && world_controls_active())
                    throw_grenade();
                mine_button_down_ = false;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                secondary_action_down_ = false;
                shouldering_ = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            note_mouse_activity();
            if (text_editing())
                break;
            if (world_controls_active() && !inventory_open_)
            {
                select_hotbar(event.wheel.y > 0.0F ? -1 : 1);
            }
            else if (world_controls_active() && inventory_open_)
            {
                inventory_wheel(event.wheel.y);
            }
            else if (menu_.screen() != ui::MenuScreen::world_preview)
            {
                navigate_menu(0, event.wheel.y > 0.0F ? -1 : 1);
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (host_text_field_ != HostTextField::none)
                apply_host_text_event(event.text.text);
            else if (locator_name_editing_)
                apply_locator_name_text_event(event.text.text);
            else if (chat_editing_)
                apply_chat_text_event(event.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat || (text_editing() && event.key.key == SDLK_BACKSPACE))
            {
                note_keyboard_activity();
                process_key(event.key.key);
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (event.key.key == SDLK_TAB)
                player_roster_visible_ = false;
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            process_gamepad(event.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (gamepad_ == nullptr)
            {
                gamepad_ = SDL_OpenGamepad(event.gdevice.which);
                gamepad_input_active_ = false;
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad_ != nullptr && SDL_GetGamepadID(gamepad_) == event.gdevice.which)
            {
                SDL_CloseGamepad(gamepad_);
                gamepad_ = nullptr;
                gamepad_input_active_ = false;
                open_first_gamepad();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            window_focused_ = true;
            apply_audio_settings();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            window_focused_ = false;
            mouse_primary_down_ = false;
            mouse_secondary_down_ = false;
            gamepad_input_active_ = false;
            player_roster_visible_ = false;
            cancel_world_action();
            apply_audio_settings();
            break;
        default:
            break;
        }
    }

    void process_key(SDL_Keycode key)
    {
        if (chat_editing_)
        {
            switch (key)
            {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                finish_chat_edit(true);
                break;
            case SDLK_ESCAPE:
                finish_chat_edit(false);
                break;
            case SDLK_BACKSPACE:
                chat_key_fallback_.clear();
                erase_chat_character();
                break;
            default: {
                std::string fallback;
                if (key >= SDLK_A && key <= SDLK_Z)
                {
                    char letter = static_cast<char>('a' + (key - SDLK_A));
                    const SDL_Keymod modifiers = SDL_GetModState();
                    if (((modifiers & SDL_KMOD_SHIFT) != 0U) != ((modifiers & SDL_KMOD_CAPS) != 0U))
                        letter =
                            static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
                    fallback.push_back(letter);
                }
                else if (key >= SDLK_0 && key <= SDLK_9)
                    fallback.push_back(static_cast<char>('0' + (key - SDLK_0)));
                else if (key == SDLK_SPACE)
                    fallback.push_back(' ');
                else if (key == SDLK_MINUS)
                    fallback.push_back('-');
                else if (key == SDLK_PERIOD)
                    fallback.push_back('.');
                else if (key == SDLK_COMMA)
                    fallback.push_back(',');
                if (!fallback.empty())
                {
                    const std::size_t before = locator_name_characters(chat_draft_);
                    append_chat_text(fallback);
                    chat_key_fallback_ =
                        locator_name_characters(chat_draft_) > before ? fallback : std::string{};
                }
                break;
            }
            }
            return;
        }
        if (host_text_field_ != HostTextField::none)
        {
            switch (key)
            {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                finish_host_text_edit(true);
                break;
            case SDLK_ESCAPE:
                finish_host_text_edit(false);
                break;
            case SDLK_BACKSPACE:
                host_text_fallback_.clear();
                erase_host_text_character();
                break;
            default: {
                std::string fallback;
                if (key >= SDLK_A && key <= SDLK_Z)
                {
                    char letter = static_cast<char>('a' + (key - SDLK_A));
                    const SDL_Keymod modifiers = SDL_GetModState();
                    if (((modifiers & SDL_KMOD_SHIFT) != 0U) != ((modifiers & SDL_KMOD_CAPS) != 0U))
                        letter =
                            static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
                    fallback.push_back(letter);
                }
                else if (key >= SDLK_0 && key <= SDLK_9)
                    fallback.push_back(static_cast<char>('0' + (key - SDLK_0)));
                else if (key == SDLK_SPACE)
                    fallback.push_back(' ');
                else if (key == SDLK_MINUS)
                    fallback.push_back('-');
                else if (key == SDLK_PERIOD)
                    fallback.push_back('.');
                if (!fallback.empty())
                {
                    const std::size_t before = locator_name_characters(host_text_draft_);
                    append_host_text(fallback);
                    host_text_fallback_ = locator_name_characters(host_text_draft_) > before
                                              ? fallback
                                              : std::string{};
                }
                break;
            }
            }
            return;
        }
        if (locator_name_editing_)
        {
            switch (key)
            {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                finish_locator_name_edit(true);
                break;
            case SDLK_ESCAPE:
                finish_locator_name_edit(false);
                break;
            case SDLK_BACKSPACE:
                locator_key_fallback_.clear();
                erase_locator_name_character();
                break;
            default: {
                std::string fallback;
                if (key >= SDLK_A && key <= SDLK_Z)
                {
                    char letter = static_cast<char>('a' + (key - SDLK_A));
                    const SDL_Keymod modifiers = SDL_GetModState();
                    if (((modifiers & SDL_KMOD_SHIFT) != 0U) != ((modifiers & SDL_KMOD_CAPS) != 0U))
                        letter =
                            static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
                    fallback.push_back(letter);
                }
                else if (key >= SDLK_0 && key <= SDLK_9)
                    fallback.push_back(static_cast<char>('0' + (key - SDLK_0)));
                else if (key == SDLK_SPACE)
                    fallback.push_back(' ');
                else if (key == SDLK_MINUS)
                    fallback.push_back('-');
                if (!fallback.empty())
                {
                    const std::size_t before = locator_name_characters(locator_name_draft_);
                    append_locator_name_text(fallback);
                    locator_key_fallback_ = locator_name_characters(locator_name_draft_) > before
                                                ? fallback
                                                : std::string{};
                }
                break;
            }
            }
            return;
        }
        if (menu_.screen() == ui::MenuScreen::world_preview)
        {
            if (waiting_for_world_)
            {
                if (key == SDLK_ESCAPE)
                {
                    cancel_world_action();
                    SDL_SetWindowRelativeMouseMode(window_, false);
                    menu_.open(ui::MenuScreen::pause);
                }
                return;
            }
            process_world_key(key);
            return;
        }
        switch (key)
        {
        case SDLK_UP:
        case SDLK_W:
            navigate_menu(0, -1);
            break;
        case SDLK_DOWN:
        case SDLK_S:
            navigate_menu(0, 1);
            break;
        case SDLK_LEFT:
        case SDLK_A:
            navigate_menu(-1, 0);
            break;
        case SDLK_RIGHT:
        case SDLK_D:
            navigate_menu(1, 0);
            break;
        case SDLK_TAB:
            if (menu_.screen() == ui::MenuScreen::settings)
            {
                menu_.change_tab((SDL_GetModState() & SDL_KMOD_SHIFT) != 0U ? -1 : 1, settings_);
            }
            break;
        case SDLK_RETURN:
        case SDLK_SPACE:
            handle(menu_.activate(settings_));
            break;
        case SDLK_ESCAPE:
            handle(menu_.back());
            break;
        case SDLK_R:
            if (menu_.screen() == ui::MenuScreen::settings)
            {
                settings_ = Settings{};
                world_preview_->set_view_distance(settings_.view_distance);
                world_preview_->set_particle_density(particle_density(settings_.particle_quality));
                apply_grading_settings();
                apply_audio_settings();
                SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,
                            settings_.raw_mouse_input ? "0" : "1");
                dirty_ = true;
                toast("DEFAULT SETTINGS RESTORED", "All categories returned to balanced defaults.");
            }
            break;
        default:
            break;
        }
    }

    void process_world_key(SDL_Keycode key)
    {
        if (player_life_ != PlayerLife::alive)
        {
            // The original's respawn inputs: Jump or Enter (plus left click,
            // handled in the mouse path); ESC still pauses.
            if (key == SDLK_ESCAPE)
            {
                cancel_world_action();
                SDL_SetWindowRelativeMouseMode(window_, false);
                menu_.open(ui::MenuScreen::pause);
            }
            else if (player_life_ == PlayerLife::dead && (key == SDLK_SPACE || key == SDLK_RETURN))
            {
                respawn_player();
            }
            return;
        }
        if (inventory_open_)
        {
            process_inventory_key(key);
            return;
        }
        switch (key)
        {
        case SDLK_ESCAPE:
            cancel_world_action();
            SDL_SetWindowRelativeMouseMode(window_, false);
            menu_.open(ui::MenuScreen::pause);
            break;
        case SDLK_Q:
            drop_one_selected_item();
            break;
        case SDLK_G:
            switch_hotbar_tray();
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            // Function 8: the separate Activate binding. Unlike right mouse,
            // Enter does not also feed the selected item's Shoulder trigger.
            (void)try_activate_target();
            break;
        case SDLK_LEFT:
            shift_world_time(-1);
            break;
        case SDLK_RIGHT:
            shift_world_time(1);
            break;
        case SDLK_E:
            inventory_open_ = !inventory_open_;
            inventory_focus_ = PlayerInventory::storage_size + inventory_.selected_index();
            SDL_SetWindowRelativeMouseMode(window_, !inventory_open_ && !inspector_view_);
            if (!inventory_open_)
                close_inventory();
            else
                cancel_world_action();
            break;
        case SDLK_T:
            if (online_gameplay_active())
                begin_chat_edit();
            else
                cycle_world_speed();
            break;
        case SDLK_Y:
            begin_chat_edit();
            break;
        case SDLK_TAB:
            if (online_gameplay_active())
                player_roster_visible_ = true;
            break;
        case SDLK_R:
            if (inventory_.selected_item() == InventoryItemId::gps ||
                inventory_.selected_item() == InventoryItemId::teleport_gps)
                begin_locator_name_edit();
            else
                begin_gun_reload();
            break;
        case SDLK_F:
            // Creative flight toggle (the original Player._flyMode, cued with
            // the fly/teleport chirp). Survival keeps its feet on the ground.
            if (infinite_resources_)
            {
                auto &state = player_.state();
                state.fly_mode = !state.fly_mode;
                if (state.fly_mode)
                    state.velocity.y = 0.0F;
                sound_bank_->play_cue(state.fly_mode ? "Teleport" : "locator");
                caption(state.fly_mode ? "[ FLIGHT ON ]" : "[ FLIGHT OFF ]", 1.2F);
                toast(state.fly_mode ? "FLIGHT ENABLED" : "FLIGHT DISABLED",
                      state.fly_mode ? "SPACE RISES  /  SHIFT SINKS  /  F LANDS"
                                     : "Gravity is back in charge.");
            }
            else
            {
                toast("FLIGHT IS A CREATIVE POWER", "Only Creative mode can fly.");
            }
            break;
        case SDLK_F1:
            inspector_view_ = !inspector_view_;
            SDL_SetWindowRelativeMouseMode(window_, !inspector_view_);
            toast(inspector_view_ ? "WORLD INSPECTOR" : "FIRST-PERSON MODE",
                  inspector_view_ ? "F1 RETURNS TO THE PLAYABLE CAMERA"
                                  : "WASD MOVE  /  MOUSE LOOK  /  SPACE JUMP");
            break;
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
        case SDLK_4:
        case SDLK_5:
        case SDLK_6:
        case SDLK_7:
        case SDLK_8:
            // GrenadeCook owns the active item until release/auto-throw. Letting
            // a number key replace it mid-fuse made the grenade vanish without
            // consuming or throwing anything.
            if (grenade_cooking_)
                break;
            inventory_.select(static_cast<std::size_t>(key - SDLK_1));
            cancel_world_action();
            sync_selected_item();
            sound_bank_->play_cue("Click");
            break;
        default:
            break;
        }
    }

    void process_inventory_key(SDL_Keycode key)
    {
        switch (key)
        {
        case SDLK_ESCAPE:
        case SDLK_E:
            sound_bank_->play_click();
            close_inventory();
            break;
        case SDLK_LEFT:
        case SDLK_A:
            move_inventory_focus(-1, 0);
            sound_bank_->play_click();
            break;
        case SDLK_RIGHT:
        case SDLK_D:
            move_inventory_focus(1, 0);
            sound_bank_->play_click();
            break;
        case SDLK_UP:
        case SDLK_W:
            move_inventory_focus(0, -1);
            sound_bank_->play_click();
            break;
        case SDLK_DOWN:
        case SDLK_S:
            move_inventory_focus(0, 1);
            sound_bank_->play_click();
            break;
        case SDLK_RETURN:
        case SDLK_SPACE: {
            const Rect rect = inventory_slot_rect(inventory_focus_);
            inventory_pointer(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F,
                              SDL_BUTTON_LEFT);
            break;
        }
        case SDLK_Q:
            drop_inventory_selection();
            break;
        case SDLK_G:
            switch_hotbar_tray();
            inventory_focus_ = PlayerInventory::storage_size + inventory_.selected_index();
            break;
        case SDLK_TAB:
            if (active_crate_.has_value())
            {
                if (inventory_focus_ >= crate_slot_base)
                    inventory_focus_ = inventory_focus_ - crate_slot_base;
                else
                {
                    const std::size_t row = std::min<std::size_t>(inventory_focus_ / 8U, 3U);
                    inventory_focus_ = crate_slot_base + row * 8U + inventory_focus_ % 8U;
                }
                sound_bank_->play_click();
            }
            break;
        case SDLK_Y:
            if (quick_move_inventory_slot(inventory_focus_))
                sound_bank_->play_click();
            break;
        default:
            break;
        }
    }

    void select_hotbar(int direction)
    {
        if (grenade_cooking_)
            return;
        cancel_world_action();
        inventory_.select_relative(direction);
        sync_selected_item();
        // InGameHUD: every active-slot change plays the 2D "Click" cue.
        sound_bank_->play_cue("Click");
    }

    void switch_hotbar_tray()
    {
        if (inventory_.profile() != InventoryProfile::modern || grenade_cooking_)
            return;
        cancel_world_action();
        inventory_.switch_current_tray();
        sync_selected_item();
        sound_bank_->play_cue("Click");
        caption(std::format("[ QUICK TRAY {} ]", inventory_.current_tray_index() + 1U), 0.8F);
    }

    void primary_action_pressed()
    {
        if (waiting_for_world_ || inventory_open_ || inspector_view_)
            return;
        if (fired_launcher_slot_.has_value() &&
            fired_launcher_tray_ == inventory_.current_tray_index() &&
            *fired_launcher_slot_ == inventory_.selected_index())
            return;
        if (selected_gun() != nullptr)
        {
            mine_button_down_ = true;
            if (gun_reloading_)
            {
                // GunInventoryItem.ProcessInput cancels Reloading on a fresh Use
                // press, then allows that same press to fire any shell/round
                // already inserted by a completed reload cycle.
                gun_reloading_ = false;
                gun_reload_time_ = 0.0F;
                gun_reload_cue_started_ = false;
            }
            // Semi-automatic guns fire once per trigger pull here; automatics
            // keep cycling in the fixed tick while the button stays held.
            fire_gun();
            return;
        }
        if (item_grenade_type(inventory_.selected_item()).has_value())
        {
            begin_grenade_cook();
            return;
        }
        if (inventory_.selected_item() == InventoryItemId::gps ||
            inventory_.selected_item() == InventoryItemId::teleport_gps)
        {
            if (!place_navigation_waypoint())
                return;
            if (inventory_.selected_item() == InventoryItemId::gps && damage_selected_item())
                return;
            begin_locator_name_edit();
            return;
        }
        // Every melee-capable item uses the same entity-or-block construction
        // ray as the original. An enemy hit owns the action and suppresses
        // placement/mining behind it.
        if (try_melee())
        {
            mine_button_down_ = true;
            return;
        }
        const auto target = player_.state().target;
        const world::BlockType selected = inventory_.selected_block();
        if (selected == world::BlockType::empty || !target.has_value())
        {
            mine_button_down_ = true;
            return;
        }
        if (!world::can_build_on(target->block))
            return;

        world::BlockType constructed = selected;
        if (inventory_.selected_item() == InventoryItemId::torch)
        {
            int face = -1;
            if (target->normal_x > 0)
                face = static_cast<int>(world::FaceDirection::positive_x);
            else if (target->normal_z < 0)
                face = static_cast<int>(world::FaceDirection::negative_z);
            else if (target->normal_x < 0)
                face = static_cast<int>(world::FaceDirection::negative_x);
            else if (target->normal_z > 0)
                face = static_cast<int>(world::FaceDirection::positive_z);
            else if (target->normal_y > 0)
                face = static_cast<int>(world::FaceDirection::positive_y);
            else if (target->normal_y < 0)
                face = static_cast<int>(world::FaceDirection::negative_y);
            if (face < 0)
                return;
            constructed = static_cast<world::BlockType>(
                static_cast<int>(world::BlockType::torch_pos_x) + face);
        }

        const InventoryItemId selected_item = inventory_.selected_item();
        const std::optional<std::uint8_t> placed_door_model = door_model_name(selected_item);
        const bool placing_door = placed_door_model.has_value();
        world::BlockType upper_door = world::BlockType::empty;
        if (placing_door)
        {
            auto &region = world_preview_->region();
            const int x = target->place_x;
            const int y = target->place_y;
            const int z = target->place_z;
            if (!region.contains(x, y + 1, z) ||
                region.block(x, y - 1, z) == world::BlockType::empty ||
                region.block(x, y + 1, z) != world::BlockType::empty)
                return;
            const bool x_neighbor = world::is_solid(region.block(x + 1, y, z)) ||
                                    world::is_solid(region.block(x - 1, y, z));
            const world::BlockType base_door = selected_item == InventoryItemId::door
                                                   ? world::BlockType::lower_door
                                                   : world::BlockType::strong_lower_door;
            constructed =
                world::door_piece(base_door, x_neighbor ? world::BlockType::lower_door_closed_x
                                                        : world::BlockType::lower_door_closed_z);
            upper_door = world::door_piece(base_door, world::BlockType::upper_door_closed);
            if (custom_doors_.size() >= net::maximum_world_doors)
                return;
        }

        player_.state().selected_block = constructed;
        if (!player_.place_target(world_preview_->region()))
            return;
        if (placing_door)
        {
            world_preview_->region().set_block(target->place_x, target->place_y + 1,
                                               target->place_z, upper_door);
            const CrateKey key{target->place_x, target->place_y, target->place_z};
            const auto [door, inserted] = custom_doors_.try_emplace(key, *placed_door_model);
            const bool door_identity_valid = inserted || door->second == *placed_door_model;
            const bool host_door_committed =
                online_host_ == nullptr || !online_host_->running() ||
                host_settings_.protocol != net::ProtocolVersion::modern_v4 ||
                online_host_->set_authoritative_custom_door(
                    {target->place_x, target->place_y, target->place_z}, *placed_door_model);
            if (!door_identity_valid || !host_door_committed)
            {
                if (inserted)
                    custom_doors_.erase(door);
                world_preview_->region().set_block(target->place_x, target->place_y,
                                                   target->place_z, world::BlockType::empty);
                world_preview_->region().set_block(target->place_x, target->place_y + 1,
                                                   target->place_z, world::BlockType::empty);
                std::cerr << "network.local.custom-door-snapshot.failure\n";
                return;
            }
            dirty_ = true;
        }
        if (!infinite_resources_)
            (void)inventory_.consume_selected();
        sound_bank_->play_place();
        caption(std::format("[ {} PLACED ]", world::block_name(selected)));
        const world::Vec3 eye{player_.state().position.x,
                              player_.state().position.y + world::PlayerSimulation::eye_height,
                              player_.state().position.z};
        const world::Vec3 direction = player_.view_direction();
        world::BlockBreakUpdate placement{target->x,
                                          target->y,
                                          target->z,
                                          target->block,
                                          {eye.x + direction.x * target->distance,
                                           eye.y + direction.y * target->distance,
                                           eye.z + direction.z * target->distance},
                                          target->normal_x,
                                          target->normal_y,
                                          target->normal_z};
        std::string dig_error;
        if (!publish_local_dig_effect(true, placement, dig_error))
            std::cerr << "network.local.place-effect.failure=" << dig_error << '\n';
        world_preview_->remember_edit(target->place_x, target->place_y, target->place_z,
                                      constructed);
        static_cast<void>(publish_local_block_edit(target->place_x, target->place_y,
                                                   target->place_z, constructed));
        if (placing_door)
        {
            world_preview_->remember_edit(target->place_x, target->place_y + 1, target->place_z,
                                          upper_door);
            static_cast<void>(publish_local_block_edit(target->place_x, target->place_y + 1,
                                                       target->place_z, upper_door));
        }
        world_preview_->rebuild_block_edit(target->place_x, target->place_y, target->place_z);
        if (placing_door)
            world_preview_->rebuild_block_edit(target->place_x, target->place_y + 1,
                                               target->place_z);
        // Vanilla placement sends DigMessage(placing=true) with the struck face's hit point,
        // normal, and block type. ProcessDigMessage then runs the same smoke/spark/rock burst
        // as mining while selecting the separate Place sound above.
        world_preview_->spawn_mining_impact(placement);
        placement_animation_time_ = 0.35F;
        toast("BLOCK PLACED", std::string{world::block_name(selected)});
        sync_selected_item();
    }

    void begin_grenade_cook()
    {
        if (grenade_cooking_ || grenade_cooldown_ > 0.0F)
            return;
        const auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() || slot->count == 0U)
            return;
        const auto grenade_type = item_grenade_type(slot->item);
        if (!grenade_type.has_value())
            return;
        grenade_cooking_ = true;
        grenade_cooking_type_ = *grenade_type;
        grenade_cook_time_ = 0.0F;
        mine_button_down_ = true;
        sound_bank_->play_cue("GrenadeArm");
        caption("[ GRENADE ARMED ]", 0.8F);
    }

    void throw_grenade()
    {
        if (!grenade_cooking_ || world_preview_ == nullptr)
            return;
        const world::Vec3 direction = player_.view_direction();
        Grenade projectile;
        projectile.position = {player_.state().position.x + direction.x,
                               player_.state().position.y + world::PlayerSimulation::eye_height +
                                   direction.y,
                               player_.state().position.z + direction.z};
        projectile.velocity = {direction.x * world::grenade_throw_speed,
                               direction.y * world::grenade_throw_speed,
                               direction.z * world::grenade_throw_speed};
        projectile.time_left = world::grenade_seconds_left(grenade_cook_time_);
        projectile.grenade_type = grenade_cooking_type_;
        projectile.owner_gid = online_gameplay_active() ? online_local_player_gid() : 0U;
        projectile.locally_authoritative = true;
        grenades_.push_back(projectile);
        net::GrenadePayload grenade_message;
        grenade_message.direction = {direction.x, direction.y, direction.z};
        grenade_message.position = {projectile.position.x, projectile.position.y,
                                    projectile.position.z};
        grenade_message.seconds_left = projectile.time_left;
        grenade_message.grenade_type = grenade_cooking_type_;
        net::PayloadError grenade_error{};
        const net::ProtocolVersion protocol = online_gameplay_protocol();
        const auto grenade_payload =
            net::encode_grenade_payload(grenade_message, protocol, grenade_error);
        std::string network_error;
        if (!grenade_payload.has_value() ||
            !publish_online_broadcast("GrenadeMessage", *grenade_payload,
                                      net::MessageDeliveryPolicy::reliable_unordered,
                                      network_error))
            std::cerr << "network.local.grenade.failure=" << network_error << '\n';
        // Player.ProcessGrenadeMessage checks specifically for
        // GrenadeInventoryItemClass. StickyGrenadeInventoryItemClass is a
        // sibling, so the shipped game accidentally never pops sticky stacks.
        if (!infinite_resources_ && grenade_cooking_type_ == 0U)
            (void)inventory_.consume_selected();
        sync_selected_item();
        grenade_cooking_ = false;
        grenade_cooking_type_ = 0U;
        grenade_cook_time_ = 0.0F;
        grenade_cooldown_ = 1.0F;
        grenade_animation_time_ = 0.3166666F;
        mine_button_down_ = false;
        caption("[ GRENADE THROWN ]", 0.8F);
    }

    [[nodiscard]] bool arm_placed_explosive(int x, int y, int z, world::ExplosiveKind kind,
                                            bool notify_player)
    {
        for (const ArmedExplosive &armed : armed_explosives_)
            if (armed.x == x && armed.y == y && armed.z == z)
                return false;
        armed_explosives_.push_back(
            {x, y, z, kind, world::placed_explosive_fuse_seconds, 0.0F, true, false});
        net::PayloadError payload_error{};
        const auto payload = net::encode_int_vector_payload({x, y, z}, payload_error);
        std::string network_error;
        if (!payload.has_value() ||
            !publish_online_broadcast("AddExplosiveFlashMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered,
                                      network_error))
            std::cerr << "network.local.explosive-flash.failure=" << network_error << '\n';
        play_positional("Fuse", {static_cast<float>(x) + 0.5F, static_cast<float>(y),
                                 static_cast<float>(z) + 0.5F});
        if (notify_player)
            caption(kind == world::ExplosiveKind::tnt ? "[ TNT FUSE LIT ]" : "[ C4 FUSE LIT ]",
                    1.0F);
        return true;
    }

    [[nodiscard]] static bool bounded_world_cell(net::IntVector3 location) noexcept
    {
        return location.x >= -1'000'000 && location.x <= 1'000'000 && location.y >= -64 &&
               location.y < 64 && location.z >= -1'000'000 && location.z <= 1'000'000;
    }

    [[nodiscard]] std::optional<world::BlockType> online_block_at(
        net::IntVector3 location) const noexcept
    {
        if (world_preview_ == nullptr)
            return std::nullopt;
        try
        {
            return world_preview_->resolved_block(location.x, location.y, location.z);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool send_authoritative_block_correction(net::PeerId peer,
                                                            net::IntVector3 location)
    {
        if (online_host_ == nullptr || !bounded_world_cell(location))
            return false;
        const auto block = online_block_at(location);
        if (!block.has_value())
            return false;
        net::PayloadError payload_error{};
        const auto payload = net::encode_alter_block_payload(
            {location, static_cast<std::int32_t>(*block)}, host_settings_.protocol, payload_error);
        const net::MessageDescriptor *descriptor =
            net::message_by_name(host_settings_.protocol, "AlterBlockMessage");
        net::PacketError packet_error{};
        const auto packet = payload.has_value() && descriptor != nullptr
                                ? net::encode_packet(host_settings_.protocol, descriptor->id,
                                                     *payload, packet_error)
                                : std::nullopt;
        if (!packet.has_value())
            return false;
        net::LegacySteamOutbound correction;
        correction.recipient = peer;
        correction.application_packet = *packet;
        correction.delivery = net::MessageDeliveryPolicy::reliable_ordered;
        std::string error;
        if (!online_host_->send_authoritative(correction, error))
        {
            std::cerr << "network.authority.block-correction.failure peer=" << peer
                      << " reason=" << error << '\n';
            return false;
        }
        return true;
    }

    void correct_rejected_block_mutation(net::PeerId peer, const net::DecodedPacket &decoded)
    {
        constexpr std::size_t maximum_corrections = 256U;
        net::PayloadError payload_error{};
        if (decoded.message != nullptr && decoded.message->name == "AlterBlockMessage")
        {
            net::AlterBlockPayload edit;
            if (net::decode_alter_block_payload(decoded.payload, host_settings_.protocol, edit,
                                                payload_error))
                static_cast<void>(send_authoritative_block_correction(peer, edit.block_location));
            return;
        }
        if (decoded.message != nullptr && decoded.message->name == "RemoveBlocksMessage")
        {
            net::RemoveBlocksPayload removal;
            if (!net::decode_remove_blocks_payload(decoded.payload, removal, payload_error))
                return;
            const std::size_t count = std::min(removal.blocks.size(), maximum_corrections);
            for (std::size_t index = 0; index < count; ++index)
                static_cast<void>(
                    send_authoritative_block_correction(peer, removal.blocks[index]));
            if (removal.blocks.size() > maximum_corrections)
                std::cerr << "network.authority.block-correction.capped peer=" << peer
                          << " requested=" << removal.blocks.size()
                          << " sent=" << maximum_corrections << '\n';
        }
    }

    [[nodiscard]] static std::optional<CrateKey> centered_pickup_cell(
        const net::Vector3 &position) noexcept
    {
        const int x = static_cast<int>(std::floor(position.x));
        const int y = static_cast<int>(std::floor(position.y));
        const int z = static_cast<int>(std::floor(position.z));
        constexpr float tolerance = 0.001F;
        if (std::abs(position.x - (static_cast<float>(x) + 0.5F)) > tolerance ||
            std::abs(position.y - (static_cast<float>(y) + 0.5F)) > tolerance ||
            std::abs(position.z - (static_cast<float>(z) + 0.5F)) > tolerance)
            return std::nullopt;
        return CrateKey{x, y, z};
    }

    [[nodiscard]] bool authorize_remote_loot_pickup(net::PeerId peer,
                                                    const net::CreatePickupPayload &pickup,
                                                    const InventoryStack &stack)
    {
        if (host_settings_.protocol != net::ProtocolVersion::modern_v4)
            return true;
        const auto key = centered_pickup_cell(pickup.spawn_position);
        if (!key.has_value())
            return true;

        const net::IntVector3 location{(*key)[0], (*key)[1], (*key)[2]};
        const auto current = online_block_at(location);
        const bool current_is_loot = current == world::BlockType::loot_block ||
                                     current == world::BlockType::lucky_loot_block;
        auto claim = remote_loot_claims_.find(*key);
        if (current_is_loot)
        {
            if (claim != remote_loot_claims_.end() &&
                (claim->second.block_removed || claim->second.block != *current))
            {
                remote_loot_claims_.erase(claim);
                claim = remote_loot_claims_.end();
            }
            if (claim == remote_loot_claims_.end())
            {
                if (remote_loot_claims_.size() >= 2048U)
                    return false;
                claim = remote_loot_claims_
                            .emplace(*key, RemoteLootClaim{peer, *current, 0U, 0U, 0.0F, false})
                            .first;
            }
        }
        else
        {
            if (claim == remote_loot_claims_.end() || !claim->second.block_removed ||
                claim->second.expires_at < world_elapsed_)
                return true;
        }

        RemoteLootClaim &authorization = claim->second;
        if (authorization.owner != peer || pickup.dropped || !pickup.display_on_pickup ||
            std::abs(stack.durability - 1.0F) > 0.0001F || stack.waypoint_valid)
            return false;
        const GunStats *gun = gun_stats(stack.item);
        if (stack.rounds_in_clip != (gun != nullptr ? gun->clip_capacity : 0U))
            return false;

        const float velocity_squared = pickup.spawn_vector.x * pickup.spawn_vector.x +
                                       pickup.spawn_vector.y * pickup.spawn_vector.y +
                                       pickup.spawn_vector.z * pickup.spawn_vector.z;
        if (std::abs(velocity_squared - 2.25F) > 0.01F || pickup.spawn_vector.y <= 0.0F ||
            std::abs(pickup.spawn_vector.x) > pickup.spawn_vector.y * 5.02F + 0.001F ||
            std::abs(pickup.spawn_vector.z) > pickup.spawn_vector.y * 5.02F + 0.001F)
            return false;

        const bool lucky = authorization.block == world::BlockType::lucky_loot_block;
        const int minimum_tier = lucky ? 4 : 1;
        const int maximum_tier = lucky ? 10 : 5;
        const std::uint8_t maximum_drops = lucky ? 3U : 1U;
        if (authorization.accepted_drops >= maximum_drops ||
            !world::modern_loot_stack_matches(stack, location.y, minimum_tier, maximum_tier, false))
            return false;
        const bool rare =
            world::modern_loot_stack_matches(stack, location.y, minimum_tier, maximum_tier, true);
        if (lucky && authorization.accepted_drops + 1U == maximum_drops &&
            authorization.rare_eligible_drops == 0U && !rare)
            return false;
        ++authorization.accepted_drops;
        if (rare)
            ++authorization.rare_eligible_drops;
        return true;
    }

    [[nodiscard]] bool apply_online_environment_effect(net::ProtocolVersion protocol,
                                                       const net::DecodedPacket &decoded)
    {
        net::PayloadError payload_error{};
        if (decoded.message->name == "AddExplosionEffectsMessage")
        {
            net::IntVector3 position;
            if (!net::decode_int_vector_payload(decoded.payload, position, payload_error) ||
                !bounded_world_cell(position))
                return false;
            const world::Vec3 location{static_cast<float>(position.x),
                                       static_cast<float>(position.y),
                                       static_cast<float>(position.z)};
            play_positional("Explosion", location);
            world_preview_->spawn_fireball_explosion(location, false, true);
            return true;
        }
        if (decoded.message->name == "AddExplosiveFlashMessage")
        {
            net::IntVector3 position;
            if (!net::decode_int_vector_payload(decoded.payload, position, payload_error) ||
                !bounded_world_cell(position))
                return false;
            const auto block = online_block_at(position);
            if (!block.has_value() ||
                (*block != world::BlockType::tnt && *block != world::BlockType::c4))
                return false;
            if (std::ranges::none_of(armed_explosives_, [&](const ArmedExplosive &armed) {
                    return armed.x == position.x && armed.y == position.y && armed.z == position.z;
                }))
                armed_explosives_.push_back(
                    {position.x, position.y, position.z,
                     *block == world::BlockType::tnt ? world::ExplosiveKind::tnt
                                                     : world::ExplosiveKind::c4,
                     world::placed_explosive_fuse_seconds, 0.0F, false, false});
            return true;
        }
        if (decoded.message->name == "DoorOpenCloseMessage")
        {
            net::DoorOpenClosePayload door;
            if (!net::decode_door_open_close_payload(decoded.payload, door, payload_error) ||
                !bounded_world_cell(door.location))
                return false;
            const auto block = online_block_at(door.location);
            if (!block.has_value() ||
                (!world::is_lower_door(*block) && !world::is_upper_door(*block)))
                return false;
            play_positional(door.opened ? "DoorOpen" : "DoorClose",
                            {static_cast<float>(door.location.x),
                             static_cast<float>(door.location.y),
                             static_cast<float>(door.location.z)});
            return true;
        }
        static_cast<void>(protocol);
        return true;
    }

    [[nodiscard]] bool try_activate_target()
    {
        if (world_preview_ == nullptr)
            return false;
        if (!player_.state().target.has_value())
            return false;
        const world::VoxelRaycastHit &target = *player_.state().target;
        if (world::is_spawner_clickable(target.block))
        {
            if (online_gameplay_active() &&
                online_gameplay_protocol() != net::ProtocolVersion::modern_v4)
            {
                sound_bank_->play_cue("Error");
                caption("[ MODERN SPAWNER UNAVAILABLE ]", 1.0F);
                return true;
            }
            const std::string gamer_tag =
                online_persona_name_.empty() ? "OpenMinerZ" : online_persona_name_;
            const std::uint8_t gid =
                online_gameplay_active() ? online_local_player_gid() : std::uint8_t{};
            if (!start_local_spawner({target.x, target.y, target.z}, {gid, gid, gamer_tag}, false))
            {
                sound_bank_->play_cue("Error");
                caption("[ SPAWNER COULD NOT START ]", 1.0F);
            }
            return true;
        }
        if (world::is_container(target.block))
        {
            active_crate_ = CrateKey{target.x, target.y, target.z};
            (void)crate_inventories_[*active_crate_];
            inventory_open_ = true;
            inventory_focus_ = crate_slot_base;
            cancel_world_action();
            SDL_SetWindowRelativeMouseMode(window_, false);
            sound_bank_->play_click();
            caption("[ CRATE OPENED ]", 0.8F);
            return true;
        }
        if (world::is_lower_door(target.block) || world::is_upper_door(target.block))
        {
            auto &region = world_preview_->region();
            const int lower_y = world::is_upper_door(target.block) ? target.y - 1 : target.y;
            world::BlockType lower = region.block(target.x, lower_y, target.z);
            const bool opening = lower == world::BlockType::lower_door_closed_z ||
                                 lower == world::BlockType::lower_door_closed_x ||
                                 lower == world::BlockType::lower_door ||
                                 lower == world::BlockType::strong_lower_door_closed_z ||
                                 lower == world::BlockType::strong_lower_door_closed_x ||
                                 lower == world::BlockType::strong_lower_door;
            if (!opening && lower != world::BlockType::lower_door_open_z &&
                lower != world::BlockType::lower_door_open_x &&
                lower != world::BlockType::strong_lower_door_open_z &&
                lower != world::BlockType::strong_lower_door_open_x)
                return false;
            const bool x_axis = lower == world::BlockType::lower_door_closed_x ||
                                lower == world::BlockType::lower_door_open_x ||
                                lower == world::BlockType::strong_lower_door_closed_x ||
                                lower == world::BlockType::strong_lower_door_open_x;
            const world::BlockType base_door = world::is_strong_door(lower)
                                                   ? world::BlockType::strong_lower_door
                                                   : world::BlockType::lower_door;
            lower = world::door_piece(base_door,
                                      opening ? (x_axis ? world::BlockType::lower_door_open_x
                                                        : world::BlockType::lower_door_open_z)
                                              : (x_axis ? world::BlockType::lower_door_closed_x
                                                        : world::BlockType::lower_door_closed_z));
            const world::BlockType upper =
                world::door_piece(base_door, opening ? world::BlockType::upper_door_open
                                                     : world::BlockType::upper_door_closed);
            net::PayloadError payload_error{};
            const auto payload = net::encode_door_open_close_payload(
                {{target.x, target.y, target.z}, opening}, payload_error);
            std::string network_error;
            if (!payload.has_value() ||
                !publish_online_broadcast("DoorOpenCloseMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_ordered,
                                          network_error))
                std::cerr << "network.local.door-sound.failure=" << network_error << '\n';
            region.set_block(target.x, lower_y, target.z, lower);
            region.set_block(target.x, lower_y + 1, target.z, upper);
            world_preview_->remember_edit(target.x, lower_y, target.z, lower);
            world_preview_->remember_edit(target.x, lower_y + 1, target.z, upper);
            static_cast<void>(publish_local_block_edit(target.x, lower_y, target.z, lower));
            static_cast<void>(publish_local_block_edit(target.x, lower_y + 1, target.z, upper));
            world_preview_->rebuild_block_edit(target.x, lower_y, target.z);
            world_preview_->rebuild_block_edit(target.x, lower_y + 1, target.z);
            play_positional(opening ? "DoorOpen" : "DoorClose",
                            {static_cast<float>(target.x) + 0.5F,
                             static_cast<float>(target.y) + 0.5F,
                             static_cast<float>(target.z) + 0.5F});
            sound_bank_->play_click();
            caption(opening ? "[ DOOR OPEN ]" : "[ DOOR CLOSED ]", 0.8F);
            player_.refresh_target(region);
            return true;
        }
        const world::ExplosiveKind kind =
            target.block == world::BlockType::tnt  ? world::ExplosiveKind::tnt
            : target.block == world::BlockType::c4 ? world::ExplosiveKind::c4
                                                   : world::ExplosiveKind::rocket;
        if (kind == world::ExplosiveKind::rocket)
        {
            sound_bank_->play_cue("Error");
            return true;
        }
        (void)arm_placed_explosive(target.x, target.y, target.z, kind, true);
        return true;
    }

    void cancel_world_action()
    {
        mine_button_down_ = false;
        secondary_action_down_ = false;
        shouldering_ = false;
        air_punch_time_ = 0.0F;
        gun_reloading_ = false;
        gun_reload_time_ = 0.0F;
        gun_reload_cue_started_ = false;
        gun_aim_progress_ = 0.0F;
        grenade_cooking_ = false;
        grenade_cooking_type_ = 0U;
        grenade_cook_time_ = 0.0F;
        player_.cancel_breaking();
    }

    void sync_selected_item()
    {
        const bool showing_fired_launcher =
            fired_launcher_slot_.has_value() && fired_launcher_visual_.has_value() &&
            fired_launcher_tray_ == inventory_.current_tray_index() &&
            fired_launcher_time_ > 0.0F && *fired_launcher_slot_ == inventory_.selected_index();
        player_.state().selected_item_id = static_cast<std::uint16_t>(
            showing_fired_launcher ? *fired_launcher_visual_ : inventory_.selected_item());
        player_.state().selected_block = inventory_.selected_block();
        player_.state().selected_waypoint = {};
        if (const auto &slot = inventory_.selected();
            slot.has_value() &&
            (slot->item == InventoryItemId::gps || slot->item == InventoryItemId::teleport_gps))
        {
            player_.state().selected_waypoint = {slot->waypoint[0], slot->waypoint[1],
                                                 slot->waypoint[2]};
        }
        const bool online = (online_host_ != nullptr && online_host_->running()) ||
                            (online_client_ != nullptr && online_client_joined_ &&
                             online_client_->connected() && online_bootstrap_client_ == nullptr);
        if (!online)
        {
            last_online_carried_item_.reset();
        }
        else
        {
            std::int16_t signed_wire_item{};
            const auto internal_item =
                static_cast<InventoryItemId>(player_.state().selected_item_id);
            if (!net::inventory_item_to_wire(internal_item, online_gameplay_protocol(),
                                             signed_wire_item))
            {
                std::cerr << "network.local.carried-item.failure=unsupported inventory item\n";
                return;
            }
            const auto wire_item = static_cast<std::uint16_t>(signed_wire_item);
            if (last_online_carried_item_ == wire_item)
                return;
            const std::array<std::uint8_t, 2> payload{static_cast<std::uint8_t>(wire_item & 0xFFU),
                                                      static_cast<std::uint8_t>(wire_item >> 8U)};
            std::string error;
            if (publish_online_broadcast("ChangeCarriedItemMessage", payload,
                                         net::MessageDeliveryPolicy::reliable_unordered, error))
                last_online_carried_item_ = wire_item;
            else
                std::cerr << "network.local.carried-item.failure=" << error << '\n';
        }
    }

    [[nodiscard]] static bool is_laser_sword_item(InventoryItemId item) noexcept
    {
        return item >= InventoryItemId::iron_laser_sword &&
               item <= InventoryItemId::blood_stone_laser_sword;
    }

    void update_equipped_item_sound()
    {
        const bool gameplay_screen = menu_.screen() == ui::MenuScreen::world_preview ||
                                     menu_.screen() == ui::MenuScreen::pause;
        const InventoryItemId selected = inventory_.selected_item();
        const std::optional<InventoryItemId> wanted =
            gameplay_screen && !waiting_for_world_ && is_laser_sword_item(selected)
                ? std::optional{selected}
                : std::nullopt;
        if (wanted == looping_equipped_item_)
            return;
        if (looping_equipped_item_.has_value())
            sound_bank_->stop_looping_cue("LightSaber");
        looping_equipped_item_ = wanted;
        if (looping_equipped_item_.has_value())
            sound_bank_->start_looping_cue("LightSaber");
    }

    void drop_one_selected_item()
    {
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value())
            return;
        InventoryStack dropped = *slot;
        dropped.count = 1U;
        drop_stack(dropped);
        if (--slot->count == 0U)
            slot.reset();
        sync_selected_item();
    }

    [[nodiscard]] const GunStats *selected_gun() const noexcept
    {
        return gun_stats(inventory_.selected_item());
    }

    [[nodiscard]] static world::DamageFlags weapon_damage_flags(InventoryItemId item) noexcept
    {
        const auto value = static_cast<std::uint16_t>(item);
        const bool shotgun = value == 52U || value == 58U || value == 64U || value == 70U ||
                             value == 84U || value == 89U || value == 94U || value == 99U ||
                             value == 103U || value == 104U ||
                             item == InventoryItemId::modern_basic_grenade_launcher;
        if (shotgun)
            return world::DamageFlags::shotgun;
        if (gun_stats(item) != nullptr)
            return world::DamageFlags::bullet;
        const bool blade = (value >= 23U && value <= 28U) || (value >= 34U && value <= 38U) ||
                           value == 49U || value == 55U || value == 61U || value == 67U ||
                           value == 81U || (value >= 114U && value <= 118U);
        return blade ? world::DamageFlags::blade : world::DamageFlags::blunt;
    }

    [[nodiscard]] world::EnemyHorde::RayHit melee_target() const
    {
        if (world_preview_ == nullptr || !item_is_melee_weapon(inventory_.selected_item()))
            return {};
        const world::Vec3 eye{player_.state().position.x,
                              player_.state().position.y + world::PlayerSimulation::eye_height,
                              player_.state().position.z};
        const world::Vec3 direction = player_.view_direction();
        // InGameHUD's ConstructionProbe is five blocks long. Terrain is
        // traced first and the enemy test is clipped to that intersection.
        constexpr float construction_reach = 5.0F;
        const float reach = clear_range(eye, direction, construction_reach);
        return enemy_horde_.trace_along_ray(eye, direction, reach);
    }

    [[nodiscard]] static std::optional<float> ray_box_time(world::Vec3 origin, world::Vec3 delta,
                                                           world::Vec3 minimum,
                                                           world::Vec3 maximum) noexcept
    {
        float in_time = 0.0F;
        float out_time = 1.0F;
        const auto clip_axis = [&](float start, float movement, float low, float high) {
            if (std::abs(movement) <= 0.000001F)
                return start >= low && start <= high;
            float first = (low - start) / movement;
            float second = (high - start) / movement;
            if (first > second)
                std::swap(first, second);
            in_time = std::max(in_time, first);
            out_time = std::min(out_time, second);
            return in_time <= out_time;
        };
        if (!clip_axis(origin.x, delta.x, minimum.x, maximum.x) ||
            !clip_axis(origin.y, delta.y, minimum.y, maximum.y) ||
            !clip_axis(origin.z, delta.z, minimum.z, maximum.z))
            return std::nullopt;
        return in_time;
    }

    [[nodiscard]] net::PlayerVersusPlayer online_pvp_policy() const noexcept
    {
        return online_host_ != nullptr && online_host_->running()
                   ? host_settings_.player_versus_player
                   : online_client_pvp_;
    }

    [[nodiscard]] std::optional<OnlineMeleeTarget> melee_player_target() const
    {
        if (!online_gameplay_active() || online_pvp_policy() == net::PlayerVersusPlayer::off ||
            !item_is_melee_weapon(inventory_.selected_item()))
            return std::nullopt;
        const world::Vec3 eye{player_.state().position.x,
                              player_.state().position.y + world::PlayerSimulation::eye_height,
                              player_.state().position.z};
        const world::Vec3 direction = player_.view_direction();
        const world::Vec3 delta{direction.x * 5.0F, direction.y * 5.0F, direction.z * 5.0F};
        // ConstructionProbe checks living remote gamers in session order and
        // accepts the first 0.7 x 1.7 x 0.7 player box hit before t = 0.5.
        for (const RemotePlayerVisual &remote : online_remote_visuals_)
        {
            if (remote.dead)
                continue;
            const world::Vec3 minimum{remote.position.x - 0.35F, remote.position.y,
                                      remote.position.z - 0.35F};
            const world::Vec3 maximum{remote.position.x + 0.35F, remote.position.y + 1.7F,
                                      remote.position.z + 0.35F};
            const auto hit_time = ray_box_time(eye, delta, minimum, maximum);
            if (!hit_time.has_value() || *hit_time >= 0.5F)
                continue;
            return OnlineMeleeTarget{remote.gid,
                                     {eye.x + delta.x * *hit_time, eye.y + delta.y * *hit_time,
                                      eye.z + delta.z * *hit_time}};
        }
        return std::nullopt;
    }

    void publish_local_enemy_kill(world::EnemyIdentity identity, InventoryItemId weapon)
    {
        if (!online_gameplay_active())
            return;
        std::int16_t wire_weapon{};
        if (!net::inventory_item_to_wire(weapon, online_gameplay_protocol(), wire_weapon))
            return;
        net::KillEnemyPayload message;
        message.enemy_id = static_cast<std::int32_t>(identity.enemy_id);
        message.target_id = static_cast<std::int32_t>(identity.target_gid);
        message.weapon_id = wire_weapon;
        message.killer_id = online_local_player_gid();
        net::PayloadError payload_error{};
        const auto payload =
            net::encode_kill_enemy_payload(message, online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("KillEnemyMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            std::cerr << "network.local.kill-enemy.failure=" << error << '\n';
        remote_spawner_enemy_owners_.erase(identity);
    }

    void damage_enemy_with_network(std::size_t index, float damage, world::DamageFlags flags,
                                   bool headshot, InventoryItemId weapon,
                                   bool locally_authoritative)
    {
        if (index >= enemy_horde_.enemies().size())
            return;
        const world::Enemy &before = enemy_horde_.enemies()[index];
        const world::EnemyIdentity identity{before.target_gid, before.id};
        const bool was_alive =
            before.state != world::EnemyState::die && before.state != world::EnemyState::despawn;
        enemy_horde_.damage_enemy(index, damage, flags, headshot, locally_authoritative);
        if (!locally_authoritative || !online_gameplay_active() || !was_alive ||
            index >= enemy_horde_.enemies().size() ||
            enemy_horde_.enemies()[index].state != world::EnemyState::die)
        {
            if (locally_authoritative && was_alive && index < enemy_horde_.enemies().size() &&
                enemy_horde_.enemies()[index].state == world::EnemyState::die)
                notify_local_spawner_enemy_defeated(
                    identity, online_gameplay_active() ? online_local_player_gid() : 0U);
            return;
        }
        publish_local_enemy_kill(identity, weapon);
        notify_local_spawner_enemy_defeated(
            identity, online_gameplay_active() ? online_local_player_gid() : 0U);
    }

    void publish_local_dragon_kill(world::Vec3 location, std::uint8_t killer_gid,
                                   InventoryItemId weapon)
    {
        if (!online_gameplay_active())
            return;
        std::int16_t wire_weapon{};
        if (!net::inventory_item_to_wire(weapon, online_gameplay_protocol(), wire_weapon))
            return;
        net::PayloadError payload_error{};
        const net::KillDragonPayload message{
            {location.x, location.y, location.z}, wire_weapon, killer_gid};
        const auto payload =
            net::encode_kill_dragon_payload(message, online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("KillDragonMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.kill-dragon.failure=" << error << '\n';
    }

    void damage_dragon_with_network(float damage, world::Vec3 location, InventoryItemId weapon,
                                    std::uint8_t killer_gid)
    {
        if (!dragon_.active())
            return;
        const bool was_alive =
            dragon_.dragon().state != world::DragonState::dying && dragon_.dragon().health > 0.0F;
        dragon_.damage(damage);
        if (!was_alive || dragon_.dragon().health > 0.0F || dragon_kill_announced_)
            return;
        dragon_kill_announced_ = true;
        dragon_loot_pending_ = !online_gameplay_active() || killer_gid == online_local_player_gid();
        queued_dragon_fireball_ids_.clear();
        if (online_gameplay_active())
            publish_local_dragon_kill(location, killer_gid, weapon);
        // KillDragon's local echo removes DragonEntity immediately while the
        // DragonClientEntity death body remains. Clearing the owner reproduces
        // that split and rejects further brain authority.
        dragon_owner_gid_.reset();
        dragon_pending_ = false;
    }

    void apply_melee_hit(const world::EnemyHorde::RayHit &hit)
    {
        if (!hit)
            return;
        const InventoryItemId item = inventory_.selected_item();
        damage_enemy_with_network(static_cast<std::size_t>(hit.index), item_enemy_damage(item),
                                  weapon_damage_flags(item), hit.headshot, item, true);
        const world::Vec3 eye{player_.state().position.x,
                              player_.state().position.y + world::PlayerSimulation::eye_height,
                              player_.state().position.z};
        const world::Vec3 direction = player_.view_direction();
        world_preview_->spawn_impact_smoke({eye.x + direction.x * hit.distance,
                                            eye.y + direction.y * hit.distance,
                                            eye.z + direction.z * hit.distance},
                                           {1.0F, 1.0F, 1.0F});
        caption("[ ENEMY STRUCK ]", 0.7F);
        melee_cooldown_ = item_use_cooldown_seconds(item);
        (void)damage_selected_item();
    }

    void apply_melee_player_hit(const OnlineMeleeTarget &hit)
    {
        const InventoryItemId item = inventory_.selected_item();
        std::int16_t wire_item{};
        if (!net::inventory_item_to_wire(item, online_gameplay_protocol(), wire_item))
            return;
        net::PayloadError payload_error{};
        const auto payload = net::encode_melee_player_payload(
            {wire_item, {hit.intersection.x, hit.intersection.y, hit.intersection.z}},
            online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_direct(hit.gid, "MeleePlayerMessage", *payload,
                                   net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.melee-player.failure=" << error << '\n';
        world_preview_->spawn_impact_smoke(hit.intersection, {1.0F, 1.0F, 1.0F});
        sound_bank_->play_cue("Hit");
        caption("[ PLAYER STRUCK ]", 0.7F);
        melee_cooldown_ = item_use_cooldown_seconds(item);
        (void)damage_selected_item();
    }

    // Returns true whenever an enemy owns the construction ray, even while
    // the item's cooldown is running. This prevents the block behind/under a
    // zombie from receiving the same held action.
    [[nodiscard]] bool try_melee()
    {
        if (const auto player_hit = melee_player_target(); player_hit.has_value())
        {
            if (melee_cooldown_ <= 0.0F)
                apply_melee_player_hit(*player_hit);
            return true;
        }
        const world::EnemyHorde::RayHit hit = melee_target();
        if (!hit)
            return false;
        if (melee_cooldown_ <= 0.0F)
            apply_melee_hit(hit);
        return true;
    }

    [[nodiscard]] bool has_melee_target() const
    {
        return melee_player_target().has_value() || static_cast<bool>(melee_target());
    }

    [[nodiscard]] float clear_range(world::Vec3 origin, world::Vec3 direction, float maximum) const
    {
        const auto block_hit =
            world::PlayerSimulation::raycast(world_preview_->region(), origin, direction, maximum);
        return block_hit.has_value() ? block_hit->distance : maximum;
    }

    void begin_gun_reload()
    {
        const GunStats *stats = selected_gun();
        if (stats == nullptr || gun_reloading_)
            return;
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() || slot->rounds_in_clip >= stats->clip_capacity ||
            inventory_.count(stats->ammo_item) == 0U)
            return;
        gun_reloading_ = true;
        gun_reload_time_ = stats->reload_seconds;
        gun_reload_cue_started_ = false;
        mine_button_down_ = false;
        toast("RELOADING",
              std::format("{} / {}", slot->rounds_in_clip, inventory_.count(stats->ammo_item)));
    }

    void reset_guided_lock() noexcept
    {
        if (guided_tone_played_)
            sound_bank_->stop_looping_cue("SolidTone");
        guided_lock_time_ = 0.0F;
        guided_lock_required_ = 0.0F;
        guided_lock_distance_ = 0.0F;
        guided_locked_ = false;
        guided_tone_played_ = false;
    }

    void update_guided_lock(float fixed_delta)
    {
        if (inventory_.selected_item() != InventoryItemId::guided_rocket_launcher ||
            !shouldering_ || !dragon_.visible())
        {
            reset_guided_lock();
            return;
        }
        const world::Vec3 eye{player_.state().position.x,
                              player_.state().position.y + world::PlayerSimulation::eye_height,
                              player_.state().position.z};
        const world::Vec3 dragon_position = dragon_.dragon().position;
        world::Vec3 toward{dragon_position.x - eye.x, dragon_position.y - eye.y,
                           dragon_position.z - eye.z};
        const float distance =
            std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
        if (distance <= 0.001F || distance >= 250.0F)
        {
            reset_guided_lock();
            return;
        }
        toward.x /= distance;
        toward.y /= distance;
        toward.z /= distance;
        const world::Vec3 forward = player_.view_direction();
        const float cosine = std::clamp(
            forward.x * toward.x + forward.y * toward.y + forward.z * toward.z, -1.0F, 1.0F);
        const float angle = std::acos(cosine);
        const GunStats *stats = selected_gun();
        const float magnification = stats != nullptr ? stats->shoulder_magnification : 2.0F;
        const float current_fov_degrees = std::lerp(
            settings_.vertical_fov, settings_.vertical_fov / magnification, gun_aim_progress_);
        const float angle_limit = current_fov_degrees * 0.01745329251994329577F * 0.3F;
        if (angle >= angle_limit)
        {
            reset_guided_lock();
            return;
        }
        const auto obstruction = world::PlayerSimulation::raycast_blocking(world_preview_->region(),
                                                                           eye, toward, distance);
        if (obstruction.has_value())
        {
            reset_guided_lock();
            return;
        }
        // The original calls EnemyManager.Trace on this line, so an accepted
        // zombie/skeleton/alien body strike also owns the probe and prevents a
        // dragon lock. It uses the same ChanceOfBulletStrike roll as a shot.
        if (enemy_horde_.trace_projectile_along_ray(eye, toward, distance))
        {
            reset_guided_lock();
            return;
        }

        guided_lock_distance_ = distance;
        guided_lock_required_ = guided_lock_seconds(distance, angle, angle_limit);
        guided_lock_time_ += fixed_delta;
        if (guided_lock_time_ >= guided_lock_required_)
        {
            guided_locked_ = true;
            if (!guided_tone_played_)
            {
                guided_tone_played_ = true;
                sound_bank_->start_looping_cue("SolidTone");
            }
            return;
        }
        guided_locked_ = false;
        guided_beep_time_ += fixed_delta;
        const float interval = guided_lock_beep_seconds(guided_lock_required_, guided_lock_time_);
        if (guided_beep_time_ >= interval)
        {
            guided_beep_time_ = 0.0F;
            sound_bank_->play_cue("Beep");
        }
    }

    void fire_gun()
    {
        if (fired_launcher_slot_.has_value() &&
            fired_launcher_tray_ == inventory_.current_tray_index() &&
            *fired_launcher_slot_ == inventory_.selected_index())
            return;
        const GunStats *stats = selected_gun();
        if (stats == nullptr || gun_cooldown_ > 0.0F || gun_reloading_)
            return;
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() || gun_stats(slot->item) == nullptr)
            return;
        const net::ProtocolVersion item_protocol =
            online_gameplay_active() ? online_gameplay_protocol()
                                     : (inventory_.profile() == InventoryProfile::modern
                                            ? net::ProtocolVersion::modern_v4
                                            : net::ProtocolVersion::classic_v3);
        std::int16_t wire_item{};
        if (!net::inventory_item_to_wire(slot->item, item_protocol, wire_item))
            return;
        if (slot->rounds_in_clip == 0U)
        {
            begin_gun_reload();
            return;
        }
        sound_bank_->play_cue(stats->shoot_cue);
        caption(std::format("[ {} SHOT ]", item_name(slot->item)), 0.8F);
        {
            // Conventional shots use TracerManager's camera-origin ballistic
            // streak. LaserGunInventoryItemClass instead creates BlasterShot
            // at the visible weapon tip and follows its separate bolt contract.
            const world::Vec3 eye{player_.state().position.x,
                                  player_.state().position.y + world::PlayerSimulation::eye_height,
                                  player_.state().position.z};
            const world::Vec3 direction = player_.view_direction();
            // ShotgunShotMessage fires five pellets; GunshotMessage fires one.
            // Both use the gun's authored inaccuracy. Conventional bullets add
            // the original +0.015 camera-up drop compensation; lasers do not.
            const bool pump_shotgun =
                weapon_damage_flags(slot->item) == world::DamageFlags::shotgun &&
                slot->item != InventoryItemId::rocket_launcher &&
                slot->item != InventoryItemId::guided_rocket_launcher;
            const bool launcher = slot->item == InventoryItemId::rocket_launcher ||
                                  slot->item == InventoryItemId::guided_rocket_launcher;
            // RocketLauncherGuidedInventoryItemClass.IsGuided snapshots the
            // completed lock into FireRocketMessage. The weapon still fires
            // normally without a lock; that round simply does not home.
            const bool homing_launch =
                slot->item == InventoryItemId::guided_rocket_launcher && guided_locked_;
            const int pellets = pump_shotgun ? 5 : 1;
            std::array<world::Vec3, net::ShotgunShotPayload::direction_count> fired_directions{};
            const float yaw = player_.state().yaw;
            const world::Vec3 right{std::cos(yaw), 0.0F, std::sin(yaw)};
            const world::Vec3 up{right.y * direction.z - right.z * direction.y,
                                 right.z * direction.x - right.x * direction.z,
                                 right.x * direction.y - right.y * direction.x};
            const bool laser = is_laser_gun(slot->item);
            // GetGunTipPosition is driven by the original animated hand/model
            // hierarchy. The clone renders that hierarchy separately, so use
            // the matching first-person muzzle offset for projectile gameplay.
            const world::Vec3 muzzle{eye.x + direction.x * 0.55F + right.x * 0.28F - up.x * 0.22F,
                                     eye.y + direction.y * 0.55F + right.y * 0.28F - up.y * 0.22F,
                                     eye.z + direction.z * 0.55F + right.z * 0.28F - up.z * 0.22F};
            auto &spread_rng = enemy_horde_.rng();
            for (int pellet = 0; pellet < pellets; ++pellet)
            {
                world::Vec3 aim = direction;
                // FireRocketMessage uses the camera matrix directly, bypassing
                // the GunshotMessage spread/drop path despite inheriting gun data.
                if (!launcher)
                {
                    if (stats->needs_drop_compensation)
                    {
                        aim.x += up.x * 0.015F;
                        aim.y += up.y * 0.015F;
                        aim.z += up.z * 0.015F;
                    }
                    const float side = (spread_rng.next01() * 2.0F - 1.0F) * stats->inaccuracy;
                    const float rise = (spread_rng.next01() * 2.0F - 1.0F) * stats->inaccuracy;
                    aim.x += right.x * side + up.x * rise;
                    aim.y += right.y * side + up.y * rise;
                    aim.z += right.z * side + up.z * rise;
                    const float length = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
                    aim.x /= length;
                    aim.y /= length;
                    aim.z /= length;
                }
                fired_directions[static_cast<std::size_t>(pellet)] = aim;
                spawn_tracer(laser ? muzzle : eye, aim, stats->enemy_damage,
                             weapon_damage_flags(slot->item), slot->item, homing_launch,
                             online_gameplay_active() ? online_local_player_gid() : 0U, true);
            }
            net::PayloadError payload_error{};
            std::optional<std::vector<std::uint8_t>> payload;
            std::string_view message_name;
            if (launcher)
            {
                net::FireRocketPayload message;
                message.direction = {direction.x, direction.y, direction.z};
                message.position = {eye.x + direction.x, eye.y + direction.y, eye.z + direction.z};
                message.guided = homing_launch;
                message.weapon_type = wire_item;
                payload = net::encode_fire_rocket_payload(message, online_gameplay_protocol(),
                                                          payload_error);
                message_name = "FireRocketMessage";
            }
            else if (pump_shotgun)
            {
                net::ShotgunShotPayload message;
                for (std::size_t index{}; index < message.directions.size(); ++index)
                    message.directions[index] = {fired_directions[index].x,
                                                 fired_directions[index].y,
                                                 fired_directions[index].z};
                message.item_id = wire_item;
                payload = net::encode_shotgun_shot_payload(message, online_gameplay_protocol(),
                                                           payload_error);
                message_name = "ShotgunShotMessage";
            }
            else
            {
                const world::Vec3 aim = fired_directions.front();
                net::GunshotPayload message{{aim.x, aim.y, aim.z}, wire_item};
                payload =
                    net::encode_gunshot_payload(message, online_gameplay_protocol(), payload_error);
                message_name = "GunshotMessage";
            }
            std::string network_error;
            if (!payload.has_value() ||
                !publish_online_broadcast(message_name, *payload,
                                          net::MessageDeliveryPolicy::reliable_unordered,
                                          network_error))
                std::cerr << "network.local.weapon-fire.failure=" << network_error << '\n';
            if (alien_aroused_time_ > 0.0F)
                alien_aroused_time_ = std::max(alien_aroused_time_, 30.0F);
        }
        if (gamepad_ != nullptr && settings_.controller_vibration)
            (void)SDL_RumbleGamepad(gamepad_, 0x1800U, 0x5200U, 65U);
        // GunInventoryItem always drains the clip, including Creative; only
        // PlayerInventory.Consume makes its later reload free.
        --slot->rounds_in_clip;
        const bool empty_clip = slot->rounds_in_clip == 0U;
        const bool one_shot_launcher = slot->item == InventoryItemId::rocket_launcher ||
                                       slot->item == InventoryItemId::guided_rocket_launcher;
        const bool grenade_launcher = slot->item == InventoryItemId::modern_basic_grenade_launcher;
        // GrenadeLauncherBaseItem overrides InflictDamage: it publishes the
        // fired-rocket visual but deliberately returns false and never marks
        // itself for deletion. It remains in the tray for a normal reload.
        const bool depleted = one_shot_launcher  ? true
                              : grenade_launcher ? false
                                                 : damage_selected_item();
        gun_recoil_duration_ = item_fire_animation_seconds(slot->item, shouldering_);
        if (one_shot_launcher || grenade_launcher)
        {
            // RocketLauncherBaseItem.InflictDamage swaps to its fired visual,
            // then removes the one-use launcher after the shot animation.
            fired_launcher_slot_ = inventory_.selected_index();
            fired_launcher_tray_ = inventory_.current_tray_index();
            fired_launcher_visual_ = slot->item == InventoryItemId::guided_rocket_launcher
                                         ? InventoryItemId::fired_guided_rocket
                                         : InventoryItemId::fired_rocket;
            fired_launcher_time_ = gun_recoil_duration_;
            sync_selected_item();
            reset_guided_lock();
        }
        gun_cooldown_ = stats->fire_cooldown_seconds;
        gun_recoil_time_ = gun_recoil_duration_;
        const GunRecoil recoil =
            gun_recoil(stats->recoil_radians, recoil_rng_.next01(), recoil_rng_.next01());
        player_.look(recoil.yaw_radians, recoil.pitch_radians);
        if (!depleted && !one_shot_launcher && empty_clip &&
            inventory_.count(stats->ammo_item) > 0U)
            begin_gun_reload();
    }

    // Per-gun tracer ballistics (GunInventoryItemClass ctors): velocity /
    // FlightTime, tracer color from the tier material.
    void tracer_ballistics(InventoryItemId item, float &velocity, float &flight,
                           std::array<float, 3> &color) const noexcept
    {
        const GunStats *stats = gun_stats(item);
        velocity = stats != nullptr ? stats->tracer_velocity : 100.0F;
        flight = stats != nullptr ? stats->tracer_flight_seconds : 2.0F;
        const std::string_view name = item_name(item);
        const auto has = [&](std::string_view token) {
            return name.find(token) != std::string_view::npos;
        };
        color = {1.0F, 1.0F, 1.0F};
        const auto value = static_cast<std::uint16_t>(item);
        if (value >= 82U && value <= 101U)
        {
            // LaserGunInventoryItemClass only replaces TracerColor. Velocity
            // and FlightTime remain the base gun values (100 and 2 seconds).
            if (has("COPPER"))
                color = {0.0F, 1.0F, 0.0F};
            else if (has("IRON"))
                color = {1.0F, 0.0F, 0.0F};
            else if (has("GOLD"))
                color = {1.0F, 1.0F, 0.0F};
            else if (has("DIAMOND"))
                color = {0.0F, 0.0F, 1.0F};
            return;
        }
        if (has("COPPER"))
            color = {184.0F / 255.0F, 115.0F / 255.0F, 51.0F / 255.0F};
        else if (has("IRON"))
            color = {0.5F, 0.5F, 0.5F};
        else if (has("GOLD"))
            color = {1.0F, 215.0F / 255.0F, 0.0F};
        else if (has("DIAMOND"))
            color = {0.0F, 1.0F, 1.0F};
        else if (has("BLOOD"))
            color = {139.0F / 255.0F, 0.0F, 0.0F};
    }

    void spawn_tracer(world::Vec3 origin, world::Vec3 direction, float damage,
                      world::DamageFlags flags, InventoryItemId item, bool homing_launch = false,
                      std::uint8_t owner_gid = 0U, bool locally_authoritative = true)
    {
        float velocity{};
        float flight{};
        std::array<float, 3> color{};
        tracer_ballistics(item, velocity, flight, color);
        Tracer tracer;
        tracer.laser = is_laser_gun(item);
        if (tracer.laser)
        {
            tracer.head = tracer.tail = origin;
            tracer.head_velocity = tracer.tail_velocity = {direction.x * laser_projectile_speed,
                                                           direction.y * laser_projectile_speed,
                                                           direction.z * laser_projectile_speed};
            tracer.time_left = tracer.flight_time = laser_projectile_lifetime_seconds;
            tracer.bounces = laser_projectile_bounces;
        }
        else
        {
            tracer.head =
                tracer.tail = {origin.x + direction.x * 0.5F, origin.y + direction.y * 0.5F,
                               origin.z + direction.z * 0.5F};
            tracer.head_velocity = tracer.tail_velocity = {
                direction.x * velocity, direction.y * velocity, direction.z * velocity};
            tracer.time_left = flight;
            tracer.flight_time = flight;
        }
        tracer.color = color;
        tracer.damage = damage;
        tracer.flags = flags;
        tracer.item = item;
        tracer.id = next_projectile_id_++;
        tracer.rocket = item == InventoryItemId::rocket_launcher ||
                        item == InventoryItemId::guided_rocket_launcher;
        tracer.guided_launcher = item == InventoryItemId::guided_rocket_launcher;
        tracer.guided = tracer.guided_launcher && homing_launch;
        tracer.owner_gid = owner_gid;
        tracer.locally_authoritative = locally_authoritative;
        tracer.rocket_direction = direction;
        tracer.rocket_emitted_direction = direction;
        if (tracer.rocket)
        {
            // FireRocketMessage.Position is camera + direction, then the
            // RocketEntity constructor adds direction again for _startPoint.
            // It therefore begins two units ahead, before the quarter-second
            // 3.5 u/s upward-biased ballistic launch.
            tracer.head =
                tracer.tail = {origin.x + direction.x * 2.0F, origin.y + direction.y * 2.0F,
                               origin.z + direction.z * 2.0F};
            tracer.rocket_start = tracer.head;
            tracer.rocket_guided_position = tracer.head;
            world::Vec3 launch{direction.x * 0.25F, direction.y * 0.25F + 0.75F,
                               direction.z * 0.25F};
            const float launch_length =
                std::sqrt(launch.x * launch.x + launch.y * launch.y + launch.z * launch.z);
            tracer.rocket_starting_velocity = {launch.x / launch_length * 3.5F,
                                               launch.y / launch_length * 3.5F,
                                               launch.z / launch_length * 3.5F};
            // _runningTime starts at -0.25 and detonates after it exceeds 10.
            tracer.time_left = tracer.flight_time = 10.25F;
        }
        tracers_.push_back(tracer);
    }

    void update_tracers(float fixed_delta)
    {
        for (Tracer &tracer : tracers_)
        {
            tracer.time_left -= fixed_delta;
            // TracerManager marks ordinary tracers RemoveAfterDraw when their
            // timer goes negative, but still advances and collides that final
            // frame. BlasterShot and RocketEntity expire before another trace.
            const bool conventional_expires_after_trace =
                !tracer.rocket && !tracer.laser && tracer.time_left < 0.0F;
            if (tracer.laser && tracer.time_left <= 0.0F)
            {
                tracer.dead = true;
                continue;
            }
            if (tracer.rocket)
            {
                const float elapsed = tracer.flight_time - tracer.time_left;
                const float running_time = elapsed - 0.25F;
                const float guidance_time = tracer.guided_launcher ? 2.5F : 1.0F;
                const float ballistic_time = running_time + 0.25F;
                const world::Vec3 ballistic{
                    tracer.rocket_start.x + tracer.rocket_starting_velocity.x * ballistic_time,
                    tracer.rocket_start.y + tracer.rocket_starting_velocity.y * ballistic_time -
                        4.9F * ballistic_time * ballistic_time,
                    tracer.rocket_start.z + tracer.rocket_starting_velocity.z * ballistic_time};
                world::Vec3 desired = tracer.rocket_emitted_direction;
                world::Vec3 visual_direction = desired;
                if (tracer.guided && dragon_.visible())
                {
                    const world::Vec3 to_dragon{dragon_.dragon().position.x - tracer.head.x,
                                                dragon_.dragon().position.y - tracer.head.y,
                                                dragon_.dragon().position.z - tracer.head.z};
                    const float target_length =
                        std::sqrt(to_dragon.x * to_dragon.x + to_dragon.y * to_dragon.y +
                                  to_dragon.z * to_dragon.z);
                    if (target_length > 0.001F)
                    {
                        const world::Vec3 target{to_dragon.x / target_length,
                                                 to_dragon.y / target_length,
                                                 to_dragon.z / target_length};
                        const float guidance = std::clamp(running_time / guidance_time, 0.0F, 1.0F);
                        const auto blend_direction = [&](float amount) {
                            world::Vec3 result{tracer.rocket_emitted_direction.x * (1.0F - amount) +
                                                   target.x * amount,
                                               tracer.rocket_emitted_direction.y * (1.0F - amount) +
                                                   target.y * amount,
                                               tracer.rocket_emitted_direction.z * (1.0F - amount) +
                                                   target.z * amount};
                            const float result_length = std::sqrt(
                                result.x * result.x + result.y * result.y + result.z * result.z);
                            return world::Vec3{result.x / result_length, result.y / result_length,
                                               result.z / result_length};
                        };
                        desired = blend_direction(guidance);
                        visual_direction = blend_direction(std::sqrt(guidance));
                    }
                }
                tracer.rocket_direction = visual_direction;
                const float maximum_speed = tracer.guided_launcher ? 50.0F : 25.0F;
                const float speed =
                    running_time >= 0.0F
                        ? maximum_speed * std::sqrt(std::clamp(running_time, 0.0F, 1.0F))
                        : 0.0F;
                tracer.tail = tracer.head;
                if (running_time >= 0.0F)
                {
                    tracer.rocket_guided_position.x += desired.x * speed * fixed_delta;
                    tracer.rocket_guided_position.y += desired.y * speed * fixed_delta;
                    tracer.rocket_guided_position.z += desired.z * speed * fixed_delta;
                    if (running_time < 1.0F)
                    {
                        const float blend = std::sqrt(std::clamp(running_time, 0.0F, 1.0F));
                        tracer.head = {
                            std::lerp(ballistic.x, tracer.rocket_guided_position.x, blend),
                            std::lerp(ballistic.y, tracer.rocket_guided_position.y, blend),
                            std::lerp(ballistic.z, tracer.rocket_guided_position.z, blend)};
                    }
                    else
                        tracer.head = tracer.rocket_guided_position;
                }
                else
                    tracer.head = ballistic;
                tracer.rocket_spin += fixed_delta * 0.5F * speed;
                if (static_cast<int>(elapsed) != static_cast<int>(elapsed - fixed_delta))
                    play_positional("RocketWhoosh", tracer.head);
                // RocketEntity retires harmlessly outside BlockTerrain instead
                // of detonating. A locked guided round is exempt only while its
                // dragon target is still alive, so it can chase beyond the
                // currently streamed terrain footprint.
                if (!(tracer.guided && dragon_.visible()))
                {
                    const auto &region = world_preview_->region();
                    const bool outside =
                        tracer.head.x < static_cast<float>(region.minimum_x()) ||
                        tracer.head.x >= static_cast<float>(region.maximum_x() + 1) ||
                        tracer.head.z < static_cast<float>(region.minimum_z()) ||
                        tracer.head.z >= static_cast<float>(region.maximum_z() + 1) ||
                        tracer.head.y < static_cast<float>(world::world_min_y);
                    if (outside)
                    {
                        tracer.dead = true;
                        continue;
                    }
                }
                // RocketEntity advances first, then detonates only after
                // _runningTime exceeds ten (equality still gets one trace).
                if (running_time > 10.0F)
                {
                    if (tracer.locally_authoritative)
                        explode_rocket(tracer.head, false, false, tracer.item);
                    else
                        remember_network_rocket(tracer.owner_gid, tracer.item, tracer.head);
                    tracer.dead = true;
                    continue;
                }
            }
            else if (tracer.laser)
            {
                // BlasterShot has no ballistic drop. Its collision segment is
                // last position to the new 200 u/s position.
                tracer.tail = tracer.head;
                tracer.head.x += tracer.head_velocity.x * fixed_delta;
                tracer.head.y += tracer.head_velocity.y * fixed_delta;
                tracer.head.z += tracer.head_velocity.z * fixed_delta;
            }
            else
            {
                tracer.head.x += tracer.head_velocity.x * fixed_delta;
                tracer.head.y += tracer.head_velocity.y * fixed_delta;
                tracer.head.z += tracer.head_velocity.z * fixed_delta;
                tracer.head_velocity.y -= 10.0F * fixed_delta;
                if (tracer.time_left < tracer.flight_time - 0.4F)
                {
                    tracer.tail.x += tracer.tail_velocity.x * fixed_delta;
                    tracer.tail.y += tracer.tail_velocity.y * fixed_delta;
                    tracer.tail.z += tracer.tail_velocity.z * fixed_delta;
                    tracer.tail_velocity.y -= 10.0F * fixed_delta;
                }
            }
            // The collision segment is the whole visible streak, like the
            // original's TraceProbe(Tail, Head).
            const world::Vec3 segment{tracer.head.x - tracer.tail.x, tracer.head.y - tracer.tail.y,
                                      tracer.head.z - tracer.tail.z};
            const float length =
                std::sqrt(segment.x * segment.x + segment.y * segment.y + segment.z * segment.z);
            if (length < 0.001F)
            {
                tracer.dead = conventional_expires_after_trace;
                continue;
            }
            const world::Vec3 direction{segment.x / length, segment.y / length, segment.z / length};
            // TracerProbe accepts CanBeTouched && BlockPlayer. In particular,
            // bullets and blaster bolts pass through leaves, torches, lava and
            // open doors instead of striking any merely rendered voxel.
            const auto terrain_hit = world::PlayerSimulation::raycast_blocking(
                world_preview_->region(), tracer.tail, direction, length);
            const float terrain_range = terrain_hit.has_value() ? terrain_hit->distance : length;
            if (tracer.rocket)
            {
                float impact_distance =
                    terrain_range < length - 0.001F ? terrain_range : length + 1.0F;
                bool hit_dragon = false;
                bool dragon_explosive_headshot = false;
                const world::EnemyHorde::RayHit enemy_hit = enemy_horde_.trace_projectile_along_ray(
                    tracer.tail, direction, std::min(terrain_range, length));
                if (enemy_hit && enemy_hit.distance < impact_distance)
                    impact_distance = enemy_hit.distance;
                if (dragon_.visible())
                {
                    const auto dragon_hit =
                        world::trace_dragon(dragon_.dragon(), tracer.tail, direction,
                                            std::min(length, impact_distance));
                    if (dragon_hit.has_value() && dragon_hit->distance < impact_distance)
                    {
                        impact_distance = dragon_hit->distance;
                        hit_dragon = true;
                        dragon_explosive_headshot = dragon_hit->explosive_headshot;
                    }
                }
                if (impact_distance <= length)
                {
                    const world::Vec3 impact{tracer.tail.x + direction.x * impact_distance,
                                             tracer.tail.y + direction.y * impact_distance,
                                             tracer.tail.z + direction.z * impact_distance};
                    if (tracer.locally_authoritative)
                        explode_rocket(impact, hit_dragon, dragon_explosive_headshot, tracer.item);
                    else
                        remember_network_rocket(tracer.owner_gid, tracer.item, impact);
                    tracer.dead = true;
                }
                continue;
            }
            const world::EnemyHorde::RayHit enemy_hit =
                enemy_horde_.trace_projectile_along_ray(tracer.tail, direction, terrain_range);
            std::optional<std::pair<std::uint8_t, float>> player_hit;
            if (online_gameplay_active() && online_pvp_policy() != net::PlayerVersusPlayer::off)
            {
                const auto consider_player = [&](std::uint8_t gid, world::Vec3 position,
                                                 bool dead) {
                    if (gid == tracer.owner_gid || dead)
                        return;
                    const world::Vec3 minimum{position.x - 0.35F, position.y,
                                              position.z - 0.35F};
                    const world::Vec3 maximum{position.x + 0.35F, position.y + 1.7F,
                                              position.z + 0.35F};
                    const auto hit_time = ray_box_time(tracer.tail, segment, minimum, maximum);
                    if (!hit_time.has_value())
                        return;
                    const float distance = *hit_time * length;
                    if (distance <= terrain_range &&
                        (!player_hit.has_value() || distance < player_hit->second))
                        player_hit = std::pair{gid, distance};
                };
                consider_player(online_local_player_gid(), player_.state().position,
                                player_life_ != PlayerLife::alive);
                for (const auto &[gid, remote] : online_remote_players_)
                    if (remote.has_update && elapsed_ - remote.last_update <= 10.0F)
                        consider_player(gid, remote.state.position, remote.state.dead);
            }
            if (player_hit.has_value() &&
                (!enemy_hit || player_hit->second < enemy_hit.distance))
            {
                const world::Vec3 impact{tracer.tail.x + direction.x * player_hit->second,
                                         tracer.tail.y + direction.y * player_hit->second,
                                         tracer.tail.z + direction.z * player_hit->second};
                if (player_hit->first == online_local_player_gid() &&
                    melee_pvp_allows(online_pvp_policy(), online_host_ != nullptr,
                                     online_lobby_host_is_friend_))
                    // TracerManager's PvP path is deliberately weapon-agnostic:
                    // every accepted gun tracer removes 0.21 normalized health.
                    apply_player_damage(21.0F, impact);
                play_positional("BulletHitHuman", impact);
                world_preview_->spawn_impact_smoke(impact, {1.0F, 1.0F, 1.0F});
                tracer.dead = true;
                continue;
            }
            if (enemy_hit)
            {
                damage_enemy_with_network(static_cast<std::size_t>(enemy_hit.index), tracer.damage,
                                          tracer.flags, enemy_hit.headshot, tracer.item,
                                          tracer.locally_authoritative);
                caption("[ TARGET HIT ]", 0.6F);
                if (static_cast<std::size_t>(enemy_hit.index) < enemy_horde_.enemies().size())
                {
                    const world::Enemy &victim =
                        enemy_horde_.enemies()[static_cast<std::size_t>(enemy_hit.index)];
                    const world::Vec3 impact{tracer.tail.x + direction.x * enemy_hit.distance,
                                             tracer.tail.y + direction.y * enemy_hit.distance,
                                             tracer.tail.z + direction.z * enemy_hit.distance};
                    if (!tracer.laser)
                        play_positional("BulletHitHuman", victim.position);
                    if (tracer.laser)
                    {
                        world::BlockBreakUpdate blaster;
                        blaster.intersection = impact;
                        blaster.normal_x = static_cast<int>(std::lround(enemy_hit.normal.x));
                        blaster.normal_y = static_cast<int>(std::lround(enemy_hit.normal.y));
                        blaster.normal_z = static_cast<int>(std::lround(enemy_hit.normal.z));
                        world_preview_->spawn_blaster_impact(blaster);
                    }
                    else
                        // TracerManager never recolors SmokeEffect per enemy;
                        // zombies, skeletons, demons and aliens share its authored tint.
                        world_preview_->spawn_impact_smoke(impact, {1.0F, 1.0F, 1.0F});
                }
                tracer.dead = true;
                continue;
            }
            if (terrain_hit.has_value() && terrain_range < length - 0.001F)
            {
                const world::Vec3 hit{tracer.tail.x + direction.x * terrain_range,
                                      tracer.tail.y + direction.y * terrain_range,
                                      tracer.tail.z + direction.z * terrain_range};
                const int cell_x = terrain_hit->x;
                const int cell_y = terrain_hit->y;
                const int cell_z = terrain_hit->z;
                world::BlockBreakUpdate burst{};
                burst.x = cell_x;
                burst.y = cell_y;
                burst.z = cell_z;
                burst.intersection = hit;
                burst.block = terrain_hit->block;
                burst.normal_x = terrain_hit->normal_x;
                burst.normal_y = terrain_hit->normal_y;
                burst.normal_z = terrain_hit->normal_z;
                if (tracer.laser)
                {
                    // BlasterShot always emits its flash/sparks/smoke triplet,
                    // including impacts which immediately ricochet.
                    world_preview_->spawn_blaster_impact(burst);
                    if (world::bounces_lasers(terrain_hit->block) && tracer.bounces > 0)
                    {
                        --tracer.bounces;
                        const world::Vec3 normal{static_cast<float>(terrain_hit->normal_x),
                                                 static_cast<float>(terrain_hit->normal_y),
                                                 static_cast<float>(terrain_hit->normal_z)};
                        const auto reflect = [&](world::Vec3 value) {
                            const float dot =
                                value.x * normal.x + value.y * normal.y + value.z * normal.z;
                            return world::Vec3{value.x - 2.0F * dot * normal.x,
                                               value.y - 2.0F * dot * normal.y,
                                               value.z - 2.0F * dot * normal.z};
                        };
                        const world::Vec3 remainder{tracer.head.x - hit.x, tracer.head.y - hit.y,
                                                    tracer.head.z - hit.z};
                        const world::Vec3 reflected_remainder = reflect(remainder);
                        tracer.head = {hit.x + reflected_remainder.x, hit.y + reflected_remainder.y,
                                       hit.z + reflected_remainder.z};
                        tracer.tail = hit;
                        tracer.head_velocity = reflect(tracer.head_velocity);
                        tracer.tail_velocity = tracer.head_velocity;
                        continue;
                    }
                    if (world::can_be_dug(terrain_hit->block))
                    {
                        // FindBlocksToRemove(blockToDestroy, Laser, true): a
                        // zero-range laser blast, not a damaging explosion.
                        destroy_blocks_with_explosive({static_cast<float>(cell_x) + 0.5F,
                                                       static_cast<float>(cell_y) + 0.5F,
                                                       static_cast<float>(cell_z) + 0.5F},
                                                      world::ExplosiveKind::laser);
                    }
                    tracer.dead = true;
                    continue;
                }
                world_preview_->spawn_tracer_impact(burst);
                play_positional("BulletHitDirt", hit);
                // TracerManager sends an immediate original-explosion message
                // when an ordinary bullet hits placed TNT/C4. It does not wait
                // for the four-second activation fuse.
                if (const auto explosive = world::placed_explosive_kind(terrain_hit->block);
                    explosive.has_value())
                {
                    detonate_explosive({static_cast<float>(cell_x) + 0.5F,
                                        static_cast<float>(cell_y) + 0.5F,
                                        static_cast<float>(cell_z) + 0.5F},
                                       *explosive);
                }
                tracer.dead = true;
                continue;
            }
            if (dragon_.visible())
            {
                if (const auto dragon_hit =
                        world::trace_dragon(dragon_.dragon(), tracer.tail, direction, length);
                    dragon_hit.has_value())
                {
                    const world::Vec3 impact = dragon_hit->position;
                    damage_dragon_with_network(tracer.damage, dragon_.dragon().position,
                                               tracer.item, tracer.owner_gid);
                    caption("[ DRAGON HIT ]", 0.7F);
                    // TracerManager's dragon feedback: a FlashEffect burst
                    // plus the untinted SmokeEffect puff at the hit point.
                    world_preview_->spawn_flash_effect(impact);
                    if (tracer.laser)
                    {
                        world::BlockBreakUpdate blaster;
                        blaster.intersection = impact;
                        blaster.normal_x = static_cast<int>(std::lround(-direction.x));
                        blaster.normal_y = static_cast<int>(std::lround(-direction.y));
                        blaster.normal_z = static_cast<int>(std::lround(-direction.z));
                        world_preview_->spawn_blaster_impact(blaster);
                    }
                    else
                        world_preview_->spawn_impact_smoke(impact, {1.0F, 1.0F, 1.0F});
                    tracer.dead = true;
                    continue;
                }
            }
            if (conventional_expires_after_trace)
                tracer.dead = true;
            else if (!tracer.rocket && !tracer.laser)
            {
                // BlockTerrain.IsTracerStillInWorld has no upper-Y limit: sky
                // shots remain valid, but rounds below or outside the streamed
                // X/Z footprint are retired immediately after their last trace.
                const auto &region = world_preview_->region();
                tracer.dead = tracer.head.x < static_cast<float>(region.minimum_x()) ||
                              tracer.head.x >= static_cast<float>(region.maximum_x() + 1) ||
                              tracer.head.z < static_cast<float>(region.minimum_z()) ||
                              tracer.head.z >= static_cast<float>(region.maximum_z() + 1) ||
                              tracer.head.y < static_cast<float>(world::world_min_y);
            }
        }
        std::erase_if(tracers_, [](const Tracer &tracer) { return tracer.dead; });
    }

    [[nodiscard]] static InventoryItemId explosive_weapon(world::ExplosiveKind kind) noexcept
    {
        switch (kind)
        {
        case world::ExplosiveKind::tnt:
            return InventoryItemId::tnt;
        case world::ExplosiveKind::c4:
            return InventoryItemId::c4;
        case world::ExplosiveKind::rocket:
            return InventoryItemId::rocket_launcher;
        case world::ExplosiveKind::he_grenade:
            return InventoryItemId::grenade;
        case world::ExplosiveKind::laser:
        default:
            return InventoryItemId::bare_hands;
        }
    }

    void apply_explosion_splash(world::Vec3 location, world::ExplosiveDamageBands bands,
                                bool damage_local_player = true,
                                bool locally_authoritative_enemy_damage = true,
                                InventoryItemId weapon = InventoryItemId::grenade)
    {
        const world::Vec3 ear{player_.state().position.x, player_.state().position.y + 1.0F,
                              player_.state().position.z};
        const world::Vec3 offset{ear.x - location.x, ear.y - location.y, ear.z - location.z};
        const float distance =
            std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        if (damage_local_player && distance < bands.damage_range)
        {
            float damage = world::explosive_damage_fraction(distance, bands);
            // DamageLOSProbe is only consulted outside the guaranteed kill
            // radius. Cover never saves a player standing inside that core.
            if (distance >= bands.kill_range && distance > 0.001F)
                damage *= world::dragon_fireball_damage_multiplier(
                    world_preview_->region(), world::DragonKind::fire, location, ear);
            apply_player_damage(std::clamp(damage, 0.0F, 1.0F) * 100.0F, location);
        }
        const std::vector<std::size_t> killed = enemy_horde_.apply_explosive_damage(
            location, bands.enemy_range, locally_authoritative_enemy_damage);
        if (locally_authoritative_enemy_damage)
            for (const std::size_t index : killed)
                if (index < enemy_horde_.enemies().size())
                {
                    const world::Enemy &enemy = enemy_horde_.enemies()[index];
                    const world::EnemyIdentity identity{enemy.target_gid, enemy.id};
                    publish_local_enemy_kill(identity, weapon);
                    notify_local_spawner_enemy_defeated(
                        identity, online_gameplay_active() ? online_local_player_gid() : 0U);
                }
    }

    [[nodiscard]] bool publish_local_placed_explosive_detonation(int x, int y, int z,
                                                                 world::ExplosiveKind kind,
                                                                 bool original_explosion)
    {
        if (kind != world::ExplosiveKind::tnt && kind != world::ExplosiveKind::c4)
            return false;
        net::PayloadError payload_error{};
        const net::DetonateExplosivePayload message{
            {x, y, z}, original_explosion, static_cast<std::uint8_t>(kind)};
        const auto payload = net::encode_detonate_explosive_payload(
            message, online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("DetonateExplosiveMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.detonate-explosive.failure=" << error << '\n';
            return false;
        }
        return true;
    }

    void destroy_blocks_with_explosive(world::Vec3 location, world::ExplosiveKind root_kind)
    {
        if (root_kind == world::ExplosiveKind::laser)
        {
            const net::IntVector3 effect_cell{static_cast<int>(std::floor(location.x)),
                                              static_cast<int>(std::floor(location.y)),
                                              static_cast<int>(std::floor(location.z))};
            net::PayloadError payload_error{};
            const auto payload = net::encode_int_vector_payload(effect_cell, payload_error);
            std::string error;
            if (!payload.has_value() ||
                !publish_online_broadcast("AddExplosionEffectsMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_unordered, error))
                std::cerr << "network.local.explosion-effects.failure=" << error << '\n';
            const world::Vec3 effect_location{static_cast<float>(effect_cell.x),
                                              static_cast<float>(effect_cell.y),
                                              static_cast<float>(effect_cell.z)};
            play_positional("Explosion", effect_location);
            world_preview_->spawn_fireball_explosion(effect_location, false, true);
        }
        struct Source final
        {
            int x{};
            int y{};
            int z{};
            world::ExplosiveKind kind{};
        };
        auto &region = world_preview_->region();
        std::vector<Source> queue;
        std::set<std::array<int, 3>> sources;
        std::set<std::array<int, 3>> removed;
        const Source root{static_cast<int>(std::floor(location.x)),
                          static_cast<int>(std::floor(location.y)),
                          static_cast<int>(std::floor(location.z)), root_kind};
        queue.push_back(root);
        sources.insert({root.x, root.y, root.z});
        if (root_kind == world::ExplosiveKind::tnt || root_kind == world::ExplosiveKind::c4)
            removed.insert({root.x, root.y, root.z});

        for (std::size_t source_index = 0; source_index < queue.size(); ++source_index)
        {
            const Source source = queue[source_index];
            if (source_index > 0U)
            {
                const world::Vec3 chain_location{static_cast<float>(source.x),
                                                 static_cast<float>(source.y),
                                                 static_cast<float>(source.z)};
                static_cast<void>(publish_local_placed_explosive_detonation(
                    source.x, source.y, source.z, source.kind, false));
                apply_explosion_splash(chain_location,
                                       world::explosive_definition(source.kind).damage, true, true,
                                       explosive_weapon(source.kind));
            }
            const int range = world::explosive_definition(source.kind).destruction_range;
            for (int dz = -range; dz <= range; ++dz)
                for (int dy = -range; dy <= range; ++dy)
                    for (int dx = -range; dx <= range; ++dx)
                    {
                        const int x = source.x + dx;
                        const int y = source.y + dy;
                        const int z = source.z + dz;
                        if (!region.contains(x, y, z))
                            continue;
                        const world::BlockType block = region.block(x, y, z);
                        if (block == world::BlockType::tnt || block == world::BlockType::c4)
                        {
                            const std::array<int, 3> key{x, y, z};
                            removed.insert(key);
                            if (sources.insert(key).second)
                            {
                                queue.push_back({x, y, z,
                                                 block == world::BlockType::tnt
                                                     ? world::ExplosiveKind::tnt
                                                     : world::ExplosiveKind::c4});
                            }
                            continue;
                        }
                        if (!world::explosive_breaks_block(source.kind, block, dx, dy, dz))
                            continue;
                        removed.insert({x, y, z});
                        // Lower doors own the upper half; ProcessOneExplosion
                        // removes both even though upper halves are otherwise skipped.
                        if (world::is_lower_door(block))
                            removed.insert({x, y + 1, z});
                    }
        }

        // Original ProcessExplosionDependents drops fixtures whose support
        // vanished, but not ordinary blast-destroyed blocks or paired door
        // upper halves.
        const std::set<std::array<int, 3>> blast_removed = removed;
        std::set<std::array<int, 3>> dependent_drop_cells;
        std::vector<world::BlockRemovalEffect> dependent_drops;
        for (const std::array<int, 3> &cell : blast_removed)
        {
            for (const world::BlockRemovalEffect &effect :
                 region.removal_effects(cell[0], cell[1], cell[2]))
            {
                const std::array<int, 3> key{effect.x, effect.y, effect.z};
                removed.insert(key);
                if (effect.drops_as_dependent && !blast_removed.contains(key) &&
                    dependent_drop_cells.insert(key).second)
                    dependent_drops.push_back(effect);
            }
        }

        std::map<CrateKey, std::pair<world::BlockType, InventoryItemId>> removed_doors;
        for (const std::array<int, 3> &cell : removed)
        {
            if (!region.contains(cell[0], cell[1], cell[2]))
                continue;
            const world::BlockType block = region.block(cell[0], cell[1], cell[2]);
            if (world::is_lower_door(block) || world::is_upper_door(block))
            {
                const CrateKey key = lower_door_key({cell[0], cell[1], cell[2]}, block);
                InventoryItemId item = block_item(block);
                const auto identity = custom_doors_.find(key);
                if (identity != custom_doors_.end())
                {
                    const std::optional<InventoryItemId> modeled =
                        door_item_from_model(identity->second);
                    if (modeled.has_value())
                        item = *modeled;
                }
                removed_doors.try_emplace(key, block, item);
            }
        }
        for (const auto &[key, door] : removed_doors)
            publish_local_custom_door_destroy(key, door.first);

        for (const std::array<int, 3> &cell : removed)
        {
            if (!region.contains(cell[0], cell[1], cell[2]))
                continue;
            if (world::is_container(region.block(cell[0], cell[1], cell[2])))
                eject_crate_contents({cell[0], cell[1], cell[2]});
            region.set_block(cell[0], cell[1], cell[2], world::BlockType::empty);
            world_preview_->remember_edit(cell[0], cell[1], cell[2], world::BlockType::empty);
        }
        for (const world::BlockRemovalEffect &effect : dependent_drops)
        {
            if (world::is_lower_door(effect.block) || world::is_upper_door(effect.block))
            {
                const CrateKey key = lower_door_key({effect.x, effect.y, effect.z}, effect.block);
                const auto door = removed_doors.find(key);
                if (door != removed_doors.end())
                {
                    world_preview_->spawn_created_stack_at(InventoryStack{door->second.second, 1U},
                                                           {static_cast<float>(key[0]),
                                                            static_cast<float>(key[1]),
                                                            static_cast<float>(key[2])});
                    continue;
                }
            }
            world::BlockBreakUpdate detached;
            detached.x = effect.x;
            detached.y = effect.y;
            detached.z = effect.z;
            detached.block = effect.block;
            world_preview_->spawn_block_pickup(detached);
        }
        if (!removed.empty())
        {
            std::erase_if(armed_explosives_, [&](const ArmedExplosive &armed) {
                return removed.contains({armed.x, armed.y, armed.z});
            });
            const std::vector<std::array<int, 3>> removed_cells{removed.begin(), removed.end()};
            world_preview_->rebuild_block_edits(removed_cells);
            if (!publish_local_block_removals(removed_cells, false))
                std::cerr << "network.local.explosion-removal.failure\n";
        }
    }

    void detonate_explosive(world::Vec3 location, world::ExplosiveKind kind,
                            bool want_rock_chunks = true)
    {
        if (kind == world::ExplosiveKind::tnt || kind == world::ExplosiveKind::c4)
            static_cast<void>(publish_local_placed_explosive_detonation(
                static_cast<int>(std::floor(location.x)), static_cast<int>(std::floor(location.y)),
                static_cast<int>(std::floor(location.z)), kind, true));
        if (kind == world::ExplosiveKind::he_grenade)
        {
            net::PayloadError payload_error{};
            const net::DetonateGrenadePayload message{
                {location.x, location.y, location.z}, 0U, want_rock_chunks};
            const auto payload = net::encode_detonate_grenade_payload(
                message, online_gameplay_protocol(), payload_error);
            std::string error;
            if (!payload.has_value() ||
                !publish_online_broadcast("DetonateGrenadeMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_unordered, error))
                std::cerr << "network.local.detonate-grenade.failure=" << error << '\n';
        }
        play_positional("Explosion", location);
        world_preview_->spawn_fireball_explosion(location, false, want_rock_chunks);
        apply_explosion_splash(location, world::explosive_definition(kind).damage, true, true,
                               explosive_weapon(kind));
        destroy_blocks_with_explosive(location, kind);
    }

    void process_enemy_block_breaks(const world::EnemyEvents &events)
    {
        auto &region = world_preview_->region();

        // EnemyBreakBlocks arms TNT/C4 on every failed break roll. SetFuseForExplosive
        // ignores a block that is already flashing/fusing.
        for (const std::array<int, 3> &cell : events.fuse_blocks)
        {
            if (!region.contains(cell[0], cell[1], cell[2]))
                continue;
            const world::BlockType block = region.block(cell[0], cell[1], cell[2]);
            if (block == world::BlockType::tnt || block == world::BlockType::c4)
                (void)arm_placed_explosive(cell[0], cell[1], cell[2],
                                           block == world::BlockType::tnt
                                               ? world::ExplosiveKind::tnt
                                               : world::ExplosiveKind::c4,
                                           false);
        }

        std::set<std::array<int, 3>> primary_removed;
        std::set<std::array<int, 3>> regular_primaries;
        struct EnemyDetonation final
        {
            std::array<int, 3> cell{};
            world::ExplosiveKind kind{world::ExplosiveKind::tnt};
        };
        std::vector<EnemyDetonation> detonations;
        for (const std::array<int, 3> &cell : events.dug_blocks)
        {
            if (!region.contains(cell[0], cell[1], cell[2]) || !primary_removed.insert(cell).second)
                continue;
            const world::BlockType block = region.block(cell[0], cell[1], cell[2]);
            if (block == world::BlockType::empty || block == world::BlockType::uninitialized ||
                world::is_upper_door(block))
            {
                primary_removed.erase(cell);
                continue;
            }
            if (block == world::BlockType::tnt || block == world::BlockType::c4)
            {
                detonations.push_back({cell, block == world::BlockType::tnt
                                                 ? world::ExplosiveKind::tnt
                                                 : world::ExplosiveKind::c4});
            }
            else
            {
                regular_primaries.insert(cell);
            }
        }

        // RememberDependentObjects is run for ordinary dug blocks (not the TNT/C4
        // sources). Directly-dug blocks never drop; fixtures detached as a side
        // effect drop once, and lower doors own their upper half.
        std::set<std::array<int, 3>> removed = primary_removed;
        std::set<std::array<int, 3>> dependent_drop_cells;
        std::vector<world::BlockRemovalEffect> dependent_drops;
        for (const std::array<int, 3> &cell : regular_primaries)
        {
            for (const world::BlockRemovalEffect &effect :
                 region.removal_effects(cell[0], cell[1], cell[2]))
            {
                const std::array<int, 3> key{effect.x, effect.y, effect.z};
                removed.insert(key);
                if (effect.drops_as_dependent && !primary_removed.contains(key) &&
                    dependent_drop_cells.insert(key).second)
                    dependent_drops.push_back(effect);
            }
        }

        std::map<CrateKey, std::pair<world::BlockType, InventoryItemId>> removed_doors;
        for (const std::array<int, 3> &cell : removed)
        {
            if (!region.contains(cell[0], cell[1], cell[2]))
                continue;
            const world::BlockType block = region.block(cell[0], cell[1], cell[2]);
            if (world::is_lower_door(block) || world::is_upper_door(block))
            {
                const CrateKey key = lower_door_key({cell[0], cell[1], cell[2]}, block);
                InventoryItemId item = block_item(block);
                const auto identity = custom_doors_.find(key);
                if (identity != custom_doors_.end())
                {
                    const std::optional<InventoryItemId> modeled =
                        door_item_from_model(identity->second);
                    if (modeled.has_value())
                        item = *modeled;
                }
                removed_doors.try_emplace(key, block, item);
            }
        }
        for (const auto &[key, door] : removed_doors)
            publish_local_custom_door_destroy(key, door.first);

        for (const std::array<int, 3> &cell : removed)
        {
            if (!region.contains(cell[0], cell[1], cell[2]))
                continue;
            const world::BlockType block = region.block(cell[0], cell[1], cell[2]);
            if (block == world::BlockType::empty || block == world::BlockType::uninitialized)
                continue;
            if (world::is_container(block))
                eject_crate_contents({cell[0], cell[1], cell[2]});

            // RemoveBlocksMessage(doDigEffects=true) produces a ground crash and
            // the smoke/rock dig burst at every cell, including paired/dependent ones.
            world::BlockBreakUpdate impact;
            impact.x = cell[0];
            impact.y = cell[1];
            impact.z = cell[2];
            impact.block = block;
            impact.intersection = {static_cast<float>(cell[0]) + 0.5F,
                                   static_cast<float>(cell[1]) + 0.5F,
                                   static_cast<float>(cell[2]) + 0.5F};
            impact.normal_y = 1;
            play_positional("GroundCrash", impact.intersection);
            world_preview_->spawn_mining_impact(impact);
            region.set_block(cell[0], cell[1], cell[2], world::BlockType::empty);
            world_preview_->remember_edit(cell[0], cell[1], cell[2], world::BlockType::empty);
        }
        for (const world::BlockRemovalEffect &effect : dependent_drops)
        {
            if (world::is_lower_door(effect.block) || world::is_upper_door(effect.block))
            {
                const CrateKey key = lower_door_key({effect.x, effect.y, effect.z}, effect.block);
                const auto door = removed_doors.find(key);
                if (door != removed_doors.end())
                {
                    world_preview_->spawn_created_stack_at(InventoryStack{door->second.second, 1U},
                                                           {static_cast<float>(key[0]),
                                                            static_cast<float>(key[1]),
                                                            static_cast<float>(key[2])});
                    continue;
                }
            }
            world::BlockBreakUpdate detached;
            detached.x = effect.x;
            detached.y = effect.y;
            detached.z = effect.z;
            detached.block = effect.block;
            world_preview_->spawn_block_pickup(detached);
        }
        if (!removed.empty())
        {
            std::erase_if(armed_explosives_, [&](const ArmedExplosive &armed) {
                return removed.contains({armed.x, armed.y, armed.z});
            });
            const std::vector<std::array<int, 3>> removed_cells{removed.begin(), removed.end()};
            world_preview_->rebuild_block_edits(removed_cells);
        }

        // The original removes the successful dig set first, then processes each
        // queued explosive immediately so its blast propagates through the new air.
        for (const EnemyDetonation &detonation : detonations)
            detonate_explosive({static_cast<float>(detonation.cell[0]),
                                static_cast<float>(detonation.cell[1]),
                                static_cast<float>(detonation.cell[2])},
                               detonation.kind);
    }

    void update_placed_explosives(float fixed_delta)
    {
        std::vector<ArmedExplosive> detonations;
        for (ArmedExplosive &armed : armed_explosives_)
        {
            if (!world_preview_->region().contains(armed.x, armed.y, armed.z))
            {
                armed.dead = true;
                continue;
            }
            const world::BlockType expected = armed.kind == world::ExplosiveKind::tnt
                                                  ? world::BlockType::tnt
                                                  : world::BlockType::c4;
            if (world_preview_->region().block(armed.x, armed.y, armed.z) != expected)
            {
                armed.dead = true;
                continue;
            }
            armed.age += fixed_delta;
            if (!armed.locally_authoritative)
                continue;
            armed.time_left -= fixed_delta;
            if (armed.time_left <= 0.0F)
            {
                detonations.push_back(armed);
                armed.dead = true;
            }
        }
        std::erase_if(armed_explosives_, [](const ArmedExplosive &armed) { return armed.dead; });
        for (const ArmedExplosive &armed : detonations)
            detonate_explosive({static_cast<float>(armed.x), static_cast<float>(armed.y),
                                static_cast<float>(armed.z)},
                               armed.kind);
    }

    void update_grenades(float fixed_delta)
    {
        grenade_cooldown_ = std::max(0.0F, grenade_cooldown_ - fixed_delta);
        grenade_animation_time_ = std::max(0.0F, grenade_animation_time_ - fixed_delta);
        if (grenade_cooking_)
        {
            grenade_cook_time_ += fixed_delta;
            if (grenade_cook_time_ >= world::grenade_max_cook_seconds)
                throw_grenade();
        }

        for (Grenade &grenade : grenades_)
        {
            if (grenade.attached_enemy.has_value())
                if (const auto enemy_index = enemy_horde_.find_enemy(*grenade.attached_enemy);
                    enemy_index.has_value())
                {
                    const world::Vec3 enemy_position =
                        enemy_horde_.enemies()[*enemy_index].position;
                    grenade.position = {enemy_position.x + grenade.attached_offset.x,
                                        enemy_position.y + grenade.attached_offset.y,
                                        enemy_position.z + grenade.attached_offset.z};
                }
            grenade.time_left -= fixed_delta;
            if (!grenade.stopped)
            {
                grenade.velocity.y -= 9.8F * fixed_delta;
                world::Vec3 segment_start = grenade.position;
                world::Vec3 segment_end{grenade.position.x + grenade.velocity.x * fixed_delta,
                                        grenade.position.y + grenade.velocity.y * fixed_delta,
                                        grenade.position.z + grenade.velocity.z * fixed_delta};
                int ignored_enemy = -1;
                // GrenadeProjectile.MoveToNewPosition resolves as many as ten
                // contacts in one update, reflecting both velocity and the
                // unconsumed part of this frame's movement at every contact.
                for (int collision_budget = world::grenade_collision_budget(grenade.grenade_type);
                     collision_budget > 0 && !grenade.stopped; --collision_budget)
                {
                    const world::Vec3 movement{segment_end.x - segment_start.x,
                                               segment_end.y - segment_start.y,
                                               segment_end.z - segment_start.z};
                    const float distance =
                        std::sqrt(movement.x * movement.x + movement.y * movement.y +
                                  movement.z * movement.z);
                    if (distance <= 0.0001F)
                    {
                        grenade.position = segment_end;
                        break;
                    }
                    const world::Vec3 direction{movement.x / distance, movement.y / distance,
                                                movement.z / distance};
                    const auto terrain_hit = world::grenade_terrain_sweep(
                        world_preview_->region(), segment_start, segment_end);
                    const float enemy_limit =
                        terrain_hit.has_value() ? terrain_hit->time * distance : distance;
                    const world::EnemyHorde::RayHit enemy_hit = enemy_horde_.trace_along_ray(
                        segment_start, direction, enemy_limit, ignored_enemy);
                    const bool hit_enemy = static_cast<bool>(enemy_hit);
                    if (!hit_enemy && !terrain_hit.has_value())
                    {
                        grenade.position = segment_end;
                        break;
                    }

                    world::Vec3 intersection{};
                    world::Vec3 normal{};
                    float restitution = 0.1F;
                    if (hit_enemy)
                    {
                        intersection = {segment_start.x + direction.x * enemy_hit.distance,
                                        segment_start.y + direction.y * enemy_hit.distance,
                                        segment_start.z + direction.z * enemy_hit.distance};
                        normal = enemy_hit.normal;
                    }
                    else
                    {
                        intersection = terrain_hit->position;
                        normal = terrain_hit->normal;
                        restitution = world::grenade_bounce_restitution(terrain_hit->block);
                        const float preimpact_speed_squared =
                            grenade.velocity.x * grenade.velocity.x +
                            grenade.velocity.y * grenade.velocity.y +
                            grenade.velocity.z * grenade.velocity.z;
                        if (preimpact_speed_squared > 0.01F)
                            play_positional("BulletHitDirt", intersection);
                    }

                    grenade.position = intersection;
                    if (collision_budget == 1)
                    {
                        grenade.stopped = true;
                        grenade.velocity = {};
                        if (world::grenade_remains_stuck(grenade.grenade_type) && hit_enemy &&
                            enemy_hit.index >= 0 &&
                            static_cast<std::size_t>(enemy_hit.index) <
                                enemy_horde_.enemies().size())
                        {
                            const world::Enemy &enemy =
                                enemy_horde_.enemies()[static_cast<std::size_t>(enemy_hit.index)];
                            grenade.attached_enemy =
                                world::EnemyIdentity{enemy.target_gid, enemy.id};
                            grenade.attached_offset = {intersection.x - enemy.position.x,
                                                       intersection.y - enemy.position.y,
                                                       intersection.z - enemy.position.z};
                        }
                        break;
                    }
                    grenade.velocity =
                        world::grenade_reflect_vector(grenade.velocity, normal, restitution);
                    const float speed_squared = grenade.velocity.x * grenade.velocity.x +
                                                grenade.velocity.y * grenade.velocity.y +
                                                grenade.velocity.z * grenade.velocity.z;
                    if (normal.y >= 0.75F && speed_squared < 1.0F)
                    {
                        grenade.stopped = true;
                        grenade.velocity = {};
                        break;
                    }
                    const world::Vec3 remaining{segment_end.x - intersection.x,
                                                segment_end.y - intersection.y,
                                                segment_end.z - intersection.z};
                    const world::Vec3 reflected =
                        world::grenade_reflect_vector(remaining, normal, restitution);
                    segment_start = intersection;
                    segment_end = {intersection.x + reflected.x, intersection.y + reflected.y,
                                   intersection.z + reflected.z};
                    ignored_enemy = hit_enemy ? enemy_hit.index : -1;
                }
                grenade.spin += std::sqrt(grenade.velocity.x * grenade.velocity.x +
                                          grenade.velocity.y * grenade.velocity.y +
                                          grenade.velocity.z * grenade.velocity.z) *
                                10.0F * fixed_delta;
            }
            else
            {
                const int x = static_cast<int>(std::floor(grenade.position.x));
                const int y = static_cast<int>(std::floor(grenade.position.y - 0.5F));
                const int z = static_cast<int>(std::floor(grenade.position.z));
                if (!world::grenade_remains_stuck(grenade.grenade_type) &&
                    (!world_preview_->region().contains(x, y, z) ||
                     !world::blocks_player(world_preview_->region().block(x, y, z))))
                    grenade.stopped = false;
            }
            if (grenade.time_left < 0.0F)
            {
                if (grenade.locally_authoritative)
                    detonate_explosive(grenade.position, world::ExplosiveKind::he_grenade,
                                       grenade.stopped);
                else
                    remember_network_projectile(NetworkProjectileKind::grenade, grenade.owner_gid,
                                                grenade.grenade_type, grenade.position);
                grenade.dead = true;
            }
        }
        std::erase_if(grenades_, [](const Grenade &grenade) { return grenade.dead; });
    }

    [[nodiscard]] std::vector<ProjectileVisual> build_projectile_visuals() const
    {
        std::vector<ProjectileVisual> visuals;
        visuals.reserve(tracers_.size() + arrows_.size() + fireballs_.size() + grenades_.size());
        for (const Tracer &tracer : tracers_)
        {
            if (tracer.rocket)
                continue;
            ProjectileVisual visual;
            visual.head = tracer.head;
            if (tracer.laser)
            {
                const float speed =
                    std::max(0.001F, std::sqrt(tracer.head_velocity.x * tracer.head_velocity.x +
                                               tracer.head_velocity.y * tracer.head_velocity.y +
                                               tracer.head_velocity.z * tracer.head_velocity.z));
                visual.tail = {tracer.head.x - tracer.head_velocity.x / speed * 1.6F,
                               tracer.head.y - tracer.head_velocity.y / speed * 1.6F,
                               tracer.head.z - tracer.head_velocity.z / speed * 1.6F};
            }
            else
                visual.tail = tracer.tail;
            visual.color = tracer.color;
            visual.width = tracer.laser ? 0.075F : 0.025F;
            visuals.push_back(visual);
        }
        for (const Arrow &arrow : arrows_)
        {
            // Tracer.Init(target, Color.Pink) renders the authored delayed-tail
            // streak rather than a fixed-length wooden line.
            ProjectileVisual visual;
            visual.head = arrow.head;
            visual.tail = arrow.tail;
            visual.color = {1.0F, 0.7529412F, 0.7960784F};
            visual.width = 0.025F;
            visuals.push_back(visual);
        }
        for (const Grenade &grenade : grenades_)
        {
            ProjectileVisual visual;
            visual.head = grenade.position;
            const float speed = std::sqrt(grenade.velocity.x * grenade.velocity.x +
                                          grenade.velocity.y * grenade.velocity.y +
                                          grenade.velocity.z * grenade.velocity.z);
            if (speed > 0.001F)
                visual.tail = {grenade.position.x - grenade.velocity.x / speed * 0.18F,
                               grenade.position.y - grenade.velocity.y / speed * 0.18F,
                               grenade.position.z - grenade.velocity.z / speed * 0.18F};
            else
                visual.tail = {grenade.position.x, grenade.position.y - 0.08F, grenade.position.z};
            visual.color = {0.24F, 0.31F, 0.16F};
            visual.width = 0.1F;
            visuals.push_back(visual);
        }
        for (const Fireball &fireball : fireballs_)
        {
            // The dragon's fireball reads as a fat glowing streak plus its
            // particle trail — the "big fireball" presentation.
            ProjectileVisual visual;
            visual.head = fireball.position;
            const float speed =
                std::max(0.001F, std::sqrt(fireball.velocity.x * fireball.velocity.x +
                                           fireball.velocity.y * fireball.velocity.y +
                                           fireball.velocity.z * fireball.velocity.z));
            visual.tail = {fireball.position.x - fireball.velocity.x / speed * 2.4F,
                           fireball.position.y - fireball.velocity.y / speed * 2.4F,
                           fireball.position.z - fireball.velocity.z / speed * 2.4F};
            visual.color = fireball.kind == world::DragonKind::ice
                               ? std::array<float, 3>{0.55F, 0.85F, 1.0F}
                               : std::array<float, 3>{1.0F, 0.55F, 0.12F};
            visual.width = 0.55F;
            visuals.push_back(visual);
        }
        return visuals;
    }

    [[nodiscard]] std::vector<RocketVisual> build_rocket_visuals() const
    {
        std::vector<RocketVisual> visuals;
        for (const Tracer &tracer : tracers_)
        {
            if (!tracer.rocket)
                continue;
            visuals.push_back(
                {tracer.head, tracer.rocket_direction, tracer.rocket_spin, tracer.id});
        }
        return visuals;
    }

    [[nodiscard]] std::vector<ExplosiveFuseVisual> build_fuse_visuals() const
    {
        std::vector<ExplosiveFuseVisual> visuals;
        visuals.reserve(armed_explosives_.size());
        for (const ArmedExplosive &armed : armed_explosives_)
        {
            const float period = armed.age > 3.0F ? 0.125F : 0.25F;
            const bool flash = static_cast<int>(std::floor(armed.age / period)) % 2 == 0;
            const int id = (armed.x * 73856093) ^ (armed.y * 19349663) ^ (armed.z * 83492791);
            visuals.push_back({armed.x, armed.y, armed.z, id, flash});
        }
        return visuals;
    }

    // The EnemyManager port: night spawns on the surface, wave-driven cave spawns,
    // the five-minute Felguard timer and aroused crash-site aliens.
    void publish_local_enemy_target(std::string_view message_name, world::EnemyIdentity identity)
    {
        if (!online_gameplay_active())
        {
            if (message_name == "SpeedUpEnemyMessage")
                static_cast<void>(enemy_horde_.speed_up_enemy(identity));
            else
                static_cast<void>(enemy_horde_.give_up_enemy(identity));
            return;
        }
        net::PayloadError payload_error{};
        const auto payload =
            net::encode_enemy_target_payload({static_cast<std::int32_t>(identity.enemy_id),
                                              static_cast<std::int32_t>(identity.target_gid)},
                                             payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast(message_name, *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            std::cerr << "network.local.enemy-target.failure message=" << message_name
                      << " reason=" << error << '\n';
        if (message_name == "SpeedUpEnemyMessage")
            static_cast<void>(enemy_horde_.speed_up_enemy(identity));
        else
            static_cast<void>(enemy_horde_.give_up_enemy(identity));
    }

    void update_enemies(float fixed_delta)
    {
        world_game_time_ += fixed_delta;
        surface_spawn_timer_ += fixed_delta;
        cave_spawn_timer_ += fixed_delta;
        felguard_timer_ += fixed_delta;
        alien_spawn_timer_ += fixed_delta;
        alien_aroused_time_ = std::max(0.0F, alien_aroused_time_ - fixed_delta);
        const world::PlayerState &player = player_.state();
        // EnemyManager.OnUpdate gates: NOENEMIES kills the whole subsystem,
        // Dragon Endurance switches off the ambient overworld (surface
        // zombies, fest, packs, ambient dragons) while caves/aliens continue.
        const bool enemies_enabled = difficulty_ != GameDifficulty::no_enemies;
        // NOENEMIES doesn't just stop spawns — anything already alive
        // (enemies, the dragon, its fireballs) is removed, like the original
        // EnemyManager returning early and dropping its lists.
        if (!enemies_enabled)
        {
            if (!enemy_horde_.enemies().empty())
                enemy_horde_.enemies().clear();
            if (dragon_.active())
                dragon_.deactivate();
            queued_dragon_fireball_ids_.clear();
            fireballs_.clear();
        }
        const bool ambient_world = game_mode_ != GameMode::dragon_endurance;
        const bool hard_branch = hard_spawn_branch(game_mode_, difficulty_);
        // CalculatePlayerDistance: geometric distance from the start point;
        // ONLY Endurance adds the forced-march day bonus (120 blocks per day).
        // Both InGameHUD and EnemyManager measure from global (0, 0), not from
        // the exact cell where the player happened to spawn.
        const float geometric_distance = std::hypot(player.position.x, player.position.z);
        current_distance_ = static_cast<int>(geometric_distance);
        max_distance_ = std::max(max_distance_, current_distance_);
        // The original's distance-triggered songs: Song1..Song6 fire once each
        // at 200/900/1600/2300/3000/3400 blocks from the start point — the
        // "entering the biome" music, since those ARE the band boundaries.
        // DistanceTrigger's teleport guard: a >10-block jump in one frame
        // consumes crossed thresholds silently instead of stacking songs.
        static constexpr std::array<float, 6> song_distances{200.0F,  900.0F,  1600.0F,
                                                             2300.0F, 3000.0F, 3400.0F};
        const bool song_teleport_jump = last_song_distance_ >= 0.0F &&
                                        std::abs(geometric_distance - last_song_distance_) > 10.0F;
        last_song_distance_ = geometric_distance;
        while (next_song_index_ < static_cast<int>(song_distances.size()) &&
               geometric_distance > song_distances[static_cast<std::size_t>(next_song_index_)])
        {
            if (!song_teleport_jump)
            {
                sound_bank_->play_song(std::format("Song{}", next_song_index_ + 1));
                ++next_song_index_;
                break;
            }
            ++next_song_index_;
        }
        // The single continuous world changes terrain by distance; announce
        // each new band the way the original's music shifts marked them.
        const int biome_band = geometric_distance < 300.0F    ? 0
                               : geometric_distance < 1000.0F ? 1
                               : geometric_distance < 1700.0F ? 2
                               : geometric_distance < 2400.0F ? 3
                               : geometric_distance < 3000.0F ? 4
                                                              : 5;
        if (biome_band != previous_biome_band_)
        {
            static constexpr std::array<const char *, 6> band_names{"THE FRONTIER", "THE LAGOONS",
                                                                    "THE DESERT",   "THE MOUNTAINS",
                                                                    "THE ARCTIC",   "THE DESCENT"};
            if (previous_biome_band_ >= 0)
                toast(std::format("ENTERING {}", band_names[static_cast<std::size_t>(biome_band)]),
                      std::format("{} BLOCKS FROM THE START", current_distance_));
            previous_biome_band_ = biome_band;
        }
        const float distance =
            geometric_distance + (game_mode_ == GameMode::endurance
                                      ? std::max(0.0F, (day_night_.day() - 0.41F) * 120.0F)
                                      : 0.0F);
        // GameScreen PercentMidnight: a triangle over the day peaking just after
        // midnight and bottoming out near 1 PM — daytime keeps a small trickle
        // of weak spawns, night ramps hard.
        float percent_midnight = std::fmod(day_night_.time_of_day() + 1.46F, 1.0F) * 2.0F;
        if (percent_midnight > 1.0F)
            percent_midnight = 2.0F - percent_midnight;
        // ZombieFest: deep night (midnight > 0.9) sustained for three seconds
        // pins the spawn factor to 1 — the original's night storm.
        if (percent_midnight > 0.9F && enemies_enabled && ambient_world)
        {
            if (!zombie_fest_)
            {
                if (fest_countdown_ < 0.0F)
                    fest_countdown_ = 3.0F;
                else
                {
                    fest_countdown_ -= fixed_delta;
                    if (fest_countdown_ < 0.0F)
                    {
                        fest_countdown_ = -1.0F;
                        zombie_fest_ = true;
                        toast("THE DEAD ARE SWARMING", "Survive until dawn.");
                    }
                }
            }
        }
        else
        {
            zombie_fest_ = false;
            fest_countdown_ = -1.0F;
        }
        const float midnight =
            zombie_fest_ && player.position.y >= -40.0F
                ? 1.0F
                : world::enemy_midnight_factor(percent_midnight, distance, player.position.y);
        const bool felguard_ready = felguard_timer_ >= 300.0F;
        auto &rng = enemy_horde_.rng();

        int surface_count{};
        int cave_count{};
        for (const world::Enemy &enemy : enemy_horde_.enemies())
        {
            if (world::enemy_uses_cave_spawn_budget(world::enemy_stats(enemy.type).habitat))
                ++cave_count;
            else
                ++surface_count;
        }

        // SpawnAbovegroundEnemy.
        if (enemies_enabled && ambient_world && midnight > 0.0001F && surface_count < 17)
        {
            const float threshold =
                std::lerp(60.0F, world::minimum_enemy_spawn_seconds(distance, hard_branch),
                          std::pow(midnight, 0.25F)) *
                (1.0F + rng.next01() * 0.5F);
            if (surface_spawn_timer_ > threshold)
            {
                const int type = world::pick_aboveground_enemy(midnight, distance, felguard_ready,
                                                               felguard_spawned_, rng);
                const int radius = world::enemy_stats(type).spawn_radius;
                world::Vec3 spot = player.position;
                spot.x += player.velocity.x * 5.0F +
                          static_cast<float>(rng.next_int(-radius, radius + 1));
                spot.z += player.velocity.z * 5.0F +
                          static_cast<float>(rng.next_int(-radius, radius + 1));
                if (try_spawn_enemy_on_ground(type, spot, midnight))
                {
                    surface_spawn_timer_ = 0.0F;
                    if (type == world::enemy_felguard)
                    {
                        felguard_timer_ = 0.0F;
                        felguard_spawned_ = true;
                    }
                }
            }
        }
        else if (midnight <= 0.0001F)
        {
            surface_spawn_timer_ = 0.0F;
        }

        // The "fresh ground" pack spawner (Normal/Hardcore/Endurance): crossing
        // each new ~40-block distance band queues 2-4 zombies that arrive over
        // the next seconds — the original's daytime ambush.
        if (enemies_enabled && fresh_ground_packs(game_mode_, difficulty_))
        {
            if (geometric_distance > cleared_distance_)
            {
                time_to_first_contact_ = -1.0F;
                if (player.position.y > 0.0F)
                {
                    if (distance_enemies_left_ == 0)
                        next_distance_enemy_timer_ = rng.next01() * 2.0F;
                    distance_enemies_left_ += rng.next_int(2, 5);
                }
                const float band = std::floor(1.5F + geometric_distance / 40.0F) * 40.0F;
                cleared_distance_ = band - 10.0F + rng.next01() * 20.0F;
            }
            if (time_to_first_contact_ > 0.0F)
            {
                time_to_first_contact_ -= fixed_delta;
                if (time_to_first_contact_ < 0.0F)
                {
                    next_distance_enemy_timer_ = rng.next01() * 2.0F;
                    distance_enemies_left_ += rng.next_int(2, 5);
                }
            }
            if (distance_enemies_left_ > 0 && surface_count < 17)
            {
                next_distance_enemy_timer_ -= fixed_delta;
                if (next_distance_enemy_timer_ <= 0.0F)
                {
                    // SpawnRandomZombies: distance-tiered zombies (midnight 1
                    // forces the zombie table past the archer roll).
                    const int type =
                        world::pick_aboveground_enemy(1.0F, distance, false, false, rng);
                    const int radius = world::enemy_stats(type).spawn_radius;
                    world::Vec3 spot = player.position;
                    spot.x += static_cast<float>(rng.next_int(-radius, radius + 1));
                    spot.z += static_cast<float>(rng.next_int(-radius, radius + 1));
                    if (try_spawn_enemy_on_ground(type, spot, std::max(midnight, 0.3F)))
                        --distance_enemies_left_;
                    if (distance_enemies_left_ > 0)
                        next_distance_enemy_timer_ += 2.0F + rng.next01() * 3.0F;
                }
            }
        }

        // SpawnBelowgroundEnemy: depth factor pulsing on the original two-minute wave.
        const float depth = std::clamp(-(player.position.y - 20.0F), 0.0F, 50.0F);
        if (enemies_enabled && depth > 0.0F && cave_count < 12)
        {
            float wave = std::sin(std::fmod(world_game_time_ / 60.0F, 2.0F) * 3.1415927F);
            wave = wave <= 0.0F ? 0.0F : std::sqrt(wave);
            const float cave_factor = depth / 50.0F * wave;
            if (cave_factor > 0.0001F)
            {
                const float cave_distance = player.position.y < -40.0F ? 3500.0F : distance;
                const float threshold =
                    std::lerp(60.0F, world::minimum_enemy_spawn_seconds(cave_distance, hard_branch),
                              cave_factor) *
                    (1.0F + rng.next01() * 0.5F);
                if (cave_spawn_timer_ > threshold)
                {
                    const int type = world::pick_belowground_enemy(
                        depth, cave_distance, felguard_ready, felguard_spawned_, rng);
                    const int radius = world::enemy_stats(type).spawn_radius;
                    world::Vec3 spot = player.position;
                    spot.y += 1.0F;
                    int offset = rng.next_int(-radius, radius);
                    spot.x += static_cast<float>(offset > 0 ? offset + 5 : offset - 5);
                    offset = rng.next_int(-radius, radius);
                    spot.z += static_cast<float>(offset > 0 ? offset + 5 : offset - 5);
                    const bool open_hell = player.position.y < -40.0F ||
                                           world_preview_->biome() == world::BiomeKind::descent;
                    if (try_spawn_enemy_at_ceiling(type, spot) ||
                        (open_hell && try_spawn_enemy_on_local_ground(type, spot, 1.0F)))
                    {
                        cave_spawn_timer_ = 0.0F;
                        if (type == world::enemy_felguard)
                        {
                            felguard_timer_ = 0.0F;
                            felguard_spawned_ = true;
                        }
                    }
                }
            }
        }

        // SearchForSpaceRock: every 3 s scan a 30-block box around the player
        // for crash-site rock. Aliens are purely crash-site-local: while a rock
        // is near, one spawns every 10-20 s (3-7 s when aroused), capped at 10.
        space_rock_scan_timer_ -= fixed_delta;
        if (space_rock_scan_timer_ <= 0.0F)
        {
            space_rock_scan_timer_ = 3.0F;
            space_rock_nearby_ = false;
            closest_space_rock_ = 1000.0F;
            const auto &region = world_preview_->region();
            const int center_x = static_cast<int>(std::floor(player.position.x));
            const int center_z = static_cast<int>(std::floor(player.position.z));
            constexpr int scan_radius = 30;
            for (int z = center_z - scan_radius; z <= center_z + scan_radius; z += 2)
                for (int x = center_x - scan_radius; x <= center_x + scan_radius; x += 2)
                {
                    if (!region.contains(x, world::world_min_y, z))
                        continue;
                    for (int y = world::world_min_y; y < world::world_min_y + world::chunk_height;
                         ++y)
                    {
                        const world::BlockType block = region.block(x, y, z);
                        if (block == world::BlockType::space_rock ||
                            block == world::BlockType::space_rock_inventory)
                        {
                            const float dx = static_cast<float>(x) - player.position.x;
                            const float dz = static_cast<float>(z) - player.position.z;
                            closest_space_rock_ = std::min(closest_space_rock_, std::hypot(dx, dz));
                            space_rock_nearby_ = true;
                            break;
                        }
                    }
                }
        }
        if (enemies_enabled && space_rock_nearby_ && alien_spawn_timer_ > next_alien_delay_)
        {
            int alien_count{};
            for (const world::Enemy &enemy : enemy_horde_.enemies())
                if (world::enemy_stats(enemy.type).enemy_class == world::EnemyClass::alien)
                    ++alien_count;
            if (alien_count < 10)
            {
                const bool aroused = alien_aroused_time_ > 0.0F;
                world::Vec3 spot = player.position;
                const int spread = aroused ? 15 : 30;
                spot.x += static_cast<float>(rng.next_int(-spread, spread + 1));
                spot.z += static_cast<float>(rng.next_int(-spread, spread + 1));
                // SpawnAlien: the alien must stand on crash-site rock unless
                // the rock is over 20 blocks out.
                const bool spawned =
                    closest_space_rock_ > 20.0F
                        ? try_spawn_enemy_on_ground(world::enemy_alien, spot, midnight)
                        : try_spawn_alien_on_rock(spot);
                if (spawned)
                {
                    alien_spawn_timer_ = 0.0F;
                    next_alien_delay_ =
                        aroused ? 3.0F + rng.next01() * 4.0F : 10.0F + rng.next01() * 10.0F;
                }
            }
        }

        // Tick the horde and apply its events to the app systems.
        if (SDL_getenv("OPENMINERZ_ENEMY_DEBUG") != nullptr && !enemy_horde_.enemies().empty())
        {
            const world::Enemy &first = enemy_horde_.enemies()[0];
            SDL_Log("enemy0 type=%d state=%d pos=(%.1f %.1f %.1f) player=(%.1f %.1f %.1f)",
                    first.type, static_cast<int>(first.state), first.position.x, first.position.y,
                    first.position.z, player.position.x, player.position.y, player.position.z);
        }
        world_preview_->set_enemies(&enemy_horde_.enemies());
        const bool online = online_gameplay_active();
        const std::uint8_t local_gid = online ? online_local_player_gid() : 0U;
        const auto target_lookup = [this](std::uint8_t gid) -> std::optional<world::PlayerState> {
            const auto found = online_remote_players_.find(gid);
            if (found == online_remote_players_.end() || !found->second.has_update ||
                elapsed_ - found->second.last_update > 10.0F)
                return std::nullopt;
            world::PlayerState target;
            target.position = found->second.state.position;
            target.velocity = found->second.state.velocity;
            target.yaw = found->second.state.yaw;
            target.health = found->second.state.dead ? 0.0F : 100.0F;
            target.grounded = std::abs(target.velocity.y) < 0.01F;
            return target;
        };
        const world::EnemyEvents events = enemy_horde_.update(
            world_preview_->region(), player, local_gid, target_lookup, fixed_delta);
        for (const world::EnemyIdentity identity : events.speed_up_requests)
            publish_local_enemy_target("SpeedUpEnemyMessage", identity);
        for (const world::EnemyIdentity identity : events.give_up_requests)
        {
            publish_local_enemy_target("EnemyGiveUpMessage", identity);
            notify_local_spawner_enemy_removed(identity);
        }
        for (DamageIndicator &indicator : damage_indicators_)
            indicator.age += fixed_delta;
        std::erase_if(damage_indicators_,
                      [](const DamageIndicator &indicator) { return indicator.age > 3.0F; });
        if (events.player_damage > 0.0F)
        {
            caption("[ STRUCK BY ENEMY ]", 0.9F);
            for (std::size_t extra = 1; extra < events.player_damage_sources.size(); ++extra)
                damage_indicators_.push_back({events.player_damage_sources[extra], 0.0F});
            apply_player_damage(events.player_damage * 100.0F,
                                events.player_damage_sources.empty()
                                    ? player.position
                                    : events.player_damage_sources.front());
        }
        else if (world_game_time_ - last_player_damage_time_ > 3.0F &&
                 player_life_ == PlayerLife::alive)
        {
            // InGameHUD.HealthRecoverRate: 0.75 of max health per second once
            // the three-second recover timer has expired. The dead don't heal.
            player_.state().health = std::min(100.0F, player_.state().health + 75.0F * fixed_delta);
        }
        // InGameHUD.timeToShowRespawnText: the prompt (and respawn input)
        // arrives three seconds after death, like the original.
        if (player_life_ == PlayerLife::dying)
        {
            death_timer_ += fixed_delta;
            if (death_timer_ >= 3.0F)
                player_life_ = PlayerLife::dead;
        }
        else if (player_life_ == PlayerLife::dead)
        {
            death_timer_ += fixed_delta;
        }
        if (player_life_ != PlayerLife::alive)
        {
            distance_enemies_left_ = 0;
        }
        process_enemy_block_breaks(events);
        // Every dig swing that touched solid blocks plays the loud, scary
        // "ZombieDig" scrape from the zombie's own position — distinct from
        // the player's own mining sound.
        for (const world::Vec3 &swing : events.dig_swings)
            play_positional("ZombieDig", swing);
        for (std::size_t drop = 0; drop < events.drops.size(); ++drop)
        {
            world_preview_->spawn_dropped_stack_at(
                InventoryStack{static_cast<InventoryItemId>(events.drops[drop].item_id),
                               events.drops[drop].item_count},
                events.drop_positions[drop], 1.5F);
        }
        for (std::size_t arrow = 0; arrow < events.arrow_origins.size(); ++arrow)
        {
            const world::Vec3 velocity = events.arrow_velocities[arrow];
            // Tracer.Init(pos, vel, target, color) has an inverted-looking
            // authored branch: every real arrow (non-zero velocity) starts
            // half a block upward, not half a block along its direction.
            const world::Vec3 start{events.arrow_origins[arrow].x,
                                    events.arrow_origins[arrow].y + 0.5F,
                                    events.arrow_origins[arrow].z};
            const std::uint8_t target_gid = arrow < events.arrow_target_gids.size()
                                                ? events.arrow_target_gids[arrow]
                                                : local_gid;
            arrows_.push_back({start, start, velocity, velocity, target_gid});
        }

        // Skeleton arrows are ordinary TracerManager tracers with a target: two
        // seconds of ballistic life and a delayed tail swept against target/terrain.
        for (Arrow &arrow : arrows_)
        {
            arrow.time_left -= fixed_delta;
            if (arrow.time_left < 0.0F)
                arrow.dead = true;
            arrow.head.x += arrow.head_velocity.x * fixed_delta;
            arrow.head.y += arrow.head_velocity.y * fixed_delta;
            arrow.head.z += arrow.head_velocity.z * fixed_delta;
            arrow.head_velocity.y -= 10.0F * fixed_delta;
            if (arrow.time_left < arrow.tail_time - 0.2F)
            {
                arrow.tail.x += arrow.tail_velocity.x * fixed_delta;
                arrow.tail.y += arrow.tail_velocity.y * fixed_delta;
                arrow.tail.z += arrow.tail_velocity.z * fixed_delta;
                arrow.tail_velocity.y -= 10.0F * fixed_delta;
            }
            const world::ArrowSweepHit impact = world::arrow_sweep(
                world_preview_->region(), player,
                arrow.target_gid == local_gid && player_life_ == PlayerLife::alive, arrow.tail,
                arrow.head);
            if (impact.kind == world::ArrowSweepKind::terrain)
            {
                arrow.head = impact.position;
                world_preview_->spawn_impact_smoke(impact.position, {1.0F, 1.0F, 1.0F});
                play_positional("BulletHitDirt", impact.position);
                arrow.dead = true;
            }
            else if (impact.kind == world::ArrowSweepKind::player)
            {
                arrow.head = impact.position;
                caption("[ ARROW HIT ]", 0.9F);
                world_preview_->spawn_impact_smoke(impact.position, {1.0F, 1.0F, 1.0F});
                play_positional("BulletHitHuman", impact.position);
                apply_player_damage(35.0F, impact.position);
                arrow.dead = true;
            }
            else if (!arrow.whizzed)
            {
                const float dx = arrow.head.x - player.position.x;
                const float dy = arrow.head.y - player.position.y;
                const float dz = arrow.head.z - player.position.z;
                if (dx * dx + dy * dy + dz * dz < 25.0F)
                {
                    arrow.whizzed = true;
                    play_positional("arrow", arrow.head);
                }
            }
            if (!arrow.dead)
            {
                const auto &region = world_preview_->region();
                arrow.dead = arrow.head.x < static_cast<float>(region.minimum_x()) ||
                             arrow.head.x >= static_cast<float>(region.maximum_x() + 1) ||
                             arrow.head.z < static_cast<float>(region.minimum_z()) ||
                             arrow.head.z >= static_cast<float>(region.maximum_z() + 1) ||
                             arrow.head.y < static_cast<float>(world::world_min_y);
            }
        }
        std::erase_if(arrows_, [](const Arrow &arrow) { return arrow.dead; });
        update_tracers(fixed_delta);
        update_placed_explosives(fixed_delta);
        update_grenades(fixed_delta);
        update_dragon(fixed_delta);
    }

    [[nodiscard]] bool local_owns_dragon_brain() const noexcept
    {
        return !online_gameplay_active() ||
               (dragon_owner_gid_.has_value() && *dragon_owner_gid_ == online_local_player_gid());
    }

    [[nodiscard]] static std::uint8_t dragon_animation(const world::Dragon &dragon) noexcept
    {
        if (dragon.clip == "Idle")
            return 3U;
        if (dragon.clip == "attack")
            return 2U;
        if (dragon.clip == "fly_forward")
            return 1U;
        return 0U;
    }

    [[nodiscard]] net::DragonWaypointPayload current_dragon_waypoint() noexcept
    {
        const world::Dragon &dragon = dragon_.dragon();
        const world::DragonBasis basis =
            world::dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
        net::DragonWaypointPayload waypoint;
        waypoint.position = {dragon.position.x, dragon.position.y, dragon.position.z};
        waypoint.velocity = {basis.forward.x * dragon.velocity, basis.forward.y * dragon.velocity,
                             basis.forward.z * dragon.velocity};
        waypoint.host_time = dragon_network_time_;
        waypoint.target_roll = dragon.roll;
        waypoint.animation = dragon_animation(dragon);
        waypoint.sound = dragon_network_sound_;
        dragon_network_sound_ = 0U;
        return waypoint;
    }

    [[nodiscard]] bool valid_network_dragon_waypoint(
        const net::DragonWaypointPayload &waypoint) const noexcept
    {
        const float speed_squared = waypoint.velocity.x * waypoint.velocity.x +
                                    waypoint.velocity.y * waypoint.velocity.y +
                                    waypoint.velocity.z * waypoint.velocity.z;
        return waypoint.position.x >= -1'000'000.0F && waypoint.position.x <= 1'000'000.0F &&
               waypoint.position.y >= -10'000.0F && waypoint.position.y <= 10'000.0F &&
               waypoint.position.z >= -1'000'000.0F && waypoint.position.z <= 1'000'000.0F &&
               speed_squared <= 250'000.0F && waypoint.host_time >= 0.0F &&
               waypoint.host_time <= 10'000'000.0F && std::abs(waypoint.target_roll) <= 6.283186F &&
               waypoint.animation <= 3U && waypoint.sound <= 3U;
    }

    [[nodiscard]] bool apply_network_dragon_waypoint(std::uint8_t sender_gid,
                                                     const net::DragonWaypointPayload &waypoint)
    {
        if (!dragon_.active() || !dragon_owner_gid_.has_value() ||
            *dragon_owner_gid_ != sender_gid || !valid_network_dragon_waypoint(waypoint))
            return false;
        // UpdateDragon switches to unreliable delivery after the first two
        // warm-up packets. DragonClientEntity itself decides whether a
        // reordered sample is still far enough ahead of the interpolation
        // bracket to insert; do not impose a stricter monotonic filter here.
        dragon_last_host_time_ = std::max(dragon_last_host_time_, waypoint.host_time);
        dragon_.apply_network_waypoint(
            {waypoint.position.x, waypoint.position.y, waypoint.position.z},
            {waypoint.velocity.x, waypoint.velocity.y, waypoint.velocity.z}, waypoint.host_time,
            waypoint.target_roll, waypoint.animation, waypoint.sound);
        return true;
    }

    [[nodiscard]] bool apply_network_dragon_spawn(const net::SpawnDragonPayload &message)
    {
        if (message.dragon_type >= static_cast<std::uint8_t>(world::DragonKind::count) ||
            (!online_gameplay_active() && message.spawner_gid != 0U) || message.health < -1.0F ||
            message.health > 10'000'000.0F)
            return false;
        const auto kind = static_cast<world::DragonKind>(message.dragon_type);
        dragon_owner_gid_ = message.spawner_gid;
        dragon_for_biome_ = message.for_biome;
        dragon_pending_ = false;
        dragon_network_time_ = 0.0F;
        dragon_update_timer_ = 0.0F;
        dragon_last_host_time_ = -1.0F;
        dragon_updates_sent_ = 0;
        dragon_network_sound_ = 0U;
        dragon_kill_announced_ = false;
        dragon_loot_pending_ = false;
        queued_dragon_fireball_ids_.clear();
        if (message.spawner_gid == online_local_player_gid())
        {
            dragon_.spawn(kind, player_.state().position, enemy_horde_.rng());
            if (message.health >= 0.0F)
                dragon_.set_health(message.health);
        }
        else
            dragon_.install_replica(kind, message.health);
        dragon_cry_timer_ = 5.0F;
        dragon_flap_timer_ = 1.0F;
        return true;
    }

    [[nodiscard]] bool publish_spawn_dragon(world::DragonKind kind, std::uint8_t spawner_gid,
                                            bool for_biome, float health)
    {
        net::PayloadError payload_error{};
        const net::SpawnDragonPayload message{static_cast<std::uint8_t>(kind), spawner_gid,
                                              for_biome, health};
        const auto payload = net::encode_spawn_dragon_payload(message, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("SpawnDragonMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.host.spawn-dragon.failure=" << error << '\n';
            return false;
        }
        return apply_network_dragon_spawn(message);
    }

    [[nodiscard]] bool request_dragon_spawn(world::DragonKind kind, bool for_biome,
                                            float health = -1.0F)
    {
        if (dragon_pending_ || dragon_.active())
            return false;
        if (!online_gameplay_active())
        {
            dragon_.spawn(kind, player_.state().position, enemy_horde_.rng());
            if (health >= 0.0F)
                dragon_.set_health(health);
            dragon_owner_gid_.reset();
            dragon_for_biome_ = for_biome;
            dragon_kill_announced_ = false;
            dragon_loot_pending_ = false;
            dragon_cry_timer_ = 5.0F;
            dragon_flap_timer_ = 1.0F;
            return true;
        }
        dragon_pending_ = true;
        if (online_host_ != nullptr && online_host_->running())
        {
            if (publish_spawn_dragon(kind, 0U, for_biome, health))
                return true;
            dragon_pending_ = false;
            return false;
        }
        net::PayloadError payload_error{};
        const auto payload = net::encode_request_dragon_payload(
            {static_cast<std::uint8_t>(kind), for_biome}, payload_error);
        std::string error;
        if (payload.has_value() &&
            publish_online_broadcast("RequestDragonMessage", *payload,
                                     net::MessageDeliveryPolicy::reliable_ordered, error))
            return true;
        dragon_pending_ = false;
        std::cerr << "network.local.request-dragon.failure=" << error << '\n';
        return false;
    }

    void publish_dragon_update()
    {
        net::PayloadError payload_error{};
        const auto payload =
            net::encode_update_dragon_payload(current_dragon_waypoint(), payload_error);
        const net::MessageDeliveryPolicy delivery =
            dragon_updates_sent_ < 2 ? net::MessageDeliveryPolicy::reliable_ordered
                                     : net::MessageDeliveryPolicy::unreliable;
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("UpdateDragonMessage", *payload, delivery, error))
            std::cerr << "network.local.update-dragon.failure=" << error << '\n';
        ++dragon_updates_sent_;
    }

    void publish_dragon_attack(world::Vec3 target, std::int32_t fireball_index)
    {
        net::DragonAttackPayload message;
        message.waypoint = current_dragon_waypoint();
        message.target = {target.x, target.y, target.z};
        message.animated_attack = false;
        message.fireball_index = fireball_index;
        net::PayloadError payload_error{};
        const auto payload = net::encode_dragon_attack_payload(message, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("DragonAttackMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.dragon-attack.failure=" << error << '\n';
        ++dragon_updates_sent_;
    }

    void publish_existing_dragon(std::uint8_t new_client_gid)
    {
        if (!dragon_.active() || !local_owns_dragon_brain() || !dragon_owner_gid_.has_value())
            return;
        net::PayloadError payload_error{};
        const world::Dragon &dragon = dragon_.dragon();
        const auto payload =
            net::encode_existing_dragon_payload({static_cast<std::uint8_t>(dragon.kind),
                                                 new_client_gid, dragon_for_biome_, dragon.health},
                                                payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("ExistingDragonMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.existing-dragon.failure=" << error << '\n';
    }

    [[nodiscard]] bool apply_network_dragon_attack(std::uint8_t sender_gid,
                                                   const net::DragonAttackPayload &message)
    {
        const std::uint8_t encoded_owner = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(message.fireball_index) >> 23U) & 0xFFU);
        if (encoded_owner != sender_gid || !dragon_.active() || !dragon_owner_gid_.has_value() ||
            *dragon_owner_gid_ != sender_gid || !valid_network_dragon_waypoint(message.waypoint) ||
            queued_dragon_fireball_ids_.contains(message.fireball_index) ||
            std::ranges::any_of(fireballs_, [&](const Fireball &fireball) {
                return fireball.id == message.fireball_index;
            }))
            return false;
        world::Vec3 attack_forward{message.waypoint.velocity.x, message.waypoint.velocity.y,
                                   message.waypoint.velocity.z};
        const float attack_speed =
            std::sqrt(attack_forward.x * attack_forward.x + attack_forward.y * attack_forward.y +
                      attack_forward.z * attack_forward.z);
        if (attack_speed > 0.001F)
        {
            attack_forward.x /= attack_speed;
            attack_forward.y /= attack_speed;
            attack_forward.z /= attack_speed;
        }
        else
            attack_forward =
                world::dragon_attitude_basis(dragon_.dragon().yaw, dragon_.dragon().pitch,
                                             dragon_.dragon().roll)
                    .forward;
        world::Vec3 origin{message.waypoint.position.x, message.waypoint.position.y,
                           message.waypoint.position.z};
        origin.x += attack_forward.x * 4.0F;
        origin.y -= 1.0F;
        origin.z += attack_forward.z * 4.0F;
        world::Vec3 velocity{message.target.x - origin.x, message.target.y - origin.y,
                             message.target.z - origin.z};
        const float length =
            std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
        if (length < 0.001F || length > 2'000'000.0F)
            return false;
        dragon_last_host_time_ = std::max(dragon_last_host_time_, message.waypoint.host_time);
        if (!dragon_.apply_network_attack_waypoint(
                {message.waypoint.position.x, message.waypoint.position.y,
                 message.waypoint.position.z},
                {message.waypoint.velocity.x, message.waypoint.velocity.y,
                 message.waypoint.velocity.z},
                message.waypoint.host_time, message.waypoint.target_roll,
                message.waypoint.animation, message.waypoint.sound,
                {message.target.x, message.target.y, message.target.z}, message.fireball_index,
                message.animated_attack))
            return false;
        queued_dragon_fireball_ids_.insert(message.fireball_index);
        return true;
    }

    void process_network_dragon_replica_events()
    {
        for (const world::DragonReplicaEvent &event : dragon_.take_network_replica_events())
        {
            if (event.kind == world::DragonReplicaEventKind::cry)
            {
                play_positional("DragonScream", event.position);
                continue;
            }
            queued_dragon_fireball_ids_.erase(event.fireball_id);
            if (event.kind == world::DragonReplicaEventKind::cancel_fireball ||
                std::ranges::find(applied_fireball_detonations_, event.fireball_id) !=
                    applied_fireball_detonations_.end() ||
                std::ranges::any_of(fireballs_, [&](const Fireball &fireball) {
                    return fireball.id == event.fireball_id;
                }))
                continue;
            world::Vec3 velocity{event.target.x - event.position.x,
                                 event.target.y - event.position.y,
                                 event.target.z - event.position.z};
            const float length = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
                                           velocity.z * velocity.z);
            if (length < 0.001F || length > 2'000'000.0F)
                continue;
            const world::DragonKind kind = dragon_.dragon().kind;
            const float speed = world::dragon_stats(kind).fireball_velocity;
            velocity.x = velocity.x / length * speed;
            velocity.y = velocity.y / length * speed;
            velocity.z = velocity.z / length * speed;
            const std::uint8_t owner_gid = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(event.fireball_id) >> 23U) & 0xFFU);
            fireballs_.push_back({event.position, velocity, kind, event.fireball_id, false, false,
                                  owner_gid, false});
            sound_bank_->sync_fireball_cue(static_cast<std::uint32_t>(event.fireball_id),
                                           kind == world::DragonKind::ice ? "Iceball" : "Fireball",
                                           event.position, velocity);
        }
    }

    [[nodiscard]] bool valid_dragon_migration(
        const net::MigrateDragonPayload &message) const noexcept
    {
        const net::DragonMigrationPayload &migration = message.migration;
        const float maximum_angle = 6.283186F;
        return dragon_.active() && dragon_owner_gid_.has_value() &&
               migration.dragon_type < static_cast<std::uint8_t>(world::DragonKind::count) &&
               migration.dragon_type == static_cast<std::uint8_t>(dragon_.dragon().kind) &&
               message.target_gid != *dragon_owner_gid_ && migration.next_dragon_time >= 0.0F &&
               migration.next_dragon_time <= 10'000'000.0F && migration.next_update_time >= 0.0F &&
               migration.next_update_time <= 10'000'000.0F &&
               std::abs(migration.roll) <= maximum_angle &&
               std::abs(migration.target_roll) <= maximum_angle &&
               std::abs(migration.pitch) <= maximum_angle &&
               std::abs(migration.target_pitch) <= maximum_angle &&
               std::abs(migration.yaw) <= maximum_angle &&
               std::abs(migration.target_yaw) <= maximum_angle &&
               std::abs(migration.default_heading) <= maximum_angle && migration.velocity >= 0.0F &&
               migration.velocity <= 500.0F && migration.target_velocity >= 0.0F &&
               migration.target_velocity <= 500.0F && std::abs(migration.flap_debt) <= 10'000.0F &&
               migration.animation <= 3U && migration.for_biome == dragon_for_biome_ &&
               migration.position.x >= -1'000'000.0F && migration.position.x <= 1'000'000.0F &&
               migration.position.y >= -10'000.0F && migration.position.y <= 10'000.0F &&
               migration.position.z >= -1'000'000.0F && migration.position.z <= 1'000'000.0F &&
               migration.target.x >= -1'000'000.0F && migration.target.x <= 1'000'000.0F &&
               migration.target.y >= -10'000.0F && migration.target.y <= 10'000.0F &&
               migration.target.z >= -1'000'000.0F && migration.target.z <= 1'000'000.0F;
    }

    void install_dragon_migration(const net::MigrateDragonPayload &message)
    {
        const net::DragonMigrationPayload &migration = message.migration;
        const float health = dragon_.dragon().health;
        const auto kind = static_cast<world::DragonKind>(migration.dragon_type);
        const bool was_local_owner = local_owns_dragon_brain();
        if (message.target_gid == online_local_player_gid() || was_local_owner)
            queued_dragon_fireball_ids_.clear();
        const world::DragonBasis basis =
            world::dragon_attitude_basis(migration.yaw, migration.pitch, migration.roll);
        const world::Vec3 position{migration.position.x, migration.position.y,
                                   migration.position.z};
        const world::Vec3 velocity{basis.forward.x * migration.velocity,
                                   basis.forward.y * migration.velocity,
                                   basis.forward.z * migration.velocity};
        if (message.target_gid == online_local_player_gid())
        {
            dragon_.install_replica(kind, health);
            dragon_.reset_network_replica(position, velocity, migration.next_dragon_time,
                                          migration.target_roll, migration.animation);
            dragon_.promote_network_replica_to_brain();
        }
        else if (was_local_owner)
        {
            dragon_.install_replica(kind, health);
            dragon_.reset_network_replica(position, velocity, migration.next_dragon_time,
                                          migration.target_roll, migration.animation);
        }
        else
            dragon_.apply_network_waypoint(position, velocity, migration.next_dragon_time,
                                           migration.target_roll, migration.animation);
        dragon_owner_gid_ = message.target_gid;
        dragon_for_biome_ = migration.for_biome;
        dragon_network_time_ = migration.next_dragon_time;
        dragon_update_timer_ =
            std::max(0.0F, migration.next_update_time - migration.next_dragon_time);
        dragon_last_host_time_ = migration.next_dragon_time;
        dragon_updates_sent_ = 0;
        next_dragon_fireball_index_ =
            static_cast<std::uint32_t>(std::max(0, migration.next_fireball_index));
        dragon_network_sound_ = 0U;
    }

    [[nodiscard]] bool publish_dragon_migration(std::uint8_t target_gid,
                                                world::Vec3 target_position)
    {
        if (!dragon_.active() || !local_owns_dragon_brain() || !dragon_owner_gid_.has_value() ||
            target_gid == *dragon_owner_gid_)
            return false;
        const world::Dragon &dragon = dragon_.dragon();
        net::MigrateDragonPayload message;
        net::DragonMigrationPayload &migration = message.migration;
        migration.next_dragon_time = dragon_network_time_ + 0.4F;
        migration.roll = dragon.roll;
        migration.target_roll = dragon.roll;
        migration.pitch = dragon.pitch;
        migration.target_pitch = dragon.pitch;
        migration.yaw = dragon.yaw;
        migration.target_yaw = dragon.yaw;
        migration.velocity = dragon.velocity;
        migration.target_velocity = dragon.velocity;
        migration.default_heading = dragon.yaw;
        migration.flap_debt = 0.0F;
        migration.next_fireball_index = static_cast<std::int32_t>(next_dragon_fireball_index_);
        migration.for_biome = dragon_for_biome_;
        migration.next_update_time =
            dragon_network_time_ + std::max(0.0F, dragon_update_timer_) + 0.4F;
        migration.animation = dragon_animation(dragon);
        migration.dragon_type = static_cast<std::uint8_t>(dragon.kind);
        migration.position = {dragon.position.x, dragon.position.y, dragon.position.z};
        migration.target = {target_position.x, target_position.y, target_position.z};
        message.target_gid = target_gid;
        net::PayloadError payload_error{};
        const auto payload = net::encode_migrate_dragon_payload(message, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("MigrateDragonMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.migrate-dragon.failure=" << error << '\n';
            return false;
        }
        install_dragon_migration(message);
        return true;
    }

    [[nodiscard]] bool consider_dragon_migration(float fixed_delta)
    {
        if (!online_gameplay_active() || !local_owns_dragon_brain() || !dragon_.active())
            return false;
        dragon_migration_scan_timer_ -= fixed_delta;
        if (dragon_migration_scan_timer_ > 0.0F)
            return false;
        dragon_migration_scan_timer_ = 0.5F;
        const world::Vec3 local = player_.state().position;
        const world::Dragon &dragon = dragon_.dragon();
        const world::DragonBasis basis =
            world::dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
        const float local_dragon_distance =
            std::hypot(local.x - dragon.position.x, local.z - dragon.position.z);
        std::optional<std::pair<std::uint8_t, world::Vec3>> best;
        float best_distance = local_dragon_distance;
        for (const auto &[gid, remote] : online_remote_players_)
        {
            if (!remote.has_update || remote.state.dead || elapsed_ - remote.last_update > 10.0F)
                continue;
            const world::Vec3 target = remote.state.position;
            if (std::hypot(target.x - local.x, target.z - local.z) <= 150.0F)
                continue;
            world::Vec3 toward{target.x - dragon.position.x, target.y + 1.5F - dragon.position.y,
                               target.z - dragon.position.z};
            const float distance =
                std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
            if (distance <= 0.001F || distance >= 300.0F || distance >= best_distance)
                continue;
            toward.x /= distance;
            toward.y /= distance;
            toward.z /= distance;
            if (toward.x * basis.forward.x + toward.y * basis.forward.y +
                    toward.z * basis.forward.z <=
                0.17F)
                continue;
            if (world::PlayerSimulation::raycast_blocking(world_preview_->region(), dragon.position,
                                                          toward, std::max(0.0F, distance - 0.75F)))
                continue;
            best = std::pair{gid, target};
            best_distance = distance;
        }
        return best.has_value() && publish_dragon_migration(best->first, best->second);
    }

    void publish_remove_dragon()
    {
        if (!online_gameplay_active() || !dragon_owner_gid_.has_value())
            return;
        std::string error;
        if (!publish_online_broadcast("RemoveDragonMessage", {},
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            std::cerr << "network.local.remove-dragon.failure=" << error << '\n';
        dragon_owner_gid_.reset();
        dragon_pending_ = false;
        dragon_loot_pending_ = false;
        queued_dragon_fireball_ids_.clear();
    }

    [[nodiscard]] bool apply_online_dragon_message(std::uint8_t sender_gid,
                                                   net::ProtocolVersion protocol,
                                                   const net::DecodedPacket &decoded,
                                                   bool receiving_as_host)
    {
        static_cast<void>(protocol);
        net::PayloadError payload_error{};
        if (decoded.message->name == "RequestDragonMessage")
        {
            net::RequestDragonPayload request;
            if (!receiving_as_host ||
                !net::decode_request_dragon_payload(decoded.payload, request, payload_error) ||
                request.dragon_type >= static_cast<std::uint8_t>(world::DragonKind::count))
                return false;
            if (!dragon_pending_ && !dragon_.active())
            {
                dragon_pending_ = true;
                if (!publish_spawn_dragon(static_cast<world::DragonKind>(request.dragon_type),
                                          sender_gid, request.for_biome, -1.0F))
                {
                    dragon_pending_ = false;
                    return false;
                }
            }
            return true;
        }
        if (decoded.message->name == "SpawnDragonMessage")
        {
            net::SpawnDragonPayload spawn;
            return !receiving_as_host && sender_gid == 0U &&
                   net::decode_spawn_dragon_payload(decoded.payload, spawn, payload_error) &&
                   apply_network_dragon_spawn(spawn);
        }
        if (decoded.message->name == "ExistingDragonMessage")
        {
            net::ExistingDragonPayload existing;
            if (!net::decode_existing_dragon_payload(decoded.payload, existing, payload_error) ||
                existing.dragon_type >= static_cast<std::uint8_t>(world::DragonKind::count))
                return false;
            if (receiving_as_host)
                return dragon_owner_gid_.has_value() && *dragon_owner_gid_ == sender_gid &&
                       existing.new_client_gid != 0U &&
                       online_remote_players_.contains(existing.new_client_gid);
            if (existing.new_client_gid != online_local_player_gid())
                return true;
            if (dragon_owner_gid_.has_value() && *dragon_owner_gid_ != sender_gid)
                return true; // A stale late-join response from the previous owner.
            dragon_.install_replica(static_cast<world::DragonKind>(existing.dragon_type),
                                    existing.current_health);
            queued_dragon_fireball_ids_.clear();
            dragon_owner_gid_ = sender_gid;
            dragon_for_biome_ = existing.for_biome;
            dragon_pending_ = false;
            dragon_last_host_time_ = -1.0F;
            return true;
        }
        if (decoded.message->name == "MigrateDragonMessage")
        {
            if (dragon_kill_announced_ && !dragon_owner_gid_.has_value())
                return true;
            net::MigrateDragonPayload migration;
            if (!net::decode_migrate_dragon_payload(decoded.payload, migration, payload_error))
                return false;
            if (!dragon_.active() || !dragon_owner_gid_.has_value())
                return !receiving_as_host;
            if (*dragon_owner_gid_ != sender_gid)
                return !receiving_as_host;
            if (!valid_dragon_migration(migration) ||
                (receiving_as_host && migration.target_gid != 0U &&
                 !online_remote_players_.contains(migration.target_gid)))
                return false;
            install_dragon_migration(migration);
            return true;
        }
        if (decoded.message->name == "KillDragonMessage")
        {
            net::KillDragonPayload kill;
            InventoryItemId weapon{};
            if (!net::decode_kill_dragon_payload(decoded.payload, protocol, kill, payload_error))
                return false;
            if (!net::inventory_item_from_wire(kill.weapon_id, protocol, weapon))
                return false;
            if (dragon_kill_announced_)
                return true;
            if (!dragon_.active())
                return !receiving_as_host;
            const world::Vec3 location{kill.location.x, kill.location.y, kill.location.z};
            const world::Vec3 dragon_position = dragon_.dragon().position;
            const float dx = location.x - dragon_position.x;
            const float dy = location.y - dragon_position.y;
            const float dz = location.z - dragon_position.z;
            const bool known_killer = !receiving_as_host || kill.killer_gid == 0U ||
                                      online_remote_players_.contains(kill.killer_gid);
            if (!known_killer || dx * dx + dy * dy + dz * dz > 2'500.0F ||
                (receiving_as_host && dragon_.dragon().health > 0.0F))
                return false;
            dragon_loot_pending_ = kill.killer_gid == online_local_player_gid();
            dragon_.damage(std::max(1.0F, dragon_.dragon().health + 1.0F));
            queued_dragon_fireball_ids_.clear();
            dragon_kill_announced_ = true;
            dragon_owner_gid_.reset();
            dragon_pending_ = false;
            return true;
        }
        if (decoded.message->name == "RemoveDragonMessage")
        {
            if (dragon_kill_announced_ && !dragon_owner_gid_.has_value())
                return true;
            if (!decoded.payload.empty())
                return false;
            if (!dragon_.active() || !dragon_owner_gid_.has_value() ||
                *dragon_owner_gid_ != sender_gid)
                return !receiving_as_host;
            dragon_.deactivate();
            queued_dragon_fireball_ids_.clear();
            dragon_owner_gid_.reset();
            dragon_pending_ = false;
            dragon_loot_pending_ = false;
            return true;
        }
        if (decoded.message->name == "UpdateDragonMessage")
        {
            if (dragon_kill_announced_ && !dragon_owner_gid_.has_value())
                return true;
            net::DragonWaypointPayload waypoint;
            if (!net::decode_update_dragon_payload(decoded.payload, waypoint, payload_error) ||
                !valid_network_dragon_waypoint(waypoint))
                return false;
            if (!dragon_.active() || !dragon_owner_gid_.has_value() ||
                *dragon_owner_gid_ != sender_gid)
                return !receiving_as_host;
            return apply_network_dragon_waypoint(sender_gid, waypoint);
        }
        if (decoded.message->name == "DragonAttackMessage")
        {
            if (dragon_kill_announced_ && !dragon_owner_gid_.has_value())
                return true;
            net::DragonAttackPayload attack;
            if (!net::decode_dragon_attack_payload(decoded.payload, attack, payload_error) ||
                !valid_network_dragon_waypoint(attack.waypoint))
                return false;
            if (!dragon_.active() || !dragon_owner_gid_.has_value() ||
                *dragon_owner_gid_ != sender_gid)
                return !receiving_as_host;
            return apply_network_dragon_attack(sender_gid, attack);
        }
        if (decoded.message->name == "DetonateFireballMessage")
            return apply_online_fireball_detonation(sender_gid, decoded.payload, receiving_as_host);
        return true;
    }

    // The original timed-dragon schedule: staying inside a thousand-cell box counts
    // down to the next escalating dragon (fire, forest, sand, ice, undead); leaving
    // the box resets the ladder.
    void update_dragon(float fixed_delta)
    {
        const world::PlayerState &player = player_.state();
        auto &rng = enemy_horde_.rng();
        // NOENEMIES kills dragons too (the whole EnemyManager returns early).
        if (difficulty_ == GameDifficulty::no_enemies)
            return;
        // Dragon Endurance replaces every ambient dragon system with the wave
        // loop: 25 HP + 100 per wave (-15 per kill), 10-30 s between waves,
        // types cycling fire -> forest -> sand -> ice -> undead -> fire...
        if (game_mode_ == GameMode::dragon_endurance)
        {
            const bool may_schedule_wave =
                !online_gameplay_active() || (online_host_ != nullptr && online_host_->running());
            if (!dragon_.active() && may_schedule_wave)
            {
                endurance_dragon_delay_ -= fixed_delta;
                if (endurance_dragon_delay_ <= 0.0F)
                {
                    const auto kind = static_cast<world::DragonKind>(endurance_dragon_type_);
                    if (request_dragon_spawn(kind, false, endurance_dragon_health_))
                    {
                        toast("DRAGON WAVE INBOUND", dragon_kind_name(kind));
                        endurance_dragon_health_ += 100.0F;
                        endurance_dragon_type_ = (endurance_dragon_type_ + 1) % 5;
                    }
                }
            }
            update_active_dragon(fixed_delta, rng);
            update_fireballs(fixed_delta);
            return;
        }
        // The original distance ladder: crossing 100/500/1000/2600/4000 blocks
        // from the start summons that biome's dragon once (fire, forest, sand,
        // ice, undead). A threshold crossed while another dragon flies is
        // consumed without a spawn, exactly like AskForDragon's no-op path.
        static constexpr std::array<float, 5> dragon_distances{100.0F, 500.0F, 1000.0F, 2600.0F,
                                                               4000.0F};
        const float geometric_distance = std::hypot(player.position.x, player.position.z);
        // DistanceTrigger's teleport guard: a >10-block jump in a single frame
        // consumes any crossed threshold WITHOUT a spawn — this is what kept
        // the original from summoning the wrong biome's dragon after fast
        // travel (e.g. an ice dragon over the desert).
        const bool teleport_jump = last_dragon_distance_ >= 0.0F &&
                                   std::abs(geometric_distance - last_dragon_distance_) > 10.0F;
        last_dragon_distance_ = geometric_distance;
        const bool may_schedule_distance_dragon =
            !online_gameplay_active() || (online_host_ != nullptr && online_host_->running());
        while (may_schedule_distance_dragon &&
               dragon_distance_index_ < static_cast<int>(dragon_distances.size()) &&
               geometric_distance >
                   dragon_distances[static_cast<std::size_t>(dragon_distance_index_)])
        {
            world::DragonKind kind = static_cast<world::DragonKind>(dragon_distance_index_);
            // Beyond 3600 blocks every dragon is the undead dragon.
            if (geometric_distance > 3600.0F)
                kind = world::DragonKind::undead;
            if (!teleport_jump && !dragon_.active())
            {
                if (request_dragon_spawn(kind, true))
                    toast("DRAGON INBOUND", dragon_kind_name(kind));
                ++dragon_distance_index_;
                break;
            }
            ++dragon_distance_index_;
            if (!teleport_jump)
                break;
        }
        const bool outside_box = !dragon_box_valid_ ||
                                 std::abs(player.position.x - dragon_box_center_x_) > 500.0F ||
                                 std::abs(player.position.z - dragon_box_center_z_) > 500.0F;
        if (outside_box)
        {
            dragon_box_valid_ = true;
            dragon_box_center_x_ = player.position.x;
            dragon_box_center_z_ = player.position.z;
            next_timed_dragon_ = world::DragonKind::fire;
            next_dragon_seconds_ = world::dragon_schedule_minutes(next_timed_dragon_) * 60.0F +
                                   16.0F * 60.0F * (rng.next01() * 0.5F - 0.25F);
        }
        else if (!dragon_.active())
        {
            next_dragon_seconds_ -= fixed_delta;
            if (next_dragon_seconds_ <= 0.0F)
            {
                const world::DragonKind timed_kind =
                    geometric_distance > 3600.0F ? world::DragonKind::undead : next_timed_dragon_;
                if (request_dragon_spawn(timed_kind, false))
                    toast("DRAGON INBOUND", dragon_kind_name(timed_kind));
                next_timed_dragon_ = static_cast<world::DragonKind>(
                    std::min<int>(static_cast<int>(next_timed_dragon_) + 1,
                                  static_cast<int>(world::DragonKind::undead)));
                next_dragon_seconds_ = world::dragon_schedule_minutes(next_timed_dragon_) * 60.0F +
                                       16.0F * 60.0F * (rng.next01() * 0.5F - 0.25F);
            }
        }
        update_active_dragon(fixed_delta, rng);
        update_fireballs(fixed_delta);
    }

    [[nodiscard]] bool dragon_can_see_player() const
    {
        if (!dragon_.active())
            return false;
        const world::Vec3 origin = dragon_.dragon().position;
        const world::PlayerState &player = player_.state();
        const std::array targets{
            world::Vec3{player.position.x, player.position.y + world::PlayerSimulation::eye_height,
                        player.position.z},
            world::Vec3{player.position.x,
                        player.position.y + world::PlayerSimulation::body_height * 0.5F,
                        player.position.z}};
        for (const world::Vec3 target : targets)
        {
            world::Vec3 direction{target.x - origin.x, target.y - origin.y, target.z - origin.z};
            const float distance = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                             direction.z * direction.z);
            if (distance <= 0.001F)
                return true;
            direction.x /= distance;
            direction.y /= distance;
            direction.z /= distance;
            // Stop just short of the player volume so the terrain trace only
            // answers whether cover lies between hunter and target.
            if (!world::PlayerSimulation::raycast_blocking(
                    world_preview_->region(), origin, direction, std::max(0.0F, distance - 0.75F)))
                return true;
        }
        return false;
    }

    void update_active_dragon(float fixed_delta, world::EnemyRng &rng)
    {
        if (!dragon_.active())
            return;
        const world::PlayerState &player = player_.state();
        if (online_gameplay_active() && !local_owns_dragon_brain())
        {
            if (dragon_.dragon().state == world::DragonState::dying)
            {
                const world::DragonEvents death = dragon_.update(player, fixed_delta, rng, false);
                if (death.died)
                {
                    spawn_pending_dragon_pickups();
                    toast("DRAGON SLAIN", dragon_kind_name(dragon_.dragon().kind));
                    caption("[ DRAGON DESTROYED ]", 2.0F);
                    if (game_mode_ == GameMode::dragon_endurance)
                    {
                        endurance_dragon_health_ =
                            std::max(25.0F, endurance_dragon_health_ - 15.0F);
                        endurance_dragon_delay_ = 10.0F + rng.next01() * 20.0F;
                    }
                }
            }
            else
            {
                dragon_.extrapolate_network_replica(fixed_delta);
                process_network_dragon_replica_events();
            }
            return;
        }
        if (consider_dragon_migration(fixed_delta))
            return;
        dragon_network_time_ += fixed_delta;
        // DragonEntity.UpdateSounds: a scream 5 s after spawn, then every
        // 6-14 s for the dragon's whole life; WingFlap every second while
        // flapping (never while gliding).
        dragon_cry_timer_ -= fixed_delta;
        if (dragon_cry_timer_ <= 0.0F)
        {
            play_positional("DragonScream", dragon_.dragon().position);
            if (online_gameplay_active())
                dragon_network_sound_ = 1U;
            dragon_cry_timer_ = 6.0F + rng.next01() * 8.0F;
        }
        if (dragon_.dragon().clip == "fly_forward")
        {
            dragon_flap_timer_ -= fixed_delta;
            if (dragon_flap_timer_ <= 0.0F)
            {
                play_positional("WingFlap", dragon_.dragon().position);
                dragon_flap_timer_ = 1.0F;
            }
        }
        const world::DragonEvents events =
            dragon_.update(player, fixed_delta, rng, dragon_can_see_player());
        bool sent_attack{};
        for (std::size_t index = 0; index < events.fireball_origins.size(); ++index)
        {
            int fireball_id = next_fireball_id_++;
            if (online_gameplay_active())
            {
                const std::uint32_t encoded =
                    (static_cast<std::uint32_t>(online_local_player_gid()) << 23U) |
                    (static_cast<std::uint32_t>(next_dragon_fireball_index_++) & 0x7FFFFFU);
                fireball_id = static_cast<std::int32_t>(encoded);
                publish_dragon_attack(player.position, fireball_id);
                sent_attack = true;
            }
            fireballs_.push_back({events.fireball_origins[index], events.fireball_velocities[index],
                                  dragon_.dragon().kind, fireball_id, false, false,
                                  online_gameplay_active() ? online_local_player_gid() : 0U, true});
            sound_bank_->sync_fireball_cue(
                static_cast<std::uint32_t>(fireball_id),
                dragon_.dragon().kind == world::DragonKind::ice ? "Iceball" : "Fireball",
                events.fireball_origins[index], events.fireball_velocities[index]);
        }
        if (online_gameplay_active() && dragon_.active())
        {
            dragon_update_timer_ -= fixed_delta;
            if (!sent_attack && dragon_update_timer_ <= 0.0F)
            {
                publish_dragon_update();
                dragon_update_timer_ += 0.2F;
            }
        }
        if (events.died)
        {
            spawn_pending_dragon_pickups();
            toast("DRAGON SLAIN", dragon_kind_name(dragon_.dragon().kind));
            caption("[ DRAGON DESTROYED ]", 2.0F);
            if (game_mode_ == GameMode::dragon_endurance)
            {
                // ENDURANCE_DRAGON_KILLED_HEALTH_INCREMENT (-15) and the
                // 10-30 s pause before the next wave.
                endurance_dragon_health_ = std::max(25.0F, endurance_dragon_health_ - 15.0F);
                endurance_dragon_delay_ = 10.0F + rng.next01() * 20.0F;
            }
        }
        else if (events.left)
        {
            if (online_gameplay_active())
                publish_remove_dragon();
            toast("DRAGON DEPARTS", "It lost interest... for now.");
            if (game_mode_ == GameMode::dragon_endurance)
                endurance_dragon_delay_ = 10.0F + rng.next01() * 20.0F;
        }
    }

    void spawn_pending_dragon_pickups()
    {
        if (!dragon_loot_pending_ || world_preview_ == nullptr)
            return;
        dragon_loot_pending_ = false;
        const world::Vec3 body = dragon_.dragon().position;
        const world::Vec3 pickup_position{body.x, body.y + 1.0F, body.z};
        for (const world::EnemyLoot loot : world::dragon_loot(body, enemy_horde_.rng()))
        {
            const InventoryStack stack{static_cast<InventoryItemId>(loot.item_id), loot.item_count};
            world_preview_->spawn_dropped_stack_at(stack, pickup_position, 3.0F);
        }
    }

    void update_fireballs(float fixed_delta)
    {
        const world::PlayerState &player = player_.state();
        const auto &region = world_preview_->region();
        for (Fireball &fireball : fireballs_)
        {
            if (fireball.awaiting_detonation)
            {
                fireball.detonation_wait += fixed_delta;
                if (fireball.detonation_wait > 2.0F)
                    fireball.dead = true;
                continue;
            }
            const world::Vec3 next{fireball.position.x + fireball.velocity.x * fixed_delta,
                                   fireball.position.y + fireball.velocity.y * fixed_delta,
                                   fireball.position.z + fireball.velocity.z * fixed_delta};
            if (next.y < -66.0F)
            {
                sound_bank_->stop_fireball_cue(static_cast<std::uint32_t>(fireball.id));
                fireball.dead = true;
                continue;
            }
            const int next_x = static_cast<int>(std::floor(next.x));
            const int next_z = static_cast<int>(std::floor(next.z));
            const bool inside_loaded_area =
                next_x >= region.minimum_x() && next_x <= region.maximum_x() &&
                next_z >= region.minimum_z() && next_z <= region.maximum_z();
            if (!inside_loaded_area)
            {
                fireball.position = next;
                if (fireball.was_in_loaded_area)
                {
                    sound_bank_->stop_fireball_cue(static_cast<std::uint32_t>(fireball.id));
                    fireball.dead = true;
                }
                else
                    sound_bank_->sync_fireball_cue(
                        static_cast<std::uint32_t>(fireball.id),
                        fireball.kind == world::DragonKind::ice ? "Iceball" : "Fireball",
                        fireball.position, fireball.velocity);
                continue;
            }
            fireball.was_in_loaded_area = true;
            const world::FireballSweepHit impact =
                world::fireball_sweep(region, player, fireball.position, next);
            if (impact)
            {
                fireball.position = impact.position;
                sound_bank_->stop_fireball_cue(static_cast<std::uint32_t>(fireball.id));
                if (fireball.spawned_locally)
                {
                    if (!publish_fireball_detonation(fireball.id, fireball.kind, impact.position))
                        fireball.dead = true;
                }
                else
                {
                    fireball.awaiting_detonation = true;
                    fireball.detonation_wait = 0.0F;
                }
                continue;
            }
            fireball.position = next;
            sound_bank_->sync_fireball_cue(static_cast<std::uint32_t>(fireball.id),
                                           fireball.kind == world::DragonKind::ice ? "Iceball"
                                                                                   : "Fireball",
                                           fireball.position, fireball.velocity);
        }
        std::erase_if(fireballs_, [](const Fireball &fireball) { return fireball.dead; });
    }

    [[nodiscard]] static const char *dragon_kind_name(world::DragonKind kind) noexcept
    {
        switch (kind)
        {
        case world::DragonKind::fire:
            return "FIRE DRAGON";
        case world::DragonKind::forest:
            return "FOREST DRAGON";
        case world::DragonKind::sand:
            return "SAND DRAGON";
        case world::DragonKind::ice:
            return "ICE DRAGON";
        case world::DragonKind::undead:
        default:
            return "UNDEAD DRAGON";
        }
    }

    [[nodiscard]] std::vector<net::IntVector3> fireball_blocks_to_remove(
        world::Vec3 impact_position, world::DragonKind kind) const
    {
        std::vector<net::IntVector3> blocks;
        if (!dragons_damage_structures(game_mode_, difficulty_))
            return blocks;
        std::vector<net::IntVector3> dependents;
        const auto &region = world_preview_->region();
        const int center_x = static_cast<int>(std::floor(impact_position.x));
        const int center_y = static_cast<int>(std::floor(impact_position.y));
        const int center_z = static_cast<int>(std::floor(impact_position.z));
        constexpr std::array<std::array<int, 3>, 6> face_offsets{{
            {{1, 0, 0}},
            {{0, 0, -1}},
            {{-1, 0, 0}},
            {{0, 0, 1}},
            {{0, 1, 0}},
            {{0, -1, 0}},
        }};
        const auto remember_dependents = [&](net::IntVector3 support) {
            for (std::size_t face{}; face < face_offsets.size(); ++face)
            {
                const net::IntVector3 neighbor{support.x + face_offsets[face][0],
                                               support.y + face_offsets[face][1],
                                               support.z + face_offsets[face][2]};
                if (!region.contains(neighbor.x, neighbor.y, neighbor.z))
                    continue;
                const world::BlockType dependent = region.block(neighbor.x, neighbor.y, neighbor.z);
                if (world::attached_face(dependent) == static_cast<world::FaceDirection>(face))
                    dependents.push_back(neighbor);
            }
        };
        // EnemyManager.DetonateFireball walks X, then Y, then Z around the
        // centre of the impact voxel and tests each voxel centre against the
        // raw impact point. Preserve that order because it is the stock wire
        // block-list order.
        for (int x = center_x - 3; x <= center_x + 3; ++x)
            for (int y = center_y - 3; y <= center_y + 3; ++y)
                for (int z = center_z - 3; z <= center_z + 3; ++z)
                {
                    const float dx = static_cast<float>(x) + 0.5F - impact_position.x;
                    const float dy = static_cast<float>(y) + 0.5F - impact_position.y;
                    const float dz = static_cast<float>(z) + 0.5F - impact_position.z;
                    if (dx * dx + dy * dy + dz * dz > 9.0F || !region.contains(x, y, z))
                        continue;
                    const world::BlockType block = region.block(x, y, z);
                    if (block == world::BlockType::empty ||
                        block == world::BlockType::uninitialized || world::is_upper_door(block) ||
                        !world::dragon_breaks_block(kind, block))
                        continue;
                    const net::IntVector3 location{x, y, z};
                    blocks.push_back(location);
                    remember_dependents(location);
                    if (world::is_lower_door(block))
                    {
                        const net::IntVector3 upper{x, y + 1, z};
                        blocks.push_back(upper);
                        remember_dependents(upper);
                    }
                }
        const std::size_t directly_damaged = blocks.size();
        for (const net::IntVector3 dependent : dependents)
        {
            if (std::ranges::find(
                    blocks.begin(), blocks.begin() + static_cast<std::ptrdiff_t>(directly_damaged),
                    dependent) != blocks.begin() + static_cast<std::ptrdiff_t>(directly_damaged))
                continue;
            blocks.push_back(dependent);
            const world::BlockType block = region.block(dependent.x, dependent.y, dependent.z);
            if (world::is_lower_door(block))
                blocks.push_back({dependent.x, dependent.y + 1, dependent.z});
        }
        return blocks;
    }

    [[nodiscard]] bool apply_online_fireball_detonation(std::uint8_t sender_gid,
                                                        std::span<const std::uint8_t> payload,
                                                        bool receiving_as_host)
    {
        net::DetonateFireballPayload message;
        net::PayloadError payload_error{};
        if (!net::decode_detonate_fireball_payload(payload, message, payload_error))
            return false;
        const std::uint8_t encoded_owner = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(message.fireball_index) >> 23U) & 0xFFU);
        if (encoded_owner != sender_gid || message.location.x < -1'000'000.0F ||
            message.location.x > 1'000'000.0F || message.location.y < -128.0F ||
            message.location.y > 128.0F || message.location.z < -1'000'000.0F ||
            message.location.z > 1'000'000.0F)
            return false;
        const bool already_applied =
            std::ranges::find(applied_fireball_detonations_, message.fireball_index) !=
            applied_fireball_detonations_.end();
        if (already_applied)
            return !receiving_as_host;

        const auto fireball = std::ranges::find_if(fireballs_, [&](const Fireball &candidate) {
            return candidate.id == message.fireball_index;
        });
        if (fireball == fireballs_.end())
            return !receiving_as_host;
        else
        {
            const world::Vec3 impact{message.location.x, message.location.y, message.location.z};
            const float dx = impact.x - fireball->position.x;
            const float dy = impact.y - fireball->position.y;
            const float dz = impact.z - fireball->position.z;
            const float maximum_error =
                world::dragon_stats(fireball->kind).fireball_velocity * 2.0F + 5.0F;
            if (fireball->owner_gid != sender_gid ||
                static_cast<std::uint8_t>(fireball->kind) != message.dragon_type ||
                dx * dx + dy * dy + dz * dz > maximum_error * maximum_error)
                return false;
        }

        const world::Vec3 impact{message.location.x, message.location.y, message.location.z};
        const auto kind = static_cast<world::DragonKind>(message.dragon_type);
        if (message.blocks_to_remove != fireball_blocks_to_remove(impact, kind))
            return false;
        if (receiving_as_host && !message.blocks_to_remove.empty())
        {
            const world::BlockType replacement =
                kind == world::DragonKind::ice ? world::BlockType::ice : world::BlockType::empty;
            if (online_host_ == nullptr ||
                !online_host_->apply_authoritative_block_replacements(
                    message.blocks_to_remove, static_cast<std::uint8_t>(replacement)))
                return false;
        }
        apply_fireball_detonation(message);
        return true;
    }

    void apply_fireball_detonation(const net::DetonateFireballPayload &message)
    {
        const world::Vec3 impact_position{message.location.x, message.location.y,
                                          message.location.z};
        const auto kind = static_cast<world::DragonKind>(message.dragon_type);
        if (auto fireball = std::ranges::find_if(
                fireballs_,
                [&](const Fireball &candidate) { return candidate.id == message.fireball_index; });
            fireball != fireballs_.end())
        {
            sound_bank_->stop_fireball_cue(static_cast<std::uint32_t>(fireball->id));
            fireball->dead = true;
            fireball->awaiting_detonation = false;
        }
        const world::DragonStats &stats = world::dragon_stats(kind);
        const world::PlayerState &player = player_.state();
        const float dx = impact_position.x - player.position.x;
        const float dy = impact_position.y - (player.position.y + 1.0F);
        const float dz = impact_position.z - player.position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        auto &region = world_preview_->region();
        // HandleDetonateFireballMessage: 5-unit blast, full damage inside the
        // first unit then a linear falloff. DamageLOSProbe runs before terrain
        // destruction, so shelter still matters even when this blast breaks it.
        if (distance < 5.0F)
        {
            const float line_of_sight = world::dragon_fireball_damage_multiplier(
                region, kind, impact_position,
                {player.position.x, player.position.y + 1.0F, player.position.z});
            const float damage = stats.fireball_damage * 100.0F * line_of_sight *
                                 (1.0F - std::max(distance - 1.0F, 0.0F) / 5.0F);
            if (damage > 0.0F)
            {
                caption("[ FIREBALL HIT ]", 1.0F);
                apply_player_damage(damage, impact_position);
            }
        }
        play_positional(kind == world::DragonKind::ice ? "Freeze" : "Explosion", impact_position);
        std::vector<std::array<int, 3>> changed;
        changed.reserve(message.blocks_to_remove.size());
        const world::BlockType replacement =
            kind == world::DragonKind::ice ? world::BlockType::ice : world::BlockType::empty;
        for (const net::IntVector3 block : message.blocks_to_remove)
        {
            if (region.contains(block.x, block.y, block.z))
                region.set_block(block.x, block.y, block.z, replacement);
            world_preview_->remember_edit(block.x, block.y, block.z, replacement);
            changed.push_back({block.x, block.y, block.z});
        }
        if (!changed.empty())
        {
            world_preview_->rebuild_block_edits(changed);
            dirty_ = true;
        }
        // FireballEntity.Detonate: FlashEffect + FirePuff + BigSmokePuff +
        // BigRockBlast, one emitter each at the impact point.
        world_preview_->spawn_fireball_explosion(impact_position, kind == world::DragonKind::ice);
        static_cast<void>(enemy_horde_.apply_explosive_damage(impact_position, 6.0F, false));
        applied_fireball_detonations_.push_back(message.fireball_index);
        if (applied_fireball_detonations_.size() > 128U)
            applied_fireball_detonations_.erase(applied_fireball_detonations_.begin());
    }

    [[nodiscard]] bool publish_fireball_detonation(std::int32_t fireball_id,
                                                   world::DragonKind fireball_kind,
                                                   world::Vec3 impact_position)
    {
        net::DetonateFireballPayload message;
        message.location = {impact_position.x, impact_position.y, impact_position.z};
        message.fireball_index = fireball_id;
        message.dragon_type = static_cast<std::uint8_t>(fireball_kind);
        message.blocks_to_remove = fireball_blocks_to_remove(impact_position, fireball_kind);
        std::map<CrateKey, world::BlockType> removed_doors;
        for (const net::IntVector3 location : message.blocks_to_remove)
        {
            const auto block = online_block_at(location);
            if (block.has_value() && (world::is_lower_door(*block) || world::is_upper_door(*block)))
                removed_doors.try_emplace(lower_door_key(location, *block), *block);
        }
        for (const auto &[key, block] : removed_doors)
            publish_local_custom_door_destroy(key, block);
        net::PayloadError payload_error{};
        const auto payload = net::encode_detonate_fireball_payload(message, payload_error);
        if (!online_gameplay_active())
        {
            if (payload.has_value())
            {
                apply_fireball_detonation(message);
                return true;
            }
            return false;
        }
        std::string error;
        const bool receiving_as_host = online_host_ != nullptr && online_host_->running();
        if (!payload.has_value() || !apply_online_fireball_detonation(online_local_player_gid(),
                                                                      *payload, receiving_as_host))
        {
            std::cerr << "network.local.detonate-fireball.failure=authority\n";
            return false;
        }
        if (!publish_online_broadcast("DetonateFireballMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.detonate-fireball.failure=" << error << '\n';
        return true;
    }

    void apply_rocket_detonation(world::Vec3 impact_position, bool hit_dragon,
                                 bool dragon_explosive_headshot, bool damage_local_player,
                                 bool locally_authoritative_enemy_damage, InventoryItemId weapon,
                                 std::uint8_t killer_gid)
    {
        // ExplosiveTypes.Rocket contract: kill range 3, player damage range 6,
        // enemy range 12, direct dragon impact 200, and a one-cell block cube.
        play_positional("Explosion", impact_position);
        world_preview_->spawn_fireball_explosion(impact_position, false, !hit_dragon);
        if (hit_dragon)
        {
            // TakeExplosiveDamage oddly multiplies a body impact by 2.5, while
            // an impact ahead of BodyHitVolume[1] (the head) keeps the base 200.
            damage_dragon_with_network(200.0F * (dragon_explosive_headshot ? 1.0F : 2.5F),
                                       dragon_.dragon().position, weapon, killer_gid);
            caption("[ ROCKET HIT DRAGON ]", 1.0F);
        }
        else
            apply_explosion_splash(impact_position, world::rocket_damage_bands, damage_local_player,
                                   locally_authoritative_enemy_damage, weapon);
    }

    void explode_rocket(world::Vec3 impact_position, bool hit_dragon,
                        bool dragon_explosive_headshot = false,
                        InventoryItemId weapon = InventoryItemId::rocket_launcher)
    {
        net::PayloadError payload_error{};
        const net::DetonateRocketPayload message{
            {impact_position.x, impact_position.y, impact_position.z},
            hit_dragon,
            static_cast<std::uint8_t>(world::ExplosiveKind::rocket),
            static_cast<std::uint8_t>(weapon)};
        const auto payload =
            net::encode_detonate_rocket_payload(message, online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("DetonateRocketMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            std::cerr << "network.local.detonate-rocket.failure=" << error << '\n';
        apply_rocket_detonation(impact_position, hit_dragon, dragon_explosive_headshot, true, true,
                                weapon, online_gameplay_active() ? online_local_player_gid() : 0U);
        // Rockets also start deterministic TNT/C4 chains inside their one-cell
        // destruction cube; HandleDetonateRocketMessage always performs this
        // block pass, including on a direct dragon hit.
        destroy_blocks_with_explosive(impact_position, world::ExplosiveKind::rocket);
    }

    void apply_biome_atmosphere(world::SkyState &sky) const
    {
        if (settings_.presentation != PresentationProfile::rebirth || world_preview_ == nullptr ||
            SDL_getenv("OPENMINERZ_DISABLE_BIOME_ATMOSPHERE") != nullptr)
        {
            return;
        }
        const world::BiomeAtmosphere atmosphere = world::biome_atmosphere(world_preview_->biome());
        const float presence = 0.35F + 0.65F * sky.daylight;
        const auto blend = [presence](world::SkyColor source, world::SkyColor target, float mix) {
            const float amount = std::clamp(mix * presence, 0.0F, 1.0F);
            return world::SkyColor{std::lerp(source.red, target.red, amount),
                                   std::lerp(source.green, target.green, amount),
                                   std::lerp(source.blue, target.blue, amount)};
        };
        sky.fog = blend(sky.fog, atmosphere.fog_target, atmosphere.fog_mix);
        sky.horizon = blend(sky.horizon, atmosphere.horizon_target, atmosphere.horizon_mix);
        sky.zenith = blend(sky.zenith, atmosphere.zenith_target, atmosphere.zenith_mix);
        // Dense overcast removes hard noon energy, not all visibility. Keep a
        // broad ambient base while muting the Classic biome's direct sun; low
        // storminess in later biomes naturally restores the clear-day range.
        const float storm =
            std::clamp(atmosphere.storminess * (0.72F + 0.28F * sky.daylight), 0.0F, 1.0F);
        const float direct_scale = std::lerp(1.0F, 0.68F, storm);
        const float ambient_scale = std::lerp(1.0F, 0.90F, storm);
        sky.direct = {sky.direct.red * direct_scale, sky.direct.green * direct_scale,
                      sky.direct.blue * direct_scale};
        sky.ambient = {sky.ambient.red * ambient_scale, sky.ambient.green * ambient_scale,
                       sky.ambient.blue * ambient_scale};
    }

    // GameScreen.Day: Classic preserves the shipped fade to black below Y=-24.
    // Rebirth keeps the same transition depth but turns the deep layer into a
    // readable volcanic atmosphere. These palette values are display-referred;
    // the renderer converts them to linear before fogging the HDR world, so the
    // red channel must carry enough range to survive that conversion. Actual
    // nearby illumination remains driven by the occlusion-safe lava/emissive
    // bake rather than this distant-air colour.
    void apply_depth_fog(world::SkyState &sky) const
    {
        const float depth_fog = std::clamp((player_.state().position.y + 32.0F) / 8.0F, 0.0F, 1.0F);
        if (settings_.presentation == PresentationProfile::rebirth)
        {
            const float hell = 1.0F - depth_fog;
            const auto blend = [hell](world::SkyColor source, world::SkyColor target) {
                return world::SkyColor{std::lerp(source.red, target.red, hell),
                                       std::lerp(source.green, target.green, hell),
                                       std::lerp(source.blue, target.blue, hell)};
            };
            const bool legacy_hell = SDL_getenv("OPENMINERZ_LEGACY_HELL_ATMOSPHERE") != nullptr;
            sky.fog = blend(sky.fog, legacy_hell ? world::SkyColor{0.110F, 0.025F, 0.015F}
                                                 : world::SkyColor{0.340F, 0.082F, 0.030F});
            sky.horizon = blend(sky.horizon, legacy_hell ? world::SkyColor{0.080F, 0.018F, 0.012F}
                                                         : world::SkyColor{0.275F, 0.058F, 0.022F});
            sky.zenith = blend(sky.zenith, legacy_hell ? world::SkyColor{0.010F, 0.004F, 0.006F}
                                                       : world::SkyColor{0.025F, 0.006F, 0.008F});
            if (!legacy_hell)
            {
                // AmbientSun is already a linear irradiance value. Keep it low:
                // it colours exposed cracks and the baked bounce source without
                // filling sealed rock with an unshadowed global light.
                sky.ambient = blend(sky.ambient, {0.045F, 0.010F, 0.004F});
                sky.direct = blend(sky.direct, {0.012F, 0.003F, 0.001F});
                sky.specular = blend(sky.specular, {0.030F, 0.006F, 0.002F});
            }
        }
        else
        {
            sky.fog.red *= depth_fog;
            sky.fog.green *= depth_fog;
            sky.fog.blue *= depth_fog;
        }
    }

    [[nodiscard]] world::SkyState current_sky_state() const
    {
        return day_night_.state(settings_.presentation == PresentationProfile::rebirth
                                    ? world::SkyModel::rebirth
                                    : world::SkyModel::classic);
    }

    // SoundManager.PlayInstance applies the real listener/emitter transforms
    // directly. Vanilla has no terrain ray that falsifies emitter distance.
    void play_positional(std::string_view cue, world::Vec3 position)
    {
        sound_bank_->play_cue_at(cue, position);
    }

    [[nodiscard]] bool player_inside_dry_crater() const noexcept
    {
        if (world_preview_ == nullptr)
            return false;
        const world::PlayerState &player = player_.state();
        const int cell_x = static_cast<int>(std::floor(player.position.x));
        const int cell_y = static_cast<int>(std::floor(player.position.y));
        const int cell_z = static_cast<int>(std::floor(player.position.z));
        const std::size_t stream_serial = world_preview_->stats().stream_serial;
        const bool probe_stale = crater_probe_x_ != cell_x || crater_probe_y_ != cell_y ||
                                 crater_probe_z_ != cell_z ||
                                 crater_probe_stream_serial_ != stream_serial ||
                                 world_game_time_ - crater_probe_time_ >= 0.5F;
        if (!probe_stale)
            return crater_probe_dry_;

        crater_probe_x_ = cell_x;
        crater_probe_y_ = cell_y;
        crater_probe_z_ = cell_z;
        crater_probe_stream_serial_ = stream_serial;
        crater_probe_time_ = world_game_time_;
        crater_probe_dry_ = false;
        const auto &region = world_preview_->region();
        constexpr int crater_radius = 30;
        constexpr int vertical_radius = 12;
        for (int z = cell_z - crater_radius; z <= cell_z + crater_radius && !crater_probe_dry_; ++z)
        {
            for (int x = cell_x - crater_radius; x <= cell_x + crater_radius && !crater_probe_dry_;
                 ++x)
            {
                const int dx = x - cell_x;
                const int dz = z - cell_z;
                if (dx * dx + dz * dz > crater_radius * crater_radius)
                    continue;
                for (int y = cell_y - vertical_radius; y <= cell_y + vertical_radius; ++y)
                {
                    if (!region.contains(x, y, z))
                        continue;
                    const world::BlockType block = region.block(x, y, z);
                    if (block == world::BlockType::space_rock ||
                        block == world::BlockType::space_rock_inventory)
                    {
                        crater_probe_dry_ = true;
                        break;
                    }
                }
            }
        }
        return crater_probe_dry_;
    }

    [[nodiscard]] bool player_water_world() const noexcept
    {
        // Water is the Lagoon's continuous sea plane, not a global volume.
        // A raw Y < water_level test incorrectly submerged every dry cave and
        // the whole hell layer, muting exactly those ambience beds. Authored
        // crash sites cut a dry hole through that plane as well.
        return world_preview_ != nullptr &&
               settings_.presentation == PresentationProfile::rebirth &&
               world_preview_->biome_name() == "LAGOON" && !player_inside_dry_crater();
    }

    [[nodiscard]] bool player_in_water(const world::PlayerState &player) const noexcept
    {
        return player_water_world() && world::water_depth(player.position.y) > 0.0F;
    }

    [[nodiscard]] bool player_underwater(const world::PlayerState &player) const noexcept
    {
        return world::is_underwater(player_water_world(), player.position.y);
    }

    [[nodiscard]] int player_depth_under_ground(const world::PlayerState &player) const noexcept
    {
        if (world_preview_ == nullptr)
            return 0;
        const auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(player.position.x));
        const int cell_z = static_cast<int>(std::floor(player.position.z));
        const int feet_y = static_cast<int>(std::floor(player.position.y));
        int depth{};
        for (int y = feet_y; y < world::world_min_y + world::chunk_height; ++y)
            if (region.contains(cell_x, y, cell_z) &&
                world::blocks_player(region.block(cell_x, y, cell_z)))
                ++depth;
        return depth;
    }

    void update_weather(float delta)
    {
        const world::PlayerState &player = player_.state();
        const world::LightningUpdate event =
            lightning_cycle_.update(delta, player_depth_under_ground(player), player.position.y);
        lightning_visible_ = event.visible;
        if (event.thunder_low)
            sound_bank_->play_cue("thunderLow");
        else if (event.thunder_high)
            sound_bank_->play_cue("thunderHigh");
    }

    // InGameHUD.Update's two independent environmental survival loops. Lava
    // goes through ApplyDamage every half-second; oxygen is a normalized ten-
    // second reserve and its penalty writes PlayerHealth directly in the
    // shipped game (therefore no Hit cue, damage arrow, or KillPlayer call).
    void update_environmental_survival(float fixed_delta)
    {
        const world::PlayerState &player = player_.state();
        if (world::lava_submersion(player.position.y) > 0.0F)
        {
            if (!lava_sound_played_)
            {
                lava_sound_played_ = true;
                sound_bank_->play_cue("Douse");
            }
            lava_damage_time_ -= fixed_delta;
            if (lava_damage_time_ <= 0.0001F)
            {
                world::Vec3 source = player.position;
                source.y -= 10.0F;
                apply_player_damage(world::lava_damage_health, source);
                lava_damage_time_ = world::lava_damage_period_seconds;
                lava_sound_played_ = false;
            }
        }
        else
        {
            // Vanilla deliberately leaves lavaSoundPlayed latched when the
            // player exits early; the first re-entry completes this timer.
            lava_damage_time_ = world::lava_damage_period_seconds;
        }

        if (player_underwater(player))
        {
            player_oxygen_ -= world::oxygen_decay_per_second * fixed_delta;
            if (player_oxygen_ < 0.0F)
            {
                player_oxygen_ = 0.0F;
                player_.state().health -= world::drowning_health_per_second * fixed_delta;
                last_player_damage_time_ = world_game_time_;
            }
        }
        else
        {
            player_oxygen_ = 1.0F;
        }
    }

    // GameScreen.Day's audio driver: listener follows the player, the four
    // ambient beds crossfade by hour/depth, and the Outdoors variable morphs
    // the authored reverb between the dry surface and the cave preset.
    void update_audio_environment(float delta)
    {
        audio_environment_timer_ -= delta;
        if (audio_environment_timer_ > 0.0F)
            return;
        audio_environment_timer_ = 0.25F;
        const world::PlayerState &player = player_.state();
        sound_bank_->update_listener(player.position, player.yaw);
        const float time = day_night_.time_of_day();
        const int hour = static_cast<int>(time * 24.0F);
        const float fraction = time * 24.0F - static_cast<float>(hour);
        float day_weight{};
        float night_weight{1.0F};
        if (hour == 6)
            night_weight = 1.0F - fraction;
        else if (hour == 7)
        {
            day_weight = 0.5F;
            night_weight = 0.5F;
        }
        else if (hour == 8)
        {
            day_weight = fraction;
            night_weight = 0.0F;
        }
        else if (hour >= 9 && hour <= 17)
        {
            day_weight = 1.0F;
            night_weight = 0.0F;
        }
        else if (hour == 18)
        {
            day_weight = 1.0F - fraction;
            night_weight = 0.0F;
        }
        else if (hour == 19)
        {
            day_weight = 0.5F;
            night_weight = 0.5F;
        }
        else if (hour == 20)
        {
            day_weight = 0.0F;
            night_weight = fraction;
        }
        // Cave weight: overburden above the head, ramping over 15 blocks.
        auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(player.position.x));
        const int cell_z = static_cast<int>(std::floor(player.position.z));
        const int feet_y = static_cast<int>(std::floor(player.position.y));
        // DepthUnderGround counts every BlockPlayer cell from the player's
        // current Y through the world top, not just contiguous ceiling.
        const int overburden = player_depth_under_ground(player);
        const float cave_weight = std::min(1.0F, static_cast<float>(overburden) / 15.0F);
        // CraterFoundTransitionMusicTrigger: SpaceTheme is a one-shot when at
        // least one crash-site block exists above the player's feet. The audio
        // bridge queues transition tracks while another song is active, matching
        // the original MediaPlayer-idle gate without interrupting it.
        int space_rock_depth = 0;
        for (int y = feet_y; y < world::world_min_y + world::chunk_height; ++y)
        {
            if (!region.contains(cell_x, y, cell_z))
                continue;
            const world::BlockType block = region.block(cell_x, y, cell_z);
            if (block == world::BlockType::space_rock ||
                block == world::BlockType::space_rock_inventory)
                ++space_rock_depth;
        }
        if (!crater_music_triggered_ && space_rock_depth >= 1)
        {
            sound_bank_->play_song("SpaceTheme");
            crater_music_triggered_ = true;
            crater_music_reset_timer_ = 0.0F;
        }
        else if (crater_music_triggered_)
        {
            // InGameHUD keeps the crater trigger latched for one minute, then
            // resets it as soon as the player is no longer under space rock.
            crater_music_reset_timer_ += 0.25F;
            if (crater_music_reset_timer_ >= 60.0F && space_rock_depth == 0)
            {
                crater_music_triggered_ = false;
                crater_music_reset_timer_ = 0.0F;
            }
        }
        const float hell_weight = player.position.y <= -37.0F
                                      ? std::min(1.0F, (-37.0F - player.position.y) / 10.0F)
                                      : 0.0F;
        const bool underwater = player_underwater(player);
        const float mute = underwater ? 0.0F : 1.0F;
        sound_bank_->set_ambience(day_weight * (1.0F - cave_weight) * mute,
                                  night_weight * (1.0F - cave_weight) * mute,
                                  cave_weight * (1.0F - hell_weight) * mute, hell_weight * mute,
                                  1.0F - std::max(cave_weight, hell_weight));
        // ZombieChase and ArcherSkeletonChase own a persistent SoundCue3D per
        // enemy. The bridge keeps authored cue lifetime, reapplies 3D at the
        // emitter's current position, and restarts only a finished chase cue.
        sound_bank_->begin_enemy_voice_frame();
        for (const world::Enemy &enemy : enemy_horde_.enemies())
        {
            if (enemy.state != world::EnemyState::chase)
                continue;
            const world::EnemyStats &stats = world::enemy_stats(enemy.type);
            const char *voice = "ZombieGrowl";
            if (stats.habitat == world::EnemyHabitat::caves ||
                stats.enemy_class == world::EnemyClass::archer)
                voice = "Skeleton";
            else if (stats.habitat == world::EnemyHabitat::crash_site)
                voice = "Alien";
            else if (stats.habitat == world::EnemyHabitat::hell)
                voice = "Felguard";
            sound_bank_->sync_enemy_voice(enemy.id, voice, enemy.position);
        }
        sound_bank_->end_enemy_voice_frame();
    }

    // Original SetDefaultInventory: a fresh world gets the stock loadout —
    // except Hardcore, which starts with nothing at all.
    void apply_difficulty_loadout()
    {
        inventory_ = PlayerInventory{};
        if (difficulty_ == GameDifficulty::hardcore)
        {
            for (auto &slot : inventory_.storage())
                slot.reset();
            for (auto &tray : inventory_.trays())
                for (auto &slot : tray)
                    slot.reset();
        }
        sync_selected_item();
    }

    [[nodiscard]] static std::size_t locator_name_characters(std::string_view text) noexcept
    {
        return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char value) {
            return (static_cast<unsigned char>(value) & 0xC0U) != 0x80U;
        }));
    }

    void append_locator_name_text(std::string_view input)
    {
        std::size_t characters = locator_name_characters(locator_name_draft_);
        for (std::size_t offset = 0; offset < input.size() && characters < 10U;)
        {
            const unsigned char lead = static_cast<unsigned char>(input[offset]);
            std::size_t bytes = 1U;
            if ((lead & 0xE0U) == 0xC0U)
                bytes = 2U;
            else if ((lead & 0xF0U) == 0xE0U)
                bytes = 3U;
            else if ((lead & 0xF8U) == 0xF0U)
                bytes = 4U;
            if (offset + bytes > input.size())
                break;
            locator_name_draft_.append(input.substr(offset, bytes));
            offset += bytes;
            ++characters;
        }
    }

    void apply_locator_name_text_event(std::string_view input)
    {
        // SDL normally emits KEY_DOWN followed by TEXT_INPUT. KEY_DOWN supplies
        // a focused-window fallback for automation/IME-host edge cases; replace
        // it with SDL's authoritative text so normal typing is never doubled.
        if (!locator_key_fallback_.empty())
        {
            erase_locator_name_character();
            locator_key_fallback_.clear();
        }
        append_locator_name_text(input);
    }

    void erase_locator_name_character()
    {
        if (locator_name_draft_.empty())
            return;
        std::size_t offset = locator_name_draft_.size() - 1U;
        while (offset > 0U &&
               (static_cast<unsigned char>(locator_name_draft_[offset]) & 0xC0U) == 0x80U)
            --offset;
        locator_name_draft_.erase(offset);
    }

    void begin_locator_name_edit()
    {
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() ||
            (slot->item != InventoryItemId::gps && slot->item != InventoryItemId::teleport_gps))
            return;
        locator_name_slot_ = inventory_.selected_index();
        locator_name_draft_ = slot->waypoint_name;
        locator_key_fallback_.clear();
        locator_name_editing_ = true;
        cancel_world_action();
        SDL_SetWindowRelativeMouseMode(window_, false);
        (void)SDL_StartTextInput(window_);
        sound_bank_->play_cue("Popup");
    }

    void finish_locator_name_edit(bool accept)
    {
        if (!locator_name_editing_)
            return;
        if (accept && locator_name_slot_ < PlayerInventory::tray_size)
        {
            auto &slot = inventory_.tray()[locator_name_slot_];
            if (slot.has_value() &&
                (slot->item == InventoryItemId::gps || slot->item == InventoryItemId::teleport_gps))
            {
                slot->waypoint_name = locator_name_draft_;
                caption("[ LOCATOR RENAMED ]", 0.9F);
            }
        }
        locator_name_editing_ = false;
        locator_name_draft_.clear();
        locator_key_fallback_.clear();
        SDL_StopTextInput(window_);
        SDL_SetWindowRelativeMouseMode(window_, menu_.screen() == ui::MenuScreen::world_preview &&
                                                    !inventory_open_ && !inspector_view_);
        sound_bank_->play_click();
    }

    void append_chat_text(std::string_view input)
    {
        std::size_t characters = locator_name_characters(chat_draft_);
        for (std::size_t offset{}; offset < input.size() && characters < 96U;)
        {
            const unsigned char lead = static_cast<unsigned char>(input[offset]);
            std::size_t bytes = 1U;
            if ((lead & 0xE0U) == 0xC0U)
                bytes = 2U;
            else if ((lead & 0xF0U) == 0xE0U)
                bytes = 3U;
            else if ((lead & 0xF8U) == 0xF0U)
                bytes = 4U;
            if (offset + bytes > input.size() || chat_draft_.size() + bytes > 384U)
                break;
            if (bytes == 1U && (lead < 0x20U || lead == 0x7FU))
            {
                ++offset;
                continue;
            }
            chat_draft_.append(input.substr(offset, bytes));
            offset += bytes;
            ++characters;
        }
    }

    void apply_chat_text_event(std::string_view input)
    {
        if (!chat_key_fallback_.empty())
        {
            erase_chat_character();
            chat_key_fallback_.clear();
        }
        append_chat_text(input);
    }

    void erase_chat_character()
    {
        if (chat_draft_.empty())
            return;
        std::size_t offset = chat_draft_.size() - 1U;
        while (offset > 0U && (static_cast<unsigned char>(chat_draft_[offset]) & 0xC0U) == 0x80U)
            --offset;
        chat_draft_.erase(offset);
    }

    void begin_chat_edit()
    {
        const bool online = (online_host_ != nullptr && online_host_->running()) ||
                            (online_client_ != nullptr && online_client_joined_ &&
                             online_client_->connected() && online_bootstrap_client_ == nullptr);
        if (!online)
        {
            toast("NETWORK CHAT UNAVAILABLE", "JOIN OR HOST AN ONLINE SESSION FIRST");
            return;
        }
        chat_draft_.clear();
        chat_key_fallback_.clear();
        chat_editing_ = true;
        cancel_world_action();
        SDL_SetWindowRelativeMouseMode(window_, false);
        (void)SDL_StartTextInput(window_);
        sound_bank_->play_cue("Popup");
    }

    void push_online_chat(std::string message)
    {
        for (char &character : message)
            if (static_cast<unsigned char>(character) < 0x20U)
                character = ' ';
        if (message.size() > 160U)
            message.resize(160U);
        if (message.empty())
            return;
        if (!online_chat_lines_.empty() && online_chat_lines_.back().message == message &&
            elapsed_ - online_chat_lines_.back().received_at < 0.75F)
            return;
        online_chat_lines_.push_back({std::move(message), elapsed_});
        if (online_chat_lines_.size() > 12U)
            online_chat_lines_.erase(online_chat_lines_.begin());
    }

    void finish_chat_edit(bool accept)
    {
        if (!chat_editing_)
            return;
        if (accept)
        {
            std::size_t begin{};
            while (begin < chat_draft_.size() &&
                   std::isspace(static_cast<unsigned char>(chat_draft_[begin])) != 0)
                ++begin;
            std::size_t end = chat_draft_.size();
            while (end > begin &&
                   std::isspace(static_cast<unsigned char>(chat_draft_[end - 1U])) != 0)
                --end;
            if (begin < end)
            {
                std::string message =
                    online_persona_name_.empty() ? "OpenMinerZ" : online_persona_name_;
                message += ": ";
                message.append(chat_draft_.substr(begin, end - begin));
                net::PayloadError payload_error{};
                const auto payload = net::encode_chat_payload(message, payload_error);
                std::string error;
                if (!payload.has_value() ||
                    !publish_online_broadcast("BroadcastTextMessage", *payload,
                                              net::MessageDeliveryPolicy::reliable_ordered, error))
                    toast("CHAT SEND FAILED", uppercase(error));
                else
                    push_online_chat(std::move(message));
            }
        }
        chat_editing_ = false;
        chat_draft_.clear();
        chat_key_fallback_.clear();
        SDL_StopTextInput(window_);
        SDL_SetWindowRelativeMouseMode(window_, menu_.screen() == ui::MenuScreen::world_preview &&
                                                    !inventory_open_ && !inspector_view_);
        sound_bank_->play_click();
    }

    void append_host_text(std::string_view input)
    {
        const bool password = host_text_field_ == HostTextField::password ||
                              host_text_field_ == HostTextField::join_password;
        const bool direct = host_text_field_ == HostTextField::direct_address;
        const std::size_t maximum_characters = direct ? 320U : (password ? 64U : 40U);
        const std::size_t maximum_bytes = direct ? 320U : (password ? 256U : 128U);
        std::size_t characters = locator_name_characters(host_text_draft_);
        for (std::size_t offset = 0U; offset < input.size() && characters < maximum_characters;)
        {
            const unsigned char lead = static_cast<unsigned char>(input[offset]);
            std::size_t bytes = 1U;
            if ((lead & 0xE0U) == 0xC0U)
                bytes = 2U;
            else if ((lead & 0xF0U) == 0xE0U)
                bytes = 3U;
            else if ((lead & 0xF8U) == 0xF0U)
                bytes = 4U;
            if (offset + bytes > input.size() || host_text_draft_.size() + bytes > maximum_bytes)
                break;
            if (bytes == 1U && (lead < 0x20U || lead == 0x7FU))
            {
                ++offset;
                continue;
            }
            host_text_draft_.append(input.substr(offset, bytes));
            offset += bytes;
            ++characters;
        }
    }

    void apply_host_text_event(std::string_view input)
    {
        if (!host_text_fallback_.empty())
        {
            erase_host_text_character();
            host_text_fallback_.clear();
        }
        append_host_text(input);
    }

    void erase_host_text_character()
    {
        if (host_text_draft_.empty())
            return;
        std::size_t offset = host_text_draft_.size() - 1U;
        while (offset > 0U &&
               (static_cast<unsigned char>(host_text_draft_[offset]) & 0xC0U) == 0x80U)
            --offset;
        host_text_draft_.erase(offset);
    }

    void begin_host_text_edit(HostTextField field)
    {
        if (field == HostTextField::none)
            return;
        host_text_field_ = field;
        switch (field)
        {
        case HostTextField::password:
            host_text_draft_ = host_settings_.password;
            break;
        case HostTextField::join_password:
            host_text_draft_ = join_password_;
            break;
        case HostTextField::direct_address:
            host_text_draft_ = direct_address_;
            break;
        case HostTextField::server_name:
            host_text_draft_ = host_settings_.server_name;
            break;
        case HostTextField::none:
            return;
        }
        host_text_fallback_.clear();
        (void)SDL_StartTextInput(window_);
        sound_bank_->play_cue("Popup");
    }

    void finish_host_text_edit(bool accept)
    {
        if (host_text_field_ == HostTextField::none)
            return;
        if (accept)
        {
            if (host_text_field_ == HostTextField::join_password)
            {
                join_password_ = host_text_draft_;
                toast("JOIN PASSWORD UPDATED",
                      join_password_.empty() ? "PASSWORD CLEARED" : "PASSWORD IS SET");
            }
            else if (host_text_field_ == HostTextField::direct_address)
            {
                net::DirectEndpoint endpoint;
                std::string error;
                if (net::parse_direct_endpoint(host_text_draft_, endpoint, error))
                {
                    direct_address_ = host_text_draft_;
                    direct_endpoint_ = std::move(endpoint);
                    toast("DIRECT TARGET SAVED",
                          "A TRANSPORT PROBE WILL VERIFY ITS SESSION CONTRACT");
                }
                else
                    toast("INVALID DIRECT ADDRESS", uppercase(std::move(error)));
            }
            else
            {
                net::HostSessionSettings candidate = host_settings_;
                if (host_text_field_ == HostTextField::password)
                    candidate.password = host_text_draft_;
                else
                    candidate.server_name = host_text_draft_;
                std::string error;
                if (candidate.validate(error))
                {
                    host_settings_ = std::move(candidate);
                    toast(host_text_field_ == HostTextField::password ? "PASSWORD UPDATED"
                                                                      : "SERVER NAME UPDATED",
                          host_text_field_ == HostTextField::password
                              ? (host_settings_.password.empty() ? "SESSION PASSWORD CLEARED"
                                                                 : "SESSION PASSWORD IS SET")
                              : host_settings_.server_name);
                }
                else
                    toast("INVALID HOST SETTING", uppercase(std::move(error)));
            }
        }
        host_text_field_ = HostTextField::none;
        host_text_draft_.clear();
        host_text_fallback_.clear();
        SDL_StopTextInput(window_);
        sound_bank_->play_click();
    }

    [[nodiscard]] bool damage_selected_item()
    {
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value())
            return false;
        if (item_self_damage_per_use(slot->item) <= 0.0F)
            return false;
        if (!inflict_item_damage(*slot, infinite_resources_))
            return false;
        slot.reset();
        sync_selected_item();
        return true;
    }

    [[nodiscard]] bool place_navigation_waypoint()
    {
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() ||
            (slot->item != InventoryItemId::gps && slot->item != InventoryItemId::teleport_gps))
            return false;
        if (!player_.state().target.has_value())
        {
            sound_bank_->play_cue("Error");
            toast("NO WAYPOINT TARGET", "Aim at a block before setting the locator.");
            return false;
        }
        const world::VoxelRaycastHit &target = *player_.state().target;
        slot->waypoint = {static_cast<float>(target.x), static_cast<float>(target.y),
                          static_cast<float>(target.z)};
        slot->waypoint_valid = true;
        sync_selected_item();
        sound_bank_->play_cue("locator");
        caption("[ WAYPOINT SET ]", 1.0F);
        toast("WAYPOINT SET",
              std::format("{}  /  {} {} {}", slot->waypoint_name, target.x, target.y, target.z));
        return true;
    }

    [[nodiscard]] std::optional<world::Vec3> safe_position_near(
        const std::array<float, 3> &waypoint) const
    {
        const auto &region = world_preview_->region();
        const int origin_x = static_cast<int>(std::floor(waypoint[0]));
        const int origin_y = static_cast<int>(std::floor(waypoint[1]));
        const int origin_z = static_cast<int>(std::floor(waypoint[2]));
        for (int radius = 0; radius <= 16; ++radius)
            for (int dz = -radius; dz <= radius; ++dz)
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    if (radius > 0 && std::abs(dx) != radius && std::abs(dz) != radius)
                        continue;
                    const int x = origin_x + dx;
                    const int z = origin_z + dz;
                    for (int offset = 0; offset < world::chunk_height; ++offset)
                    {
                        const std::array<int, 2> candidates{origin_y + offset, origin_y - offset};
                        for (const int y : candidates)
                        {
                            if (!region.contains(x, y - 1, z) || !region.contains(x, y + 1, z) ||
                                !world::blocks_player(region.block(x, y - 1, z)) ||
                                world::blocks_player(region.block(x, y, z)) ||
                                world::blocks_player(region.block(x, y + 1, z)))
                                continue;
                            const world::Vec3 position{static_cast<float>(x) + 0.5F,
                                                       static_cast<float>(y) + 0.1F,
                                                       static_cast<float>(z) + 0.5F};
                            if (!world::PlayerSimulation::touches_solid(region, position))
                                return position;
                        }
                    }
                }
        return std::nullopt;
    }

    [[nodiscard]] bool teleport_to_selected_waypoint()
    {
        auto &slot = inventory_.tray()[inventory_.selected_index()];
        if (!slot.has_value() || slot->item != InventoryItemId::teleport_gps)
            return false;
        if (!slot->waypoint_valid)
        {
            sound_bank_->play_cue("Error");
            toast("TELEPORTER NOT SET", "Aim at a block and press use first.");
            return true;
        }

        auto &state = player_.state();
        state.position = {slot->waypoint[0] + 0.5F, slot->waypoint[1] + 1.1F,
                          slot->waypoint[2] + 0.5F};
        state.velocity = {};
        world_preview_->recenter_on(state);
        if (const auto safe = safe_position_near(slot->waypoint); safe.has_value())
            state.position = *safe;
        else
            player_.spawn(world_preview_->region(), static_cast<int>(slot->waypoint[0]),
                          static_cast<int>(slot->waypoint[2]));
        state.velocity = {};
        state.grounded = true;
        player_.refresh_target(world_preview_->region());
        sound_bank_->play_cue("Teleport");
        reset_farthest_distance();
        caption("[ TELEPORTED ]", 1.0F);
        const std::string waypoint_name = slot->waypoint_name;
        toast("TELEPORTED", waypoint_name);
        (void)damage_selected_item();
        return true;
    }

    // EnemyManager.ResetFarthestDistance, called by every original
    // GameScreen.TeleportToLocation operation.
    void reset_farthest_distance()
    {
        cleared_distance_ = 50.0F;
        static constexpr std::array<float, 5> dragon_distances{100.0F, 500.0F, 1000.0F, 2600.0F,
                                                               4000.0F};
        const float distance = std::hypot(player_.state().position.x, player_.state().position.z);
        dragon_distance_index_ =
            std::min(dragon_distance_index_, static_cast<int>(dragon_distances.size()));
        while (dragon_distance_index_ > 0 &&
               distance <= dragon_distances[static_cast<std::size_t>(dragon_distance_index_ - 1)])
            --dragon_distance_index_;
    }

    // TeleportToLocation(DefaultStartLocation, true): center terrain on the
    // exact original coordinate and then put the player on the topmost safe
    // cell. RefreshPlayer changes health only, so view and stamina survive.
    void place_player_at_world_start(bool refresh_health)
    {
        const float yaw = player_.state().yaw;
        const float pitch = player_.state().pitch;
        const float health = player_.state().health;
        const float stamina = player_.state().stamina;
        player_.state().position = world::default_start_location;
        player_.state().velocity = {};
        world_preview_->recenter_on(player_.state());
        player_.spawn(world_preview_->region(), static_cast<int>(world::default_start_location.x),
                      static_cast<int>(world::default_start_location.z));
        player_.state().yaw = yaw;
        player_.state().pitch = pitch;
        player_.state().health = refresh_health ? 100.0F : health;
        player_.state().stamina = stamina;
        player_.refresh_target(world_preview_->region());
        reset_farthest_distance();
    }

    void teleport_player(bool to_start)
    {
        if (to_start)
        {
            place_player_at_world_start(false);
            toast("TELEPORTED TO START", "Back at the origin tower.");
        }
        else
        {
            const float yaw = player_.state().yaw;
            const float pitch = player_.state().pitch;
            const float health = player_.state().health;
            const float stamina = player_.state().stamina;
            const auto &region = world_preview_->region();
            player_.spawn(region, static_cast<int>(std::floor(player_.state().position.x)),
                          static_cast<int>(std::floor(player_.state().position.z)));
            player_.state().yaw = yaw;
            player_.state().pitch = pitch;
            player_.state().health = health;
            player_.state().stamina = stamina;
            toast("TELEPORTED TO SURFACE", "Daylight again.");
            player_.refresh_target(region);
            reset_farthest_distance();
        }
        sound_bank_->play_cue("Teleport");
    }

    // PlayerInventory.DropAll: death spills the backpack; Hardcore also spills
    // the hotbar tray and respawns with the empty hardcore loadout.
    void drop_inventory_on_death()
    {
        for (auto &slot : inventory_.storage())
        {
            if (slot.has_value())
            {
                // DropAll calls PickupManager directly: unlike an intentional
                // Q-drop, death has no per-stack sound, caption, or toast.
                world_preview_->spawn_dropped_stack(*slot, player_.state());
                slot.reset();
            }
        }
        if (difficulty_ == GameDifficulty::hardcore)
        {
            for (auto &tray : inventory_.trays())
                for (auto &slot : tray)
                {
                    if (slot.has_value())
                    {
                        world_preview_->spawn_dropped_stack(*slot, player_.state());
                        slot.reset();
                    }
                }
        }
        sync_selected_item();
    }

    // InGameHUD.ApplyDamage: directional indicator, hit cue, recover-timer
    // reset, then the death check.
    void apply_player_damage(float health_points, world::Vec3 source)
    {
        if (health_points <= 0.0F)
            return;
        if (player_life_ != PlayerLife::alive)
            return;
        // InfiniteResourceMode (creative) skips damage entirely.
        if (infinite_resources_)
            return;
        damage_indicators_.push_back({source, 0.0F});
        if (damage_indicators_.size() > 12U)
            damage_indicators_.erase(damage_indicators_.begin());
        sound_bank_->play_cue("Hit");
        last_player_damage_time_ = world_game_time_;
        player_.state().health -= health_points;
        if (player_.state().health <= 0.0F && player_life_ == PlayerLife::alive)
        {
            // KillPlayer: the camera collapses with the body, the inventory
            // spills where you fell, and the world keeps running until you
            // press a button to respawn.
            player_.state().health = 0.0F;
            player_life_ = PlayerLife::dying;
            death_timer_ = 0.0F;
            online_player_update_timer_ = 0.0F;
            sound_bank_->play_cue("Fall");
            drop_inventory_on_death();
            cancel_world_action();
            close_inventory();
            inventory_open_ = false;
        }
    }

    void respawn_player()
    {
        player_life_ = PlayerLife::alive;
        death_timer_ = 0.0F;
        player_oxygen_ = 1.0F;
        lava_damage_time_ = world::lava_damage_period_seconds;
        lava_sound_played_ = false;
        online_player_update_timer_ = 0.0F;
        place_player_at_world_start(true);
        if (game_mode_ == GameMode::endurance)
        {
            // Offline Endurance sends RestartLevelMessage. Its receiver keeps
            // the terrain and pickups, but restarts the clock/music/HUD record.
            sound_bank_->stop_music();
            day_night_ = world::DayNightCycle{0.4F};
            last_day_number_ = 0;
            max_distance_ = 0;
        }
        damage_indicators_.clear();
        SDL_SetWindowRelativeMouseMode(window_, true);
        toast("RESPAWNED", difficulty_ == GameDifficulty::hardcore
                               ? "Everything you carried is gone."
                               : "Your gear is lying where you fell.");
    }

    [[nodiscard]] bool spawn_local_enemy(int type, world::Vec3 position, float midnight)
    {
        if (next_enemy_id_ > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            return false;
        const std::uint32_t enemy_id = next_enemy_id_++;
        const bool online = online_gameplay_active();
        const std::uint8_t target_gid = online ? online_local_player_gid() : 0U;
        if (!enemy_horde_.spawn(type, position, midnight, enemy_id, target_gid))
            return false;
        if (!online)
            return true;

        world::Enemy &enemy = enemy_horde_.enemies().back();
        const auto milliseconds = [](float seconds) {
            const double value =
                std::clamp(static_cast<double>(seconds) * 1'000.0, 0.0,
                           static_cast<double>(std::numeric_limits<std::int32_t>::max()));
            return static_cast<std::int32_t>(value);
        };
        net::SpawnEnemyPayload message;
        message.spawn_position = {position.x, position.y, position.z};
        message.enemy_type = static_cast<std::uint8_t>(type);
        message.enemy_id = static_cast<std::int32_t>(enemy_id);
        message.random_seed = enemy.random_seed;
        message.initialization = {milliseconds(enemy.slow_speed), milliseconds(enemy.fast_speed),
                                  milliseconds(enemy.emerge_speed),
                                  milliseconds(enemy.run_activation_time),
                                  milliseconds(enemy.normal_activation_time)};
        if (online_gameplay_protocol() == net::ProtocolVersion::modern_v4)
        {
            const world::Vec3 spawner = player_.state().position;
            message.spawner_position = {spawner.x, spawner.y, spawner.z};
            message.spawn_value = 0;
            message.player_name =
                online_persona_name_.empty() ? "OpenMinerZ" : online_persona_name_;
        }

        // The stock sender serializes millisecond integers and consumes its
        // local echo. Quantize our already-created local copy to that same data.
        enemy.slow_speed =
            static_cast<float>(message.initialization.slow_speed_milliseconds) / 1'000.0F;
        enemy.fast_speed =
            static_cast<float>(message.initialization.fast_speed_milliseconds) / 1'000.0F;
        enemy.emerge_speed =
            static_cast<float>(message.initialization.emerge_speed_milliseconds) / 1'000.0F;
        enemy.run_activation_time =
            static_cast<float>(message.initialization.run_activation_milliseconds) / 1'000.0F;
        enemy.normal_activation_time =
            static_cast<float>(message.initialization.normal_activation_milliseconds) / 1'000.0F;
        if (enemy.state == world::EnemyState::emerge)
            enemy.clip_speed = enemy.emerge_speed;

        net::PayloadError payload_error{};
        const auto payload =
            net::encode_spawn_enemy_payload(message, online_gameplay_protocol(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("SpawnEnemyMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            std::cerr << "network.local.spawn-enemy.failure=" << error << '\n';
        return true;
    }

    // EnemyManager's surface gate is torchlight < 0.4 OR no lantern within
    // 7.2 cells — a torch alone never stops a spawn (players still get
    // swarmed in torched bases), only a LANTERN protects the area.
    [[nodiscard]] bool spawn_point_dark(int cell_x, int cell_y, int cell_z) const
    {
        const auto &region = world_preview_->region();
        for (int y = cell_y - 4; y <= cell_y + 4; ++y)
            for (int z = cell_z - 7; z <= cell_z + 7; ++z)
                for (int x = cell_x - 7; x <= cell_x + 7; ++x)
                {
                    if (!region.contains(x, y, z))
                        continue;
                    const world::BlockType block = region.block(x, y, z);
                    if (block == world::BlockType::lantern ||
                        block == world::BlockType::fixed_lantern)
                        return false;
                }
        return true;
    }

    // BlockTerrain.FindClosestCeiling: walk up out of the ground into the air
    // pocket, then to the next solid ceiling. Reaching open sky means there is
    // no cave here and the spawn FAILS — cave enemies never appear on the
    // surface or inside sealed rock.
    // FindAlienSpawnPoint: aliens rise out of the crash-site rock — the stand
    // block must be SpaceRock with two open blocks above it.
    [[nodiscard]] bool try_spawn_alien_on_rock(world::Vec3 spot)
    {
        const auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(spot.x));
        const int cell_z = static_cast<int>(std::floor(spot.z));
        if (cell_x < region.minimum_x() + 2 || cell_x > region.maximum_x() - 2 ||
            cell_z < region.minimum_z() + 2 || cell_z > region.maximum_z() - 2)
            return false;
        const int top = world::world_min_y + world::chunk_height - 3;
        for (int y = top; y > world::world_min_y; --y)
        {
            const world::BlockType block = region.block(cell_x, y, cell_z);
            if (block != world::BlockType::space_rock &&
                block != world::BlockType::space_rock_inventory)
                continue;
            if (world::blocks_player(region.block(cell_x, y + 1, cell_z)) ||
                world::blocks_player(region.block(cell_x, y + 2, cell_z)))
                continue;
            const world::Vec3 spawn_spot{static_cast<float>(cell_x) + 0.5F,
                                         static_cast<float>(y + 1),
                                         static_cast<float>(cell_z) + 0.5F};
            if (!spawn_local_enemy(world::enemy_alien, spawn_spot, 0.5F))
                return false;
            play_positional("CreatureUnearth", spawn_spot);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_spawn_enemy_at_ceiling(int type, world::Vec3 spot)
    {
        const auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(spot.x));
        const int cell_z = static_cast<int>(std::floor(spot.z));
        if (cell_x < region.minimum_x() + 2 || cell_x > region.maximum_x() - 2 ||
            cell_z < region.minimum_z() + 2 || cell_z > region.maximum_z() - 2)
            return false;
        const int top = world::world_min_y + world::chunk_height - 1;
        int y = std::clamp(static_cast<int>(std::floor(spot.y)), world::world_min_y, top);
        while (y < top && world::blocks_player(region.block(cell_x, y, cell_z)))
            ++y;
        const int pocket_floor = y;
        while (y < top && !world::blocks_player(region.block(cell_x, y, cell_z)))
            ++y;
        if (y >= top || y - pocket_floor < 2)
            return false; // Open to the sky, or a pocket too small to stand in.
        if (!spawn_point_dark(cell_x, y - 1, cell_z))
            return false;
        return spawn_local_enemy(type,
                                 {static_cast<float>(cell_x) + 0.5F, static_cast<float>(y - 2),
                                  static_cast<float>(cell_z) + 0.5F},
                                 0.0F);
    }

    [[nodiscard]] bool try_spawn_enemy_on_ground(int type, world::Vec3 spot, float midnight)
    {
        const auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(spot.x));
        const int cell_z = static_cast<int>(std::floor(spot.z));
        if (cell_x < region.minimum_x() + 2 || cell_x > region.maximum_x() - 2 ||
            cell_z < region.minimum_z() + 2 || cell_z > region.maximum_z() - 2)
            return false;
        // Find the topmost stand position near the requested height.
        int ground_y = world::world_min_y;
        for (int y = world::world_min_y + world::chunk_height - 2; y > world::world_min_y; --y)
        {
            if (region.contains(cell_x, y, cell_z) &&
                world::blocks_player(region.block(cell_x, y, cell_z)))
            {
                ground_y = y + 1;
                break;
            }
        }
        if (ground_y <= world::world_min_y)
            return false;
        if (!spawn_point_dark(cell_x, ground_y, cell_z))
            return false;
        const world::Vec3 spawn_spot{static_cast<float>(cell_x) + 0.5F,
                                     static_cast<float>(ground_y),
                                     static_cast<float>(cell_z) + 0.5F};
        if (!spawn_local_enemy(type, spawn_spot, midnight))
            return false;
        // ZombieEmerge.Enter: aboveground families claw out of the ground with
        // the unearth scrape and their first cry.
        if (world::enemy_stats(type).habitat == world::EnemyHabitat::aboveground)
        {
            play_positional("CreatureUnearth", spawn_spot);
            play_positional("ZombieCry", spawn_spot);
        }
        return true;
    }

    [[nodiscard]] bool try_spawn_enemy_on_local_ground(int type, world::Vec3 spot, float midnight)
    {
        const auto &region = world_preview_->region();
        const int cell_x = static_cast<int>(std::floor(spot.x));
        const int cell_z = static_cast<int>(std::floor(spot.z));
        if (cell_x < region.minimum_x() + 2 || cell_x > region.maximum_x() - 2 ||
            cell_z < region.minimum_z() + 2 || cell_z > region.maximum_z() - 2)
            return false;

        const int top = world::world_min_y + world::chunk_height - 3;
        const int start_y =
            std::clamp(static_cast<int>(std::floor(spot.y)), world::world_min_y + 1, top);
        // Open descent terrain has no ceiling, so FindClosestCeiling correctly
        // fails there. Search only downward from the requested Hell-level spot;
        // unlike the surface helper this cannot jump the enemy to a remote
        // mountaintop above the player.
        for (int y = start_y; y > world::world_min_y; --y)
        {
            if (!world::blocks_player(region.block(cell_x, y, cell_z)) ||
                world::blocks_player(region.block(cell_x, y + 1, cell_z)) ||
                world::blocks_player(region.block(cell_x, y + 2, cell_z)))
                continue;
            if (!spawn_point_dark(cell_x, y + 1, cell_z))
                return false;
            const world::Vec3 spawn_spot{static_cast<float>(cell_x) + 0.5F,
                                         static_cast<float>(y + 1),
                                         static_cast<float>(cell_z) + 0.5F};
            return spawn_local_enemy(type, spawn_spot, midnight);
        }
        return false;
    }

    void place_preview_environment()
    {
        if (!options_.preview_environment.has_value() || *options_.preview_environment == "surface")
            return;

        auto &region = world_preview_->region();
        const std::string &environment = *options_.preview_environment;
        const int center_x = world_preview_->center_x();
        const int center_z = world_preview_->center_z();
        if (environment == "foliage")
        {
            // A stable close-range foliage/shadow probe. The ordinary world
            // generator remains untouched; this capture-only scene makes
            // alpha-pore, mip and dappled-shadow regressions obvious.
            const int feet_y = static_cast<int>(std::floor(player_.state().position.y));
            const int tree_x = center_x + 3;
            const int tree_z = center_z + 3;
            for (int z = tree_z - 4; z <= tree_z + 4; ++z)
                for (int x = tree_x - 4; x <= tree_x + 4; ++x)
                {
                    region.set_block(x, feet_y - 2, z, world::BlockType::dirt);
                    // A mid-grey neutral receiver keeps both sunlit and shaded
                    // values below the tone mapper's white shoulder, making the
                    // canopy's projected transmission legible in matched captures.
                    // This exists only in the explicit foliage diagnostic;
                    // generated worlds retain their authored surface placement.
                    region.set_block(x, feet_y - 1, z, world::BlockType::rock);
                    for (int y = feet_y; y <= feet_y + 8; ++y)
                        region.set_block(x, y, z, world::BlockType::empty);
                }
            for (int y = feet_y; y <= feet_y + 5; ++y)
                region.set_block(tree_x, y, tree_z, world::BlockType::wood);
            for (int y = feet_y + 3; y <= feet_y + 6; ++y)
            {
                const int radius = y == feet_y + 6 ? 1 : 2;
                for (int z = tree_z - radius; z <= tree_z + radius; ++z)
                    for (int x = tree_x - radius; x <= tree_x + radius; ++x)
                        if ((x != tree_x || z != tree_z || y > feet_y + 5) &&
                            (radius == 1 || std::abs(x - tree_x) + std::abs(z - tree_z) < 4))
                            region.set_block(x, y, z, world::BlockType::leaves);
            }
            region.set_block(tree_x, feet_y + 7, tree_z, world::BlockType::leaves);
            world_preview_->rebuild_mesh();
            return;
        }
        float best_score = std::numeric_limits<float>::max();
        std::optional<world::Vec3> best;
        std::optional<std::array<int, 3>> crater_pocket;
        float crater_pocket_score = std::numeric_limits<float>::max();
        int best_overburden{};
        int best_space_rock{};
        for (int z = region.minimum_z() + 2; z <= region.maximum_z() - 2; ++z)
        {
            for (int x = region.minimum_x() + 2; x <= region.maximum_x() - 2; ++x)
            {
                int overburden{};
                int space_rock_above{};
                for (int y = world::world_min_y + world::chunk_height - 1;
                     y > world::world_min_y + 1; --y)
                {
                    const world::BlockType cell = region.block(x, y, z);
                    if (world::blocks_player(cell))
                        ++overburden;
                    if (cell == world::BlockType::space_rock ||
                        cell == world::BlockType::space_rock_inventory)
                    {
                        ++space_rock_above;
                        // A newly found crater is solid until the player digs
                        // into it. Keep the nearest two-cell pocket that can be
                        // opened immediately below space rock for the explicit
                        // diagnostic capture only.
                        const int pocket_y = y - 2;
                        if (environment == "crater" && pocket_y > world::world_min_y + 1 &&
                            world::blocks_player(region.block(x, pocket_y - 1, z)))
                        {
                            const float pocket_dx = static_cast<float>(x - center_x);
                            const float pocket_dz = static_cast<float>(z - center_z);
                            const float pocket_score =
                                pocket_dx * pocket_dx + pocket_dz * pocket_dz;
                            if (pocket_score < crater_pocket_score)
                            {
                                crater_pocket_score = pocket_score;
                                crater_pocket = std::array{x, pocket_y, z};
                            }
                        }
                    }

                    // Empty feet/head cells with a full collision floor are a
                    // safe deterministic first-person position.
                    if (cell != world::BlockType::empty ||
                        region.block(x, y + 1, z) != world::BlockType::empty ||
                        !world::blocks_player(region.block(x, y - 1, z)))
                        continue;

                    bool eligible{};
                    float vertical_target{};
                    if (environment == "cave")
                    {
                        eligible = y > -37 && y <= 15 && overburden >= 5;
                        vertical_target = 0.0F;
                    }
                    else if (environment == "water")
                    {
                        eligible = y <= 0;
                        vertical_target = 0.0F;
                    }
                    else if (environment == "hell")
                    {
                        eligible = y <= -40;
                        vertical_target = -50.0F;
                    }
                    else if (environment == "lava")
                    {
                        // A dedicated parity probe for the global lava plane:
                        // keep ordinary hell captures above the red overlay.
                        eligible = y <= -61;
                        vertical_target = -61.0F;
                    }
                    else if (environment == "crater")
                    {
                        eligible = space_rock_above >= 1;
                        vertical_target = 5.0F;
                    }
                    if (!eligible)
                        continue;

                    const float dx = static_cast<float>(x - center_x);
                    const float dz = static_cast<float>(z - center_z);
                    const float dy = static_cast<float>(y) - vertical_target;
                    const float score =
                        dx * dx + dz * dz + dy * dy * 0.18F - static_cast<float>(overburden) * 0.4F;
                    if (score < best_score)
                    {
                        best_score = score;
                        best = world::Vec3{static_cast<float>(x) + 0.5F,
                                           static_cast<float>(y) +
                                               (environment == "water" ? 0.0F : 0.001F),
                                           static_cast<float>(z) + 0.5F};
                        best_overburden = overburden;
                        best_space_rock = space_rock_above;
                    }
                }
            }
        }
        bool opened_crater_pocket{};
        bool opened_lava_pocket{};
        bool opened_water_shelf{};
        if (!best.has_value() && environment == "crater" && crater_pocket.has_value())
        {
            const auto [x, y, z] = *crater_pocket;
            region.set_block(x, y, z, world::BlockType::empty);
            region.set_block(x, y + 1, z, world::BlockType::empty);
            best = world::Vec3{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.001F,
                               static_cast<float>(z) + 0.5F};
            best_overburden = 2;
            best_space_rock = 1;
            opened_crater_pocket = true;
            // Diagnostic capture is only two ready frames long, so rebuild
            // now instead of leaving its freshly opened pocket in the normal
            // frame-budgeted edit queue.
            world_preview_->rebuild_mesh();
        }
        if (!best.has_value() && environment == "lava")
        {
            // The generated bottom two layers can be entirely non-blocking
            // lava. The explicit diagnostic owns a small rock shelf and clear
            // standing volume so the -60..-62 overlay/damage band is always
            // reproducible without changing an ordinary world.
            constexpr int feet_y = -61;
            for (int z = center_z - 2; z <= center_z + 2; ++z)
            {
                for (int x = center_x - 2; x <= center_x + 2; ++x)
                {
                    region.set_block(x, feet_y - 1, z, world::BlockType::rock);
                    region.set_block(x, feet_y, z, world::BlockType::empty);
                    region.set_block(x, feet_y + 1, z, world::BlockType::empty);
                }
            }
            best = world::Vec3{static_cast<float>(center_x) + 0.5F,
                               static_cast<float>(feet_y) + 0.001F,
                               static_cast<float>(center_z) + 0.5F};
            best_overburden = 0;
            opened_lava_pocket = true;
            world_preview_->rebuild_mesh();
        }
        if (!best.has_value() && environment == "water")
        {
            // Guarantee feet depth 1.5 in the explicit Lagoon probe even when
            // its generated sea floor is deeper than the active collision
            // search can safely select.
            constexpr int feet_y = 0;
            for (int z = center_z - 4; z <= center_z + 4; ++z)
            {
                for (int x = center_x - 4; x <= center_x + 4; ++x)
                {
                    region.set_block(x, feet_y - 1, z, world::BlockType::rock);
                    region.set_block(x, feet_y, z, world::BlockType::empty);
                    region.set_block(x, feet_y + 1, z, world::BlockType::empty);
                }
            }
            best = world::Vec3{static_cast<float>(center_x) + 0.5F, static_cast<float>(feet_y),
                               static_cast<float>(center_z) + 0.5F};
            best_overburden = 0;
            opened_water_shelf = true;
            world_preview_->rebuild_mesh();
        }
        if (!best.has_value())
        {
            std::cerr << "No safe " << environment << " preview cell found in active region\n";
            return;
        }
        player_.state().position = *best;
        player_.state().velocity = {};
        player_.state().grounded = true;
        std::cout << "Environment: " << environment << " at " << best->x << ' ' << best->y << ' '
                  << best->z << " (overburden " << best_overburden << ", space rock "
                  << best_space_rock << (opened_crater_pocket ? ", opened crater pocket" : "")
                  << (opened_lava_pocket ? ", opened lava shelf" : "")
                  << (opened_water_shelf ? ", opened water shelf" : "") << ")\n";
    }

    void spawn_player()
    {
        int spawn_x = static_cast<int>(world::default_start_location.x);
        int spawn_z = static_cast<int>(world::default_start_location.z);
        if (pending_load_position_.has_value())
        {
            spawn_x = static_cast<int>(std::floor(pending_load_position_->x));
            spawn_z = static_cast<int>(std::floor(pending_load_position_->z));
        }
        else if (options_.preview_environment == std::optional<std::string>{"crater"})
        {
            // CrashSiteDepositer is disabled inside radius 300, so a short
            // origin-centred diagnostic window can never contain a crater.
            // Locate one from the same seeded field and synchronously recenter
            // before the ordinary safe-cell scan opens its capture pocket.
            const world::WorldGenerator generator{world_seed_};
            if (const auto crater = generator.find_crash_site_column(
                    world_preview_->center_x(), world_preview_->center_z(), 3600);
                crater.has_value())
            {
                spawn_x = (*crater)[0];
                spawn_z = (*crater)[1];
                world::PlayerState target;
                target.position = {static_cast<float>(spawn_x) + 0.5F, 32.0F,
                                   static_cast<float>(spawn_z) + 0.5F};
                world_preview_->recenter_on(target);
            }
        }
        else if (options_.world_biome != 0 || options_.preview_environment.has_value())
        {
            // Biome/environment command-line captures intentionally launch far
            // from the origin; ordinary worlds use WorldInfo's exact start.
            spawn_x = world_preview_->center_x() + 8;
            spawn_z = world_preview_->center_z() + 8;
        }
        player_.spawn(world_preview_->region(), spawn_x, spawn_z);
        place_preview_environment();
        // Establish the deterministic capture camera before placing any
        // explicitly requested enemy relative to it.
        player_.state().yaw = options_.world_yaw.value_or(2.3561945F);
        if (options_.preview_environment.has_value() && !options_.world_yaw.has_value())
        {
            const float toward_x =
                static_cast<float>(world_preview_->center_x()) - player_.state().position.x;
            const float toward_z =
                static_cast<float>(world_preview_->center_z()) - player_.state().position.z;
            player_.state().yaw = std::atan2(toward_x, -toward_z);
        }
        player_.state().pitch = options_.world_pitch.value_or(-0.30F);
        if (!distance_origin_set_)
        {
            // Legacy save fields remain readable, but the original measures
            // progression from global zero rather than the spawn-cell centre.
            distance_origin_x_ = 0.0F;
            distance_origin_z_ = 0.0F;
            distance_origin_set_ = true;
        }
        else
        {
            // ResetFarthestDistance: dying rolls the dragon ladder back to the
            // respawn position so already-passed thresholds can fire again.
            static constexpr std::array<float, 5> dragon_distances{100.0F, 500.0F, 1000.0F, 2600.0F,
                                                                   4000.0F};
            const float respawn_distance =
                std::hypot(player_.state().position.x, player_.state().position.z);
            while (dragon_distance_index_ > 0 &&
                   respawn_distance <=
                       dragon_distances[static_cast<std::size_t>(dragon_distance_index_ - 1)])
                --dragon_distance_index_;
        }
        zombie_fest_ = false;
        fest_countdown_ = -1.0F;
        if (options_.spawn_enemy >= 0 && !diagnostic_enemy_spawned_)
        {
            diagnostic_enemy_spawned_ = true;
            // Deterministic capture support: stand the requested enemy in the
            // final camera direction. Environment captures already selected a
            // safe floor, so keep the actor in that chamber instead of scanning
            // up to the unrelated surface above it.
            const world::Vec3 direction = player_.view_direction();
            const float horizontal_length = std::max(0.001F, std::hypot(direction.x, direction.z));
            const world::Vec3 ahead{
                player_.state().position.x + direction.x / horizontal_length * 4.5F,
                player_.state().position.y,
                player_.state().position.z + direction.z / horizontal_length * 4.5F};
            if (options_.preview_environment.has_value())
                static_cast<void>(spawn_local_enemy(options_.spawn_enemy, ahead, 1.0F));
            else if (!try_spawn_enemy_on_ground(options_.spawn_enemy, ahead, 1.0F))
                static_cast<void>(spawn_local_enemy(options_.spawn_enemy, ahead, 1.0F));
            if (options_.screenshot_path.has_value())
            {
                // The deterministic capture flow freezes simulation, so walk the
                // fresh enemy into a representative mid-chase pose here.
                for (int tick = 0; tick < 45; ++tick)
                    (void)enemy_horde_.update(world_preview_->region(), player_.state(),
                                              1.0F / 60.0F);
                // Preserve that animated pose but keep the actor on the diagnostic
                // camera ray. Terrain several blocks ahead can be far below a
                // cliff-top spawn and made a healthy rendered zombie look absent.
                if (!enemy_horde_.enemies().empty())
                {
                    world::Enemy &enemy = enemy_horde_.enemies().back();
                    enemy.position = ahead;
                    enemy.velocity = {};
                }
            }
        }
        if (options_.spawn_dragon >= 0 && !dragon_.active())
        {
            dragon_.spawn(static_cast<world::DragonKind>(options_.spawn_dragon),
                          player_.state().position, enemy_horde_.rng());
            if (options_.screenshot_path.has_value())
            {
                // Fly the dragon into camera range for the still frame.
                auto &mutable_dragon = const_cast<world::Dragon &>(dragon_.dragon());
                mutable_dragon.position = {player_.state().position.x + 34.0F,
                                           player_.state().position.y + 30.0F,
                                           player_.state().position.z + 34.0F};
                mutable_dragon.state = world::DragonState::strafe;
                mutable_dragon.clip = "fly_forward";
                mutable_dragon.yaw = -2.3F;
            }
        }
        const char *torch_debug = SDL_getenv("OPENMINERZ_TORCH_DEBUG");
        if (torch_debug != nullptr)
        {
            // A rock pillar with every torch orientation, for prop captures.
            const bool held_shadow_only = std::string_view{torch_debug} == "held-shadow";
            auto &region = world_preview_->region();
            const world::Vec3 view = player_.view_direction();
            const float horizontal = std::max(0.001F, std::hypot(view.x, view.z));
            const int base_x = static_cast<int>(
                std::floor(player_.state().position.x + view.x / horizontal * 4.0F));
            const int base_z = static_cast<int>(
                std::floor(player_.state().position.z + view.z / horizontal * 4.0F));
            const int base_y = static_cast<int>(std::floor(player_.state().position.y));
            // Generated caves can put the requested point inside a wall. Own a
            // small sealed pocket for this explicit diagnostic so every torch,
            // receiving face, and falloff band is visible and repeatable.
            for (int z = base_z - 3; z <= base_z + 3; ++z)
                for (int x = base_x - 3; x <= base_x + 3; ++x)
                {
                    region.set_block(x, base_y - 1, z, world::BlockType::rock);
                    for (int y = base_y; y <= base_y + 5; ++y)
                        region.set_block(x, y, z, world::BlockType::empty);
                }
            if (held_shadow_only)
            {
                const int forward_x =
                    std::abs(view.x) >= std::abs(view.z) ? (view.x >= 0.0F ? 1 : -1) : 0;
                const int forward_z = forward_x == 0 ? (view.z >= 0.0F ? 1 : -1) : 0;
                const int side_x = -forward_z;
                const int side_z = forward_x;
                const int wall_x = base_x + forward_x * 3;
                const int wall_z = base_z + forward_z * 3;
                // A receiving wall seven units from the eye makes geometric
                // held-light occlusion unambiguous: Classic stays radially lit,
                // while Rebirth shows the pillar's bounded point shadow.
                for (int side = -3; side <= 3; ++side)
                    for (int y = base_y; y <= base_y + 5; ++y)
                        region.set_block(wall_x + side_x * side, y, wall_z + side_z * side,
                                         world::BlockType::rock);
            }
            for (int y = base_y; y < base_y + 3; ++y)
                region.set_block(base_x, y, base_z, world::BlockType::rock);
            region.set_block(base_x + 1, base_y + 4, base_z, world::BlockType::rock);
            if (!held_shadow_only)
            {
                region.set_block(base_x + 1, base_y + 1, base_z, world::BlockType::torch_neg_x);
                region.set_block(base_x - 1, base_y + 1, base_z, world::BlockType::torch_pos_x);
                region.set_block(base_x, base_y + 1, base_z + 1, world::BlockType::torch_neg_z);
                region.set_block(base_x, base_y + 1, base_z - 1, world::BlockType::torch_pos_z);
                region.set_block(base_x + 2, base_y, base_z + 2, world::BlockType::torch);
                region.set_block(base_x + 1, base_y + 3, base_z, world::BlockType::torch_neg_y);
            }
            // A two-frame screenshot exits before the normal asynchronous edit
            // queue can install the relit mesh. Diagnostic stills own this tiny
            // fixture, so make its placed-light bake synchronous; live sessions
            // retain the bounded edit path.
            if (options_.screenshot_path.has_value())
                world_preview_->rebuild_mesh();
            else
                world_preview_->rebuild_block_edit(base_x, base_y + 1, base_z);
        }
        const char *radiance_capture = SDL_getenv("OPENMINERZ_DYNAMIC_RADIANCE_CAPTURE");
        if (radiance_capture != nullptr && !diagnostic_dynamic_radiance_spawned_)
        {
            diagnostic_dynamic_radiance_spawned_ = true;
            const std::string_view mode{radiance_capture};
            const bool ice = mode == "iceball" || mode == "ice-blast";
            const world::Vec3 view = player_.view_direction();
            const float horizontal = std::max(0.001F, std::hypot(view.x, view.z));
            const bool projectile_capture =
                mode == "fireball" || mode == "iceball" || mode == "rocket";
            const float forward_distance = projectile_capture ? 4.5F : 3.25F;
            const float side_distance = projectile_capture ? 1.6F : 0.0F;
            const world::Vec3 source{
                player_.state().position.x + view.x / horizontal * forward_distance -
                    view.z / horizontal * side_distance,
                player_.state().position.y + world::PlayerSimulation::eye_height +
                    view.y / horizontal * forward_distance,
                player_.state().position.z + view.z / horizontal * forward_distance +
                    view.x / horizontal * side_distance};
            if (mode == "fireball" || mode == "iceball")
            {
                const world::Vec3 drift{view.x / horizontal * 0.01F, 0.0F,
                                        view.z / horizontal * 0.01F};
                const int fireball_id = next_fireball_id_++;
                fireballs_.push_back({source, drift,
                                      ice ? world::DragonKind::ice : world::DragonKind::fire,
                                      fireball_id, true, false});
                if (options_.screenshot_path.has_value())
                {
                    // Two-frame stills otherwise exit before the rate-based
                    // local FireGlow/FireBallEffect and trail emitters accrue a
                    // particle. Prime exactly 0.18 s only in this diagnostic so
                    // the capture audits the projectile's real blend recipes.
                    world_preview_->set_fireball_visuals({{source, drift, 0.0F, fireball_id, ice}});
                    (void)world_preview_->update_effects(0.18F, player_.state().position);
                }
            }
            else if (mode == "fire-blast" || mode == "ice-blast")
            {
                world_preview_->spawn_fireball_explosion(source, ice, false);
            }
            else if (mode == "mining-impact")
            {
                world::BlockBreakUpdate impact;
                impact.intersection = source;
                impact.block = world::BlockType::rock;
                if (std::abs(view.x) >= std::abs(view.z))
                    impact.normal_x = view.x >= 0.0F ? -1 : 1;
                else
                    impact.normal_z = view.z >= 0.0F ? -1 : 1;
                world_preview_->spawn_mining_impact(impact);
                if (options_.screenshot_path.has_value())
                {
                    // A two-frame still otherwise catches expanding smoke at
                    // its centimetre-scale birth size. Prime only the diagnostic
                    // capture to the readable middle of the real live recipe.
                    for (int step = 0; step < 3; ++step)
                        (void)world_preview_->update_effects(0.1F, player_.state().position);
                }
            }
            else if (mode == "enemy-impact")
            {
                world_preview_->spawn_impact_smoke(source, {1.0F, 1.0F, 1.0F});
                if (options_.screenshot_path.has_value())
                {
                    for (int step = 0; step < 3; ++step)
                        (void)world_preview_->update_effects(0.1F, player_.state().position);
                }
            }
            else if (mode == "rocket")
            {
                const world::Vec3 direction{view.x / horizontal, 0.0F, view.z / horizontal};
                Tracer rocket;
                rocket.head = source;
                rocket.tail = source;
                rocket.time_left = 10.25F;
                rocket.flight_time = 10.25F;
                rocket.item = InventoryItemId::rocket_launcher;
                rocket.id = next_projectile_id_++;
                rocket.rocket_direction = direction;
                rocket.rocket_emitted_direction = direction;
                rocket.rocket_start = source;
                rocket.rocket_guided_position = source;
                rocket.rocket_starting_velocity = {};
                rocket.rocket = true;
                tracers_.push_back(rocket);
            }
        }
        // A loaded save (or this Steam player's per-world join record) restores
        // exactly where the player left off, unless later world edits made that
        // volume solid. In that case the terrain-safe spawn chosen above wins.
        if (pending_load_position_.has_value())
        {
            if (!world::PlayerSimulation::touches_solid(world_preview_->region(),
                                                        *pending_load_position_))
            {
                player_.state().position = *pending_load_position_;
                player_.state().yaw = pending_load_yaw_;
                player_.state().pitch = pending_load_pitch_;
                player_.state().health = pending_load_health_;
            }
            else if (pending_load_is_online_)
            {
                toast("SAVED POSITION OBSTRUCTED",
                      "THE SERVER WORLD CHANGED  /  USING A SAFE SPAWN");
            }
            pending_load_position_.reset();
            pending_load_is_online_ = false;
        }
        online_player_update_timer_ = 0.0F;
        sync_selected_item();
        player_.refresh_target(world_preview_->region());
        schedule_authoritative_chunk_check(
            {static_cast<int>(std::floor(player_.state().position.x)),
             static_cast<int>(std::floor(player_.state().position.y)),
             static_cast<int>(std::floor(player_.state().position.z))});
    }

    void shift_world_time(int hours)
    {
        day_night_.set_time(day_night_.time_of_day() + static_cast<float>(hours) / 24.0F);
        toast(hours > 0 ? "TIME ADVANCED" : "TIME REWOUND",
              std::format("SKY AND LIGHTING UPDATED  /  {}", day_night_.clock_text()));
    }

    void cycle_world_speed()
    {
        constexpr std::array speeds{0.0F, 1.0F, 4.0F, 24.0F};
        const auto iterator = std::find(speeds.begin(), speeds.end(), world_time_speed_);
        const std::size_t next =
            iterator == speeds.end()
                ? 0U
                : (static_cast<std::size_t>(iterator - speeds.begin()) + 1U) % speeds.size();
        world_time_speed_ = speeds[next];
        toast(world_time_speed_ == 0.0F ? "DAY CYCLE PAUSED" : "DAY CYCLE SPEED",
              world_time_speed_ == 0.0F
                  ? "PRESS T TO RESUME"
                  : std::format("{}X  /  ORIGINAL DAY LENGTH: 16 MINUTES", world_time_speed_));
    }

    void process_gamepad(Uint8 button)
    {
        // Never mix a controller's button stream into an active keyboard/mouse
        // session. Steam Input and other remappers can expose one physical mouse
        // click twice (RMB plus virtual A), while a noisy attached pad can emit
        // shoulders/reload and even large analogue values on its own. A real
        // controller explicitly claims ownership with Start/Menu; any subsequent
        // mouse or keyboard event immediately hands ownership back.
        if (!gamepad_input_active_)
        {
            if (button == SDL_GAMEPAD_BUTTON_START && !mouse_suppresses_gamepad_actions())
            {
                gamepad_input_active_ = true;
                gamepad_use_down_ = false;
                gamepad_activate_down_ = false;
                if (menu_.screen() == ui::MenuScreen::world_preview)
                    toast("CONTROLLER ACTIVE", "MOVE THE MOUSE OR PRESS A KEY TO RETURN TO KB/M");
            }
            return;
        }
        if (mouse_suppresses_gamepad_actions())
            return;
        if (host_text_field_ != HostTextField::none)
        {
            if (button == SDL_GAMEPAD_BUTTON_SOUTH)
                finish_host_text_edit(true);
            else if (button == SDL_GAMEPAD_BUTTON_EAST)
                finish_host_text_edit(false);
            return;
        }
        if (locator_name_editing_)
        {
            if (button == SDL_GAMEPAD_BUTTON_SOUTH)
                finish_locator_name_edit(true);
            else if (button == SDL_GAMEPAD_BUTTON_EAST)
                finish_locator_name_edit(false);
            return;
        }
        if (menu_.screen() == ui::MenuScreen::world_preview)
        {
            if (waiting_for_world_)
            {
                if (button == SDL_GAMEPAD_BUTTON_START)
                {
                    cancel_world_action();
                    SDL_SetWindowRelativeMouseMode(window_, false);
                    menu_.open(ui::MenuScreen::pause);
                }
                return;
            }
            if (inventory_open_)
            {
                switch (button)
                {
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    move_inventory_focus(-1, 0);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    move_inventory_focus(1, 0);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    move_inventory_focus(0, -1);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    move_inventory_focus(0, 1);
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH: {
                    const Rect rect = inventory_slot_rect(inventory_focus_);
                    inventory_pointer(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F,
                                      SDL_BUTTON_LEFT);
                    break;
                }
                case SDL_GAMEPAD_BUTTON_WEST:
                    // BlockPickerScreen/CrateScreen bind X to the same whole-stack
                    // drop action as keyboard Q while an inventory screen is open.
                    drop_inventory_selection();
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                    close_inventory();
                    break;
                case SDL_GAMEPAD_BUTTON_NORTH:
                    if (active_crate_.has_value())
                    {
                        if (quick_move_inventory_slot(inventory_focus_))
                            sound_bank_->play_click();
                    }
                    else
                        close_inventory();
                    break;
                default:
                    break;
                }
                return;
            }
            switch (button)
            {
            case SDL_GAMEPAD_BUTTON_START:
                cancel_world_action();
                SDL_SetWindowRelativeMouseMode(window_, false);
                menu_.open(ui::MenuScreen::pause);
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                // Function 8 / B is Activate only. Left trigger supplies the
                // shared Activate+Shoulder path in poll_gamepad below.
                (void)try_activate_target();
                break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                select_hotbar(-1);
                break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                select_hotbar(1);
                break;
            case SDL_GAMEPAD_BUTTON_WEST:
                if (inventory_.selected_item() == InventoryItemId::gps ||
                    inventory_.selected_item() == InventoryItemId::teleport_gps)
                    begin_locator_name_edit();
                else
                    begin_gun_reload();
                break;
            case SDL_GAMEPAD_BUTTON_NORTH:
                inventory_open_ = true;
                inventory_focus_ = PlayerInventory::storage_size + inventory_.selected_index();
                cancel_world_action();
                SDL_SetWindowRelativeMouseMode(window_, false);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                drop_one_selected_item();
                break;
            default:
                break;
            }
            return;
        }
        switch (button)
        {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            navigate_menu(0, -1);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            navigate_menu(0, 1);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            navigate_menu(-1, 0);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            navigate_menu(1, 0);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            handle(menu_.activate(settings_));
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            handle(menu_.back());
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            menu_.change_tab(-1, settings_);
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            menu_.change_tab(1, settings_);
            break;
        default:
            break;
        }
    }

    void open_first_gamepad()
    {
        if (gamepad_ != nullptr)
            return;
        int count{};
        SDL_JoystickID *identifiers = SDL_GetGamepads(&count);
        if (identifiers != nullptr && count > 0)
            gamepad_ = SDL_OpenGamepad(identifiers[0]);
        SDL_free(identifiers);
    }

    void note_mouse_activity() noexcept
    {
        last_mouse_activity_ticks_ = SDL_GetTicks();
        gamepad_input_active_ = false;
    }

    void note_keyboard_activity() noexcept
    {
        gamepad_input_active_ = false;
    }

    void note_mouse_button(Uint8 button, bool down) noexcept
    {
        note_mouse_activity();
        if (button == SDL_BUTTON_LEFT)
            mouse_primary_down_ = down;
        else if (button == SDL_BUTTON_RIGHT)
            mouse_secondary_down_ = down;
    }

    [[nodiscard]] bool mouse_suppresses_gamepad_actions() const noexcept
    {
        constexpr Uint64 mouse_priority_milliseconds = 250U;
        const Uint64 now = SDL_GetTicks();
        const Uint64 since_mouse = now >= last_mouse_activity_ticks_
                                       ? now - last_mouse_activity_ticks_
                                       : mouse_priority_milliseconds;
        return mouse_primary_down_ || mouse_secondary_down_ ||
               since_mouse < mouse_priority_milliseconds;
    }

    void poll_gamepad(world::PlayerInput &input, float delta)
    {
        if (gamepad_ == nullptr || text_editing() || !gamepad_input_active_)
            return;
        const auto stick = [](Sint16 value) {
            constexpr float dead_zone = 0.18F;
            const float raw = std::clamp(static_cast<float>(value) / 32767.0F, -1.0F, 1.0F);
            const float magnitude = std::abs(raw);
            if (magnitude <= dead_zone)
                return 0.0F;
            return std::copysign((magnitude - dead_zone) / (1.0F - dead_zone), raw);
        };
        input.strafe =
            std::clamp(input.strafe + stick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX)),
                       -1.0F, 1.0F);
        input.forward =
            std::clamp(input.forward - stick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY)),
                       -1.0F, 1.0F);
        const bool accept_gamepad_actions = !mouse_suppresses_gamepad_actions();
        input.jump = input.jump || (accept_gamepad_actions &&
                                    SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_SOUTH));
        const bool left_stick =
            accept_gamepad_actions && SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        if (player_.state().fly_mode)
            input.descend = input.descend || left_stick;
        else if (inventory_.profile() == InventoryProfile::modern &&
                 game_mode_ != GameMode::endurance && game_mode_ != GameMode::dragon_endurance)
            input.sprint = input.sprint || left_stick;

        const float aim_x = stick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTX));
        const float aim_y = stick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTY));
        constexpr float radians_per_second = 2.65F;
        player_.look(aim_x * settings_.controller_sensitivity * radians_per_second * delta,
                     -aim_y * settings_.controller_sensitivity * radians_per_second * delta *
                         (settings_.invert_y ? -1.0F : 1.0F));

        constexpr Sint16 trigger_threshold = 12000;
        const bool use =
            accept_gamepad_actions &&
            SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > trigger_threshold;
        if (use && !gamepad_use_down_)
            primary_action_pressed();
        if (!use && gamepad_use_down_)
        {
            if (grenade_cooking_)
                throw_grenade();
            mine_button_down_ = false;
        }
        gamepad_use_down_ = use;

        const bool activate =
            accept_gamepad_actions &&
            SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > trigger_threshold;
        if (activate && !gamepad_activate_down_)
        {
            (void)try_activate_target();
            if (inventory_.selected_item() == InventoryItemId::teleport_gps)
                (void)teleport_to_selected_waypoint();
        }
        if (selected_gun() != nullptr)
            shouldering_ = activate;
        else
        {
            const InventoryItemId item = inventory_.selected_item();
            secondary_action_down_ =
                activate && item != InventoryItemId::gps && item != InventoryItemId::teleport_gps;
        }
        gamepad_activate_down_ = activate;
    }

    // Push the display pipeline settings into the renderer.
    void apply_grading_settings()
    {
        if (world_preview_ == nullptr)
            return;
        world_preview_->set_grading(static_cast<int>(settings_.tone_mapping),
                                    settings_.auto_exposure, settings_.exposure_compensation,
                                    settings_.color_grading);
    }

    [[nodiscard]] static const char *join_filter_label(net::DiscoveryFilter filter) noexcept
    {
        switch (filter)
        {
        case net::DiscoveryFilter::all:
            return "ALL";
        case net::DiscoveryFilter::friends:
            return "FRIENDS";
        case net::DiscoveryFilter::public_lobbies:
            return "PUBLIC";
        case net::DiscoveryFilter::lan:
            return "LAN";
        case net::DiscoveryFilter::direct:
            return "DIRECT";
        }
        return "ALL";
    }

    [[nodiscard]] std::vector<net::DiscoveredSession> diagnostic_join_sessions() const
    {
        net::HostSessionSettings classic;
        classic.server_name = "THE DEEP FRONTIER";
        classic.server_message = "DAY 12  •  FRIENDS WELCOME";
        classic.maximum_players = 8;
        classic.visibility = net::HostVisibility::friends_only;

        net::DiscoveredSession friend_session;
        friend_session.session_id = 0x1001U;
        friend_session.server_name = classic.server_name;
        friend_session.host_name = "RUSTY PICKAXE";
        friend_session.server_message = classic.server_message;
        friend_session.source = net::DiscoverySource::friend_session;
        friend_session.properties = net::make_session_properties(classic, true, 1, 2, false);
        friend_session.current_players = 3U;
        friend_session.maximum_players = 8U;
        friend_session.favorite = true;
        friend_session.ping_milliseconds = 24U;

        net::DiscoveredSession password_session = friend_session;
        password_session.session_id = 0x1002U;
        password_session.server_name = "FORT IRONWOOD";
        password_session.host_name = "NIGHT SHIFT";
        password_session.server_message = "HARDCORE  •  PASSWORD REQUIRED";
        password_session.source = net::DiscoverySource::public_lobby;
        password_session.current_players = 6U;
        password_session.maximum_players = 12U;
        password_session.password_required = true;
        password_session.favorite = false;
        password_session.ping_milliseconds = 52U;

        net::HostSessionSettings modern;
        modern.protocol = net::ProtocolVersion::modern_v4;
        modern.mod_manifest.protocol = modern.protocol;
        modern.server_name = "REBIRTH TEST RANGE";
        modern.server_message = "MODERN CORE PROFILE";
        modern.maximum_players = 16;
        modern.visibility = net::HostVisibility::public_lobby;
        net::DiscoveredSession modern_session;
        modern_session.session_id = 0x1003U;
        modern_session.server_name = modern.server_name;
        modern_session.host_name = "ORACLE";
        modern_session.server_message = modern.server_message;
        modern_session.source = net::DiscoverySource::public_lobby;
        modern_session.properties = net::make_session_properties(modern, false, 1, 2, false);
        modern_session.current_players = 2U;
        modern_session.maximum_players = 16U;
        modern_session.ping_milliseconds = 71U;

        net::DiscoveredSession full_session = friend_session;
        full_session.session_id = 0x1004U;
        full_session.server_name = "LAN BENCH WORLD";
        full_session.host_name = "LOCAL HOST";
        full_session.server_message = "PRIVATE COMPATIBILITY SOAK";
        full_session.source = net::DiscoverySource::lan;
        full_session.current_players = 4U;
        full_session.maximum_players = 4U;
        full_session.favorite = false;
        full_session.ping_milliseconds = 1U;
        return {std::move(friend_session), std::move(password_session), std::move(modern_session),
                std::move(full_session)};
    }

    [[nodiscard]] net::JoinAssessment browser_join_assessment(std::size_t session_index) const
    {
        const auto &sessions = join_browser_.sessions();
        if (session_index >= sessions.size())
            return join_browser_.assessment(session_index);

        const net::DiscoveredSession &session = sessions[session_index];
        if (!session.properties[0].has_value() ||
            (*session.properties[0] !=
                 static_cast<std::int32_t>(net::ProtocolVersion::classic_v3) &&
             *session.properties[0] != static_cast<std::int32_t>(net::ProtocolVersion::modern_v4)))
            return join_browser_.assessment(session_index);

        const auto protocol = static_cast<net::ProtocolVersion>(*session.properties[0]);
        net::ModManifest protocol_manifest;
        std::string manifest_error;
        if (!mod_catalog_valid_ ||
            !net::make_mod_manifest(mod_catalog_, protocol, protocol_manifest, manifest_error))
        {
            protocol_manifest.protocol = protocol;
        }
        return net::assess_join(session, protocol_manifest, true);
    }

    void rebuild_join_menu_entries()
    {
        const auto &visible = join_browser_.visible_indices();
        const auto &sessions = join_browser_.sessions();
        join_labels_.clear();
        join_descriptions_.clear();
        join_badges_.clear();
        join_row_sessions_.clear();
        join_labels_.reserve(visible.size());
        join_descriptions_.reserve(visible.size());
        join_badges_.reserve(visible.size());
        join_row_sessions_.reserve(visible.size());
        for (const std::size_t session_index : visible)
        {
            const net::DiscoveredSession &session = sessions[session_index];
            const net::JoinAssessment assessment = browser_join_assessment(session_index);
            join_labels_.push_back(session.server_name);
            join_descriptions_.push_back(
                std::format("HOST {}  •  {}", session.host_name, session.server_message));
            join_badges_.push_back(
                std::format("{}  {}/{}  {}", net::join_compatibility_label(assessment.status),
                            session.current_players, session.maximum_players,
                            session.ping_milliseconds.has_value()
                                ? std::format("{} MS", *session.ping_milliseconds)
                                : std::string{"-- MS"}));
            join_row_sessions_.push_back(session_index);
        }

        std::vector<ui::MenuEntry> entries;
        entries.reserve(visible.size() + 4U);
        for (std::size_t index = 0U; index < visible.size(); ++index)
        {
            entries.push_back({join_labels_[index], join_descriptions_[index],
                               ui::MenuCommand::select_join_session, true, join_badges_[index]});
        }
        entries.push_back({"REFRESH", "Refresh friend, public, LAN, and direct session sources.",
                           ui::MenuCommand::refresh_join_sessions, true, "F5"});
        entries.push_back({"FILTER", "Cycle the active discovery source tab.",
                           ui::MenuCommand::cycle_join_filter, true,
                           join_filter_label(join_browser_.options().filter)});
        entries.push_back({"DIRECT CONNECT",
                           "Enter a bounded host or IP endpoint manually.",
                           ui::MenuCommand::direct_connect,
                           true,
                           {}});
        entries.push_back({"BACK", "Return to the main menu.", ui::MenuCommand::back, true, {}});
        menu_.set_join_entries(std::move(entries));
    }

    void refresh_join_sessions(bool diagnostic = false, bool quiet = false)
    {
        if (diagnostic)
        {
            join_browser_.replace(diagnostic_join_sessions());
            join_browser_state_ = JoinBrowserState::diagnostic;
            join_browser_status_ = "DETERMINISTIC UI FIXTURES  /  NOT LIVE STEAM SERVERS";
            selected_join_session_.reset();
            join_password_.clear();
            rebuild_join_menu_entries();
            return;
        }
        join_browser_state_ = JoinBrowserState::searching;
        join_browser_status_ = join_browser_.sessions().empty()
                                   ? "AUTHENTICATING WITH STEAM AND REQUESTING LIVE LOBBIES"
                                   : "REFRESHING LIVE STEAM LOBBIES  /  CURRENT RESULTS PRESERVED";
        selected_join_session_.reset();
        join_password_.clear();
        rebuild_join_menu_entries();
        begin_online_discovery(quiet);
    }

    void cycle_join_filter(int direction)
    {
        net::SessionBrowserOptions options = join_browser_.options();
        constexpr int count = 5;
        const int current = static_cast<int>(options.filter);
        const int step = direction < 0 ? -1 : 1;
        options.filter = static_cast<net::DiscoveryFilter>((current + step + count) % count);
        join_browser_.set_options(std::move(options));
        rebuild_join_menu_entries();
        sound_bank_->play_click();
    }

    [[nodiscard]] const net::DiscoveredSession *selected_join_session() const noexcept
    {
        if (!selected_join_session_.has_value() ||
            *selected_join_session_ >= join_browser_.sessions().size())
            return nullptr;
        return &join_browser_.sessions()[*selected_join_session_];
    }

    [[nodiscard]] std::string host_setting_value(ui::MenuCommand command) const
    {
        switch (command)
        {
        case ui::MenuCommand::edit_host_name:
            return host_settings_.server_name;
        case ui::MenuCommand::select_host_protocol:
            return net::protocol_label(host_settings_.protocol);
        case ui::MenuCommand::select_host_max_players:
            return std::format("{} PLAYERS", host_settings_.maximum_players);
        case ui::MenuCommand::select_host_visibility:
            return net::visibility_label(host_settings_.visibility);
        case ui::MenuCommand::edit_host_password:
            return host_settings_.password.empty() ? "NOT SET" : "SET";
        case ui::MenuCommand::select_host_join_policy:
            return net::join_policy_label(host_settings_.join_policy);
        case ui::MenuCommand::select_host_pvp:
            return net::pvp_label(host_settings_.player_versus_player);
        case ui::MenuCommand::toggle_host_anticheat:
            return host_settings_.anti_cheat ? "STRICT" : "OFF";
        case ui::MenuCommand::toggle_host_join_safety:
            return host_settings_.dedicated_join_safety ? "ON" : "SOURCE EXACT";
        case ui::MenuCommand::select_host_spawn_protection:
            return host_settings_.spawn_protection_radius == 0
                       ? "OFF"
                       : std::format("{} BLOCKS", host_settings_.spawn_protection_radius);
        case ui::MenuCommand::review_host_mods: {
            const std::size_t synchronized = static_cast<std::size_t>(std::count_if(
                host_settings_.mod_manifest.mods.begin(), host_settings_.mod_manifest.mods.end(),
                [](const net::ModDescriptor &mod) { return mod.scope != net::ModScope::client; }));
            return synchronized == 0U ? "VANILLA" : std::format("{} REQUIRED", synchronized);
        }
        case ui::MenuCommand::confirm_host_settings:
            return "READY";
        default:
            return {};
        }
    }

    void adjust_host_setting(int direction)
    {
        const ui::MenuScreen screen = menu_.screen();
        const bool settings_screen = screen == ui::MenuScreen::host_details ||
                                     screen == ui::MenuScreen::host_access ||
                                     screen == ui::MenuScreen::host_gameplay;
        if (!settings_screen || menu_.entries().empty())
            return;
        const ui::MenuCommand command = menu_.entries()[menu_.selection()].command;
        const int step = direction < 0 ? -1 : 1;
        switch (command)
        {
        case ui::MenuCommand::select_host_protocol:
            host_settings_.protocol = host_settings_.protocol == net::ProtocolVersion::classic_v3
                                          ? net::ProtocolVersion::modern_v4
                                          : net::ProtocolVersion::classic_v3;
            if (mod_catalog_valid_)
            {
                std::string ignored;
                mod_catalog_valid_ = net::make_mod_manifest(mod_catalog_, host_settings_.protocol,
                                                            host_settings_.mod_manifest, ignored);
                if (!mod_catalog_valid_)
                    mod_catalog_error_ = std::move(ignored);
            }
            break;
        case ui::MenuCommand::select_host_max_players:
            host_settings_.maximum_players =
                std::clamp(host_settings_.maximum_players + step, 1, 16);
            break;
        case ui::MenuCommand::select_host_visibility: {
            constexpr int count = 4;
            const int current = static_cast<int>(host_settings_.visibility);
            host_settings_.visibility =
                static_cast<net::HostVisibility>((current + step + count) % count);
            break;
        }
        case ui::MenuCommand::select_host_join_policy: {
            constexpr int count = 3;
            const int current = static_cast<int>(host_settings_.join_policy);
            host_settings_.join_policy =
                static_cast<net::HostJoinPolicy>((current + step + count) % count);
            break;
        }
        case ui::MenuCommand::select_host_pvp: {
            constexpr int count = 3;
            const int current = static_cast<int>(host_settings_.player_versus_player);
            host_settings_.player_versus_player =
                static_cast<net::PlayerVersusPlayer>((current + step + count) % count);
            break;
        }
        case ui::MenuCommand::toggle_host_anticheat:
            host_settings_.anti_cheat = !host_settings_.anti_cheat;
            break;
        case ui::MenuCommand::toggle_host_join_safety:
            host_settings_.dedicated_join_safety = !host_settings_.dedicated_join_safety;
            break;
        case ui::MenuCommand::select_host_spawn_protection:
            host_settings_.spawn_protection_radius =
                std::clamp(host_settings_.spawn_protection_radius + step * 4, 0, 128);
            break;
        default:
            return;
        }
        sound_bank_->play_click();
    }

    void navigate_menu(int horizontal, int vertical)
    {
        const ui::MenuScreen screen = menu_.screen();
        std::size_t selection = menu_.selection();
        if (screen == ui::MenuScreen::main)
        {
            if (horizontal != 0)
            {
                if (selection < 2U)
                    selection = selection == 0U ? 1U : 0U;
                else
                    selection = 2U + (selection - 2U + (horizontal < 0 ? 3U : 1U)) % 4U;
            }
            else if (vertical != 0)
            {
                selection =
                    selection < 2U ? (selection == 0U ? 2U : 4U) : (selection < 4U ? 0U : 1U);
            }
            menu_.set_selection(selection, settings_);
            return;
        }
        if (screen == ui::MenuScreen::multiplayer)
        {
            if (horizontal != 0 && selection < 2U)
                selection = selection == 0U ? 1U : 0U;
            else if (vertical != 0)
                selection = selection < 2U ? 2U : 0U;
            menu_.set_selection(selection, settings_);
            return;
        }
        if (screen == ui::MenuScreen::host_setup)
        {
            if (horizontal != 0)
            {
                if (selection < 4U)
                    selection = selection % 2U == 0U ? selection + 1U : selection - 1U;
                else
                    selection = selection == 4U ? 5U : 4U;
            }
            else if (vertical < 0)
            {
                if (selection >= 4U)
                    selection = selection == 4U ? 2U : 3U;
                else if (selection >= 2U)
                    selection -= 2U;
            }
            else if (vertical > 0)
            {
                if (selection < 2U)
                    selection += 2U;
                else if (selection < 4U)
                    selection = selection == 2U ? 4U : 5U;
            }
            menu_.set_selection(selection, settings_);
            return;
        }
        if (screen == ui::MenuScreen::world_select && !menu_.entries().empty())
        {
            const std::size_t back = menu_.entries().size() - 1U;
            const std::size_t content = back;
            if (content == 0U)
                return;
            if (horizontal != 0)
            {
                if (selection == back)
                    selection = horizontal < 0 ? content - 1U : 0U;
                else
                {
                    const std::size_t row_start = selection / 3U * 3U;
                    const std::size_t row_end = std::min(row_start + 3U, content);
                    const std::size_t row_size = row_end - row_start;
                    selection =
                        row_start +
                        (selection - row_start + (horizontal < 0 ? row_size - 1U : 1U)) % row_size;
                }
            }
            else if (vertical < 0)
            {
                if (selection == back)
                    selection = content - 1U;
                else if (selection >= 3U)
                    selection -= 3U;
            }
            else if (vertical > 0)
            {
                selection = selection != back && selection + 3U < content ? selection + 3U : back;
            }
            menu_.set_selection(selection, settings_);
            return;
        }
        if (screen == ui::MenuScreen::join_browser && !menu_.entries().empty())
        {
            const std::size_t session_count = join_row_sessions_.size();
            const std::size_t action_count = menu_.entries().size() - session_count;
            if (selection >= session_count)
            {
                std::size_t action = selection - session_count;
                if (horizontal != 0)
                    action = (action + (horizontal < 0 ? action_count - 1U : 1U)) % action_count;
                else if (vertical > 0 && session_count > 0U)
                    selection = 0U;
                if (horizontal != 0)
                    selection = session_count + action;
            }
            else if (vertical < 0)
                selection = selection == 0U ? session_count : selection - 1U;
            else if (vertical > 0)
                selection = selection + 1U < session_count ? selection + 1U : session_count;
            menu_.set_selection(selection, settings_);
            return;
        }
        if (vertical != 0)
            menu_.select_next(vertical, settings_);
        else if (horizontal != 0)
            adjust(horizontal);
    }

    void adjust(int direction)
    {
        if (menu_.screen() == ui::MenuScreen::join_browser && !menu_.entries().empty() &&
            menu_.entries()[menu_.selection()].command == ui::MenuCommand::cycle_join_filter)
        {
            cycle_join_filter(direction);
            return;
        }
        if (menu_.screen() == ui::MenuScreen::host_details ||
            menu_.screen() == ui::MenuScreen::host_access ||
            menu_.screen() == ui::MenuScreen::host_gameplay)
        {
            adjust_host_setting(direction);
            return;
        }
        if (menu_.screen() == ui::MenuScreen::settings)
        {
            const auto &rows = ui::rows_for_tab(menu_.settings_tab());
            const ui::SettingId id = rows[menu_.selection()].id;
            handle(menu_.adjust_selected(settings_, direction));
            dirty_ = true;
            dirty_timer_ = 0.45F;
            SDL_SetRenderVSync(renderer_, settings_.vsync ? 1 : 0);
            if (id == ui::SettingId::view_distance)
            {
                world_preview_->set_view_distance(settings_.view_distance);
            }
            if (id == ui::SettingId::particle_quality)
            {
                world_preview_->set_particle_density(particle_density(settings_.particle_quality));
            }
            if (id == ui::SettingId::tone_mapping || id == ui::SettingId::auto_exposure ||
                id == ui::SettingId::exposure_compensation || id == ui::SettingId::color_grading)
            {
                apply_grading_settings();
            }
            if (id == ui::SettingId::master_volume || id == ui::SettingId::music_volume ||
                id == ui::SettingId::effects_volume || id == ui::SettingId::ambience_volume ||
                id == ui::SettingId::mute_unfocused)
            {
                apply_audio_settings();
            }
            if (id == ui::SettingId::raw_mouse_input)
            {
                SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,
                            settings_.raw_mouse_input ? "0" : "1");
            }
            if (id == ui::SettingId::window_mode || id == ui::SettingId::resolution)
            {
                apply_window_settings();
            }
            if (id == ui::SettingId::texture_style)
            {
                toast("RESTART REQUIRED",
                      std::format("{} ASSET PACK WILL LOAD ON NEXT START",
                                  settings_.texture_style == AssetStyle::classic ? "CLASSIC"
                                                                                 : "MODERN"));
            }
            if (id == ui::SettingId::audio_output)
            {
                cycle_audio_output(direction);
                toast("RESTART REQUIRED",
                      std::format("AUDIO WILL USE {} ON NEXT START", settings_.audio_output_name));
            }
        }
    }

    void begin_offline_world()
    {
        settings_.game_mode = static_cast<int>(game_mode_);
        settings_.game_difficulty = static_cast<int>(difficulty_);
        dirty_ = true;
        apply_difficulty_loadout();
        sound_bank_->stop_music();
        waiting_for_world_ = !world_preview_->preload_complete();
        if (waiting_for_world_)
        {
            cancel_world_action();
            SDL_SetWindowRelativeMouseMode(window_, false);
            toast("PREPARING FRONTIER", "GENERATING AND MESHING THE ACTIVE PLAY WINDOW");
        }
        else
        {
            spawn_player();
            SDL_SetWindowRelativeMouseMode(window_, true);
            toast(std::format("{} / {}", game_mode_label(game_mode_),
                              game_difficulty_label(difficulty_)),
                  std::format("SEED {}  /  {} CHUNKS  /  {:L} EXPOSED FACES", world_seed_,
                              world_preview_->stats().chunk_count,
                              world_preview_->stats().visible_face_count));
        }
    }

    void begin_online_discovery(bool quiet = false)
    {
        if (online_client_operation_.valid())
        {
            if (!quiet && online_client_pending_ == OnlineClientOperation::discovery)
                online_discovery_quiet_ = false;
            join_browser_state_ = JoinBrowserState::searching;
            join_browser_status_ = "WAITING FOR THE ACTIVE LIVE STEAM LOBBY QUERY";
            if (!quiet)
                toast("STEAM DISCOVERY BUSY", "WAITING FOR THE CURRENT LOBBY QUERY");
            return;
        }
        if (online_client_ != nullptr && online_client_joined_)
        {
            join_browser_state_ = JoinBrowserState::failed;
            join_browser_status_ = "LEAVE THE CURRENT SESSION BEFORE REFRESHING";
            if (!quiet)
                toast("ALREADY IN SESSION", "LEAVE THE CURRENT SESSION BEFORE REFRESHING");
            return;
        }
        const std::filesystem::path steam_api = installed_steam_api_path();
        if (steam_api.empty())
        {
            join_browser_state_ = JoinBrowserState::failed;
            join_browser_status_ = "OWNED X86 STEAM_API.DLL WAS NOT FOUND";
            if (!quiet)
                toast("STEAM RUNTIME NOT FOUND",
                      "SET OPENMINERZ_STEAM_API TO THE OWNED X86 STEAM_API.DLL");
            return;
        }
        online_client_pending_ = OnlineClientOperation::discovery;
        online_discovery_quiet_ = quiet;
        join_browser_state_ = JoinBrowserState::searching;
        join_browser_status_ = "AUTHENTICATING WITH STEAM  /  WORLDWIDE PUBLIC  /  MAX 50";
        if (!quiet)
            toast("SEARCHING STEAM LOBBIES", "FRIENDS FIRST  /  WORLDWIDE PUBLIC  /  MAX 50");
        if (online_client_ != nullptr)
        {
            std::unique_ptr<net::LegacySteamRuntimeClient> client = std::move(online_client_);
            online_client_operation_ =
                std::async(std::launch::async, [client = std::move(client)]() mutable {
                    OnlineClientOperationResult result;
                    result.client = std::move(client);
                    if (!result.client->refresh(result.discovery, result.error))
                    {
                        std::string ignored;
                        static_cast<void>(result.client->stop(ignored));
                    }
                    return result;
                });
            return;
        }
        net::LegacySteamRuntimeClientOptions options;
        options.protocol = net::ProtocolVersion::classic_v3;
        options.discovery_protocol.reset();
        options.helper_path =
            application_directory() / "SteamBridge" / "OpenMinerZ.SteamBridge32.exe";
        options.steam_api_path = steam_api;
        options.trace_path = options_.network_trace_path;
        online_client_operation_ =
            std::async(std::launch::async, [options = std::move(options)]() mutable {
                OnlineClientOperationResult result;
                result.client = std::make_unique<net::LegacySteamRuntimeClient>();
                if (!result.client->start_browser(options, result.discovery, result.error))
                {
                    result.client.reset();
                }
                return result;
            });
    }

    void begin_online_join(std::uint64_t lobby_id, net::ProtocolVersion protocol,
                           std::string password)
    {
        if (online_client_operation_.valid() || online_client_ == nullptr || online_client_joined_)
        {
            toast("JOIN TRANSPORT UNAVAILABLE", "REFRESH DISCOVERY BEFORE JOINING");
            return;
        }
        online_client_pending_ = OnlineClientOperation::join;
        online_join_stage_ = OnlineJoinStage::joining_lobby;
        online_client_protocol_ = protocol;
        online_join_elapsed_ = 0.0F;
        online_join_lobby_id_ = lobby_id;
        online_join_host_name_.clear();
        online_join_detail_ = "CONTACTING STEAM MATCHMAKING";
        online_sync_chunks_received_ = 0U;
        online_sync_chunks_total_ = 0U;
        if (const net::DiscoveredSession *session = selected_join_session(); session != nullptr)
        {
            online_join_server_name_ = uppercase(session->server_name);
            online_join_host_name_ = uppercase(session->host_name);
        }
        else
        {
            online_join_server_name_ = std::format("STEAM LOBBY {}", lobby_id);
        }
        toast("JOINING STEAM LOBBY", std::format("LOBBY {}  /  NEGOTIATING ADMISSION", lobby_id));
        std::unique_ptr<net::LegacySteamRuntimeClient> client = std::move(online_client_);
        const net::ModManifest client_manifest = join_client_manifest_;
        online_client_operation_ = std::async(
            std::launch::async, [client = std::move(client), lobby_id, protocol, client_manifest,
                                 password = std::move(password)]() mutable {
                OnlineClientOperationResult result;
                result.client = std::move(client);
                if (!result.client->join(lobby_id, protocol, client_manifest, std::move(password),
                                         result.ready, result.error))
                {
                    std::string ignored;
                    static_cast<void>(result.client->stop(ignored));
                }
                return result;
            });
    }

    [[nodiscard]] bool publish_online_broadcast(std::string_view message,
                                                std::span<const std::uint8_t> payload,
                                                net::MessageDeliveryPolicy delivery,
                                                std::string &error)
    {
        const bool hosting = online_host_ != nullptr && online_host_->running();
        const bool joined = online_client_ != nullptr && online_client_joined_ &&
                            online_client_->connected() && online_bootstrap_client_ == nullptr;
        if (!hosting && !joined)
        {
            error.clear();
            return true;
        }
        const net::ProtocolVersion protocol =
            hosting ? host_settings_.protocol : online_client_protocol_;
        const net::MessageDescriptor *descriptor = net::message_by_name(protocol, message);
        net::PacketError packet_error{};
        const auto packet = descriptor != nullptr ? net::encode_packet(protocol, descriptor->id,
                                                                       payload, packet_error)
                                                  : std::nullopt;
        if (!packet.has_value())
        {
            error = std::format("Could not encode online {} packet", message);
            return false;
        }
        if (hosting)
        {
            net::LegacySteamOutbound outbound;
            outbound.application_packet = *packet;
            outbound.delivery = delivery;
            return online_host_->send_authoritative(outbound, error);
        }
        return online_client_->send(std::nullopt, *packet, delivery, error);
    }

    [[nodiscard]] bool publish_online_direct(std::uint8_t recipient_gid, std::string_view message,
                                             std::span<const std::uint8_t> payload,
                                             net::MessageDeliveryPolicy delivery,
                                             std::string &error)
    {
        const bool hosting = online_host_ != nullptr && online_host_->running();
        const bool joined = online_client_ != nullptr && online_client_joined_ &&
                            online_client_->connected() && online_bootstrap_client_ == nullptr;
        if (!hosting && !joined)
        {
            error = "No active Steam gameplay session can carry a direct packet";
            return false;
        }
        const net::ProtocolVersion protocol =
            hosting ? host_settings_.protocol : online_client_protocol_;
        const net::MessageDescriptor *descriptor = net::message_by_name(protocol, message);
        net::PacketError packet_error{};
        const auto packet = descriptor != nullptr ? net::encode_packet(protocol, descriptor->id,
                                                                       payload, packet_error)
                                                  : std::nullopt;
        if (!packet.has_value())
        {
            error = std::format("Could not encode online {} packet", message);
            return false;
        }
        if (!hosting)
            return online_client_->send(recipient_gid, *packet, delivery, error);
        const std::optional<net::PeerId> peer = online_host_->peer_for_gid(recipient_gid);
        if (!peer.has_value())
        {
            error = "The direct packet target is no longer connected";
            return false;
        }
        net::LegacySteamOutbound outbound;
        outbound.recipient = *peer;
        outbound.application_packet = *packet;
        outbound.delivery = delivery;
        return online_host_->send_authoritative(outbound, error);
    }

    [[nodiscard]] bool online_gameplay_active() const noexcept
    {
        return options_.diagnostic_multiplayer ||
               (online_host_ != nullptr && online_host_->running()) ||
               (online_client_ != nullptr && online_client_joined_ && online_client_->connected() &&
                online_bootstrap_client_ == nullptr);
    }

    [[nodiscard]] net::ProtocolVersion online_gameplay_protocol() const noexcept
    {
        return online_host_ != nullptr && online_host_->running() ? host_settings_.protocol
                                                                  : online_client_protocol_;
    }

    [[nodiscard]] bool publish_local_dig_effect(bool placing, const world::BlockBreakUpdate &impact,
                                                std::string &error)
    {
        if (!online_gameplay_active())
        {
            error.clear();
            return true;
        }
        net::DigPayload message;
        message.placing = placing;
        message.location = {impact.intersection.x, impact.intersection.y, impact.intersection.z};
        message.direction = {static_cast<float>(impact.normal_x),
                             static_cast<float>(impact.normal_y),
                             static_cast<float>(impact.normal_z)};
        message.block_type = static_cast<std::int32_t>(impact.block);
        net::PayloadError payload_error{};
        const auto payload =
            net::encode_dig_payload(message, online_gameplay_protocol(), payload_error);
        if (!payload.has_value())
        {
            error = "The local DigMessage presentation payload was not stock-valid";
            return false;
        }
        return publish_online_broadcast("DigMessage", *payload,
                                        net::MessageDeliveryPolicy::reliable_unordered, error);
    }

    [[nodiscard]] static std::string_view dig_sound_cue(world::BlockType block) noexcept
    {
        if (block == world::BlockType::sand || block == world::BlockType::snow)
            return "Sand";
        if (block == world::BlockType::leaves)
            return "leaves";
        return "punch";
    }

    [[nodiscard]] bool apply_online_dig_effect(std::uint8_t sender_gid,
                                               net::ProtocolVersion protocol,
                                               std::span<const std::uint8_t> payload,
                                               bool enforce_spatial_authority)
    {
        // The original sender receives its echoed DigMessage. Local effects
        // were already applied synchronously, so suppress that one duplicate.
        if (online_client_ != nullptr && sender_gid == online_local_gid_)
            return true;
        net::DigPayload dig;
        net::PayloadError payload_error{};
        if (!net::decode_dig_payload(payload, protocol, dig, payload_error))
            return false;
        if (world_preview_ == nullptr)
            return !enforce_spatial_authority;
        const auto remote = online_remote_players_.find(sender_gid);
        if (remote == online_remote_players_.end() || !remote->second.has_update ||
            elapsed_ - remote->second.last_update > 10.0F)
            return !enforce_spatial_authority;
        const float dx = dig.location.x - remote->second.state.position.x;
        const float dy = dig.location.y -
                         (remote->second.state.position.y + world::PlayerSimulation::eye_height);
        const float dz = dig.location.z - remote->second.state.position.z;
        if (!net::valid_network_surface_normal(dig.direction))
            return false;
        if (dx * dx + dy * dy + dz * dz > 49.0F)
            return !enforce_spatial_authority;
        world::BlockBreakUpdate impact;
        impact.block = static_cast<world::BlockType>(dig.block_type);
        impact.intersection = {dig.location.x, dig.location.y, dig.location.z};
        impact.normal_x = static_cast<int>(std::lround(dig.direction.x));
        impact.normal_y = static_cast<int>(std::lround(dig.direction.y));
        impact.normal_z = static_cast<int>(std::lround(dig.direction.z));
        const world::Vec3 sound_position = remote->second.state.position;
        play_positional(dig.placing ? std::string_view{"Place"} : dig_sound_cue(impact.block),
                        sound_position);
        world_preview_->spawn_mining_impact(impact);
        return true;
    }

    [[nodiscard]] bool apply_online_restart_level()
    {
        if (world_preview_ == nullptr)
            return false;
        player_life_ = PlayerLife::alive;
        death_timer_ = 0.0F;
        player_oxygen_ = 1.0F;
        lava_damage_time_ = world::lava_damage_period_seconds;
        lava_sound_played_ = false;
        place_player_at_world_start(true);
        sound_bank_->stop_music();
        day_night_ = world::DayNightCycle{0.4F};
        last_day_number_ = 0;
        max_distance_ = 0;
        damage_indicators_.clear();
        cancel_world_action();
        toast("LEVEL RESTARTED", "THE HOST RETURNED EVERYONE TO THE START");
        return true;
    }

    [[nodiscard]] std::uint8_t online_local_player_gid() const noexcept
    {
        return online_host_ != nullptr && online_host_->running() ? 0U : online_local_gid_;
    }

    void sync_network_pickup_mode()
    {
        if (world_preview_ == nullptr)
            return;
        const bool online = online_gameplay_active();
        world_preview_->set_network_pickups_enabled(
            online, online ? static_cast<std::int32_t>(online_local_player_gid()) : -1);
    }

    [[nodiscard]] bool pickup_stack_to_item(const InventoryStack &stack,
                                            net::ProtocolVersion protocol,
                                            net::InventoryItemPayload &result) const
    {
        net::InventorySlotPayload slot;
        if (!net::inventory_slot_to_payload(PlayerInventory::Slot{stack}, protocol, slot) ||
            !slot.has_value())
            return false;
        result = std::move(*slot);
        return true;
    }

    [[nodiscard]] bool pickup_item_to_stack(const net::InventoryItemPayload &item,
                                            net::ProtocolVersion protocol,
                                            InventoryStack &result) const
    {
        PlayerInventory::Slot slot;
        if (!net::inventory_slot_to_stack(net::InventorySlotPayload{item}, protocol, slot) ||
            !slot.has_value())
            return false;
        result = std::move(*slot);
        return true;
    }

    void award_local_pickup(const InventoryStack &stack)
    {
        const std::uint16_t remainder = inventory_.add_stack(stack);
        if (remainder != 0U)
        {
            // PlayerInventory.AddInventoryItem preserves a full pickup by throwing only its
            // unstacked remainder back into the world as a player-owned pickup.
            InventoryStack returned = stack;
            returned.count = remainder;
            world_preview_->spawn_dropped_stack(returned, player_.state());
        }
        sound_bank_->play_pickup();
        caption("[ ITEM PICKED UP ]", 1.2F);
        toast(remainder == 0U ? "ITEM PICKED UP" : "INVENTORY FULL",
              std::format("{} ×{}", item_name(stack.item), stack.count - remainder));
        sync_selected_item();
    }

    [[nodiscard]] bool publish_spawned_network_pickups()
    {
        if (world_preview_ == nullptr || !online_gameplay_active())
            return true;
        const std::uint8_t local_gid = online_local_player_gid();
        const net::ProtocolVersion protocol = online_gameplay_protocol();
        bool success = true;
        for (const SpawnedNetworkPickup &spawned :
             world_preview_->drain_spawned_network_pickups(local_gid))
        {
            net::InventoryItemPayload item;
            net::PayloadError payload_error{};
            net::CreatePickupPayload message;
            message.spawn_position = {spawned.position.x, spawned.position.y, spawned.position.z};
            message.spawn_vector = {spawned.velocity.x, spawned.velocity.y, spawned.velocity.z};
            message.pickup_id = spawned.pickup_id;
            message.dropped = spawned.dropped;
            message.display_on_pickup = spawned.display_on_pickup;
            if (!pickup_stack_to_item(spawned.stack, protocol, item))
            {
                static_cast<void>(
                    world_preview_->consume_network_pickup(spawned.pickup_id, local_gid));
                success = false;
                continue;
            }
            message.item = std::move(item);
            const auto payload =
                net::encode_create_pickup_payload(message, protocol, payload_error);
            std::string error;
            if (!payload.has_value() ||
                !publish_online_broadcast("CreatePickupMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_unordered, error))
            {
                static_cast<void>(
                    world_preview_->consume_network_pickup(spawned.pickup_id, local_gid));
                std::cerr << "network.local.create-pickup.failure=" << error << '\n';
                success = false;
            }
        }
        return success;
    }

    [[nodiscard]] bool consume_pickup_as_host(std::int32_t pickup_id, std::int32_t spawner_id,
                                              std::uint8_t picker_gid)
    {
        if (online_host_ == nullptr || !online_host_->running() || world_preview_ == nullptr)
            return false;
        const auto found = world_preview_->find_network_pickup(pickup_id, spawner_id);
        if (!found.has_value())
            return false;
        net::InventoryItemPayload item;
        if (!pickup_stack_to_item(found->stack, host_settings_.protocol, item))
            return false;
        net::ConsumePickupPayload message;
        message.pickup_position = {found->position.x, found->position.y, found->position.z};
        message.pickup_id = pickup_id;
        message.spawner_id = spawner_id;
        message.picker_upper = picker_gid;
        message.item = std::move(item);
        message.display_on_pickup = found->display_on_pickup;
        net::PayloadError payload_error{};
        const auto payload =
            net::encode_consume_pickup_payload(message, host_settings_.protocol, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("ConsumePickupMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
        {
            std::cerr << "network.host.consume-pickup.failure=" << error << '\n';
            return false;
        }
        const auto consumed = world_preview_->consume_network_pickup(pickup_id, spawner_id);
        if (!consumed.has_value())
            return false;
        if (picker_gid == 0U)
            award_local_pickup(consumed->stack);
        return true;
    }

    [[nodiscard]] bool publish_local_pickup_request(const CollectedBlockStack &pickup)
    {
        if (!pickup.pickup_id.has_value() || pickup.spawner_id < 0)
            return false;
        if (online_host_ != nullptr && online_host_->running())
        {
            if (consume_pickup_as_host(*pickup.pickup_id, pickup.spawner_id, 0U))
                return true;
            world_preview_->clear_network_pickup_request(*pickup.pickup_id, pickup.spawner_id);
            return false;
        }
        net::PayloadError payload_error{};
        const auto payload = net::encode_request_pickup_payload(
            {*pickup.pickup_id, pickup.spawner_id}, payload_error);
        std::string error;
        if (payload.has_value() &&
            publish_online_broadcast("RequestPickupMessage", *payload,
                                     net::MessageDeliveryPolicy::reliable_unordered, error))
            return true;
        world_preview_->clear_network_pickup_request(*pickup.pickup_id, pickup.spawner_id);
        std::cerr << "network.local.request-pickup.failure=" << error << '\n';
        return false;
    }

    [[nodiscard]] bool apply_online_create_pickup(std::uint8_t spawner_gid,
                                                  net::ProtocolVersion protocol,
                                                  std::span<const std::uint8_t> payload)
    {
        net::CreatePickupPayload message;
        net::PayloadError payload_error{};
        InventoryStack stack;
        return world_preview_ != nullptr &&
               net::decode_create_pickup_payload(payload, protocol, message, payload_error) &&
               pickup_item_to_stack(message.item, protocol, stack) &&
               world_preview_->spawn_network_pickup(
                   stack,
                   {message.spawn_position.x, message.spawn_position.y, message.spawn_position.z},
                   {message.spawn_vector.x, message.spawn_vector.y, message.spawn_vector.z},
                   message.pickup_id, spawner_gid, message.dropped, message.display_on_pickup);
    }

    [[nodiscard]] bool apply_online_consume_pickup(std::uint8_t sender_gid,
                                                   net::ProtocolVersion protocol,
                                                   std::span<const std::uint8_t> payload)
    {
        if (sender_gid != 0U || world_preview_ == nullptr)
            return false;
        net::ConsumePickupPayload message;
        net::PayloadError payload_error{};
        if (!net::decode_consume_pickup_payload(payload, protocol, message, payload_error))
            return false;
        const auto consumed =
            world_preview_->consume_network_pickup(message.pickup_id, message.spawner_id);
        if (message.picker_upper != online_local_gid_)
            return true;
        InventoryStack stack;
        if (!consumed.has_value() || !pickup_item_to_stack(message.item, protocol, stack))
            return false;
        award_local_pickup(stack);
        return true;
    }

    [[nodiscard]] std::optional<std::uint8_t> resolve_online_enemy_target_gid(
        std::uint8_t sender_gid, net::ProtocolVersion protocol, std::string_view player_name) const
    {
        if (protocol == net::ProtocolVersion::classic_v3 || player_name.empty())
            return sender_gid;
        const std::uint8_t local_gid = online_local_player_gid();
        const std::string_view local_name = online_persona_name_.empty()
                                                ? std::string_view{"OpenMinerZ"}
                                                : std::string_view{online_persona_name_};
        if (player_name == local_name)
            return local_gid;
        std::optional<std::uint8_t> result;
        for (const auto &[gid, remote] : online_remote_players_)
            if (remote.gamer_tag == player_name && (!result.has_value() || gid < *result))
                result = gid;
        return result;
    }

    [[nodiscard]] bool apply_online_spawn_enemy(std::uint8_t sender_gid,
                                                net::ProtocolVersion protocol,
                                                std::span<const std::uint8_t> payload,
                                                bool require_remote_position)
    {
        net::SpawnEnemyPayload message;
        net::PayloadError payload_error{};
        if (!net::decode_spawn_enemy_payload(payload, protocol, message, payload_error))
            return false;
        const auto target_gid =
            resolve_online_enemy_target_gid(sender_gid, protocol, message.player_name);
        if (!target_gid.has_value())
            return false;
        const world::EnemyIdentity identity{*target_gid,
                                            static_cast<std::uint32_t>(message.enemy_id)};
        if (enemy_horde_.find_enemy(identity).has_value())
            return true; // The stock local echo follows our eager local spawn.
        if (message.enemy_type >= world::enemy_type_count)
            return false;
        if (message.initialization.slow_speed_milliseconds > 100'000 ||
            message.initialization.fast_speed_milliseconds > 100'000 ||
            message.initialization.emerge_speed_milliseconds > 100'000 ||
            message.initialization.run_activation_milliseconds > 3'600'000 ||
            message.initialization.normal_activation_milliseconds > 3'600'000)
            return false;
        if (require_remote_position)
        {
            const auto remote = online_remote_players_.find(sender_gid);
            if (remote == online_remote_players_.end() || !remote->second.has_update)
                return false;
            const float dx = message.spawn_position.x - remote->second.state.position.x;
            const float dy = message.spawn_position.y - remote->second.state.position.y;
            const float dz = message.spawn_position.z - remote->second.state.position.z;
            if (dx * dx + dy * dy + dz * dz > 15'625.0F)
                return false;
        }
        std::size_t target_count{};
        for (const world::Enemy &enemy : enemy_horde_.enemies())
            if (enemy.target_gid == *target_gid && enemy.state != world::EnemyState::despawn)
                ++target_count;
        if (target_count >= 64U)
            return false;
        if (protocol == net::ProtocolVersion::modern_v4 &&
            message.spawner_position.x >= -1'000'000.0F &&
            message.spawner_position.x <= 1'000'000.0F && message.spawner_position.y >= -64.0F &&
            message.spawner_position.y < 64.0F && message.spawner_position.z >= -1'000'000.0F &&
            message.spawner_position.z <= 1'000'000.0F)
        {
            const net::IntVector3 spawner_location{
                static_cast<int>(std::floor(message.spawner_position.x)),
                static_cast<int>(std::floor(message.spawner_position.y)),
                static_cast<int>(std::floor(message.spawner_position.z))};
            const auto spawner_block = online_block_at(spawner_location);
            if (spawner_block.has_value() && world::is_spawner(*spawner_block))
            {
                const net::UpdateSpawnerPayload update{message.spawner_position, true};
                if ((require_remote_position &&
                     !online_host_->update_authoritative_spawner(spawner_location, true)) ||
                    !apply_online_spawner_update(update))
                    return false;
            }
        }
        const world::EnemyInitialization initialization{
            static_cast<float>(message.initialization.slow_speed_milliseconds) / 1'000.0F,
            static_cast<float>(message.initialization.fast_speed_milliseconds) / 1'000.0F,
            static_cast<float>(message.initialization.emerge_speed_milliseconds) / 1'000.0F,
            static_cast<float>(message.initialization.run_activation_milliseconds) / 1'000.0F,
            static_cast<float>(message.initialization.normal_activation_milliseconds) / 1'000.0F};
        if (!enemy_horde_.spawn_networked(
                static_cast<int>(message.enemy_type),
                {message.spawn_position.x, message.spawn_position.y, message.spawn_position.z},
                initialization, static_cast<std::uint32_t>(message.enemy_id), message.random_seed,
                *target_gid))
            return false;
        if (require_remote_position && protocol == net::ProtocolVersion::modern_v4 &&
            message.spawn_value > 0)
        {
            const net::IntVector3 source{static_cast<int>(std::floor(message.spawner_position.x)),
                                         static_cast<int>(std::floor(message.spawner_position.y)),
                                         static_cast<int>(std::floor(message.spawner_position.z))};
            const auto source_block = online_block_at(source);
            if (source_block.has_value() && world::is_spawner(*source_block))
                remote_spawner_enemy_owners_[identity] = sender_gid;
        }
        if (world::enemy_stats(static_cast<int>(message.enemy_type)).habitat ==
            world::EnemyHabitat::aboveground)
        {
            const world::Vec3 position{message.spawn_position.x, message.spawn_position.y,
                                       message.spawn_position.z};
            play_positional("CreatureUnearth", position);
            play_positional("ZombieCry", position);
        }
        return true;
    }

    [[nodiscard]] bool apply_online_enemy_event(std::uint8_t sender_gid,
                                                net::ProtocolVersion protocol,
                                                const net::DecodedPacket &decoded,
                                                bool require_existing)
    {
        net::PayloadError payload_error{};
        if (decoded.message->name == "KillEnemyMessage")
        {
            net::KillEnemyPayload message;
            InventoryItemId weapon{};
            if (!net::decode_kill_enemy_payload(decoded.payload, protocol, message,
                                                payload_error) ||
                message.killer_id != sender_gid ||
                !net::inventory_item_from_wire(message.weapon_id, protocol, weapon))
                return false;
            const world::EnemyIdentity identity{static_cast<std::uint8_t>(message.target_id),
                                                static_cast<std::uint32_t>(message.enemy_id)};
            if (!enemy_horde_.find_enemy(identity).has_value())
                return !require_existing;
            if (!enemy_horde_.kill_enemy(identity, message.killer_id == online_local_player_gid()))
                return false;
            remote_spawner_enemy_owners_.erase(identity);
            notify_local_spawner_enemy_defeated(identity, message.killer_id);
            return true;
        }
        net::EnemyTargetPayload message;
        if (!net::decode_enemy_target_payload(decoded.payload, message, payload_error) ||
            message.target_id < 0 || message.target_id > 255 ||
            (sender_gid != 0U && message.target_id != static_cast<std::int32_t>(sender_gid)))
            return false;
        const world::EnemyIdentity identity{static_cast<std::uint8_t>(message.target_id),
                                            static_cast<std::uint32_t>(message.enemy_id)};
        if (!enemy_horde_.find_enemy(identity).has_value())
            return !require_existing;
        if (decoded.message->name == "SpeedUpEnemyMessage")
            return enemy_horde_.speed_up_enemy(identity);
        if (decoded.message->name == "EnemyGiveUpMessage")
        {
            if (!enemy_horde_.give_up_enemy(identity))
                return false;
            remote_spawner_enemy_owners_.erase(identity);
            notify_local_spawner_enemy_removed(identity);
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool network_direction(net::Vector3 encoded,
                                                world::Vec3 &direction) noexcept
    {
        const float length_squared =
            encoded.x * encoded.x + encoded.y * encoded.y + encoded.z * encoded.z;
        if (!std::isfinite(length_squared) || length_squared < 0.81F || length_squared > 1.21F)
            return false;
        const float inverse_length = 1.0F / std::sqrt(length_squared);
        direction = {encoded.x * inverse_length, encoded.y * inverse_length,
                     encoded.z * inverse_length};
        return true;
    }

    [[nodiscard]] bool apply_online_combat_launch(std::uint8_t sender_gid,
                                                  net::ProtocolVersion protocol,
                                                  const net::DecodedPacket &decoded,
                                                  bool require_remote_position)
    {
        const auto found = online_remote_players_.find(sender_gid);
        if (found == online_remote_players_.end() || !found->second.has_update)
            return !require_remote_position;
        const net::RemotePlayerReplicationState &remote = found->second.state;
        const world::Vec3 eye{remote.position.x,
                              remote.position.y + world::PlayerSimulation::eye_height,
                              remote.position.z};
        net::PayloadError payload_error{};
        const auto close_to_remote_eye = [&](net::Vector3 position) {
            const float dx = position.x - eye.x;
            const float dy = position.y - eye.y;
            const float dz = position.z - eye.z;
            return dx * dx + dy * dy + dz * dz <= 100.0F;
        };
        const auto spawn_shot = [&](world::Vec3 direction, InventoryItemId item) {
            const GunStats *stats = gun_stats(item);
            if (stats == nullptr)
                return false;
            world::Vec3 origin = eye;
            if (is_laser_gun(item))
            {
                const world::Vec3 right{std::cos(remote.yaw), 0.0F, std::sin(remote.yaw)};
                const world::Vec3 up{right.y * direction.z - right.z * direction.y,
                                     right.z * direction.x - right.x * direction.z,
                                     right.x * direction.y - right.y * direction.x};
                origin = {eye.x + direction.x * 0.55F + right.x * 0.28F - up.x * 0.22F,
                          eye.y + direction.y * 0.55F + right.y * 0.28F - up.y * 0.22F,
                          eye.z + direction.z * 0.55F + right.z * 0.28F - up.z * 0.22F};
            }
            spawn_tracer(origin, direction, stats->enemy_damage, weapon_damage_flags(item), item,
                         false, sender_gid, false);
            return true;
        };

        if (decoded.message->name == "GunshotMessage")
        {
            net::GunshotPayload shot;
            world::Vec3 direction;
            InventoryItemId item{};
            if (!net::decode_gunshot_payload(decoded.payload, protocol, shot, payload_error) ||
                !network_direction(shot.direction, direction) ||
                !net::inventory_item_from_wire(shot.item_id, protocol, item) ||
                !spawn_shot(direction, item))
                return false;
            play_positional(gun_stats(item)->shoot_cue, remote.position);
            return true;
        }
        if (decoded.message->name == "ShotgunShotMessage")
        {
            net::ShotgunShotPayload shot;
            if (!net::decode_shotgun_shot_payload(decoded.payload, protocol, shot, payload_error))
                return false;
            InventoryItemId item{};
            if (!net::inventory_item_from_wire(shot.item_id, protocol, item))
                return false;
            std::array<world::Vec3, net::ShotgunShotPayload::direction_count> directions{};
            for (std::size_t index{}; index < shot.directions.size(); ++index)
            {
                if (!network_direction(shot.directions[index], directions[index]))
                    return false;
            }
            for (const world::Vec3 direction : directions)
                if (!spawn_shot(direction, item))
                    return false;
            play_positional(gun_stats(item)->shoot_cue, remote.position);
            return true;
        }
        if (decoded.message->name == "GrenadeMessage")
        {
            net::GrenadePayload message;
            world::Vec3 direction;
            if (!net::decode_grenade_payload(decoded.payload, protocol, message, payload_error) ||
                !network_direction(message.direction, direction) || message.seconds_left < 1.0F ||
                message.seconds_left > 5.0F || !close_to_remote_eye(message.position))
                return false;
            Grenade grenade;
            grenade.position = {message.position.x, message.position.y, message.position.z};
            grenade.velocity = {direction.x * world::grenade_throw_speed,
                                direction.y * world::grenade_throw_speed,
                                direction.z * world::grenade_throw_speed};
            grenade.time_left = message.seconds_left;
            grenade.grenade_type = message.grenade_type;
            grenade.owner_gid = sender_gid;
            grenade.locally_authoritative = false;
            grenades_.push_back(grenade);
            return true;
        }
        if (decoded.message->name == "FireRocketMessage")
        {
            net::FireRocketPayload message;
            world::Vec3 direction;
            if (!net::decode_fire_rocket_payload(decoded.payload, protocol, message,
                                                 payload_error) ||
                !network_direction(message.direction, direction) ||
                !close_to_remote_eye(message.position))
                return false;
            InventoryItemId item{};
            if (!net::inventory_item_from_wire(message.weapon_type, protocol, item))
                return false;
            const GunStats *stats = gun_stats(item);
            if (stats == nullptr)
                return false;
            const world::Vec3 origin{message.position.x - direction.x,
                                     message.position.y - direction.y,
                                     message.position.z - direction.z};
            spawn_tracer(origin, direction, stats->enemy_damage, weapon_damage_flags(item), item,
                         message.guided, sender_gid, false);
            play_positional("RPGLaunch", remote.position);
            return true;
        }
        return true;
    }

    void remember_network_projectile(NetworkProjectileKind kind, std::uint8_t owner_gid,
                                     std::uint16_t type, world::Vec3 position)
    {
        if (!online_gameplay_active())
            return;
        recent_network_projectiles_.push_back({kind, owner_gid, type, position, elapsed_ + 2.0F});
        if (recent_network_projectiles_.size() > 128U)
            recent_network_projectiles_.erase(recent_network_projectiles_.begin());
    }

    void remember_network_rocket(std::uint8_t owner_gid, InventoryItemId item, world::Vec3 position)
    {
        std::int16_t wire_item{};
        if (!net::inventory_item_to_wire(item, online_gameplay_protocol(), wire_item))
            return;
        remember_network_projectile(NetworkProjectileKind::rocket, owner_gid,
                                    static_cast<std::uint16_t>(wire_item), position);
    }

    void discard_remote_projectiles(std::optional<std::uint8_t> owner_gid = std::nullopt)
    {
        std::erase_if(fireballs_, [&](const Fireball &fireball) {
            return !fireball.spawned_locally &&
                   (!owner_gid.has_value() || fireball.owner_gid == *owner_gid);
        });
        std::erase_if(grenades_, [&](const Grenade &grenade) {
            return !grenade.locally_authoritative &&
                   (!owner_gid.has_value() || grenade.owner_gid == *owner_gid);
        });
        std::erase_if(tracers_, [&](const Tracer &tracer) {
            return !tracer.locally_authoritative &&
                   (!owner_gid.has_value() || tracer.owner_gid == *owner_gid);
        });
        std::erase_if(recent_network_projectiles_, [&](const RecentNetworkProjectile &projectile) {
            return !owner_gid.has_value() || projectile.owner_gid == *owner_gid;
        });
    }

    void discard_network_enemies(std::optional<std::uint8_t> departed_gid = std::nullopt,
                                 std::optional<std::uint8_t> surviving_local_gid = std::nullopt)
    {
        std::erase_if(enemy_horde_.enemies(), [&](const world::Enemy &enemy) {
            if (departed_gid.has_value())
                return enemy.target_gid == *departed_gid;
            return surviving_local_gid.has_value() && enemy.target_gid != *surviving_local_gid;
        });
        if (surviving_local_gid.has_value())
            for (world::Enemy &enemy : enemy_horde_.enemies())
                enemy.target_gid = 0U;
    }

    void discard_network_dragon(std::optional<std::uint8_t> departed_gid = std::nullopt,
                                std::optional<std::uint8_t> surviving_local_gid = std::nullopt)
    {
        if (!dragon_owner_gid_.has_value() ||
            (departed_gid.has_value() && *dragon_owner_gid_ != *departed_gid))
            return;
        if (surviving_local_gid.has_value() && *dragon_owner_gid_ == *surviving_local_gid)
        {
            dragon_owner_gid_.reset();
            dragon_pending_ = false;
            dragon_loot_pending_ = false;
            return;
        }
        dragon_.deactivate();
        queued_dragon_fireball_ids_.clear();
        dragon_owner_gid_.reset();
        dragon_pending_ = false;
        dragon_loot_pending_ = false;
    }

    [[nodiscard]] bool authorize_network_detonation(NetworkProjectileKind kind,
                                                    std::uint8_t owner_gid, std::uint16_t type,
                                                    world::Vec3 position,
                                                    net::ProtocolVersion protocol)
    {
        const float maximum_distance_squared =
            kind == NetworkProjectileKind::grenade ? 64.0F : 400.0F;
        const auto near = [&](world::Vec3 candidate) {
            const float dx = candidate.x - position.x;
            const float dy = candidate.y - position.y;
            const float dz = candidate.z - position.z;
            return dx * dx + dy * dy + dz * dz <= maximum_distance_squared;
        };
        if (kind == NetworkProjectileKind::grenade)
        {
            const auto active = std::ranges::find_if(grenades_, [&](const Grenade &grenade) {
                // HEGrenadeProjectile backs both HE (0) and Sticky (3), but its
                // Explode override hard-codes DetonateGrenadeMessage type 0.
                const bool type_matches =
                    world::grenade_detonation_type(grenade.grenade_type) == type;
                return !grenade.dead && !grenade.locally_authoritative &&
                       grenade.owner_gid == owner_gid && type_matches && near(grenade.position);
            });
            if (active != grenades_.end())
            {
                active->dead = true;
                return true;
            }
        }
        else
        {
            const auto active = std::ranges::find_if(tracers_, [&](const Tracer &tracer) {
                std::int16_t wire_item{};
                return !tracer.dead && tracer.rocket && !tracer.locally_authoritative &&
                       tracer.owner_gid == owner_gid &&
                       net::inventory_item_to_wire(tracer.item, protocol, wire_item) &&
                       static_cast<std::uint16_t>(wire_item) == type && near(tracer.head);
            });
            if (active != tracers_.end())
            {
                active->dead = true;
                return true;
            }
        }
        const auto recent = std::ranges::find_if(
            recent_network_projectiles_, [&](const RecentNetworkProjectile &p) {
                const bool type_matches =
                    p.type == type ||
                    (kind == NetworkProjectileKind::grenade &&
                     world::grenade_detonation_type(static_cast<std::uint8_t>(p.type)) == type);
                return p.expires >= elapsed_ && p.kind == kind && p.owner_gid == owner_gid &&
                       type_matches && near(p.position);
            });
        if (recent == recent_network_projectiles_.end())
            return false;
        recent_network_projectiles_.erase(recent);
        return true;
    }

    [[nodiscard]] bool apply_online_combat_detonation(std::uint8_t sender_gid,
                                                      net::ProtocolVersion protocol,
                                                      const net::DecodedPacket &decoded,
                                                      bool require_projectile)
    {
        net::PayloadError payload_error{};
        const bool pvp_enabled = (online_host_ != nullptr && online_host_->running()
                                      ? host_settings_.player_versus_player
                                      : online_client_pvp_) != net::PlayerVersusPlayer::off;
        if (decoded.message->name == "DetonateExplosiveMessage")
        {
            net::DetonateExplosivePayload message;
            if (!net::decode_detonate_explosive_payload(decoded.payload, protocol, message,
                                                        payload_error) ||
                !bounded_world_cell(message.location) || message.explosive_type > 1U)
                return false;
            const auto block = online_block_at(message.location);
            if (!block.has_value() ||
                (*block != world::BlockType::tnt && *block != world::BlockType::c4))
                return false;
            if (require_projectile &&
                !online_host_->apply_authoritative_block_edit(message.location, 0U))
                return false;
            world_preview_->remember_edit(message.location.x, message.location.y,
                                          message.location.z, world::BlockType::empty);
            if (world_preview_->region().contains(message.location.x, message.location.y,
                                                  message.location.z))
            {
                world_preview_->region().set_block(message.location.x, message.location.y,
                                                   message.location.z, world::BlockType::empty);
                world_preview_->rebuild_block_edit(message.location.x, message.location.y,
                                                   message.location.z);
            }
            std::erase_if(armed_explosives_, [&](const ArmedExplosive &armed) {
                return armed.x == message.location.x && armed.y == message.location.y &&
                       armed.z == message.location.z;
            });
            const world::Vec3 location{static_cast<float>(message.location.x),
                                       static_cast<float>(message.location.y),
                                       static_cast<float>(message.location.z)};
            if (message.original_explosion)
            {
                play_positional("Explosion", location);
                world_preview_->spawn_fireball_explosion(location, false, true);
            }
            const world::ExplosiveKind kind =
                message.explosive_type == 0U ? world::ExplosiveKind::tnt : world::ExplosiveKind::c4;
            apply_explosion_splash(location, world::explosive_definition(kind).damage, pvp_enabled,
                                   false, explosive_weapon(kind));
            return true;
        }
        if (decoded.message->name == "DetonateGrenadeMessage")
        {
            net::DetonateGrenadePayload message;
            if (!net::decode_detonate_grenade_payload(decoded.payload, protocol, message,
                                                      payload_error))
                return false;
            const world::Vec3 location{message.location.x, message.location.y, message.location.z};
            const bool authorized =
                authorize_network_detonation(NetworkProjectileKind::grenade, sender_gid,
                                             message.grenade_type, location, protocol);
            if (require_projectile && !authorized)
                return false;
            // The recovered Smoke/Flash handlers are intentionally empty. Only
            // HE (ordinal zero) creates an explosion and splash-damage pass.
            if (message.grenade_type == 0U)
            {
                play_positional("Explosion", location);
                world_preview_->spawn_fireball_explosion(location, false, message.on_ground);
                apply_explosion_splash(location, world::he_grenade_damage_bands, pvp_enabled, false,
                                       InventoryItemId::grenade);
            }
            return true;
        }
        if (decoded.message->name == "DetonateRocketMessage")
        {
            net::DetonateRocketPayload message;
            if (!net::decode_detonate_rocket_payload(decoded.payload, protocol, message,
                                                     payload_error) ||
                message.explosive_type != static_cast<std::uint8_t>(world::ExplosiveKind::rocket))
                return false;
            const world::Vec3 location{message.location.x, message.location.y, message.location.z};
            const bool authorized = authorize_network_detonation(
                NetworkProjectileKind::rocket, sender_gid, message.item_type, location, protocol);
            if (require_projectile && !authorized)
                return false;
            InventoryItemId item{};
            if (message.item_type >
                    static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()) ||
                !net::inventory_item_from_wire(static_cast<std::int16_t>(message.item_type),
                                               protocol, item))
                return false;
            bool explosive_headshot = false;
            if (message.hit_dragon && dragon_.visible())
            {
                const world::Dragon &dragon = dragon_.dragon();
                const world::DragonBasis basis =
                    world::dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
                const world::Vec3 backward{-basis.forward.x, -basis.forward.y, -basis.forward.z};
                const world::Vec3 offset{location.x - dragon.position.x,
                                         location.y - dragon.position.y,
                                         location.z - dragon.position.z};
                explosive_headshot =
                    offset.x * backward.x + offset.y * backward.y + offset.z * backward.z < -4.2F;
            }
            apply_rocket_detonation(location, message.hit_dragon, explosive_headshot, pvp_enabled,
                                    false, item, sender_gid);
            return true;
        }
        return true;
    }

    void schedule_authoritative_chunk_check(net::IntVector3 location)
    {
        if (online_client_ == nullptr || !online_client_joined_ || !online_client_->connected() ||
            online_bootstrap_client_ != nullptr)
            return;
        const net::IntVector3 corner = net::chunk_corner(location);
        if (!requested_chunk_reconciliations_.contains(corner))
            pending_chunk_reconciliations_.insert_or_assign(corner, elapsed_ + 0.35F);
    }

    void pump_authoritative_chunk_checks()
    {
        std::erase_if(requested_chunk_reconciliations_,
                      [&](const auto &entry) { return entry.second <= elapsed_; });
        for (auto iterator = pending_chunk_reconciliations_.begin();
             iterator != pending_chunk_reconciliations_.end();)
        {
            if (iterator->second > elapsed_)
            {
                ++iterator;
                continue;
            }
            const net::IntVector3 corner = iterator->first;
            net::PayloadError payload_error{};
            const auto payload = net::encode_request_chunk_payload({corner, 0U}, payload_error);
            std::string error;
            if (payload.has_value() &&
                publish_online_direct(online_terrain_server_gid_, "RequestChunkMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
            {
                requested_chunk_reconciliations_.insert_or_assign(corner, elapsed_ + 5.0F);
                iterator = pending_chunk_reconciliations_.erase(iterator);
            }
            else
            {
                iterator->second = elapsed_ + 1.0F;
                ++iterator;
                if (!error.empty())
                    std::cerr << "network.chunk-reconcile.request-failure=" << error << '\n';
            }
        }
    }

    [[nodiscard]] bool apply_authoritative_chunk(const net::ProvideChunkPayload &chunk)
    {
        if (world_preview_ == nullptr || chunk.priority != 0U ||
            net::chunk_corner(chunk.block_location) != chunk.block_location ||
            !requested_chunk_reconciliations_.contains(chunk.block_location))
            return false;
        const int maximum_block =
            online_client_protocol_ == net::ProtocolVersion::classic_v3 ? 46 : 93;
        std::set<net::IntVector3> positions;
        std::vector<std::array<int, 4>> edits;
        edits.reserve(chunk.delta.size());
        for (const std::int32_t signed_entry : chunk.delta)
        {
            const auto entry = static_cast<std::uint32_t>(signed_entry);
            const int block = static_cast<int>(entry >> 24U);
            const int local_x = static_cast<int>((entry >> 16U) & 0xFFU);
            const int local_y = static_cast<int>((entry >> 8U) & 0xFFU);
            const int local_z = static_cast<int>(entry & 0xFFU);
            if (block > maximum_block || local_x >= world::chunk_width ||
                local_y >= world::chunk_height || local_z >= world::chunk_depth)
                return false;
            const net::IntVector3 position{chunk.block_location.x + local_x,
                                           chunk.block_location.y + local_y,
                                           chunk.block_location.z + local_z};
            if (position.x < -1'000'000 || position.x > 1'000'000 ||
                position.y < world::world_min_y ||
                position.y >= world::world_min_y + world::chunk_height ||
                position.z < -1'000'000 || position.z > 1'000'000 ||
                !positions.insert(position).second)
                return false;
            edits.push_back({position.x, position.y, position.z, block});
        }
        requested_chunk_reconciliations_.erase(chunk.block_location);
        world_preview_->replace_chunk_edits(chunk.block_location.x, chunk.block_location.z, edits);
        dirty_ = true;
        return true;
    }

    [[nodiscard]] bool publish_local_block_edit(int x, int y, int z, world::BlockType block)
    {
        if (!online_gameplay_active())
            return true;
        const net::ProtocolVersion protocol =
            online_host_ != nullptr ? host_settings_.protocol : online_client_protocol_;
        net::PayloadError payload_error{};
        const net::AlterBlockPayload edit{{x, y, z}, static_cast<std::int32_t>(block)};
        const auto payload = net::encode_alter_block_payload(edit, protocol, payload_error);
        if (!payload.has_value())
            return false;
        if (online_host_ != nullptr && online_host_->running() &&
            !online_host_->apply_authoritative_block_edit(
                edit.block_location, static_cast<std::uint8_t>(edit.block_type)))
            return false;
        std::string error;
        if (!publish_online_broadcast("AlterBlockMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.block-edit.failure=" << error << '\n';
            return false;
        }
        schedule_authoritative_chunk_check({x, y, z});
        return true;
    }

    [[nodiscard]] bool publish_local_block_removals(std::span<const std::array<int, 3>> removed,
                                                    bool do_dig_effects)
    {
        if (removed.empty())
            return true;
        for (std::size_t offset{}; offset < removed.size(); offset += net::maximum_remove_blocks)
        {
            const std::size_t count = std::min(net::maximum_remove_blocks, removed.size() - offset);
            net::RemoveBlocksPayload message;
            message.blocks.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const std::array<int, 3> &cell = removed[offset + index];
                message.blocks.push_back({cell[0], cell[1], cell[2]});
            }
            message.do_dig_effects = do_dig_effects;
            net::PayloadError payload_error{};
            const auto payload = net::encode_remove_blocks_payload(message, payload_error);
            if (!payload.has_value())
                return false;
            if (online_host_ != nullptr && online_host_->running() &&
                !online_host_->apply_authoritative_block_removals(message.blocks))
                return false;
            std::string error;
            if (!publish_online_broadcast("RemoveBlocksMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_ordered, error))
            {
                std::cerr << "network.local.block-removal.failure=" << error << '\n';
                return false;
            }
            for (const net::IntVector3 location : message.blocks)
                schedule_authoritative_chunk_check(location);
        }
        return true;
    }

    [[nodiscard]] bool publish_local_crate_item(const CrateKey &location, std::size_t index,
                                                const PlayerInventory::Slot &slot)
    {
        if (index >= world_save_crate_slot_count)
            return false;
        const net::ProtocolVersion protocol =
            online_host_ != nullptr ? host_settings_.protocol : online_client_protocol_;
        net::InventorySlotPayload item;
        if (!net::inventory_slot_to_payload(slot, protocol, item))
            return false;
        net::ItemCratePayload crate{{location[0], location[1], location[2]},
                                    static_cast<std::int32_t>(index),
                                    std::move(item)};
        net::PayloadError payload_error{};
        const auto payload = net::encode_item_crate_payload(crate, protocol, payload_error);
        if (!payload.has_value())
            return false;
        if (online_host_ != nullptr && online_host_->running() &&
            !online_host_->apply_authoritative_crate_item(crate))
            return false;
        std::string error;
        if (!publish_online_broadcast("ItemCrateMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.crate-item.failure=" << error << '\n';
            return false;
        }
        return true;
    }

    void publish_crate_differences(const CrateKey &location, const CrateSlots &before)
    {
        const auto found = crate_inventories_.find(location);
        if (found == crate_inventories_.end())
            return;
        for (std::size_t index{}; index < found->second.size(); ++index)
        {
            net::InventorySlotPayload old_item;
            net::InventorySlotPayload new_item;
            const net::ProtocolVersion protocol =
                online_host_ != nullptr ? host_settings_.protocol : online_client_protocol_;
            if (!net::inventory_slot_to_payload(before[index], protocol, old_item) ||
                !net::inventory_slot_to_payload(found->second[index], protocol, new_item) ||
                old_item == new_item)
                continue;
            if (!publish_local_crate_item(location, index, found->second[index]))
                std::cerr << "network.local.crate-diff.failure index=" << index << '\n';
        }
    }

    void publish_local_crate_destroy(const CrateKey &location)
    {
        const net::IntVector3 crate_location{location[0], location[1], location[2]};
        net::PayloadError payload_error{};
        const auto payload = net::encode_int_vector_payload(crate_location, payload_error);
        if (!payload.has_value())
            return;
        if (online_host_ != nullptr && online_host_->running() &&
            !online_host_->destroy_authoritative_crate(crate_location))
            return;
        std::string error;
        if (!publish_online_broadcast("DestroyCrateMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.destroy-crate.failure=" << error << '\n';
    }

    void publish_local_custom_door_destroy(const CrateKey &location,
                                           world::BlockType destroyed_piece)
    {
        const auto found = custom_doors_.find(location);
        if (found == custom_doors_.end())
            return;
        const net::ProtocolVersion protocol =
            online_host_ != nullptr ? host_settings_.protocol : online_client_protocol_;
        if (online_gameplay_active() && protocol == net::ProtocolVersion::modern_v4)
        {
            const net::DestroyCustomBlockPayload custom{{location[0], location[1], location[2]},
                                                        static_cast<std::uint8_t>(destroyed_piece)};
            net::PayloadError payload_error{};
            const auto payload = net::encode_destroy_custom_block_payload(custom, payload_error);
            bool authoritative = true;
            if (online_host_ != nullptr && online_host_->running())
                authoritative = online_host_->destroy_authoritative_custom_block(custom.location);
            std::string error{"payload-encode-failed"};
            if (!authoritative)
                error = "authoritative snapshot rejected the door";
            if (!payload.has_value() || !authoritative ||
                !publish_online_broadcast("DestroyCustomBlockMessage", *payload,
                                          net::MessageDeliveryPolicy::reliable_ordered, error))
            {
                std::cerr << "network.local.destroy-custom-block.failure=" << error << '\n';
            }
        }
        custom_doors_.erase(found);
        dirty_ = true;
    }

    [[nodiscard]] bool publish_local_inventory_store(bool final_save)
    {
        if (online_client_ == nullptr || !online_client_joined_ || !online_client_->connected() ||
            online_bootstrap_client_ != nullptr)
            return true;
        net::PlayerInventoryPayload visible_inventory;
        if (!net::player_inventory_to_payload(inventory_, online_client_protocol_,
                                              visible_inventory))
            return false;
        net::PlayerInventoryPayload inventory = std::move(visible_inventory);
        if (online_inventory_payload_.has_value() &&
            online_inventory_payload_->trays.size() == inventory.trays.size())
        {
            net::PlayerInventoryPayload preserved = *online_inventory_payload_;
            preserved.storage = inventory.storage;
            preserved.trays = inventory.trays;
            inventory = std::move(preserved);
        }
        online_inventory_payload_ = inventory;
        net::PayloadError payload_error{};
        const auto payload = net::encode_inventory_store_payload(
            {inventory, final_save}, online_client_protocol_, payload_error);
        if (!payload.has_value())
            return false;
        std::string error;
        if (!publish_online_broadcast("InventoryStoreOnServerMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.inventory-store.failure=" << error << '\n';
            return false;
        }
        return true;
    }

    void publish_host_time_of_day()
    {
        if (online_host_ == nullptr || !online_host_->running())
            return;
        net::PayloadError payload_error{};
        const auto payload = net::encode_time_of_day_payload(day_night_.day(), payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("TimeOfDayMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.host.time-of-day.failure=" << error << '\n';
    }

    void publish_local_presence_response()
    {
        if (online_client_ == nullptr || !online_client_joined_ || !online_client_->connected() ||
            online_bootstrap_client_ != nullptr)
            return;
        const net::LegacyGamerIdentity *local = online_client_->local_gamer();
        if (local == nullptr)
            return;
        net::PlayerExistsPayload presence;
        presence.gamer_tag = local->gamer_tag;
        presence.player_id = local->player_id;
        net::PayloadError payload_error{};
        const auto payload = net::encode_player_exists_payload(presence, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("PlayerExistsMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
        {
            std::cerr << "network.local.presence-response.failure=" << error << '\n';
            return;
        }
        const std::int64_t connected_ticks = static_cast<std::int64_t>(
            std::max(0.0, static_cast<double>(world_elapsed_)) * 10'000'000.0);
        const auto connected_payload =
            net::encode_time_connected_payload(connected_ticks, payload_error);
        if (!connected_payload.has_value() ||
            !publish_online_broadcast("TimeConnectedMessage", *connected_payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.time-connected.failure=" << error << '\n';
        // The recovered handler follows its presence response with the current
        // carried item so the newcomer does not wait for the next selection.
        last_online_carried_item_.reset();
        sync_selected_item();
        net::CrateFocusPayload focus;
        if (active_crate_.has_value())
        {
            focus.location = {(*active_crate_)[0], (*active_crate_)[1], (*active_crate_)[2]};
            if (inventory_focus_ >= crate_slot_base)
            {
                const std::size_t slot = inventory_focus_ - crate_slot_base;
                focus.item_x = static_cast<std::int32_t>(slot % 8U);
                focus.item_y = static_cast<std::int32_t>(slot / 8U);
            }
        }
        const auto focus_payload = net::encode_crate_focus_payload(focus, payload_error);
        if (!focus_payload.has_value() ||
            !publish_online_broadcast("CrateFocusMessage", *focus_payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.crate-focus.failure=" << error << '\n';
    }

    void publish_local_player_update(const world::PlayerInput &input, float fixed_delta)
    {
        const bool online = (online_host_ != nullptr && online_host_->running()) ||
                            (online_client_ != nullptr && online_client_joined_ &&
                             online_client_->connected() && online_bootstrap_client_ == nullptr);
        if (!online)
        {
            online_player_update_timer_ = 0.0F;
            return;
        }
        online_player_update_timer_ -= fixed_delta;
        const bool important = player_.state().using_tool || gun_reloading_ ||
                               player_life_ != PlayerLife::alive;
        if (online_player_update_timer_ > 0.0F)
            return;
        online_player_update_timer_ = important ? 1.0F / 30.0F : 1.0F / 20.0F;

        net::LocalPlayerReplicationInput replication;
        replication.position = player_.state().position;
        replication.velocity = player_.state().velocity;
        replication.yaw = player_.state().yaw;
        replication.torso_pitch = player_.state().pitch;
        replication.movement_x = input.strafe;
        replication.movement_y = input.forward;
        replication.animation_mode = item_player_animation_mode(inventory_.selected_item());
        replication.using_tool = player_.state().using_tool;
        replication.dead = player_life_ != PlayerLife::alive;
        replication.shouldering = shouldering_;
        replication.reloading = gun_reloading_;
        replication.throwing_grenade = grenade_animation_time_ > 0.0F;
        replication.ready_to_throw_grenade = grenade_cooking_;
        const net::ProtocolVersion protocol =
            online_host_ != nullptr ? host_settings_.protocol : online_client_protocol_;
        net::PlayerUpdatePayload update;
        net::PayloadError payload_error{};
        if (!net::make_player_update(replication, protocol, update, payload_error))
            return;
        const auto payload = net::encode_player_update_payload(update, protocol, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("PlayerUpdateMessage", *payload,
                                      net::player_update_delivery(update), error))
            std::cerr << "network.local.player-update.failure=" << error << '\n';
    }

    void remember_online_player(std::uint8_t gid, std::string gamer_tag)
    {
        if ((gid == 0U && online_host_ != nullptr) ||
            (online_client_ != nullptr && gid == online_local_gid_))
            return;
        OnlineRemotePlayer &player = online_remote_players_[gid];
        if (!gamer_tag.empty())
            player.gamer_tag = std::move(gamer_tag);
    }

    [[nodiscard]] bool announce_online_player(std::uint8_t gid, std::string gamer_tag)
    {
        if ((gid == 0U && online_host_ != nullptr) ||
            (online_client_ != nullptr && gid == online_local_gid_))
            return true;
        const auto found = online_remote_players_.find(gid);
        if (found == online_remote_players_.end())
            return false;
        // The stock game constructs the player from the authenticated network
        // sender and does not use the identity echoed inside PlayerExists as an
        // authorization check. Prefer the admitted roster identity and use
        // the embedded tag only when the provider did not supply one.
        if (found->second.gamer_tag.empty() && !gamer_tag.empty())
            found->second.gamer_tag = std::move(gamer_tag);
        found->second.presence_announced = true;
        return true;
    }

    [[nodiscard]] bool online_player_presence_ready(std::uint8_t gid) const noexcept
    {
        const auto found = online_remote_players_.find(gid);
        return found != online_remote_players_.end() && found->second.presence_announced;
    }

    [[nodiscard]] static bool coalesced_pre_presence_message(
        std::string_view message) noexcept
    {
        return message == "PlayerUpdateMessage" || message == "ChangeCarriedItemMessage" ||
               message == "TimeConnectedMessage" || message == "CrateFocusMessage";
    }

    [[nodiscard]] static int pre_presence_replay_priority(std::string_view message) noexcept
    {
        if (message == "PlayerUpdateMessage")
            return 0;
        if (message == "ChangeCarriedItemMessage")
            return 1;
        if (message == "TimeConnectedMessage")
            return 2;
        if (message == "CrateFocusMessage")
            return 3;
        return 4;
    }

    void forget_pending_pre_presence(std::uint8_t gid) noexcept
    {
        const auto found = pending_pre_presence_packets_.find(gid);
        if (found == pending_pre_presence_packets_.end())
            return;
        for (const PendingPrePresencePacket &packet : found->second)
            pending_pre_presence_bytes_ -=
                std::min(pending_pre_presence_bytes_, packet.event.payload.size());
        pending_pre_presence_packets_.erase(found);
    }

    void forget_online_player(std::uint8_t gid) noexcept
    {
        forget_pending_pre_presence(gid);
        online_remote_players_.erase(gid);
    }

    void clear_online_remote_players() noexcept
    {
        pending_pre_presence_packets_.clear();
        pending_pre_presence_bytes_ = 0U;
        online_remote_players_.clear();
    }

    void queue_pre_presence_packet(const net::LegacySteamRuntimeClientEvent &event,
                                   std::string_view message_name)
    {
        constexpr std::size_t max_packets_per_player = 64U;
        constexpr std::size_t max_total_bytes = 256U * 1024U;
        auto &pending = pending_pre_presence_packets_[event.gid];
        if (coalesced_pre_presence_message(message_name))
        {
            const auto existing = std::ranges::find_if(
                pending, [&](const PendingPrePresencePacket &packet) {
                    return packet.message_name == message_name;
                });
            if (existing != pending.end())
            {
                const std::size_t replaced_size = existing->event.payload.size();
                const std::size_t next_bytes =
                    pending_pre_presence_bytes_ -
                    std::min(pending_pre_presence_bytes_, replaced_size) + event.payload.size();
                if (next_bytes <= max_total_bytes)
                {
                    existing->event = event;
                    pending_pre_presence_bytes_ = next_bytes;
                }
                return;
            }
        }
        if (pending.size() >= max_packets_per_player ||
            event.payload.size() > max_total_bytes -
                                       std::min(max_total_bytes, pending_pre_presence_bytes_))
        {
            std::cerr << "network.client.pre-presence.overflow-drop gid="
                      << static_cast<int>(event.gid) << " message=" << message_name << '\n';
            return;
        }
        pending.push_back({event, std::string{message_name}});
        pending_pre_presence_bytes_ += event.payload.size();
        std::cout << "network.client.pre-presence.queued gid=" << static_cast<int>(event.gid)
                  << " message=" << message_name << '\n';
    }

    [[nodiscard]] bool replay_pre_presence_packets(std::uint8_t gid, std::string &error)
    {
        const auto found = pending_pre_presence_packets_.find(gid);
        if (found == pending_pre_presence_packets_.end())
            return true;
        std::vector<PendingPrePresencePacket> pending = std::move(found->second);
        pending_pre_presence_packets_.erase(found);
        for (const PendingPrePresencePacket &packet : pending)
            pending_pre_presence_bytes_ -=
                std::min(pending_pre_presence_bytes_, packet.event.payload.size());
        std::ranges::stable_sort(pending, {}, [](const PendingPrePresencePacket &packet) {
            return pre_presence_replay_priority(packet.message_name);
        });
        std::cout << "network.client.pre-presence.replay gid=" << static_cast<int>(gid)
                  << " packets=" << pending.size() << '\n';
        for (const PendingPrePresencePacket &packet : pending)
            if (!handle_online_client_gameplay(packet.event, error))
                return false;
        return true;
    }

    [[nodiscard]] bool apply_online_player_update(std::uint8_t gid, net::ProtocolVersion protocol,
                                                  std::span<const std::uint8_t> payload)
    {
        net::PlayerUpdatePayload update;
        net::PayloadError payload_error{};
        if (!net::decode_player_update_payload(payload, protocol, update, payload_error))
            return false;
        const auto found = online_remote_players_.find(gid);
        if (found == online_remote_players_.end() || !found->second.presence_announced)
            return false;
        OnlineRemotePlayer &remote = found->second;
        const bool was_dead = remote.has_update && remote.state.dead;
        if (!net::apply_player_update(update, protocol, remote.state))
            return false;
        if (remote.state.dead && !was_dead)
            remote.death_animation_started = elapsed_;
        remote.last_update = elapsed_;
        remote.has_update = true;
        return true;
    }

    [[nodiscard]] bool apply_online_auxiliary_player_state(std::uint8_t gid,
                                                           const net::DecodedPacket &decoded)
    {
        net::PayloadError payload_error{};
        if (decoded.message->name == "ChangeCarriedItemMessage")
        {
            if (decoded.payload.size() != 2U)
                return false;
            const auto found = online_remote_players_.find(gid);
            if (found == online_remote_players_.end() || !found->second.presence_announced)
                return false;
            OnlineRemotePlayer &remote = found->second;
            remote.carried_item = static_cast<std::uint16_t>(decoded.payload[0]) |
                                  (static_cast<std::uint16_t>(decoded.payload[1]) << 8U);
        }
        else if (decoded.message->name == "TimeConnectedMessage")
        {
            const auto found = online_remote_players_.find(gid);
            if (found == online_remote_players_.end() || !found->second.presence_announced)
                return false;
            OnlineRemotePlayer &remote = found->second;
            if (!net::decode_time_connected_payload(decoded.payload, remote.connected_ticks,
                                                    payload_error))
                return false;
        }
        else if (decoded.message->name == "CrateFocusMessage")
        {
            net::CrateFocusPayload focus;
            if (!net::decode_crate_focus_payload(decoded.payload, focus, payload_error))
                return false;
            const auto found = online_remote_players_.find(gid);
            if (found == online_remote_players_.end() || !found->second.presence_announced)
                return false;
            OnlineRemotePlayer &remote = found->second;
            remote.focus_crate = focus.location;
            remote.focus_item_x = focus.item_x;
            remote.focus_item_y = focus.item_y;
        }
        return true;
    }

    [[nodiscard]] static bool melee_pvp_allows(net::PlayerVersusPlayer policy, bool victim_is_host,
                                               bool victim_is_friend_of_host) noexcept
    {
        return policy == net::PlayerVersusPlayer::everyone ||
               (policy == net::PlayerVersusPlayer::not_friends && !victim_is_host &&
                !victim_is_friend_of_host);
    }

    [[nodiscard]] static bool melee_source_near_player(const net::Vector3 &source,
                                                       world::Vec3 position) noexcept
    {
        // The authored box is +/-0.35 x 1.7. A bounded allowance absorbs one
        // state-snapshot interval without permitting arbitrary remote damage.
        return std::abs(source.x - position.x) <= 2.0F && source.y >= position.y - 1.5F &&
               source.y <= position.y + 3.2F && std::abs(source.z - position.z) <= 2.0F;
    }

    [[nodiscard]] bool validate_online_melee_sender(const net::MeleePlayerPayload &melee,
                                                    std::uint8_t sender_gid,
                                                    world::Vec3 victim_position,
                                                    net::ProtocolVersion protocol) const
    {
        const auto sender = online_remote_players_.find(sender_gid);
        if (sender == online_remote_players_.end() || !sender->second.has_update ||
            elapsed_ - sender->second.last_update > 10.0F || sender->second.state.dead ||
            sender->second.carried_item != static_cast<std::uint16_t>(melee.item_id))
            return false;
        InventoryItemId item{};
        if (!net::inventory_item_from_wire(melee.item_id, protocol, item) ||
            !item_is_melee_weapon(item))
            return false;
        const world::Vec3 eye{sender->second.state.position.x,
                              sender->second.state.position.y + world::PlayerSimulation::eye_height,
                              sender->second.state.position.z};
        const float source_dx = melee.damage_source.x - eye.x;
        const float source_dy = melee.damage_source.y - eye.y;
        const float source_dz = melee.damage_source.z - eye.z;
        return source_dx * source_dx + source_dy * source_dy + source_dz * source_dz <= 16.0F &&
               melee_source_near_player(melee.damage_source, victim_position);
    }

    [[nodiscard]] bool apply_online_crate_item(net::ProtocolVersion protocol,
                                               const net::ItemCratePayload &crate)
    {
        PlayerInventory::Slot slot;
        if (!net::inventory_slot_to_stack(crate.item, protocol, slot) || crate.index < 0 ||
            crate.index >= static_cast<std::int32_t>(world_save_crate_slot_count) ||
            !net::valid_network_block_location(crate.location))
            return false;
        const CrateKey location{crate.location.x, crate.location.y, crate.location.z};
        if (!crate_inventories_.contains(location) &&
            crate_inventories_.size() >= net::maximum_world_crates)
            return false;
        crate_inventories_[location][static_cast<std::size_t>(crate.index)] = std::move(slot);
        dirty_ = true;
        return true;
    }

    [[nodiscard]] static CrateKey lower_door_key(net::IntVector3 location,
                                                 world::BlockType block) noexcept
    {
        if (world::is_upper_door(block))
            --location.y;
        return {location.x, location.y, location.z};
    }

    [[nodiscard]] bool remember_custom_door(const CrateKey &key, std::uint8_t model_name,
                                            bool authoritative)
    {
        if (model_name < 1U || model_name > 4U)
            return false;
        const auto found = custom_doors_.find(key);
        if (found != custom_doors_.end())
            return found->second == model_name;
        if (custom_doors_.size() >= net::maximum_world_doors)
            return false;
        if (authoritative &&
            (online_host_ == nullptr || !online_host_->running() ||
             !online_host_->set_authoritative_custom_door({key[0], key[1], key[2]}, model_name)))
            return false;
        custom_doors_.emplace(key, model_name);
        dirty_ = true;
        return true;
    }

    [[nodiscard]] std::optional<std::uint8_t> online_carried_door_model(
        std::uint8_t gid, net::ProtocolVersion protocol) const
    {
        const auto remote = online_remote_players_.find(gid);
        if (remote == online_remote_players_.end() ||
            remote->second.carried_item >
                static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()))
            return std::nullopt;
        InventoryItemId carried{};
        if (!net::inventory_item_from_wire(static_cast<std::int16_t>(remote->second.carried_item),
                                           protocol, carried))
            return std::nullopt;
        return door_model_name(carried);
    }

    [[nodiscard]] bool validate_online_door_edit(std::uint8_t gid, net::ProtocolVersion protocol,
                                                 const net::AlterBlockPayload &edit,
                                                 std::optional<world::BlockType> previous) const
    {
        if (protocol != net::ProtocolVersion::modern_v4)
            return true;
        const world::BlockType block = static_cast<world::BlockType>(edit.block_type);
        const bool writes_door = world::is_lower_door(block) || world::is_upper_door(block);
        const bool replaces_door = previous.has_value() && (world::is_lower_door(*previous) ||
                                                            world::is_upper_door(*previous));
        if (!writes_door)
            return !replaces_door || block == world::BlockType::empty;
        if (previous.has_value() && *previous != world::BlockType::empty)
        {
            if (!replaces_door ||
                world::is_strong_door(*previous) != world::is_strong_door(block) ||
                world::is_lower_door(*previous) != world::is_lower_door(block))
                return false;
            switch (*previous)
            {
            case world::BlockType::lower_door_closed_z:
                return block == world::BlockType::lower_door_open_z;
            case world::BlockType::lower_door_closed_x:
                return block == world::BlockType::lower_door_open_x;
            case world::BlockType::lower_door_open_z:
                return block == world::BlockType::lower_door_closed_z;
            case world::BlockType::lower_door_open_x:
                return block == world::BlockType::lower_door_closed_x;
            case world::BlockType::strong_lower_door_closed_z:
                return block == world::BlockType::strong_lower_door_open_z;
            case world::BlockType::strong_lower_door_closed_x:
                return block == world::BlockType::strong_lower_door_open_x;
            case world::BlockType::strong_lower_door_open_z:
                return block == world::BlockType::strong_lower_door_closed_z;
            case world::BlockType::strong_lower_door_open_x:
                return block == world::BlockType::strong_lower_door_closed_x;
            case world::BlockType::upper_door_closed:
                return block == world::BlockType::upper_door_open;
            case world::BlockType::upper_door_open:
                return block == world::BlockType::upper_door_closed;
            case world::BlockType::strong_upper_door_closed:
                return block == world::BlockType::strong_upper_door_open;
            case world::BlockType::strong_upper_door_open:
                return block == world::BlockType::strong_upper_door_closed;
            default:
                return false;
            }
        }

        const bool lower_placement = block == world::BlockType::lower_door_closed_z ||
                                     block == world::BlockType::lower_door_closed_x ||
                                     block == world::BlockType::strong_lower_door_closed_z ||
                                     block == world::BlockType::strong_lower_door_closed_x;
        const bool upper_placement = block == world::BlockType::upper_door_closed ||
                                     block == world::BlockType::strong_upper_door_closed;
        const std::optional<std::uint8_t> model = online_carried_door_model(gid, protocol);
        if (!model.has_value() || ((*model == 1U) == world::is_strong_door(block)))
            return false;
        if (lower_placement)
            return true;
        if (!upper_placement)
            return false;
        const net::IntVector3 lower{edit.block_location.x, edit.block_location.y - 1,
                                    edit.block_location.z};
        const auto lower_block = online_block_at(lower);
        const CrateKey lower_key{lower.x, lower.y, lower.z};
        const auto identity = custom_doors_.find(lower_key);
        return lower_block.has_value() && world::is_lower_door(*lower_block) &&
               world::is_strong_door(*lower_block) == world::is_strong_door(block) &&
               identity != custom_doors_.end() && identity->second == *model;
    }

    [[nodiscard]] bool remember_online_door_placement(std::uint8_t gid,
                                                      net::ProtocolVersion protocol,
                                                      const net::AlterBlockPayload &edit,
                                                      std::optional<world::BlockType> previous,
                                                      bool authoritative)
    {
        if (protocol != net::ProtocolVersion::modern_v4)
            return true;
        const world::BlockType block = static_cast<world::BlockType>(edit.block_type);
        const bool closed_lower = block == world::BlockType::lower_door_closed_z ||
                                  block == world::BlockType::lower_door_closed_x ||
                                  block == world::BlockType::strong_lower_door_closed_z ||
                                  block == world::BlockType::strong_lower_door_closed_x;
        if (!closed_lower || (previous.has_value() && *previous != world::BlockType::empty))
            return true;
        const std::optional<std::uint8_t> model = online_carried_door_model(gid, protocol);
        if (!model.has_value() || ((*model == 1U) == world::is_strong_door(block)))
            return false;
        return remember_custom_door(
            {edit.block_location.x, edit.block_location.y, edit.block_location.z}, *model,
            authoritative);
    }

    void apply_online_destroy_crate(net::IntVector3 location)
    {
        if (!net::valid_network_block_location(location))
            return;
        const CrateKey key{location.x, location.y, location.z};
        crate_inventories_.erase(key);
        if (active_crate_.has_value() && *active_crate_ == key)
        {
            if (held_stack_.has_value())
            {
                const std::uint16_t remainder = inventory_.add_stack(*held_stack_);
                if (remainder != 0U)
                    drop_stack(held_stack_->with_count(remainder));
                held_stack_.reset();
            }
            active_crate_.reset();
            inventory_open_ = false;
            SDL_SetWindowRelativeMouseMode(window_, !inspector_view_);
        }
        dirty_ = true;
    }

    [[nodiscard]] bool apply_online_destroy_custom_block(
        const net::DestroyCustomBlockPayload &custom)
    {
        const world::BlockType block = static_cast<world::BlockType>(custom.block_type);
        if (!bounded_world_cell(custom.location) ||
            (!world::is_lower_door(block) && !world::is_upper_door(block)))
            return false;
        const CrateKey key{custom.location.x, custom.location.y, custom.location.z};
        custom_doors_.erase(key);
        dirty_ = true;
        return true;
    }

    [[nodiscard]] bool apply_online_spawner_update(const net::UpdateSpawnerPayload &update)
    {
        if (update.spawner_position.x < -1'000'000.0F || update.spawner_position.x > 1'000'000.0F ||
            update.spawner_position.y < -64.0F || update.spawner_position.y >= 64.0F ||
            update.spawner_position.z < -1'000'000.0F || update.spawner_position.z > 1'000'000.0F)
            return false;
        const CrateKey key{static_cast<int>(std::floor(update.spawner_position.x)),
                           static_cast<int>(std::floor(update.spawner_position.y)),
                           static_cast<int>(std::floor(update.spawner_position.z))};
        auto found = world_spawners_.find(key);
        if (!update.is_started && found == world_spawners_.end())
            return false;
        if (update.is_started && found == world_spawners_.end())
        {
            if (world_spawners_.size() >= net::maximum_world_spawners)
                return false;
            std::size_t ordinal{};
            std::string spawn_id;
            do
            {
                spawn_id = "spawner" + std::to_string(ordinal++);
            } while (std::ranges::any_of(
                world_spawners_, [&](const auto &spawner) { return spawner.second == spawn_id; }));
            world_spawners_.emplace(key, std::move(spawn_id));
        }
        if (update.is_started)
            active_world_spawners_.insert(key);
        else
            active_world_spawners_.erase(key);
        dirty_ = true;
        return true;
    }

    [[nodiscard]] static CrateKey spawner_key(net::IntVector3 location) noexcept
    {
        return {location.x, location.y, location.z};
    }

    [[nodiscard]] static net::IntVector3 spawner_location(const CrateKey &key) noexcept
    {
        return {key[0], key[1], key[2]};
    }

    [[nodiscard]] bool set_local_spawner_block(net::IntVector3 location, world::BlockType block)
    {
        if (world_preview_ == nullptr || !bounded_world_cell(location) ||
            !publish_local_block_edit(location.x, location.y, location.z, block))
            return false;
        world_preview_->remember_edit(location.x, location.y, location.z, block);
        if (world_preview_->region().contains(location.x, location.y, location.z))
        {
            world_preview_->region().set_block(location.x, location.y, location.z, block);
            world_preview_->rebuild_block_edit(location.x, location.y, location.z);
        }
        const CrateKey key = spawner_key(location);
        if (block == world::BlockType::loot_block || block == world::BlockType::lucky_loot_block)
        {
            active_world_spawners_.erase(key);
            remote_loot_claims_.erase(key);
        }
        dirty_ = true;
        return true;
    }

    [[nodiscard]] bool mark_local_spawner_started(net::IntVector3 location)
    {
        const CrateKey key = spawner_key(location);
        if (!world_spawners_.contains(key) && world_spawners_.size() >= net::maximum_world_spawners)
            return false;
        if (online_host_ != nullptr && online_host_->running() &&
            !online_host_->update_authoritative_spawner(location, true))
            return false;
        return apply_online_spawner_update(
            {{static_cast<float>(location.x), static_cast<float>(location.y),
              static_cast<float>(location.z)},
             true});
    }

    void mark_local_spawner_stopped(net::IntVector3 location)
    {
        active_world_spawners_.erase(spawner_key(location));
        if (online_host_ != nullptr && online_host_->running() &&
            world_spawners_.contains(spawner_key(location)))
            static_cast<void>(online_host_->update_authoritative_spawner(location, false));
        dirty_ = true;
    }

    void announce_local_spawner(std::string text, std::string_view title)
    {
        toast(std::string{title}, uppercase(text));
        if (!online_gameplay_active())
            return;
        net::PayloadError payload_error{};
        const auto payload = net::encode_chat_payload(text, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("BroadcastTextMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_ordered, error))
            std::cerr << "network.local.spawner-announcement.failure=" << error << '\n';
    }

    [[nodiscard]] std::string online_gamer_tag(std::uint8_t gid, std::string_view fallback) const
    {
        if (!online_gameplay_active() || gid == online_local_player_gid())
            return online_persona_name_.empty() ? std::string{fallback} : online_persona_name_;
        const auto found = online_remote_players_.find(gid);
        return found == online_remote_players_.end() || found->second.gamer_tag.empty()
                   ? std::string{fallback}
                   : found->second.gamer_tag;
    }

    void prune_remote_spawner_triggers(net::PeerId peer)
    {
        const auto found = pending_remote_spawner_triggers_.find(peer);
        if (found == pending_remote_spawner_triggers_.end())
            return;
        std::erase_if(found->second, [this](const PendingRemoteSpawnerTrigger &trigger) {
            return trigger.expires_at < elapsed_;
        });
        if (found->second.empty())
            pending_remote_spawner_triggers_.erase(found);
    }

    void remember_remote_spawner_trigger(net::PeerId peer)
    {
        prune_remote_spawner_triggers(peer);
        auto &pending = pending_remote_spawner_triggers_[peer];
        if (pending.size() >= 64U)
            pending.erase(pending.begin());
        pending.push_back({elapsed_ + 10.0F, std::nullopt});
    }

    [[nodiscard]] static bool is_source_spawner_clear_location(const CrateKey &source,
                                                               net::IntVector3 mutation) noexcept
    {
        constexpr int radius = world::SpawnerSimulation::clear_radius;
        return mutation.y >= source[1] + 1 && mutation.y < source[1] + radius + 1 &&
               mutation.x >= source[0] - radius && mutation.x < source[0] + radius &&
               mutation.z >= source[2] - radius && mutation.z < source[2] + radius;
    }

    [[nodiscard]] std::optional<CrateKey> infer_remote_spawner_source(
        net::IntVector3 mutation) const
    {
        constexpr int radius = world::SpawnerSimulation::clear_radius;
        const net::IntVector3 first_packet_source{mutation.x + radius, mutation.y - 1,
                                                  mutation.z + radius};
        const auto first_block = online_block_at(first_packet_source);
        if (first_block.has_value() && world::is_spawner_clickable(*first_block))
            return spawner_key(first_packet_source);
        for (int y = mutation.y - 1; y >= mutation.y - radius; --y)
            for (int x = mutation.x - (radius - 1); x <= mutation.x + radius; ++x)
                for (int z = mutation.z - (radius - 1); z <= mutation.z + radius; ++z)
                {
                    const net::IntVector3 candidate{x, y, z};
                    const auto block = online_block_at(candidate);
                    const CrateKey key = spawner_key(candidate);
                    if (block.has_value() && world::is_spawner_clickable(*block) &&
                        is_source_spawner_clear_location(key, mutation))
                        return key;
                }
        return std::nullopt;
    }

    [[nodiscard]] bool capture_remote_spawner_clear(net::PeerId peer, net::IntVector3 mutation)
    {
        prune_remote_spawner_triggers(peer);
        const auto found = pending_remote_spawner_triggers_.find(peer);
        if (found == pending_remote_spawner_triggers_.end() || found->second.empty())
            return false;
        PendingRemoteSpawnerTrigger &trigger = found->second.back();
        if (trigger.location.has_value())
            return is_source_spawner_clear_location(*trigger.location, mutation);
        trigger.location = infer_remote_spawner_source(mutation);
        return trigger.location.has_value();
    }

    [[nodiscard]] bool pop_remote_spawner_trigger(net::PeerId peer, net::IntVector3 location)
    {
        prune_remote_spawner_triggers(peer);
        const auto found = pending_remote_spawner_triggers_.find(peer);
        if (found == pending_remote_spawner_triggers_.end() || found->second.empty())
            return false;
        const PendingRemoteSpawnerTrigger &trigger = found->second.back();
        if (trigger.location.has_value() && *trigger.location != spawner_key(location))
            return false;
        found->second.pop_back();
        if (found->second.empty())
            pending_remote_spawner_triggers_.erase(found);
        return true;
    }

    [[nodiscard]] bool clear_local_spawner_surroundings(net::IntVector3 source, int radius)
    {
        if (radius != world::SpawnerSimulation::clear_radius)
            return false;
        struct ClearCell final
        {
            net::IntVector3 location;
            world::BlockType block{};
        };
        std::vector<ClearCell> cells;
        cells.reserve(static_cast<std::size_t>(radius * radius * radius * 4));
        for (int y = source.y + 1; y < source.y + radius + 1; ++y)
            for (int x = source.x - radius; x < source.x + radius; ++x)
                for (int z = source.z - radius; z < source.z + radius; ++z)
                {
                    const net::IntVector3 location{x, y, z};
                    const auto block = online_block_at(location);
                    if (!block.has_value())
                        return false;
                    if (!world::is_spawner_clickable(*block) &&
                        !world::is_spawner_preserved_loot(*block))
                        cells.push_back({location, *block});
                }
        for (const ClearCell &cell : cells)
            if (!set_local_spawner_block(cell.location, world::BlockType::empty))
                return false;
        return true;
    }

    [[nodiscard]] bool trigger_linked_local_spawners(net::IntVector3 source, int radius,
                                                     const world::SpawnerTarget &fallback)
    {
        if (radius != world::SpawnerSimulation::linked_spawner_radius)
            return false;
        for (int y = source.y; y < source.y + radius; ++y)
            for (int x = source.x - radius; x < source.x + radius; ++x)
                for (int z = source.z - radius; z < source.z + radius; ++z)
                {
                    const net::IntVector3 location{x, y, z};
                    if (location == source)
                        continue;
                    const auto block = online_block_at(location);
                    if (!block.has_value())
                        return false;
                    if (!world::is_spawner_clickable(*block))
                        continue;
                    const CrateKey key = spawner_key(location);
                    const auto existing = local_spawners_.find(key);
                    if (existing != local_spawners_.end() &&
                        !existing->second.simulation->can_start())
                        continue;
                    if (!start_local_spawner(location, fallback, true))
                        return false;
                }
        return true;
    }

    [[nodiscard]] std::vector<world::SpawnerTarget> nearby_local_spawner_targets(
        net::IntVector3 location) const
    {
        std::vector<world::SpawnerTarget> targets;
        const auto in_range = [location](world::Vec3 position) {
            const float x = position.x - static_cast<float>(location.x);
            const float y = position.y + 1.5F - static_cast<float>(location.y);
            const float z = position.z - static_cast<float>(location.z);
            return x * x + y * y + z * z <=
                   world::SpawnerSimulation::target_range * world::SpawnerSimulation::target_range;
        };
        const std::uint8_t local_gid =
            online_gameplay_active() ? online_local_player_gid() : std::uint8_t{};
        if (player_life_ == PlayerLife::alive && in_range(player_.state().position))
            targets.push_back({local_gid, local_gid,
                               online_persona_name_.empty() ? "OpenMinerZ" : online_persona_name_});
        for (const auto &[gid, remote] : online_remote_players_)
            if (remote.has_update && !remote.state.dead && elapsed_ - remote.last_update <= 10.0F &&
                in_range(remote.state.position) && !remote.gamer_tag.empty())
                targets.push_back({gid, gid, remote.gamer_tag});
        std::ranges::sort(targets, {}, &world::SpawnerTarget::gid);
        return targets;
    }

    [[nodiscard]] std::optional<std::int32_t> next_local_spawner_enemy_id(
        const world::SpawnerTarget &target)
    {
        for (std::size_t attempt{}; attempt <= 500U; ++attempt)
        {
            if (next_enemy_id_ >
                static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
                next_enemy_id_ = 0U;
            const std::uint32_t candidate = next_enemy_id_++;
            if (!enemy_horde_.find_enemy({target.gid, candidate}).has_value())
                return static_cast<std::int32_t>(candidate);
        }
        return std::nullopt;
    }

    [[nodiscard]] bool spawn_local_spawner_enemy(const CrateKey &source,
                                                 const world::SpawnerEnemyAction &action)
    {
        const net::SpawnEnemyPayload &message = action.payload;
        if (message.enemy_type >= world::enemy_type_count || message.enemy_id < 0 ||
            message.random_seed < 0 || message.player_name.empty() ||
            message.player_name.size() > 64U)
            return false;
        const world::EnemyIdentity identity{action.target.gid,
                                            static_cast<std::uint32_t>(message.enemy_id)};
        const world::EnemyInitialization initialization{
            static_cast<float>(message.initialization.slow_speed_milliseconds) / 1000.0F,
            static_cast<float>(message.initialization.fast_speed_milliseconds) / 1000.0F,
            static_cast<float>(message.initialization.emerge_speed_milliseconds) / 1000.0F,
            static_cast<float>(message.initialization.run_activation_milliseconds) / 1000.0F,
            static_cast<float>(message.initialization.normal_activation_milliseconds) / 1000.0F};
        if (!enemy_horde_.spawn_networked(
                static_cast<int>(message.enemy_type),
                {message.spawn_position.x, message.spawn_position.y, message.spawn_position.z},
                initialization, identity.enemy_id, message.random_seed, identity.target_gid))
            return false;

        net::PayloadError payload_error{};
        const net::ProtocolVersion protocol =
            online_gameplay_active() ? online_gameplay_protocol() : net::ProtocolVersion::modern_v4;
        const auto payload = net::encode_spawn_enemy_payload(message, protocol, payload_error);
        std::string error;
        if (!payload.has_value() ||
            !publish_online_broadcast("SpawnEnemyMessage", *payload,
                                      net::MessageDeliveryPolicy::reliable_unordered, error))
        {
            std::erase_if(enemy_horde_.enemies(), [&](const world::Enemy &enemy) {
                return enemy.target_gid == identity.target_gid && enemy.id == identity.enemy_id;
            });
            std::cerr << "network.local.spawner-enemy.failure=" << error << '\n';
            return false;
        }
        local_spawner_enemy_sources_[identity] = source;
        if (message.enemy_type == 51U)
            announce_local_spawner("The Lord of Hell Awakens! Prepare yourself!", "HELL LORD");
        return true;
    }

    void reset_local_spawner_enemies(const CrateKey &source)
    {
        std::vector<world::EnemyIdentity> identities;
        for (const auto &[identity, key] : local_spawner_enemy_sources_)
            if (key == source)
                identities.push_back(identity);
        for (const world::EnemyIdentity identity : identities)
            local_spawner_enemy_sources_.erase(identity);
        const bool host_authoritative = online_host_ != nullptr && online_host_->running();
        const std::uint8_t local_gid =
            online_gameplay_active() ? online_local_player_gid() : std::uint8_t{};
        for (const world::EnemyIdentity identity : identities)
        {
            if (!online_gameplay_active() || host_authoritative || identity.target_gid == local_gid)
                publish_local_enemy_target("EnemyGiveUpMessage", identity);
            else
                static_cast<void>(enemy_horde_.give_up_enemy(identity));
        }
    }

    [[nodiscard]] bool process_local_spawner_actions(const CrateKey &source,
                                                     std::vector<world::SpawnerAction> actions)
    {
        auto runtime = local_spawners_.find(source);
        if (runtime == local_spawners_.end() || runtime->second.simulation == nullptr)
            return false;
        const auto reset_after_failure = [&]() {
            auto reset = runtime->second.simulation->handle_enemy_removed();
            static_cast<void>(process_local_spawner_actions(source, std::move(reset)));
            mark_local_spawner_stopped(spawner_location(source));
        };
        for (const world::SpawnerAction &action : actions)
        {
            if (const auto *block = std::get_if<world::SpawnerBlockAction>(&action))
            {
                // The deployed action processor ignores a failed world write and
                // continues with the remaining transcript.
                static_cast<void>(set_local_spawner_block(block->location, block->block));
                continue;
            }
            if (const auto *enemy = std::get_if<world::SpawnerEnemyAction>(&action))
            {
                if (!spawn_local_spawner_enemy(source, *enemy))
                {
                    reset_after_failure();
                    return false;
                }
                continue;
            }
            if (const auto *completed = std::get_if<world::SpawnerCompletedAction>(&action))
            {
                ++defeated_spawner_count_;
                mark_local_spawner_stopped(spawner_location(source));
                const std::string winner = online_gamer_tag(
                    completed->last_killer_gid, runtime->second.fallback_target.gamer_tag);
                announce_local_spawner(winner + " has extinguished a monster spawner!",
                                       "SPAWNER EXTINGUISHED");
                continue;
            }
            if (std::holds_alternative<world::SpawnerResetBroadcastAction>(action))
            {
                announce_local_spawner(
                    "No available targets. Spawner reset. Stay closer to avoid this!",
                    "SPAWNER RESET");
                mark_local_spawner_stopped(spawner_location(source));
                continue;
            }
            if (const auto *reset = std::get_if<world::SpawnerResetEnemiesAction>(&action))
            {
                reset_local_spawner_enemies(spawner_key(reset->location));
                continue;
            }
            if (const auto *clear = std::get_if<world::SpawnerClearSurroundingsAction>(&action))
            {
                if (!clear_local_spawner_surroundings(clear->location, clear->radius))
                {
                    reset_after_failure();
                    return false;
                }
                continue;
            }
            if (const auto *linked = std::get_if<world::SpawnerTriggerLinkedAction>(&action))
            {
                if (!trigger_linked_local_spawners(linked->location, linked->radius,
                                                   runtime->second.fallback_target))
                {
                    reset_after_failure();
                    return false;
                }
                continue;
            }
            if (std::holds_alternative<world::SpawnerActivateAction>(action))
            {
                if (!process_local_spawner_actions(source,
                                                   runtime->second.simulation->complete_start()))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool start_local_spawner(net::IntVector3 location,
                                           const world::SpawnerTarget &fallback, bool linked)
    {
        if (world_preview_ == nullptr || !bounded_world_cell(location))
            return false;
        const CrateKey key = spawner_key(location);
        auto existing = local_spawners_.find(key);
        if (existing != local_spawners_.end() && !existing->second.simulation->can_start())
            return true;
        const auto block = online_block_at(location);
        if (!block.has_value())
            return false;
        const world::SpawnerDefinition *definition = world::find_spawner_definition(*block);
        if (definition == nullptr || definition->off_block != *block ||
            !world::is_spawner_clickable(*block))
            return false;
        if (existing == local_spawners_.end())
        {
            LocalSpawnerRuntime runtime;
            runtime.simulation = std::make_unique<world::SpawnerSimulation>(
                location, *definition, spawner_world_random_, spawner_enemy_random_);
            runtime.fallback_target = fallback;
            existing = local_spawners_.emplace(key, std::move(runtime)).first;
        }
        else
        {
            existing->second.fallback_target = fallback;
        }
        if (!existing->second.simulation->valid())
            return false;
        announce_local_spawner(linked ? "A linked spawner was also triggered!"
                                      : fallback.gamer_tag + " has triggered a monster spawner!",
                               linked ? "LINKED SPAWNER" : "SPAWNER TRIGGERED");
        auto actions = existing->second.simulation->start(defeated_spawner_count_,
                                                          static_cast<int>(difficulty_));
        if (actions.empty() || !process_local_spawner_actions(key, std::move(actions)) ||
            !mark_local_spawner_started(location))
        {
            auto reset = existing->second.simulation->handle_enemy_removed();
            static_cast<void>(process_local_spawner_actions(key, std::move(reset)));
            return false;
        }
        sound_bank_->play_click();
        return true;
    }

    void update_local_spawners(float fixed_delta)
    {
        if (local_spawners_.empty())
            return;
        std::vector<CrateKey> sources;
        sources.reserve(local_spawners_.size());
        for (const auto &[source, runtime] : local_spawners_)
            if (runtime.simulation != nullptr)
                sources.push_back(source);
        for (const CrateKey &source : sources)
        {
            auto runtime = local_spawners_.find(source);
            if (runtime == local_spawners_.end())
                continue;
            const net::IntVector3 location = spawner_location(source);
            const std::vector<world::SpawnerTarget> targets =
                nearby_local_spawner_targets(location);
            auto actions = runtime->second.simulation->advance(
                fixed_delta, targets, runtime->second.fallback_target,
                [this](const world::SpawnerTarget &target) {
                    return next_local_spawner_enemy_id(target);
                });
            static_cast<void>(process_local_spawner_actions(source, std::move(actions)));
        }
    }

    void notify_local_spawner_enemy_defeated(world::EnemyIdentity identity, std::uint8_t killer_gid)
    {
        const auto found = local_spawner_enemy_sources_.find(identity);
        if (found == local_spawner_enemy_sources_.end())
            return;
        const CrateKey source = found->second;
        local_spawner_enemy_sources_.erase(found);
        const auto runtime = local_spawners_.find(source);
        if (runtime == local_spawners_.end())
            return;
        static_cast<void>(process_local_spawner_actions(
            source, runtime->second.simulation->handle_enemy_defeated(killer_gid)));
    }

    void notify_local_spawner_enemy_removed(world::EnemyIdentity identity)
    {
        const auto found = local_spawner_enemy_sources_.find(identity);
        if (found == local_spawner_enemy_sources_.end())
            return;
        const CrateKey source = found->second;
        const auto runtime = local_spawners_.find(source);
        if (runtime == local_spawners_.end())
        {
            local_spawner_enemy_sources_.erase(found);
            return;
        }
        static_cast<void>(process_local_spawner_actions(
            source, runtime->second.simulation->handle_enemy_removed()));
    }

    [[nodiscard]] bool apply_online_block_removals(net::ProtocolVersion protocol,
                                                   const net::RemoveBlocksPayload &removal)
    {
        if (world_preview_ == nullptr || removal.blocks.empty())
            return false;
        for (const net::IntVector3 location : removal.blocks)
        {
            std::int32_t ignored{};
            if (!net::valid_network_block_location(location) ||
                !net::encode_delta_entry(location, 0U, protocol, ignored))
                return false;
        }
        std::vector<std::array<int, 3>> changed;
        changed.reserve(removal.blocks.size());
        auto &region = world_preview_->region();
        for (const net::IntVector3 location : removal.blocks)
        {
            if (region.contains(location.x, location.y, location.z))
            {
                const world::BlockType block = region.block(location.x, location.y, location.z);
                if (removal.do_dig_effects && block != world::BlockType::empty &&
                    block != world::BlockType::uninitialized)
                {
                    world::BlockBreakUpdate impact;
                    impact.x = location.x;
                    impact.y = location.y;
                    impact.z = location.z;
                    impact.block = block;
                    impact.intersection = {static_cast<float>(location.x) + 0.5F,
                                           static_cast<float>(location.y) + 0.5F,
                                           static_cast<float>(location.z) + 0.5F};
                    impact.normal_y = 1;
                    play_positional("GroundCrash", impact.intersection);
                    world_preview_->spawn_mining_impact(impact);
                }
                region.set_block(location.x, location.y, location.z, world::BlockType::empty);
            }
            world_preview_->remember_edit(location.x, location.y, location.z,
                                          world::BlockType::empty);
            changed.push_back({location.x, location.y, location.z});
        }
        std::erase_if(armed_explosives_, [&](const ArmedExplosive &armed) {
            return std::ranges::any_of(changed, [&](const std::array<int, 3> &location) {
                return location == std::array<int, 3>{armed.x, armed.y, armed.z};
            });
        });
        world_preview_->rebuild_block_edits(changed);
        dirty_ = true;
        return true;
    }

    void refresh_online_remote_visuals()
    {
        online_remote_visuals_.clear();
        online_remote_visuals_.reserve(online_remote_players_.size());
        for (const auto &[gid, remote] : online_remote_players_)
        {
            if (!remote.has_update || elapsed_ - remote.last_update > 10.0F)
                continue;
            const float extrapolation = std::clamp(elapsed_ - remote.last_update, 0.0F, 0.1F);
            const world::Vec3 position{
                remote.state.position.x + remote.state.velocity.x * extrapolation,
                remote.state.position.y + remote.state.velocity.y * extrapolation,
                remote.state.position.z + remote.state.velocity.z * extrapolation};
            InventoryItemId carried_item = InventoryItemId::bare_hands;
            if (remote.carried_item <=
                static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()))
            {
                static_cast<void>(net::inventory_item_from_wire(
                    static_cast<std::int16_t>(remote.carried_item),
                    online_gameplay_active() ? online_gameplay_protocol()
                                             : net::ProtocolVersion::modern_v4,
                    carried_item));
            }
            online_remote_visuals_.push_back(
                {gid, carried_item, position, remote.state.velocity, remote.state.yaw,
                  remote.state.torso_pitch, remote.state.movement_x, remote.state.movement_y,
                  remote.state.dead
                      ? std::max(0.0F, elapsed_ - remote.death_animation_started)
                      : elapsed_ + static_cast<float>(gid) * 0.173F,
                  remote.state.animation_mode, remote.state.using_tool, remote.state.dead,
                  remote.state.shouldering, remote.state.reloading,
                  remote.state.throwing_grenade, remote.state.ready_to_throw_grenade});
        }
        std::ranges::sort(online_remote_visuals_, {}, &RemotePlayerVisual::gid);
    }

    void pump_online_client()
    {
        if (online_client_operation_.valid() &&
            online_client_operation_.wait_for(std::chrono::milliseconds{0}) ==
                std::future_status::ready)
        {
            OnlineClientOperationResult result = online_client_operation_.get();
            const OnlineClientOperation completed = online_client_pending_;
            online_client_pending_ = OnlineClientOperation::none;
            if (!result.error.empty() || result.client == nullptr || !result.client->running())
            {
                const std::string detail =
                    result.error.empty() ? "STEAM HELPER STOPPED" : result.error;
                if (completed == OnlineClientOperation::discovery)
                {
                    join_browser_state_ = JoinBrowserState::failed;
                    join_browser_status_ = uppercase(detail);
                    online_persona_name_.clear();
                    steam_identity_verified_ = false;
                }
                if (completed != OnlineClientOperation::discovery || !online_discovery_quiet_)
                    toast(completed == OnlineClientOperation::join ? "STEAM JOIN FAILED"
                                                                   : "STEAM DISCOVERY FAILED",
                          uppercase(detail));
                if (completed == OnlineClientOperation::join)
                    online_join_stage_ = OnlineJoinStage::idle;
                online_client_.reset();
                online_client_joined_ = false;
                online_lobby_host_is_friend_ = false;
                online_bootstrap_client_.reset();
                clear_online_remote_players();
            }
            else
            {
                online_client_ = std::move(result.client);
                if (completed == OnlineClientOperation::discovery)
                {
                    online_persona_name_ = result.discovery.persona_name;
                    steam_identity_verified_ = true;
                    const std::string local_default = profile_name_ + "'S FRONTIER";
                    if (host_settings_.server_name == local_default)
                        host_settings_.server_name = online_persona_name_ + "'S FRONTIER";
                    join_browser_.replace(net::make_discovered_sessions(result.discovery));
                    selected_join_session_.reset();
                    join_password_.clear();
                    rebuild_join_menu_entries();
                    const std::size_t count = join_browser_.visible_indices().size();
                    join_browser_state_ = JoinBrowserState::ready;
                    join_browser_status_ =
                        count == 0U ? "STEAM AUTHENTICATED  /  NO LIVE CASTLEMINER Z LOBBIES ONLINE"
                                    : std::format("STEAM AUTHENTICATED  /  {} LIVE SESSION{}",
                                                  count, count == 1U ? "" : "S");
                    join_refresh_timer_ = 30.0F;
                    if (!online_discovery_quiet_)
                        toast("DISCOVERY COMPLETE",
                              count == 0U ? "NO CLASSIC OR MODERN STEAM LOBBIES REPORTED"
                                          : std::format("{} SESSION{} READY TO REVIEW", count,
                                                        count == 1U ? "" : "S"));
                    std::cout << "network.discovery.results=" << result.discovery.lobbies.size()
                              << '\n';
                    if (options_.open_join_details && !join_row_sessions_.empty())
                    {
                        selected_join_session_ = join_row_sessions_.front();
                        menu_.open(ui::MenuScreen::join_details);
                    }
                    options_.open_join_details = false;
                    if (options_.ui_join_probe_lobby.has_value())
                    {
                        const auto target = std::ranges::find_if(
                            join_row_sessions_, [this](std::size_t session_index) {
                                return session_index < join_browser_.sessions().size() &&
                                       join_browser_.sessions()[session_index].session_id ==
                                           *options_.ui_join_probe_lobby;
                            });
                        if (target == join_row_sessions_.end())
                        {
                            ui_join_probe_failed_ = true;
                            ui_join_probe_detail_ = "The requested lobby was not returned by live discovery";
                            std::cerr << "network.ui-join-probe.failure=" << ui_join_probe_detail_
                                      << '\n';
                            running_ = false;
                        }
                        else
                        {
                            menu_.set_selection(
                                static_cast<std::size_t>(target - join_row_sessions_.begin()),
                                settings_);
                            handle({ui::MenuCommand::select_join_session, true});
                            handle({ui::MenuCommand::connect_join_session, false});
                            ui_join_probe_started_ = online_client_pending_ == OnlineClientOperation::join;
                            if (!ui_join_probe_started_)
                            {
                                ui_join_probe_failed_ = true;
                                ui_join_probe_detail_ = "The real menu preflight refused the selected lobby";
                                std::cerr << "network.ui-join-probe.failure="
                                          << ui_join_probe_detail_ << '\n';
                                running_ = false;
                            }
                        }
                    }
                }
                else if (completed == OnlineClientOperation::join)
                {
                    online_client_joined_ = true;
                    online_terrain_server_gid_ = 0U;
                    online_inventory_save_timer_ = 10.0F;
                    online_inventory_payload_.reset();
                    online_persona_name_ = result.ready.persona_name;
                    steam_identity_verified_ = true;
                    online_client_protocol_ = static_cast<net::ProtocolVersion>(
                        *result.ready.lobby.session_properties[0]);
                    const std::int32_t remote_mode = *result.ready.lobby.session_properties[2];
                    const std::int32_t maximum_mode =
                        online_client_protocol_ == net::ProtocolVersion::modern_v4 ? 5 : 3;
                    online_client_mode_ = remote_mode >= 0 && remote_mode <= maximum_mode
                                              ? static_cast<GameMode>(remote_mode)
                                              : GameMode::survival;
                    online_client_difficulty_ =
                        static_cast<GameDifficulty>(*result.ready.lobby.session_properties[3]);
                    online_client_pvp_ = static_cast<net::PlayerVersusPlayer>(
                        *result.ready.lobby.session_properties[5]);
                    online_lobby_host_is_friend_ = result.ready.lobby.host_is_friend;
                    online_bootstrap_client_.reset();
                    online_join_stage_ = OnlineJoinStage::awaiting_admission;
                    if (!result.ready.lobby.name.empty())
                        online_join_server_name_ = uppercase(result.ready.lobby.name);
                    if (!result.ready.lobby.host_name.empty())
                        online_join_host_name_ = uppercase(result.ready.lobby.host_name);
                    online_join_detail_ = "STEAM TRANSPORT READY  /  WAITING FOR HOST ADMISSION";
                    toast("LOBBY JOINED", std::format("{}  /  WAITING FOR HOST ADMISSION",
                                                      result.ready.lobby.host_name));
                    std::cout << "network.client.joined lobby=" << result.ready.lobby.lobby_id
                              << " host_peer=" << result.ready.lobby.host_peer
                              << " local_peer=" << result.ready.local_peer << '\n';
                }
            }
            if (completed == OnlineClientOperation::discovery)
                online_discovery_quiet_ = false;
        }
        if (online_client_ == nullptr || !online_client_joined_)
            return;
        std::vector<net::LegacySteamRuntimeClientEvent> events;
        std::string error;
        if (!online_client_->pump(events, error))
        {
            toast("STEAM SESSION ENDED", uppercase(error));
            std::cerr << "network.client.failure=" << error << '\n';
            save_online_position();
            static_cast<void>(online_client_->stop(error));
            discard_remote_projectiles();
            discard_network_enemies(std::nullopt, online_local_gid_);
            discard_network_dragon(std::nullopt, online_local_gid_);
            clear_online_remote_players();
            online_remote_visuals_.clear();
            online_client_.reset();
            online_client_joined_ = false;
            online_lobby_host_is_friend_ = false;
            online_bootstrap_client_.reset();
            online_join_stage_ = OnlineJoinStage::idle;
            online_world_key_.clear();
            pending_chunk_reconciliations_.clear();
            requested_chunk_reconciliations_.clear();
            return;
        }
        for (const net::LegacySteamRuntimeClientEvent &event : events)
        {
            switch (event.kind)
            {
            case net::LegacySteamRuntimeClientEventKind::connected:
                online_join_stage_ = OnlineJoinStage::receiving_world;
                online_join_detail_ =
                    std::format("ADMITTED AS PLAYER {}  /  REQUESTING WORLD MANIFEST",
                                static_cast<int>(event.gid));
                toast("HOST ADMITTED CLIENT",
                      std::format("LOCAL GID {}  /  REQUESTING AUTHORITATIVE WORLD",
                                  static_cast<int>(event.gid)));
                std::cout << "network.client.admitted gid=" << static_cast<int>(event.gid) << '\n';
                if (!begin_online_client_bootstrap(event.gid, error))
                {
                    toast("WORLD BOOTSTRAP FAILED", uppercase(error));
                    std::cerr << "network.client.bootstrap.failure=" << error << '\n';
                    static_cast<void>(online_client_->stop(error));
                    discard_remote_projectiles();
                    discard_network_enemies(std::nullopt, online_local_gid_);
                    discard_network_dragon(std::nullopt, online_local_gid_);
                    clear_online_remote_players();
                    online_remote_visuals_.clear();
                    online_client_.reset();
                    online_client_joined_ = false;
                    online_lobby_host_is_friend_ = false;
                    online_bootstrap_client_.reset();
                    online_join_stage_ = OnlineJoinStage::idle;
                    return;
                }
                online_local_gid_ = event.gid;
                break;
            case net::LegacySteamRuntimeClientEventKind::peer_joined:
                remember_online_player(event.gid, event.gamer.gamer_tag);
                std::cout << "network.client.peer.joined gid=" << static_cast<int>(event.gid)
                          << " gamer=" << event.gamer.gamer_tag << '\n';
                break;
            case net::LegacySteamRuntimeClientEventKind::peer_left: {
                std::optional<world::EnemyIdentity> departed_spawner_enemy;
                for (const auto &[identity, source] : local_spawner_enemy_sources_)
                    if (identity.target_gid == event.gid)
                    {
                        departed_spawner_enemy = identity;
                        break;
                    }
                if (departed_spawner_enemy.has_value())
                    notify_local_spawner_enemy_removed(*departed_spawner_enemy);
                discard_remote_projectiles(event.gid);
                discard_network_enemies(event.gid);
                discard_network_dragon(event.gid);
                forget_online_player(event.gid);
                std::cout << "network.client.peer.left gid=" << static_cast<int>(event.gid) << '\n';
                break;
            }
            case net::LegacySteamRuntimeClientEventKind::session_properties_changed:
                std::cout << "network.client.session_properties.changed\n";
                break;
            case net::LegacySteamRuntimeClientEventKind::gameplay:
                if (!handle_online_client_gameplay(event, error))
                {
                    toast("WORLD BOOTSTRAP FAILED", uppercase(error));
                    std::cerr << "network.client.bootstrap.failure=" << error << '\n';
                    save_online_position();
                    static_cast<void>(online_client_->stop(error));
                    discard_remote_projectiles();
                    discard_network_enemies(std::nullopt, online_local_gid_);
                    discard_network_dragon(std::nullopt, online_local_gid_);
                    clear_online_remote_players();
                    online_remote_visuals_.clear();
                    online_client_.reset();
                    online_client_joined_ = false;
                    online_lobby_host_is_friend_ = false;
                    online_bootstrap_client_.reset();
                    online_join_stage_ = OnlineJoinStage::idle;
                    online_world_key_.clear();
                    pending_chunk_reconciliations_.clear();
                    requested_chunk_reconciliations_.clear();
                    return;
                }
                break;
            case net::LegacySteamRuntimeClientEventKind::rejected:
                std::cerr << "network.client.packet.rejected=" << event.detail << '\n';
                break;
            case net::LegacySteamRuntimeClientEventKind::transport_failure:
                break;
            }
        }
    }

    [[nodiscard]] bool send_online_bootstrap_packets(
        std::vector<net::AuthoritativeBootstrapClientSend> pending, std::uint8_t local_gid,
        std::string &error)
    {
        for (std::size_t index{}; index < pending.size(); ++index)
        {
            const net::AuthoritativeBootstrapClientSend &send = pending[index];
            if (!online_client_->send(send.recipient_gid, send.application_packet, send.delivery,
                                      error))
                return false;

            // DNA.Net.Message.DoSend delivers Echo messages to the local gamer
            // synchronously. Steam's broadcast relay intentionally excludes the sender,
            // so waiting for the host to return this packet deadlocks original servers.
            net::DecodedPacket decoded;
            net::PacketError packet_error{};
            if (!net::decode_packet(online_client_protocol_, send.application_packet, decoded,
                                    packet_error) ||
                decoded.message == nullptr)
            {
                error = "A bootstrap send could not be decoded for local echo";
                return false;
            }
            if (send.recipient_gid.has_value() || decoded.message->name != "PlayerExistsMessage")
                continue;

            net::BootstrapClientHandleResult echoed =
                online_bootstrap_client_->handle_packet(local_gid, send.application_packet);
            if (!echoed.handled())
            {
                error = std::format("Bootstrap local-echo error {} on {}",
                                    static_cast<int>(echoed.error), echoed.input_message);
                return false;
            }
            pending.insert(pending.end(), std::make_move_iterator(echoed.outbound.begin()),
                           std::make_move_iterator(echoed.outbound.end()));
        }
        error.clear();
        return true;
    }

    [[nodiscard]] bool begin_online_client_bootstrap(std::uint8_t local_gid, std::string &error)
    {
        const net::LegacyGamerIdentity *gamer = online_client_->local_gamer();
        if (gamer == nullptr)
        {
            error = "The admitted Steam client has no local gamer identity";
            return false;
        }
        try
        {
            net::AuthoritativeBootstrapClientOptions options;
            options.protocol = online_client_protocol_;
            options.local_gid = local_gid;
            options.avatar_description.assign(10U, 0U);
            options.gamer_tag = gamer->gamer_tag;
            options.player_id = gamer->player_id;
            online_bootstrap_client_ =
                std::make_unique<net::AuthoritativeBootstrapClient>(std::move(options));
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }
        net::BootstrapClientError bootstrap_error{};
        const auto request = online_bootstrap_client_->begin(bootstrap_error);
        if (!request.has_value())
        {
            error = std::format("Bootstrap request contract error {}",
                                static_cast<int>(bootstrap_error));
            return false;
        }
        return send_online_bootstrap_packets({*request}, local_gid, error);
    }

    [[nodiscard]] static bool drop_online_gameplay_packet(std::uint8_t sender_gid,
                                                          std::string_view message,
                                                          std::string_view reason,
                                                          std::string &error)
    {
        std::cerr << "network.client.gameplay.dropped sender_gid="
                  << static_cast<int>(sender_gid) << " message=" << message
                  << " reason=" << reason << '\n';
        error.clear();
        return true;
    }

    void install_online_world(WorldSaveData save)
    {
        reset_world_session_state();
        game_mode_ = online_client_mode_;
        difficulty_ = online_client_difficulty_;
        infinite_resources_ = save.infinite_resources;
        recreate_world(save.seed,
                       online_client_protocol_ == net::ProtocolVersion::modern_v4
                           ? world::TerrainProfile::modern
                           : world::TerrainProfile::classic);
        world_preview_->import_edits(save.edits);
        day_night_ = world::DayNightCycle{};
        inventory_ = std::move(save.inventory);
        // Admission selected the wire profile. Keep movement (including the
        // Modern air jump) and tray semantics tied to it even if an older host
        // sends a default-profile inventory payload.
        inventory_.set_profile(online_client_protocol_ == net::ProtocolVersion::modern_v4
                                   ? InventoryProfile::modern
                                   : InventoryProfile::classic);
        crate_inventories_ = std::move(save.crates);
        custom_doors_ = std::move(save.doors);
        world_spawners_ = std::move(save.spawners);
        active_world_spawners_.clear();
        sync_selected_item();
        active_world_path_.clear();
        // WorldInfo.LastPosition belongs to the authority's save. Restore this
        // Steam player's own last coordinate for this world ID when available;
        // a first-time visitor still enters at the terrain-safe world spawn.
        pending_load_position_.reset();
        pending_load_yaw_ = 2.3561945F;
        pending_load_pitch_ = -0.3F;
        pending_load_health_ = 100.0F;
        pending_load_is_online_ = false;
        if (!online_world_key_.empty())
        {
            const auto remembered = read_online_positions();
            const auto found = remembered.find(online_world_key_);
            if (found != remembered.end())
            {
                pending_load_position_ = found->second.position;
                pending_load_yaw_ = found->second.yaw;
                pending_load_pitch_ = found->second.pitch;
                pending_load_is_online_ = true;
                player_.state().position = found->second.position;
                world_preview_->recenter_on(player_.state());
            }
        }
        online_position_save_timer_ = 5.0F;
        settings_.game_mode = static_cast<int>(game_mode_);
        settings_.game_difficulty = static_cast<int>(difficulty_);
        dirty_ = true;
        sound_bank_->stop_music();
        menu_.open(ui::MenuScreen::world_preview);
        waiting_for_world_ = !world_preview_->preload_complete();
        online_join_stage_ =
            waiting_for_world_ ? OnlineJoinStage::preparing_world : OnlineJoinStage::idle;
        online_join_detail_ = "BUILDING VISIBLE TERRAIN  /  PREPARING FIRST FRAME";
        world_ready_frames_ = 0U;
        cancel_world_action();
        SDL_SetWindowRelativeMouseMode(window_, false);
        toast("AUTHORITATIVE WORLD RECEIVED",
              std::format("SEED {}  /  {}  /  {}", save.seed, game_mode_label(game_mode_),
                          net::protocol_label(online_client_protocol_)));
    }

    [[nodiscard]] bool handle_online_client_gameplay(
        const net::LegacySteamRuntimeClientEvent &event, std::string &error)
    {
        if (online_bootstrap_client_ != nullptr && !online_bootstrap_client_->complete())
        {
            net::BootstrapClientHandleResult result =
                online_bootstrap_client_->handle_packet(event.gid, event.payload);
            if (result.error != net::BootstrapClientError::unexpected_message)
            {
                if (!result.handled())
                {
                    error = std::format("Bootstrap contract error {} on {}",
                                        static_cast<int>(result.error), result.input_message);
                    return false;
                }
                if (result.input_message == "WorldInfoMessage")
                {
                    online_join_stage_ = OnlineJoinStage::syncing_profile;
                    online_join_detail_ = "WORLD MANIFEST VERIFIED  /  SYNCHRONIZING PLAYER DATA";
                }
                else if (result.input_message == "InventoryRetrieveFromServerMessage")
                {
                    online_join_stage_ = OnlineJoinStage::syncing_profile;
                    online_join_detail_ = "PLAYER INVENTORY VERIFIED  /  REQUESTING WORLD DELTAS";
                }
                else if (result.input_message == "ProvideDeltaListMessage")
                {
                    online_join_stage_ = OnlineJoinStage::syncing_chunks;
                    online_sync_chunks_received_ = 0U;
                    online_sync_chunks_total_ = result.outbound.size();
                    online_join_detail_ =
                        online_sync_chunks_total_ == 0U
                            ? "WORLD HAS NO DELTA CHUNKS  /  SNAPSHOT VERIFIED"
                            : std::format("0 OF {} AUTHORITATIVE WORLD CHUNKS VERIFIED",
                                          online_sync_chunks_total_);
                }
                else if (result.input_message == "ProvideChunkMessage")
                {
                    online_join_stage_ = OnlineJoinStage::syncing_chunks;
                    online_sync_chunks_received_ =
                        std::min(online_sync_chunks_received_ + 1U, online_sync_chunks_total_);
                    online_join_detail_ =
                        std::format("{} OF {} AUTHORITATIVE WORLD CHUNKS VERIFIED",
                                    online_sync_chunks_received_, online_sync_chunks_total_);
                }
                if (!send_online_bootstrap_packets(std::move(result.outbound), online_local_gid_,
                                                   error))
                    return false;
                if (result.completed)
                {
                    const auto &save = online_bootstrap_client_->world_save();
                    if (!save.has_value())
                    {
                        error = "The completed bootstrap did not expose an authoritative snapshot";
                        return false;
                    }
                    const auto &world_info = online_bootstrap_client_->world_info();
                    online_world_key_ = world_info.has_value() ? online_world_key(*world_info)
                                                               : std::string{};
                    online_inventory_payload_ = online_bootstrap_client_->native_inventory();
                    WorldSaveData installed = *save;
                    online_bootstrap_client_.reset();
                    install_online_world(std::move(installed));
                    std::cout << "network.client.bootstrap.complete edits="
                              << world_preview_->exported_edits().size() << '\n';
                }
                error.clear();
                return true;
            }
        }
        net::DecodedPacket decoded;
        net::PacketError packet_error{};
        if (!net::decode_packet(online_client_protocol_, event.payload, decoded, packet_error) ||
            decoded.message == nullptr)
        {
            error = "A validated runtime packet failed application decoding";
            return false;
        }
        const bool requires_announced_player =
            net::player_routed_message(decoded.message->name);
        // GID 0 is both the session authority and the host player's identity.
        // Only the original PlayerUpdate dispatch domain requires a Player
        // object; global enemy/pickup/system traffic is valid before presence.
        if (event.gid != online_local_gid_ && decoded.message->name != "PlayerExistsMessage" &&
            !online_player_presence_ready(event.gid) && requires_announced_player)
        {
            queue_pre_presence_packet(event, decoded.message->name);
            error.clear();
            return true;
        }
        if (decoded.message->name == "AppointServerMessage")
        {
            if (event.gid != 0U || decoded.payload.size() != 1U)
            {
                error = "An invalid terrain-server appointment crossed the host boundary";
                return false;
            }
            online_terrain_server_gid_ = decoded.payload.front();
            std::cout << "network.client.terrain-server gid="
                      << static_cast<int>(online_terrain_server_gid_) << '\n';
        }
        if (decoded.message->name == "ProvideChunkMessage")
        {
            net::ProvideChunkPayload chunk;
            net::PayloadError payload_error{};
            if (event.gid != online_terrain_server_gid_ ||
                !net::decode_provide_chunk_payload(decoded.payload, chunk, payload_error) ||
                !apply_authoritative_chunk(chunk))
            {
                error = "An unrequested or invalid authoritative chunk snapshot was received";
                return false;
            }
            std::cout << "network.client.chunk-reconciled corner=" << chunk.block_location.x
                      << ',' << chunk.block_location.z << " edits=" << chunk.delta.size() << '\n';
            error.clear();
            return true;
        }
        if (decoded.message->name == "KickMessage")
        {
            net::KickPayload kick;
            net::PayloadError payload_error{};
            if (event.gid != 0U || !net::decode_kick_payload(decoded.payload, kick, payload_error))
            {
                error = "An invalid kick packet crossed the host boundary";
                return false;
            }
            if (kick.player_id == online_local_gid_)
            {
                error = kick.banned ? "You were banned by the session host"
                                    : "You were kicked by the session host";
                return false;
            }
        }
        if (decoded.message->name == "ChatMessage")
        {
            std::string message;
            net::PayloadError payload_error{};
            if (net::decode_chat_payload(decoded.payload, message, payload_error))
                push_online_chat(std::move(message));
        }
        if (decoded.message->name == "BroadcastTextMessage")
        {
            std::string message;
            net::PayloadError payload_error{};
            if (net::decode_chat_payload(decoded.payload, message, payload_error))
                push_online_chat(std::move(message));
        }
        if (decoded.message->name == "PlayerExistsMessage")
        {
            net::PlayerExistsPayload presence;
            net::PayloadError payload_error{};
            if (!net::decode_player_exists_payload(decoded.payload, presence, payload_error))
            {
                error = "A validated remote presence failed application decoding";
                return false;
            }
            if (event.gid != online_local_gid_ &&
                !announce_online_player(event.gid, presence.gamer_tag))
            {
                error = "A remote presence did not match its admitted Steam identity";
                return false;
            }
            if (presence.request_response)
            {
                publish_local_presence_response();
                publish_existing_dragon(event.gid);
            }
            if (event.gid != online_local_gid_ &&
                !replay_pre_presence_packets(event.gid, error))
                return false;
        }
        if (decoded.message->name == "PlayerUpdateMessage" &&
            !apply_online_player_update(event.gid, online_client_protocol_, decoded.payload))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-out-of-bounds-player-update", error);
        if (!apply_online_auxiliary_player_state(event.gid, decoded))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-auxiliary-player-state", error);
        if (decoded.message->name == "DigMessage" &&
            !apply_online_dig_effect(event.gid, online_client_protocol_, decoded.payload, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "invalid-dig-presentation", error);
        if ((decoded.message->name == "AddExplosionEffectsMessage" ||
             decoded.message->name == "AddExplosiveFlashMessage" ||
             decoded.message->name == "DoorOpenCloseMessage") &&
            !apply_online_environment_effect(online_client_protocol_, decoded))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "invalid-environment-presentation", error);
        if (decoded.message->name == "MeleePlayerMessage")
        {
            net::MeleePlayerPayload melee;
            net::PayloadError payload_error{};
            if (!net::decode_melee_player_payload(decoded.payload, online_client_protocol_, melee,
                                                   payload_error) ||
                !melee_source_near_player(melee.damage_source, player_.state().position))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "stale-or-out-of-range-melee", error);
            if (melee_pvp_allows(online_client_pvp_, false, online_lobby_host_is_friend_))
            {
                InventoryItemId item{};
                if (!net::inventory_item_from_wire(melee.item_id, online_client_protocol_, item))
                    return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                       "unsupported-melee-item", error);
                apply_player_damage(
                    item_player_melee_damage(item) * 100.0F,
                    {melee.damage_source.x, melee.damage_source.y, melee.damage_source.z});
            }
        }
        if (decoded.message->name == "TimeOfDayMessage")
        {
            float authoritative_day{};
            net::PayloadError payload_error{};
            if (!net::decode_time_of_day_payload(decoded.payload, authoritative_day, payload_error))
            {
                error = "A validated time authority packet failed application decoding";
                return false;
            }
            day_night_ = world::DayNightCycle{authoritative_day};
            last_day_number_ = static_cast<int>(std::floor(authoritative_day));
        }
        if (decoded.message->name == "RestartLevelMessage" &&
            (event.gid != 0U || !apply_online_restart_level()))
        {
            error = "A restart-level packet failed host authority or application state";
            return false;
        }
        if (decoded.message->name == "CreatePickupMessage" &&
            !apply_online_create_pickup(event.gid, online_client_protocol_, decoded.payload))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "invalid-or-capacity-limited-pickup", error);
        if (decoded.message->name == "ConsumePickupMessage" &&
            !apply_online_consume_pickup(event.gid, online_client_protocol_, decoded.payload))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-pickup-consumption", error);
        if ((decoded.message->name == "GunshotMessage" ||
             decoded.message->name == "ShotgunShotMessage" ||
             decoded.message->name == "GrenadeMessage" ||
             decoded.message->name == "FireRocketMessage") &&
            !apply_online_combat_launch(event.gid, online_client_protocol_, decoded, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-invalid-combat-launch", error);
        if ((decoded.message->name == "DetonateGrenadeMessage" ||
             decoded.message->name == "DetonateRocketMessage" ||
             decoded.message->name == "DetonateExplosiveMessage") &&
            !apply_online_combat_detonation(event.gid, online_client_protocol_, decoded, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-invalid-combat-detonation", error);
        if (decoded.message->name == "SpawnEnemyMessage" &&
            !apply_online_spawn_enemy(event.gid, online_client_protocol_, decoded.payload, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-invalid-enemy-spawn", error);
        if ((decoded.message->name == "KillEnemyMessage" ||
             decoded.message->name == "SpeedUpEnemyMessage" ||
             decoded.message->name == "EnemyGiveUpMessage") &&
            !apply_online_enemy_event(event.gid, online_client_protocol_, decoded, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-invalid-enemy-event", error);
        if ((decoded.message->name == "SpawnDragonMessage" ||
             decoded.message->name == "ExistingDragonMessage" ||
             decoded.message->name == "UpdateDragonMessage" ||
             decoded.message->name == "DragonAttackMessage" ||
             decoded.message->name == "DetonateFireballMessage" ||
             decoded.message->name == "KillDragonMessage" ||
             decoded.message->name == "MigrateDragonMessage" ||
             decoded.message->name == "RemoveDragonMessage") &&
            !apply_online_dragon_message(event.gid, online_client_protocol_, decoded, false))
            return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                               "stale-or-invalid-dragon-state", error);
        if (decoded.message->name == "AlterBlockMessage" && world_preview_ != nullptr)
        {
            net::AlterBlockPayload edit;
            net::PayloadError payload_error{};
            if (!net::decode_alter_block_payload(decoded.payload, online_client_protocol_, edit,
                                                 payload_error) ||
                !net::valid_network_block_location(edit.block_location))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "out-of-bounds-block-edit", error);
            const auto block = static_cast<world::BlockType>(edit.block_type);
            const auto previous = online_block_at(edit.block_location);
            if (!remember_online_door_placement(event.gid, online_client_protocol_, edit, previous,
                                                false))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "stale-door-identity", error);
            world_preview_->remember_edit(edit.block_location.x, edit.block_location.y,
                                          edit.block_location.z, block);
            if (world_preview_->region().contains(edit.block_location.x, edit.block_location.y,
                                                  edit.block_location.z))
            {
                world_preview_->region().set_block(edit.block_location.x, edit.block_location.y,
                                                   edit.block_location.z, block);
                world_preview_->rebuild_block_edit(edit.block_location.x, edit.block_location.y,
                                                   edit.block_location.z);
            }
            dirty_ = true;
            schedule_authoritative_chunk_check(edit.block_location);
        }
        if (decoded.message->name == "ItemCrateMessage")
        {
            net::ItemCratePayload crate;
            net::PayloadError payload_error{};
            if (!net::decode_item_crate_payload(decoded.payload, online_client_protocol_, crate,
                                                 payload_error) ||
                !apply_online_crate_item(online_client_protocol_, crate))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "invalid-or-capacity-limited-crate-item", error);
        }
        if (decoded.message->name == "DestroyCrateMessage")
        {
            net::IntVector3 location;
            net::PayloadError payload_error{};
            if (!net::decode_int_vector_payload(decoded.payload, location, payload_error) ||
                !net::valid_network_block_location(location))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "out-of-bounds-crate-destruction", error);
            apply_online_destroy_crate(location);
        }
        if (decoded.message->name == "DestroyCustomBlockMessage")
        {
            net::DestroyCustomBlockPayload custom;
            net::PayloadError payload_error{};
            if (!net::decode_destroy_custom_block_payload(decoded.payload, custom, payload_error) ||
                !apply_online_destroy_custom_block(custom))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "invalid-custom-block-destruction", error);
        }
        if (decoded.message->name == "UpdateSpawnerMessage")
        {
            net::UpdateSpawnerPayload update;
            net::PayloadError payload_error{};
            if (event.gid != 0U ||
                !net::decode_update_spawner_payload(decoded.payload, update, payload_error) ||
                !apply_online_spawner_update(update))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "invalid-spawner-update", error);
        }
        if (decoded.message->name == "RemoveBlocksMessage")
        {
            net::RemoveBlocksPayload removal;
            net::PayloadError payload_error{};
            if (!net::decode_remove_blocks_payload(decoded.payload, removal, payload_error) ||
                !apply_online_block_removals(online_client_protocol_, removal))
                return drop_online_gameplay_packet(event.gid, decoded.message->name,
                                                   "invalid-batch-block-removal", error);
            for (const net::IntVector3 location : removal.blocks)
                schedule_authoritative_chunk_check(location);
        }
        std::cout << "network.client.gameplay.validated sender_gid=" << static_cast<int>(event.gid)
                  << " message=" << decoded.message->name << '\n';
        error.clear();
        return true;
    }

    void shutdown_online_client() noexcept
    {
        discard_remote_projectiles();
        discard_network_enemies(std::nullopt, online_local_gid_);
        discard_network_dragon(std::nullopt, online_local_gid_);
        try
        {
            save_online_position();
            static_cast<void>(publish_local_inventory_store(true));
        }
        catch (...)
        {
        }
        online_client_joined_ = false;
        online_local_gid_ = 0U;
        online_terrain_server_gid_ = 0U;
        online_client_pvp_ = net::PlayerVersusPlayer::off;
        online_lobby_host_is_friend_ = false;
        clear_online_remote_players();
        online_remote_visuals_.clear();
        online_bootstrap_client_.reset();
        online_inventory_payload_.reset();
        online_client_pending_ = OnlineClientOperation::none;
        online_join_stage_ = OnlineJoinStage::idle;
        online_join_elapsed_ = 0.0F;
        online_sync_chunks_received_ = 0U;
        online_sync_chunks_total_ = 0U;
        online_world_key_.clear();
        pending_chunk_reconciliations_.clear();
        requested_chunk_reconciliations_.clear();
        if (online_client_operation_.valid())
        {
            try
            {
                OnlineClientOperationResult result = online_client_operation_.get();
                if (result.client != nullptr)
                {
                    std::string ignored;
                    static_cast<void>(result.client->stop(ignored));
                }
            }
            catch (...)
            {
            }
        }
        if (online_client_ != nullptr)
        {
            std::string ignored;
            static_cast<void>(online_client_->stop(ignored));
            online_client_.reset();
        }
    }

    void begin_online_host(bool inventory_is_default)
    {
        if (!hosting_flow_ || online_host_ != nullptr || online_host_start_.valid())
            return;
        // Discovery owns the Steam client from app launch through the frontend.
        // Relinquish it only when the user actually commits to publishing a host.
        shutdown_online_client();
        inventory_.set_profile(host_settings_.protocol == net::ProtocolVersion::modern_v4
                                   ? InventoryProfile::modern
                                   : InventoryProfile::classic);
        sync_selected_item();
        net::LegacySteamRuntimeHostOptions host_options;
        host_options.settings = host_settings_;
        host_options.game_mode = static_cast<std::int32_t>(game_mode_);
        host_options.difficulty = static_cast<std::int32_t>(difficulty_);
        host_options.infinite_resources = infinite_resources_;
        host_options.helper_path =
            application_directory() / "SteamBridge" / "OpenMinerZ.SteamBridge32.exe";
        host_options.steam_api_path = installed_steam_api_path();
        host_options.trace_path = options_.network_trace_path;
        WorldSaveData bootstrap_save;
        bootstrap_save.seed = world_seed_;
        bootstrap_save.mode = game_mode_;
        bootstrap_save.difficulty = difficulty_;
        bootstrap_save.infinite_resources = infinite_resources_;
        bootstrap_save.day = day_night_.day();
        bootstrap_save.inventory = inventory_;
        bootstrap_save.crates = crate_inventories_;
        bootstrap_save.doors = custom_doors_;
        bootstrap_save.spawners = world_spawners_;
        bootstrap_save.edits = world_preview_->exported_edits();
        if (!inventory_is_default)
        {
            bootstrap_save.has_player = true;
            bootstrap_save.player_position =
                pending_load_position_.value_or(player_.state().position);
        }
        net::BootstrapWorldIdentity identity;
        identity.name =
            active_world_path_.empty() ? "OpenMinerZ Frontier" : active_world_path_.stem().string();
        std::uint64_t world_key =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(world_seed_));
        for (const unsigned char character : identity.name)
            world_key = (world_key ^ character) * 1099511628211ULL;
        for (std::size_t index{}; index < identity.world_id.size(); ++index)
        {
            identity.world_id[index] = static_cast<std::uint8_t>(world_key >> ((index & 7U) * 8U));
            if (index == 7U)
                world_key ^= 0x9E3779B97F4A7C15ULL;
        }
        net::AuthoritativeBootstrapSnapshot bootstrap_snapshot;
        net::BootstrapSnapshotError snapshot_error{};
        if (!net::make_authoritative_bootstrap_snapshot(bootstrap_save, host_settings_, identity,
                                                        inventory_is_default, bootstrap_snapshot,
                                                        snapshot_error))
        {
            hosting_flow_ = false;
            toast("HOST SNAPSHOT REJECTED",
                  std::format("BOOTSTRAP CONTRACT ERROR {}", static_cast<int>(snapshot_error)));
            return;
        }
        // A joining player starts from the stock default inventory. The host's
        // local save is not another player's inventory and must never cross
        // this authority boundary.
        bootstrap_snapshot.inventory = net::make_default_player_inventory(
            host_settings_.protocol, difficulty_ == GameDifficulty::hardcore);
        bootstrap_snapshot.inventory_is_default = true;
        host_options.bootstrap_snapshot = std::move(bootstrap_snapshot);
        net::SteamBridgeHostConfig checked_config;
        std::string error;
        if (host_options.steam_api_path.empty())
        {
            hosting_flow_ = false;
            toast("STEAM RUNTIME NOT FOUND",
                  "SET OPENMINERZ_STEAM_API TO THE OWNED X86 STEAM_API.DLL");
            return;
        }
        if (!net::make_steam_bridge_host_config(host_options, checked_config, error))
        {
            hosting_flow_ = false;
            toast("HOST TRANSPORT UNAVAILABLE", uppercase(std::move(error)));
            return;
        }
        toast("PUBLISHING STEAM LOBBY", "INITIALIZING THE OWNED 2013 X86 STEAM RUNTIME");
        online_host_start_ =
            std::async(std::launch::async, [options = std::move(host_options)]() mutable {
                OnlineHostStartResult result;
                result.host = std::make_unique<net::LegacySteamRuntimeHost>();
                if (!result.host->start(options, result.ready, result.error))
                    result.host.reset();
                return result;
            });
    }

    void pump_online_host()
    {
        if (online_host_start_.valid() &&
            online_host_start_.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready)
        {
            OnlineHostStartResult result = online_host_start_.get();
            if (!hosting_flow_)
            {
                if (result.host != nullptr)
                {
                    std::string ignored;
                    static_cast<void>(result.host->stop(ignored));
                }
            }
            else if (result.host == nullptr)
            {
                hosting_flow_ = false;
                toast("STEAM HOST FAILED", uppercase(std::move(result.error)));
            }
            else
            {
                online_host_ = std::move(result.host);
                online_lobby_id_ = result.ready.lobby_id;
                online_persona_name_ = result.ready.persona_name;
                steam_identity_verified_ = true;
                online_world_update_timer_ = 5.0F;
                toast("STEAM LOBBY ONLINE",
                      std::format("{}  /  LOBBY {}", result.ready.persona_name,
                                  result.ready.lobby_id));
                std::cout << "network.host.online lobby=" << result.ready.lobby_id
                          << " local_peer=" << result.ready.local_peer << '\n';
            }
        }
        if (online_host_ == nullptr)
            return;
        std::vector<net::LegacySteamRuntimeEvent> events;
        std::string error;
        if (!online_host_->pump(events, error))
        {
            toast("STEAM HOST STOPPED", uppercase(error));
            std::cerr << "network.host.failure=" << error << '\n';
            static_cast<void>(online_host_->stop(error));
            discard_remote_projectiles();
            discard_network_enemies(std::nullopt, 0U);
            discard_network_dragon(std::nullopt, 0U);
            online_gameplay_rate_limiter_.clear();
            clear_online_remote_players();
            online_remote_visuals_.clear();
            online_host_.reset();
            online_lobby_id_ = 0U;
            hosting_flow_ = false;
            return;
        }
        for (const net::LegacySteamRuntimeEvent &event : events)
        {
            switch (event.kind)
            {
            case net::LegacySteamRuntimeEventKind::peer_connected:
                if (const net::LegacyGamerIdentity *gamer =
                        online_host_->gamer_for_gid(event.inbound.sender_gid);
                    gamer != nullptr)
                    remember_online_player(event.inbound.sender_gid, gamer->gamer_tag);
                {
                    const std::array<std::uint8_t, 1U> terrain_server{0U};
                    std::string appointment_error;
                    if (!publish_online_broadcast("AppointServerMessage", terrain_server,
                                                  net::MessageDeliveryPolicy::reliable_ordered,
                                                  appointment_error))
                        std::cerr << "network.terrain-server.appointment.failure="
                                  << appointment_error << '\n';
                }
                toast("PLAYER CONNECTED", std::format("STEAM PEER {}", event.peer));
                std::cout << "network.peer.connected peer=" << event.peer
                          << " gid=" << static_cast<int>(event.inbound.sender_gid) << '\n';
                break;
            case net::LegacySteamRuntimeEventKind::peer_disconnected: {
                std::optional<world::EnemyIdentity> departed_local_spawner_enemy;
                for (const auto &[identity, source] : local_spawner_enemy_sources_)
                    if (identity.target_gid == event.inbound.sender_gid)
                    {
                        departed_local_spawner_enemy = identity;
                        break;
                    }
                if (departed_local_spawner_enemy.has_value())
                    notify_local_spawner_enemy_removed(*departed_local_spawner_enemy);
                std::vector<world::EnemyIdentity> orphaned_spawner_enemies;
                for (const auto &[identity, owner_gid] : remote_spawner_enemy_owners_)
                    if (owner_gid == event.inbound.sender_gid)
                        orphaned_spawner_enemies.push_back(identity);
                for (const world::EnemyIdentity identity : orphaned_spawner_enemies)
                {
                    remote_spawner_enemy_owners_.erase(identity);
                    publish_local_enemy_target("EnemyGiveUpMessage", identity);
                }
                discard_remote_projectiles(event.inbound.sender_gid);
                discard_network_enemies(event.inbound.sender_gid);
                discard_network_dragon(event.inbound.sender_gid);
                forget_online_player(event.inbound.sender_gid);
                online_gameplay_rate_limiter_.disconnect(event.peer);
                pending_remote_spawner_triggers_.erase(event.peer);
                std::erase_if(remote_spawner_owners_,
                              [&](const auto &owner) { return owner.second == event.peer; });
                std::erase_if(remote_loot_claims_,
                              [&](const auto &claim) { return claim.second.owner == event.peer; });
                toast("PLAYER LEFT", std::format("STEAM PEER {}", event.peer));
                std::cout << "network.peer.disconnected peer=" << event.peer << '\n';
                break;
            }
            case net::LegacySteamRuntimeEventKind::rejected:
                std::cerr << "network.packet.rejected peer="
                          << (event.audit.peer != 0U ? event.audit.peer : event.peer)
                          << " stage=" << static_cast<int>(event.audit.stage)
                          << " bytes=" << event.audit.input_length << " reason="
                          << (!event.audit.reason.empty() ? event.audit.reason : event.detail)
                          << '\n';
                break;
            case net::LegacySteamRuntimeEventKind::gameplay:
                handle_online_gameplay(event.inbound);
                break;
            case net::LegacySteamRuntimeEventKind::transport_failure:
                break;
            }
        }
    }

    void handle_online_gameplay(const net::LegacySteamHostInbound &inbound)
    {
        std::erase_if(remote_loot_claims_, [&](const auto &claim) {
            return claim.second.block_removed && claim.second.expires_at < world_elapsed_;
        });
        net::DecodedPacket decoded;
        net::PacketError packet_error{};
        if (!net::decode_packet(host_settings_.protocol, inbound.payload, decoded, packet_error) ||
            decoded.message == nullptr)
            return;
        const net::ClientGameplayDecision decision =
            net::assess_client_gameplay_authority(host_settings_.protocol, inbound);
        if (!decision.accepted())
        {
            std::cerr << "network.authority.rejected peer=" << inbound.peer
                      << " gid=" << static_cast<int>(inbound.sender_gid)
                      << " message=" << decoded.message->name
                      << " reason=" << net::client_gameplay_authority_error_label(decision.error)
                      << '\n';
            return;
        }
        if (online_host_ == nullptr ||
            !online_host_->gameplay_ready(inbound.peer, inbound.sender_gid))
        {
            std::cerr << "network.authority.pre-presence.rejected peer=" << inbound.peer
                      << " gid=" << static_cast<int>(inbound.sender_gid)
                      << " message=" << decoded.message->name << '\n';
            return;
        }
        if (const net::LegacyGamerIdentity *gamer =
                online_host_->gamer_for_gid(inbound.sender_gid);
            gamer == nullptr || !announce_online_player(inbound.sender_gid, gamer->gamer_tag))
        {
            std::cerr << "network.authority.presence-identity.rejected peer=" << inbound.peer
                      << " gid=" << static_cast<int>(inbound.sender_gid) << '\n';
            return;
        }
        const std::uint64_t rate_time =
            static_cast<std::uint64_t>(std::max(0.0F, elapsed_) * 1'000.0F);
        bool exact_spawner_trigger{};
        if (host_settings_.protocol == net::ProtocolVersion::modern_v4 &&
            decoded.message->name == "BroadcastTextMessage")
        {
            std::string text;
            net::PayloadError text_error{};
            const auto sender = online_remote_players_.find(inbound.sender_gid);
            exact_spawner_trigger =
                net::decode_chat_payload(decoded.payload, text, text_error) &&
                (text == "A linked spawner was also triggered!" ||
                 (sender != online_remote_players_.end() && !sender->second.gamer_tag.empty() &&
                  text == sender->second.gamer_tag + " has triggered a monster spawner!"));
        }
        bool spawner_clear_transcript{};
        if (host_settings_.protocol == net::ProtocolVersion::modern_v4 &&
            decoded.message->name == "AlterBlockMessage")
        {
            net::AlterBlockPayload edit;
            net::PayloadError edit_error{};
            spawner_clear_transcript =
                net::decode_alter_block_payload(decoded.payload, host_settings_.protocol, edit,
                                                edit_error) &&
                edit.block_type == static_cast<std::int32_t>(world::BlockType::empty) &&
                capture_remote_spawner_clear(inbound.peer, edit.block_location);
        }
        const std::string_view rate_message =
            spawner_clear_transcript ? std::string_view{"SpawnerClearAlterBlockMessage"}
                                     : decision.message;
        const bool rate_allowed =
            exact_spawner_trigger
                ? online_gameplay_rate_limiter_.allow_spawner_trigger(inbound.peer, rate_time)
                : online_gameplay_rate_limiter_.allow(inbound.peer, rate_message, rate_time);
        if (!rate_allowed)
        {
            std::cerr << "network.authority.rate-limited peer=" << inbound.peer
                      << " message=" << decoded.message->name << '\n';
            correct_rejected_block_mutation(inbound.peer, decoded);
            return;
        }
        if (exact_spawner_trigger)
            remember_remote_spawner_trigger(inbound.peer);

        std::optional<net::RequestPickupPayload> pending_pickup_request;

        if (decoded.message->name == "PlayerExistsMessage")
        {
            net::PlayerExistsPayload presence;
            net::PayloadError payload_error{};
            if (!net::decode_player_exists_payload(decoded.payload, presence, payload_error) ||
                !announce_online_player(inbound.sender_gid, std::move(presence.gamer_tag)))
            {
                std::cerr << "network.authority.presence-payload.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
        }
        if (decoded.message->name == "PlayerUpdateMessage" &&
            !apply_online_player_update(inbound.sender_gid, host_settings_.protocol,
                                        decoded.payload))
        {
            std::cerr << "network.authority.player-update.rejected peer=" << inbound.peer << '\n';
            return;
        }
        if (!apply_online_auxiliary_player_state(inbound.sender_gid, decoded))
        {
            std::cerr << "network.authority.auxiliary-player-state.rejected peer=" << inbound.peer
                      << '\n';
            return;
        }
        if (decoded.message->name == "DigMessage" &&
            !apply_online_dig_effect(inbound.sender_gid, host_settings_.protocol, decoded.payload,
                                     true))
        {
            std::cerr << "network.authority.dig-effect.rejected peer=" << inbound.peer << '\n';
            return;
        }
        if ((decoded.message->name == "AddExplosionEffectsMessage" ||
             decoded.message->name == "AddExplosiveFlashMessage" ||
             decoded.message->name == "DoorOpenCloseMessage") &&
            !apply_online_environment_effect(host_settings_.protocol, decoded))
        {
            std::cerr << "network.authority.environment-effect.rejected peer=" << inbound.peer
                      << '\n';
            return;
        }
        if (decoded.message->name == "MeleePlayerMessage")
        {
            net::MeleePlayerPayload melee;
            net::PayloadError payload_error{};
            world::Vec3 victim_position = player_.state().position;
            bool victim_dead = player_life_ != PlayerLife::alive;
            if (inbound.recipient_gid != 0U)
            {
                const auto victim = online_remote_players_.find(inbound.recipient_gid);
                if (victim == online_remote_players_.end() || !victim->second.has_update ||
                    elapsed_ - victim->second.last_update > 10.0F)
                {
                    std::cerr << "network.authority.melee-target.rejected peer=" << inbound.peer
                              << '\n';
                    return;
                }
                victim_position = victim->second.state.position;
                victim_dead = victim->second.state.dead;
            }
            if (!net::decode_melee_player_payload(decoded.payload, host_settings_.protocol, melee,
                                                  payload_error) ||
                victim_dead ||
                !melee_pvp_allows(host_settings_.player_versus_player, inbound.recipient_gid == 0U,
                                  false) ||
                !validate_online_melee_sender(melee, inbound.sender_gid, victim_position,
                                              host_settings_.protocol))
            {
                std::cerr << "network.authority.melee.rejected peer=" << inbound.peer << '\n';
                return;
            }
            if (inbound.recipient_gid == 0U)
            {
                InventoryItemId item{};
                if (!net::inventory_item_from_wire(melee.item_id, host_settings_.protocol, item))
                {
                    std::cerr << "network.authority.melee-item.rejected peer=" << inbound.peer
                              << '\n';
                    return;
                }
                apply_player_damage(
                    item_player_melee_damage(item) * 100.0F,
                    {melee.damage_source.x, melee.damage_source.y, melee.damage_source.z});
            }
        }
        if ((decoded.message->name == "GunshotMessage" ||
             decoded.message->name == "ShotgunShotMessage" ||
             decoded.message->name == "GrenadeMessage" ||
             decoded.message->name == "FireRocketMessage") &&
            !apply_online_combat_launch(inbound.sender_gid, host_settings_.protocol, decoded, true))
        {
            std::cerr << "network.authority.combat-launch.rejected peer=" << inbound.peer << '\n';
            return;
        }
        if ((decoded.message->name == "DetonateGrenadeMessage" ||
             decoded.message->name == "DetonateRocketMessage" ||
             decoded.message->name == "DetonateExplosiveMessage") &&
            !apply_online_combat_detonation(inbound.sender_gid, host_settings_.protocol, decoded,
                                            true))
        {
            std::cerr << "network.authority.combat-detonation.rejected peer=" << inbound.peer
                      << '\n';
            return;
        }
        if (decoded.message->name == "SpawnEnemyMessage" &&
            !apply_online_spawn_enemy(inbound.sender_gid, host_settings_.protocol, decoded.payload,
                                      true))
        {
            std::cerr << "network.authority.spawn-enemy.rejected peer=" << inbound.peer << '\n';
            return;
        }
        if ((decoded.message->name == "KillEnemyMessage" ||
             decoded.message->name == "SpeedUpEnemyMessage" ||
             decoded.message->name == "EnemyGiveUpMessage") &&
            !apply_online_enemy_event(inbound.sender_gid, host_settings_.protocol, decoded, true))
        {
            std::cerr << "network.authority.enemy-event.rejected peer=" << inbound.peer << '\n';
            return;
        }
        if ((decoded.message->name == "RequestDragonMessage" ||
             decoded.message->name == "ExistingDragonMessage" ||
             decoded.message->name == "UpdateDragonMessage" ||
             decoded.message->name == "DragonAttackMessage" ||
             decoded.message->name == "DetonateFireballMessage" ||
             decoded.message->name == "KillDragonMessage" ||
             decoded.message->name == "MigrateDragonMessage" ||
             decoded.message->name == "RemoveDragonMessage") &&
            !apply_online_dragon_message(inbound.sender_gid, host_settings_.protocol, decoded,
                                         true))
        {
            std::cerr << "network.authority.dragon-message.rejected peer=" << inbound.peer << '\n';
            return;
        }

        if (decoded.message->name == "CreatePickupMessage")
        {
            net::CreatePickupPayload pickup;
            net::PayloadError payload_error{};
            InventoryStack stack;
            const auto remote = online_remote_players_.find(inbound.sender_gid);
            if (!net::decode_create_pickup_payload(decoded.payload, host_settings_.protocol, pickup,
                                                   payload_error) ||
                !pickup_item_to_stack(pickup.item, host_settings_.protocol, stack) ||
                remote == online_remote_players_.end() || !remote->second.has_update)
            {
                std::cerr << "network.authority.create-pickup.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
            const float dx = pickup.spawn_position.x - remote->second.state.position.x;
            const float dy = pickup.spawn_position.y - (remote->second.state.position.y + 1.0F);
            const float dz = pickup.spawn_position.z - remote->second.state.position.z;
            const float velocity_squared = pickup.spawn_vector.x * pickup.spawn_vector.x +
                                           pickup.spawn_vector.y * pickup.spawn_vector.y +
                                           pickup.spawn_vector.z * pickup.spawn_vector.z;
            if (pickup.spawn_position.x < -1'000'000.0F || pickup.spawn_position.x > 1'000'000.0F ||
                pickup.spawn_position.y < -128.0F || pickup.spawn_position.y > 128.0F ||
                pickup.spawn_position.z < -1'000'000.0F || pickup.spawn_position.z > 1'000'000.0F ||
                dx * dx + dy * dy + dz * dz > 100.0F || velocity_squared > 400.0F ||
                !authorize_remote_loot_pickup(inbound.peer, pickup, stack) ||
                !world_preview_->spawn_network_pickup(
                    stack,
                    {pickup.spawn_position.x, pickup.spawn_position.y, pickup.spawn_position.z},
                    {pickup.spawn_vector.x, pickup.spawn_vector.y, pickup.spawn_vector.z},
                    pickup.pickup_id, inbound.sender_gid, pickup.dropped, pickup.display_on_pickup))
            {
                std::cerr << "network.authority.create-pickup.out-of-range-duplicate-or-full peer="
                          << inbound.peer << '\n';
                return;
            }
        }

        if (decoded.message->name == "RequestPickupMessage")
        {
            net::RequestPickupPayload request;
            net::PayloadError payload_error{};
            const auto remote = online_remote_players_.find(inbound.sender_gid);
            if (!net::decode_request_pickup_payload(decoded.payload, request, payload_error) ||
                remote == online_remote_players_.end() || !remote->second.has_update)
            {
                std::cerr << "network.authority.request-pickup.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
            const auto pickup =
                world_preview_->find_network_pickup(request.pickup_id, request.spawner_id);
            if (!pickup.has_value())
            {
                std::cerr << "network.authority.request-pickup.missing peer=" << inbound.peer
                          << '\n';
                return;
            }
            const float dx = pickup->position.x - remote->second.state.position.x;
            const float dy = pickup->position.y - (remote->second.state.position.y + 1.0F);
            const float dz = pickup->position.z - remote->second.state.position.z;
            if (dx * dx + dy * dy + dz * dz > 16.0F)
            {
                std::cerr << "network.authority.request-pickup.out-of-reach peer=" << inbound.peer
                          << '\n';
                return;
            }
            pending_pickup_request = request;
        }

        if (decoded.message->name == "InventoryStoreOnServerMessage")
        {
            net::InventoryStorePayload stored;
            net::PayloadError payload_error{};
            if (!net::decode_inventory_store_payload(decoded.payload, host_settings_.protocol,
                                                     stored, payload_error) ||
                !online_host_->store_authoritative_inventory(inbound.peer, stored.inventory))
            {
                std::cerr << "network.authority.inventory-store.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
        }

        if (decoded.message->name == "ItemCrateMessage")
        {
            net::ItemCratePayload crate;
            net::PayloadError payload_error{};
            const auto remote = online_remote_players_.find(inbound.sender_gid);
            if (!net::decode_item_crate_payload(decoded.payload, host_settings_.protocol, crate,
                                                payload_error) ||
                remote == online_remote_players_.end() || !remote->second.has_update)
            {
                std::cerr << "network.authority.crate-item.rejected peer=" << inbound.peer << '\n';
                return;
            }
            const auto crate_block = online_block_at(crate.location);
            if (!crate_block.has_value() || !world::is_container(*crate_block))
            {
                std::cerr << "network.authority.crate-item.non-container peer=" << inbound.peer
                          << '\n';
                return;
            }
            const float dx = static_cast<float>(crate.location.x) - remote->second.state.position.x;
            const float dy = static_cast<float>(crate.location.y) - remote->second.state.position.y;
            const float dz = static_cast<float>(crate.location.z) - remote->second.state.position.z;
            if (dx * dx + dy * dy + dz * dz > 100.0F ||
                !online_host_->apply_authoritative_crate_item(crate) ||
                !apply_online_crate_item(host_settings_.protocol, crate))
            {
                std::cerr << "network.authority.crate-item.out-of-reach-or-full peer="
                          << inbound.peer << '\n';
                return;
            }
        }

        if (decoded.message->name == "DestroyCrateMessage")
        {
            net::IntVector3 location;
            net::PayloadError payload_error{};
            if (!net::decode_int_vector_payload(decoded.payload, location, payload_error))
            {
                std::cerr << "network.authority.destroy-crate.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
            const auto crate_block = online_block_at(location);
            if (!crate_block.has_value() || !world::is_container(*crate_block) ||
                !online_host_->destroy_authoritative_crate(location))
            {
                std::cerr << "network.authority.destroy-crate.non-container-or-missing peer="
                          << inbound.peer << '\n';
                static_cast<void>(send_authoritative_block_correction(inbound.peer, location));
                return;
            }
            apply_online_destroy_crate(location);
        }

        if (decoded.message->name == "DestroyCustomBlockMessage")
        {
            net::DestroyCustomBlockPayload custom;
            net::PayloadError payload_error{};
            if (!net::decode_destroy_custom_block_payload(decoded.payload, custom, payload_error))
            {
                std::cerr << "network.authority.destroy-custom-block.rejected peer=" << inbound.peer
                          << '\n';
                return;
            }
            const world::BlockType block = static_cast<world::BlockType>(custom.block_type);
            const CrateKey key{custom.location.x, custom.location.y, custom.location.z};
            const auto door = custom_doors_.find(key);
            if (door == custom_doors_.end() ||
                (!world::is_lower_door(block) && !world::is_upper_door(block)) ||
                ((door->second == 1U) == world::is_strong_door(block)) ||
                !online_host_->destroy_authoritative_custom_block(custom.location) ||
                !apply_online_destroy_custom_block(custom))
            {
                std::cerr << "network.authority.destroy-custom-block.rejected peer=" << inbound.peer
                          << '\n';
                static_cast<void>(
                    send_authoritative_block_correction(inbound.peer, custom.location));
                return;
            }
        }

        if (decoded.message->name == "RemoveBlocksMessage")
        {
            net::RemoveBlocksPayload removal;
            net::PayloadError payload_error{};
            const auto remote = online_remote_players_.find(inbound.sender_gid);
            if (!net::decode_remove_blocks_payload(decoded.payload, removal, payload_error) ||
                removal.blocks.empty() || remote == online_remote_players_.end() ||
                !remote->second.has_update)
            {
                std::cerr << "network.authority.block-removal.rejected peer=" << inbound.peer
                          << '\n';
                if (!removal.blocks.empty())
                    correct_rejected_block_mutation(inbound.peer, decoded);
                return;
            }
            const net::IntVector3 first = removal.blocks.front();
            const float dx = static_cast<float>(first.x) - remote->second.state.position.x;
            const float dy = static_cast<float>(first.y) - remote->second.state.position.y;
            const float dz = static_cast<float>(first.z) - remote->second.state.position.z;
            if (dx * dx + dy * dy + dz * dz > 40'000.0F ||
                !online_host_->apply_authoritative_block_removals(removal.blocks) ||
                !apply_online_block_removals(host_settings_.protocol, removal))
            {
                std::cerr << "network.authority.block-removal.out-of-range-or-full peer="
                          << inbound.peer << '\n';
                correct_rejected_block_mutation(inbound.peer, decoded);
                return;
            }
        }

        if (decoded.message->name == "AlterBlockMessage" && world_preview_ != nullptr)
        {
            net::AlterBlockPayload edit;
            net::PayloadError payload_error{};
            if (!net::decode_alter_block_payload(decoded.payload, host_settings_.protocol, edit,
                                                 payload_error))
            {
                std::cerr << "network.authority.block-edit.rejected peer=" << inbound.peer << '\n';
                return;
            }
            const auto block = static_cast<world::BlockType>(edit.block_type);
            const auto current = online_block_at(edit.block_location);
            const world::SpawnerDefinition *spawner_definition =
                world::find_spawner_definition(block);
            bool assign_spawner_owner{};
            if (spawner_definition != nullptr)
            {
                const CrateKey key = spawner_key(edit.block_location);
                const auto owner = remote_spawner_owners_.find(key);
                if (block == spawner_definition->on_block && current.has_value() &&
                    *current == spawner_definition->off_block)
                {
                    if (!world::is_spawner_clickable(*current) ||
                        !pop_remote_spawner_trigger(inbound.peer, edit.block_location))
                    {
                        std::cerr << "network.authority.spawner-activation.rejected peer="
                                  << inbound.peer << '\n';
                        static_cast<void>(send_authoritative_block_correction(
                            inbound.peer, edit.block_location));
                        return;
                    }
                    assign_spawner_owner = true;
                }
                else if (owner == remote_spawner_owners_.end() || owner->second != inbound.peer ||
                         !current.has_value() ||
                         (*current != spawner_definition->on_block &&
                          *current != spawner_definition->dim_block &&
                          *current != spawner_definition->off_block))
                {
                    std::cerr << "network.authority.spawner-light-owner.rejected peer="
                              << inbound.peer << '\n';
                    static_cast<void>(send_authoritative_block_correction(
                        inbound.peer, edit.block_location));
                    return;
                }
            }
            const bool removes_loot = block == world::BlockType::empty && current.has_value() &&
                                      (*current == world::BlockType::loot_block ||
                                       *current == world::BlockType::lucky_loot_block);
            if (removes_loot)
            {
                const CrateKey key = spawner_key(edit.block_location);
                auto claim = remote_loot_claims_.find(key);
                if (claim != remote_loot_claims_.end() &&
                    (claim->second.block_removed || claim->second.block != *current))
                {
                    remote_loot_claims_.erase(claim);
                    claim = remote_loot_claims_.end();
                }
                if ((claim != remote_loot_claims_.end() && claim->second.owner != inbound.peer) ||
                    (claim == remote_loot_claims_.end() && remote_loot_claims_.size() >= 2048U))
                {
                    std::cerr << "network.authority.loot-block-owner-or-cap.rejected peer="
                              << inbound.peer << '\n';
                    static_cast<void>(send_authoritative_block_correction(
                        inbound.peer, edit.block_location));
                    return;
                }
            }
            const auto remote = online_remote_players_.find(inbound.sender_gid);
            if (!infinite_resources_)
            {
                if (remote == online_remote_players_.end() || !remote->second.has_update)
                {
                    std::cerr << "network.authority.block-edit.no-position peer=" << inbound.peer
                              << '\n';
                    static_cast<void>(send_authoritative_block_correction(
                        inbound.peer, edit.block_location));
                    return;
                }
                const float dx =
                    static_cast<float>(edit.block_location.x) - remote->second.state.position.x;
                const float dy =
                    static_cast<float>(edit.block_location.y) - remote->second.state.position.y;
                const float dz =
                    static_cast<float>(edit.block_location.z) - remote->second.state.position.z;
                if (dx * dx + dy * dy + dz * dz > 100.0F)
                {
                    std::cerr << "network.authority.block-edit.out-of-reach peer=" << inbound.peer
                              << '\n';
                    static_cast<void>(send_authoritative_block_correction(
                        inbound.peer, edit.block_location));
                    return;
                }
            }
            const CrateKey door_key{edit.block_location.x, edit.block_location.y,
                                    edit.block_location.z};
            const bool had_door_identity = custom_doors_.contains(door_key);
            if (!validate_online_door_edit(inbound.sender_gid, host_settings_.protocol, edit,
                                           current) ||
                !remember_online_door_placement(inbound.sender_gid, host_settings_.protocol, edit,
                                                current, true))
            {
                std::cerr << "network.authority.door-placement.identity-rejected peer="
                          << inbound.peer << '\n';
                static_cast<void>(send_authoritative_block_correction(
                    inbound.peer, edit.block_location));
                return;
            }
            if (!online_host_->apply_authoritative_block_edit(
                    edit.block_location, static_cast<std::uint8_t>(edit.block_type)))
            {
                if (!had_door_identity && custom_doors_.contains(door_key))
                {
                    custom_doors_.erase(door_key);
                    static_cast<void>(
                        online_host_->destroy_authoritative_custom_block(edit.block_location));
                }
                std::cerr << "network.authority.block-edit.snapshot-full peer=" << inbound.peer
                          << '\n';
                static_cast<void>(send_authoritative_block_correction(
                    inbound.peer, edit.block_location));
                return;
            }
            world_preview_->remember_edit(edit.block_location.x, edit.block_location.y,
                                          edit.block_location.z, block);
            if (world_preview_->region().contains(edit.block_location.x, edit.block_location.y,
                                                  edit.block_location.z))
            {
                world_preview_->region().set_block(edit.block_location.x, edit.block_location.y,
                                                   edit.block_location.z, block);
                world_preview_->rebuild_block_edit(edit.block_location.x, edit.block_location.y,
                                                   edit.block_location.z);
            }
            if (assign_spawner_owner)
                remote_spawner_owners_[spawner_key(edit.block_location)] = inbound.peer;
            const CrateKey edit_key = spawner_key(edit.block_location);
            if (removes_loot)
            {
                auto claim = remote_loot_claims_
                                 .try_emplace(edit_key, RemoteLootClaim{inbound.peer, *current, 0U,
                                                                        0U, 0.0F, false})
                                 .first;
                claim->second.block_removed = true;
                claim->second.expires_at = world_elapsed_ + 15.0F;
            }
            else if (block == world::BlockType::loot_block ||
                     block == world::BlockType::lucky_loot_block)
            {
                remote_loot_claims_.erase(edit_key);
            }
            if (removes_loot && active_world_spawners_.contains(edit_key))
                active_world_spawners_.erase(edit_key);
            dirty_ = true;
        }

        if (decoded.message->name == "ChatMessage" ||
            decoded.message->name == "BroadcastTextMessage")
        {
            std::string message;
            net::PayloadError payload_error{};
            if (!net::decode_chat_payload(decoded.payload, message, payload_error))
                return;
            for (char &character : message)
            {
                if (static_cast<unsigned char>(character) < 0x20U)
                    character = ' ';
            }
            if (message.size() > 96U)
                message.resize(96U);
            push_online_chat(message);
            std::cout << "network.chat peer=" << inbound.peer << " text=" << message << '\n';
        }
        if (decision.should_relay())
        {
            net::LegacySteamOutbound relay;
            relay.application_packet = inbound.payload;
            relay.delivery = inbound.delivery;
            relay.channel = inbound.channel;
            relay.sender_gid = inbound.sender_gid;
            relay.recipient_gid = inbound.recipient_gid;
            relay.client_relay = true;
            std::string error;
            if (!online_host_->send_authoritative(relay, error))
                std::cerr << "network.gameplay.relay.failure peer=" << inbound.peer
                          << " message=" << decoded.message->name << " reason=" << error << '\n';
        }
        if (pending_pickup_request.has_value() &&
            !consume_pickup_as_host(pending_pickup_request->pickup_id,
                                    pending_pickup_request->spawner_id, inbound.sender_gid))
            std::cerr << "network.authority.request-pickup.consume-failure peer=" << inbound.peer
                      << '\n';
        std::cout << "network.gameplay.accepted peer=" << inbound.peer
                  << " message=" << decoded.message->name << " relay=" << decision.should_relay()
                  << " consume=" << decision.should_consume() << '\n';
    }

    void shutdown_online_host() noexcept
    {
        hosting_flow_ = false;
        discard_remote_projectiles();
        discard_network_enemies(std::nullopt, 0U);
        discard_network_dragon(std::nullopt, 0U);
        online_gameplay_rate_limiter_.clear();
        pending_remote_spawner_triggers_.clear();
        remote_spawner_owners_.clear();
        remote_spawner_enemy_owners_.clear();
        remote_loot_claims_.clear();
        clear_online_remote_players();
        online_remote_visuals_.clear();
        if (online_host_start_.valid())
        {
            try
            {
                OnlineHostStartResult result = online_host_start_.get();
                if (result.host != nullptr)
                {
                    std::string ignored;
                    static_cast<void>(result.host->stop(ignored));
                }
            }
            catch (...)
            {
            }
        }
        if (online_host_ != nullptr)
        {
            std::string ignored;
            static_cast<void>(online_host_->stop(ignored));
            online_host_.reset();
        }
        online_lobby_id_ = 0U;
    }

    // ---- World persistence: one .omw text file per world under the settings
    // directory's Worlds folder (never inside the repo). ----

    [[nodiscard]] std::filesystem::path worlds_directory() const
    {
        return store_.path().parent_path() / "Worlds";
    }

    struct WorldSlot final
    {
        std::string name;
        std::string description;
        std::string owner;
        std::string saved_at;
        std::string mode;
        std::string difficulty;
        std::int32_t seed{};
        float day{};
        std::filesystem::path path;
        std::shared_ptr<SDL_Texture> preview;
    };

    [[nodiscard]] static std::filesystem::path world_preview_path(
        const std::filesystem::path &world_path)
    {
        std::filesystem::path preview = world_path;
        preview.replace_extension(".preview.bmp");
        return preview;
    }

    [[nodiscard]] static std::string world_saved_at(std::int64_t saved_unix_seconds,
                                                    std::filesystem::file_time_type written)
    {
        std::chrono::system_clock::time_point point;
        if (saved_unix_seconds > 0)
            point = std::chrono::system_clock::time_point{std::chrono::seconds{saved_unix_seconds}};
        else
            point = std::chrono::system_clock::now() +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        written - std::filesystem::file_time_type::clock::now());
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(point);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &timestamp);
#else
        localtime_r(&timestamp, &local);
#endif
        std::array<char, 32> formatted{};
        if (std::strftime(formatted.data(), formatted.size(), "%Y-%m-%d  %H:%M", &local) == 0U)
            return "UNKNOWN DATE";
        return formatted.data();
    }

    void refresh_world_list()
    {
        world_slots_.clear();
        std::error_code scan_error;
        std::filesystem::create_directories(worlds_directory(), scan_error);
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
        for (const auto &entry :
             std::filesystem::directory_iterator(worlds_directory(), scan_error))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".omw")
                found.emplace_back(entry.last_write_time(), entry.path());
        }
        std::sort(found.begin(), found.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[written, path] : found)
        {
            WorldSlot slot;
            slot.name = path.stem().string();
            for (char &character : slot.name)
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            slot.path = path;
            const std::optional<WorldSaveData> save = read_world_save(path);
            if (save.has_value())
            {
                slot.owner = save->owner.empty() ? "LEGACY SAVE" : save->owner;
                slot.saved_at = world_saved_at(save->saved_unix_seconds, written);
                slot.mode = game_mode_label(save->mode);
                slot.difficulty = game_difficulty_label(save->difficulty);
                slot.seed = save->seed;
                slot.day = save->day;
            }
            else
            {
                slot.owner = "UNREADABLE SAVE";
                slot.saved_at = world_saved_at(0, written);
                slot.mode = "UNKNOWN MODE";
                slot.difficulty = "UNKNOWN";
            }
            slot.description =
                std::format("{}  /  {}  /  {}", slot.owner, slot.saved_at, slot.mode);
            if (SDL_Texture *texture = load_bmp_texture(renderer_, world_preview_path(path)))
                slot.preview = std::shared_ptr<SDL_Texture>{texture, SDL_DestroyTexture};
            world_slots_.push_back(std::move(slot));
        }
        std::vector<ui::MenuEntry> entries;
        entries.push_back({"NEW WORLD",
                           "Generate a fresh frontier with a new seed.",
                           ui::MenuCommand::create_new_world,
                           true,
                           {}});
        for (const WorldSlot &slot : world_slots_)
            entries.push_back(
                {slot.name, slot.description, ui::MenuCommand::select_world, true, "SAVED"});
        entries.push_back(
            {"BACK", "Back to game mode selection.", ui::MenuCommand::back, true, {}});
        menu_.set_world_entries(std::move(entries));
    }

    // Clears everything a previous session left behind before a world starts.
    void reset_world_session_state()
    {
        sound_bank_->clear_enemy_voices();
        sound_bank_->clear_fireball_cues();
        enemy_horde_.enemies().clear();
        arrows_.clear();
        tracers_.clear();
        next_projectile_id_ = 1;
        grenades_.clear();
        armed_explosives_.clear();
        fireballs_.clear();
        applied_fireball_detonations_.clear();
        queued_dragon_fireball_ids_.clear();
        damage_indicators_.clear();
        dragon_ = world::DragonFlight{};
        dragon_owner_gid_.reset();
        dragon_for_biome_ = false;
        dragon_pending_ = false;
        dragon_network_time_ = 0.0F;
        dragon_update_timer_ = 0.0F;
        dragon_last_host_time_ = -1.0F;
        dragon_updates_sent_ = 0;
        next_dragon_fireball_index_ = 0U;
        dragon_network_sound_ = 0U;
        dragon_migration_scan_timer_ = 0.0F;
        dragon_kill_announced_ = false;
        dragon_loot_pending_ = false;
        dragon_box_valid_ = false;
        zombie_fest_ = false;
        fest_countdown_ = -1.0F;
        dragon_distance_index_ = 0;
        distance_origin_set_ = false;
        current_distance_ = 0;
        max_distance_ = 0;
        next_song_index_ = 0;
        last_song_distance_ = -1.0F;
        last_dragon_distance_ = -1.0F;
        crater_music_triggered_ = false;
        crater_music_reset_timer_ = 0.0F;
        last_day_number_ = 0;
        cleared_distance_ = 50.0F;
        time_to_first_contact_ = 20.0F;
        distance_enemies_left_ = 0;
        endurance_dragon_health_ = 25.0F;
        endurance_dragon_type_ = 0;
        endurance_dragon_delay_ = 0.0F;
        felguard_timer_ = 0.0F;
        felguard_spawned_ = false;
        surface_spawn_timer_ = 0.0F;
        cave_spawn_timer_ = 0.0F;
        player_life_ = PlayerLife::alive;
        death_timer_ = 0.0F;
        player_oxygen_ = 1.0F;
        lava_damage_time_ = world::lava_damage_period_seconds;
        lava_sound_played_ = false;
        lightning_cycle_.reset(static_cast<std::uint64_t>(static_cast<std::uint32_t>(world_seed_)) ^
                               0x5A17C0DEULL);
        lightning_visible_ = false;
        previous_biome_band_ = -1;
        if (locator_name_editing_)
            SDL_StopTextInput(window_);
        if (chat_editing_)
            SDL_StopTextInput(window_);
        locator_name_editing_ = false;
        locator_name_draft_.clear();
        locator_key_fallback_.clear();
        chat_editing_ = false;
        player_roster_visible_ = false;
        chat_draft_.clear();
        chat_key_fallback_.clear();
        online_chat_lines_.clear();
        grenade_cooking_ = false;
        grenade_cooking_type_ = 0U;
        grenade_cook_time_ = 0.0F;
        grenade_cooldown_ = 0.0F;
        grenade_animation_time_ = 0.0F;
        diagnostic_enemy_spawned_ = false;
        diagnostic_dynamic_radiance_spawned_ = false;
        gun_recoil_time_ = 0.0F;
        gun_recoil_duration_ = 0.15F;
        fired_launcher_time_ = 0.0F;
        fired_launcher_slot_.reset();
        fired_launcher_tray_.reset();
        fired_launcher_visual_.reset();
        crate_inventories_.clear();
        custom_doors_.clear();
        world_spawners_.clear();
        active_world_spawners_.clear();
        local_spawners_.clear();
        local_spawner_enemy_sources_.clear();
        pending_remote_spawner_triggers_.clear();
        remote_spawner_owners_.clear();
        remote_spawner_enemy_owners_.clear();
        defeated_spawner_count_ = 0;
        active_crate_.reset();
    }

    void recreate_world(std::int32_t seed, world::TerrainProfile terrain_profile)
    {
        world_seed_ = seed;
        lightning_cycle_.reset(static_cast<std::uint64_t>(static_cast<std::uint32_t>(world_seed_)) ^
                               0x5A17C0DEULL);
        spawner_world_random_.reset(world_seed_ ^ 0x53A9D17B);
        spawner_enemy_random_.reset(
            static_cast<std::int32_t>(static_cast<std::uint32_t>(world_seed_) ^ 0xA17C39E5U));
        loot_random_.reset(
            static_cast<std::int32_t>(static_cast<std::uint32_t>(world_seed_) ^ 0x4C4F4F54U));
        lightning_visible_ = false;
        world_preview_ = std::make_unique<WorldPreviewRenderer>(
            renderer_, world_seed_, settings_.view_distance, 0, terrain_profile, game_mode_);
        world_preview_->set_doors(&custom_doors_);
        world_preview_->set_particle_density(particle_density(settings_.particle_quality));
    }

    void start_new_world()
    {
        std::random_device device;
        const auto seed = static_cast<std::int32_t>(device() & 0x7FFFFFFFU);
        reset_world_session_state();
        const world::TerrainProfile terrain_profile =
            hosting_flow_ && host_settings_.protocol == net::ProtocolVersion::modern_v4
                ? world::TerrainProfile::modern
                : inventory_.profile() == InventoryProfile::modern
                      ? world::TerrainProfile::modern
                      : world::TerrainProfile::classic;
        recreate_world(seed, terrain_profile);
        // Endurance worlds are throwaway in the original; everything else saves.
        if (game_mode_ == GameMode::endurance)
        {
            active_world_path_.clear();
        }
        else
        {
            std::error_code make_error;
            std::filesystem::create_directories(worlds_directory(), make_error);
            active_world_path_ = worlds_directory() / std::format("frontier-{}.omw", seed);
        }
        day_night_ = world::DayNightCycle{};
        if (options_.world_time.has_value())
            day_night_.set_time(*options_.world_time);
        begin_offline_world();
        begin_online_host(true);
    }

    void save_world()
    {
        if (active_world_path_.empty() || !world_ready())
            return;
        const world::PlayerState &player = player_.state();
        WorldSaveData save;
        save.seed = world_seed_;
        save.owner = steam_identity_verified_ && !online_persona_name_.empty()
                         ? online_persona_name_
                         : profile_name_;
        save.saved_unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
        save.mode = game_mode_;
        save.difficulty = difficulty_;
        save.infinite_resources = infinite_resources_;
        save.day = day_night_.day();
        save.player_position = player.position;
        save.player_yaw = player.yaw;
        save.player_pitch = player.pitch;
        save.player_health = player.health;
        save.has_player = true;
        save.distance_origin_x = distance_origin_x_;
        save.distance_origin_z = distance_origin_z_;
        save.has_distance_origin = distance_origin_set_;
        save.dragon_distance_index = dragon_distance_index_;
        save.max_distance = max_distance_;
        save.next_song_index = next_song_index_;
        save.inventory = inventory_;
        save.crates = crate_inventories_;
        save.doors = custom_doors_;
        save.spawners = world_spawners_;
        save.edits = world_preview_->exported_edits();
        (void)write_world_save(active_world_path_, save);
    }

    void save_online_position()
    {
        if (online_world_key_.empty() || world_preview_ == nullptr || waiting_for_world_ ||
            menu_.screen() != ui::MenuScreen::world_preview ||
            !(online_client_ != nullptr && online_client_joined_))
            return;
        const world::PlayerState &player = player_.state();
        write_online_position(online_world_key_,
                              {player.position, player.yaw, player.pitch});
    }

    void load_world(const std::filesystem::path &path)
    {
        std::optional<WorldSaveData> loaded = read_world_save(path);
        if (!loaded.has_value())
        {
            toast("LOAD FAILED", "The world file could not be opened.");
            return;
        }
        WorldSaveData &save = *loaded;
        reset_world_session_state();
        game_mode_ = save.mode;
        difficulty_ = save.difficulty;
        infinite_resources_ = save.infinite_resources;
        const world::TerrainProfile terrain_profile =
            hosting_flow_ && host_settings_.protocol == net::ProtocolVersion::modern_v4
                ? world::TerrainProfile::modern
                : save.inventory.profile() == InventoryProfile::modern
                      ? world::TerrainProfile::modern
                      : world::TerrainProfile::classic;
        recreate_world(save.seed, terrain_profile);
        world_preview_->import_edits(save.edits);
        day_night_ = world::DayNightCycle{save.day};
        inventory_ = std::move(save.inventory);
        crate_inventories_ = std::move(save.crates);
        custom_doors_ = std::move(save.doors);
        world_spawners_ = std::move(save.spawners);
        active_world_spawners_.clear();
        sync_selected_item();
        dragon_distance_index_ = save.dragon_distance_index;
        max_distance_ = save.max_distance;
        next_song_index_ = save.next_song_index;
        last_day_number_ = static_cast<int>(std::floor(save.day));
        active_world_path_ = path;
        pending_load_is_online_ = false;
        pending_load_position_ =
            save.has_player ? std::optional{save.player_position} : std::nullopt;
        pending_load_yaw_ = save.player_yaw;
        pending_load_pitch_ = save.player_pitch;
        pending_load_health_ = save.player_health;
        if (pending_load_position_.has_value())
        {
            // Center the configured active window before the loading gate. The
            // old path preloaded origin, applied a far-away player coordinate,
            // and simulated at least a frame outside the loaded terrain.
            player_.state().position = *pending_load_position_;
            world_preview_->recenter_on(player_.state());
        }
        if (save.has_distance_origin)
        {
            distance_origin_x_ = save.distance_origin_x;
            distance_origin_z_ = save.distance_origin_z;
            distance_origin_set_ = true;
        }
        sound_bank_->stop_music();
        waiting_for_world_ = !world_preview_->preload_complete();
        cancel_world_action();
        SDL_SetWindowRelativeMouseMode(window_, false);
        toast("LOADING WORLD",
              std::format("SEED {}  /  {}", save.seed, game_mode_label(game_mode_)));
        begin_online_host(false);
    }

    [[nodiscard]] bool world_ready() const noexcept
    {
        return !waiting_for_world_ && world_ready_frames_ > 0U;
    }

    void handle(const ui::MenuEvent &event)
    {
        switch (event.command)
        {
        case ui::MenuCommand::play_offline:
            hosting_flow_ = false;
            std::cout << "menu.action=play_offline\n";
            break;
        // Original FrontEndScreen semantics: picking a mode resets creative
        // and difficulty first; Endurance and Dragon Endurance start
        // immediately, Survival/Creative go through the difficulty screen.
        case ui::MenuCommand::select_mode_endurance:
            // Endurance: throwaway world, straight in — no difficulty or
            // world list, exactly like the original.
            game_mode_ = GameMode::endurance;
            infinite_resources_ = false;
            difficulty_ = GameDifficulty::easy;
            start_new_world();
            break;
        case ui::MenuCommand::select_mode_dragon_endurance:
            game_mode_ = GameMode::dragon_endurance;
            infinite_resources_ = false;
            difficulty_ = GameDifficulty::easy;
            refresh_world_list();
            break;
        case ui::MenuCommand::select_mode_survival:
            game_mode_ = GameMode::survival;
            infinite_resources_ = false;
            break;
        case ui::MenuCommand::select_mode_creative:
            // Creative is Survival + infinite resources in the original.
            game_mode_ = GameMode::survival;
            infinite_resources_ = true;
            break;
        case ui::MenuCommand::select_difficulty_no_enemies:
            difficulty_ = GameDifficulty::no_enemies;
            refresh_world_list();
            break;
        case ui::MenuCommand::select_difficulty_easy:
            difficulty_ = GameDifficulty::easy;
            refresh_world_list();
            break;
        case ui::MenuCommand::select_difficulty_normal:
            difficulty_ = GameDifficulty::hard;
            refresh_world_list();
            break;
        case ui::MenuCommand::select_difficulty_hardcore:
            difficulty_ = GameDifficulty::hardcore;
            refresh_world_list();
            break;
        case ui::MenuCommand::create_new_world:
            menu_.open(ui::MenuScreen::world_preview);
            start_new_world();
            break;
        case ui::MenuCommand::select_world: {
            // Row 0 is NEW WORLD; saved worlds follow in list order.
            const std::size_t row = menu_.selection();
            if (row >= 1U && row - 1U < world_slots_.size())
            {
                const std::filesystem::path chosen = world_slots_[row - 1U].path;
                menu_.open(ui::MenuScreen::world_preview);
                load_world(chosen);
            }
            break;
        }
        case ui::MenuCommand::open_inventory:
            cancel_world_action();
            inventory_open_ = true;
            inventory_focus_ = PlayerInventory::storage_size + inventory_.selected_index();
            SDL_SetWindowRelativeMouseMode(window_, false);
            break;
        case ui::MenuCommand::teleport_surface:
        case ui::MenuCommand::teleport_start:
            if (game_mode_ == GameMode::endurance || difficulty_ == GameDifficulty::hardcore)
            {
                toast("TELEPORT UNAVAILABLE", "Endurance and Hardcore refuse the easy way out.");
                SDL_SetWindowRelativeMouseMode(window_, true);
                break;
            }
            teleport_player(event.command == ui::MenuCommand::teleport_start);
            SDL_SetWindowRelativeMouseMode(window_, true);
            break;
        case ui::MenuCommand::host_online:
            hosting_flow_ = false;
            toast("HOST SESSION SETUP",
                  "CONFIGURE COMPATIBILITY, ADMISSION, SAFETY, AND MOD PLAYLIST");
            std::cout << "menu.action=host_online\n";
            break;
        case ui::MenuCommand::edit_host_name:
            begin_host_text_edit(HostTextField::server_name);
            break;
        case ui::MenuCommand::edit_host_password:
            begin_host_text_edit(HostTextField::password);
            break;
        case ui::MenuCommand::select_host_protocol:
        case ui::MenuCommand::select_host_max_players:
        case ui::MenuCommand::select_host_visibility:
        case ui::MenuCommand::select_host_join_policy:
        case ui::MenuCommand::select_host_pvp:
        case ui::MenuCommand::toggle_host_anticheat:
        case ui::MenuCommand::toggle_host_join_safety:
        case ui::MenuCommand::select_host_spawn_protection:
            adjust_host_setting(1);
            break;
        case ui::MenuCommand::review_host_mods: {
            const std::size_t synchronized = static_cast<std::size_t>(std::count_if(
                host_settings_.mod_manifest.mods.begin(), host_settings_.mod_manifest.mods.end(),
                [](const net::ModDescriptor &mod) { return mod.scope != net::ModScope::client; }));
            toast(synchronized == 0U ? "VANILLA PLAYLIST" : "SYNCHRONIZED PLAYLIST",
                  synchronized == 0U
                      ? "STOCK CLIENTS MAY JOIN; CLIENT-ONLY PACKS ARE IGNORED"
                      : std::format("{} SERVER/CORE MODS MUST MATCH IN ORDER", synchronized));
            break;
        }
        case ui::MenuCommand::confirm_host_settings: {
            if (!mod_catalog_valid_)
            {
                hosting_flow_ = false;
                toast("MOD CATALOG BLOCKED", uppercase(mod_catalog_error_));
                break;
            }
            std::string error;
            if (!host_settings_.validate(error))
            {
                hosting_flow_ = false;
                menu_.open(ui::MenuScreen::host_setup);
                toast("HOST SETTINGS INVALID", uppercase(std::move(error)));
                break;
            }
            hosting_flow_ = true;
            toast("HOST PROFILE ARMED",
                  std::format("{}  /  {} PLAYERS  /  {}",
                              net::protocol_label(host_settings_.protocol),
                              host_settings_.maximum_players,
                              net::visibility_label(host_settings_.visibility)));
            break;
        }
        case ui::MenuCommand::join_online:
            shutdown_online_host();
            if (mod_catalog_valid_)
            {
                std::string ignored;
                if (!net::make_mod_manifest(mod_catalog_, net::ProtocolVersion::classic_v3,
                                            join_client_manifest_, ignored))
                {
                    mod_catalog_valid_ = false;
                    mod_catalog_error_ = std::move(ignored);
                }
            }
            else
                join_client_manifest_.protocol = net::ProtocolVersion::classic_v3;
            join_browser_.set_client(join_client_manifest_, true);
            refresh_join_sessions(false);
            std::cout << "menu.action=join_online\n";
            break;
        case ui::MenuCommand::refresh_join_sessions:
            refresh_join_sessions(false);
            break;
        case ui::MenuCommand::cycle_join_filter:
            cycle_join_filter(1);
            break;
        case ui::MenuCommand::direct_connect:
            begin_host_text_edit(HostTextField::direct_address);
            break;
        case ui::MenuCommand::select_join_session:
            if (menu_.selection() < join_row_sessions_.size())
            {
                selected_join_session_ = join_row_sessions_[menu_.selection()];
                join_password_.clear();
                const net::DiscoveredSession &chosen =
                    join_browser_.sessions()[*selected_join_session_];
                if (chosen.properties[0].has_value() &&
                    (*chosen.properties[0] ==
                         static_cast<std::int32_t>(net::ProtocolVersion::classic_v3) ||
                     *chosen.properties[0] ==
                         static_cast<std::int32_t>(net::ProtocolVersion::modern_v4)))
                {
                    const auto selected_protocol =
                        static_cast<net::ProtocolVersion>(*chosen.properties[0]);
                    if (mod_catalog_valid_)
                    {
                        std::string manifest_error;
                        if (!net::make_mod_manifest(mod_catalog_, selected_protocol,
                                                    join_client_manifest_, manifest_error))
                        {
                            mod_catalog_valid_ = false;
                            mod_catalog_error_ = std::move(manifest_error);
                        }
                    }
                    else
                        join_client_manifest_.protocol = selected_protocol;
                    join_browser_.set_client(join_client_manifest_, true);
                }
                menu_.open(ui::MenuScreen::join_details);
                toast("SESSION SELECTED",
                      std::format("{}  /  {} CORE PROFILE", chosen.server_name,
                                  net::protocol_label(join_client_manifest_.protocol)));
            }
            break;
        case ui::MenuCommand::edit_join_password:
            if (selected_join_session() != nullptr)
                begin_host_text_edit(HostTextField::join_password);
            break;
        case ui::MenuCommand::connect_join_session: {
            if (!mod_catalog_valid_)
            {
                toast("MOD CATALOG BLOCKED", uppercase(mod_catalog_error_));
                break;
            }
            const net::DiscoveredSession *session = selected_join_session();
            if (session == nullptr)
            {
                menu_.open(ui::MenuScreen::join_browser);
                toast("SESSION EXPIRED", "REFRESH DISCOVERY AND SELECT THE HOST AGAIN");
                break;
            }
            const net::JoinAssessment assessment =
                join_browser_.assessment(*selected_join_session_);
            if (!assessment.joinable())
            {
                toast(net::join_compatibility_label(assessment.status),
                      uppercase(assessment.detail));
                break;
            }
            if (assessment.password_required && join_password_.empty())
            {
                begin_host_text_edit(HostTextField::join_password);
                break;
            }
            begin_online_join(session->session_id, assessment.protocol, join_password_);
            std::cout << "menu.action=join_preflight protocol="
                      << net::protocol_label(assessment.protocol)
                      << " source=" << net::discovery_source_label(session->source) << '\n';
            break;
        }
        case ui::MenuCommand::confirm_exit:
            save_world();
            shutdown_online_client();
            shutdown_online_host();
            running_ = false;
            break;
        case ui::MenuCommand::resume_game:
            cancel_world_action();
            SDL_SetWindowRelativeMouseMode(window_, true);
            break;
        case ui::MenuCommand::return_to_main:
            save_world();
            shutdown_online_client();
            shutdown_online_host();
            cancel_world_action();
            close_inventory();
            SDL_SetWindowRelativeMouseMode(window_, false);
            sound_bank_->play_menu_music();
            sound_bank_->set_ambience(1.0F, 0.0F, 0.0F, 0.0F, 1.0F);
            begin_online_discovery(true);
            break;
        case ui::MenuCommand::back:
            if (dirty_)
            {
                save_settings();
            }
            break;
        default:
            break;
        }
    }

    void hover_at(float x, float y)
    {
        for (auto iterator = hits_.rbegin(); iterator != hits_.rend(); ++iterator)
        {
            if (!iterator->rect.contains(x, y))
            {
                continue;
            }
            if (iterator->kind == HitKind::menu_item || iterator->kind == HitKind::setting_row)
            {
                menu_.set_selection(iterator->index, settings_);
            }
            return;
        }
    }

    void click_at(float x, float y)
    {
        for (auto iterator = hits_.rbegin(); iterator != hits_.rend(); ++iterator)
        {
            if (!iterator->rect.contains(x, y))
            {
                continue;
            }
            switch (iterator->kind)
            {
            case HitKind::menu_item:
                menu_.set_selection(iterator->index, settings_);
                handle(menu_.activate(settings_));
                break;
            case HitKind::setting_row:
                menu_.set_selection(iterator->index, settings_);
                if (x > iterator->rect.x + iterator->rect.width * 0.58F)
                {
                    adjust(x < iterator->rect.x + iterator->rect.width * 0.78F ? -1 : 1);
                }
                break;
            case HitKind::tab: {
                const int target = static_cast<int>(iterator->index);
                while (static_cast<int>(menu_.settings_tab()) != target)
                {
                    menu_.change_tab(1, settings_);
                }
                break;
            }
            case HitKind::back:
                handle(menu_.back());
                break;
            }
            return;
        }
    }

    void update(float delta)
    {
        elapsed_ += delta;
        if (online_join_stage_ != OnlineJoinStage::idle)
            online_join_elapsed_ += delta;
        pump_online_host();
        pump_online_client();
        pump_authoritative_chunk_checks();
        if (menu_.screen() == ui::MenuScreen::join_browser &&
            join_browser_state_ == JoinBrowserState::ready && online_client_ != nullptr &&
            !online_client_joined_ && !online_client_operation_.valid())
        {
            join_refresh_timer_ -= delta;
            if (join_refresh_timer_ <= 0.0F)
            {
                join_refresh_timer_ = 30.0F;
                refresh_join_sessions(false, true);
            }
        }
        sync_network_pickup_mode();
        std::erase_if(recent_network_projectiles_, [&](const RecentNetworkProjectile &projectile) {
            return projectile.expires < elapsed_;
        });
        if (online_host_ != nullptr && online_host_->running())
        {
            online_world_update_timer_ -= delta;
            if (online_world_update_timer_ <= 0.0F)
            {
                online_world_update_timer_ = 5.0F;
                publish_host_time_of_day();
            }
        }
        if (online_client_ != nullptr && online_client_joined_ &&
            online_bootstrap_client_ == nullptr)
        {
            online_inventory_save_timer_ -= delta;
            if (online_inventory_save_timer_ <= 0.0F)
            {
                online_inventory_save_timer_ = 10.0F;
                if (!publish_local_inventory_store(false))
                    std::cerr << "network.local.periodic-inventory-store.failure\n";
            }
        }
        refresh_online_remote_visuals();
        if (ui_join_probe_started_)
        {
            if (online_client_ == nullptr && !online_client_operation_.valid())
            {
                ui_join_probe_failed_ = true;
                ui_join_probe_detail_ = "The UI join path ended before installing the world";
                std::cerr << "network.ui-join-probe.failure=" << ui_join_probe_detail_ << '\n';
                running_ = false;
            }
            else if (online_client_joined_ && online_bootstrap_client_ == nullptr &&
                     menu_.screen() == ui::MenuScreen::world_preview && !waiting_for_world_)
            {
                std::cout << "network.ui-join-probe.complete=1\n";
                running_ = false;
            }
            else if (online_join_elapsed_ > 60.0F)
            {
                ui_join_probe_failed_ = true;
                ui_join_probe_detail_ = "The UI join path timed out before installing the world";
                std::cerr << "network.ui-join-probe.failure=" << ui_join_probe_detail_ << '\n';
                running_ = false;
            }
        }
        subtitle_time_ = std::max(0.0F, subtitle_time_ - delta);
        // PickInventoryItemClass owns the laser-sword hum for exactly the
        // duration that its item class remains equipped.
        update_equipped_item_sound();
        if (menu_.screen() != ui::MenuScreen::world_preview || waiting_for_world_)
        {
            (void)world_preview_->preload_step(waiting_for_world_ ? 16U : 4U);
            if (waiting_for_world_ && world_preview_->preload_complete())
            {
                waiting_for_world_ = false;
                if (online_join_stage_ == OnlineJoinStage::preparing_world)
                    online_join_stage_ = OnlineJoinStage::idle;
                spawn_player();
                if (!options_.screenshot_path.has_value())
                    SDL_SetWindowRelativeMouseMode(window_, !inventory_open_);
                toast("WORLD READY", std::format("{} CHUNKS PRELOADED  /  {:L} EXPOSED FACES",
                                                 world_preview_->stats().chunk_count,
                                                 world_preview_->stats().visible_face_count));
            }
        }
        if (menu_.screen() == ui::MenuScreen::world_preview && !waiting_for_world_)
        {
            ++world_ready_frames_;
            update_audio_environment(delta);
            if (!active_world_path_.empty())
            {
                autosave_timer_ -= delta;
                if (autosave_timer_ <= 0.0F)
                {
                    autosave_timer_ = 30.0F;
                    save_world();
                }
            }
            else if (!online_world_key_.empty() && online_client_ != nullptr &&
                     online_client_joined_)
            {
                online_position_save_timer_ -= delta;
                if (online_position_save_timer_ <= 0.0F)
                {
                    online_position_save_timer_ = 5.0F;
                    save_online_position();
                }
            }
            // Original pause semantics: the INVENTORY never pauses anything —
            // time advances and enemies keep coming while you craft.  Only the
            // pause MENU freezes a local game (menu screens skip this branch
            // entirely because menu_.screen() != world_preview there).
            {
                world_elapsed_ += delta;
                const std::string previous_period{current_sky_state().period};
                day_night_.update(delta, world_time_speed_);
                const auto updated_sky = current_sky_state();
                if (updated_sky.period != previous_period)
                {
                    toast(std::format("{} ARRIVES", updated_sky.period),
                          std::format("{}  /  DAY {}", day_night_.clock_text(),
                                      static_cast<int>(std::floor(day_night_.day())) + 1));
                }
                // The original plays the horror stinger on each day rollover.
                const int day_number = static_cast<int>(std::floor(day_night_.day()));
                if (day_number > last_day_number_)
                {
                    last_day_number_ = day_number;
                    sound_bank_->play_cue("HorrorStinger");
                }
            }
            update_weather(delta);
            if (!inspector_view_)
            {
                physics_accumulator_ = std::min(physics_accumulator_ + delta, 0.25F);
                const bool *keys = SDL_GetKeyboardState(nullptr);
                world::PlayerInput input;
                if (!inventory_open_ && !locator_name_editing_ && player_life_ == PlayerLife::alive)
                {
                    input.strafe =
                        (keys[SDL_SCANCODE_D] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_A] ? 1.0F : 0.0F);
                    input.forward =
                        (keys[SDL_SCANCODE_W] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_S] ? 1.0F : 0.0F);
                    input.jump = keys[SDL_SCANCODE_SPACE];
                    const bool left_shift = keys[SDL_SCANCODE_LSHIFT];
                    input.sprint = left_shift && inventory_.profile() == InventoryProfile::modern &&
                                   game_mode_ != GameMode::endurance &&
                                   game_mode_ != GameMode::dragon_endurance;
                    input.descend =
                        keys[SDL_SCANCODE_LCTRL] || (player_.state().fly_mode && left_shift);
                }
                // Flight is a Creative power: leaving creative (or loading a
                // survival save) drops the player back under gravity.
                if (!infinite_resources_ && player_.state().fly_mode)
                    player_.state().fly_mode = false;
                if (options_.bench_frames > 0)
                {
                    // Benchmark runs walk forward so chunk streaming and remeshing
                    // are part of the measured frames, not just the cached view.
                    input.forward = 1.0F;
                    input.jump = true;
                }
                if (!inventory_open_ && !locator_name_editing_)
                    poll_gamepad(input, delta);
                constexpr float fixed_delta = 1.0F / 60.0F;
                while (physics_accumulator_ >= fixed_delta)
                {
                    melee_cooldown_ = std::max(0.0F, melee_cooldown_ - fixed_delta);
                    gun_cooldown_ = std::max(0.0F, gun_cooldown_ - fixed_delta);
                    gun_recoil_time_ = std::max(0.0F, gun_recoil_time_ - fixed_delta);
                    if (fired_launcher_slot_.has_value())
                    {
                        fired_launcher_time_ = std::max(0.0F, fired_launcher_time_ - fixed_delta);
                        if (fired_launcher_time_ <= 0.0F)
                        {
                            const std::size_t fired_slot = *fired_launcher_slot_;
                            auto &spent =
                                inventory_.tray(fired_launcher_tray_.value_or(0U))[fired_slot];
                            if (spent.has_value() &&
                                (spent->item == InventoryItemId::rocket_launcher ||
                                 spent->item == InventoryItemId::guided_rocket_launcher))
                                spent.reset();
                            fired_launcher_slot_.reset();
                            fired_launcher_tray_.reset();
                            fired_launcher_visual_.reset();
                            sync_selected_item();
                        }
                    }
                    constexpr float gun_shoulder_seconds = 0.15F;
                    const GunStats *active_gun = selected_gun();
                    if (active_gun == nullptr)
                    {
                        gun_aim_progress_ = 0.0F;
                    }
                    else if (gun_reloading_ && gun_aim_progress_ > 0.0F)
                    {
                        // Vanilla reverses the shoulder clip before it starts channel-2 reload.
                        gun_aim_progress_ =
                            std::max(0.0F, gun_aim_progress_ - fixed_delta / gun_shoulder_seconds);
                    }
                    else
                    {
                        const float aim_target = shouldering_ && !gun_reloading_ ? 1.0F : 0.0F;
                        gun_aim_progress_ += std::clamp(aim_target - gun_aim_progress_,
                                                        -fixed_delta / gun_shoulder_seconds,
                                                        fixed_delta / gun_shoulder_seconds);
                    }
                    if (gun_reloading_ && gun_aim_progress_ <= 0.0F && active_gun != nullptr)
                    {
                        if (!gun_reload_cue_started_)
                        {
                            gun_reload_cue_started_ = true;
                            sound_bank_->play_cue(active_gun->reload_cue);
                            caption(std::format("[ {} RELOADS ]",
                                                item_name(inventory_.selected_item())),
                                    1.7F);
                        }
                        gun_reload_time_ = std::max(0.0F, gun_reload_time_ - fixed_delta);
                        if (gun_reload_time_ <= 0.0F)
                        {
                            auto &active = inventory_.tray()[inventory_.selected_index()];
                            const GunStats *slot_stats =
                                active.has_value() ? gun_stats(active->item) : nullptr;
                            bool continue_reloading{};
                            if (slot_stats != nullptr)
                            {
                                const GunReloadCycle cycle =
                                    gun_reload_cycle(*slot_stats, active->rounds_in_clip,
                                                     inventory_.count(slot_stats->ammo_item));
                                if (cycle.loaded > 0U &&
                                    (infinite_resources_ ||
                                     inventory_.consume_item(slot_stats->ammo_item, cycle.loaded)))
                                {
                                    active->rounds_in_clip = static_cast<std::uint16_t>(
                                        active->rounds_in_clip + cycle.loaded);
                                    continue_reloading = cycle.continue_reloading;
                                }
                            }
                            // Player.FinishReload plays Click after every cycle,
                            // including each individually inserted shotgun shell.
                            sound_bank_->play_cue("Click");
                            gun_reloading_ = continue_reloading;
                            if (continue_reloading && slot_stats != nullptr)
                                gun_reload_time_ = slot_stats->reload_seconds;
                            gun_reload_cue_started_ = false;
                        }
                    }
                    update_guided_lock(fixed_delta);
                    const float fall_velocity_before = player_.state().velocity.y;
                    const bool was_grounded = player_.state().grounded;
                    const bool was_in_water = player_in_water(player_.state());
                    player_.update(world_preview_->region(), input, fixed_delta,
                                   settings_.auto_climb, player_water_world(),
                                   settings_.creative_flight_speed,
                                   inventory_.profile() == InventoryProfile::modern);
                    // Player.Jump: the takeoff itself plays a FootStep; a
                    // landing plays one too so airtime never mutes the feet.
                    if (was_grounded != player_.state().grounded && !was_in_water &&
                        !player_in_water(player_.state()) && player_life_ == PlayerLife::alive)
                    {
                        const bool jumped =
                            !player_.state().grounded && player_.state().velocity.y > 1.0F;
                        const bool landed = player_.state().grounded;
                        if (jumped || landed)
                        {
                            sound_bank_->play_footstep();
                            footstep_timer_ = 0.4F;
                        }
                    }
                    // Original fall damage: when a landing absorbs more than 15
                    // units of vertical speed, damage = (delta - 15) / 15 of max
                    // health (safe to ~5.6 blocks, lethal from ~22.5).
                    const float fall_delta = player_.state().velocity.y - fall_velocity_before;
                    if (fall_delta > 15.0F && player_.state().velocity.y < 0.1F)
                    {
                        world::Vec3 below = player_.state().position;
                        below.y -= 1.0F;
                        caption("[ HARD LANDING ]", 0.9F);
                        apply_player_damage((fall_delta - 15.0F) * (100.0F / 15.0F), below);
                    }
                    update_environmental_survival(fixed_delta);
                    const float horizontal_speed =
                        std::hypot(player_.state().velocity.x, player_.state().velocity.z);
                    if (player_.state().grounded && !player_in_water(player_.state()) &&
                        horizontal_speed > 0.1F)
                    {
                        footstep_timer_ -= fixed_delta;
                        if (footstep_timer_ <= 0.0F)
                        {
                            sound_bank_->play_footstep();
                            caption("[ FOOTSTEPS ]", 0.65F);
                            // Player.UpdateAudio: 0.4 s cadence running, 0.5 s
                            // walking.
                            footstep_timer_ +=
                                horizontal_speed > world::PlayerSimulation::movement_speed * 0.8F
                                    ? 0.4F
                                    : 0.5F;
                        }
                    }
                    else if (!player_.state().grounded)
                    {
                        footstep_timer_ = std::max(footstep_timer_, 0.15F);
                    }
                    else
                    {
                        footstep_timer_ = 0.5F;
                    }
                    const bool action_held = mine_button_down_ || secondary_action_down_;
                    const bool gun_selected = active_gun != nullptr;
                    const bool grenade_selected =
                        item_grenade_type(inventory_.selected_item()).has_value();
                    // Automatic guns keep cycling while the trigger is held; semi-autos
                    // only fire on the press edge in primary_action_pressed.
                    if (gun_selected && mine_button_down_ && active_gun->automatic)
                        fire_gun();
                    const bool melee_hit =
                        action_held && !gun_selected && !grenade_selected && try_melee();
                    const bool breaking = action_held && !melee_hit && !grenade_selected &&
                                          player_.state().target.has_value() && !gun_selected &&
                                          (inventory_.selected_block() == world::BlockType::empty ||
                                           secondary_action_down_);
                    const InventoryItemId action_item = inventory_.selected_item();
                    const bool block_action =
                        inventory_.selected_block() != world::BlockType::empty;
                    const float impact_period =
                        block_action ? 0.1F : item_use_cooldown_seconds(action_item);
                    const std::uint8_t action_key =
                        action_item == InventoryItemId::bare_hands
                            ? 0U
                            : static_cast<std::uint8_t>(inventory_.selected_index() + 1U);
                    // TimeToDig for the held tool against the aimed block.
                    const float dig_seconds =
                        player_.state().target.has_value()
                            ? dig_time_seconds(action_item, player_.state().target->block)
                            : -1.0F;
                    const auto block_update =
                        player_.update_breaking(world_preview_->region(), breaking, fixed_delta,
                                                impact_period, action_key, dig_seconds);
                    if (action_held && !breaking && !gun_selected && !grenade_selected &&
                        air_punch_time_ <= 0.0F)
                    {
                        air_punch_time_ = 0.5F;
                    }
                    if (air_punch_time_ > 0.0F)
                    {
                        air_punch_time_ = std::max(0.0F, air_punch_time_ - fixed_delta);
                        player_.state().using_tool = true;
                        player_.state().tool_swing_progress = 1.0F - air_punch_time_ / 0.5F;
                    }
                    if (placement_animation_time_ > 0.0F)
                    {
                        placement_animation_time_ =
                            std::max(0.0F, placement_animation_time_ - fixed_delta);
                        player_.state().using_tool = true;
                        player_.state().tool_swing_progress =
                            1.0F - placement_animation_time_ / 0.35F;
                    }
                    if (gun_recoil_time_ > 0.0F)
                    {
                        player_.state().using_tool = true;
                        player_.state().tool_swing_progress =
                            1.0F - gun_recoil_time_ / gun_recoil_duration_;
                    }
                    if (block_update.has_value() && block_update->impact)
                    {
                        sound_bank_->play_break(block_update->block);
                        caption(
                            std::format("[ {} IMPACT ]", world::block_name(block_update->block)),
                            1.1F);
                        std::string dig_error;
                        if (!publish_local_dig_effect(false, *block_update, dig_error))
                            std::cerr << "network.local.dig-effect.failure=" << dig_error << '\n';
                        world_preview_->spawn_mining_impact(*block_update);
                    }
                    if (block_update.has_value() && block_update->completed)
                    {
                        // Mining the crash-site goo or space rock wakes the aliens.
                        if (block_update->block == world::BlockType::slime ||
                            block_update->block == world::BlockType::space_rock ||
                            block_update->block == world::BlockType::space_rock_inventory)
                        {
                            alien_aroused_time_ = 60.0F;
                            toast("THE ORB STIRS", "Something is coming out of the goo.");
                        }
                        std::vector<std::array<int, 3>> removed_cells;
                        removed_cells.reserve(block_update->removed_blocks.size());
                        std::map<CrateKey, std::pair<InventoryItemId, world::BlockType>>
                            removed_doors;
                        for (const world::BlockRemovalEffect &effect : block_update->removed_blocks)
                        {
                            if (!world::is_lower_door(effect.block) &&
                                !world::is_upper_door(effect.block))
                                continue;
                            const CrateKey key =
                                lower_door_key({effect.x, effect.y, effect.z}, effect.block);
                            InventoryItemId item = block_item(effect.block);
                            const auto identity = custom_doors_.find(key);
                            if (identity != custom_doors_.end())
                            {
                                const std::optional<InventoryItemId> modeled =
                                    door_item_from_model(identity->second);
                                if (modeled.has_value())
                                    item = *modeled;
                            }
                            removed_doors.try_emplace(key, item, effect.block);
                        }
                        for (const world::BlockRemovalEffect &effect : block_update->removed_blocks)
                        {
                            if (world::is_container(effect.block))
                                eject_crate_contents({effect.x, effect.y, effect.z});
                            const bool primary = effect.x == block_update->x &&
                                                 effect.y == block_update->y &&
                                                 effect.z == block_update->z;
                            if (primary || effect.drops_as_dependent)
                            {
                                bool processed_loot{};
                                const bool door_piece = world::is_lower_door(effect.block) ||
                                                        world::is_upper_door(effect.block);
                                if (door_piece)
                                {
                                    const CrateKey key = lower_door_key(
                                        {effect.x, effect.y, effect.z}, effect.block);
                                    const auto door = removed_doors.find(key);
                                    if (door != removed_doors.end())
                                    {
                                        world_preview_->spawn_created_stack_at(
                                            InventoryStack{door->second.first, 1U},
                                            {static_cast<float>(key[0]), static_cast<float>(key[1]),
                                             static_cast<float>(key[2])});
                                        processed_loot = true;
                                    }
                                }
                                if (!processed_loot && primary &&
                                    inventory_.profile() == InventoryProfile::modern)
                                {
                                    const auto loot = world::process_modern_loot_block(
                                        loot_random_, effect.block, effect.y);
                                    if (loot.has_value())
                                    {
                                        processed_loot = true;
                                        const world::Vec3 position{
                                            static_cast<float>(effect.x) + 0.5F,
                                            static_cast<float>(effect.y) + 0.5F,
                                            static_cast<float>(effect.z) + 0.5F};
                                        for (const world::ModernLootDrop &loot_drop : loot->drops)
                                            world_preview_->spawn_loot_stack_at(
                                                loot_drop.stack, position,
                                                {loot_drop.spawn_velocity[0],
                                                 loot_drop.spawn_velocity[1],
                                                 loot_drop.spawn_velocity[2]});
                                    }
                                }
                                if (!processed_loot)
                                {
                                    world::BlockBreakUpdate drop = *block_update;
                                    drop.x = effect.x;
                                    drop.y = effect.y;
                                    drop.z = effect.z;
                                    drop.block = effect.block;
                                    drop.removed_blocks.clear();
                                    world_preview_->spawn_block_pickup(drop);
                                }
                            }
                            world_preview_->remember_edit(effect.x, effect.y, effect.z,
                                                          world::BlockType::empty);
                            static_cast<void>(publish_local_block_edit(effect.x, effect.y, effect.z,
                                                                       world::BlockType::empty));
                            removed_cells.push_back({effect.x, effect.y, effect.z});
                        }
                        for (const auto &[key, door] : removed_doors)
                            publish_local_custom_door_destroy(key, door.second);
                        world_preview_->rebuild_block_edits(removed_cells);
                        toast("BLOCK MINED", std::string{world::block_name(block_update->block)});
                    }
                    if (block_update.has_value() && block_update->impact &&
                        action_item != InventoryItemId::gps &&
                        action_item != InventoryItemId::teleport_gps)
                    {
                        // InGameHUD.Dig calls InflictDamage after every authored
                        // item cooldown impact, whether or not this impact was
                        // the one that completed the block.
                        (void)damage_selected_item();
                    }
                    update_local_spawners(fixed_delta);
                    update_enemies(fixed_delta);
                    if (!publish_spawned_network_pickups())
                        std::cerr << "network.local.pickup-publication.failure\n";
                    for (const CollectedBlockStack &collected :
                         world_preview_->update_effects(fixed_delta, player_.state().position))
                    {
                        if (collected.pickup_id.has_value())
                            static_cast<void>(publish_local_pickup_request(collected));
                        else
                            award_local_pickup(collected.stack);
                    }
                    publish_local_player_update(input, fixed_delta);
                    physics_accumulator_ -= fixed_delta;
                }
                const std::string previous_biome{world_preview_->biome_name()};
                if (world_preview_->stream_around(player_.state()))
                {
                    player_.refresh_target(world_preview_->region());
                    if (world_preview_->biome_name() != previous_biome)
                    {
                        toast("FRONTIER DISCOVERED",
                              std::format("{} BIOME  /  WORLD STREAM {}",
                                          world_preview_->biome_name(),
                                          world_preview_->stats().stream_serial));
                    }
                }
                if (player_.state().position.y < static_cast<float>(world::world_min_y) + 1.0F)
                {
                    spawn_player();
                }
            }
        }
        if (last_transition_serial_ != menu_.transition_serial())
        {
            last_transition_serial_ = menu_.transition_serial();
            transition_ = settings_.reduced_motion ? 1.0F : 0.0F;
        }
        transition_ = approach(transition_, 1.0F, settings_.reduced_motion ? 40.0F : 8.5F, delta);
        const std::size_t selected = menu_.selection();
        for (std::size_t index = 0U; index < focus_.size(); ++index)
        {
            const float target = index == selected ? 1.0F : 0.0F;
            focus_[index] =
                approach(focus_[index], target, settings_.reduced_motion ? 40.0F : 13.0F, delta);
        }
        if (toast_time_ > 0.0F)
        {
            toast_time_ -= delta;
        }
        if (dirty_)
        {
            dirty_timer_ -= delta;
            if (dirty_timer_ <= 0.0F)
            {
                save_settings();
            }
        }
    }

    void render()
    {
        int width{};
        int height{};
        if (!SDL_GetRenderOutputSize(renderer_, &width, &height))
        {
            return;
        }
        metrics_ =
            ui::DesignCanvas::compute(width, height, settings_.ui_scale, settings_.safe_area);
        hits_.clear();
        if (online_join_stage_ != OnlineJoinStage::idle &&
            online_join_stage_ != OnlineJoinStage::preparing_world)
        {
            draw_background();
            draw_chrome();
            draw_online_join_loading();
            SDL_RenderPresent(renderer_);
            return;
        }
        if (menu_.screen() == ui::MenuScreen::world_preview ||
            menu_.screen() == ui::MenuScreen::pause)
        {
            if (waiting_for_world_)
            {
                draw_background();
                draw_chrome();
                if (online_join_stage_ == OnlineJoinStage::preparing_world)
                    draw_online_join_loading();
                else
                    draw_world_loading();
                SDL_RenderPresent(renderer_);
                return;
            }
            world::PlayerState render_player = player_.state();
            float render_aim_progress = gun_aim_progress_;
            render_player.viewmodel_cue = world::ViewmodelCue::idle;
            render_player.viewmodel_cue_progress = 0.0F;
            if (player_life_ != PlayerLife::alive)
            {
                // The body faints: the eye accelerates down like a felled body
                // (quadratic, not linear), tips forward as it goes, and lands
                // with a small settle bounce instead of stopping dead.
                constexpr float fall_duration = 0.85F;
                constexpr float fall_depth = 1.15F;
                const float t = std::min(1.0F, death_timer_ / fall_duration);
                float drop = t * t * fall_depth;
                if (death_timer_ > fall_duration)
                {
                    const float after = std::min(1.0F, (death_timer_ - fall_duration) / 0.4F);
                    drop = fall_depth - std::sin(after * 3.14159265F) * 0.07F * (1.0F - after);
                }
                render_player.position.y -= drop;
                render_player.pitch =
                    render_player.pitch + (-0.30F - render_player.pitch) * (t * t);
                render_player.yaw += 0.10F * t * t;
            }
            if (menu_.screen() == ui::MenuScreen::world_preview && !inventory_open_ &&
                !inspector_view_)
            {
                // Simulation is intentionally fixed at 60 Hz, but the first-person rig should
                // advance on every presented frame. Extrapolate only the visual swing through
                // the unconsumed fixed-step remainder; authoritative mining and firing stay on
                // their deterministic ticks.
                const float subframe = std::clamp(physics_accumulator_, 0.0F, 1.0F / 60.0F);
                const bool gun_selected = selected_gun() != nullptr;
                const bool grenade_selected =
                    item_grenade_type(inventory_.selected_item()).has_value();
                if (gun_selected)
                {
                    constexpr float gun_shoulder_seconds = 0.15F;
                    const float aim_target = shouldering_ && !gun_reloading_ ? 1.0F : 0.0F;
                    render_aim_progress += std::clamp(aim_target - render_aim_progress,
                                                      -subframe / gun_shoulder_seconds,
                                                      subframe / gun_shoulder_seconds);
                }
                const bool action_held = mine_button_down_ || secondary_action_down_;
                const bool breaking = action_held && render_player.target.has_value() &&
                                      !gun_selected && !grenade_selected &&
                                      (inventory_.selected_block() == world::BlockType::empty ||
                                       secondary_action_down_);
                if (breaking)
                {
                    const bool block_action =
                        inventory_.selected_block() != world::BlockType::empty;
                    const float period =
                        block_action ? 0.1F : item_use_cooldown_seconds(inventory_.selected_item());
                    render_player.using_tool = true;
                    render_player.tool_swing_progress =
                        std::fmod(render_player.tool_swing_progress + subframe / period, 1.0F);
                    render_player.viewmodel_cue = world::ViewmodelCue::primary_use;
                    render_player.viewmodel_cue_progress = render_player.tool_swing_progress;
                }
                if (air_punch_time_ > 0.0F)
                {
                    render_player.using_tool = true;
                    render_player.tool_swing_progress =
                        1.0F - std::max(0.0F, air_punch_time_ - subframe) / 0.5F;
                    render_player.viewmodel_cue = world::ViewmodelCue::primary_use;
                    render_player.viewmodel_cue_progress = render_player.tool_swing_progress;
                }
                if (placement_animation_time_ > 0.0F)
                {
                    render_player.using_tool = true;
                    render_player.tool_swing_progress =
                        1.0F - std::max(0.0F, placement_animation_time_ - subframe) / 0.35F;
                    render_player.viewmodel_cue = world::ViewmodelCue::primary_use;
                    render_player.viewmodel_cue_progress = render_player.tool_swing_progress;
                }
                if (grenade_cooking_)
                {
                    render_player.using_tool = true;
                    render_player.viewmodel_cue = world::ViewmodelCue::grenade_cook;
                    // GrenadeCook is authored at 0.983333 s; after it finishes,
                    // the final pose remains held until release or auto-throw.
                    render_player.viewmodel_cue_progress =
                        std::clamp((grenade_cook_time_ + subframe) / 0.9833333F, 0.0F, 1.0F);
                }
                else if (grenade_animation_time_ > 0.0F)
                {
                    render_player.using_tool = true;
                    render_player.viewmodel_cue = world::ViewmodelCue::grenade_throw;
                    render_player.viewmodel_cue_progress = std::clamp(
                        1.0F - std::max(0.0F, grenade_animation_time_ - subframe) / 0.3166666F,
                        0.0F, 1.0F);
                }
                if (gun_selected)
                {
                    const float reload_seconds = std::max(0.1F, selected_gun()->reload_seconds);
                    if (gun_reloading_ && render_aim_progress > 0.0F)
                    {
                        render_player.viewmodel_cue = world::ViewmodelCue::unshoulder;
                        render_player.viewmodel_cue_progress = 1.0F - render_aim_progress;
                    }
                    else if (gun_reloading_)
                    {
                        render_player.viewmodel_cue = world::ViewmodelCue::reload;
                        render_player.viewmodel_cue_progress = std::clamp(
                            1.0F - std::max(0.0F, gun_reload_time_ - subframe) / reload_seconds,
                            0.0F, 1.0F);
                    }
                    else if (gun_recoil_time_ > 0.0F)
                    {
                        render_player.using_tool = true;
                        render_player.tool_swing_progress =
                            1.0F -
                            std::max(0.0F, gun_recoil_time_ - subframe) / gun_recoil_duration_;
                        render_player.viewmodel_cue = render_aim_progress >= 0.5F
                                                          ? world::ViewmodelCue::shoulder_use
                                                          : world::ViewmodelCue::primary_use;
                        render_player.viewmodel_cue_progress = render_player.tool_swing_progress;
                    }
                    else if (render_aim_progress >= 0.999F)
                    {
                        render_player.viewmodel_cue = world::ViewmodelCue::shoulder_idle;
                    }
                    else if (render_aim_progress > 0.001F)
                    {
                        const bool raising = shouldering_;
                        render_player.viewmodel_cue = raising ? world::ViewmodelCue::shoulder
                                                              : world::ViewmodelCue::unshoulder;
                        render_player.viewmodel_cue_progress =
                            raising ? render_aim_progress : 1.0F - render_aim_progress;
                    }
                }
                render_player.tool_swing_progress =
                    std::clamp(render_player.tool_swing_progress, 0.0F, 1.0F);
            }
            if (options_.preview_viewmodel.has_value())
            {
                const std::string &state = *options_.preview_viewmodel;
                const float progress = std::clamp(options_.preview_viewmodel_progress, 0.0F, 1.0F);
                render_player.viewmodel_cue_progress = progress;
                render_player.tool_swing_progress = progress;
                if (state == "use" || state == "fire")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::primary_use;
                    render_player.using_tool = true;
                }
                else if (state == "aim")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::shoulder;
                    render_aim_progress = progress;
                }
                else if (state == "unshoulder")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::unshoulder;
                    render_aim_progress = 1.0F - progress;
                }
                else if (state == "aim-idle")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::shoulder_idle;
                    render_aim_progress = 1.0F;
                }
                else if (state == "aim-fire")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::shoulder_use;
                    render_player.using_tool = true;
                    render_aim_progress = 1.0F;
                }
                else if (state == "reload")
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::reload;
                    render_player.using_tool = true;
                    render_aim_progress = 0.0F;
                }
                else
                {
                    render_player.viewmodel_cue = world::ViewmodelCue::idle;
                }
            }
            const GunStats *drawn_gun = selected_gun();
            const float draw_aim = drawn_gun != nullptr ? render_aim_progress : 0.0F;
            world_preview_->set_enemies(&enemy_horde_.enemies());
            world_preview_->set_remote_players(&online_remote_visuals_);
            world_preview_->set_dragon(dragon_.visible() ? &dragon_.dragon() : nullptr);
            world_preview_->set_projectiles(build_projectile_visuals());
            world_preview_->set_rockets(build_rocket_visuals());
            world_preview_->set_explosive_fuses(build_fuse_visuals());
            {
                std::vector<FireballVisual> fireball_visuals;
                fireball_visuals.reserve(fireballs_.size());
                for (const Fireball &fireball : fireballs_)
                    fireball_visuals.push_back({fireball.position, fireball.velocity,
                                                world_elapsed_ * 3.0F, fireball.id,
                                                fireball.kind == world::DragonKind::ice});
                world_preview_->set_fireball_visuals(std::move(fireball_visuals));
            }
            world::SkyState biome_sky = current_sky_state();
            biome_sky.lightning = options_.force_lightning || lightning_visible_;
            apply_biome_atmosphere(biome_sky);
            apply_depth_fog(biome_sky);
            world_preview_->draw(
                metrics_, biome_sky, render_player,
                // FPSCamera.FieldOfView lerps to DefaultFOV / ShoulderMagnification
                // with the gun's own zoom factor.
                std::lerp(settings_.vertical_fov,
                          settings_.vertical_fov /
                              (drawn_gun != nullptr ? drawn_gun->shoulder_magnification : 1.0F),
                          draw_aim),
                world_elapsed_, settings_.render_scale, settings_.camera_bob,
                settings_.screen_shake,
                // The original halves the avatar-eye FOV when fully shouldered
                // (ShoulderedAvatarFOV = viewFOV * 0.5), magnifying the gun while the
                // arms leave the frame edges.
                std::lerp(settings_.viewmodel_fov, settings_.viewmodel_fov * 0.5F, draw_aim),
                settings_.presentation == PresentationProfile::rebirth, settings_.reduced_motion,
                inspector_view_, settings_.ambient_occlusion,
                terrain_shadow_strength(settings_.shadow_quality));
            if (menu_.screen() == ui::MenuScreen::world_preview)
                world_thumbnail_captured_for_pause_ = false;
            else if (!world_thumbnail_captured_for_pause_)
            {
                capture_world_thumbnail();
                world_thumbnail_captured_for_pause_ = true;
            }
            draw_world_hud();
            draw_online_chat_feed();
            if (player_roster_visible_)
                draw_player_roster();
            if (player_life_ != PlayerLife::alive)
                draw_death_overlay();
            if (inventory_open_)
                draw_inventory_overlay();
            if (locator_name_editing_)
                draw_locator_name_dialog();
            if (chat_editing_)
                draw_chat_dialog();
            if (menu_.screen() == ui::MenuScreen::pause)
                draw_pause_menu();
            draw_subtitle();
            draw_toast();
            SDL_RenderPresent(renderer_);
            return;
        }
        draw_background();
        draw_chrome();

        switch (menu_.screen())
        {
        case ui::MenuScreen::main:
        case ui::MenuScreen::multiplayer:
        case ui::MenuScreen::host_setup:
        case ui::MenuScreen::host_details:
        case ui::MenuScreen::host_access:
        case ui::MenuScreen::host_gameplay:
        case ui::MenuScreen::join_browser:
        case ui::MenuScreen::join_details:
        case ui::MenuScreen::game_mode:
        case ui::MenuScreen::difficulty:
        case ui::MenuScreen::world_select:
            draw_main_menu();
            break;
        case ui::MenuScreen::addons:
            draw_addons();
            break;
        case ui::MenuScreen::settings:
            draw_settings();
            break;
        case ui::MenuScreen::credits:
            draw_credits();
            break;
        case ui::MenuScreen::world_preview:
        case ui::MenuScreen::pause:
            break;
        case ui::MenuScreen::exit_confirmation:
            draw_exit_confirmation();
            break;
        }
        if (host_text_field_ != HostTextField::none)
            draw_host_text_dialog();
        draw_toast();
        SDL_RenderPresent(renderer_);
    }

    void fill(Rect rect, SDL_Color color)
    {
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        const SDL_FRect converted{rect.x, rect.y, rect.width, rect.height};
        SDL_RenderFillRect(renderer_, &converted);
    }

    void stroke(Rect rect, SDL_Color color, float thickness = 1.0F)
    {
        fill({rect.x, rect.y, rect.width, thickness}, color);
        fill({rect.x, rect.y + rect.height - thickness, rect.width, thickness}, color);
        fill({rect.x, rect.y, thickness, rect.height}, color);
        fill({rect.x + rect.width - thickness, rect.y, thickness, rect.height}, color);
    }

    void text(std::string_view value, float x, float y, float design_size, SDL_Color color,
              TextAlign align = TextAlign::left, bool outline = false)
    {
        const float large_factor = settings_.larger_text ? 1.12F : 1.0F;
        font_->draw(value, x, y, metrics_.font_pixels(design_size * large_factor), color, align,
                    outline);
    }

    void draw_background()
    {
        const int bands = 28;
        for (int band = 0; band < bands; ++band)
        {
            const float ratio = static_cast<float>(band) / static_cast<float>(bands - 1);
            const Uint8 r = static_cast<Uint8>(7.0F + ratio * 10.0F);
            const Uint8 g = static_cast<Uint8>(11.0F + ratio * 12.0F);
            const Uint8 b = static_cast<Uint8>(14.0F + ratio * 11.0F);
            fill({0.0F, static_cast<float>(metrics_.output_height) * ratio,
                  static_cast<float>(metrics_.output_width),
                  static_cast<float>(metrics_.output_height) / static_cast<float>(bands) + 2.0F},
                 {r, g, b, 255U});
        }

        const float motion = settings_.reduced_motion ? 0.0F : elapsed_;
        for (int index = 0; index < 14; ++index)
        {
            const float lane = static_cast<float>(index % 5);
            const float width = metrics_.px(170.0F + static_cast<float>((index * 37) % 210));
            const float wrap = static_cast<float>(metrics_.output_width) + width * 2.0F;
            float x =
                std::fmod(static_cast<float>(index * 311) + motion * (8.0F + lane * 3.0F), wrap) -
                width;
            const float y = metrics_.px(90.0F + lane * 74.0F);
            fill({x, y, width, metrics_.px(26.0F + lane * 4.0F)}, {65U, 72U, 73U, 18U});
            fill({x + width * 0.23F, y - metrics_.px(18.0F), width * 0.45F, metrics_.px(22.0F)},
                 {71U, 76U, 77U, 14U});
        }

        const float horizon = static_cast<float>(metrics_.output_height) * 0.69F;
        const float block = std::max(8.0F, metrics_.px(24.0F));
        for (float x = 0.0F; x < static_cast<float>(metrics_.output_width) + block; x += block)
        {
            const float wave = std::sin(x * 0.006F + motion * 0.025F) * block * 1.7F +
                               std::sin(x * 0.014F) * block;
            const float top = horizon + wave;
            fill({x, top, block + 1.0F, static_cast<float>(metrics_.output_height) - top},
                 {12U, 22U, 20U, 255U});
            fill({x, top, block + 1.0F, std::max(2.0F, block * 0.12F)}, {43U, 68U, 47U, 255U});
        }

        const float tower_x = static_cast<float>(metrics_.output_width) * 0.72F;
        const float tower_w = metrics_.px(94.0F);
        const float tower_top = metrics_.px(260.0F);
        fill({tower_x, tower_top, tower_w, horizon - tower_top}, {17U, 20U, 21U, 255U});
        for (float y = tower_top; y < horizon; y += block * 0.65F)
        {
            fill({tower_x, y, tower_w, 1.0F}, {56U, 61U, 61U, 150U});
        }
        fill({tower_x - metrics_.px(22.0F), tower_top - metrics_.px(18.0F),
              tower_w + metrics_.px(44.0F), metrics_.px(20.0F)},
             {91U, 20U, 23U, 255U});

        for (int index = 0; index < 26; ++index)
        {
            const float seed = static_cast<float>(index * 97);
            const float x =
                std::fmod(seed * 11.0F + motion * (15.0F + static_cast<float>(index % 4) * 5.0F),
                          static_cast<float>(metrics_.output_width));
            const float y = std::fmod(seed * 7.0F + std::sin(motion + seed) * 22.0F, horizon);
            const float size = metrics_.px(2.0F + static_cast<float>(index % 3));
            fill({x, y, size, size}, {230U, 42U, 47U, static_cast<Uint8>(80 + (index % 4) * 35)});
        }
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {2U, 4U, 5U, 52U});
    }

    void draw_chrome()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const float top = metrics_.safe.y + 26.0F * s;
        text("OPEN", metrics_.safe.x + 34.0F * s, top, 58.0F, colors.text, TextAlign::left, true);
        text("MINER", metrics_.safe.x + 205.0F * s, top, 58.0F, colors.text, TextAlign::left, true);
        text("Z", metrics_.safe.x + 406.0F * s, top - 7.0F * s, 72.0F, colors.accent,
             TextAlign::left, true);
        fill({metrics_.safe.x + 36.0F * s, top + 72.0F * s, 390.0F * s, 3.0F * s}, colors.accent);
        text("CLEAN-ROOM FRONTEND  /  BUILD 0.1.0", metrics_.safe.x + 38.0F * s, top + 84.0F * s,
             14.0F, colors.secondary);

        const std::string_view visible_profile =
            steam_identity_verified_ && !online_persona_name_.empty()
                ? std::string_view{online_persona_name_}
                : std::string_view{profile_name_};
        text(visible_profile, metrics_.safe.x + metrics_.safe.width - 28.0F * s, top + 4.0F * s,
             20.0F, colors.text, TextAlign::right);
        text(steam_identity_verified_ ? "STEAM PROFILE" : "LOCAL PROFILE",
             metrics_.safe.x + metrics_.safe.width - 28.0F * s, top + 34.0F * s, 13.0F,
             colors.secondary, TextAlign::right);

        const float bottom = metrics_.safe.y + metrics_.safe.height - 34.0F * s;
        text(online_join_stage_ == OnlineJoinStage::idle
                 ? "ARROWS / WASD  NAVIGATE    ENTER  SELECT    ESC  BACK"
                 : "CONNECTION IN PROGRESS  /  KEEP OPENMINERZ RUNNING",
             metrics_.safe.x + 34.0F * s, bottom, 13.0F, colors.secondary);
        text(settings_.presentation == PresentationProfile::classic ? "CLASSIC PROFILE"
                                                                    : "REBIRTH PROFILE",
             metrics_.safe.x + metrics_.safe.width - 28.0F * s, bottom, 13.0F, colors.accent,
             TextAlign::right);
    }

    void draw_world_hud()
    {
        const auto colors = palette(settings_.high_contrast);
        const auto &player = player_.state();
        const float s = metrics_.scale;
        const float safe_left = metrics_.safe.x + 26.0F * s;
        const float safe_right = metrics_.safe.x + metrics_.safe.width - 26.0F * s;
        const float safe_top = metrics_.safe.y + 22.0F * s;
        const float safe_bottom = metrics_.safe.y + metrics_.safe.height - 24.0F * s;
        const auto sky = current_sky_state();

        Rect world_card{safe_left, safe_top, 374.0F * s, 86.0F * s};
        fill(world_card, {5U, 8U, 9U, 208U});
        stroke(world_card, {129U, 143U, 145U, 110U});
        fill({world_card.x, world_card.y, 5.0F * s, world_card.height}, colors.accent);
        text(inspector_view_ ? "WORLD INSPECTOR"
                             : (online_gameplay_active() ? "STEAM MULTIPLAYER"
                                                         : "OFFLINE SURVIVAL"),
             world_card.x + 20.0F * s,
             world_card.y + 13.0F * s, 12.0F, colors.accent);
        text(world_preview_->biome_name() == "CLASSIC"
                 ? "ORIGIN FRONTIER"
                 : std::format("{} FRONTIER", world_preview_->biome_name()),
             world_card.x + 20.0F * s, world_card.y + 34.0F * s, 23.0F, colors.text,
             TextAlign::left, true);
        text(std::format("SEED {}  /  {} BIOME", world_seed_, world_preview_->biome_name()),
             world_card.x + 20.0F * s, world_card.y + 65.0F * s, 11.0F, colors.secondary);

        Rect clock_card{safe_right - 272.0F * s, safe_top, 272.0F * s, 86.0F * s};
        fill(clock_card, {5U, 8U, 9U, 208U});
        stroke(clock_card, {129U, 143U, 145U, 110U});
        text(sky.period, clock_card.x + 18.0F * s, clock_card.y + 14.0F * s, 12.0F, colors.accent);
        text(day_night_.clock_text(), clock_card.x + 18.0F * s, clock_card.y + 34.0F * s, 25.0F,
             colors.text, TextAlign::left, true);
        text(std::format("DAY {}", static_cast<int>(std::floor(day_night_.day())) + 1),
             clock_card.x + clock_card.width - 18.0F * s, clock_card.y + 17.0F * s, 12.0F,
             colors.secondary, TextAlign::right);
        text(world_time_speed_ == 0.0F ? "PAUSED" : std::format("CYCLE {:.0f}X", world_time_speed_),
             clock_card.x + clock_card.width - 18.0F * s, clock_card.y + 56.0F * s, 11.0F,
             world_time_speed_ == 0.0F ? colors.accent : colors.secondary, TextAlign::right);
        fill({clock_card.x + 18.0F * s, clock_card.y + clock_card.height - 7.0F * s,
              clock_card.width - 36.0F * s, 2.0F * s},
             {68U, 78U, 80U, 180U});
        fill({clock_card.x + 18.0F * s, clock_card.y + clock_card.height - 7.0F * s,
              (clock_card.width - 36.0F * s) * sky.time_of_day, 2.0F * s},
             colors.accent);

        const float center_x = static_cast<float>(metrics_.output_width) * 0.5F;
        const float compass_y = safe_top + 111.0F * s;
        Rect compass{center_x - 190.0F * s, compass_y, 380.0F * s, 37.0F * s};
        fill(compass, {5U, 8U, 9U, 156U});
        fill({compass.x + compass.width * 0.5F - 1.0F * s, compass.y, 2.0F * s, 9.0F * s},
             colors.accent);
        constexpr std::array compass_points{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        constexpr float pi = 3.14159265358979323846F;
        const float heading = std::fmod(player.yaw * 180.0F / pi + 360.0F, 360.0F);
        const int heading_index = static_cast<int>(std::floor(heading / 45.0F));
        const float marker_spacing = 74.0F * s;
        const float compass_center = compass.x + compass.width * 0.5F;
        for (int offset = -3; offset <= 4; ++offset)
        {
            const int absolute_index = heading_index + offset;
            const float marker_heading = static_cast<float>(absolute_index) * 45.0F;
            const float x = compass_center + (marker_heading - heading) / 45.0F * marker_spacing;
            if (x < compass.x + 12.0F * s || x > compass.x + compass.width - 12.0F * s)
                continue;
            const int wrapped = (absolute_index % 8 + 8) % 8;
            const float center_distance = std::abs(x - compass_center) / (compass.width * 0.5F);
            const float emphasis = 1.0F - std::clamp(center_distance, 0.0F, 1.0F);
            const SDL_Color marker_color =
                emphasis > 0.72F ? colors.text
                                 : with_alpha(colors.secondary, 0.45F + emphasis * 0.55F);
            text(compass_points[static_cast<std::size_t>(wrapped)], x,
                 compass.y + (emphasis > 0.72F ? 9.0F : 11.0F) * s,
                 emphasis > 0.72F ? 15.0F : 11.0F, marker_color, TextAlign::center);
        }

        // The original's top-of-screen block counter: current distance from the
        // start point and the best distance reached this session.
        text("DISTANCE - MAX", center_x, compass.y + compass.height + 6.0F * s, 9.0F,
             colors.secondary, TextAlign::center);
        text(std::format("{} - {}", current_distance_, max_distance_), center_x,
             compass.y + compass.height + 21.0F * s, 13.0F, colors.text, TextAlign::center, true);

        const bool submerged = player_underwater(player);
        if (submerged)
        {
            Rect water_status{center_x - 116.0F * s, compass.y + compass.height + 44.0F * s,
                              232.0F * s, 27.0F * s};
            fill(water_status, {4U, 37U, 52U, 218U});
            stroke(water_status, {94U, 186U, 201U, 155U});
            const float oxygen = std::clamp(player_oxygen_, 0.0F, 1.0F);
            const float inset = 3.0F * s;
            fill({water_status.x + inset, water_status.y + inset,
                  (water_status.width - inset * 2.0F) * oxygen, water_status.height - inset * 2.0F},
                 {24U, 119U, 151U, 180U});
            text("OXYGEN", center_x, water_status.y + 7.0F * s, 9.0F, {205U, 244U, 248U, 255U},
                 TextAlign::center, true);
        }

        if (!settings_.hud_visible)
        {
            text("HUD HIDDEN IN SETTINGS  /  ESC RETURNS TO MENU", safe_left, safe_bottom, 11.0F,
                 colors.secondary);
            return;
        }

        // InGameHUD damage feedback: a low-health red wash plus directional
        // indicators that swing around the crosshair toward each attacker.
        const float center_y_feedback = static_cast<float>(metrics_.output_height) * 0.5F;
        const float hurt = 1.0F - std::clamp(player.health / 100.0F, 0.0F, 1.0F);
        if (hurt > 0.0F)
            fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
                  static_cast<float>(metrics_.output_height)},
                 {static_cast<Uint8>(hurt * 102.0F), 0U, 0U, static_cast<Uint8>(hurt * 120.0F)});
        // InGameHUD.Draw overlays the lava fade after the low-health wash and
        // promotes it to opaque red once the player's feet are two blocks
        // below the global lava surface.
        const float lava = world::lava_submersion(player.position.y);
        if (lava > 0.0F)
            fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
                  static_cast<float>(metrics_.output_height)},
                 {255U, 0U, 0U, static_cast<Uint8>(lava * 255.0F)});
        for (const auto &indicator : damage_indicators_)
        {
            const float fade =
                indicator.age < 2.0F ? 1.0F : std::clamp(3.0F - indicator.age, 0.0F, 1.0F);
            if (fade <= 0.0F)
                continue;
            const float world_angle = std::atan2(indicator.source.x - player.position.x,
                                                 -(indicator.source.z - player.position.z));
            const float screen_angle = world_angle - player.yaw;
            const float dir_x = std::sin(screen_angle);
            const float dir_y = -std::cos(screen_angle);
            const float perp_x = -dir_y;
            const float perp_y = dir_x;
            const float ring = 158.0F * s;
            const float tip = 196.0F * s;
            const float half = 24.0F * s;
            const SDL_FColor blood{0.545F * fade, 0.0F, 0.0F, fade};
            const std::array<SDL_Vertex, 3> wedge{
                SDL_Vertex{{center_x + dir_x * tip, center_y_feedback + dir_y * tip}, blood, {}},
                SDL_Vertex{{center_x + dir_x * ring + perp_x * half,
                            center_y_feedback + dir_y * ring + perp_y * half},
                           blood,
                           {}},
                SDL_Vertex{{center_x + dir_x * ring - perp_x * half,
                            center_y_feedback + dir_y * ring - perp_y * half},
                           blood,
                           {}}};
            SDL_RenderGeometry(renderer_, nullptr, wedge.data(), static_cast<int>(wedge.size()),
                               nullptr, 0);
        }

        const float center_y = static_cast<float>(metrics_.output_height) * 0.5F;
        const InventoryItemId held_item = inventory_.selected_item();
        const bool scoped_weapon = held_item == InventoryItemId::rocket_launcher ||
                                   held_item == InventoryItemId::guided_rocket_launcher ||
                                   held_item == InventoryItemId::iron_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::copper_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::gold_space_bolt_action_rifle ||
                                   held_item == InventoryItemId::diamond_space_bolt_action_rifle;
        const bool preview_scoped_aim = options_.preview_viewmodel.has_value() &&
                                        (*options_.preview_viewmodel == "aim-idle" ||
                                         *options_.preview_viewmodel == "aim-fire" ||
                                         (*options_.preview_viewmodel == "aim" &&
                                          options_.preview_viewmodel_progress >= 0.999F));
        const bool scoped_aim =
            scoped_weapon && (gun_aim_progress_ >= 0.999F || preview_scoped_aim);
        // InGameHUD sizes the reticle opening directly from Innaccuracy times
        // display height, clamped to eleven pixels. It disappears for every
        // shouldered weapon, not only scoped families.
        float crosshair_gap = 11.0F;
        if (const GunStats *stats = selected_gun(); stats != nullptr)
            crosshair_gap = std::max(11.0F, stats->inaccuracy * metrics_.output_height);
        constexpr float arm = 9.0F;
        constexpr float thick = 3.0F;
        const bool preview_shouldering =
            options_.preview_viewmodel.has_value() &&
            (*options_.preview_viewmodel == "aim" || *options_.preview_viewmodel == "aim-idle" ||
             *options_.preview_viewmodel == "aim-fire");
        if (!shouldering_ && !preview_shouldering)
        {
            const SDL_Color crosshair_color = has_melee_target()
                                                  ? SDL_Color{255U, 0U, 0U, 255U}
                                                  : SDL_Color{255U, 255U, 255U, 255U};
            fill({center_x - crosshair_gap - arm, center_y - thick * 0.5F, arm, thick},
                 crosshair_color);
            fill({center_x + crosshair_gap, center_y - thick * 0.5F, arm, thick}, crosshair_color);
            fill({center_x - thick * 0.5F, center_y - crosshair_gap - arm, thick, arm},
                 crosshair_color);
            fill({center_x - thick * 0.5F, center_y + crosshair_gap, thick, arm}, crosshair_color);
        }

        if (guided_lock_required_ > 0.0F && dragon_.visible() && scoped_aim)
        {
            const world::Vec3 eye{player.position.x,
                                  player.position.y + world::PlayerSimulation::eye_height,
                                  player.position.z};
            const world::Vec3 delta{dragon_.dragon().position.x - eye.x,
                                    dragon_.dragon().position.y - eye.y,
                                    dragon_.dragon().position.z - eye.z};
            const world::Vec3 forward = player_.view_direction();
            const world::Vec3 right{std::cos(player.yaw), 0.0F, std::sin(player.yaw)};
            const world::Vec3 up{right.y * forward.z - right.z * forward.y,
                                 right.z * forward.x - right.x * forward.z,
                                 right.x * forward.y - right.y * forward.x};
            const float depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z;
            if (depth > 0.01F)
            {
                const GunStats *stats = selected_gun();
                const float magnification = stats != nullptr ? stats->shoulder_magnification : 2.0F;
                const float vertical_fov =
                    std::lerp(settings_.vertical_fov, settings_.vertical_fov / magnification,
                              gun_aim_progress_) *
                    0.01745329251994329577F;
                const float tangent_vertical = std::tan(vertical_fov * 0.5F);
                const float aspect = static_cast<float>(metrics_.output_width) /
                                     static_cast<float>(metrics_.output_height);
                const float tangent_horizontal = tangent_vertical * aspect;
                const float ndc_x = (delta.x * right.x + delta.y * right.y + delta.z * right.z) /
                                    (depth * tangent_horizontal);
                const float ndc_y = -(delta.x * up.x + delta.y * up.y + delta.z * up.z) /
                                    (depth * tangent_vertical);
                const float lock_x =
                    (ndc_x * 0.5F + 0.5F) * static_cast<float>(metrics_.output_width);
                const float lock_y =
                    (ndc_y * 0.5F + 0.5F) * static_cast<float>(metrics_.output_height);
                const float lock_size = 35.0F + 215.0F * (1.0F - guided_lock_distance_ / 250.0F);
                const Rect lock{lock_x - lock_size * 0.5F, lock_y - lock_size * 0.5F, lock_size,
                                lock_size};
                const SDL_Color lock_color =
                    guided_locked_ ? SDL_Color{255U, 0U, 0U, 255U} : SDL_Color{0U, 255U, 0U, 255U};
                SDL_Texture *lock_texture =
                    guided_locked_ ? missile_lock_texture_ : missile_locking_texture_;
                if (lock_texture != nullptr)
                {
                    SDL_SetTextureColorMod(lock_texture, lock_color.r, lock_color.g, lock_color.b);
                    SDL_SetTextureAlphaMod(lock_texture, lock_color.a);
                    const SDL_FRect destination{lock.x, lock.y, lock.width, lock.height};
                    SDL_RenderTexture(renderer_, lock_texture, nullptr, &destination);
                }
                else
                {
                    stroke(lock, lock_color, 2.0F);
                }
            }
        }

        if (scoped_aim && sniper_scope_texture_ != nullptr)
        {
            float scope_width{};
            float scope_height{};
            if (SDL_GetTextureSize(sniper_scope_texture_, &scope_width, &scope_height))
            {
                const SDL_FRect scope{center_x - scope_width * 0.5F, center_y - scope_height * 0.5F,
                                      scope_width, scope_height};
                SDL_SetTextureColorMod(sniper_scope_texture_, 255U, 255U, 255U);
                SDL_SetTextureAlphaMod(sniper_scope_texture_, 255U);
                SDL_RenderTexture(renderer_, sniper_scope_texture_, nullptr, &scope);
                fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
                      std::max(0.0F, scope.y)},
                     {0U, 0U, 0U, 255U});
                fill({0.0F, scope.y + scope.h, static_cast<float>(metrics_.output_width),
                      std::max(0.0F,
                               static_cast<float>(metrics_.output_height) - scope.y - scope.h)},
                     {0U, 0U, 0U, 255U});
                fill({0.0F, scope.y, std::max(0.0F, scope.x), scope.h}, {0U, 0U, 0U, 255U});
                fill({scope.x + scope.w, scope.y,
                      std::max(0.0F, static_cast<float>(metrics_.output_width) - scope.x - scope.w),
                      scope.h},
                     {0U, 0U, 0U, 255U});
            }
        }

        const bool modern_inventory = inventory_.profile() == InventoryProfile::modern;
        const float vitals_height = modern_inventory ? 116.0F : 70.0F;
        Rect vitals{safe_left, safe_bottom - vitals_height * s, 310.0F * s, vitals_height * s};
        fill(vitals, {5U, 8U, 9U, 204U});
        stroke(vitals, {129U, 143U, 145U, 100U});
        text("VITALS", vitals.x + 17.0F * s, vitals.y + 12.0F * s, 11.0F, colors.secondary);
        text(std::format("{:.0f}", player.health), vitals.x + vitals.width - 17.0F * s,
             vitals.y + 10.0F * s, 14.0F, colors.text, TextAlign::right);
        fill({vitals.x + 17.0F * s, vitals.y + 38.0F * s, vitals.width - 34.0F * s, 18.0F * s},
             {35U, 12U, 14U, 230U});
        fill({vitals.x + 17.0F * s, vitals.y + 38.0F * s,
              (vitals.width - 34.0F * s) * std::clamp(player.health / 100.0F, 0.0F, 1.0F),
              18.0F * s},
             colors.accent);
        if (modern_inventory)
        {
            text("STAMINA", vitals.x + 17.0F * s, vitals.y + 68.0F * s, 10.0F, colors.secondary);
            fill({vitals.x + 17.0F * s, vitals.y + 91.0F * s, vitals.width - 34.0F * s, 8.0F * s},
                 {20U, 30U, 31U, 230U});
            fill({vitals.x + 17.0F * s, vitals.y + 91.0F * s,
                  (vitals.width - 34.0F * s) * std::clamp(player.stamina / 100.0F, 0.0F, 1.0F),
                  8.0F * s},
                 {196U, 210U, 201U, 255U});
        }

        const float slot = 60.0F * s;
        const float gap = 7.0F * s;
        const float hotbar_width =
            slot * PlayerInventory::tray_size + gap * (PlayerInventory::tray_size - 1U);
        const float hotbar_x = center_x - hotbar_width * 0.5F;
        const float hotbar_y = safe_bottom - slot;
        for (std::size_t index = 0; index < PlayerInventory::tray_size; ++index)
        {
            Rect cell{hotbar_x + static_cast<float>(index) * (slot + gap), hotbar_y, slot, slot};
            const bool selected = index == inventory_.selected_index();
            const bool filled = inventory_.tray()[index].has_value();
            fill(cell, selected ? SDL_Color{47U, 24U, 26U, 240U}
                                : (filled ? SDL_Color{36U, 40U, 42U, 226U}
                                          : SDL_Color{5U, 8U, 9U, 210U}));
            if (filled)
                fill({cell.x, cell.y, cell.width, cell.height * 0.45F}, {255U, 255U, 255U, 13U});
            stroke(cell, selected ? colors.accent : SDL_Color{129U, 143U, 145U, 115U},
                   selected ? 2.0F * s : 1.0F);
            if (filled)
            {
                const bool has_count = inventory_.tray()[index]->count > 1U;
                const Rect icon{cell.x + 5.0F * s, cell.y + 4.0F * s, 50.0F * s,
                                (has_count ? 38.0F : 50.0F) * s};
                draw_item_icon(*inventory_.tray()[index], icon);
                draw_item_durability(*inventory_.tray()[index], icon);
            }
            text(std::format("{}", index + 1U), cell.x + 6.0F * s, cell.y + 5.0F * s, 9.0F,
                 colors.secondary);
            if (filled && inventory_.tray()[index]->count > 1U)
            {
                const std::string count_label = std::format("{}", inventory_.tray()[index]->count);
                fill({cell.x + 3.0F * s, cell.y + 42.0F * s, cell.width - 6.0F * s, 15.0F * s},
                     {8U, 10U, 11U, 224U});
                text(count_label, cell.x + cell.width - 6.0F * s, cell.y + 43.0F * s, 9.0F,
                     colors.text, TextAlign::right);
            }
        }
        std::string selected_label{item_name(inventory_.selected_item())};
        if (const GunStats *stats = selected_gun();
            stats != nullptr && inventory_.selected().has_value())
            selected_label = std::format("{}  {}/{}", item_name(inventory_.selected_item()),
                                         inventory_.selected()->rounds_in_clip,
                                         inventory_.count(stats->ammo_item));
        else if ((inventory_.selected_item() == InventoryItemId::gps ||
                  inventory_.selected_item() == InventoryItemId::teleport_gps) &&
                 inventory_.selected().has_value())
        {
            const InventoryStack &navigation = *inventory_.selected();
            if (navigation.waypoint_valid)
            {
                const float dx = navigation.waypoint[0] - player.position.x;
                const float dy = navigation.waypoint[1] - player.position.y;
                const float dz = navigation.waypoint[2] - player.position.z;
                selected_label = std::format(
                    "{}: {} - DISTANCE: {}", item_name(navigation.item), navigation.waypoint_name,
                    static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz)));
            }
            else
                selected_label = std::format("{}: UNSET", item_name(navigation.item));
        }
        text(selected_label, center_x, hotbar_y - 24.0F * s, 12.0F, colors.text, TextAlign::center,
             true);
        if (modern_inventory)
            text(std::format("QUICK TRAY {} / 2   ·   G SWITCH",
                             inventory_.current_tray_index() + 1U),
                 center_x, hotbar_y - 45.0F * s, 9.0F, colors.secondary, TextAlign::center);

        Rect telemetry{safe_right - 330.0F * s, safe_bottom - 139.0F * s, 330.0F * s, 139.0F * s};
        fill(telemetry, {5U, 8U, 9U, 204U});
        stroke(telemetry, {129U, 143U, 145U, 100U});
        text("WORLD TELEMETRY", telemetry.x + 17.0F * s, telemetry.y + 13.0F * s, 11.0F,
             colors.accent);
        text(std::format("X  {}     Y  {}     Z  {}",
                         static_cast<int>(std::floor(player.position.x)),
                         static_cast<int>(std::floor(player.position.y)),
                         static_cast<int>(std::floor(player.position.z))),
             telemetry.x + 17.0F * s, telemetry.y + 39.0F * s, 15.0F, colors.text);
        text(player.target.has_value()
                 ? std::format("TARGET {}  /  {:.1f}M", world::block_name(player.target->block),
                               player.target->distance)
                 : std::format("{} CHUNKS  /  {} FACES", world_preview_->stats().chunk_count,
                               world_preview_->stats().visible_face_count),
             telemetry.x + 17.0F * s, telemetry.y + 68.0F * s, 11.0F, colors.secondary);
        const bool imported = world_preview_->stats().original_atlas_loaded;
        text(world_preview_->stats().depth_buffered
                 ? std::format("D3D11 + {} + SUN {} + ART",
                               settings_.ambient_occlusion ? "AO" : "NO AO",
                               uppercase(to_string(settings_.shadow_quality)))
                 : (imported && world_preview_->stats().original_skies_loaded
                        ? "ORIGINAL ATLAS + SKY ACTIVE"
                        : (imported ? "ORIGINAL 8×8 ATLAS ACTIVE" : "ORIGINAL ATLAS NOT IMPORTED")),
             telemetry.x + 17.0F * s, telemetry.y + 92.0F * s, 11.0F,
             imported ? SDL_Color{102U, 211U, 143U, 255U} : colors.accent);
        text(inspector_view_ ? std::format("S{}   F1 PLAY   Q/E TIME   B BIOME",
                                           world_preview_->stats().stream_serial)
                             : (online_gameplay_active()
                                    ? std::format("S{}   WASD   T CHAT   TAB PLAYERS",
                                                  world_preview_->stats().stream_serial)
                                    : std::format("S{}   WASD   SPACE   T CYCLE   F1 VIEW",
                                                  world_preview_->stats().stream_serial)),
             telemetry.x + 17.0F * s, telemetry.y + 116.0F * s, 10.0F, colors.secondary);
    }

    [[nodiscard]] Rect inventory_panel() const noexcept
    {
        const float s = metrics_.scale;
        // Wide enough for the slot grid on the right and the cookbook on the left.
        const float width = 1180.0F * s;
        const float height = 680.0F * s;
        return {(static_cast<float>(metrics_.output_width) - width) * 0.5F,
                (static_cast<float>(metrics_.output_height) - height) * 0.5F, width, height};
    }

    // Every storage surface uses the same 8-wide cell geometry. Keeping crate,
    // backpack, and tray dimensions identical makes transfers visually stable.
    static constexpr float inventory_slot_pitch = 66.0F;
    static constexpr float inventory_slot_gap = 2.0F;

    [[nodiscard]] std::size_t visible_inventory_slot_count() const noexcept
    {
        return crate_slot_base + (active_crate_.has_value() ? crate_slot_count : 0U);
    }

    [[nodiscard]] Rect crate_slot_rect(std::size_t crate_index) const noexcept
    {
        if (!active_crate_.has_value() || crate_index >= crate_slot_count)
            return {};
        const Rect panel = inventory_panel();
        const float s = metrics_.scale;
        const float size = inventory_slot_pitch * s;
        const float gap = inventory_slot_gap * s;
        const std::size_t row = crate_index / 8U;
        const std::size_t column = crate_index % 8U;
        return {panel.x + 36.0F * s + static_cast<float>(column) * (size + gap),
                panel.y + 218.0F * s + static_cast<float>(row) * (size + gap), size, size};
    }

    [[nodiscard]] Rect inventory_slot_rect(std::size_t linear_index) const noexcept
    {
        if (linear_index >= crate_slot_base)
            return crate_slot_rect(linear_index - crate_slot_base);
        const Rect panel = inventory_panel();
        const float s = metrics_.scale;
        const float size = inventory_slot_pitch * s;
        const float gap = inventory_slot_gap * s;
        const float grid_width = size * 8.0F + gap * 7.0F;
        const float x = panel.x + panel.width - grid_width - 36.0F * s;
        if (linear_index < PlayerInventory::storage_size)
        {
            const std::size_t row = linear_index / 8U;
            const std::size_t column = linear_index % 8U;
            return {x + static_cast<float>(column) * (size + gap),
                    panel.y + 218.0F * s + static_cast<float>(row) * (size + gap), size, size};
        }
        // The tray row: 14 px below the backpack block, like the original.
        const float tray_y = panel.y + 218.0F * s + 4.0F * (size + gap) - gap + 14.0F * s;
        const std::size_t column = linear_index - PlayerInventory::storage_size;
        return {x + static_cast<float>(column) * (size + gap), tray_y, size, size};
    }

    void move_inventory_focus(int horizontal, int vertical) noexcept
    {
        const bool crate = active_crate_.has_value() && inventory_focus_ >= crate_slot_base;
        const std::size_t base = crate ? crate_slot_base : 0U;
        const std::size_t rows = crate ? 4U : 5U;
        const std::size_t local = inventory_focus_ - base;
        const int row = static_cast<int>(local / 8U);
        const int column = static_cast<int>(local % 8U);
        const int next_row = (row + vertical + static_cast<int>(rows)) % static_cast<int>(rows);
        const int next_column = (column + horizontal + 8) % 8;
        inventory_focus_ = base + static_cast<std::size_t>(next_row * 8 + next_column);
    }

    [[nodiscard]] Rect crafting_pane() const noexcept
    {
        const Rect panel = inventory_panel();
        const float s = metrics_.scale;
        const float grid_width = inventory_slot_pitch * s * 8.0F + inventory_slot_gap * s * 7.0F;
        return {panel.x + 36.0F * s, panel.y + 148.0F * s, panel.width - grid_width - 108.0F * s,
                panel.height - 178.0F * s};
    }

    static constexpr int modern_grid_columns = 6;

    // PlayerInventory.Discovered: a recipe becomes visible in the classic pane
    // once you hold its result, one of its ingredients, or a gun fed by its
    // result.
    [[nodiscard]] bool recipe_discovered(const CraftingRecipe &recipe) const noexcept
    {
        // InfiniteResourceMode: everything is discovered and craftable.
        if (infinite_resources_)
            return true;
        const auto slot_matches = [&](const PlayerInventory::Slot &slot) {
            if (!slot.has_value())
                return false;
            if (slot->item == recipe.result)
                return true;
            if (const GunStats *stats = gun_stats(slot->item);
                stats != nullptr && stats->ammo_item == recipe.result)
                return true;
            for (const CraftingIngredient &ingredient : recipe.ingredients)
                if (ingredient.count != 0U && ingredient.item == slot->item)
                    return true;
            return false;
        };
        for (const auto &slot : inventory_.storage())
            if (slot_matches(slot))
                return true;
        for (const auto &slot : inventory_.tray())
            if (slot_matches(slot))
                return true;
        return false;
    }

    [[nodiscard]] bool recipe_in_category(const CraftingRecipe &recipe) const noexcept
    {
        return craft_category_ == -1 || item_category(recipe.result) == craft_category_;
    }

    // PlayerInventory.DiscoverRecipies: craftable discoveries first, then
    // discovered-but-missing ones, then every recipe producing an ingredient of
    // a listed recipe, with CookBook[0] as the never-empty fallback.
    [[nodiscard]] std::vector<std::size_t> classic_recipe_list() const
    {
        const auto &book = crafting_recipes();
        std::vector<std::size_t> list;
        std::vector<bool> listed(book.size(), false);
        for (const bool wants_craftable : {true, false})
            for (std::size_t index = 0; index < book.size(); ++index)
                if (!listed[index] && recipe_in_category(book[index]) &&
                    recipe_discovered(book[index]) &&
                    inventory_.can_craft(book[index]) == wants_craftable)
                {
                    listed[index] = true;
                    list.push_back(index);
                }
        for (std::size_t cursor = 0; cursor < list.size(); ++cursor)
            for (const CraftingIngredient &ingredient : book[list[cursor]].ingredients)
            {
                if (ingredient.count == 0U)
                    continue;
                for (std::size_t index = 0; index < book.size(); ++index)
                    if (!listed[index] && book[index].result == ingredient.item &&
                        recipe_in_category(book[index]))
                    {
                        listed[index] = true;
                        list.push_back(index);
                    }
            }
        if (list.empty())
            list.push_back(0U);
        return list;
    }

    // The modern browser lists the whole cookbook for the active tab.
    [[nodiscard]] std::vector<std::size_t> modern_recipe_list() const
    {
        const auto &book = crafting_recipes();
        std::vector<std::size_t> list;
        for (std::size_t index = 0; index < book.size(); ++index)
            if (recipe_in_category(book[index]))
                list.push_back(index);
        return list;
    }

    [[nodiscard]] std::vector<std::size_t> active_recipe_list() const
    {
        return craft_modern_ ? modern_recipe_list() : classic_recipe_list();
    }

    [[nodiscard]] static const char *category_label(int category) noexcept
    {
        for (const CraftCategoryEntry &entry : craft_categories)
            if (entry.id == category)
                return entry.label;
        return "ALL";
    }

    void cycle_craft_category(int direction)
    {
        const int count = static_cast<int>(craft_categories.size());
        int position = 0;
        for (int index = 0; index < count; ++index)
            if (craft_categories[static_cast<std::size_t>(index)].id == craft_category_)
                position = index;
        position = ((position + direction) % count + count) % count;
        craft_category_ = craft_categories[static_cast<std::size_t>(position)].id;
        craft_scroll_ = 0;
        craft_selected_.reset();
        sound_bank_->play_click();
    }

    // Clicking a component jumps to the recipe that produces it, exactly like
    // the original ingredient hit test.
    void jump_to_ingredient_recipe(const CraftingIngredient &ingredient)
    {
        const auto list = active_recipe_list();
        const auto &book = crafting_recipes();
        for (std::size_t position = 0; position < list.size(); ++position)
            if (book[list[position]].result == ingredient.item)
            {
                craft_selected_ = list[position];
                craft_scroll_ = craft_modern_ ? static_cast<int>(position) / modern_grid_columns
                                              : static_cast<int>(position);
                sound_bank_->play_click();
                return;
            }
    }

    void handle_craft_hit(const CraftHit &hit, Uint8 button)
    {
        const bool shift = (SDL_GetModState() & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) != 0U;
        const auto &book = crafting_recipes();
        switch (hit.kind)
        {
        case CraftHitKind::mode_classic:
        case CraftHitKind::mode_modern: {
            const bool modern = hit.kind == CraftHitKind::mode_modern;
            if (craft_modern_ != modern)
            {
                craft_modern_ = modern;
                craft_scroll_ = 0;
                settings_.crafting_menu_modern = modern;
                dirty_ = true;
            }
            sound_bank_->play_click();
            return;
        }
        case CraftHitKind::tab:
            if (hit.index < craft_categories.size())
            {
                craft_category_ = craft_categories[hit.index].id;
                craft_scroll_ = 0;
                craft_selected_.reset();
            }
            sound_bank_->play_click();
            return;
        case CraftHitKind::category_cycle:
            cycle_craft_category(button == SDL_BUTTON_RIGHT ? -1 : 1);
            return;
        case CraftHitKind::recipe:
            if (button != SDL_BUTTON_LEFT || hit.index >= book.size())
                return;
            // Hovering already selected the recipe (the ingredient strip below
            // follows the pointer); a click crafts it outright.
            craft_selected_ = hit.index;
            craft_recipe(hit.index, shift);
            return;
        case CraftHitKind::create:
            if (button == SDL_BUTTON_LEFT && craft_selected_.has_value())
                craft_recipe(*craft_selected_, shift);
            return;
        case CraftHitKind::ingredient:
            if (button != SDL_BUTTON_LEFT || !craft_selected_.has_value() ||
                *craft_selected_ >= book.size())
                return;
            if (hit.index < book[*craft_selected_].ingredients.size())
            {
                const CraftingIngredient ingredient = book[*craft_selected_].ingredients[hit.index];
                if (ingredient.count != 0U)
                    jump_to_ingredient_recipe(ingredient);
            }
            return;
        }
    }

    void craft_recipe(std::size_t recipe_index, bool craft_all = false)
    {
        const auto &book = crafting_recipes();
        if (recipe_index >= book.size())
            return;
        const CraftingRecipe &recipe = book[recipe_index];
        if (!infinite_resources_ && !inventory_.can_craft(recipe))
        {
            // The original plays "Error" when a craft can't complete.
            sound_bank_->play_cue("Error");
            toast("MISSING MATERIALS", std::string{item_name(recipe.result)});
            return;
        }
        std::uint32_t crafted = 0U;
        if (infinite_resources_)
        {
            // Creative crafting consumes nothing; shift makes a full stack.
            const auto amount = static_cast<std::uint16_t>(
                craft_all ? std::max<std::uint16_t>(recipe.result_count,
                                                    PlayerInventory::block_stack_limit)
                          : recipe.result_count);
            const std::uint16_t remainder = inventory_.add_item(recipe.result, amount);
            if (remainder != 0U)
                drop_stack(InventoryStack{recipe.result, remainder});
            crafted = amount;
        }
        else
        {
            do
            {
                const std::uint16_t remainder = inventory_.craft(recipe);
                if (remainder != 0U)
                    drop_stack(InventoryStack{recipe.result, remainder});
                crafted += recipe.result_count;
            } while (craft_all && inventory_.can_craft(recipe));
        }
        // CraftingUIScreen plays the dedicated "craft" cue on success.
        sound_bank_->play_cue("craft");
        caption("[ ITEM CRAFTED ]", 1.2F);
        toast("CRAFTED", std::format("{} ×{}", item_name(recipe.result), crafted));
        sync_selected_item();
    }

    [[nodiscard]] std::optional<std::size_t> inventory_slot_at(float x, float y) const noexcept
    {
        for (std::size_t index = 0; index < visible_inventory_slot_count(); ++index)
            if (inventory_slot_rect(index).contains(x, y))
                return index;
        return std::nullopt;
    }

    [[nodiscard]] PlayerInventory::Slot &visible_inventory_slot(std::size_t index)
    {
        if (index < crate_slot_base)
            return inventory_.slot(index);
        return crate_inventories_[*active_crate_][index - crate_slot_base];
    }

    [[nodiscard]] const PlayerInventory::Slot &visible_inventory_slot(std::size_t index) const
    {
        if (index < crate_slot_base)
            return inventory_.slot(index);
        return crate_inventories_.at(*active_crate_)[index - crate_slot_base];
    }

    [[nodiscard]] static std::uint16_t add_stack_to_crate(CrateSlots &slots,
                                                          const InventoryStack &source)
    {
        std::uint16_t remaining = source.count;
        const std::uint16_t limit = item_stack_limit(source.item);
        for (auto &slot : slots)
        {
            if (!slot.has_value() || slot->item != source.item || slot->count >= limit)
                continue;
            const std::uint16_t moved =
                std::min<std::uint16_t>(remaining, static_cast<std::uint16_t>(limit - slot->count));
            slot->count = static_cast<std::uint16_t>(slot->count + moved);
            remaining = static_cast<std::uint16_t>(remaining - moved);
            if (remaining == 0U)
                return 0U;
        }
        for (auto &slot : slots)
        {
            if (slot.has_value())
                continue;
            const std::uint16_t moved = std::min(remaining, limit);
            slot = source.with_count(moved);
            remaining = static_cast<std::uint16_t>(remaining - moved);
            if (remaining == 0U)
                return 0U;
        }
        return remaining;
    }

    [[nodiscard]] bool quick_move_inventory_slot(std::size_t index)
    {
        if (!active_crate_.has_value())
            return index < crate_slot_base && inventory_.move_to_other_section(index);
        const CrateKey crate_location = *active_crate_;
        const CrateSlots crate_before = crate_inventories_[crate_location];
        auto &source = visible_inventory_slot(index);
        if (!source.has_value())
            return false;
        const std::uint16_t original = source->count;
        const std::uint16_t remainder =
            index >= crate_slot_base
                ? inventory_.add_stack(*source)
                : add_stack_to_crate(crate_inventories_[*active_crate_], *source);
        if (remainder == original)
            return false;
        if (remainder == 0U)
            source.reset();
        else
            source->count = remainder;
        sync_selected_item();
        publish_crate_differences(crate_location, crate_before);
        return true;
    }

    void close_inventory()
    {
        if (held_stack_.has_value())
        {
            // add_stack preserves the held stack's per-instance state; rebuilding it
            // from the item id refilled a spent gun clip on every overlay close.
            const std::uint16_t remainder = inventory_.add_stack(*held_stack_);
            if (remainder != 0U)
            {
                held_stack_->count = remainder;
                toast("INVENTORY FULL", "Place or drop the held stack before closing.");
                inventory_open_ = true;
                SDL_SetWindowRelativeMouseMode(window_, false);
                return;
            }
            held_stack_.reset();
        }
        inventory_open_ = false;
        active_crate_.reset();
        SDL_SetWindowRelativeMouseMode(window_, !inspector_view_);
        sync_selected_item();
    }

    void inventory_pointer(float x, float y, Uint8 button)
    {
        mouse_x_ = x;
        mouse_y_ = y;
        const Rect panel = inventory_panel();
        const float s = metrics_.scale;
        const Rect close{panel.x + panel.width - 48.0F * s, panel.y + 17.0F * s, 28.0F * s,
                         28.0F * s};
        if (button == SDL_BUTTON_LEFT && close.contains(x, y))
        {
            sound_bank_->play_click();
            close_inventory();
            return;
        }
        if (!active_crate_.has_value() && (button == SDL_BUTTON_LEFT || button == SDL_BUTTON_RIGHT))
        {
            for (const CraftHit &craft_hit : craft_hits_)
                if (craft_hit.rect.contains(x, y))
                {
                    handle_craft_hit(craft_hit, button);
                    return;
                }
        }
        const auto hit = inventory_slot_at(x, y);
        if (inventory_pointer_route(hit.has_value()) == InventoryPointerRoute::ignore)
        {
            // Do not turn an imprecise UI click into an irreversible world action.
            // The old path dropped the cursor stack whenever the pointer landed a
            // single pixel outside the panel, which was especially easy just after
            // relative mouse mode released. Q remains the explicit, documented drop
            // command for either the cursor stack or the focused slot.
            return;
        }
        inventory_focus_ = *hit;
        if (button == SDL_BUTTON_LEFT &&
            (SDL_GetModState() & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) != 0U)
        {
            if (quick_move_inventory_slot(*hit))
                sound_bank_->play_click();
            return;
        }

        const std::optional<CrateKey> crate_location = active_crate_;
        std::optional<CrateSlots> crate_before;
        if (crate_location.has_value())
            crate_before = crate_inventories_[*crate_location];
        auto &target = visible_inventory_slot(*hit);
        if (button == SDL_BUTTON_LEFT)
        {
            if (!held_stack_.has_value())
            {
                held_stack_ = target;
                target.reset();
            }
            else if (!target.has_value())
            {
                target = held_stack_;
                held_stack_.reset();
            }
            else if (target->item == held_stack_->item &&
                     target->count < item_stack_limit(target->item))
            {
                const auto available =
                    static_cast<std::uint16_t>(item_stack_limit(target->item) - target->count);
                const auto moved = std::min(available, held_stack_->count);
                target->count = static_cast<std::uint16_t>(target->count + moved);
                held_stack_->count = static_cast<std::uint16_t>(held_stack_->count - moved);
                if (held_stack_->count == 0U)
                    held_stack_.reset();
            }
            else
            {
                std::swap(target, held_stack_);
            }
        }
        else if (button == SDL_BUTTON_RIGHT)
        {
            if (!held_stack_.has_value() && target.has_value())
            {
                if (target->count == 1U)
                {
                    held_stack_ = target;
                    target.reset();
                }
                else
                {
                    const auto split = static_cast<std::uint16_t>(target->count / 2U);
                    held_stack_ = target->with_count(split);
                    target->count = static_cast<std::uint16_t>(target->count - split);
                }
            }
            else if (held_stack_.has_value() && !target.has_value())
            {
                target = held_stack_->with_count(1U);
                held_stack_->count--;
                if (held_stack_->count == 0U)
                    held_stack_.reset();
            }
            else if (held_stack_.has_value() && target.has_value() &&
                     target->item == held_stack_->item &&
                     target->count < item_stack_limit(target->item))
            {
                target->count++;
                held_stack_->count--;
                if (held_stack_->count == 0U)
                    held_stack_.reset();
            }
        }
        sound_bank_->play_click();
        sync_selected_item();
        if (crate_location.has_value() && crate_before.has_value())
            publish_crate_differences(*crate_location, *crate_before);
    }

    void inventory_wheel(float amount)
    {
        if (amount == 0.0F)
            return;
        if (!active_crate_.has_value() && crafting_pane().contains(mouse_x_, mouse_y_))
        {
            // The upper bound depends on the active list; the draw pass clamps.
            craft_scroll_ = std::max(0, craft_scroll_ + (amount > 0.0F ? -1 : 1));
            return;
        }
        const auto hit = inventory_slot_at(mouse_x_, mouse_y_);
        if (!hit.has_value())
            return;
        inventory_focus_ = *hit;
        const std::optional<CrateKey> crate_location = active_crate_;
        std::optional<CrateSlots> crate_before;
        if (crate_location.has_value())
            crate_before = crate_inventories_[*crate_location];
        auto &target = visible_inventory_slot(*hit);
        bool changed{};
        if (amount > 0.0F)
        {
            // Original ItemCountUp: move one item from the selected stack to
            // the hand, combining only with a compatible held stack.
            if (target.has_value() && (!held_stack_.has_value() ||
                                       (held_stack_->item == target->item &&
                                        held_stack_->count < item_stack_limit(held_stack_->item))))
            {
                if (!held_stack_.has_value())
                    held_stack_ = target->with_count(1U);
                else
                    held_stack_->count++;
                target->count--;
                if (target->count == 0U)
                    target.reset();
                changed = true;
            }
        }
        else if (held_stack_.has_value() &&
                 (!target.has_value() || (target->item == held_stack_->item &&
                                          target->count < item_stack_limit(target->item))))
        {
            if (!target.has_value())
                target = held_stack_->with_count(1U);
            else
                target->count++;
            held_stack_->count--;
            if (held_stack_->count == 0U)
                held_stack_.reset();
            changed = true;
        }
        if (changed)
        {
            sound_bank_->play_click();
            sync_selected_item();
            if (crate_location.has_value() && crate_before.has_value())
                publish_crate_differences(*crate_location, *crate_before);
        }
    }

    void drop_stack(const InventoryStack &stack)
    {
        world_preview_->spawn_dropped_stack(stack, player_.state());
        sound_bank_->play_drop();
        caption("[ ITEM DROPPED ]");
        toast("ITEM DROPPED", std::format("{} ×{}", item_name(stack.item), stack.count));
    }

    void eject_crate_contents(const CrateKey &location)
    {
        const auto crate = crate_inventories_.find(location);
        if (crate == crate_inventories_.end())
            return;
        const world::Vec3 center{static_cast<float>(location[0]) + 0.5F,
                                 static_cast<float>(location[1]) + 0.5F,
                                 static_cast<float>(location[2]) + 0.5F};
        for (const auto &slot : crate->second)
            if (slot.has_value())
                world_preview_->spawn_dropped_stack_at(*slot, center, 3.0F);
        publish_local_crate_destroy(location);
        crate_inventories_.erase(crate);
        if (active_crate_.has_value() && *active_crate_ == location)
        {
            active_crate_.reset();
            inventory_open_ = false;
            held_stack_.reset();
            SDL_SetWindowRelativeMouseMode(window_, !inspector_view_);
        }
    }

    void drop_inventory_selection()
    {
        if (held_stack_.has_value())
        {
            sound_bank_->play_click();
            drop_stack(*held_stack_);
            held_stack_.reset();
        }
        else
        {
            const std::optional<CrateKey> crate_location = active_crate_;
            std::optional<CrateSlots> crate_before;
            if (crate_location.has_value())
                crate_before = crate_inventories_[*crate_location];
            const auto hovered = inventory_slot_at(mouse_x_, mouse_y_);
            const std::size_t index = hovered.value_or(inventory_focus_);
            auto &target = visible_inventory_slot(index);
            if (!target.has_value())
                return;
            sound_bank_->play_click();
            drop_stack(*target);
            target.reset();
            if (crate_location.has_value() && crate_before.has_value())
                publish_crate_differences(*crate_location, *crate_before);
        }
        sync_selected_item();
    }

    void draw_block_icon(world::BlockType block, Rect rect)
    {
        // Isometric cube corners shared by both the textured and fallback
        // paths. Proper 2:1 dimetric proportions (top diamond twice as wide as
        // tall, sides as deep as the diamond) squared off inside the rect so
        // blocks read as cubes instead of squashed slabs.
        const float size = std::min(rect.width, rect.height);
        const float ox = rect.x + (rect.width - size) * 0.5F;
        const float oy = rect.y + (rect.height - size) * 0.5F;
        const float cx = ox + size * 0.5F;
        const float left = ox + size * 0.06F;
        const float right = ox + size * 0.94F;
        const float half = size * 0.44F;
        const float top = oy + size * 0.04F;
        const float middle = top + half * 0.5F;
        const float front = top + half;
        const float bottom = oy + size * 0.97F;
        const float side_bottom = middle + (bottom - front);

        SDL_Texture *atlas = world_preview_ != nullptr ? world_preview_->terrain_atlas() : nullptr;
        if (atlas != nullptr)
        {
            // Sample the real terrain tiles: top face plus the two visible
            // sides, shaded like the world cube pickups.
            struct IconFace final
            {
                world::FaceDirection direction{};
                // Quad corners in top-left, top-right, bottom-right,
                // bottom-left order so v0 maps to the top of the tile (keeps
                // grass strips and door panels upright).
                std::array<SDL_FPoint, 4> corners{};
                float light{};
            };
            const std::array<IconFace, 3> faces{{
                {world::FaceDirection::positive_y,
                 {{{cx, top}, {right, middle}, {cx, front}, {left, middle}}},
                 1.0F},
                {world::FaceDirection::negative_x,
                 {{{left, middle}, {cx, front}, {cx, bottom}, {left, side_bottom}}},
                 0.70F},
                {world::FaceDirection::positive_z,
                 {{{cx, front}, {right, middle}, {right, side_bottom}, {cx, bottom}}},
                 0.88F},
            }};
            constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
            for (const IconFace &face : faces)
            {
                const std::uint8_t tile = world::atlas_tile(block, face.direction);
                const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
                const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
                const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
                const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
                const std::array<SDL_FPoint, 4> uv{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
                std::array<SDL_Vertex, 6> vertices{};
                for (std::size_t index = 0U; index < vertices.size(); ++index)
                {
                    const auto corner = static_cast<std::size_t>(order[index]);
                    vertices[index] = {face.corners[corner],
                                       {face.light, face.light, face.light, 1.0F},
                                       uv[corner]};
                }
                SDL_RenderGeometry(renderer_, atlas, vertices.data(),
                                   static_cast<int>(vertices.size()), nullptr, 0);
            }
            return;
        }

        const auto raw = world::fallback_color(block);
        const auto shade = [](std::uint8_t value, float amount) {
            return static_cast<std::uint8_t>(
                std::clamp(static_cast<float>(value) * amount, 0.0F, 255.0F));
        };
        const auto vertex = [&](float x, float y, float light) {
            return SDL_Vertex{{x, y},
                              {shade(raw.red, light) / 255.0F, shade(raw.green, light) / 255.0F,
                               shade(raw.blue, light) / 255.0F, 1.0F},
                              {0.0F, 0.0F}};
        };
        std::array<SDL_Vertex, 12> vertices{
            vertex(cx, top, 1.18F),      vertex(right, middle, 1.08F),
            vertex(cx, front, 1.08F),    vertex(cx, top, 1.18F),
            vertex(cx, front, 1.08F),    vertex(left, middle, 1.08F),
            vertex(left, middle, 0.70F), vertex(cx, front, 0.70F),
            vertex(cx, bottom, 0.70F),   vertex(left, middle, 0.70F),
            vertex(cx, bottom, 0.70F),   vertex(left, side_bottom, 0.70F),
        };
        std::array<SDL_Vertex, 6> right_face{
            vertex(cx, front, 0.88F),          vertex(right, middle, 0.88F),
            vertex(right, side_bottom, 0.88F), vertex(cx, front, 0.88F),
            vertex(right, side_bottom, 0.88F), vertex(cx, bottom, 0.88F),
        };
        SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                           nullptr, 0);
        SDL_RenderGeometry(renderer_, nullptr, right_face.data(),
                           static_cast<int>(right_face.size()), nullptr, 0);
    }

    void draw_item_icon(const InventoryStack &stack, Rect rect)
    {
        if (draw_original_item_icon(stack.item, rect))
            return;
        if (stack.block != world::BlockType::empty)
        {
            draw_block_icon(stack.block, rect);
            return;
        }
        const auto quad = [&](Rect shape, SDL_Color color) { fill(shape, color); };
        const float u = rect.width / 36.0F;
        switch (stack.item)
        {
        case InventoryItemId::stone_pickaxe: {
            std::array<SDL_Vertex, 6> handle{
                SDL_Vertex{
                    {rect.x + 11.0F * u, rect.y + 30.0F * u}, {0.43F, 0.25F, 0.10F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 15.0F * u, rect.y + 32.0F * u}, {0.43F, 0.25F, 0.10F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 26.0F * u, rect.y + 8.0F * u}, {0.58F, 0.34F, 0.14F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 11.0F * u, rect.y + 30.0F * u}, {0.43F, 0.25F, 0.10F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 26.0F * u, rect.y + 8.0F * u}, {0.58F, 0.34F, 0.14F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 23.0F * u, rect.y + 6.0F * u}, {0.58F, 0.34F, 0.14F, 1.0F}, {}}};
            SDL_RenderGeometry(renderer_, nullptr, handle.data(), static_cast<int>(handle.size()),
                               nullptr, 0);
            quad({rect.x + 10.0F * u, rect.y + 5.0F * u, 22.0F * u, 6.0F * u},
                 {139U, 137U, 128U, 255U});
            break;
        }
        case InventoryItemId::compass:
            quad({rect.x + 5.0F * u, rect.y + 5.0F * u, 26.0F * u, 26.0F * u},
                 {181U, 151U, 60U, 255U});
            quad({rect.x + 8.0F * u, rect.y + 8.0F * u, 20.0F * u, 20.0F * u},
                 {32U, 37U, 38U, 255U});
            quad({rect.x + 17.0F * u, rect.y + 9.0F * u, 2.0F * u, 18.0F * u},
                 {211U, 51U, 48U, 255U});
            quad({rect.x + 9.0F * u, rect.y + 17.0F * u, 18.0F * u, 2.0F * u},
                 {224U, 228U, 220U, 255U});
            break;
        case InventoryItemId::pistol:
            quad({rect.x + 5.0F * u, rect.y + 8.0F * u, 26.0F * u, 10.0F * u},
                 {104U, 109U, 110U, 255U});
            quad({rect.x + 18.0F * u, rect.y + 17.0F * u, 8.0F * u, 14.0F * u},
                 {66U, 69U, 70U, 255U});
            quad({rect.x + 4.0F * u, rect.y + 10.0F * u, 7.0F * u, 5.0F * u},
                 {175U, 178U, 174U, 255U});
            break;
        case InventoryItemId::knife: {
            std::array<SDL_Vertex, 3> blade{
                SDL_Vertex{{rect.x + 6.0F * u, rect.y + 9.0F * u}, {0.85F, 0.87F, 0.87F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 26.0F * u, rect.y + 16.0F * u}, {0.65F, 0.68F, 0.69F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 10.0F * u, rect.y + 17.0F * u}, {0.95F, 0.96F, 0.94F, 1.0F}, {}}};
            SDL_RenderGeometry(renderer_, nullptr, blade.data(), static_cast<int>(blade.size()),
                               nullptr, 0);
            quad({rect.x + 23.0F * u, rect.y + 15.0F * u, 7.0F * u, 15.0F * u},
                 {91U, 57U, 30U, 255U});
            break;
        }
        case InventoryItemId::bullets:
            for (int index = 0; index < 3; ++index)
            {
                const float x = rect.x + (10.0F + static_cast<float>(index) * 7.0F) * u;
                quad({x, rect.y + 9.0F * u, 5.0F * u, 19.0F * u}, {190U, 143U, 48U, 255U});
                quad({x, rect.y + 8.0F * u, 5.0F * u, 4.0F * u}, {93U, 91U, 86U, 255U});
            }
            break;
        case InventoryItemId::stick: {
            std::array<SDL_Vertex, 6> shaft{
                SDL_Vertex{
                    {rect.x + 8.0F * u, rect.y + 28.0F * u}, {0.50F, 0.32F, 0.14F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 12.0F * u, rect.y + 30.0F * u}, {0.42F, 0.26F, 0.11F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 28.0F * u, rect.y + 8.0F * u}, {0.62F, 0.42F, 0.20F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 8.0F * u, rect.y + 28.0F * u}, {0.50F, 0.32F, 0.14F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 28.0F * u, rect.y + 8.0F * u}, {0.62F, 0.42F, 0.20F, 1.0F}, {}},
                SDL_Vertex{
                    {rect.x + 24.0F * u, rect.y + 6.0F * u}, {0.58F, 0.38F, 0.17F, 1.0F}, {}}};
            SDL_RenderGeometry(renderer_, nullptr, shaft.data(), static_cast<int>(shaft.size()),
                               nullptr, 0);
            break;
        }
        default:
            quad({rect.x + 8.0F * u, rect.y + 8.0F * u, 20.0F * u, 20.0F * u},
                 {134U, 139U, 139U, 255U});
            break;
        }
    }

    void draw_item_durability(const InventoryStack &stack, Rect rect)
    {
        if (item_self_damage_per_use(stack.item) <= 0.0F || stack.durability >= 1.0F)
            return;
        const float health = std::clamp(stack.durability, 0.0F, 1.0F);
        const auto channel = [&](float broken, float healthy) {
            return static_cast<Uint8>(std::lerp(broken, healthy, health));
        };
        const SDL_Color color{channel(255.0F, 67.0F), channel(69.0F, 188.0F), channel(0.0F, 0.0F),
                              255U};
        const float margin = rect.width * 0.14F;
        const float height = std::max(4.0F, rect.height * 0.10F);
        const Rect background{rect.x + margin, rect.y + rect.height - height - margin * 0.35F,
                              rect.width - margin * 2.0F, height};
        fill(background, {0U, 0U, 0U, 230U});
        fill({background.x + 1.0F, background.y + 1.0F, (background.width - 2.0F) * health,
              background.height - 2.0F},
             color);
    }

    [[nodiscard]] StaticModelAsset *item_art(InventoryItemId item) const noexcept
    {
        switch (item)
        {
        case InventoryItemId::stone_pickaxe:
        case InventoryItemId::copper_pickaxe:
        case InventoryItemId::iron_pickaxe:
        case InventoryItemId::gold_pickaxe:
        case InventoryItemId::diamond_pickaxe:
        case InventoryItemId::bloodstone_pickaxe:
            return pickaxe_art_.get();
        case InventoryItemId::stone_spade:
        case InventoryItemId::copper_spade:
        case InventoryItemId::iron_spade:
        case InventoryItemId::gold_spade:
        case InventoryItemId::diamond_spade:
            return spade_art_.get();
        case InventoryItemId::stone_axe:
        case InventoryItemId::copper_axe:
        case InventoryItemId::iron_axe:
        case InventoryItemId::gold_axe:
        case InventoryItemId::diamond_axe:
            return axe_art_.get();
        case InventoryItemId::compass:
            return compass_art_.get();
        case InventoryItemId::pistol:
        case InventoryItemId::gold_pistol:
        case InventoryItemId::diamond_pistol:
        case InventoryItemId::blood_stone_pistol:
            return pistol_art_.get();
        case InventoryItemId::knife:
        case InventoryItemId::gold_knife:
        case InventoryItemId::diamond_knife:
        case InventoryItemId::blood_stone_knife:
            return knife_art_.get();
        case InventoryItemId::bullets:
        case InventoryItemId::iron_bullets:
        case InventoryItemId::gold_bullets:
        case InventoryItemId::diamond_bullets:
        case InventoryItemId::blood_stone_bullets:
        case InventoryItemId::laser_bullets:
        case InventoryItemId::rocket_ammo:
            return ammo_art_.get();
        case InventoryItemId::torch:
            return torch_art_.get();
        case InventoryItemId::assault_rifle:
        case InventoryItemId::gold_assault_rifle:
        case InventoryItemId::diamond_assault_rifle:
        case InventoryItemId::blood_stone_assault_rifle:
            return ak_art_.get();
        case InventoryItemId::pump_shotgun:
        case InventoryItemId::gold_pump_shotgun:
        case InventoryItemId::diamond_pump_shotgun:
        case InventoryItemId::blood_stone_pump_shotgun:
            return shotgun_art_.get();
        case InventoryItemId::bolt_action_rifle:
        case InventoryItemId::gold_bolt_action_rifle:
        case InventoryItemId::diamond_bolt_action_rifle:
        case InventoryItemId::blood_stone_bolt_action_rifle:
            return rifle_art_.get();
        case InventoryItemId::smg:
        case InventoryItemId::gold_smg:
        case InventoryItemId::diamond_smg:
        case InventoryItemId::blood_stone_smg:
            return smg_art_.get();
        case InventoryItemId::lmg:
        case InventoryItemId::gold_lmg:
        case InventoryItemId::diamond_lmg:
        case InventoryItemId::blood_stone_lmg:
            return lmg_art_.get();
        case InventoryItemId::rocket_launcher:
        case InventoryItemId::guided_rocket_launcher:
            return rpg_art_.get();
        case InventoryItemId::grenade:
        case InventoryItemId::modern_sticky_grenade:
            return grenade_art_.get();
        case InventoryItemId::iron_space_assault_rifle:
        case InventoryItemId::copper_space_assault_rifle:
        case InventoryItemId::gold_space_assault_rifle:
        case InventoryItemId::diamond_space_assault_rifle:
            return space_ak_art_.get();
        case InventoryItemId::iron_space_pistol:
        case InventoryItemId::copper_space_pistol:
        case InventoryItemId::gold_space_pistol:
        case InventoryItemId::diamond_space_pistol:
            return space_pistol_art_.get();
        case InventoryItemId::iron_space_pump_shotgun:
        case InventoryItemId::copper_space_pump_shotgun:
        case InventoryItemId::gold_space_pump_shotgun:
        case InventoryItemId::diamond_space_pump_shotgun:
            return space_shotgun_art_.get();
        case InventoryItemId::iron_space_bolt_action_rifle:
        case InventoryItemId::copper_space_bolt_action_rifle:
        case InventoryItemId::gold_space_bolt_action_rifle:
        case InventoryItemId::diamond_space_bolt_action_rifle:
            return space_rifle_art_.get();
        case InventoryItemId::iron_space_smg:
        case InventoryItemId::copper_space_smg:
        case InventoryItemId::gold_space_smg:
        case InventoryItemId::diamond_space_smg:
            return space_smg_art_.get();
        case InventoryItemId::coal:
        case InventoryItemId::copper_ore:
        case InventoryItemId::iron_ore:
        case InventoryItemId::gold_ore:
            return ore_art_.get();
        case InventoryItemId::diamond:
            return gems_art_.get();
        case InventoryItemId::iron:
        case InventoryItemId::copper:
        case InventoryItemId::gold:
            return bars_art_.get();
        case InventoryItemId::iron_laser_sword:
        case InventoryItemId::copper_laser_sword:
        case InventoryItemId::gold_laser_sword:
        case InventoryItemId::diamond_laser_sword:
        case InventoryItemId::blood_stone_laser_sword:
        case InventoryItemId::space_knife:
            return saber_art_.get();
        case InventoryItemId::gps:
            return locator_art_.get();
        case InventoryItemId::teleport_gps:
            return teleporter_art_.get();
        case InventoryItemId::clock:
            return clock_art_.get();
        case InventoryItemId::gun_powder:
        case InventoryItemId::explosive_powder:
            return gunpowder_art_.get();
        case InventoryItemId::brass_casing:
        case InventoryItemId::iron_casing:
        case InventoryItemId::gold_casing:
        case InventoryItemId::diamond_casing:
            return ammo_art_.get();
        default:
            return nullptr;
        }
    }

    bool draw_original_item_icon(InventoryItemId item, Rect rect)
    {
        StaticModelAsset *asset = item_art(item);
        if (asset == nullptr || !asset->loaded())
            return false;
        constexpr float pi = 3.14159265358979323846F;
        float rotation_x{};
        float rotation_z{};
        const auto value = static_cast<std::uint16_t>(item);
        const bool tool_shaped = (value >= 23U && value <= 38U) || item == InventoryItemId::torch;
        const bool knife_shaped =
            item == InventoryItemId::knife || item == InventoryItemId::gold_knife ||
            item == InventoryItemId::diamond_knife || item == InventoryItemId::blood_stone_knife;
        // Flat-faced instruments lie in the XZ plane; without this pitch they
        // render edge-on as a sliver in the icon view.
        if (item == InventoryItemId::compass || item == InventoryItemId::clock ||
            item == InventoryItemId::gps || item == InventoryItemId::teleport_gps)
            rotation_x = pi * 0.5F;
        else if (knife_shaped)
        {
            rotation_x = -pi * 0.5F;
            rotation_z = pi * 0.10F;
        }
        else if (gun_stats(item) != nullptr)
        {
            // The original UI entity rolls guns 45 degrees like the pistol icon.
            rotation_x = -pi * 0.5F;
            rotation_z = -pi * 0.25F;
        }
        else if (tool_shaped || item_stack_limit(item) == 5000U)
            rotation_z = -pi * 0.25F;
        const float sine_x = std::sin(rotation_x);
        const float cosine_x = std::cos(rotation_x);
        const float sine_z = std::sin(rotation_z);
        const float cosine_z = std::cos(rotation_z);
        const auto transform = [](const std::array<float, 3> &point,
                                  const StaticModelMatrix &matrix) {
            return std::array<float, 3>{
                point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8] + matrix[12],
                point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9] + matrix[13],
                point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]};
        };
        const auto orient = [&](std::array<float, 3> point) {
            const auto center = asset->center();
            point[0] -= center[0];
            point[1] -= center[1];
            point[2] -= center[2];
            const float y = point[1] * cosine_x - point[2] * sine_x;
            const float z = point[1] * sine_x + point[2] * cosine_x;
            const float x = point[0] * cosine_z - y * sine_z;
            return std::array<float, 3>{x, point[0] * sine_z + y * cosine_z, z};
        };
        struct IconTriangle final
        {
            float depth{};
            SDL_Texture *texture{};
            std::array<SDL_Vertex, 3> vertices{};
        };
        std::vector<IconTriangle> triangles;
        float authored_size = 0.43F;
        if (item == InventoryItemId::compass)
            authored_size = 0.35F;
        if (gun_stats(item) != nullptr)
            authored_size = 0.40F;
        if (knife_shaped)
            authored_size = 0.46F;
        const float scale =
            std::min(rect.width, rect.height) * authored_size / std::max(asset->radius(), 0.00001F);
        const float cx = rect.x + rect.width * 0.5F;
        const float cy = rect.y + rect.height * 0.52F;
        const auto wrap_uv = [](float coordinate) {
            float wrapped = coordinate - std::floor(coordinate);
            if (coordinate > 0.0F && std::abs(wrapped) < 0.00001F)
                wrapped = 1.0F;
            return wrapped;
        };
        std::array<float, 3> primary_tint{1.0F, 1.0F, 1.0F};
        std::array<float, 3> secondary_tint{1.0F, 1.0F, 1.0F};
        const auto rgb = [](int red_channel, int green_channel, int blue_channel) {
            constexpr float channel_scale = 1.0F / 255.0F;
            return std::array<float, 3>{red_channel * channel_scale, green_channel * channel_scale,
                                        blue_channel * channel_scale};
        };
        primary_tint = item_material_tint(item);
        switch (item)
        {
        case InventoryItemId::bullets:
            primary_tint = rgb(169, 169, 169);
            secondary_tint = rgb(234, 227, 150);
            break;
        case InventoryItemId::iron_bullets:
            primary_tint = rgb(128, 128, 128);
            secondary_tint = rgb(234, 227, 150);
            break;
        case InventoryItemId::gold_bullets:
            primary_tint = rgb(255, 215, 0);
            secondary_tint = rgb(234, 227, 150);
            break;
        case InventoryItemId::diamond_bullets:
            primary_tint = rgb(0, 255, 255);
            secondary_tint = rgb(234, 227, 150);
            break;
        case InventoryItemId::blood_stone_bullets:
            primary_tint = rgb(139, 0, 0);
            secondary_tint = rgb(0, 255, 255);
            break;
        case InventoryItemId::laser_bullets:
            primary_tint = rgb(50, 205, 50);
            secondary_tint = rgb(169, 169, 169);
            break;
        case InventoryItemId::rocket_ammo:
            primary_tint = rgb(169, 169, 169);
            secondary_tint = rgb(234, 227, 150);
            break;
        case InventoryItemId::coal:
            primary_tint = rgb(20, 20, 20);
            break;
        case InventoryItemId::copper_ore:
            primary_tint = rgb(99, 146, 131);
            break;
        case InventoryItemId::iron_ore:
            primary_tint = rgb(183, 65, 14);
            break;
        case InventoryItemId::gold_ore:
            primary_tint = rgb(255, 215, 0);
            break;
        case InventoryItemId::diamond:
            primary_tint = rgb(0, 255, 255);
            break;
        case InventoryItemId::iron:
            primary_tint = rgb(128, 128, 128);
            break;
        case InventoryItemId::copper:
            primary_tint = rgb(184, 115, 51);
            break;
        case InventoryItemId::gold:
            primary_tint = rgb(255, 215, 0);
            break;
        default:
            break;
        }
        for (const auto &part : asset->parts())
        {
            const std::array<float, 3> part_tint =
                (part.material_flags & 2U) != 0U
                    ? secondary_tint
                    : ((part.material_flags & 1U) != 0U ? primary_tint
                                                        : std::array<float, 3>{1.0F, 1.0F, 1.0F});
            for (std::size_t triangle = 0; triangle < part.indices.size(); triangle += 3U)
            {
                IconTriangle output;
                output.texture = part.texture;
                for (std::size_t corner = 0; corner < 3U; ++corner)
                {
                    const auto &source = part.vertices[part.indices[triangle + corner]];
                    const auto point = orient(transform(source.position, part.transform));
                    const float light = std::clamp(0.72F + source.normal[1] * 0.20F, 0.48F, 1.0F);
                    constexpr float color_scale = 1.0F / 255.0F;
                    output.vertices[corner] = {
                        {cx + point[0] * scale, cy - point[1] * scale},
                        {part.diffuse[0] * source.color[0] * color_scale * part_tint[0] * light,
                         part.diffuse[1] * source.color[1] * color_scale * part_tint[1] * light,
                         part.diffuse[2] * source.color[2] * color_scale * part_tint[2] * light,
                         source.color[3] * color_scale},
                        {wrap_uv(source.uv[0]), wrap_uv(source.uv[1])}};
                    output.depth += point[2];
                }
                triangles.push_back(output);
            }
        }
        std::stable_sort(triangles.begin(), triangles.end(),
                         [](const IconTriangle &left, const IconTriangle &right) {
                             return left.depth < right.depth;
                         });
        std::vector<SDL_Vertex> run;
        run.reserve(triangles.size() * 3U);
        SDL_Texture *run_texture{};
        bool run_started{};
        const auto flush_run = [&]() {
            if (!run.empty())
                SDL_RenderGeometry(renderer_, run_texture, run.data(), static_cast<int>(run.size()),
                                   nullptr, 0);
            run.clear();
        };
        for (const auto &triangle : triangles)
        {
            if (run_started && triangle.texture != run_texture)
                flush_run();
            run_texture = triangle.texture;
            run_started = true;
            run.insert(run.end(), triangle.vertices.begin(), triangle.vertices.end());
        }
        flush_run();
        return true;
    }

    // A uniform icon cell shared by every part of the crafting pane: a bright
    // plate so the model art pops off the dark panel, dimmed to a silhouette
    // when the recipe or component is out of reach (the original grays the
    // sprite the same way).
    void draw_craft_cell(Rect rect, InventoryItemId item, bool available, bool selected, bool hover)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        fill(rect, available ? SDL_Color{64U, 70U, 73U, 255U} : SDL_Color{20U, 23U, 24U, 255U});
        fill({rect.x, rect.y, rect.width, rect.height * 0.45F},
             {255U, 255U, 255U, available ? 28U : 7U});
        const float inset = rect.width * 0.10F;
        draw_item_icon(InventoryStack{item, 1U},
                       {rect.x + inset, rect.y + inset, rect.width - inset * 2.0F,
                        rect.height - inset * 2.0F});
        if (!available)
            fill(rect, {8U, 10U, 11U, 140U});
        SDL_Color line = available ? SDL_Color{150U, 159U, 161U, 170U} : colors.line;
        if (hover)
            line = colors.text;
        stroke(rect, selected ? colors.accent : line, selected ? 2.0F * s : 1.0F);
    }

    void draw_create_button(Rect rect, bool craftable)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const bool hover = rect.contains(mouse_x_, mouse_y_);
        if (craftable)
        {
            fill(rect, with_alpha(colors.accent, hover ? 1.0F : 0.82F));
            stroke(rect, colors.text, hover ? 2.0F * s : 1.0F);
        }
        else
        {
            fill(rect, {24U, 27U, 28U, 255U});
            stroke(rect, colors.line);
        }
        text("CREATE ITEM", rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F - 8.0F * s,
             11.0F, craftable ? colors.text : SDL_Color{95U, 101U, 102U, 255U}, TextAlign::center,
             craftable);
        craft_hits_.push_back({rect, CraftHitKind::create, 0U});
    }

    // COMPONENTS strip for a recipe: icon plates with have/need badges; a click
    // jumps to the component's own recipe like the original hit test.
    void draw_components_row(const CraftingRecipe &recipe, float x, float y, float cell)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const float gap = 10.0F * s;
        std::size_t drawn = 0;
        for (std::size_t index = 0; index < recipe.ingredients.size(); ++index)
        {
            const CraftingIngredient &ingredient = recipe.ingredients[index];
            if (ingredient.count == 0U)
                continue;
            const Rect rect{x + static_cast<float>(drawn) * (cell + gap), y, cell, cell};
            const std::uint32_t have = inventory_.count(ingredient.item);
            const bool enough = have >= ingredient.count;
            draw_craft_cell(rect, ingredient.item, enough, false,
                            rect.contains(mouse_x_, mouse_y_));
            text(std::format("{}/{}", std::min<std::uint32_t>(have, 9999U), ingredient.count),
                 rect.x + rect.width * 0.5F, rect.y + rect.height + 4.0F * s, 8.0F,
                 enough ? SDL_Color{102U, 211U, 143U, 255U} : colors.accent, TextAlign::center);
            craft_hits_.push_back({rect, CraftHitKind::ingredient, index});
            ++drawn;
        }
    }

    // Port of the original CraftingUIScreen: the discovery-gated icon strip,
    // category cycle button, component icons and CREATE ITEM, restyled.
    void draw_classic_craft(Rect body, const std::vector<std::size_t> &list)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &book = crafting_recipes();
        const float cell = 58.0F * s;
        const float pitch = cell + 8.0F * s;
        const float strip_top = body.y + 20.0F * s;
        const int visible =
            std::max(1, static_cast<int>((body.y + body.height - strip_top) / pitch));
        craft_scroll_ =
            std::clamp(craft_scroll_, 0, std::max(0, static_cast<int>(list.size()) - visible));
        text(std::format("RECIPES {}", list.size()), body.x, body.y, 8.0F, colors.secondary);
        for (int row = 0; row < visible; ++row)
        {
            const std::size_t entry = static_cast<std::size_t>(craft_scroll_ + row);
            if (entry >= list.size())
                break;
            const std::size_t recipe_index = list[entry];
            const CraftingRecipe &recipe = book[recipe_index];
            const Rect rect{body.x, strip_top + static_cast<float>(row) * pitch, cell, cell};
            // The original CraftingUIScreen: hovering a row selects it (the
            // ingredient strip follows the pointer); clicking crafts.
            if (rect.contains(mouse_x_, mouse_y_) && !held_stack_.has_value())
                craft_selected_ = recipe_index;
            draw_craft_cell(rect, recipe.result, inventory_.can_craft(recipe),
                            craft_selected_ == recipe_index, rect.contains(mouse_x_, mouse_y_));
            if (recipe.result_count > 1U)
                text(std::format("{}", recipe.result_count), rect.x + rect.width - 4.0F * s,
                     rect.y + rect.height - 15.0F * s, 8.0F, colors.text, TextAlign::right, true);
            craft_hits_.push_back({rect, CraftHitKind::recipe, recipe_index});
        }
        if (craft_scroll_ > 0)
            text("^", body.x + cell + 6.0F * s, strip_top, 10.0F, colors.secondary);
        if (craft_scroll_ + visible < static_cast<int>(list.size()))
            text("v", body.x + cell + 6.0F * s, body.y + body.height - 16.0F * s, 10.0F,
                 colors.secondary);

        const float dx = body.x + cell + 24.0F * s;
        const CraftingRecipe &recipe = book[*craft_selected_];
        text(item_name(recipe.result), dx, body.y, 16.0F, colors.text, TextAlign::left, true);
        if (recipe.result_count > 1U)
            text(std::format("MAKES ×{}", recipe.result_count), body.x + body.width,
                 body.y + 6.0F * s, 9.0F, colors.secondary, TextAlign::right);
        text(item_description(recipe.result), dx, body.y + 30.0F * s, 9.0F, colors.secondary);
        if (const GunStats *stats = gun_stats(recipe.result); stats != nullptr)
            text(std::format("USES {}", item_name(stats->ammo_item)), dx, body.y + 47.0F * s, 9.0F,
                 colors.secondary);

        const Rect create{dx, body.y + 74.0F * s, 150.0F * s, 32.0F * s};
        draw_create_button(create, inventory_.can_craft(recipe));
        const Rect category{create.x + create.width + 10.0F * s, create.y, 172.0F * s, 32.0F * s};
        const bool category_hover = category.contains(mouse_x_, mouse_y_);
        fill(category,
             category_hover ? SDL_Color{31U, 35U, 36U, 255U} : SDL_Color{18U, 21U, 22U, 255U});
        stroke(category, category_hover ? colors.text : colors.line);
        text(std::format("CATEGORY: {}", category_label(craft_category_)),
             category.x + category.width * 0.5F, category.y + 10.0F * s, 9.0F, colors.text,
             TextAlign::center);
        craft_hits_.push_back({category, CraftHitKind::category_cycle, 0U});
        text("SHIFT+CLICK CRAFTS ALL   RMB CATEGORY BACK", dx, create.y + create.height + 9.0F * s,
             8.0F, with_alpha(colors.secondary, 0.7F));

        text("COMPONENTS", dx, body.y + 152.0F * s, 9.0F, colors.secondary);
        draw_components_row(recipe, dx, body.y + 170.0F * s, cell);
    }

    // The reworked original's category browser: tabs, a full icon grid of the
    // cookbook and a detail card, in our own styling.
    void draw_modern_craft(Rect body, const std::vector<std::size_t> &list)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &book = crafting_recipes();

        const float tab_width = 104.0F * s;
        const float tab_height = 30.0F * s;
        for (std::size_t index = 0; index < craft_categories.size(); ++index)
        {
            const Rect rect{body.x, body.y + static_cast<float>(index) * (tab_height + 6.0F * s),
                            tab_width, tab_height};
            const bool hover = rect.contains(mouse_x_, mouse_y_);
            // Categories activate on mouse-over, the way the user remembers
            // the original browser behaving.
            if (hover && !held_stack_.has_value() && craft_categories[index].id != craft_category_)
            {
                craft_category_ = craft_categories[index].id;
                craft_scroll_ = 0;
                sound_bank_->play_click();
            }
            const bool active = craft_categories[index].id == craft_category_;
            fill(rect, active ? colors.panel_selected
                              : (hover ? SDL_Color{26U, 30U, 31U, 255U}
                                       : SDL_Color{13U, 16U, 17U, 255U}));
            stroke(rect, active ? colors.accent : colors.line, active ? 2.0F * s : 1.0F);
            if (active)
                fill({rect.x, rect.y, 4.0F * s, rect.height}, colors.accent);
            text(craft_categories[index].label, rect.x + 12.0F * s, rect.y + 9.0F * s, 9.0F,
                 active ? colors.text : colors.secondary);
            craft_hits_.push_back({rect, CraftHitKind::tab, index});
        }

        const float detail_height = 168.0F * s;
        const Rect grid{body.x + tab_width + 16.0F * s, body.y, body.width - tab_width - 16.0F * s,
                        body.height - detail_height - 12.0F * s};
        const float pitch = 66.0F * s;
        const float cell = 58.0F * s;
        const int columns = modern_grid_columns;
        const int rows_visible = std::max(1, static_cast<int>(grid.height / pitch));
        const int total_rows = (static_cast<int>(list.size()) + columns - 1) / columns;
        craft_scroll_ = std::clamp(craft_scroll_, 0, std::max(0, total_rows - rows_visible));
        for (int row = 0; row < rows_visible; ++row)
            for (int column = 0; column < columns; ++column)
            {
                const std::size_t entry =
                    static_cast<std::size_t>((craft_scroll_ + row) * columns + column);
                if (entry >= list.size())
                    break;
                const std::size_t recipe_index = list[entry];
                const CraftingRecipe &recipe = book[recipe_index];
                const Rect rect{grid.x + static_cast<float>(column) * pitch,
                                grid.y + static_cast<float>(row) * pitch, cell, cell};
                // Hover selects; click crafts.
                if (rect.contains(mouse_x_, mouse_y_) && !held_stack_.has_value())
                    craft_selected_ = recipe_index;
                draw_craft_cell(rect, recipe.result, inventory_.can_craft(recipe),
                                craft_selected_ == recipe_index, rect.contains(mouse_x_, mouse_y_));
                if (recipe.result_count > 1U)
                    text(std::format("{}", recipe.result_count), rect.x + rect.width - 4.0F * s,
                         rect.y + rect.height - 15.0F * s, 8.0F, colors.text, TextAlign::right,
                         true);
                craft_hits_.push_back({rect, CraftHitKind::recipe, recipe_index});
            }
        if (craft_scroll_ > 0)
            text("^", grid.x + grid.width - 10.0F * s, grid.y, 10.0F, colors.secondary);
        if (craft_scroll_ + rows_visible < total_rows)
            text("v", grid.x + grid.width - 10.0F * s, grid.y + grid.height - 14.0F * s, 10.0F,
                 colors.secondary);

        const float dy = body.y + body.height - detail_height;
        fill({body.x, dy - 8.0F * s, body.width, std::max(1.0F, 1.0F * s)}, colors.line);
        const CraftingRecipe &recipe = book[*craft_selected_];
        const Rect portrait{body.x, dy + 4.0F * s, 84.0F * s, 84.0F * s};
        draw_craft_cell(portrait, recipe.result, inventory_.can_craft(recipe), false, false);
        const float info_x = portrait.x + portrait.width + 16.0F * s;
        text(item_name(recipe.result), info_x, dy + 2.0F * s, 14.0F, colors.text, TextAlign::left,
             true);
        std::string flavor{item_description(recipe.result)};
        if (recipe.result_count > 1U)
            flavor = flavor.empty() ? std::format("Makes ×{}", recipe.result_count)
                                    : std::format("{} — makes ×{}", flavor, recipe.result_count);
        text(flavor, info_x, dy + 26.0F * s, 8.0F, colors.secondary);
        if (const GunStats *stats = gun_stats(recipe.result); stats != nullptr)
            text(std::format("USES {}", item_name(stats->ammo_item)), info_x, dy + 41.0F * s, 8.0F,
                 colors.secondary);
        const Rect create{body.x + body.width - 146.0F * s, dy + 2.0F * s, 146.0F * s, 30.0F * s};
        draw_create_button(create, inventory_.can_craft(recipe));
        text("SHIFT = ALL", create.x + create.width * 0.5F, create.y + create.height + 5.0F * s,
             7.0F, with_alpha(colors.secondary, 0.7F), TextAlign::center);
        text("COMPONENTS", info_x, dy + 60.0F * s, 8.0F, colors.secondary);
        draw_components_row(recipe, info_x, dy + 76.0F * s, 46.0F * s);
    }

    void draw_crafting_pane()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const Rect pane = crafting_pane();
        craft_hits_.clear();

        text("CRAFTING", pane.x, pane.y + 2.0F * s, 13.0F, colors.accent);
        text(craft_modern_ ? "FULL CATALOG BY CATEGORY" : "DISCOVER RECIPES AS YOU PLAY", pane.x,
             pane.y + 22.0F * s, 8.0F, colors.secondary);
        const float seg_width = 96.0F * s;
        for (int segment = 0; segment < 2; ++segment)
        {
            const bool modern = segment == 1;
            const Rect rect{pane.x + pane.width - seg_width * (2.0F - static_cast<float>(segment)) -
                                (modern ? 0.0F : 2.0F * s),
                            pane.y, seg_width, 30.0F * s};
            const bool active = craft_modern_ == modern;
            const bool hover = rect.contains(mouse_x_, mouse_y_);
            fill(rect, active ? colors.panel_selected
                              : (hover ? SDL_Color{26U, 30U, 31U, 255U}
                                       : SDL_Color{13U, 16U, 17U, 255U}));
            stroke(rect, active ? colors.accent : colors.line, active ? 2.0F * s : 1.0F);
            text(modern ? "MODERN" : "CLASSIC", rect.x + rect.width * 0.5F, rect.y + 9.0F * s,
                 10.0F, active ? colors.text : colors.secondary, TextAlign::center);
            craft_hits_.push_back(
                {rect, modern ? CraftHitKind::mode_modern : CraftHitKind::mode_classic, 0U});
        }

        const Rect body{pane.x, pane.y + 44.0F * s, pane.width, pane.height - 44.0F * s};
        const auto list = active_recipe_list();
        if (list.empty())
            return;
        if (!craft_selected_.has_value() ||
            std::find(list.begin(), list.end(), *craft_selected_) == list.end())
            craft_selected_ = list.front();
        if (craft_modern_)
            draw_modern_craft(body, list);
        else
            draw_classic_craft(body, list);
    }

    void draw_inventory_overlay()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const Rect panel = inventory_panel();
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {0U, 0U, 0U, 156U});
        fill(panel, {10U, 13U, 14U, 248U});
        stroke(panel, {156U, 164U, 164U, 210U}, std::max(1.0F, 2.0F * s));
        fill({panel.x, panel.y, panel.width, 6.0F * s}, colors.accent);
        text(active_crate_.has_value() ? "STORAGE CRATE" : "INVENTORY", panel.x + 34.0F * s,
             panel.y + 28.0F * s, 28.0F, colors.text, TextAlign::left, true);
        const std::string inventory_subtitle =
            active_crate_.has_value()
                ? "32 CRATE SLOTS  /  PLAYER INVENTORY"
                : (inventory_.profile() == InventoryProfile::modern
                       ? std::format("32 STORAGE SLOTS  /  16 QUICK SLOTS  /  TRAY {} OF 2",
                                     inventory_.current_tray_index() + 1U)
                       : "32 STORAGE SLOTS  /  8 QUICK TRAY");
        text(inventory_subtitle, panel.x + 36.0F * s, panel.y + 70.0F * s, 11.0F, colors.secondary);
        text(active_crate_.has_value() ? "LMB GRAB / PLACE   RMB SPLIT / ONE   TAB SWITCH GRID   "
                                         "SHIFT+LMB OR Y TRANSFER   Q DROP"
                                       : "LMB GRAB / PLACE   RMB SPLIT / ONE   WHEEL MOVE ONE   "
                                         "SHIFT+LMB QUICK MOVE   Q DROP",
             panel.x + 36.0F * s, panel.y + 108.0F * s, 10.0F, colors.secondary);
        if (!active_crate_.has_value())
            draw_crafting_pane();
        const Rect close{panel.x + panel.width - 48.0F * s, panel.y + 17.0F * s, 28.0F * s,
                         28.0F * s};
        fill(close, close.contains(mouse_x_, mouse_y_) ? SDL_Color{78U, 82U, 82U, 255U}
                                                       : SDL_Color{42U, 46U, 47U, 255U});
        stroke(close, colors.line);
        text("X", close.x + close.width * 0.5F, close.y + 4.0F * s, 13.0F, colors.text,
             TextAlign::center, true);

        const auto hovered = inventory_slot_at(mouse_x_, mouse_y_);
        for (std::size_t index = 0; index < visible_inventory_slot_count(); ++index)
        {
            const Rect slot_rect = inventory_slot_rect(index);
            const bool selected =
                index >= PlayerInventory::storage_size &&
                index - PlayerInventory::storage_size == inventory_.selected_index();
            const bool hover = (hovered.has_value() && *hovered == index) ||
                               (!hovered.has_value() && inventory_focus_ == index);
            const auto &stack = visible_inventory_slot(index);
            const bool filled = stack.has_value();
            // Occupied slots sit on a bright plate so the art pops off the
            // panel; empty slots stay recessed and dark.
            fill(slot_rect,
                 filled
                     ? (hover ? SDL_Color{82U, 89U, 92U, 255U} : SDL_Color{65U, 71U, 74U, 255U})
                     : (hover ? SDL_Color{26U, 30U, 31U, 255U} : SDL_Color{13U, 16U, 17U, 255U}));
            if (filled)
            {
                fill({slot_rect.x + 2.0F * s, slot_rect.y + 2.0F * s, slot_rect.width - 4.0F * s,
                      slot_rect.height * 0.50F},
                     {255U, 255U, 255U, 29U});
                fill({slot_rect.x + 7.0F * s, slot_rect.y + 8.0F * s, slot_rect.width - 14.0F * s,
                      slot_rect.height - 16.0F * s},
                     {220U, 237U, 241U, 10U});
            }
            // The original's Selector: white around the hovered slot, RED while
            // carrying a stack; the active tray slot keeps the accent.
            stroke(slot_rect,
                   selected ? colors.accent
                            : (hover ? (held_stack_.has_value() ? SDL_Color{214U, 40U, 46U, 255U}
                                                                : SDL_Color{255U, 255U, 255U, 255U})
                                     : (filled ? SDL_Color{150U, 159U, 161U, 150U} : colors.line)),
                   selected ? 3.0F * s : (hover ? 2.0F * s : 1.0F * s));
            if (filled)
            {
                const float footer_height = stack->count > 1U ? 18.0F * s : 0.0F;
                const Rect icon_rect{slot_rect.x + 5.0F * s, slot_rect.y + 4.0F * s,
                                     slot_rect.width - 10.0F * s,
                                     slot_rect.height - 8.0F * s - footer_height};
                draw_item_icon(*stack, icon_rect);
                draw_item_durability(*stack, icon_rect);
                if (stack->count > 1U)
                {
                    const Rect footer{slot_rect.x + 2.0F * s,
                                      slot_rect.y + slot_rect.height - footer_height - 2.0F * s,
                                      slot_rect.width - 4.0F * s, footer_height};
                    fill(footer, {9U, 13U, 15U, 226U});
                    text(std::format("{}", stack->count), footer.x + footer.width - 5.0F * s,
                         footer.y + 2.0F * s, 10.0F, {255U, 255U, 255U, 255U}, TextAlign::right,
                         true);
                }
            }
        }
        // Label the tray row above its right-aligned grid, clear of the cookbook.
        const std::string tray_label = inventory_.profile() == InventoryProfile::modern
                                           ? std::format("QUICK TRAY {} / 2   ·   G SWITCH",
                                                         inventory_.current_tray_index() + 1U)
                                           : "QUICK TRAY";
        text(tray_label, inventory_slot_rect(PlayerInventory::storage_size).x,
             inventory_slot_rect(PlayerInventory::storage_size).y - 20.0F * s, 11.0F,
             colors.secondary);
        if (active_crate_.has_value())
            text("CRATE CONTENTS", crate_slot_rect(0U).x, crate_slot_rect(0U).y - 20.0F * s, 11.0F,
                 colors.secondary);
        if (hovered.has_value() && visible_inventory_slot(*hovered).has_value())
        {
            const auto &stack = *visible_inventory_slot(*hovered);
            text(item_name(stack.item), panel.x + panel.width - 36.0F * s, panel.y + 151.0F * s,
                 18.0F, colors.text, TextAlign::right, true);
            text(std::format("{}  /  STACK {}/{}", is_block_item(stack.item) ? "BLOCK" : "ITEM",
                             stack.count, item_stack_limit(stack.item, inventory_.profile())),
                 panel.x + panel.width - 36.0F * s, panel.y + 183.0F * s, 10.0F, colors.secondary,
                 TextAlign::right);
        }
        if (held_stack_.has_value())
        {
            const Rect held{mouse_x_ - 29.0F * s, mouse_y_ - 29.0F * s, 58.0F * s, 58.0F * s};
            draw_item_icon(*held_stack_, held);
            draw_item_durability(*held_stack_, held);
            text(std::format("{}", held_stack_->count), held.x + held.width,
                 held.y + held.height - 13.0F * s, 10.0F, colors.text, TextAlign::right, true);
        }
    }

    void draw_locator_name_dialog()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {0U, 0U, 0U, 168U});
        const float width = std::min(620.0F * s, metrics_.safe.width - 48.0F * s);
        const float height = 270.0F * s;
        const Rect panel{metrics_.safe.x + (metrics_.safe.width - width) * 0.5F,
                         metrics_.safe.y + (metrics_.safe.height - height) * 0.5F, width, height};
        fill(panel, {13U, 17U, 18U, 248U});
        stroke(panel, {154U, 166U, 168U, 210U}, 2.0F * s);
        fill({panel.x, panel.y, 7.0F * s, panel.height}, colors.accent);
        text("NAME", panel.x + 31.0F * s, panel.y + 25.0F * s, 24.0F, colors.text, TextAlign::left,
             true);
        text("ENTER A NAME FOR THIS LOCATOR", panel.x + 31.0F * s, panel.y + 68.0F * s, 12.0F,
             colors.secondary);
        const Rect input{panel.x + 31.0F * s, panel.y + 107.0F * s, panel.width - 62.0F * s,
                         64.0F * s};
        fill(input, {3U, 6U, 7U, 245U});
        stroke(input, colors.accent, 2.0F * s);
        text(locator_name_draft_ + "|", input.x + 19.0F * s, input.y + 17.0F * s, 22.0F,
             colors.text, TextAlign::left, true);
        text(std::format("{}/10", locator_name_characters(locator_name_draft_)),
             input.x + input.width - 14.0F * s, input.y + 23.0F * s, 10.0F, colors.secondary,
             TextAlign::right);
        text("ENTER  ACCEPT     ESC  CANCEL", panel.x + 31.0F * s,
             panel.y + panel.height - 45.0F * s, 11.0F, colors.secondary);
    }

    void draw_host_text_dialog()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {0U, 0U, 0U, 184U});
        const float width = std::min(760.0F * s, metrics_.safe.width - 48.0F * s);
        const float height = 286.0F * s;
        const Rect panel{metrics_.safe.x + (metrics_.safe.width - width) * 0.5F,
                         metrics_.safe.y + (metrics_.safe.height - height) * 0.5F, width, height};
        fill(panel, {10U, 14U, 15U, 252U});
        stroke(panel, {154U, 166U, 168U, 220U}, 2.0F * s);
        fill({panel.x, panel.y, 7.0F * s, panel.height}, colors.accent);
        const bool password = host_text_field_ == HostTextField::password ||
                              host_text_field_ == HostTextField::join_password;
        const bool direct = host_text_field_ == HostTextField::direct_address;
        const std::string_view title = direct ? "DIRECT CONNECT ADDRESS"
                                       : host_text_field_ == HostTextField::join_password
                                           ? "JOIN PASSWORD"
                                       : password ? "SESSION PASSWORD"
                                                  : "SERVER NAME";
        text(title, panel.x + 31.0F * s, panel.y + 25.0F * s, 24.0F, colors.text, TextAlign::left,
             true);
        text(direct ? "HOST[:PORT] OR [IPV6]:PORT  •  DEFAULT PORT 25343"
             : host_text_field_ == HostTextField::join_password
                 ? "SENT ONLY TO THE SELECTED HOST ADMISSION GATE"
             : password ? "LEAVE EMPTY TO ALLOW PASSWORD-FREE JOINING"
                        : "SHOWN IN FRIEND, PUBLIC, AND LAN DISCOVERY",
             panel.x + 31.0F * s, panel.y + 69.0F * s, 12.0F, colors.secondary);
        const Rect input{panel.x + 31.0F * s, panel.y + 110.0F * s, panel.width - 62.0F * s,
                         66.0F * s};
        fill(input, {3U, 6U, 7U, 245U});
        stroke(input, colors.accent, 2.0F * s);
        const std::size_t character_count = locator_name_characters(host_text_draft_);
        const std::string visible = password ? std::string(character_count, '*') : host_text_draft_;
        text(visible + "|", input.x + 19.0F * s, input.y + 17.0F * s, 22.0F, colors.text,
             TextAlign::left, true);
        text(std::format("{}/{}", character_count, direct ? 320 : (password ? 64 : 40)),
             input.x + input.width - 14.0F * s, input.y + 23.0F * s, 10.0F, colors.secondary,
             TextAlign::right);
        text("ENTER  ACCEPT     ESC  CANCEL", panel.x + 31.0F * s,
             panel.y + panel.height - 48.0F * s, 11.0F, colors.secondary);
    }

    void draw_start_hub()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        if (entries.size() < 2U)
            return;

        const bool multiplayer = menu_.screen() == ui::MenuScreen::multiplayer;
        const float slide = (1.0F - smoothstep(transition_)) * -34.0F * s;
        const float x = metrics_.safe.x + 36.0F * s + slide;
        const float top = metrics_.safe.y + 184.0F * s;
        const float content_width = metrics_.safe.width - 72.0F * s;
        text(multiplayer ? "MULTIPLAYER" : "START", x, top, 14.0F, colors.accent);
        text(multiplayer ? "CREATE A LOBBY OR FIND PEOPLE ALREADY PLAYING."
                         : "CHOOSE HOW YOU WANT TO PLAY.",
             x, top + 27.0F * s, 13.0F, colors.secondary);

        const float primary_top = top + 60.0F * s;
        const float primary_gap = 18.0F * s;
        const float primary_width = (content_width - primary_gap) * 0.5F;
        const float primary_height = 194.0F * s;
        for (std::size_t index = 0U; index < 2U; ++index)
        {
            const float focus = index < focus_.size() ? focus_[index] : 0.0F;
            Rect card{x + static_cast<float>(index) * (primary_width + primary_gap), primary_top,
                      primary_width, primary_height};
            card.y -= focus * 5.0F * s;
            const bool selected = index == menu_.selection();
            fill(card, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.84F));
            stroke(card, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            fill({card.x, card.y, selected ? 8.0F * s : 3.0F * s, card.height},
                 selected ? colors.accent : colors.line);
            text(multiplayer ? (index == 0U ? "CREATE" : "DISCOVER")
                             : (index == 0U ? "LOCAL PLAY" : "ONLINE PLAY"),
                 card.x + 28.0F * s, card.y + 25.0F * s, 11.0F, colors.accent);
            std::string badge{entries[index].badge};
            if (!multiplayer && index == 1U)
            {
                badge = steam_identity_verified_                             ? "STEAM READY"
                        : join_browser_state_ == JoinBrowserState::searching ? "CONNECTING"
                                                                             : "STEAM";
            }
            text(badge, card.x + card.width - 25.0F * s, card.y + 25.0F * s, 11.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::right);
            text(entries[index].label, card.x + 28.0F * s, card.y + 64.0F * s, 38.0F,
                 entries[index].enabled ? colors.text : colors.secondary, TextAlign::left, true);
            text(entries[index].description, card.x + 28.0F * s, card.y + 119.0F * s, 14.0F,
                 colors.secondary);
            if (selected)
                text("ENTER  SELECT", card.x + 28.0F * s, card.y + card.height - 34.0F * s, 10.0F,
                     colors.accent);
            if (entries[index].enabled)
                hits_.push_back({card, HitKind::menu_item, index});
        }

        const float utility_top = primary_top + primary_height + 22.0F * s;
        const float utility_height = 78.0F * s;
        if (!multiplayer)
        {
            const std::size_t utility_count = entries.size() - 2U;
            const float utility_gap = 12.0F * s;
            const float utility_width =
                (content_width - utility_gap * static_cast<float>(utility_count - 1U)) /
                static_cast<float>(utility_count);
            for (std::size_t index = 2U; index < entries.size(); ++index)
            {
                const float focus = index < focus_.size() ? focus_[index] : 0.0F;
                Rect card{x + static_cast<float>(index - 2U) * (utility_width + utility_gap),
                          utility_top - focus * 3.0F * s, utility_width, utility_height};
                const bool selected = index == menu_.selection();
                fill(card, with_alpha(selected ? colors.panel_selected : colors.panel,
                                      selected ? 0.98F : 0.76F));
                stroke(card, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
                if (selected)
                    fill({card.x, card.y, 6.0F * s, card.height}, colors.accent);
                text(entries[index].label, card.x + 20.0F * s, card.y + 19.0F * s, 19.0F,
                     colors.text, TextAlign::left, selected);
                text(entries[index].badge, card.x + 20.0F * s, card.y + 49.0F * s, 9.0F,
                     selected ? colors.accent : colors.secondary);
                hits_.push_back({card, HitKind::menu_item, index});
            }
        }
        else
        {
            const std::size_t back_index = 2U;
            const bool selected = menu_.selection() == back_index;
            Rect back{x, utility_top, 230.0F * s, utility_height};
            fill(back, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.76F));
            stroke(back, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({back.x, back.y, 6.0F * s, back.height}, colors.accent);
            text("BACK", back.x + 20.0F * s, back.y + 25.0F * s, 20.0F, colors.text,
                 TextAlign::left, selected);
            hits_.push_back({back, HitKind::menu_item, back_index});

            Rect status{back.x + back.width + 14.0F * s, utility_top,
                        content_width - back.width - 14.0F * s, utility_height};
            fill(status, with_alpha(colors.panel, 0.62F));
            stroke(status, colors.line);
            text("STEAM STATUS", status.x + 20.0F * s, status.y + 15.0F * s, 9.0F,
                 colors.secondary);
            const std::string identity = steam_identity_verified_ && !online_persona_name_.empty()
                                             ? std::format("READY  /  {}", online_persona_name_)
                                         : join_browser_state_ == JoinBrowserState::searching
                                             ? "CONNECTING TO STEAM"
                                             : "STEAM WILL CONNECT WHEN THE FRONTEND STARTS";
            text(identity, status.x + 20.0F * s, status.y + 39.0F * s, 15.0F,
                 steam_identity_verified_ ? colors.text : colors.secondary);
        }
    }

    void draw_world_picker()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        if (entries.size() < 2U)
            return;
        constexpr std::size_t page_size = 6U;
        constexpr std::size_t columns = 3U;
        const std::size_t back_index = entries.size() - 1U;
        const std::size_t content_count = back_index;
        const std::size_t selected_content =
            menu_.selection() == back_index ? content_count - 1U : menu_.selection();
        const std::size_t page = selected_content / page_size;
        const std::size_t first = page * page_size;
        const std::size_t last = std::min(content_count, first + page_size);
        const std::size_t pages = (content_count + page_size - 1U) / page_size;
        const float slide = (1.0F - smoothstep(transition_)) * -34.0F * s;
        const float x = metrics_.safe.x + 36.0F * s + slide;
        const float top = metrics_.safe.y + 174.0F * s;
        const float content_width = metrics_.safe.width - 72.0F * s;
        text("YOUR WORLDS", x, top, 14.0F, colors.accent);
        text("THUMBNAILS ARE CAPTURED WHEN YOU PAUSE BEFORE LEAVING.", x, top + 25.0F * s, 11.0F,
             colors.secondary);
        text(std::format("PAGE {} / {}", page + 1U, pages), x + content_width, top + 4.0F * s,
             10.0F, colors.secondary, TextAlign::right);

        const float gap = 13.0F * s;
        const float card_width = (content_width - gap * 2.0F) / 3.0F;
        const float card_height = 218.0F * s;
        const float cards_top = top + 54.0F * s;
        const auto clipped = [](std::string_view value, std::size_t maximum) {
            if (value.size() <= maximum)
                return std::string{value};
            return std::string{value.substr(0U, maximum - 3U)} + "...";
        };
        for (std::size_t index = first; index < last; ++index)
        {
            const std::size_t offset = index - first;
            const std::size_t column = offset % columns;
            const std::size_t row = offset / columns;
            const float focus = index < focus_.size() ? focus_[index] : 0.0F;
            Rect card{x + static_cast<float>(column) * (card_width + gap),
                      cards_top + static_cast<float>(row) * (card_height + gap) - focus * 3.0F * s,
                      card_width, card_height};
            const bool selected = menu_.selection() == index;
            fill(card, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.86F));
            stroke(card, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            Rect preview{card.x + 2.0F * s, card.y + 2.0F * s, card.width - 4.0F * s, 118.0F * s};
            if (index == 0U)
            {
                fill(preview, {8U, 15U, 15U, 255U});
                fill({preview.x, preview.y + preview.height * 0.58F, preview.width,
                      preview.height * 0.42F},
                     {18U, 39U, 29U, 255U});
                fill({preview.x + preview.width * 0.60F, preview.y + preview.height * 0.27F,
                      4.0F * s, preview.height * 0.31F},
                     colors.accent);
                text("+", preview.x + preview.width * 0.5F, preview.y + 25.0F * s, 46.0F,
                     colors.accent, TextAlign::center, true);
            }
            else
            {
                const WorldSlot &slot = world_slots_[index - 1U];
                if (slot.preview != nullptr)
                {
                    SDL_FRect destination{preview.x, preview.y, preview.width, preview.height};
                    SDL_RenderTexture(renderer_, slot.preview.get(), nullptr, &destination);
                    fill({preview.x, preview.y + preview.height - 28.0F * s, preview.width,
                          28.0F * s},
                         {2U, 4U, 5U, 170U});
                }
                else
                {
                    fill(preview, {8U, 12U, 13U, 255U});
                    fill({preview.x, preview.y + preview.height * 0.64F, preview.width,
                          preview.height * 0.36F},
                         {17U, 31U, 24U, 255U});
                    text("NO PREVIEW YET", preview.x + preview.width * 0.5F,
                         preview.y + preview.height * 0.47F, 10.0F, colors.secondary,
                         TextAlign::center);
                }
            }
            if (selected)
                fill({card.x, card.y, 6.0F * s, card.height}, colors.accent);
            text(clipped(entries[index].label, 28U), card.x + 17.0F * s, card.y + 134.0F * s, 18.0F,
                 colors.text, TextAlign::left, selected);
            if (index == 0U)
            {
                text("GENERATE A FRESH FRONTIER", card.x + 17.0F * s, card.y + 171.0F * s, 10.0F,
                     colors.secondary);
                text("NEW RANDOM SEED", card.x + 17.0F * s, card.y + 193.0F * s, 9.0F,
                     selected ? colors.accent : colors.secondary);
            }
            else
            {
                const WorldSlot &slot = world_slots_[index - 1U];
                text(std::format("{}  /  {}", clipped(slot.owner, 18U), slot.mode),
                     card.x + 17.0F * s, card.y + 166.0F * s, 10.0F, colors.secondary);
                text(std::format("{}  /  SEED {}", slot.saved_at, slot.seed), card.x + 17.0F * s,
                     card.y + 190.0F * s, 9.0F, selected ? colors.accent : colors.secondary);
            }
            hits_.push_back({card, HitKind::menu_item, index});
        }

        const float back_top = cards_top + card_height * 2.0F + gap + 14.0F * s;
        Rect back{x, back_top, 220.0F * s, 55.0F * s};
        const bool back_selected = menu_.selection() == back_index;
        fill(back, with_alpha(back_selected ? colors.panel_selected : colors.panel,
                              back_selected ? 0.98F : 0.78F));
        stroke(back, back_selected ? colors.accent : colors.line, back_selected ? 2.0F * s : 1.0F);
        if (back_selected)
            fill({back.x, back.y, 6.0F * s, back.height}, colors.accent);
        text("BACK", back.x + 20.0F * s, back.y + 16.0F * s, 18.0F, colors.text, TextAlign::left,
             back_selected);
        hits_.push_back({back, HitKind::menu_item, back_index});
        text("LEFT / RIGHT  CHOOSE    UP / DOWN  ROW    ENTER  OPEN", x + content_width,
             back.y + 19.0F * s, 10.0F, colors.secondary, TextAlign::right);
    }

    void draw_host_setup_hub()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        if (entries.size() != 6U)
            return;
        const float slide = (1.0F - smoothstep(transition_)) * -34.0F * s;
        const float x = metrics_.safe.x + 36.0F * s + slide;
        const float top = metrics_.safe.y + 166.0F * s;
        const float content_width = metrics_.safe.width - 72.0F * s;
        text("SERVER SETTINGS", x, top, 15.0F, colors.accent);
        text("Build the lobby in four clear sections, then choose the world you want to host.", x,
             top + 29.0F * s, 11.0F, colors.secondary);
        text(steam_identity_verified_ && !online_persona_name_.empty()
                 ? std::format("STEAM READY  /  {}", online_persona_name_)
                 : std::string{"STEAM SIGN-IN IN PROGRESS"},
             x + content_width, top + 4.0F * s, 10.0F, colors.secondary, TextAlign::right);

        const float gap = 14.0F * s;
        const float card_width = (content_width - gap) * 0.5F;
        const float card_height = 124.0F * s;
        const float grid_top = top + 65.0F * s;
        for (std::size_t index{}; index < 4U; ++index)
        {
            const float column = static_cast<float>(index % 2U);
            const float row = static_cast<float>(index / 2U);
            Rect card{x + column * (card_width + gap), grid_top + row * (card_height + gap),
                      card_width, card_height};
            const bool selected = menu_.selection() == index;
            fill(card, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.80F));
            stroke(card, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({card.x, card.y, 6.0F * s, card.height}, colors.accent);
            text(entries[index].label, card.x + 22.0F * s, card.y + 17.0F * s, 20.0F, colors.text,
                 TextAlign::left, selected);
            text(entries[index].description, card.x + 22.0F * s, card.y + 51.0F * s, 9.0F,
                 colors.secondary);
            const std::string summary =
                index == 0U   ? std::format("{}  /  {}  /  {} SLOTS", host_settings_.server_name,
                                            net::protocol_label(host_settings_.protocol),
                                            host_settings_.maximum_players)
                : index == 1U ? std::format("{}  /  PASSWORD {}",
                                            net::visibility_label(host_settings_.visibility),
                                            host_settings_.password.empty() ? "OPEN" : "SET")
                : index == 2U
                    ? std::format("PVP {}  /  ANTI-CHEAT {}  /  SAFE JOIN {}",
                                  net::pvp_label(host_settings_.player_versus_player),
                                  host_settings_.anti_cheat ? "STRICT" : "OFF",
                                  host_settings_.dedicated_join_safety ? "ON" : "SOURCE EXACT")
                    : host_setting_value(ui::MenuCommand::review_host_mods);
            text(summary, card.x + 22.0F * s, card.y + 91.0F * s, 10.0F,
                 selected ? colors.accent : colors.secondary);
            hits_.push_back({card, HitKind::menu_item, index});
        }

        const float action_top = grid_top + 2.0F * (card_height + gap) + 8.0F * s;
        Rect next{x, action_top, content_width * 0.72F, 62.0F * s};
        Rect back{next.x + next.width + gap, action_top, content_width - next.width - gap,
                  next.height};
        for (const auto [rect, index] :
             std::array<std::pair<Rect, std::size_t>, 2>{{{next, 4U}, {back, 5U}}})
        {
            const bool selected = menu_.selection() == index;
            fill(rect, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.80F));
            stroke(rect, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({rect.x, rect.y, 6.0F * s, rect.height}, colors.accent);
            text(entries[index].label, rect.x + 22.0F * s, rect.y + 18.0F * s, 20.0F, colors.text,
                 TextAlign::left, selected);
            text(entries[index].badge, rect.x + rect.width - 20.0F * s, rect.y + 23.0F * s, 9.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::right);
            hits_.push_back({rect, HitKind::menu_item, index});
        }
        text("ARROWS  NAVIGATE    ENTER  OPEN", x + content_width, action_top + 79.0F * s, 10.0F,
             colors.secondary, TextAlign::right);
    }

    void draw_host_settings_panel()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        const ui::MenuScreen screen = menu_.screen();
        const std::string_view title = screen == ui::MenuScreen::host_details  ? "SERVER DETAILS"
                                       : screen == ui::MenuScreen::host_access ? "ACCESS & PRIVACY"
                                                                               : "RULES & SAFETY";
        const std::string_view subtitle = screen == ui::MenuScreen::host_details
                                              ? "Identity, compatibility, and total capacity."
                                          : screen == ui::MenuScreen::host_access
                                              ? "Discovery and admission controls for this lobby."
                                              : "World authority and player protection controls.";
        const float slide = (1.0F - smoothstep(transition_)) * -34.0F * s;
        const float x = metrics_.safe.x + 36.0F * s + slide;
        const float top = metrics_.safe.y + 166.0F * s;
        const float content_width = metrics_.safe.width - 72.0F * s;
        text("SERVER SETTINGS  /  " + std::string{title}, x, top, 15.0F, colors.accent);
        text(subtitle, x, top + 30.0F * s, 11.0F, colors.secondary);

        const float panel_top = top + 70.0F * s;
        const float gap = 18.0F * s;
        const float left_width = content_width * 0.62F;
        const float right_x = x + left_width + gap;
        const float right_width = content_width - left_width - gap;
        const float row_height = 72.0F * s;
        const float row_gap = 8.0F * s;
        for (std::size_t index{}; index < entries.size(); ++index)
        {
            Rect row{x, panel_top + static_cast<float>(index) * (row_height + row_gap), left_width,
                     row_height};
            const bool selected = menu_.selection() == index;
            fill(row, with_alpha(selected ? colors.panel_selected : colors.panel,
                                 selected ? 0.98F : 0.80F));
            stroke(row, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({row.x, row.y, 6.0F * s, row.height}, colors.accent);
            text(entries[index].label, row.x + 21.0F * s, row.y + 11.0F * s, 17.0F, colors.text,
                 TextAlign::left, selected);
            text(entries[index].description, row.x + 21.0F * s, row.y + 41.0F * s, 9.0F,
                 colors.secondary);
            const std::string current = host_setting_value(entries[index].command);
            if (!current.empty())
                text(current, row.x + row.width - 19.0F * s, row.y + 24.0F * s, 11.0F,
                     selected ? colors.accent : colors.secondary, TextAlign::right);
            hits_.push_back({row, HitKind::menu_item, index});
        }

        Rect summary{right_x, panel_top, right_width,
                     static_cast<float>(entries.size()) * (row_height + row_gap) - row_gap};
        fill(summary, with_alpha(colors.panel, 0.76F));
        stroke(summary, colors.line);
        text("CURRENT SESSION", summary.x + 24.0F * s, summary.y + 24.0F * s, 10.0F, colors.accent);
        text(host_settings_.server_name, summary.x + 24.0F * s, summary.y + 56.0F * s, 24.0F,
             colors.text, TextAlign::left, true);
        fill({summary.x + 24.0F * s, summary.y + 101.0F * s, summary.width - 48.0F * s, 1.0F},
             colors.line);
        text(std::format("{}  /  {} PLAYERS", net::protocol_label(host_settings_.protocol),
                         host_settings_.maximum_players),
             summary.x + 24.0F * s, summary.y + 129.0F * s, 14.0F, colors.text);
        text(net::visibility_label(host_settings_.visibility), summary.x + 24.0F * s,
             summary.y + 165.0F * s, 12.0F, colors.secondary);
        text(std::format("PASSWORD {}", host_settings_.password.empty() ? "OPEN" : "SET"),
             summary.x + 24.0F * s, summary.y + 194.0F * s, 12.0F, colors.secondary);
        text(std::format("PVP {}", net::pvp_label(host_settings_.player_versus_player)),
             summary.x + 24.0F * s, summary.y + 223.0F * s, 12.0F, colors.secondary);
        text(std::format("ANTI-CHEAT {}  /  SAFE JOIN {}",
                         host_settings_.anti_cheat ? "STRICT" : "OFF",
                         host_settings_.dedicated_join_safety ? "ON" : "SOURCE EXACT"),
             summary.x + 24.0F * s, summary.y + 252.0F * s, 10.0F, colors.secondary);
        text("LEFT / RIGHT  CHANGE    ENTER  EDIT OR SELECT", x + content_width,
             summary.y + summary.height + 22.0F * s, 9.0F, colors.secondary, TextAlign::right);
    }

    void draw_server_browser()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        const std::size_t session_rows = join_row_sessions_.size();
        if (entries.size() < session_rows + 4U)
            return;
        const std::size_t refresh_index = session_rows;
        const std::size_t filter_index = session_rows + 1U;
        const std::size_t direct_index = session_rows + 2U;
        const std::size_t back_index = session_rows + 3U;
        const float slide = (1.0F - smoothstep(transition_)) * -34.0F * s;
        const float x = metrics_.safe.x + 36.0F * s + slide;
        const float top = metrics_.safe.y + 170.0F * s;
        const float content_width = metrics_.safe.width - 72.0F * s;
        text("LIVE SERVERS", x, top, 15.0F, colors.accent);
        fill({x, top + 31.0F * s, 8.0F * s, 8.0F * s},
             join_browser_state_ == JoinBrowserState::failed ? colors.accent
             : join_browser_state_ == JoinBrowserState::searching
                 ? SDL_Color{238U, 176U, 54U, 255U}
                 : SDL_Color{75U, 201U, 120U, 255U});
        text(join_browser_status_, x + 18.0F * s, top + 27.0F * s, 11.0F, colors.secondary);
        text(steam_identity_verified_ && !online_persona_name_.empty()
                 ? std::format("SIGNED IN AS {}", online_persona_name_)
                 : std::string{"STEAM IDENTITY PENDING"},
             x + content_width, top + 4.0F * s, 10.0F, colors.secondary, TextAlign::right);

        const float toolbar_top = top + 55.0F * s;
        const float toolbar_gap = 10.0F * s;
        const std::array<float, 4> toolbar_widths{170.0F * s, 180.0F * s, 210.0F * s, 120.0F * s};
        const std::array<std::size_t, 4> toolbar_indices{refresh_index, filter_index, direct_index,
                                                         back_index};
        float toolbar_x = x;
        for (std::size_t button{}; button < toolbar_indices.size(); ++button)
        {
            const std::size_t index = toolbar_indices[button];
            Rect rect{toolbar_x, toolbar_top, toolbar_widths[button], 48.0F * s};
            const bool selected = menu_.selection() == index;
            fill(rect, with_alpha(selected ? colors.panel_selected : colors.panel,
                                  selected ? 0.98F : 0.80F));
            stroke(rect, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({rect.x, rect.y, 5.0F * s, rect.height}, colors.accent);
            text(entries[index].label, rect.x + 15.0F * s, rect.y + 13.0F * s, 15.0F, colors.text,
                 TextAlign::left, selected);
            std::string badge{entries[index].badge};
            if (index == refresh_index)
                badge = std::format("AUTO {:02}", static_cast<int>(std::ceil(join_refresh_timer_)));
            text(badge, rect.x + rect.width - 13.0F * s, rect.y + 17.0F * s, 8.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::right);
            hits_.push_back({rect, HitKind::menu_item, index});
            toolbar_x += rect.width + toolbar_gap;
        }

        const float list_top = toolbar_top + 66.0F * s;
        const float panel_bottom = metrics_.safe.y + metrics_.safe.height - 72.0F * s;
        const float left_width = content_width * 0.61F;
        const float panel_gap = 18.0F * s;
        const float right_x = x + left_width + panel_gap;
        const float right_width = content_width - left_width - panel_gap;
        const float row_height = 72.0F * s;
        const float row_gap = 8.0F * s;
        const std::size_t visible_rows = std::max<std::size_t>(
            1U, static_cast<std::size_t>((panel_bottom - list_top) / (row_height + row_gap)));
        std::size_t first{};
        if (menu_.selection() < session_rows && menu_.selection() >= visible_rows)
            first = menu_.selection() - visible_rows + 1U;
        if (session_rows > visible_rows)
            first = std::min(first, session_rows - visible_rows);

        fill({x, list_top, left_width, panel_bottom - list_top}, with_alpha(colors.panel, 0.42F));
        stroke({x, list_top, left_width, panel_bottom - list_top}, colors.line);
        if (session_rows == 0U)
        {
            text(join_browser_state_ == JoinBrowserState::searching ? "SEARCHING STEAM..."
                                                                    : "NO LIVE SERVERS ONLINE",
                 x + left_width * 0.5F, list_top + 92.0F * s, 22.0F, colors.text, TextAlign::center,
                 true);
            text("USE REFRESH, CHANGE THE FILTER, OR TRY DIRECT CONNECT.", x + left_width * 0.5F,
                 list_top + 135.0F * s, 10.0F, colors.secondary, TextAlign::center);
        }
        for (std::size_t index = first; index < session_rows && index < first + visible_rows;
             ++index)
        {
            const std::size_t visible_index = index - first;
            Rect row{x + 10.0F * s,
                     list_top + 10.0F * s +
                         static_cast<float>(visible_index) * (row_height + row_gap),
                     left_width - 20.0F * s, row_height};
            const bool selected = menu_.selection() == index;
            fill(row, with_alpha(selected ? colors.panel_selected : colors.panel,
                                 selected ? 0.98F : 0.78F));
            stroke(row, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
                fill({row.x, row.y, 6.0F * s, row.height}, colors.accent);
            text(join_labels_[index], row.x + 20.0F * s, row.y + 11.0F * s, 19.0F, colors.text,
                 TextAlign::left, selected);
            text(join_descriptions_[index], row.x + 20.0F * s, row.y + 42.0F * s, 9.0F,
                 colors.secondary);
            text(join_badges_[index], row.x + row.width - 16.0F * s, row.y + 18.0F * s, 9.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::right);
            hits_.push_back({row, HitKind::menu_item, index});
        }
        if (session_rows > visible_rows)
        {
            const float track_height = panel_bottom - list_top - 20.0F * s;
            const float thumb_height =
                track_height * static_cast<float>(visible_rows) / static_cast<float>(session_rows);
            const float travel = track_height - thumb_height;
            const float progress =
                static_cast<float>(first) / static_cast<float>(session_rows - visible_rows);
            fill({x + left_width - 6.0F * s, list_top + 10.0F * s, 2.0F * s, track_height},
                 colors.line);
            fill({x + left_width - 8.0F * s, list_top + 10.0F * s + travel * progress, 6.0F * s,
                  thumb_height},
                 colors.accent);
        }

        Rect details{right_x, list_top, right_width, panel_bottom - list_top};
        fill(details, with_alpha(colors.panel, 0.76F));
        stroke(details, colors.line);
        const bool session_selected = menu_.selection() < session_rows;
        text(session_selected ? "SERVER DETAILS" : "DISCOVERY", details.x + 24.0F * s,
             details.y + 24.0F * s, 11.0F, colors.accent);
        if (session_selected)
        {
            const std::size_t session_index = join_row_sessions_[menu_.selection()];
            const net::DiscoveredSession &session = join_browser_.sessions()[session_index];
            const net::JoinAssessment assessment = browser_join_assessment(session_index);
            text(session.server_name, details.x + 24.0F * s, details.y + 54.0F * s, 27.0F,
                 colors.text, TextAlign::left, true);
            text(std::format("HOSTED BY {}", session.host_name), details.x + 24.0F * s,
                 details.y + 99.0F * s, 11.0F, colors.secondary);
            fill({details.x + 24.0F * s, details.y + 126.0F * s, details.width - 48.0F * s, 1.0F},
                 colors.line);
            text("PLAYERS", details.x + 24.0F * s, details.y + 151.0F * s, 9.0F, colors.secondary);
            text(std::format("{} / {}", session.current_players, session.maximum_players),
                 details.x + 24.0F * s, details.y + 172.0F * s, 20.0F, colors.text);
            text("COMPATIBILITY", details.x + details.width * 0.52F, details.y + 151.0F * s, 9.0F,
                 colors.secondary);
            text(net::join_compatibility_label(assessment.status),
                 details.x + details.width * 0.52F, details.y + 172.0F * s, 16.0F,
                 assessment.joinable() ? colors.text : colors.accent);
            text(std::format("{}  /  {}", net::protocol_label(assessment.protocol),
                             net::discovery_source_label(session.source)),
                 details.x + 24.0F * s, details.y + 222.0F * s, 11.0F, colors.secondary);
            text(session.password_required ? "PASSWORD REQUIRED" : "OPEN SESSION",
                 details.x + 24.0F * s, details.y + 250.0F * s, 11.0F,
                 session.password_required ? colors.accent : colors.secondary);
            text("ENTER  VIEW & JOIN", details.x + 24.0F * s,
                 details.y + details.height - 36.0F * s, 10.0F, colors.accent);
        }
        else
        {
            text(join_browser_state_ == JoinBrowserState::searching ? "REFRESHING"
                                                                    : "LIVE QUERY READY",
                 details.x + 24.0F * s, details.y + 58.0F * s, 25.0F, colors.text, TextAlign::left,
                 true);
            text(std::format("{} SERVER{} IN THIS FILTER", session_rows,
                             session_rows == 1U ? "" : "S"),
                 details.x + 24.0F * s, details.y + 108.0F * s, 13.0F, colors.secondary);
            text("FRIENDS + PUBLIC STEAM LOBBIES", details.x + 24.0F * s, details.y + 142.0F * s,
                 10.0F, colors.secondary);
        }
    }

    void draw_main_menu()
    {
        if (menu_.screen() == ui::MenuScreen::main || menu_.screen() == ui::MenuScreen::multiplayer)
        {
            draw_start_hub();
            return;
        }
        if (menu_.screen() == ui::MenuScreen::world_select)
        {
            draw_world_picker();
            return;
        }
        if (menu_.screen() == ui::MenuScreen::host_setup)
        {
            draw_host_setup_hub();
            return;
        }
        if (menu_.screen() == ui::MenuScreen::host_details ||
            menu_.screen() == ui::MenuScreen::host_access ||
            menu_.screen() == ui::MenuScreen::host_gameplay)
        {
            draw_host_settings_panel();
            return;
        }
        if (menu_.screen() == ui::MenuScreen::join_browser)
        {
            draw_server_browser();
            return;
        }
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const auto &entries = menu_.entries();
        const float x = metrics_.safe.x + 36.0F * s;
        const bool join_browser_screen = menu_.screen() == ui::MenuScreen::join_browser;
        const float y = metrics_.safe.y + (join_browser_screen ? 230.0F : 205.0F) * s;
        const float width = std::min(700.0F * s, metrics_.safe.width * 0.53F);
        const float row_height = 62.0F * s;
        const float gap = 7.0F * s;
        const float slide = (1.0F - smoothstep(transition_)) * -42.0F * s;

        const std::string_view menu_title =
            menu_.screen() == ui::MenuScreen::host_setup     ? "HOST SESSION SETUP"
            : menu_.screen() == ui::MenuScreen::join_browser ? "JOIN ONLINE"
            : menu_.screen() == ui::MenuScreen::join_details ? "SESSION DETAILS"
            : menu_.screen() == ui::MenuScreen::game_mode    ? "CHOOSE A GAME MODE"
            : menu_.screen() == ui::MenuScreen::difficulty   ? "CHOOSE A DIFFICULTY LEVEL"
            : menu_.screen() == ui::MenuScreen::world_select ? "CHOOSE A WORLD"
                                                             : "MAIN MENU";
        text(menu_title, x + slide, y - 44.0F * s, 17.0F, colors.accent);
        if (join_browser_screen)
            text(join_browser_status_, x + slide, y - 20.0F * s, 11.0F,
                 join_browser_state_ == JoinBrowserState::failed ? colors.accent
                                                                 : colors.secondary);
        // The world list can hold any number of saves: rows window around the
        // keyboard selection so the menu never runs off the safe area.
        const float safe_bottom = metrics_.safe.y + metrics_.safe.height - 34.0F * s;
        const std::size_t visible_rows = std::max<std::size_t>(
            3U, static_cast<std::size_t>(std::max(1.0F, (safe_bottom - y) / (row_height + gap))));
        std::size_t first_row = 0U;
        if (entries.size() > visible_rows && menu_.selection() >= visible_rows)
            first_row =
                std::min(menu_.selection() - visible_rows + 1U, entries.size() - visible_rows);
        const std::size_t last_row = std::min(entries.size(), first_row + visible_rows);
        if (first_row > 0U)
            text("^  MORE ABOVE", x + slide, y - 20.0F * s, 10.0F, colors.secondary);
        if (last_row < entries.size())
            text("v  MORE BELOW", x + slide,
                 y + static_cast<float>(visible_rows) * (row_height + gap) + 2.0F * s, 10.0F,
                 colors.secondary);
        for (std::size_t index = first_row; index < last_row; ++index)
        {
            const float focus = index < focus_.size() ? focus_[index] : 0.0F;
            const bool action_group_visible = join_browser_screen &&
                                              first_row <= join_row_sessions_.size() &&
                                              join_row_sessions_.size() < last_row;
            const bool action_row = action_group_visible && index >= join_row_sessions_.size();
            const float action_gap = action_row ? 20.0F * s : 0.0F;
            Rect row{x + slide + focus * 13.0F * s,
                     y + static_cast<float>(index - first_row) * (row_height + gap) + action_gap,
                     width, row_height};
            if (action_group_visible && index == join_row_sessions_.size())
                text("BROWSER ACTIONS", row.x, row.y - 16.0F * s, 10.0F, colors.secondary);
            const bool selected = index == menu_.selection();
            const SDL_Color background = selected ? colors.panel_selected : colors.panel;
            fill(row, with_alpha(background, selected ? 0.97F : 0.76F));
            stroke(row, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
            {
                fill({row.x, row.y, 6.0F * s, row.height}, colors.accent);
            }
            const SDL_Color label_color =
                entries[index].enabled ? colors.text : with_alpha(colors.secondary, 0.46F);
            text(entries[index].label, row.x + 24.0F * s, row.y + 13.0F * s, 25.0F, label_color);
            std::string dynamic_value;
            if (menu_.screen() == ui::MenuScreen::join_details &&
                entries[index].command == ui::MenuCommand::edit_join_password)
                dynamic_value = join_password_.empty() ? "NOT SET" : "SET";
            const std::string_view badge =
                dynamic_value.empty() ? entries[index].badge : std::string_view{dynamic_value};
            if (!badge.empty())
            {
                text(badge, row.x + row.width - 20.0F * s, row.y + 18.0F * s, 12.0F,
                     selected ? colors.accent : colors.secondary, TextAlign::right);
            }
            if (entries[index].enabled)
            {
                hits_.push_back({row, HitKind::menu_item, index});
            }
        }

        if (!entries.empty() && menu_.selection() < entries.size())
        {
            const float detail_x = x + width + 62.0F * s;
            const float detail_y = y + 76.0F * s;
            const float detail_width =
                std::max(300.0F * s, metrics_.safe.x + metrics_.safe.width - detail_x - 28.0F * s);
            const net::DiscoveredSession *detail_session = nullptr;
            std::optional<std::size_t> detail_session_index;
            if (menu_.screen() == ui::MenuScreen::join_browser &&
                menu_.selection() < join_row_sessions_.size())
            {
                detail_session_index = join_row_sessions_[menu_.selection()];
                detail_session = &join_browser_.sessions()[*detail_session_index];
            }
            else if (menu_.screen() == ui::MenuScreen::join_details)
            {
                detail_session = selected_join_session();
                detail_session_index = selected_join_session_;
            }
            text("SELECTED", detail_x, detail_y, 13.0F, colors.accent);
            text(detail_session != nullptr ? std::string_view{detail_session->server_name}
                                           : entries[menu_.selection()].label,
                 detail_x, detail_y + 30.0F * s, 34.0F,
                 entries[menu_.selection()].enabled ? colors.text : colors.secondary);
            fill({detail_x, detail_y + 82.0F * s, detail_width, 1.0F}, colors.line);
            text(detail_session != nullptr
                     ? std::string{std::format("HOST {}  •  {}", detail_session->host_name,
                                               detail_session->server_message)}
                     : std::string{entries[menu_.selection()].description},
                 detail_x, detail_y + 104.0F * s, 18.0F, colors.secondary);

            if (menu_.screen() == ui::MenuScreen::host_setup)
            {
                const std::string current = host_setting_value(entries[menu_.selection()].command);
                if (!current.empty())
                    text("CURRENT  " + current, detail_x, detail_y + 150.0F * s, 14.0F,
                         colors.accent);
            }

            fill({detail_x, detail_y + 190.0F * s, detail_width, 146.0F * s}, colors.panel);
            stroke({detail_x, detail_y + 190.0F * s, detail_width, 146.0F * s}, colors.line);
            text(menu_.screen() == ui::MenuScreen::host_setup     ? "SESSION CONTRACT"
                 : detail_session != nullptr                      ? "DISCOVERED CONTRACT"
                 : menu_.screen() == ui::MenuScreen::join_browser ? "DISCOVERY STATUS"
                                                                  : "FRONTEND STATUS",
                 detail_x + 22.0F * s, detail_y + 212.0F * s, 13.0F, colors.accent);
            if (menu_.screen() == ui::MenuScreen::host_setup)
            {
                text(std::format("{}  •  {} SLOTS  •  {}",
                                 net::protocol_label(host_settings_.protocol),
                                 host_settings_.maximum_players,
                                 net::visibility_label(host_settings_.visibility)),
                     detail_x + 22.0F * s, detail_y + 246.0F * s, 17.0F, colors.text);
                text(std::format("PASSWORD {}  •  MODS {}  •  PACKET BOUNDS ALWAYS ON",
                                 host_settings_.password.empty() ? "OPEN" : "SET",
                                 host_settings_.mod_manifest.mods.empty() ? "VANILLA" : "SYNCED"),
                     detail_x + 22.0F * s, detail_y + 277.0F * s, 13.0F, colors.secondary);
            }
            else if (detail_session != nullptr && detail_session_index.has_value())
            {
                const net::JoinAssessment assessment =
                    browser_join_assessment(*detail_session_index);
                text(std::format("{}  •  {}  •  {}/{} PLAYERS",
                                 net::protocol_label(assessment.protocol),
                                 net::discovery_source_label(detail_session->source),
                                 detail_session->current_players, detail_session->maximum_players),
                     detail_x + 22.0F * s, detail_y + 246.0F * s, 17.0F, colors.text);
                text(std::format("{}  •  PASSWORD {}  •  MODS {}",
                                 net::join_compatibility_label(assessment.status),
                                 detail_session->password_required ? "REQUIRED" : "OPEN",
                                 detail_session->required_manifest.has_value() ? "SYNCED"
                                                                               : "VANILLA"),
                     detail_x + 22.0F * s, detail_y + 277.0F * s, 13.0F,
                     assessment.joinable() ? colors.secondary : colors.accent);
            }
            else if (menu_.screen() == ui::MenuScreen::join_browser)
            {
                const std::string_view state =
                    join_browser_state_ == JoinBrowserState::searching    ? "CONNECTING TO STEAM"
                    : join_browser_state_ == JoinBrowserState::failed     ? "DISCOVERY FAILED"
                    : join_browser_state_ == JoinBrowserState::diagnostic ? "DIAGNOSTIC FIXTURES"
                    : join_browser_.visible_indices().empty()             ? "NO LIVE SERVERS ONLINE"
                                                                          : "LIVE SERVERS READY";
                text(state, detail_x + 22.0F * s, detail_y + 246.0F * s, 17.0F,
                     join_browser_state_ == JoinBrowserState::failed ? colors.accent : colors.text);
                text(join_browser_status_, detail_x + 22.0F * s, detail_y + 277.0F * s, 11.0F,
                     colors.secondary);
            }
            else
            {
                text("RESPONSIVE CANVAS", detail_x + 22.0F * s, detail_y + 246.0F * s, 17.0F,
                     colors.text);
                text("NATIVE-DENSITY TYPE  •  LIVE SETTINGS", detail_x + 22.0F * s,
                     detail_y + 277.0F * s, 14.0F, colors.secondary);
            }
            fill({detail_x + 22.0F * s, detail_y + 311.0F * s, detail_width - 44.0F * s, 3.0F * s},
                 colors.accent);
        }
    }

    Rect content_panel(std::string_view eyebrow, std::string_view title, std::string_view subtitle)
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        Rect panel{metrics_.safe.x + 34.0F * s, metrics_.safe.y + 188.0F * s,
                   metrics_.safe.width - 68.0F * s, metrics_.safe.height - 280.0F * s};
        fill(panel, colors.panel);
        stroke(panel, colors.line, 1.0F * s);
        fill({panel.x, panel.y, 7.0F * s, panel.height}, colors.accent);
        text(eyebrow, panel.x + 38.0F * s, panel.y + 28.0F * s, 13.0F, colors.accent);
        text(title, panel.x + 38.0F * s, panel.y + 57.0F * s, 36.0F, colors.text);
        text(subtitle, panel.x + 38.0F * s, panel.y + 108.0F * s, 16.0F, colors.secondary);
        fill({panel.x + 38.0F * s, panel.y + 150.0F * s, panel.width - 76.0F * s, 1.0F},
             colors.line);
        Rect back{panel.x + panel.width - 150.0F * s, panel.y + 24.0F * s, 112.0F * s, 42.0F * s};
        fill(back, colors.panel_selected);
        stroke(back, colors.line);
        text("BACK", back.x + back.width * 0.5F, back.y + 10.0F * s, 15.0F, colors.text,
             TextAlign::center);
        hits_.push_back({back, HitKind::back, 0U});
        return panel;
    }

    void draw_addons()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const Rect panel = content_panel("CONTENT", "ADD-ONS",
                                         "Detected content is read from the installed game; "
                                         "nothing proprietary is copied into this project.");
        float y = panel.y + 188.0F * s;
        if (addons_.empty())
        {
            text("NO ADD-ONS DETECTED", panel.x + 40.0F * s, y, 22.0F, colors.text);
            text("The loader will watch the configured CastleMiner Z installation and OpenMinerZ "
                 "add-on folder.",
                 panel.x + 40.0F * s, y + 42.0F * s, 15.0F, colors.secondary);
            return;
        }
        for (std::size_t index = 0U; index < addons_.size() && index < 7U; ++index)
        {
            Rect row{panel.x + 38.0F * s, y, panel.width - 76.0F * s, 58.0F * s};
            fill(row, index % 2U == 0U ? colors.panel_selected : colors.panel);
            text(addons_[index], row.x + 18.0F * s, row.y + 15.0F * s, 18.0F, colors.text);
            text("DETECTED", row.x + row.width - 18.0F * s, row.y + 18.0F * s, 12.0F, colors.accent,
                 TextAlign::right);
            y += 66.0F * s;
        }
    }

    void draw_credits()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const Rect panel = content_panel("PROJECT", "OPENMINERZ",
                                         "A clean C++20 reconstruction informed by observable "
                                         "behavior from a legally installed copy.");
        const float x = panel.x + 42.0F * s;
        float y = panel.y + 190.0F * s;
        const std::array<std::pair<std::string_view, std::string_view>, 5U> lines{{
            {"DIRECTION", "KikoTs and the OpenMinerZ community"},
            {"SOURCE GAME", "CastleMiner Z by DigitalDNA Games"},
            {"RUNTIME", "SDL3 • FreeType • CMake • C++20"},
            {"PRINCIPLE", "Deterministic simulation, native presentation, portable content import"},
            {"CURRENT SLICE", "Responsive animated frontend and advanced settings"},
        }};
        for (const auto &[label, value] : lines)
        {
            text(label, x, y, 12.0F, colors.accent);
            text(value, x + 190.0F * s, y - 4.0F * s, 18.0F, colors.text);
            y += 66.0F * s;
        }
    }

    void draw_settings()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        Rect panel{metrics_.safe.x + 24.0F * s, metrics_.safe.y + 172.0F * s,
                   metrics_.safe.width - 48.0F * s, metrics_.safe.height - 252.0F * s};
        fill(panel, colors.panel);
        stroke(panel, colors.line, 1.0F * s);

        const float sidebar_width = std::min(290.0F * s, panel.width * 0.24F);
        fill({panel.x, panel.y, sidebar_width, panel.height}, {4U, 7U, 8U, 215U});
        text("SETTINGS", panel.x + 26.0F * s, panel.y + 25.0F * s, 28.0F, colors.text);
        text("LIVE CONFIGURATION", panel.x + 27.0F * s, panel.y + 66.0F * s, 11.0F, colors.accent);

        constexpr std::array tabs{ui::SettingsTab::display,  ui::SettingsTab::graphics,
                                  ui::SettingsTab::gameplay, ui::SettingsTab::audio,
                                  ui::SettingsTab::controls, ui::SettingsTab::accessibility};
        float tab_y = panel.y + 112.0F * s;
        for (std::size_t index = 0U; index < tabs.size(); ++index)
        {
            Rect tab{panel.x + 12.0F * s, tab_y, sidebar_width - 24.0F * s, 51.0F * s};
            const bool selected = tabs[index] == menu_.settings_tab();
            if (selected)
            {
                fill(tab, colors.panel_selected);
                fill({tab.x, tab.y, 4.0F * s, tab.height}, colors.accent);
            }
            text(ui::tab_label(tabs[index]), tab.x + 20.0F * s, tab.y + 15.0F * s, 15.0F,
                 selected ? colors.text : colors.secondary);
            hits_.push_back({tab, HitKind::tab, index});
            tab_y += 58.0F * s;
        }

        Rect back{panel.x + 18.0F * s, panel.y + panel.height - 62.0F * s,
                  sidebar_width - 36.0F * s, 42.0F * s};
        fill(back, colors.panel_selected);
        stroke(back, colors.line);
        text("SAVE & BACK", back.x + back.width * 0.5F, back.y + 11.0F * s, 14.0F, colors.text,
             TextAlign::center);
        hits_.push_back({back, HitKind::back, 0U});

        const float content_x = panel.x + sidebar_width + 30.0F * s;
        const float content_width = panel.width - sidebar_width - 60.0F * s;
        text(ui::tab_label(menu_.settings_tab()), content_x, panel.y + 26.0F * s, 25.0F,
             colors.text);
        text("LEFT / RIGHT TO ADJUST  •  R TO RESTORE DEFAULTS", content_x, panel.y + 66.0F * s,
             11.0F, colors.secondary);
        fill({content_x, panel.y + 94.0F * s, content_width, 1.0F}, colors.line);

        const auto &rows = ui::rows_for_tab(menu_.settings_tab());
        const float row_height = 60.0F * s;
        const float row_gap = 7.0F * s;
        const float list_top = panel.y + 114.0F * s;
        const float list_bottom = panel.y + panel.height - 96.0F * s;
        const std::size_t visible = std::max<std::size_t>(
            1U, static_cast<std::size_t>((list_bottom - list_top) / (row_height + row_gap)));
        std::size_t first = 0U;
        if (menu_.selection() >= visible)
        {
            first = menu_.selection() - visible + 1U;
        }
        first = std::min(first, rows.size() > visible ? rows.size() - visible : 0U);

        float y = list_top;
        for (std::size_t index = first; index < rows.size() && index < first + visible; ++index)
        {
            const bool selected = index == menu_.selection();
            Rect row{content_x, y, content_width, row_height};
            fill(row, selected ? colors.panel_selected : with_alpha(colors.panel, 0.68F));
            stroke(row, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            if (selected)
            {
                fill({row.x, row.y, 4.0F * s, row.height}, colors.accent);
            }
            text(rows[index].label, row.x + 18.0F * s, row.y + 10.0F * s, 17.0F, colors.text);
            text(rows[index].description, row.x + 18.0F * s, row.y + 34.0F * s, 11.0F,
                 colors.secondary);

            const std::string value = uppercase(ui::setting_value(settings_, rows[index].id));
            const float value_x = row.x + row.width - 24.0F * s;
            text("‹", row.x + row.width * 0.63F, row.y + 15.0F * s, 18.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::center);
            text(value, value_x - 36.0F * s, row.y + 15.0F * s, 15.0F,
                 selected ? colors.text : colors.secondary, TextAlign::right);
            text("›", value_x, row.y + 15.0F * s, 18.0F,
                 selected ? colors.accent : colors.secondary, TextAlign::center);

            if (rows[index].kind == ui::SettingKind::slider)
            {
                const float bar_x = row.x + row.width * 0.64F;
                const float bar_y = row.y + row.height - 11.0F * s;
                const float bar_width = row.width * 0.30F;
                fill({bar_x, bar_y, bar_width, 3.0F * s}, colors.line);
                fill({bar_x, bar_y,
                      bar_width * ui::setting_normalized_value(settings_, rows[index].id),
                      3.0F * s},
                     colors.accent);
            }
            hits_.push_back({row, HitKind::setting_row, index});
            y += row_height + row_gap;
        }

        const auto &selected_row = rows[std::min(menu_.selection(), rows.size() - 1U)];
        text(selected_row.description, content_x, panel.y + panel.height - 63.0F * s, 13.0F,
             colors.secondary);
        const bool asset_restart_required = settings_.texture_style != loaded_asset_style_;
        text(asset_restart_required ? "RESTART REQUIRED" : "AUTO-SAVED", content_x + content_width,
             panel.y + panel.height - 63.0F * s, 11.0F, colors.accent, TextAlign::right);
    }

    void draw_exit_confirmation()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {0U, 0U, 0U, 178U});
        Rect box{static_cast<float>(metrics_.output_width) * 0.5F - 320.0F * s,
                 static_cast<float>(metrics_.output_height) * 0.5F - 170.0F * s, 640.0F * s,
                 340.0F * s};
        fill(box, colors.panel);
        stroke(box, colors.accent, 2.0F * s);
        text("EXIT OPENMINERZ?", box.x + 32.0F * s, box.y + 30.0F * s, 30.0F, colors.text);
        text("Your settings have already been saved.", box.x + 32.0F * s, box.y + 80.0F * s, 15.0F,
             colors.secondary);
        const auto &entries = menu_.entries();
        for (std::size_t index = 0U; index < entries.size(); ++index)
        {
            Rect row{box.x + 32.0F * s, box.y + 132.0F * s + static_cast<float>(index) * 76.0F * s,
                     box.width - 64.0F * s, 60.0F * s};
            const bool selected = index == menu_.selection();
            fill(row, selected ? colors.panel_selected : colors.panel);
            stroke(row, selected ? colors.accent : colors.line, selected ? 2.0F * s : 1.0F);
            text(entries[index].label, row.x + 20.0F * s, row.y + 16.0F * s, 18.0F,
                 selected ? colors.text : colors.secondary);
            hits_.push_back({row, HitKind::menu_item, index});
        }
    }

    void draw_death_overlay()
    {
        const float width = static_cast<float>(metrics_.output_width);
        const float height = static_cast<float>(metrics_.output_height);
        // A faint red wash while the body settles — the original leaves the
        // world visible and only prints the prompt.
        const float settle = std::min(1.0F, death_timer_ / 0.9F);
        fill({0.0F, 0.0F, width, height},
             {80U, 0U, 0U, static_cast<std::uint8_t>(30.0F + settle * 40.0F)});
        if (player_life_ == PlayerLife::dead)
        {
            // InGameHUD: "Click To Respawn", white with a black outline,
            // centered on the safe area.
            text("Click To Respawn", width * 0.5F, height * 0.48F, 26.0F, {255U, 255U, 255U, 255U},
                 TextAlign::center, true);
            if (game_mode_ == GameMode::endurance)
            {
                const int days = std::max(1, static_cast<int>(day_night_.day()));
                text(std::format("Distance Traveled: {} In {} {}", max_distance_, days,
                                 days == 1 ? "Day" : "Days"),
                     width * 0.5F, height * 0.55F, 14.0F, {255U, 255U, 255U, 230U},
                     TextAlign::center);
            }
            else
            {
                text(difficulty_ == GameDifficulty::hardcore
                         ? "HARDCORE: EVERYTHING YOU CARRIED IS GONE"
                         : "YOUR BACKPACK SPILLED WHERE YOU FELL",
                     width * 0.5F, height * 0.55F, 11.0F, {220U, 200U, 200U, 200U},
                     TextAlign::center);
            }
        }
    }

    void draw_pause_menu()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        fill({0.0F, 0.0F, static_cast<float>(metrics_.output_width),
              static_cast<float>(metrics_.output_height)},
             {0U, 0U, 0U, 174U});
        // The box sizes itself to the entry count (six rows since the teleport
        // and inventory entries joined) and clamps to the safe area.
        const auto &entries = menu_.entries();
        const float row_pitch = 58.0F * s;
        const float row_height = 50.0F * s;
        const float header_height = 112.0F * s;
        const float footer_height = 44.0F * s;
        float box_height =
            header_height + static_cast<float>(entries.size()) * row_pitch + footer_height;
        box_height = std::min(box_height, metrics_.safe.height * 0.94F);
        Rect box{static_cast<float>(metrics_.output_width) * 0.5F - 310.0F * s,
                 static_cast<float>(metrics_.output_height) * 0.5F - box_height * 0.5F, 620.0F * s,
                 box_height};
        fill(box, {8U, 11U, 12U, 248U});
        stroke(box, colors.accent, std::max(1.0F, 2.0F * s));
        fill({box.x, box.y, box.width, 6.0F * s}, colors.accent);
        text("GAME PAUSED", box.x + 34.0F * s, box.y + 27.0F * s, 27.0F, colors.text,
             TextAlign::left, true);
        text("THE WORLD IS FROZEN", box.x + 36.0F * s, box.y + 70.0F * s, 11.0F, colors.secondary);
        for (std::size_t index = 0U; index < entries.size(); ++index)
        {
            Rect row{box.x + 34.0F * s,
                     box.y + header_height + static_cast<float>(index) * row_pitch,
                     box.width - 68.0F * s, row_height};
            const bool selected = index == menu_.selection();
            fill(row, selected ? colors.panel_selected : SDL_Color{13U, 17U, 18U, 238U});
            stroke(row, selected ? colors.accent : colors.line,
                   selected ? std::max(1.0F, 2.0F * s) : 1.0F);
            if (selected)
                fill({row.x, row.y, 5.0F * s, row.height}, colors.accent);
            text(entries[index].label, row.x + 20.0F * s, row.y + 8.0F * s, 16.0F,
                 selected ? colors.text : colors.secondary, TextAlign::left, selected);
            text(entries[index].description, row.x + 20.0F * s, row.y + 31.0F * s, 9.0F,
                 colors.secondary);
            hits_.push_back({row, HitKind::menu_item, index});
        }
        text("ESC  RESUME", box.x + box.width - 34.0F * s, box.y + box.height - 27.0F * s, 10.0F,
             colors.secondary, TextAlign::right);
    }

    void draw_toast()
    {
        if (toast_time_ <= 0.0F)
        {
            return;
        }
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const float enter = std::min(1.0F, (4.2F - toast_time_) * 5.0F);
        const float leave = std::min(1.0F, toast_time_ * 4.0F);
        const float opacity = smoothstep(std::min(enter, leave));
        Rect box{metrics_.safe.x + metrics_.safe.width - 530.0F * s,
                 metrics_.safe.y + metrics_.safe.height - 154.0F * s + (1.0F - opacity) * 30.0F * s,
                 500.0F * s, 96.0F * s};
        fill(box, with_alpha(colors.panel, opacity));
        stroke(box, with_alpha(colors.accent, opacity), 2.0F * s);
        text(toast_title_, box.x + 20.0F * s, box.y + 17.0F * s, 15.0F,
             with_alpha(colors.text, opacity));
        text(toast_body_, box.x + 20.0F * s, box.y + 50.0F * s, 11.0F,
             with_alpha(colors.secondary, opacity));
    }

    void draw_chat_dialog()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const float width = std::min(metrics_.safe.width - 48.0F * s, 760.0F * s);
        const Rect box{metrics_.safe.x + 24.0F * s,
                       metrics_.safe.y + metrics_.safe.height - 112.0F * s, width, 72.0F * s};
        fill(box, {7U, 12U, 16U, 236U});
        stroke(box, colors.accent, 2.0F * s);
        text("NETWORK CHAT", box.x + 18.0F * s, box.y + 10.0F * s, 10.0F, colors.secondary);
        const std::string display = chat_draft_ + (std::fmod(elapsed_, 1.0F) < 0.55F ? "_" : "");
        text(display, box.x + 18.0F * s, box.y + 33.0F * s, 15.0F, colors.text);
        text("ENTER SENDS  /  ESC CANCELS", box.x + box.width - 16.0F * s, box.y + 12.0F * s, 9.0F,
             colors.secondary, TextAlign::right);
    }

    void draw_online_chat_feed()
    {
        if (online_chat_lines_.empty() || !online_gameplay_active())
            return;
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        std::vector<const OnlineChatLine *> visible;
        for (const OnlineChatLine &line : online_chat_lines_)
            if (chat_editing_ || elapsed_ - line.received_at < 13.0F)
                visible.push_back(&line);
        if (visible.empty())
            return;
        if (visible.size() > 6U)
            visible.erase(visible.begin(), visible.end() - 6);
        const float row_height = 25.0F * s;
        const float width = std::min(metrics_.safe.width - 48.0F * s, 820.0F * s);
        const float bottom = metrics_.safe.y + metrics_.safe.height -
                             (chat_editing_ ? 126.0F : 38.0F) * s;
        const Rect panel{metrics_.safe.x + 24.0F * s,
                         bottom - row_height * static_cast<float>(visible.size()) - 10.0F * s,
                         width, row_height * static_cast<float>(visible.size()) + 10.0F * s};
        fill(panel, {4U, 8U, 11U, static_cast<Uint8>(chat_editing_ ? 220U : 160U)});
        fill({panel.x, panel.y, 4.0F * s, panel.height}, colors.accent);
        for (std::size_t index = 0U; index < visible.size(); ++index)
        {
            const float age = elapsed_ - visible[index]->received_at;
            const float opacity = chat_editing_ ? 1.0F : std::clamp((13.0F - age) * 0.75F, 0.0F, 1.0F);
            text(visible[index]->message, panel.x + 17.0F * s,
                 panel.y + 8.0F * s + static_cast<float>(index) * row_height,
                 11.0F, with_alpha(colors.text, opacity));
        }
    }

    void draw_player_roster()
    {
        if (!online_gameplay_active())
            return;
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        struct RosterEntry final
        {
            std::uint8_t gid{};
            std::string name;
            std::string role;
            bool active{};
        };
        std::vector<RosterEntry> entries;
        const bool local_host = online_host_ != nullptr && online_host_->running();
        entries.push_back({online_local_player_gid(),
                           online_persona_name_.empty() ? std::string{"OpenMinerZ"}
                                                        : online_persona_name_,
                           local_host ? "HOST / YOU" : "YOU", true});
        for (const auto &[gid, remote] : online_remote_players_)
        {
            entries.push_back({gid,
                               remote.gamer_tag.empty()
                                   ? std::format("PLAYER {}", static_cast<int>(gid))
                                   : remote.gamer_tag,
                               gid == 0U ? "HOST" : "PLAYER",
                               remote.has_update && elapsed_ - remote.last_update <= 10.0F});
        }
        std::ranges::sort(entries, {}, &RosterEntry::gid);
        const float width = std::min(metrics_.safe.width - 60.0F * s, 610.0F * s);
        const float row_height = 43.0F * s;
        const float height = 74.0F * s + row_height * static_cast<float>(entries.size());
        const Rect panel{metrics_.safe.x + (metrics_.safe.width - width) * 0.5F,
                         metrics_.safe.y + 42.0F * s, width, height};
        fill(panel, {4U, 8U, 11U, 235U});
        stroke(panel, colors.accent, 2.0F * s);
        text("SESSION PLAYERS", panel.x + 22.0F * s, panel.y + 17.0F * s, 17.0F,
             colors.text, TextAlign::left, true);
        text(std::format("{} ONLINE  /  HOLD TAB", entries.size()),
             panel.x + panel.width - 22.0F * s, panel.y + 21.0F * s, 10.0F,
             colors.secondary, TextAlign::right);
        fill({panel.x + 20.0F * s, panel.y + 55.0F * s, panel.width - 40.0F * s, 1.0F * s},
             {86U, 101U, 104U, 180U});
        for (std::size_t index = 0U; index < entries.size(); ++index)
        {
            const RosterEntry &entry = entries[index];
            const float y = panel.y + 67.0F * s + static_cast<float>(index) * row_height;
            const SDL_Color status = entry.active ? SDL_Color{91U, 218U, 148U, 255U}
                                                  : SDL_Color{116U, 126U, 129U, 255U};
            fill({panel.x + 22.0F * s, y + 8.0F * s, 7.0F * s, 7.0F * s}, status);
            text(entry.name, panel.x + 44.0F * s, y, 14.0F,
                 entry.active ? colors.text : colors.secondary, TextAlign::left, true);
            text(std::format("{}  /  GID {}", entry.role, static_cast<int>(entry.gid)),
                 panel.x + panel.width - 22.0F * s, y + 3.0F * s, 10.0F,
                 entry.active ? colors.accent : colors.secondary, TextAlign::right);
        }
    }

    void draw_subtitle()
    {
        if (!settings_.subtitles || subtitle_time_ <= 0.0F || subtitle_text_.empty())
            return;
        const float s = metrics_.scale;
        const float size = 14.0F * settings_.subtitle_size;
        const float width = std::min(metrics_.safe.width * 0.72F, 620.0F * s);
        const Rect box{metrics_.safe.x + (metrics_.safe.width - width) * 0.5F,
                       metrics_.safe.y + metrics_.safe.height - 176.0F * s, width,
                       42.0F * s * settings_.subtitle_size};
        fill(box, {0U, 0U, 0U, 190U});
        stroke(box, {126U, 137U, 140U, 155U}, std::max(1.0F, s));
        text(subtitle_text_, box.x + box.width * 0.5F, box.y + box.height * 0.5F - size * s * 0.42F,
             size, white, TextAlign::center, true);
    }

    void caption(std::string value, float duration = 1.4F)
    {
        if (!settings_.subtitles)
            return;
        subtitle_text_ = std::move(value);
        subtitle_time_ = duration;
    }

    void draw_online_join_loading()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        std::size_t active_step{};
        float progress{};
        switch (online_join_stage_)
        {
        case OnlineJoinStage::joining_lobby:
            active_step = 0U;
            progress = 0.07F + std::min(0.08F, online_join_elapsed_ * 0.012F);
            break;
        case OnlineJoinStage::awaiting_admission:
            active_step = 1U;
            progress = 0.25F;
            break;
        case OnlineJoinStage::receiving_world:
            active_step = 2U;
            progress = 0.42F;
            break;
        case OnlineJoinStage::syncing_profile:
            active_step = 2U;
            progress = 0.53F;
            break;
        case OnlineJoinStage::syncing_chunks: {
            active_step = 3U;
            const float chunks = online_sync_chunks_total_ == 0U
                                     ? 0.25F
                                     : static_cast<float>(online_sync_chunks_received_) /
                                           static_cast<float>(online_sync_chunks_total_);
            progress = 0.60F + std::clamp(chunks, 0.0F, 1.0F) * 0.27F;
            break;
        }
        case OnlineJoinStage::preparing_world:
            active_step = 4U;
            progress = 0.88F + world_preview_->preload_progress() * 0.12F;
            break;
        case OnlineJoinStage::idle:
            return;
        }
        progress = std::clamp(progress, 0.0F, 1.0F);

        const float panel_width = std::min(980.0F * s, metrics_.safe.width * 0.88F);
        const float panel_height = 438.0F * s;
        const Rect panel{metrics_.safe.x + (metrics_.safe.width - panel_width) * 0.5F,
                         metrics_.safe.y + (metrics_.safe.height - panel_height) * 0.5F + 34.0F * s,
                         panel_width, panel_height};
        fill({panel.x + 12.0F * s, panel.y + 14.0F * s, panel.width, panel.height},
             {0U, 0U, 0U, 105U});
        fill(panel, {7U, 11U, 13U, 247U});
        stroke(panel, {87U, 99U, 102U, 190U}, std::max(1.0F, s));
        fill({panel.x, panel.y, panel.width, 5.0F * s}, colors.accent);

        const float left = panel.x + 34.0F * s;
        const float right = panel.x + panel.width - 34.0F * s;
        text("SECURE STEAM SESSION  /  AUTHORITATIVE SYNC", left, panel.y + 28.0F * s, 10.0F,
             colors.accent, TextAlign::left, true);
        text("SYNCHRONIZING FRONTIER", left, panel.y + 54.0F * s, 29.0F, colors.text,
             TextAlign::left, true);
        const std::string server =
            online_join_server_name_.empty() ? "CASTLEMINER Z SESSION" : online_join_server_name_;
        text(server, left, panel.y + 101.0F * s, 14.0F, colors.text);
        const std::string host = online_join_host_name_.empty()
                                     ? "RESOLVING HOST IDENTITY"
                                     : std::format("HOST  {}", online_join_host_name_);
        text(host, left, panel.y + 126.0F * s, 10.0F, colors.secondary);

        const std::string protocol = uppercase(net::protocol_label(online_client_protocol_));
        const Rect protocol_box{right - 148.0F * s, panel.y + 42.0F * s, 148.0F * s, 46.0F * s};
        fill(protocol_box, {21U, 27U, 29U, 255U});
        stroke(protocol_box, colors.line, std::max(1.0F, s));
        text(protocol, protocol_box.x + protocol_box.width * 0.5F, protocol_box.y + 13.0F * s,
             11.0F, colors.text, TextAlign::center, true);

        fill({left, panel.y + 157.0F * s, right - left, std::max(1.0F, s)}, colors.line);
        constexpr std::array<std::string_view, 5> step_labels{
            "STEAM LOBBY", "HOST ADMISSION", "PLAYER DATA", "WORLD DELTAS", "RENDER READY"};
        const float steps_y = panel.y + 181.0F * s;
        const float gap = 8.0F * s;
        const float step_width = (right - left - gap * 4.0F) / 5.0F;
        for (std::size_t index{}; index < step_labels.size(); ++index)
        {
            const bool complete = index < active_step;
            const bool active = index == active_step;
            const Rect step{left + static_cast<float>(index) * (step_width + gap), steps_y,
                            step_width, 82.0F * s};
            fill(step, active     ? SDL_Color{34U, 17U, 19U, 255U}
                       : complete ? SDL_Color{19U, 25U, 25U, 255U}
                                  : SDL_Color{12U, 16U, 18U, 255U});
            stroke(step, active ? colors.accent : colors.line, active ? 2.0F * s : 1.0F * s);
            if (complete)
                fill({step.x, step.y, step.width, 3.0F * s}, {126U, 151U, 141U, 255U});
            else if (active)
                fill({step.x, step.y, step.width, 3.0F * s}, colors.accent);
            text(std::format("{:02}", index + 1U), step.x + 13.0F * s, step.y + 13.0F * s, 10.0F,
                 active     ? colors.accent
                 : complete ? colors.text
                            : colors.secondary,
                 TextAlign::left, active);
            text(step_labels[index], step.x + 13.0F * s, step.y + 45.0F * s, 9.0F,
                 active || complete ? colors.text : colors.secondary, TextAlign::left, active);
        }

        const Rect track{left, panel.y + 294.0F * s, right - left, 13.0F * s};
        fill(track, {22U, 28U, 30U, 255U});
        fill({track.x, track.y, track.width * progress, track.height}, colors.accent);
        const float pulse =
            settings_.reduced_motion ? 1.0F : 0.70F + 0.30F * std::sin(online_join_elapsed_ * 5.0F);
        const Uint8 pulse_alpha = static_cast<Uint8>(145.0F + 110.0F * pulse);
        const float head_x = track.x + track.width * progress;
        fill({std::max(track.x, head_x - 4.0F * s), track.y - 3.0F * s, 4.0F * s,
              track.height + 6.0F * s},
             {255U, 205U, 207U, pulse_alpha});
        text(std::format("{:3.0F}%", progress * 100.0F), right, panel.y + 319.0F * s, 11.0F,
             colors.text, TextAlign::right, true);
        text(online_join_detail_, left, panel.y + 321.0F * s, 11.0F, colors.secondary);

        const Rect status{left, panel.y + 365.0F * s, right - left, 43.0F * s};
        fill(status, {11U, 17U, 18U, 255U});
        stroke(status, colors.line, std::max(1.0F, s));
        const std::string identity = online_persona_name_.empty()
                                         ? "STEAM IDENTITY PENDING"
                                         : std::format("STEAM IDENTITY  {}", online_persona_name_);
        text(identity, status.x + 14.0F * s, status.y + 13.0F * s, 10.0F, colors.text);
        const std::string authority =
            online_join_lobby_id_ == 0U
                ? "LIVE AUTHORITY  /  PACKETS VALIDATED"
                : std::format("LOBBY {}  /  PACKETS VALIDATED", online_join_lobby_id_);
        text(authority, status.x + status.width - 14.0F * s, status.y + 13.0F * s, 10.0F,
             colors.secondary, TextAlign::right);
    }

    void draw_world_loading()
    {
        const auto colors = palette(settings_.high_contrast);
        const float s = metrics_.scale;
        const float progress = world_preview_->preload_progress();
        const float panel_width = std::min(720.0F * s, metrics_.safe.width * 0.72F);
        const float panel_height = 190.0F * s;
        const Rect panel{metrics_.safe.x + (metrics_.safe.width - panel_width) * 0.5F,
                         metrics_.safe.y + (metrics_.safe.height - panel_height) * 0.5F,
                         panel_width, panel_height};
        fill(panel, colors.panel);
        stroke(panel, colors.line, 1.0F * s);
        fill({panel.x, panel.y, panel.width, 4.0F * s}, colors.accent);
        text("PREPARING FRONTIER", panel.x + 34.0F * s, panel.y + 34.0F * s, 24.0F, colors.text);
        text("GENERATING VOXELS  /  BUILDING VERTICAL CHUNK SECTIONS", panel.x + 34.0F * s,
             panel.y + 79.0F * s, 11.0F, colors.secondary);
        const Rect track{panel.x + 34.0F * s, panel.y + 125.0F * s, panel.width - 68.0F * s,
                         12.0F * s};
        fill(track, {26U, 31U, 32U, 255U});
        fill({track.x, track.y, track.width * progress, track.height}, colors.accent);
        text(std::format("{:3.0F}%", progress * 100.0F), panel.x + panel.width - 34.0F * s,
             panel.y + 149.0F * s, 11.0F, colors.secondary, TextAlign::right);
    }

    void toast(std::string title, std::string body)
    {
        toast_title_ = std::move(title);
        toast_body_ = std::move(body);
        toast_time_ = 4.2F;
    }

    void scan_addons()
    {
        addons_.clear();
        mod_catalog_ = {};
        mod_catalog_error_.clear();
        mod_catalog_valid_ = net::load_mod_catalog(std::filesystem::current_path() / "addons",
                                                   mod_catalog_, mod_catalog_error_);
        if (!mod_catalog_valid_)
        {
            addons_.push_back("BLOCKED  /  " + uppercase(mod_catalog_error_));
            host_settings_.mod_manifest = {};
            host_settings_.mod_manifest.protocol = host_settings_.protocol;
            join_client_manifest_ = {};
            join_client_manifest_.protocol = net::ProtocolVersion::classic_v3;
            join_browser_.set_client(join_client_manifest_, true);
            return;
        }
        for (const net::LoadedMod &mod : mod_catalog_.mods)
        {
            const std::string profiles = mod.classic && mod.modern ? "CLASSIC+MODERN"
                                         : mod.classic             ? "CLASSIC"
                                                                   : "MODERN";
            addons_.push_back(std::format("{} {}  /  {}  /  {}", mod.descriptor.id,
                                          mod.descriptor.version,
                                          net::mod_scope_label(mod.descriptor.scope), profiles));
        }
        if (!net::make_mod_manifest(mod_catalog_, host_settings_.protocol,
                                    host_settings_.mod_manifest, mod_catalog_error_) ||
            !net::make_mod_manifest(mod_catalog_, net::ProtocolVersion::classic_v3,
                                    join_client_manifest_, mod_catalog_error_))
        {
            mod_catalog_valid_ = false;
            addons_.clear();
            addons_.push_back("BLOCKED  /  " + uppercase(mod_catalog_error_));
        }
        join_browser_.set_client(join_client_manifest_, true);
    }

    void apply_window_settings()
    {
        if (options_.screenshot_path.has_value())
        {
            return;
        }
        if (settings_.window_mode == WindowMode::windowed)
        {
            SDL_SetWindowFullscreen(window_, false);
            SDL_SetWindowFullscreenMode(window_, nullptr);
            SDL_SetWindowBordered(window_, true);
            SDL_SetWindowSize(window_, settings_.window_width, settings_.window_height);
            SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        else if (settings_.window_mode == WindowMode::borderless)
        {
            SDL_SetWindowFullscreenMode(window_, nullptr);
            SDL_SetWindowFullscreen(window_, true);
        }
        else
        {
            SDL_DisplayMode closest{};
            const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
            if (display != 0U &&
                SDL_GetClosestFullscreenDisplayMode(display, settings_.window_width,
                                                    settings_.window_height, 0.0F, true, &closest))
            {
                SDL_SetWindowFullscreenMode(window_, &closest);
                SDL_SetWindowFullscreen(window_, true);
            }
            else
            {
                SDL_SetWindowFullscreenMode(window_, nullptr);
                SDL_SetWindowFullscreen(window_, true);
            }
        }
        SDL_SyncWindow(window_);
    }

    void apply_audio_settings()
    {
        if (sound_bank_ == nullptr)
            return;
        sound_bank_->set_levels(settings_.master_volume, settings_.music_volume,
                                settings_.effects_volume, settings_.ambience_volume,
                                settings_.mute_unfocused && !window_focused_);
    }

    void cycle_audio_output(int direction)
    {
        if (audio_outputs_.empty())
            return;
        auto found =
            std::find_if(audio_outputs_.begin(), audio_outputs_.end(), [&](const auto &output) {
                return output.id == settings_.audio_output_id;
            });
        int index =
            found == audio_outputs_.end() ? 0 : static_cast<int>(found - audio_outputs_.begin());
        const int count = static_cast<int>(audio_outputs_.size());
        index = (index + (direction < 0 ? -1 : 1) + count) % count;
        settings_.audio_output_id = audio_outputs_[static_cast<std::size_t>(index)].id;
        settings_.audio_output_name = audio_outputs_[static_cast<std::size_t>(index)].name;
    }

    void save_settings()
    {
        if (!dirty_ && frames_rendered_ > 0U)
        {
            return;
        }
        try
        {
            store_.save(settings_);
            dirty_ = false;
            dirty_timer_ = 0.0F;
        }
        catch (const std::exception &error)
        {
            std::cerr << "settings save failed: " << error.what() << '\n';
        }
    }

    void capture(const std::filesystem::path &path)
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr);
        if (surface == nullptr || !SDL_SaveBMP(surface, path.string().c_str()))
        {
            if (surface != nullptr)
            {
                SDL_DestroySurface(surface);
            }
            throw std::runtime_error{std::string{"screenshot failed: "} + SDL_GetError()};
        }
        SDL_DestroySurface(surface);
        std::cout << "Screenshot: " << path.string() << '\n';
    }

    [[nodiscard]] bool write_interop_state_dump(const std::filesystem::path &path,
                                                std::string &error) const
    {
        InteropStateSnapshot snapshot;
        if (online_host_ != nullptr || hosting_flow_)
        {
            snapshot.role = InteropStateRole::host;
            snapshot.phase = online_host_ != nullptr ? InteropStatePhase::hosting
                                                     : InteropStatePhase::loading;
            snapshot.protocol = host_settings_.protocol;
        }
        else if (online_client_joined_)
        {
            snapshot.role = InteropStateRole::client;
            snapshot.phase = waiting_for_world_ ? InteropStatePhase::loading
                                                : InteropStatePhase::connected;
            snapshot.protocol = online_client_protocol_;
        }
        else
        {
            snapshot.role = InteropStateRole::offline;
            snapshot.phase = waiting_for_world_
                                 ? InteropStatePhase::loading
                             : menu_.screen() == ui::MenuScreen::world_preview ||
                                       menu_.screen() == ui::MenuScreen::pause
                                 ? InteropStatePhase::world
                                 : InteropStatePhase::frontend;
            snapshot.protocol = inventory_.profile() == InventoryProfile::modern
                                    ? net::ProtocolVersion::modern_v4
                                    : net::ProtocolVersion::classic_v3;
        }
        snapshot.world_seed = world_seed_;
        snapshot.player_position = player_.state().position;
        snapshot.player_yaw = player_.state().yaw;
        snapshot.player_pitch = player_.state().pitch;
        snapshot.player_health = player_.state().health;
        snapshot.inventory_hash = fingerprint_interop_inventory(inventory_);
        const std::vector<std::array<int, 4>> edits = world_preview_->exported_edits();
        snapshot.edit_hash = fingerprint_interop_edits(edits);
        snapshot.edit_count = edits.size();
        int region_center_x = static_cast<int>(std::floor(snapshot.player_position.x));
        int region_center_z = static_cast<int>(std::floor(snapshot.player_position.z));
        if (options_.state_region_center.has_value())
        {
            region_center_x = (*options_.state_region_center)[0];
            region_center_z = (*options_.state_region_center)[1];
            if (!interop_region_fully_loaded(world_preview_->region(), region_center_x,
                                             region_center_z))
            {
                error = "The requested 33x128x33 interop state region is not fully loaded";
                return false;
            }
        }
        snapshot.region = fingerprint_interop_region(world_preview_->region(), region_center_x,
                                                     region_center_z);
        snapshot.enemy_count = enemy_horde_.enemies().size();
        snapshot.pickup_count = world_preview_->stats().block_pickup_count;
        snapshot.player_dead = player_life_ != PlayerLife::alive;
        snapshot.dragon_active = dragon_.active();
        const std::uint8_t local_gid = snapshot.role == InteropStateRole::host
                                           ? 0U
                                       : snapshot.role == InteropStateRole::client ||
                                                 options_.diagnostic_multiplayer
                                           ? online_local_gid_
                                           : 0U;
        snapshot.players.push_back({local_gid,
                                    true,
                                    snapshot.player_position,
                                    snapshot.player_yaw,
                                    snapshot.player_pitch,
                                    snapshot.player_health,
                                    snapshot.player_dead});
        for (const auto &[gid, remote] : online_remote_players_)
        {
            if (!remote.has_update || gid == local_gid)
                continue;
            snapshot.players.push_back({gid,
                                        false,
                                        remote.state.position,
                                        remote.state.yaw,
                                        remote.state.torso_pitch,
                                        std::nullopt,
                                        remote.state.dead});
        }
        snapshot.remote_player_count = snapshot.players.size() - 1U;
        return write_interop_state_snapshot(path, snapshot, error);
    }

    void capture_world_thumbnail()
    {
        if (active_world_path_.empty() || !world_ready())
            return;
        SDL_Surface *full = SDL_RenderReadPixels(renderer_, nullptr);
        if (full == nullptr)
            return;
        SDL_Surface *scaled = SDL_ScaleSurface(full, 480, 270, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(full);
        if (scaled == nullptr)
            return;
        const std::filesystem::path path = world_preview_path(active_world_path_);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!error && !SDL_SaveBMP(scaled, path.string().c_str()))
            std::cerr << "world.preview.capture.failure=" << SDL_GetError() << '\n';
        SDL_DestroySurface(scaled);
    }

    LaunchOptions options_;
    SettingsStore store_;
    Settings settings_;
    AssetStyle loaded_asset_style_{AssetStyle::modern};
    ui::MenuModel menu_;
    SDL_Window *window_{};
    SDL_Renderer *renderer_{};
    std::unique_ptr<FontRenderer> font_;
    std::unique_ptr<WorldPreviewRenderer> world_preview_;
    SDL_Texture *sniper_scope_texture_{};
    SDL_Texture *missile_lock_texture_{};
    SDL_Texture *missile_locking_texture_{};
    std::unique_ptr<StaticModelAsset> pickaxe_art_;
    std::unique_ptr<StaticModelAsset> compass_art_;
    std::unique_ptr<StaticModelAsset> pistol_art_;
    std::unique_ptr<StaticModelAsset> knife_art_;
    std::unique_ptr<StaticModelAsset> ammo_art_;
    std::unique_ptr<StaticModelAsset> torch_art_;
    std::unique_ptr<StaticModelAsset> spade_art_;
    std::unique_ptr<StaticModelAsset> axe_art_;
    std::unique_ptr<StaticModelAsset> ak_art_;
    std::unique_ptr<StaticModelAsset> shotgun_art_;
    std::unique_ptr<StaticModelAsset> rifle_art_;
    std::unique_ptr<StaticModelAsset> smg_art_;
    std::unique_ptr<StaticModelAsset> lmg_art_;
    std::unique_ptr<StaticModelAsset> rpg_art_;
    std::unique_ptr<StaticModelAsset> grenade_art_;
    std::unique_ptr<StaticModelAsset> ore_art_;
    std::unique_ptr<StaticModelAsset> gems_art_;
    std::unique_ptr<StaticModelAsset> bars_art_;
    std::unique_ptr<StaticModelAsset> space_ak_art_;
    std::unique_ptr<StaticModelAsset> space_pistol_art_;
    std::unique_ptr<StaticModelAsset> space_shotgun_art_;
    std::unique_ptr<StaticModelAsset> space_rifle_art_;
    std::unique_ptr<StaticModelAsset> space_smg_art_;
    std::unique_ptr<StaticModelAsset> saber_art_;
    std::unique_ptr<StaticModelAsset> locator_art_;
    std::unique_ptr<StaticModelAsset> teleporter_art_;
    std::unique_ptr<StaticModelAsset> clock_art_;
    std::unique_ptr<StaticModelAsset> gunpowder_art_;
    std::unique_ptr<StaticModelAsset> grenade_icon_art_;
    std::vector<AudioOutputDevice> audio_outputs_;
    std::unique_ptr<OriginalSoundBank> sound_bank_;
    SDL_Gamepad *gamepad_{};
    world::PlayerSimulation player_;
    PlayerInventory inventory_;
    CrateMap crate_inventories_;
    DoorMap custom_doors_;
    SpawnerMap world_spawners_;
    std::set<CrateKey> active_world_spawners_;
    std::map<CrateKey, LocalSpawnerRuntime> local_spawners_;
    std::map<world::EnemyIdentity, CrateKey> local_spawner_enemy_sources_;
    std::unordered_map<net::PeerId, std::vector<PendingRemoteSpawnerTrigger>>
        pending_remote_spawner_triggers_;
    std::map<CrateKey, net::PeerId> remote_spawner_owners_;
    std::map<world::EnemyIdentity, std::uint8_t> remote_spawner_enemy_owners_;
    std::map<CrateKey, RemoteLootClaim> remote_loot_claims_;
    world::EnemyInstanceRng spawner_world_random_{1337 ^ 0x53A9D17B};
    world::EnemyInstanceRng spawner_enemy_random_{static_cast<std::int32_t>(1337U ^ 0xA17C39E5U)};
    world::LegacyRandom loot_random_{static_cast<std::int32_t>(1337U ^ 0x4C4F4F54U)};
    int defeated_spawner_count_{};
    std::optional<CrateKey> active_crate_;
    world::DayNightCycle day_night_;
    std::int32_t world_seed_{1337};
    world::LightningCycle lightning_cycle_{1337U ^ 0x5A17C0DEULL};
    bool lightning_visible_{};
    float world_time_speed_{1.0F};
    float world_elapsed_{};
    float physics_accumulator_{};
    float footstep_timer_{0.5F};
    float placement_animation_time_{};
    ui::UiMetrics metrics_{};
    std::vector<HitRegion> hits_;
    std::vector<float> focus_;
    std::vector<std::string> addons_;
    net::ModCatalog mod_catalog_;
    std::string mod_catalog_error_;
    bool mod_catalog_valid_{true};
    std::string profile_name_{system_profile_name()};
    net::HostSessionSettings host_settings_;
    net::SessionBrowser join_browser_;
    net::ModManifest join_client_manifest_;
    std::vector<std::string> join_labels_;
    std::vector<std::string> join_descriptions_;
    std::vector<std::string> join_badges_;
    std::vector<std::size_t> join_row_sessions_;
    std::optional<std::size_t> selected_join_session_;
    std::string join_password_;
    JoinBrowserState join_browser_state_{JoinBrowserState::idle};
    std::string join_browser_status_{"LIVE STEAM DISCOVERY HAS NOT STARTED"};
    float join_refresh_timer_{30.0F};
    bool online_discovery_quiet_{};
    std::string direct_address_;
    std::optional<net::DirectEndpoint> direct_endpoint_;
    bool hosting_flow_{};
    std::future<OnlineHostStartResult> online_host_start_;
    std::unique_ptr<net::LegacySteamRuntimeHost> online_host_;
    std::uint64_t online_lobby_id_{};
    std::future<OnlineClientOperationResult> online_client_operation_;
    std::unique_ptr<net::LegacySteamRuntimeClient> online_client_;
    std::unique_ptr<net::AuthoritativeBootstrapClient> online_bootstrap_client_;
    OnlineClientOperation online_client_pending_{OnlineClientOperation::none};
    OnlineJoinStage online_join_stage_{OnlineJoinStage::idle};
    std::string online_join_server_name_;
    std::string online_join_host_name_;
    std::string online_join_detail_;
    std::uint64_t online_join_lobby_id_{};
    std::size_t online_sync_chunks_received_{};
    std::size_t online_sync_chunks_total_{};
    float online_join_elapsed_{};
    net::ProtocolVersion online_client_protocol_{net::ProtocolVersion::classic_v3};
    GameMode online_client_mode_{GameMode::survival};
    GameDifficulty online_client_difficulty_{GameDifficulty::hard};
    net::PlayerVersusPlayer online_client_pvp_{net::PlayerVersusPlayer::off};
    bool online_lobby_host_is_friend_{};
    bool online_client_joined_{};
    bool ui_join_probe_started_{};
    bool ui_join_probe_failed_{};
    std::string ui_join_probe_detail_;
    std::uint8_t online_local_gid_{};
    std::uint8_t online_terrain_server_gid_{};
    float online_player_update_timer_{};
    float online_inventory_save_timer_{10.0F};
    float online_position_save_timer_{5.0F};
    float online_world_update_timer_{5.0F};
    std::optional<net::PlayerInventoryPayload> online_inventory_payload_;
    std::string online_world_key_;
    std::map<net::IntVector3, float> pending_chunk_reconciliations_;
    std::map<net::IntVector3, float> requested_chunk_reconciliations_;
    std::string online_persona_name_;
    bool steam_identity_verified_{};
    std::optional<std::uint16_t> last_online_carried_item_;
    std::unordered_map<std::uint8_t, OnlineRemotePlayer> online_remote_players_;
    std::unordered_map<std::uint8_t, std::vector<PendingPrePresencePacket>>
        pending_pre_presence_packets_;
    std::size_t pending_pre_presence_bytes_{};
    std::vector<RemotePlayerVisual> online_remote_visuals_;
    net::ClientGameplayRateLimiter online_gameplay_rate_limiter_;
    HostTextField host_text_field_{HostTextField::none};
    std::string host_text_draft_;
    std::string host_text_fallback_;
    bool chat_editing_{};
    bool player_roster_visible_{};
    std::string chat_draft_;
    std::string chat_key_fallback_;
    std::vector<OnlineChatLine> online_chat_lines_;
    bool running_{true};
    bool window_focused_{true};
    bool inspector_view_{};
    bool inventory_open_{};
    bool locator_name_editing_{};
    std::size_t locator_name_slot_{};
    std::string locator_name_draft_;
    std::string locator_key_fallback_;
    bool waiting_for_world_{};
    bool gamepad_use_down_{};
    bool gamepad_activate_down_{};
    bool gamepad_input_active_{};
    bool mouse_primary_down_{};
    bool mouse_secondary_down_{};
    Uint64 last_mouse_activity_ticks_{};
    bool mine_button_down_{};
    bool secondary_action_down_{};
    bool shouldering_{};
    float air_punch_time_{};
    float melee_cooldown_{};
    float gun_cooldown_{};
    float gun_recoil_time_{};
    float gun_recoil_duration_{0.15F};
    float fired_launcher_time_{};
    std::optional<std::size_t> fired_launcher_slot_;
    std::optional<std::size_t> fired_launcher_tray_;
    std::optional<InventoryItemId> fired_launcher_visual_;
    float gun_reload_time_{};
    float gun_aim_progress_{};
    bool gun_reloading_{};
    bool gun_reload_cue_started_{};
    std::optional<InventoryItemId> looping_equipped_item_;
    world::EnemyRng recoil_rng_{0xC45A71E2D39B5601ULL};
    float guided_lock_time_{};
    float guided_beep_time_{};
    float guided_lock_required_{};
    float guided_lock_distance_{};
    bool guided_locked_{};
    bool guided_tone_played_{};
    bool grenade_cooking_{};
    std::uint8_t grenade_cooking_type_{};
    float grenade_cook_time_{};
    float grenade_cooldown_{};
    float grenade_animation_time_{};
    int craft_scroll_{};
    bool craft_modern_{};
    int craft_category_{-1};
    std::optional<std::size_t> craft_selected_;
    std::vector<CraftHit> craft_hits_;
    world::EnemyHorde enemy_horde_;
    float surface_spawn_timer_{};
    float cave_spawn_timer_{};
    float felguard_timer_{};
    bool felguard_spawned_{};
    bool diagnostic_enemy_spawned_{};
    bool diagnostic_dynamic_radiance_spawned_{};
    float alien_aroused_time_{};
    float alien_spawn_timer_{};
    float next_alien_delay_{15.0F};
    float space_rock_scan_timer_{};
    bool space_rock_nearby_{};
    float closest_space_rock_{1000.0F};
    mutable int crater_probe_x_{std::numeric_limits<int>::min()};
    mutable int crater_probe_y_{std::numeric_limits<int>::min()};
    mutable int crater_probe_z_{std::numeric_limits<int>::min()};
    mutable std::size_t crater_probe_stream_serial_{std::numeric_limits<std::size_t>::max()};
    mutable float crater_probe_time_{-1000.0F};
    mutable bool crater_probe_dry_{};
    float world_game_time_{};
    float last_player_damage_time_{-100.0F};
    float player_oxygen_{1.0F};
    float lava_damage_time_{world::lava_damage_period_seconds};
    bool lava_sound_played_{};
    std::uint32_t next_enemy_id_{1U};
    struct Arrow final
    {
        world::Vec3 head{};
        world::Vec3 tail{};
        world::Vec3 head_velocity{};
        world::Vec3 tail_velocity{};
        std::uint8_t target_gid{};
        float time_left{2.0F};
        float tail_time{1.8F};
        bool whizzed{};
        bool dead{};
    };
    std::vector<Arrow> arrows_;
    struct ArmedExplosive final
    {
        int x{};
        int y{};
        int z{};
        world::ExplosiveKind kind{world::ExplosiveKind::tnt};
        float time_left{};
        float age{};
        bool locally_authoritative{};
        bool dead{};
    };
    std::vector<ArmedExplosive> armed_explosives_;
    struct Grenade final
    {
        world::Vec3 position{};
        world::Vec3 velocity{};
        float time_left{};
        float spin{};
        std::uint8_t grenade_type{};
        std::uint8_t owner_gid{};
        std::optional<world::EnemyIdentity> attached_enemy;
        world::Vec3 attached_offset{};
        bool locally_authoritative{true};
        bool stopped{};
        bool dead{};
    };
    std::vector<Grenade> grenades_;
    std::vector<RecentNetworkProjectile> recent_network_projectiles_;
    // TracerManager.Tracer: a flying projectile whose visible streak IS the
    // collision segment.  The tail freezes for the first 0.4 s so the streak
    // grows, then follows at the same ballistic rate.
    struct Tracer final
    {
        world::Vec3 head{};
        world::Vec3 tail{};
        world::Vec3 head_velocity{};
        world::Vec3 tail_velocity{};
        float time_left{};
        float flight_time{};
        std::array<float, 3> color{1.0F, 1.0F, 1.0F};
        float damage{};
        world::DamageFlags flags{};
        InventoryItemId item{InventoryItemId::bare_hands};
        int id{};
        std::uint8_t owner_gid{};
        float rocket_spin{};
        world::Vec3 rocket_direction{};
        world::Vec3 rocket_emitted_direction{};
        world::Vec3 rocket_start{};
        world::Vec3 rocket_guided_position{};
        world::Vec3 rocket_starting_velocity{};
        bool rocket{};
        bool guided_launcher{};
        bool guided{};
        bool laser{};
        bool locally_authoritative{true};
        int bounces{};
        bool dead{};
    };
    std::vector<Tracer> tracers_;
    int next_projectile_id_{1};
    struct DamageIndicator final
    {
        world::Vec3 source{};
        float age{};
    };
    std::vector<DamageIndicator> damage_indicators_;
    world::DragonFlight dragon_;
    std::optional<std::uint8_t> dragon_owner_gid_;
    bool dragon_for_biome_{};
    bool dragon_pending_{};
    float dragon_network_time_{};
    float dragon_update_timer_{};
    float dragon_last_host_time_{-1.0F};
    int dragon_updates_sent_{};
    std::uint32_t next_dragon_fireball_index_{};
    std::uint8_t dragon_network_sound_{};
    std::set<std::int32_t> queued_dragon_fireball_ids_;
    float dragon_migration_scan_timer_{};
    bool dragon_kill_announced_{};
    bool dragon_loot_pending_{};
    world::DragonKind next_timed_dragon_{world::DragonKind::fire};
    int dragon_distance_index_{};
    float last_dragon_distance_{-1.0F};
    float last_song_distance_{-1.0F};
    float distance_origin_x_{};
    float distance_origin_z_{};
    bool distance_origin_set_{};
    int current_distance_{};
    int max_distance_{};
    bool zombie_fest_{};
    float fest_countdown_{-1.0F};
    float dragon_box_center_x_{};
    float dragon_box_center_z_{};
    bool dragon_box_valid_{};
    float next_dragon_seconds_{};
    GameMode game_mode_{GameMode::survival};
    GameDifficulty difficulty_{GameDifficulty::hard};
    bool infinite_resources_{};
    // The "fresh ground" pack spawner (Normal/Hardcore/Endurance).
    float cleared_distance_{50.0F};
    float time_to_first_contact_{20.0F};
    int distance_enemies_left_{};
    float next_distance_enemy_timer_{};
    // Dragon Endurance wave loop.
    float endurance_dragon_health_{25.0F};
    int endurance_dragon_type_{};
    float endurance_dragon_delay_{};
    float dragon_cry_timer_{5.0F};
    float dragon_flap_timer_{1.0F};
    float audio_environment_timer_{};
    int next_song_index_{};
    bool crater_music_triggered_{};
    float crater_music_reset_timer_{};
    int last_day_number_{};
    enum class PlayerLife : std::uint8_t
    {
        alive,
        dying,
        dead
    };
    PlayerLife player_life_{PlayerLife::alive};
    float death_timer_{};
    struct Fireball final
    {
        world::Vec3 position{};
        world::Vec3 velocity{};
        world::DragonKind kind{};
        int id{};
        bool was_in_loaded_area{};
        bool dead{};
        std::uint8_t owner_gid{};
        bool spawned_locally{true};
        bool awaiting_detonation{};
        float detonation_wait{};
    };
    std::vector<Fireball> fireballs_;
    std::vector<std::int32_t> applied_fireball_detonations_;
    int next_fireball_id_{};
    std::vector<WorldSlot> world_slots_;
    std::filesystem::path active_world_path_;
    float autosave_timer_{30.0F};
    std::optional<world::Vec3> pending_load_position_;
    bool pending_load_is_online_{};
    float pending_load_yaw_{};
    float pending_load_pitch_{};
    float pending_load_health_{100.0F};
    int previous_biome_band_{-1};
    bool dirty_{};
    float dirty_timer_{};
    float elapsed_{};
    float transition_{1.0F};
    float mouse_x_{};
    float mouse_y_{};
    std::size_t last_transition_serial_{};
    std::size_t frames_rendered_{};
    std::size_t world_ready_frames_{};
    bool world_thumbnail_captured_for_pause_{};
    std::optional<InventoryStack> held_stack_;
    std::size_t inventory_focus_{PlayerInventory::storage_size};
    std::string toast_title_;
    std::string toast_body_;
    float toast_time_{};
    std::string subtitle_text_;
    float subtitle_time_{};
};

Application::Application(LaunchOptions options) : impl_(new Impl{std::move(options)})
{
}
Application::~Application()
{
    delete impl_;
}
int Application::run()
{
    return impl_->run();
}

} // namespace openminerz
