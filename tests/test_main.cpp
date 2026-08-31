#include "openminerz/design_canvas.hpp"
#include "openminerz/input_contract.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/interop_state.hpp"
#include "openminerz/menu_model.hpp"
#include "openminerz/net/admission.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/host_session.hpp"
#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_codec.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/legacy_steam_runtime_host.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/net/protocol_trace.hpp"
#include "openminerz/net/session_browser.hpp"
#include "openminerz/net/steam_bridge_client.hpp"
#include "openminerz/net/steam_bridge_protocol.hpp"
#include "openminerz/net/transport.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"
#include "openminerz/world/biome_atmosphere.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/dynamic_radiance.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/explosive.hpp"
#include "openminerz/world/legacy_noise.hpp"
#include "openminerz/world/loot.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/spawner.hpp"
#include "openminerz/world/world.hpp"
#include "openminerz/world_save.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error{std::string{message}};
    }
}

bool near(float left, float right, float tolerance = 0.001F)
{
    return std::abs(left - right) <= tolerance;
}

std::vector<std::uint8_t> bytes_from_hex(std::string_view text)
{
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint8_t>(value - 'A' + 10);
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::runtime_error{"invalid hexadecimal fixture"};
    };
    if ((text.size() & 1U) != 0U)
        throw std::runtime_error{"odd hexadecimal fixture"};
    std::vector<std::uint8_t> result(text.size() / 2U);
    for (std::size_t index{}; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((nibble(text[index * 2U]) << 4U) |
                                                  nibble(text[index * 2U + 1U]));
    return result;
}

void pointer_input_contract_never_routes_secondary_to_jump_or_drop()
{
    using openminerz::InventoryPointerRoute;
    using openminerz::PointerButton;

    const auto primary = openminerz::world_pointer_press(PointerButton::primary);
    const auto secondary = openminerz::world_pointer_press(PointerButton::secondary);
    expect(primary.primary_action && !primary.activate && !primary.secondary_action &&
               !primary.jump,
           "left mouse must remain primary use without a hidden movement action");
    expect(!secondary.primary_action && secondary.activate && secondary.secondary_action &&
               !secondary.jump,
           "right mouse must route to activate/shoulder and can never request jump");
    expect(openminerz::inventory_pointer_route(false) == InventoryPointerRoute::ignore &&
               openminerz::inventory_pointer_route(true) == InventoryPointerRoute::slot,
           "an outside inventory click must be ignored; only a real slot may consume it");
    expect(openminerz::world_controls_enabled(true, false) &&
               !openminerz::world_controls_enabled(true, true) &&
               !openminerz::world_controls_enabled(false, false),
           "gameplay controls must exist only in a ready world, never in loading or menu state");
}

void biome_atmospheres_are_distinct_bounded_and_reversible()
{
    using namespace openminerz::world;
    const BiomeAtmosphere classic = biome_atmosphere(BiomeKind::classic);
    const BiomeAtmosphere desert = biome_atmosphere(BiomeKind::desert);
    const BiomeAtmosphere arctic = biome_atmosphere(BiomeKind::arctic);
    const BiomeAtmosphere lagoon = biome_atmosphere(BiomeKind::lagoon);

    expect(classic.storminess == 1.0F && classic.saturation < 0.9F &&
               classic.fog_start_ratio < 0.40F && classic.grade_strength > 0.0F,
           "the origin biome must retain its cool foggy storm identity through daylight");
    expect(desert.fog_target.red > desert.fog_target.blue &&
               desert.grade_multiplier.red > desert.grade_multiplier.blue &&
               desert.fog_start_ratio < classic.fog_start_ratio,
           "desert air must be warmer and begin farther from the streaming edge");
    expect(arctic.fog_target.blue > arctic.fog_target.red &&
               arctic.grade_multiplier.blue > arctic.grade_multiplier.red &&
               arctic.saturation < 1.0F,
           "arctic air must be colder and more desaturated than neutral presentation");
    expect(lagoon.grade_multiplier.blue > lagoon.grade_multiplier.red &&
               lagoon.fog_start_ratio > desert.fog_start_ratio &&
               lagoon.storminess < classic.storminess && lagoon.storminess > desert.storminess,
           "humid lagoon haze must remain distinct from the denser desert dust curtain");
    for (BiomeKind biome : {BiomeKind::classic, BiomeKind::lagoon, BiomeKind::desert,
                            BiomeKind::mountain, BiomeKind::arctic, BiomeKind::descent})
    {
        const BiomeAtmosphere atmosphere = biome_atmosphere(biome);
        expect(atmosphere.fog_start_ratio >= 0.30F && atmosphere.fog_start_ratio <= 0.55F &&
                   atmosphere.grade_strength >= 0.0F && atmosphere.grade_strength <= 0.30F &&
                   atmosphere.saturation >= 0.75F && atmosphere.saturation <= 1.0F &&
                   atmosphere.storminess >= 0.0F && atmosphere.storminess <= 1.0F,
               "biome fog and grading must stay inside the restrained authored envelope");
    }
}

void dynamic_radiance_is_world_anchored_colored_and_occlusion_safe()
{
    using namespace openminerz::world;
    WorldRegion region{0, 0, 1, 1};
    constexpr int target_x = 9;
    constexpr int target_y = 8;
    constexpr int target_z = 8;
    region.set_block(target_x, target_y, target_z, BlockType::blood_stone);
    const Vec3 light{5.5F, 8.5F, 8.5F};
    const auto open = build_dynamic_radiance(region, light);
    const auto repeated = build_dynamic_radiance(region, light);
    const auto red = open.radiance_at(target_x, target_y, target_z);
    expect(open.active && open.texels.size() == static_cast<std::size_t>(dynamic_radiance_extent *
                                                                         dynamic_radiance_extent *
                                                                         dynamic_radiance_extent),
           "an enabled held light must build the bounded 19-cubed radiance clipmap");
    expect(open.origin_x == repeated.origin_x && open.origin_y == repeated.origin_y &&
               open.origin_z == repeated.origin_z && open.texels == repeated.texels,
           "the world-anchored clipmap must rebuild bit-identically without camera history");
    expect(red[0] > red[1] * 3.0F && red[0] > red[2] * 2.0F,
           "a directly visible blood-stone face must seed red reflected radiance");
    const auto transported = open.radiance_at(target_x - 1, target_y, target_z);
    expect(transported[0] > 0.0F && transported[0] > transported[1] * 3.0F,
           "surface radiance must propagate into adjacent open cells without losing its color");

    region.set_block(7, target_y, target_z, BlockType::rock);
    const auto blocked = build_dynamic_radiance(region, light);
    expect(blocked.radiance_at(target_x, target_y, target_z) == std::array<float, 3>{},
           "an intervening opaque voxel must prevent a hidden surface from seeding radiance");
    const auto disabled = build_dynamic_radiance(region, light, false);
    expect(!disabled.active && disabled.texels.empty(),
           "a disabled moving light must allocate no dynamic radiance payload");

    WorldRegion colored_region{0, 0, 1, 1};
    colored_region.set_block(5, target_y, target_z, BlockType::rock);
    colored_region.set_block(11, target_y, target_z, BlockType::rock);
    const std::array colored_sources{
        DynamicRadianceSource{{2.5F, 8.5F, 8.5F}, {1.0F, 0.01F, 0.01F}, 4.5F, 1.0F},
        DynamicRadianceSource{{14.5F, 8.5F, 8.5F}, {0.01F, 0.08F, 1.0F}, 4.5F, 1.0F},
    };
    const auto colored =
        build_dynamic_radiance(colored_region, {8.5F, 8.5F, 8.5F},
                               std::span<const DynamicRadianceSource>{colored_sources});
    const auto red_source = colored.radiance_at(5, target_y, target_z);
    const auto blue_source = colored.radiance_at(11, target_y, target_z);
    expect(red_source[0] > red_source[2] * 5.0F && blue_source[2] > blue_source[0] * 5.0F,
           "independent moving lights must preserve warm and ice-blue source colours");
    expect(colored.origin_x == -1 && colored.origin_y == -1 && colored.origin_z == -1,
           "multiple moving lights must keep the clipmap anchored to the player, not a source");

    const std::array dark_source{DynamicRadianceSource{light, {1.0F, 0.5F, 0.1F}, 9.0F, 0.0F}};
    const auto inactive =
        build_dynamic_radiance(region, light, std::span<const DynamicRadianceSource>{dark_source});
    expect(!inactive.active && inactive.texels.empty(),
           "expired zero-intensity emitters must not allocate a clipmap");
    expect(near(dynamic_radiance_lifetime_fade(0.0F, 1.0F), 1.0F) &&
               near(dynamic_radiance_lifetime_fade(0.5F, 1.0F), 0.25F) &&
               near(dynamic_radiance_lifetime_fade(1.0F, 1.0F), 0.0F) &&
               near(dynamic_radiance_lifetime_fade(0.0F, 0.0F), 0.0F),
           "transient radiance must decay deterministically and expire cleanly");

    WorldRegion empty_region{0, 0, 1, 1};
    const std::array transient_source{
        DynamicRadianceSource{{8.5F, 8.5F, 8.5F}, {0.1F, 0.4F, 1.0F}, 6.5F, 1.0F, 1.0F}};
    const auto transient = build_dynamic_radiance(
        empty_region, {8.5F, 8.5F, 8.5F}, std::span<const DynamicRadianceSource>{transient_source});
    const auto emission = transient.radiance_at(8, 8, 8);
    const auto nearby_emission = transient.radiance_at(10, 8, 8);
    expect(emission[2] > emission[0] * 5.0F && nearby_emission[2] > 0.1F,
           "a transient projectile must seed colored air radiance for nearby entities and VFX");

    std::array<DynamicRadianceSource, dynamic_radiance_max_sources + 1U> prioritized{};
    for (std::size_t index = 0U; index < dynamic_radiance_max_sources; ++index)
    {
        prioritized[index] = {{8.5F, 8.5F, 8.5F}, {1.0F, 0.01F, 0.01F}, 6.5F, 1.0F, 1.0F};
    }
    prioritized.back() = {{8.5F, 8.5F, 8.5F}, {0.01F, 0.01F, 1.0F}, 6.5F, 1.0F, 1.0F};
    const auto capped = build_dynamic_radiance(empty_region, {8.5F, 8.5F, 8.5F},
                                               std::span<const DynamicRadianceSource>{prioritized});
    const auto priority_color = capped.radiance_at(8, 8, 8);
    expect(priority_color[0] > priority_color[2] * 5.0F,
           "the bounded builder must consume only the first five caller-prioritized sources");
}

void canvas_normalizes_resolutions_and_fonts()
{
    const auto hd = openminerz::ui::DesignCanvas::compute(1920, 1080, 1.0F, 0.96F);
    const auto uhd = openminerz::ui::DesignCanvas::compute(3840, 2160, 1.0F, 0.96F);
    const auto ultrawide = openminerz::ui::DesignCanvas::compute(3440, 1440, 1.0F, 0.96F);
    expect(near(hd.scale, 1.0F), "1080p must map to the reference density");
    expect(near(uhd.scale, 2.0F), "4K must rasterize fonts at twice the 1080p density");
    expect(near(ultrawide.scale, 1440.0F / 1080.0F), "ultrawide density must be height-limited");
    expect(hd.font_pixels(24.0F) == 24 && uhd.font_pixels(24.0F) == 48,
           "font pixels must track real output density");
    expect(ultrawide.safe.width > 3000.0F, "safe area must preserve ultrawide horizontal space");
}

void settings_clamp_and_round_trip()
{
    openminerz::Settings settings;
    settings.ui_scale = 9.0F;
    settings.vertical_fov = 5.0F;
    settings.master_volume = -2.0F;
    settings.creative_flight_speed = 100.0F;
    settings.normalize();
    expect(near(settings.ui_scale, 1.5F), "UI scale must clamp high values");
    expect(near(settings.vertical_fov, 50.0F), "vertical FOV must clamp low values");
    expect(near(settings.master_volume, 0.0F), "volume must clamp low values");
    expect(near(settings.creative_flight_speed, 32.0F),
           "Creative flight speed must clamp unsafe high values");

    const auto path = std::filesystem::temp_directory_path() / "openminerz-settings-test.cfg";
    openminerz::SettingsStore store{path};
    settings.window_width = 2560;
    settings.window_height = 1440;
    settings.presentation = openminerz::PresentationProfile::classic;
    settings.texture_style = openminerz::AssetStyle::classic_hd;
    settings.audio_output_id = "{test-renderer-id}";
    settings.audio_output_name = "Test speakers";
    settings.creative_flight_speed = 19.0F;
    settings.reduced_motion = true;
    settings.ore_glow = false;
    store.save(settings);
    const auto loaded = store.load();
    expect(loaded.window_width == 2560 && loaded.window_height == 1440,
           "resolution must survive a settings round trip");
    expect(loaded.presentation == openminerz::PresentationProfile::classic,
           "presentation profile must survive a settings round trip");
    expect(loaded.texture_style == openminerz::AssetStyle::classic_hd,
           "Classic HD texture style must survive a settings round trip");
    expect(loaded.audio_output_id == settings.audio_output_id &&
               loaded.audio_output_name == settings.audio_output_name,
           "audio output selection must survive a settings round trip");
    expect(near(loaded.creative_flight_speed, 19.0F),
           "Creative flight speed must survive a settings round trip");
    expect(loaded.reduced_motion, "accessibility settings must survive a round trip");
    expect(!loaded.ore_glow, "ore glow must survive a settings round trip");
    std::filesystem::remove(path);
}

void advanced_settings_have_live_adjustments()
{
    openminerz::Settings settings;
    const float original_scale = settings.ui_scale;
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::ui_scale, 1);
    expect(settings.ui_scale > original_scale, "UI scale must adjust live");
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::reduced_motion, 1);
    expect(settings.reduced_motion, "toggle settings must adjust live");
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::ore_glow, 1);
    expect(!settings.ore_glow &&
               openminerz::ui::setting_value(settings,
                                              openminerz::ui::SettingId::ore_glow) == "OFF",
           "ore glow must expose a readable live toggle");
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::texture_style, 1);
    expect(settings.texture_style == openminerz::AssetStyle::classic,
           "texture style must cycle through the restart-scoped content packs");
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::texture_style, 1);
    expect(settings.texture_style == openminerz::AssetStyle::classic_hd &&
               openminerz::ui::setting_value(settings,
                                              openminerz::ui::SettingId::texture_style) ==
                   "CLASSIC HD",
           "texture style must expose the locally generated Classic HD pack");
    const float original_flight_speed = settings.creative_flight_speed;
    openminerz::ui::adjust_setting(settings, openminerz::ui::SettingId::creative_flight_speed, 1);
    expect(settings.creative_flight_speed == original_flight_speed + 1.0F &&
               openminerz::ui::setting_value(
                   settings, openminerz::ui::SettingId::creative_flight_speed) == "9 M/S",
           "Creative flight speed must expose a readable live slider");
    expect(openminerz::ui::rows_for_tab(openminerz::ui::SettingsTab::graphics).size() >= 8U,
           "graphics settings must expose the advanced control surface");
    expect(openminerz::ui::rows_for_tab(openminerz::ui::SettingsTab::accessibility).size() >= 5U,
           "accessibility must be a first-class category");
}

void main_menu_routes_without_scrolling_or_dead_actions()
{
    openminerz::Settings settings;
    openminerz::ui::MenuModel menu;
    expect(menu.screen() == openminerz::ui::MenuScreen::main && menu.selection() == 0U &&
               menu.entries().size() == 6U,
           "main menu must start on a playable action and keep every option visible");
    auto event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::play_offline &&
               menu.screen() == openminerz::ui::MenuScreen::game_mode,
           "solo activation must open the game-mode screen first");
    // Original flow: Survival -> difficulty screen with the cursor on NORMAL;
    // picking a difficulty starts the world.
    menu.set_selection(1U, settings);
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::select_mode_survival &&
               menu.screen() == openminerz::ui::MenuScreen::difficulty && menu.selection() == 2U,
           "survival must open the difficulty screen defaulting to NORMAL");
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::select_difficulty_normal &&
               menu.screen() == openminerz::ui::MenuScreen::world_select,
           "picking a difficulty must open the world list");
    // The world list is runtime data: NEW WORLD first, then saves, then BACK.
    menu.set_world_entries(
        {{"NEW WORLD", "Fresh seed.", openminerz::ui::MenuCommand::create_new_world, true, {}},
         {"BACK", "Back.", openminerz::ui::MenuCommand::back, true, {}}});
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::create_new_world &&
               menu.screen() == openminerz::ui::MenuScreen::world_select,
           "creating a world stays on the list until the app enters the world");
    menu.open(openminerz::ui::MenuScreen::world_preview);
    event = menu.back();
    expect(event.command == openminerz::ui::MenuCommand::back &&
               menu.screen() == openminerz::ui::MenuScreen::main,
           "world preview back must return to the main menu");
    menu.select_next(-1, settings);
    expect(menu.selection() == menu.entries().size() - 1U,
           "linear navigation must wrap from Play Solo to Quit");
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::request_exit &&
               menu.screen() == openminerz::ui::MenuScreen::exit_confirmation,
           "Exit must open a confirmation screen");
    event = menu.back();
    expect(event.command == openminerz::ui::MenuCommand::back &&
               menu.screen() == openminerz::ui::MenuScreen::main,
           "modal back must return to the main menu");
}

void settings_navigation_is_bounded_across_tabs()
{
    openminerz::Settings settings;
    openminerz::ui::MenuModel menu;
    menu.open(openminerz::ui::MenuScreen::settings);
    menu.select_next(-1, settings);
    expect(menu.selection() == openminerz::ui::rows_for_tab(menu.settings_tab()).size() - 1U,
           "settings rows must wrap");
    menu.change_tab(1, settings);
    expect(menu.settings_tab() == openminerz::ui::SettingsTab::graphics && menu.selection() == 0U,
           "tab changes must reset to a valid row");
}

void host_menu_exposes_full_preflight_and_preserves_flow_origin()
{
    openminerz::Settings settings;
    openminerz::ui::MenuModel menu;
    menu.set_selection(1U, settings);
    auto event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::open_multiplayer &&
               menu.screen() == openminerz::ui::MenuScreen::multiplayer &&
               menu.entries().size() == 3U,
           "the main menu must group hosting and joining in one focused multiplayer hub");
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::host_online &&
               menu.screen() == openminerz::ui::MenuScreen::host_setup &&
               menu.entries().size() == 6U,
           "Host Online must open a categorized pre-host settings overview");
    expect(menu.entries()[0].command == openminerz::ui::MenuCommand::open_host_details &&
               menu.entries()[1].command == openminerz::ui::MenuCommand::open_host_access &&
               menu.entries()[2].command == openminerz::ui::MenuCommand::open_host_gameplay &&
               menu.entries()[3].command == openminerz::ui::MenuCommand::review_host_mods,
           "pre-host setup must group details, access, safety, and the mod playlist");
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::open_host_details &&
               menu.screen() == openminerz::ui::MenuScreen::host_details &&
               menu.entries().size() == 4U &&
               menu.entries()[0].command == openminerz::ui::MenuCommand::edit_host_name &&
               menu.entries()[2].command == openminerz::ui::MenuCommand::select_host_max_players,
           "server details must expose identity, compatibility, and capacity without scrolling");
    event = menu.back();
    expect(menu.screen() == openminerz::ui::MenuScreen::host_setup,
           "server-detail back must retain the hosting overview");
    menu.set_selection(1U, settings);
    event = menu.activate(settings);
    expect(menu.screen() == openminerz::ui::MenuScreen::host_access &&
               menu.entries()[0].command == openminerz::ui::MenuCommand::select_host_visibility &&
               menu.entries()[1].command == openminerz::ui::MenuCommand::edit_host_password,
           "access settings must keep visibility and password controls together");
    event = menu.back();
    menu.set_selection(2U, settings);
    event = menu.activate(settings);
    expect(
        menu.screen() == openminerz::ui::MenuScreen::host_gameplay && menu.entries().size() == 5U &&
            menu.entries()[3].command == openminerz::ui::MenuCommand::select_host_spawn_protection,
        "rules and safety must expose all authority controls without scrolling");
    event = menu.back();
    menu.set_selection(4U, settings);
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::confirm_host_settings &&
               menu.screen() == openminerz::ui::MenuScreen::game_mode,
           "validated host setup must continue into the original game-mode flow");
    event = menu.back();
    expect(event.command == openminerz::ui::MenuCommand::back &&
               menu.screen() == openminerz::ui::MenuScreen::host_setup,
           "backing out of host game-mode selection must retain configured host settings");
    event = menu.back();
    expect(menu.screen() == openminerz::ui::MenuScreen::multiplayer,
           "host setup must return to the multiplayer role chooser");
}

void join_menu_routes_browser_details_and_password_actions()
{
    openminerz::Settings settings;
    openminerz::ui::MenuModel menu;
    menu.set_selection(1U, settings);
    auto event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::open_multiplayer &&
               menu.screen() == openminerz::ui::MenuScreen::multiplayer,
           "online play must open the multiplayer hub before choosing a role");
    menu.set_selection(1U, settings);
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::join_online &&
               menu.screen() == openminerz::ui::MenuScreen::join_browser,
           "Join Online must open the real session browser instead of a placeholder toast");

    const std::vector<std::string> labels{"FRIEND WORLD"};
    const std::vector<std::string> descriptions{"HOST MINER  /  CLASSIC V3"};
    menu.set_join_entries({{labels[0], descriptions[0],
                            openminerz::ui::MenuCommand::select_join_session, true, "COMPATIBLE"},
                           {"REFRESH", "Refresh discovery.",
                            openminerz::ui::MenuCommand::refresh_join_sessions, true, "F5"},
                           {"BACK", "Return.", openminerz::ui::MenuCommand::back, true, {}}});
    event = menu.activate(settings);
    expect(event.command == openminerz::ui::MenuCommand::select_join_session &&
               !event.screen_changed,
           "a browser row must let the app bind the selected discovery record before navigation");
    menu.open(openminerz::ui::MenuScreen::join_details);
    expect(menu.entries().size() == 3U &&
               menu.entries()[0].command == openminerz::ui::MenuCommand::connect_join_session &&
               menu.entries()[1].command == openminerz::ui::MenuCommand::edit_join_password,
           "session details must expose bounded admission and password-safe editing actions");
    event = menu.back();
    expect(event.command == openminerz::ui::MenuCommand::back &&
               menu.screen() == openminerz::ui::MenuScreen::join_browser,
           "session detail back must preserve the discovered browser result set");
    event = menu.back();
    expect(menu.screen() == openminerz::ui::MenuScreen::multiplayer,
           "browser back must return to the multiplayer role chooser");
    event = menu.back();
    expect(menu.screen() == openminerz::ui::MenuScreen::main,
           "the multiplayer role chooser must return to the main menu");
}

void network_protocol_manifest_and_framing_match_deployed_profiles()
{
    using namespace openminerz::net;
    const auto classic = message_manifest(ProtocolVersion::classic_v3);
    const auto modern = message_manifest(ProtocolVersion::modern_v4);
    expect(classic.size() == 53U && modern.size() == 55U,
           "v3/v4 message manifests must retain their deployed reflected sizes");
    expect(classic[0].name == "AddExplosionEffectsMessage" &&
               classic[50].name == "WorldInfoMessage" && classic[51].name == "ChatMessage" &&
               modern[11].name == "DestroyCustomBlockMessage" &&
               modern[51].name == "UpdateSpawnerMessage" && modern[54].name == "VoiceChatMessage",
           "message ids must match the exact Classic and Modern assembly registries");
    expect(
        message_by_name(ProtocolVersion::classic_v3, "PlayerUpdateMessage")->transport.delivery ==
                MessageDeliveryPolicy::player_update_state &&
            !message_by_name(ProtocolVersion::classic_v3, "PlayerUpdateMessage")->transport.echo &&
            message_by_name(ProtocolVersion::modern_v4, "ProvideChunkMessage")
                    ->transport.delivery == MessageDeliveryPolicy::reliable_unordered,
        "stock reliability and echo overrides must survive the C++ port");
    expect(player_routed_message("PlayerUpdateMessage") &&
               player_routed_message("DigMessage") &&
               player_routed_message("GunshotMessage") &&
               !player_routed_message("UpdateDragonMessage") &&
               !player_routed_message("SpawnEnemyMessage") &&
               !player_routed_message("CreatePickupMessage"),
           "pre-presence ordering must preserve the original player/global dispatch domains");

    PacketError error{};
    constexpr std::array<std::uint8_t, 4U> payload{0x03U, 0x43U, 0x4DU, 0x5AU};
    const auto encoded = encode_packet(ProtocolVersion::classic_v3, 51U, payload, error);
    const std::vector<std::uint8_t> expected{0x33U, 0x03U, 0x43U, 0x4DU, 0x5AU, 0x64U};
    expect(encoded.has_value() && *encoded == expected && error == PacketError::none,
           "application packets must use message-id + payload + original XOR8 framing");
    DecodedPacket decoded;
    expect(decode_packet(ProtocolVersion::classic_v3, *encoded, decoded, error) &&
               decoded.message != nullptr && decoded.message->name == "ChatMessage" &&
               std::equal(decoded.payload.begin(), decoded.payload.end(), payload.begin()),
           "bounded decoder must return the manifest entry and payload without copying");

    std::vector<std::uint8_t> oversized(maximum_packet_size + 1U);
    expect(!decode_packet(ProtocolVersion::classic_v3, oversized, decoded, error) &&
               error == PacketError::too_large,
           "wire frames above 4096 bytes must fail before payload parsing");
    auto damaged = *encoded;
    damaged.back() ^= 0xFFU;
    expect(!decode_packet(ProtocolVersion::classic_v3, damaged, decoded, error) &&
               error == PacketError::checksum_mismatch,
           "corrupt stock frames must fail closed");

    BoundedWriter writer{32U};
    expect(writer.write_u32(0x78563412U) && writer.write_dotnet_string("CMZ", 8U),
           "bounded writer must emit little-endian integers and BinaryWriter strings");
    BoundedReader reader{writer.bytes()};
    std::uint32_t integer{};
    std::string text;
    expect(reader.read_u32(integer) && integer == 0x78563412U &&
               reader.read_dotnet_string(text, 8U) && text == "CMZ" && reader.exhausted(),
           "bounded reader must round-trip explicit primitives exactly");
    constexpr std::array<std::uint8_t, 5U> unterminated_length{0x80U, 0x80U, 0x80U, 0x80U, 0x80U};
    BoundedReader malformed{unterminated_length};
    expect(!malformed.read_dotnet_string(text, 8U) && malformed.position() == 0U,
           "malformed 7-bit lengths must fail transactionally without advancing the reader");
}

void mod_handshake_is_ordered_strict_and_stock_client_safe()
{
    using namespace openminerz::net;
    std::array<std::uint8_t, 32U> behavior_digest{};
    std::array<std::uint8_t, 32U> core_digest{};
    behavior_digest.fill(0x31U);
    core_digest.fill(0xA7U);

    ModManifest host{ProtocolVersion::modern_v4,
                     {{"local.texture-pack", "9", {}, ModScope::client},
                      {"openminerz.entities", "2.1.0", behavior_digest, ModScope::server},
                      {"openminerz.core.modern", "4", core_digest, ModScope::core}}};
    ModManifest peer = host;
    peer.mods.insert(peer.mods.begin() + 1, {"client.hud", "1.4", {}, ModScope::client});
    const auto compatible = compare_mod_manifests(host, &peer, true);
    expect(compatible.compatible(),
           "client-only mods must not participate in synchronized admission");

    ExtensionMessage hello{ExtensionMessageKind::client_hello, 0x1122334455667788ULL, peer, {}};
    const std::vector<std::uint8_t> wire = encode_extension_message(hello);
    expect(is_extension_packet(wire) && wire.size() <= maximum_extension_packet_size,
           "OpenMinerZ negotiation must use a bounded, recognizable control frame");
    ExtensionMessage decoded;
    ExtensionDecodeError decode_error{};
    expect(decode_extension_message(wire, decoded, decode_error) && decoded.nonce == hello.nonce &&
               decoded.manifest.mods.size() == 2U &&
               decoded.manifest.mods[0].id == "openminerz.entities" &&
               decoded.manifest.mods[1].scope == ModScope::core,
           "extension hello must preserve synchronized packet order and omit client-only mods");

    auto corrupt = wire;
    corrupt[12] ^= 0x40U;
    expect(!decode_extension_message(corrupt, decoded, decode_error) &&
               decode_error == ExtensionDecodeError::checksum_mismatch,
           "corrupt extension frames must be rejected before manifest admission");
    expect(compare_mod_manifests(host, nullptr, false).status ==
               ModCompatibility::extension_required,
           "a stock client must be denied before gameplay when server/core mods are required");

    ModManifest vanilla{ProtocolVersion::classic_v3, {}};
    expect(compare_mod_manifests(vanilla, nullptr, false).compatible(),
           "a capability-silent stock client must remain eligible for a vanilla host");

    std::swap(peer.mods[2], peer.mods[3]);
    expect(compare_mod_manifests(host, &peer, true).status == ModCompatibility::order_mismatch,
           "the same synchronized mods in a different load order must be rejected");
    peer = host;
    peer.mods[1].content_digest[0] ^= 1U;
    expect(compare_mod_manifests(host, &peer, true).status == ModCompatibility::digest_mismatch,
           "matching names and versions must not hide different synchronized content");
}

void mod_catalog_hashes_content_and_selects_ordered_profiles()
{
    using namespace openminerz::net;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("openminerz-mod-catalog-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto write_mod = [&](std::string_view directory, std::string_view descriptor,
                               std::string_view content) {
        const std::filesystem::path path = root / directory;
        std::filesystem::create_directories(path / "content");
        std::ofstream{path / "openminerz.mod", std::ios::binary} << descriptor;
        std::ofstream{path / "content" / "payload.bin", std::ios::binary} << content;
    };
    write_mod("client-hud", "id=local.hud\nversion=1.0\nscope=client\nprotocol=classic\norder=5\n",
              "cosmetic");
    write_mod("modern-core",
              "id=openminerz.core.modern\nversion=4\nscope=core\nprotocol=modern\norder=10\n",
              "core-state");
    write_mod("entities",
              "id=openminerz.entities\nversion=2.1\nscope=server\nprotocol=both\norder=20\n",
              "entity-state");

    ModCatalog catalog;
    std::string error;
    expect(load_mod_catalog(root, catalog, error) && catalog.mods.size() == 3U,
           "manifest-backed add-ons must load from deterministic immediate child roots");
    ModManifest classic;
    ModManifest modern;
    expect(make_mod_manifest(catalog, ProtocolVersion::classic_v3, classic, error) &&
               make_mod_manifest(catalog, ProtocolVersion::modern_v4, modern, error) &&
               classic.mods.size() == 2U && classic.mods[0].id == "local.hud" &&
               classic.mods[1].id == "openminerz.entities" && modern.mods.size() == 2U &&
               modern.mods[0].id == "openminerz.core.modern" &&
               modern.mods[1].id == "openminerz.entities" && requires_extension(classic) &&
               requires_extension(modern),
           "profile selection must preserve explicit load order while retaining local client mods");
    expect(std::any_of(classic.mods[1].content_digest.begin(), classic.mods[1].content_digest.end(),
                       [](std::uint8_t value) { return value != 0U; }),
           "synchronized catalog entries must carry a real nonzero SHA-256 content digest");

    const auto original_digest = classic.mods[1].content_digest;
    std::ofstream{root / "entities" / "content" / "payload.bin", std::ios::binary | std::ios::trunc}
        << "entity-state-changed";
    ModCatalog changed;
    expect(load_mod_catalog(root, changed, error) &&
               make_mod_manifest(changed, ProtocolVersion::classic_v3, classic, error) &&
               classic.mods[1].content_digest != original_digest,
           "changing one content byte must change the synchronized catalog digest");

    write_mod("broken", "id=bad\nversion=1\nscope=server\nprotocol=both\n", "bad");
    expect(!load_mod_catalog(root, changed, error) && !error.empty(),
           "a malformed descriptor must block the entire catalog before partial activation");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

void host_session_settings_publish_original_session_properties()
{
    using namespace openminerz::net;
    HostSessionSettings settings;
    std::string error;
    expect(settings.validate(error), "default pre-host settings must be valid");
    const SessionProperties properties = make_session_properties(settings, false, 1, 2, false);
    expect(properties[0] == 3 && properties[1] == 0 && properties[2] == 1 && properties[3] == 2 &&
               properties[4] == 0 && properties[5] == 0 && !properties[6].has_value() &&
               !properties[7].has_value(),
           "host discovery must publish the original eight nullable property slots");
    settings.maximum_players = 17;
    expect(!settings.validate(error),
           "stock compatibility must cap a peer host at sixteen players");
    settings.maximum_players = 8;
    settings.password.assign(257U, 'x');
    expect(!settings.validate(error), "pre-host passwords must be bounded before transport use");
}

void session_browser_preflights_discovery_and_feeds_the_admission_gate()
{
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.server_name = "FRIEND FRONTIER";
    settings.server_message = "WELCOME";
    settings.password = "ore";
    settings.visibility = HostVisibility::friends_only;
    DiscoveredSession session;
    session.session_id = 0x1234U;
    session.server_name = settings.server_name;
    session.host_name = "MINER";
    session.server_message = settings.server_message;
    session.source = DiscoverySource::friend_session;
    session.properties = make_session_properties(settings, false, 1, 2, false);
    session.current_players = 1U;
    session.maximum_players = 8U;
    session.password_required = true;
    session.ping_milliseconds = 24U;

    ModManifest client;
    client.protocol = ProtocolVersion::classic_v3;
    const JoinAssessment assessment = assess_join(session, client, true);
    expect(assessment.joinable() && assessment.password_required &&
               assessment.protocol == ProtocolVersion::classic_v3,
           "a valid friend lobby must expose its profile and password preflight without guessing");

    HostAdmissionGate gate{settings, false, 1, 2, false};
    const PeerAdmissionRequest correct = make_peer_admission_request(session, 7U, "ore", true);
    expect(gate.begin(correct).status == AdmissionStatus::admitted &&
               gate.may_forward_gameplay(correct.peer),
           "browser-selected metadata must feed the same exact property/password admission gate");
    const PeerAdmissionRequest wrong = make_peer_admission_request(session, 8U, "coal", true);
    expect(gate.begin(wrong).status == AdmissionStatus::rejected_password,
           "browser preflight must not bypass the host's constant-time password decision");

    session.current_players = session.maximum_players;
    expect(assess_join(session, client, true).status == JoinCompatibility::full,
           "full lobbies must be visible but non-joinable before transport mutation");
    session.current_players = 1U;
    session.properties[7] = 1;
    expect(assess_join(session, client, true).status == JoinCompatibility::malformed_advertisement,
           "non-null reserved original session properties must fail closed in discovery");
    session.properties[7] = std::nullopt;

    std::array<std::uint8_t, 32U> digest{};
    digest[0] = 0xA5U;
    settings.mod_manifest.mods.push_back({"openminerz.core", "1", digest, ModScope::core});
    session.required_manifest = settings.mod_manifest;
    session.advertises_extension = true;
    client.mods = settings.mod_manifest.mods;
    expect(assess_join(session, client, true).joinable() &&
               assess_join(session, client, false).status == JoinCompatibility::extension_required,
           "ordered synchronized mods must require and accept only the advertised extension path");
    client.mods[0].content_digest[0] ^= 1U;
    expect(assess_join(session, client, true).status == JoinCompatibility::mod_mismatch,
           "discovery details must surface digest mismatches before any gameplay channel opens");

    DirectEndpoint endpoint;
    std::string endpoint_error;
    expect(parse_direct_endpoint("example.test:27018", endpoint, endpoint_error) &&
               endpoint == DirectEndpoint{"example.test", 27018U} &&
               parse_direct_endpoint("[::1]:25343", endpoint, endpoint_error) &&
               endpoint == DirectEndpoint{"::1", 25343U},
           "direct connect must parse bounded DNS and bracketed IPv6 endpoints deterministically");
    expect(!parse_direct_endpoint("host:0", endpoint, endpoint_error) &&
               !parse_direct_endpoint("host:999999999999999999999", endpoint, endpoint_error) &&
               !parse_direct_endpoint("bad host:123", endpoint, endpoint_error),
           "invalid, overflowing, and whitespace-bearing direct endpoints must fail closed");

    session.required_manifest.reset();
    session.advertises_extension = false;
    client.mods.clear();
    SessionBrowser browser;
    browser.set_client(client, true);
    DiscoveredSession public_session = session;
    public_session.session_id = 2U;
    public_session.server_name = "PUBLIC MINE";
    public_session.source = DiscoverySource::public_lobby;
    public_session.ping_milliseconds = 10U;
    DiscoveredSession favorite = session;
    favorite.session_id = 3U;
    favorite.server_name = "FAVORITE CAVE";
    favorite.favorite = true;
    favorite.ping_milliseconds = 80U;
    DiscoveredSession duplicate = favorite;
    browser.replace({public_session, favorite, duplicate, {}});
    expect(browser.sessions().size() == 2U && browser.visible_indices().size() == 2U &&
               browser.sessions()[browser.visible_indices()[0]].favorite,
           "browser refresh must drop zero/duplicate IDs and sort favorites before ping");
    SessionBrowserOptions options;
    options.filter = DiscoveryFilter::public_lobbies;
    options.query = "mine";
    browser.set_options(options);
    expect(browser.visible_indices().size() == 1U &&
               browser.sessions()[browser.visible_indices()[0]].session_id == 2U,
           "source tabs and case-insensitive search must rebuild one stable visible index list");
}

void admission_gate_never_forwards_before_authentication_and_mod_sync()
{
    using namespace openminerz::net;
    HostSessionSettings vanilla;
    vanilla.maximum_players = 2;
    vanilla.password = "frontier";
    HostAdmissionGate gate{vanilla, false, 1, 1, false};

    PeerAdmissionRequest wrong_password;
    wrong_password.peer = 10U;
    wrong_password.properties = gate.advertised_properties();
    wrong_password.password = "wrong";
    expect(gate.begin(wrong_password).status == AdmissionStatus::rejected_password &&
               !gate.may_forward_gameplay(wrong_password.peer) && gate.reserved_peer_count() == 0U,
           "wrong-password attempts must fail without consuming a player slot");

    PeerAdmissionRequest stock = wrong_password;
    stock.peer = 11U;
    stock.password = "frontier";
    expect(gate.begin(stock).status == AdmissionStatus::admitted &&
               gate.may_forward_gameplay(stock.peer) && gate.admitted_peer_count() == 1U,
           "a matching stock peer must enter a vanilla session without an extension packet");
    PeerAdmissionRequest full = stock;
    full.peer = 12U;
    expect(gate.begin(full).status == AdmissionStatus::rejected_full,
           "the local host slot plus admitted peers must enforce the configured total limit");
    gate.disconnect(stock.peer);
    expect(gate.reserved_peer_count() == 0U,
           "disconnect must release the reservation for reconnect and late join");

    std::array<std::uint8_t, 32U> digest{};
    digest.fill(0x42U);
    HostSessionSettings modded;
    modded.maximum_players = 4;
    modded.mod_manifest.mods.push_back({"openminerz.entities", "1.0", digest, ModScope::server});
    HostAdmissionGate mod_gate{modded, false, 1, 1, false};
    PeerAdmissionRequest legacy;
    legacy.peer = 20U;
    legacy.properties = mod_gate.advertised_properties();
    expect(mod_gate.begin(legacy).status == AdmissionStatus::rejected_extension_required &&
               !mod_gate.may_forward_gameplay(legacy.peer),
           "a modded host must reject a capability-silent original client before gameplay");

    PeerAdmissionRequest openminer = legacy;
    openminer.peer = 21U;
    openminer.advertised_extension = true;
    expect(mod_gate.begin(openminer).status == AdmissionStatus::awaiting_extension &&
               !mod_gate.may_forward_gameplay(openminer.peer),
           "capability advertisement reserves a slot but cannot authorize gameplay by itself");
    ModManifest wrong_manifest{ProtocolVersion::classic_v3, {}};
    ExtensionMessage wrong_hello{ExtensionMessageKind::client_hello, 77U, wrong_manifest, {}};
    const auto rejected =
        mod_gate.receive_extension(openminer.peer, encode_extension_message(wrong_hello));
    ExtensionMessage rejection_reply;
    ExtensionDecodeError extension_error{};
    expect(rejected.decision.status == AdmissionStatus::rejected_mod_manifest &&
               !mod_gate.may_forward_gameplay(openminer.peer) &&
               decode_extension_message(rejected.reply, rejection_reply, extension_error) &&
               rejection_reply.kind == ExtensionMessageKind::server_reject &&
               rejection_reply.nonce == 77U,
           "manifest mismatch must return a bounded correlated rejection and remain fail-closed");

    openminer.peer = 22U;
    expect(mod_gate.begin(openminer).status == AdmissionStatus::awaiting_extension,
           "a second valid extension peer must reach the isolated handshake state");
    ExtensionMessage correct_hello{
        ExtensionMessageKind::client_hello, 88U, modded.mod_manifest, {}};
    const auto accepted =
        mod_gate.receive_extension(openminer.peer, encode_extension_message(correct_hello));
    ExtensionMessage acceptance_reply;
    expect(accepted.decision.status == AdmissionStatus::admitted &&
               mod_gate.may_forward_gameplay(openminer.peer) &&
               decode_extension_message(accepted.reply, acceptance_reply, extension_error) &&
               acceptance_reply.kind == ExtensionMessageKind::server_accept &&
               acceptance_reply.nonce == 88U,
           "only an exact ordered manifest may open the gameplay forwarding gate");
}

void bounded_loopback_transport_rejects_native_envelope_abuse()
{
    using namespace openminerz::net;
    BoundedDatagramQueue queue{2U, 8U, 6U};
    QueueError error{};
    constexpr std::array<std::uint8_t, 5U> first{1U, 2U, 3U, 4U, 5U};
    constexpr std::array<std::uint8_t, 4U> second{6U, 7U, 8U, 9U};
    expect(queue.push(1U, 2U, first, MessageDeliveryPolicy::reliable_ordered,
                      stock_gameplay_channel, error) &&
               queue.buffered_bytes() == first.size(),
           "loopback must retain bounded datagrams with their delivery metadata");
    expect(!queue.push(2U, 1U, second, MessageDeliveryPolicy::chat, openminerz_control_channel,
                       error) &&
               error == QueueError::byte_budget_exhausted,
           "aggregate queue limits must be checked before copying another payload");
    const auto popped = queue.pop();
    expect(popped.has_value() && popped->sender == 1U && popped->recipient == 2U &&
               popped->payload == std::vector<std::uint8_t>(first.begin(), first.end()) &&
               queue.buffered_bytes() == 0U,
           "loopback pop must preserve packet order and release its exact byte budget");
    expect(
        !queue.push(1U, 2U, {}, MessageDeliveryPolicy::unreliable, stock_gameplay_channel, error) &&
            error == QueueError::empty_payload,
        "empty native datagrams must never enter application dispatch");

    expect(validate_advertised_length(0U) == NativeEnvelopeError::invalid_advertised_length &&
               validate_advertised_length(maximum_packet_size + 1U) ==
                   NativeEnvelopeError::invalid_advertised_length &&
               validate_advertised_length(maximum_packet_size) == NativeEnvelopeError::none,
           "native packet allocation must reject zero/oversized lengths before reading");
    expect(validate_native_read(12U, false, 0U, true) == NativeEnvelopeError::read_failed &&
               validate_native_read(12U, true, 11U, true) == NativeEnvelopeError::length_mismatch &&
               validate_native_read(12U, true, 12U, false) ==
                   NativeEnvelopeError::sender_not_session_member &&
               validate_native_read(12U, true, 12U, true) == NativeEnvelopeError::none,
           "native reads must require success, exact length, and authenticated lobby membership");
}

void typed_bootstrap_codecs_match_original_layouts_and_reject_hostile_values()
{
    using namespace openminerz::net;
    PayloadError error{};
    const auto chat = encode_chat_payload("CMZ", error);
    expect(chat.has_value() && *chat == std::vector<std::uint8_t>({3U, 'C', 'M', 'Z'}),
           "chat must use BinaryWriter's 7-bit UTF-8 byte length prefix");
    std::string chat_text;
    expect(decode_chat_payload(*chat, chat_text, error) && chat_text == "CMZ",
           "chat payload must round-trip through the bounded source-compatible codec");
    const auto day = encode_time_of_day_payload(0.5F, error);
    float decoded_day{};
    expect(day.has_value() && *day == bytes_from_hex("0000003F") &&
               decode_time_of_day_payload(*day, decoded_day, error) && decoded_day == 0.5F,
           "TimeOfDay must preserve the original full-day IEEE single payload");
    const auto connected = encode_time_connected_payload(100'000'000LL, error);
    std::int64_t decoded_ticks{};
    expect(connected.has_value() && *connected == bytes_from_hex("00E1F50500000000") &&
               decode_time_connected_payload(*connected, decoded_ticks, error) &&
               decoded_ticks == 100'000'000LL,
           "TimeConnected must preserve the original nonnegative TimeSpan tick payload");
    expect(!encode_time_connected_payload(-1, error).has_value() &&
               error == PayloadError::out_of_range,
           "negative connected time must fail before peer state mutation");

    PlayerExistsPayload presence;
    presence.request_response = true;
    presence.avatar_description = {1U, 2U, 3U, 4U};
    presence.gamer_tag = "PLAYER";
    presence.player_id = {9U, 8U, 7U};
    const auto encoded_presence = encode_player_exists_payload(presence, error);
    PlayerExistsPayload decoded_presence;
    expect(
        encoded_presence.has_value() &&
            decode_player_exists_payload(*encoded_presence, decoded_presence, error) &&
            decoded_presence == presence,
        "PlayerExists must preserve boolean, bounded avatar blob, gamertag, and player ID order");
    BoundedWriter bad_presence_writer{32U};
    expect(bad_presence_writer.write_bool(false) && bad_presence_writer.write_i32(-1),
           "malformed presence fixture must be constructible");
    expect(!decode_player_exists_payload(bad_presence_writer.bytes(), decoded_presence, error) &&
               error == PayloadError::out_of_range,
           "negative attacker-controlled avatar lengths must fail before allocation");

    PlayerUpdatePayload update;
    update.local_position = {8.5F, 128.0F, -8.5F};
    update.torso_pitch = 64U;
    update.yaw = 128U;
    update.state_flags = PlayerUpdatePayload::shouldering_flag;
    update.player_mode = 16U;
    update.packed_movement = 0xE7U;
    update.velocity_x = 0x3C00U;
    update.velocity_y = 0x0000U;
    update.velocity_z = 0xBC00U;
    const auto encoded_update =
        encode_player_update_payload(update, ProtocolVersion::classic_v3, error);
    PlayerUpdatePayload decoded_update;
    expect(encoded_update.has_value() && encoded_update->size() == 23U &&
               decode_player_update_payload(*encoded_update, ProtocolVersion::classic_v3,
                                            decoded_update, error) &&
               decoded_update == update,
           "PlayerUpdate must retain the original fixed 23-byte packed layout");
    update.local_position.x = std::numeric_limits<float>::quiet_NaN();
    expect(!encode_player_update_payload(update, ProtocolVersion::classic_v3, error).has_value() &&
               error == PayloadError::non_finite,
           "NaN positions must never reach player simulation");
    update.local_position.x = 8.5F;
    update.player_mode = 17U;
    expect(!encode_player_update_payload(update, ProtocolVersion::classic_v3, error).has_value() &&
               encode_player_update_payload(update, ProtocolVersion::modern_v4, error).has_value(),
           "Classic and Modern player-mode ordinals must enforce their distinct 16/18 maxima");

    RequestChunkPayload request{{-32, 0, 48}, 7U};
    const auto request_bytes = encode_request_chunk_payload(request, error);
    RequestChunkPayload decoded_request;
    expect(request_bytes.has_value() && request_bytes->size() == 13U &&
               decode_request_chunk_payload(*request_bytes, decoded_request, error) &&
               decoded_request == request,
           "chunk requests must be exactly IntVector3 followed by one priority byte");
    ProvideChunkPayload provided{{-32, 0, 48}, 7U, {1, -2, 3, 4}};
    const auto provided_bytes = encode_provide_chunk_payload(provided, error);
    ProvideChunkPayload decoded_provided;
    expect(provided_bytes.has_value() &&
               decode_provide_chunk_payload(*provided_bytes, decoded_provided, error) &&
               decoded_provided == provided,
           "chunk replies must preserve position, priority, count, and signed delta words");
    auto truncated = *provided_bytes;
    truncated.pop_back();
    expect(!decode_provide_chunk_payload(truncated, decoded_provided, error),
           "truncated chunk delta arrays must fail without partial world mutation");

    const MessageDescriptor *dragon_attack =
        message_by_name(ProtocolVersion::classic_v3, "DragonAttackMessage");
    expect(dragon_attack != nullptr &&
               !validate_known_payload(ProtocolVersion::classic_v3, *dragon_attack, {}, error) &&
               error == PayloadError::malformed,
           "registered typed schemas must reject empty malformed payloads at dispatch");
}

void combat_codecs_match_source_layouts_profiles_and_hostile_bounds()
{
    using namespace openminerz::net;
    using namespace std::literals;
    PayloadError error{};

    AlterBlockPayload alter{{1, -2, 3}, 6};
    const auto alter_bytes = encode_alter_block_payload(alter, ProtocolVersion::classic_v3, error);
    AlterBlockPayload decoded_alter;
    expect(alter_bytes.has_value() &&
               *alter_bytes == bytes_from_hex("01000000FEFFFFFF0300000006000000") &&
               decode_alter_block_payload(*alter_bytes, ProtocolVersion::classic_v3, decoded_alter,
                                          error) &&
               decoded_alter == alter,
           "AlterBlock must remain IntVector3 followed by the profile-bounded Int32 block type");

    GunshotPayload gunshot{{1.0F, 2.0F, 3.0F}, 51};
    const auto gunshot_bytes = encode_gunshot_payload(gunshot, ProtocolVersion::classic_v3, error);
    GunshotPayload decoded_gunshot;
    expect(gunshot_bytes.has_value() && gunshot_bytes->size() == 14U &&
               decode_gunshot_payload(*gunshot_bytes, ProtocolVersion::classic_v3, decoded_gunshot,
                                      error) &&
               decoded_gunshot == gunshot,
           "Gunshot must preserve one finite direction then the exact Int16 firearm ID");

    ShotgunShotPayload shotgun;
    shotgun.directions = {{{1.0F, 0.0F, 0.0F},
                           {0.9F, 0.1F, 0.0F},
                           {0.9F, -0.1F, 0.0F},
                           {0.9F, 0.0F, 0.1F},
                           {0.9F, 0.0F, -0.1F}}};
    shotgun.item_id = 52;
    const auto shotgun_bytes =
        encode_shotgun_shot_payload(shotgun, ProtocolVersion::classic_v3, error);
    ShotgunShotPayload decoded_shotgun;
    expect(shotgun_bytes.has_value() && shotgun_bytes->size() == 62U &&
               decode_shotgun_shot_payload(*shotgun_bytes, ProtocolVersion::classic_v3,
                                           decoded_shotgun, error) &&
               decoded_shotgun == shotgun,
           "ShotgunShot must contain exactly five finite directions and one shotgun ID");

    MeleePlayerPayload melee{49, {4.0F, 5.0F, 6.0F}};
    const auto melee_bytes = encode_melee_player_payload(melee, ProtocolVersion::classic_v3, error);
    MeleePlayerPayload decoded_melee;
    expect(melee_bytes.has_value() && melee_bytes->size() == 14U &&
               decode_melee_player_payload(*melee_bytes, ProtocolVersion::classic_v3, decoded_melee,
                                           error) &&
               decoded_melee == melee,
           "MeleePlayer must retain item-before-damage-source field order");

    GrenadePayload grenade{{1.0F, 0.0F, 0.0F}, {8.0F, 128.0F, -8.0F}, 2U, 1.5F};
    const auto grenade_bytes = encode_grenade_payload(grenade, ProtocolVersion::classic_v3, error);
    GrenadePayload decoded_grenade;
    expect(grenade_bytes.has_value() && grenade_bytes->size() == 29U &&
               decode_grenade_payload(*grenade_bytes, ProtocolVersion::classic_v3, decoded_grenade,
                                      error) &&
               decoded_grenade == grenade,
           "Grenade must retain direction, position, profile type, and finite fuse order");

    FireRocketPayload rocket{{0.0F, 0.0F, -1.0F}, {8.0F, 128.0F, -8.0F}, true, 103};
    const auto rocket_bytes =
        encode_fire_rocket_payload(rocket, ProtocolVersion::classic_v3, error);
    FireRocketPayload decoded_rocket;
    expect(rocket_bytes.has_value() && rocket_bytes->size() == 27U &&
               decode_fire_rocket_payload(*rocket_bytes, ProtocolVersion::classic_v3,
                                          decoded_rocket, error) &&
               decoded_rocket == rocket,
           "FireRocket must encode its bool before the Int16 launcher type");

    DetonateGrenadePayload grenade_hit{{10.0F, 20.0F, 30.0F}, 1U, true};
    const auto grenade_hit_bytes =
        encode_detonate_grenade_payload(grenade_hit, ProtocolVersion::classic_v3, error);
    DetonateGrenadePayload decoded_grenade_hit;
    expect(grenade_hit_bytes.has_value() && grenade_hit_bytes->size() == 14U &&
               decode_detonate_grenade_payload(*grenade_hit_bytes, ProtocolVersion::classic_v3,
                                               decoded_grenade_hit, error) &&
               decoded_grenade_hit == grenade_hit,
           "DetonateGrenade must preserve location, grenade type, and ground flag");

    DetonateRocketPayload rocket_hit{{10.0F, 20.0F, 30.0F}, true, 4U, 103U};
    const auto rocket_hit_bytes =
        encode_detonate_rocket_payload(rocket_hit, ProtocolVersion::classic_v3, error);
    DetonateRocketPayload decoded_rocket_hit;
    expect(rocket_hit_bytes.has_value() && rocket_hit_bytes->size() == 15U &&
               decode_detonate_rocket_payload(*rocket_hit_bytes, ProtocolVersion::classic_v3,
                                              decoded_rocket_hit, error) &&
               decoded_rocket_hit == rocket_hit,
           "DetonateRocket must retain hit-dragon, explosive, and launcher bytes");

    DetonateExplosivePayload explosive{{17, 1, -1}, true, 4U};
    const auto explosive_bytes =
        encode_detonate_explosive_payload(explosive, ProtocolVersion::classic_v3, error);
    DetonateExplosivePayload decoded_explosive;
    expect(explosive_bytes.has_value() && explosive_bytes->size() == 14U &&
               decode_detonate_explosive_payload(*explosive_bytes, ProtocolVersion::classic_v3,
                                                 decoded_explosive, error) &&
               decoded_explosive == explosive,
           "DetonateExplosive must retain integer location, original flag, and type byte");

    const auto effect_bytes = encode_int_vector_payload({-16, 3, 32}, error);
    IntVector3 decoded_effect;
    expect(effect_bytes.has_value() && effect_bytes->size() == 12U &&
               decode_int_vector_payload(*effect_bytes, decoded_effect, error) &&
               decoded_effect == IntVector3{-16, 3, 32},
           "explosion effects, flash, and crate destruction share one exact IntVector3 layout");

    DigPayload dig{false, {1.0F, 2.0F, 3.0F}, {0.0F, -1.0F, 0.0F}, 6};
    const auto dig_bytes = encode_dig_payload(dig, ProtocolVersion::classic_v3, error);
    DigPayload decoded_dig;
    expect(dig_bytes.has_value() && dig_bytes->size() == 29U &&
               decode_dig_payload(*dig_bytes, ProtocolVersion::classic_v3, decoded_dig, error) &&
               decoded_dig == dig,
           "Dig must preserve placing, two finite vectors, and profile block type order");

    RemoveBlocksPayload removed{{{1, 2, 3}, {-4, 5, -6}}, true};
    const auto removed_bytes = encode_remove_blocks_payload(removed, error);
    RemoveBlocksPayload decoded_removed;
    expect(removed_bytes.has_value() && removed_bytes->size() == 29U &&
               decode_remove_blocks_payload(*removed_bytes, decoded_removed, error) &&
               decoded_removed == removed,
           "RemoveBlocks must bound its count before allocating and retain its final effects flag");

    CrateFocusPayload focus{{1, -2, 3}, 7, 4};
    const auto focus_bytes = encode_crate_focus_payload(focus, error);
    CrateFocusPayload decoded_focus;
    expect(focus_bytes.has_value() &&
               *focus_bytes == bytes_from_hex("01000000FEFFFFFF030000000700000004000000") &&
               decode_crate_focus_payload(*focus_bytes, decoded_focus, error) &&
               decoded_focus == focus,
           "CrateFocus must match the original location then X/Y point binary");

    DoorOpenClosePayload door{{10, 20, -30}, true};
    const auto door_bytes = encode_door_open_close_payload(door, error);
    DoorOpenClosePayload decoded_door;
    KickPayload kick{true, 7U};
    const auto kick_bytes = encode_kick_payload(kick, error);
    KickPayload decoded_kick;
    expect(door_bytes.has_value() && door_bytes->size() == 13U &&
               decode_door_open_close_payload(*door_bytes, decoded_door, error) &&
               decoded_door == door && kick_bytes.has_value() && kick_bytes->size() == 2U &&
               decode_kick_payload(*kick_bytes, decoded_kick, error) && decoded_kick == kick,
           "door and moderation boolean fields must use strict one-byte BinaryWriter booleans");

    const std::array validated_messages{
        std::pair{"AlterBlockMessage"sv, std::span<const std::uint8_t>{*alter_bytes}},
        std::pair{"GunshotMessage"sv, std::span<const std::uint8_t>{*gunshot_bytes}},
        std::pair{"ShotgunShotMessage"sv, std::span<const std::uint8_t>{*shotgun_bytes}},
        std::pair{"MeleePlayerMessage"sv, std::span<const std::uint8_t>{*melee_bytes}},
        std::pair{"GrenadeMessage"sv, std::span<const std::uint8_t>{*grenade_bytes}},
        std::pair{"FireRocketMessage"sv, std::span<const std::uint8_t>{*rocket_bytes}},
        std::pair{"DetonateGrenadeMessage"sv, std::span<const std::uint8_t>{*grenade_hit_bytes}},
        std::pair{"DetonateRocketMessage"sv, std::span<const std::uint8_t>{*rocket_hit_bytes}},
        std::pair{"DetonateExplosiveMessage"sv, std::span<const std::uint8_t>{*explosive_bytes}},
        std::pair{"AddExplosionEffectsMessage"sv, std::span<const std::uint8_t>{*effect_bytes}},
        std::pair{"DigMessage"sv, std::span<const std::uint8_t>{*dig_bytes}},
        std::pair{"RemoveBlocksMessage"sv, std::span<const std::uint8_t>{*removed_bytes}},
        std::pair{"CrateFocusMessage"sv, std::span<const std::uint8_t>{*focus_bytes}},
        std::pair{"DoorOpenCloseMessage"sv, std::span<const std::uint8_t>{*door_bytes}},
        std::pair{"KickMessage"sv, std::span<const std::uint8_t>{*kick_bytes}},
    };
    for (const auto &[name, bytes] : validated_messages)
    {
        const MessageDescriptor *descriptor = message_by_name(ProtocolVersion::classic_v3, name);
        expect(descriptor != nullptr &&
                   validate_known_payload(ProtocolVersion::classic_v3, *descriptor, bytes, error),
               "every ported combat schema must be reachable through fail-closed dispatch");
    }

    AlterBlockPayload modern_alter{{0, 0, 0}, 93};
    expect(encode_alter_block_payload(modern_alter, ProtocolVersion::modern_v4, error) &&
               !encode_alter_block_payload(modern_alter, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "Modern block 93 must never cross into the Classic profile");
    grenade.grenade_type = 3U;
    expect(encode_grenade_payload(grenade, ProtocolVersion::modern_v4, error) &&
               !encode_grenade_payload(grenade, ProtocolVersion::classic_v3, error),
           "Modern's fourth grenade type must remain core-profile gated");
    grenade.grenade_type = 0U;
    grenade.seconds_left = 5.01F;
    expect(!encode_grenade_payload(grenade, ProtocolVersion::modern_v4, error) &&
               error == PayloadError::out_of_range,
           "grenade launch fuses outside the recovered one-to-five-second interval must fail");
    explosive.explosive_type = 5U;
    expect(encode_detonate_explosive_payload(explosive, ProtocolVersion::modern_v4, error) &&
               !encode_detonate_explosive_payload(explosive, ProtocolVersion::classic_v3, error),
           "Modern's sixth explosive type must remain core-profile gated");

    gunshot.direction.x = std::numeric_limits<float>::quiet_NaN();
    expect(!encode_gunshot_payload(gunshot, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::non_finite,
           "NaN shot directions must stop before hitscan authority");
    shotgun.item_id = 51;
    expect(!encode_shotgun_shot_payload(shotgun, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "a pistol cannot forge the five-pellet shotgun message");
    BoundedWriter hostile_remove{8U};
    expect(hostile_remove.write_i32(static_cast<std::int32_t>(maximum_remove_blocks + 1U)) &&
               !decode_remove_blocks_payload(hostile_remove.bytes(), decoded_removed, error) &&
               error == PayloadError::out_of_range,
           "hostile RemoveBlocks counts must fail before vector allocation");
    auto corrupt_door = *door_bytes;
    corrupt_door.back() = 2U;
    expect(!decode_door_open_close_payload(corrupt_door, decoded_door, error),
           "non-boolean door state must fail before world mutation");
}

void pickup_codecs_preserve_variable_items_and_authority_fields()
{
    using namespace openminerz::net;
    PayloadError error{};

    RequestPickupPayload request{0x11223344, 2};
    const auto request_bytes = encode_request_pickup_payload(request, error);
    RequestPickupPayload decoded_request;
    expect(request_bytes.has_value() && *request_bytes == bytes_from_hex("4433221102000000") &&
               decode_request_pickup_payload(*request_bytes, decoded_request, error) &&
               decoded_request == request,
           "RequestPickup must write pickup ID before the bounded spawner ID");

    CreatePickupPayload created{
        {1.0F, 2.0F, 3.0F}, {-1.0F, 0.5F, 4.0F}, 7, InventoryItemPayload{2, 3, 0.5F}, true, false};
    const auto classic_created =
        encode_create_pickup_payload(created, ProtocolVersion::classic_v3, error);
    CreatePickupPayload decoded_created;
    expect(classic_created.has_value() && classic_created->size() == 37U &&
               classic_created->back() == 1U &&
               decode_create_pickup_payload(*classic_created, ProtocolVersion::classic_v3,
                                            decoded_created, error) &&
               decoded_created == created,
           "Classic CreatePickup must place its variable item immediately before dropped");

    created.display_on_pickup = true;
    const auto modern_created =
        encode_create_pickup_payload(created, ProtocolVersion::modern_v4, error);
    expect(modern_created.has_value() && modern_created->size() == 38U &&
               modern_created->at(modern_created->size() - 2U) == 1U &&
               modern_created->back() == 1U &&
               decode_create_pickup_payload(*modern_created, ProtocolVersion::modern_v4,
                                            decoded_created, error) &&
               decoded_created == created,
           "Modern CreatePickup must append display only after the dropped flag");

    ConsumePickupPayload consumed{
        {10.0F, 20.0F, 30.0F}, 9, 3, 4U, InventoryItemPayload{4, 1, 1.0F}, true};
    const auto consumed_bytes =
        encode_consume_pickup_payload(consumed, ProtocolVersion::modern_v4, error);
    ConsumePickupPayload decoded_consumed;
    expect(consumed_bytes.has_value() && consumed_bytes->size() == 30U &&
               consumed_bytes->at(21U) == 1U &&
               decode_consume_pickup_payload(*consumed_bytes, ProtocolVersion::modern_v4,
                                             decoded_consumed, error) &&
               decoded_consumed == consumed,
           "ConsumePickup must bind position, IDs, picker GID, Modern display, then typed item");

    const std::array dispatch_cases{
        std::pair{"RequestPickupMessage", std::span<const std::uint8_t>{*request_bytes}},
        std::pair{"CreatePickupMessage", std::span<const std::uint8_t>{*modern_created}},
        std::pair{"ConsumePickupMessage", std::span<const std::uint8_t>{*consumed_bytes}},
    };
    for (const auto &[name, bytes] : dispatch_cases)
    {
        const MessageDescriptor *descriptor = message_by_name(ProtocolVersion::modern_v4, name);
        expect(descriptor != nullptr &&
                   validate_known_payload(ProtocolVersion::modern_v4, *descriptor, bytes, error),
               "pickup codecs must be reachable through typed fail-closed dispatch");
    }

    consumed.pickup_position.x = std::numeric_limits<float>::infinity();
    expect(!encode_consume_pickup_payload(consumed, ProtocolVersion::modern_v4, error) &&
               error == PayloadError::non_finite,
           "a non-finite host pickup position must never reach client inventory application");
    request.spawner_id = 256;
    expect(!encode_request_pickup_payload(request, error) && error == PayloadError::out_of_range,
           "spawner IDs must fit the original byte-sized ownership domain");
    auto corrupt_create = *modern_created;
    corrupt_create.back() = 2U;
    expect(!decode_create_pickup_payload(corrupt_create, ProtocolVersion::modern_v4,
                                         decoded_created, error),
           "non-boolean Modern display tails must fail before pickup creation");
    auto truncated_consume = *consumed_bytes;
    truncated_consume.pop_back();
    expect(!decode_consume_pickup_payload(truncated_consume, ProtocolVersion::modern_v4,
                                          decoded_consumed, error),
           "truncated variable inventory items must fail without partial consumption");
}

void modern_and_voice_codecs_are_profile_gated_and_bounded()
{
    using namespace openminerz::net;
    PayloadError error{};

    DestroyCustomBlockPayload custom{{10, -20, 30}, 93U};
    const auto custom_bytes = encode_destroy_custom_block_payload(custom, error);
    DestroyCustomBlockPayload decoded_custom;
    expect(custom_bytes.has_value() && custom_bytes->size() == 13U &&
               decode_destroy_custom_block_payload(*custom_bytes, decoded_custom, error) &&
               decoded_custom == custom,
           "Modern DestroyCustomBlock must use IntVector3 then one bounded block byte");

    UpdateSpawnerPayload spawner{{1.0F, 2.0F, 3.0F}, true};
    const auto spawner_bytes = encode_update_spawner_payload(spawner, error);
    UpdateSpawnerPayload decoded_spawner;
    expect(spawner_bytes.has_value() && spawner_bytes->size() == 13U &&
               decode_update_spawner_payload(*spawner_bytes, decoded_spawner, error) &&
               decoded_spawner == spawner,
           "Modern UpdateSpawner must retain finite position then strict started boolean");

    const std::vector<std::uint8_t> audio{0x10U, 0x20U, 0x30U};
    const auto voice_bytes = encode_voice_chat_payload(audio, error);
    std::vector<std::uint8_t> decoded_audio;
    expect(voice_bytes.has_value() && *voice_bytes == bytes_from_hex("03000000102030") &&
               decode_voice_chat_payload(*voice_bytes, decoded_audio, error) &&
               decoded_audio == audio,
           "VoiceChat must prefix the exact byte count and consume the entire bounded frame");

    const MessageDescriptor *custom_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "DestroyCustomBlockMessage");
    const MessageDescriptor *spawner_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "UpdateSpawnerMessage");
    const MessageDescriptor *voice_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "VoiceChatMessage");
    expect(custom_descriptor != nullptr && spawner_descriptor != nullptr &&
               voice_descriptor != nullptr &&
               validate_known_payload(ProtocolVersion::modern_v4, *custom_descriptor, *custom_bytes,
                                      error) &&
               validate_known_payload(ProtocolVersion::modern_v4, *spawner_descriptor,
                                      *spawner_bytes, error) &&
               validate_known_payload(ProtocolVersion::modern_v4, *voice_descriptor, *voice_bytes,
                                      error),
           "Modern-only and voice schemas must enter dispatch only through their typed validators");

    custom.block_type = 94U;
    expect(!encode_destroy_custom_block_payload(custom, error) &&
               error == PayloadError::out_of_range,
           "Modern's sentinel block ordinal must never be serialized as a real custom block");
    spawner.spawner_position.y = std::numeric_limits<float>::quiet_NaN();
    expect(!encode_update_spawner_payload(spawner, error) && error == PayloadError::non_finite,
           "NaN spawner coordinates must stop before entity authority");
    auto corrupt_spawner = *spawner_bytes;
    corrupt_spawner.back() = 2U;
    expect(!decode_update_spawner_payload(corrupt_spawner, decoded_spawner, error),
           "non-boolean spawner state must fail before world mutation");
    BoundedWriter hostile_voice{8U};
    expect(hostile_voice.write_i32(-1) &&
               !decode_voice_chat_payload(hostile_voice.bytes(), decoded_audio, error) &&
               error == PayloadError::out_of_range,
           "negative voice lengths must fail before audio allocation");
    auto trailing_voice = *voice_bytes;
    trailing_voice.push_back(0U);
    expect(!decode_voice_chat_payload(trailing_voice, decoded_audio, error),
           "voice frames with undeclared trailing data must fail closed");
}

void enemy_codecs_match_profile_extensions_and_authority_domains()
{
    using namespace openminerz::net;
    PayloadError error{};

    SpawnEnemyPayload classic;
    classic.spawn_position = {1.0F, 2.0F, 3.0F};
    classic.enemy_type = 51U;
    classic.enemy_id = 42;
    classic.random_seed = 7;
    classic.initialization = {100, 200, 300, 400, 500};
    const auto classic_bytes =
        encode_spawn_enemy_payload(classic, ProtocolVersion::classic_v3, error);
    SpawnEnemyPayload decoded_classic;
    expect(classic_bytes.has_value() && classic_bytes->size() == 41U &&
               *classic_bytes == bytes_from_hex("0000803F0000004000004040332A00000007000000"
                                                "64000000C80000002C01000090010000F4010000") &&
               decode_spawn_enemy_payload(*classic_bytes, ProtocolVersion::classic_v3,
                                          decoded_classic, error) &&
               decoded_classic == classic,
           "Classic SpawnEnemy must retain its exact 41-byte source layout");

    SpawnEnemyPayload modern = classic;
    modern.spawner_position = {-4.0F, 5.0F, 6.0F};
    modern.enemy_type = 52U;
    modern.spawn_value = 9;
    modern.player_name = "Ada";
    const auto modern_bytes = encode_spawn_enemy_payload(modern, ProtocolVersion::modern_v4, error);
    SpawnEnemyPayload decoded_modern;
    expect(modern_bytes.has_value() && modern_bytes->size() == 61U &&
               decode_spawn_enemy_payload(*modern_bytes, ProtocolVersion::modern_v4, decoded_modern,
                                          error) &&
               decoded_modern == modern && modern_bytes->at(24U) == 52U &&
               modern_bytes->at(37U) == 3U,
           "Modern SpawnEnemy must insert spawner, spawn value, and BinaryWriter name fields");

    KillEnemyPayload killed{42, 255, 122, 7U};
    const auto kill_bytes = encode_kill_enemy_payload(killed, ProtocolVersion::classic_v3, error);
    KillEnemyPayload decoded_kill;
    expect(kill_bytes.has_value() && *kill_bytes == bytes_from_hex("2A000000FF0000007A0007") &&
               decode_kill_enemy_payload(*kill_bytes, ProtocolVersion::classic_v3, decoded_kill,
                                         error) &&
               decoded_kill == killed,
           "KillEnemy must retain enemy, byte-domain target, weapon, and killer ordering");

    EnemyTargetPayload target{42, 255};
    const auto target_bytes = encode_enemy_target_payload(target, error);
    EnemyTargetPayload decoded_target;
    expect(target_bytes.has_value() && *target_bytes == bytes_from_hex("2A000000FF000000") &&
               decode_enemy_target_payload(*target_bytes, decoded_target, error) &&
               decoded_target == target,
           "SpeedUpEnemy and EnemyGiveUp must share the exact eight-byte target schema");

    const MessageDescriptor *spawn_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "SpawnEnemyMessage");
    const MessageDescriptor *kill_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "KillEnemyMessage");
    const MessageDescriptor *speed_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "SpeedUpEnemyMessage");
    const MessageDescriptor *give_up_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "EnemyGiveUpMessage");
    expect(spawn_descriptor != nullptr && kill_descriptor != nullptr &&
               speed_descriptor != nullptr && give_up_descriptor != nullptr &&
               validate_known_payload(ProtocolVersion::modern_v4, *spawn_descriptor, *modern_bytes,
                                      error) &&
               validate_known_payload(ProtocolVersion::modern_v4, *kill_descriptor, *kill_bytes,
                                      error) &&
               validate_known_payload(ProtocolVersion::modern_v4, *speed_descriptor, *target_bytes,
                                      error) &&
               validate_known_payload(ProtocolVersion::modern_v4, *give_up_descriptor,
                                      *target_bytes, error),
           "enemy codecs must be reachable through typed fail-closed dispatch");

    classic.enemy_type = 52U;
    expect(!encode_spawn_enemy_payload(classic, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "Classic must reject the first Modern-only constructible enemy type");
    modern.enemy_type = 53U;
    expect(!encode_spawn_enemy_payload(modern, ProtocolVersion::modern_v4, error) &&
               error == PayloadError::out_of_range,
           "Modern must reject enum tail values absent from the stock constructible type table");
    classic.enemy_type = 51U;
    classic.spawn_position.x = std::numeric_limits<float>::quiet_NaN();
    expect(!encode_spawn_enemy_payload(classic, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::non_finite,
           "non-finite enemy positions must stop before entity creation");
    classic.spawn_position.x = 1.0F;
    classic.player_name = "Modern tail";
    expect(!encode_spawn_enemy_payload(classic, ProtocolVersion::classic_v3, error),
           "Classic must never serialize Modern spawn extensions");
    target.target_id = 256;
    expect(!encode_enemy_target_payload(target, error) && error == PayloadError::out_of_range,
           "enemy targets must remain in the original byte-sized gamer domain");
    killed.weapon_id = 123;
    expect(!encode_kill_enemy_payload(killed, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "Classic kills must reject Modern-only item ordinals");
    auto truncated_modern = *modern_bytes;
    truncated_modern.resize(39U);
    expect(!decode_spawn_enemy_payload(truncated_modern, ProtocolVersion::modern_v4, decoded_modern,
                                       error),
           "truncated Modern names and initialization records must fail without partial spawn");
}

void dragon_codecs_preserve_waypoints_migration_and_bounded_detonation()
{
    using namespace openminerz::net;
    PayloadError error{};

    SpawnDragonPayload spawned{4U, 255U, true, -1.0F};
    const auto spawn_bytes = encode_spawn_dragon_payload(spawned, error);
    SpawnDragonPayload decoded_spawn;
    expect(spawn_bytes.has_value() && *spawn_bytes == bytes_from_hex("04FF01000080BF") &&
               decode_spawn_dragon_payload(*spawn_bytes, decoded_spawn, error) &&
               decoded_spawn == spawned,
           "SpawnDragon must retain type, spawner GID, biome flag, and -1 health sentinel");

    RequestDragonPayload requested{4U, true};
    const auto request_bytes = encode_request_dragon_payload(requested, error);
    RequestDragonPayload decoded_request;
    expect(request_bytes.has_value() && *request_bytes == bytes_from_hex("0401") &&
               decode_request_dragon_payload(*request_bytes, decoded_request, error) &&
               decoded_request == requested,
           "RequestDragon must remain the exact type byte and strict biome boolean");

    ExistingDragonPayload existing{3U, 7U, false, 250.0F};
    const auto existing_bytes = encode_existing_dragon_payload(existing, error);
    ExistingDragonPayload decoded_existing;
    expect(existing_bytes.has_value() && existing_bytes->size() == 7U &&
               decode_existing_dragon_payload(*existing_bytes, decoded_existing, error) &&
               decoded_existing == existing,
           "ExistingDragon must reuse the seven-byte announcement layout without field drift");

    DragonWaypointPayload waypoint{{1.0F, 2.0F, 3.0F}, {-1.0F, -2.0F, -3.0F}, 4.0F, 5.0F, 3U, 2U};
    const auto update_bytes = encode_update_dragon_payload(waypoint, error);
    DragonWaypointPayload decoded_waypoint;
    expect(update_bytes.has_value() && update_bytes->size() == 34U &&
               decode_update_dragon_payload(*update_bytes, decoded_waypoint, error) &&
               decoded_waypoint == waypoint && update_bytes->at(32U) == 3U &&
               update_bytes->at(33U) == 2U,
           "UpdateDragon must preserve the original 34-byte BaseDragonWaypoint binary");

    DragonAttackPayload attack{waypoint, {8.0F, 9.0F, 10.0F}, true, -7};
    const auto attack_bytes = encode_dragon_attack_payload(attack, error);
    DragonAttackPayload decoded_attack;
    expect(attack_bytes.has_value() && attack_bytes->size() == 51U &&
               decode_dragon_attack_payload(*attack_bytes, decoded_attack, error) &&
               decoded_attack == attack && attack_bytes->at(46U) == 1U,
           "DragonAttack must append target, strict animation flag, and signed fireball index");

    DragonMigrationPayload migration{1.0F,
                                     2.0F,
                                     3.0F,
                                     4.0F,
                                     5.0F,
                                     6.0F,
                                     7.0F,
                                     8.0F,
                                     9.0F,
                                     10.0F,
                                     11.0F,
                                     12,
                                     true,
                                     13.0F,
                                     3U,
                                     4U,
                                     {14.0F, 15.0F, 16.0F},
                                     {17.0F, 18.0F, 19.0F}};
    MigrateDragonPayload migrated{migration, 255U};
    const auto migration_bytes = encode_migrate_dragon_payload(migrated, error);
    MigrateDragonPayload decoded_migration;
    expect(migration_bytes.has_value() && migration_bytes->size() == 80U &&
               migration_bytes->at(48U) == 1U && migration_bytes->back() == 255U &&
               decode_migrate_dragon_payload(*migration_bytes, decoded_migration, error) &&
               decoded_migration == migrated,
           "MigrateDragon must retain every host state scalar, vector, enum, and target GID");

    KillDragonPayload killed{{-1.0F, 2.0F, -3.0F}, 122, 9U};
    const auto kill_bytes = encode_kill_dragon_payload(killed, ProtocolVersion::classic_v3, error);
    KillDragonPayload decoded_kill;
    expect(kill_bytes.has_value() && kill_bytes->size() == 15U &&
               decode_kill_dragon_payload(*kill_bytes, ProtocolVersion::classic_v3, decoded_kill,
                                          error) &&
               decoded_kill == killed,
           "KillDragon must preserve finite location, profile item ordinal, and killer GID");

    DetonateFireballPayload detonated{{20.0F, 21.0F, 22.0F}, -1, 4U, {{1, -2, 3}, {-4, 5, -6}}};
    const auto detonate_bytes = encode_detonate_fireball_payload(detonated, error);
    DetonateFireballPayload decoded_detonation;
    expect(detonate_bytes.has_value() && detonate_bytes->size() == 42U &&
               detonate_bytes->at(17U) == 2U &&
               decode_detonate_fireball_payload(*detonate_bytes, decoded_detonation, error) &&
               decoded_detonation == detonated,
           "DetonateFireball must bind its declared byte count to exact IntVector3 records");

    const std::array dispatch_cases{
        std::pair{"SpawnDragonMessage", &*spawn_bytes},
        std::pair{"RequestDragonMessage", &*request_bytes},
        std::pair{"ExistingDragonMessage", &*existing_bytes},
        std::pair{"UpdateDragonMessage", &*update_bytes},
        std::pair{"DragonAttackMessage", &*attack_bytes},
        std::pair{"MigrateDragonMessage", &*migration_bytes},
        std::pair{"KillDragonMessage", &*kill_bytes},
        std::pair{"DetonateFireballMessage", &*detonate_bytes},
    };
    for (const auto &[name, bytes] : dispatch_cases)
    {
        const MessageDescriptor *descriptor = message_by_name(ProtocolVersion::modern_v4, name);
        expect(descriptor != nullptr &&
                   validate_known_payload(ProtocolVersion::modern_v4, *descriptor, *bytes, error),
               "dragon codecs must be reachable through typed fail-closed dispatch");
    }

    spawned.dragon_type = 5U;
    expect(!encode_spawn_dragon_payload(spawned, error) && error == PayloadError::out_of_range,
           "dragon type sentinels must fail before spawn authority");
    spawned.dragon_type = 4U;
    spawned.health = std::numeric_limits<float>::quiet_NaN();
    expect(!encode_spawn_dragon_payload(spawned, error) && error == PayloadError::non_finite,
           "NaN dragon health must fail before entity allocation");
    waypoint.animation = 4U;
    expect(!encode_update_dragon_payload(waypoint, error) && error == PayloadError::out_of_range,
           "unknown waypoint animations must never enter the stock state machine");
    auto corrupt_attack = *attack_bytes;
    corrupt_attack[46U] = 2U;
    expect(!decode_dragon_attack_payload(corrupt_attack, decoded_attack, error),
           "non-boolean animated-attack bytes must fail without partial state application");
    migration.next_fireball_index = -1;
    expect(!encode_migrate_dragon_payload({migration, 1U}, error) &&
               error == PayloadError::out_of_range,
           "negative next-fireball migration state must fail closed");
    auto corrupt_migration = *migration_bytes;
    corrupt_migration[48U] = 2U;
    expect(!decode_migrate_dragon_payload(corrupt_migration, decoded_migration, error),
           "non-boolean migration biome state must fail before host ownership moves");
    killed.weapon_id = 123;
    expect(!encode_kill_dragon_payload(killed, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "Classic dragon kills must reject Modern-only item ordinals");
    auto count_mismatch = *detonate_bytes;
    count_mismatch[17U] = 3U;
    expect(!decode_detonate_fireball_payload(count_mismatch, decoded_detonation, error) &&
               error == PayloadError::malformed,
           "fireball block count mismatches must fail before terrain removal");
    detonated.blocks_to_remove.resize(maximum_fireball_blocks + 1U);
    expect(!encode_detonate_fireball_payload(detonated, error) &&
               error == PayloadError::out_of_range,
           "fireball block arrays must remain bounded by the original byte count");
}

void inventory_bootstrap_matches_both_deployed_profiles_and_rejects_hostile_blobs()
{
    using namespace openminerz::net;
    PayloadError error{};

    const PlayerInventoryPayload empty_classic =
        make_empty_player_inventory(ProtocolVersion::classic_v3);
    const auto classic_bytes =
        encode_player_inventory_payload(empty_classic, ProtocolVersion::classic_v3, error);
    expect(classic_bytes.has_value() && classic_bytes->size() == 49U &&
               *classic_bytes ==
                   bytes_from_hex("0450494E5601000000"
                                  "0000000000000000000000000000000000000000000000000000000000000000"
                                  "0000000000000000"),
           "Classic PINV v1 must match the original 32-slot plus 8-slot empty binary");

    const PlayerInventoryPayload empty_modern =
        make_empty_player_inventory(ProtocolVersion::modern_v4);
    const auto modern_bytes =
        encode_player_inventory_payload(empty_modern, ProtocolVersion::modern_v4, error);
    expect(modern_bytes.has_value() && modern_bytes->size() == 74U &&
               *modern_bytes ==
                   bytes_from_hex("0450494E5604000000"
                                  "0000000000000000000000000000000000000000000000000000000000000000"
                                  "02000000080000000000000000000000080000000000000000000000"
                                  "0000000000"),
           "Modern PINV v4 must match the original double-tray, teleporter, and spawn layout");

    const PlayerInventoryPayload default_classic =
        make_default_player_inventory(ProtocolVersion::classic_v3, false);
    const auto default_classic_bytes =
        encode_player_inventory_payload(default_classic, ProtocolVersion::classic_v3, error);
    expect(default_classic_bytes.has_value() && default_classic_bytes->size() == 101U &&
               *default_classic_bytes ==
                   bytes_from_hex("0450494E5601000000"
                                  "0000000000000000000000000000000000000000000000000000000000000000"
                                  "01170001000000803F01270001000000803F"
                                  "01330001000000803F08000000"
                                  "01310001000000803F012C00C8000000803F"
                                  "010E0010000000803F0000"),
           "the Classic default loadout must retain exact item IDs, health, clip, and slot order");

    PlayerInventoryPayload rich = make_default_player_inventory(ProtocolVersion::modern_v4, false);
    rich.storage[0] = InventoryItemPayload{
        77, 1, 0.75F, std::nullopt, Vector3{1.0F, 2.0F, 3.0F}, std::string{"Home"}};
    rich.teleport_stations.push_back(InventoryItemPayload{
        151, 1, 1.0F, std::nullopt, Vector3{-4.0F, 5.0F, 6.0F}, std::string{"Mine"}});
    rich.spawn_point = InventoryItemPayload{
        133, 1, 1.0F, std::nullopt, Vector3{8.0F, 128.0F, -8.0F}, std::nullopt};
    const auto rich_bytes =
        encode_player_inventory_payload(rich, ProtocolVersion::modern_v4, error);
    PlayerInventoryPayload rich_decoded;
    expect(rich_bytes.has_value() &&
               decode_player_inventory_payload(*rich_bytes, ProtocolVersion::modern_v4,
                                               rich_decoded, error) &&
               rich_decoded == rich,
           "Modern variable item subtypes and both extra persistence collections must round-trip");

    InventoryStorePayload store{rich, true};
    const auto store_bytes =
        encode_inventory_store_payload(store, ProtocolVersion::modern_v4, error);
    InventoryStorePayload decoded_store;
    expect(store_bytes.has_value() &&
               decode_inventory_store_payload(*store_bytes, ProtocolVersion::modern_v4,
                                              decoded_store, error) &&
               decoded_store == store,
           "InventoryStore must append one strict final-save boolean after the typed PINV blob");
    auto corrupt_tail = *store_bytes;
    corrupt_tail.back() = 2U;
    expect(!decode_inventory_store_payload(corrupt_tail, ProtocolVersion::modern_v4, decoded_store,
                                           error),
           "a non-boolean persistence tail must fail before inventory state is replaced");

    InventoryRetrievePayload retrieve{rich, 7U, true};
    const auto retrieve_bytes =
        encode_inventory_retrieve_payload(retrieve, ProtocolVersion::modern_v4, error);
    InventoryRetrievePayload decoded_retrieve;
    expect(retrieve_bytes.has_value() && retrieve_bytes->at(retrieve_bytes->size() - 2U) == 7U &&
               decode_inventory_retrieve_payload(*retrieve_bytes, ProtocolVersion::modern_v4,
                                                 decoded_retrieve, error) &&
               decoded_retrieve == retrieve,
           "InventoryRetrieve must preserve the original typed inventory, target GID, flag order");

    InventoryItemPayload impossible_gun{51, 1, 1.0F, 9, std::nullopt, std::nullopt};
    expect(!encode_inventory_item_payload(impossible_gun, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::out_of_range,
           "clip counts above the source-derived weapon capacity must be rejected");
    InventoryItemPayload nan_locator{77,
                                     1,
                                     1.0F,
                                     std::nullopt,
                                     Vector3{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F},
                                     std::string{"bad"}};
    expect(!encode_inventory_item_payload(nan_locator, ProtocolVersion::classic_v3, error) &&
               error == PayloadError::non_finite,
           "non-finite locator coordinates must never enter navigation state");
    expect(!decode_player_inventory_payload(*modern_bytes, ProtocolVersion::classic_v3,
                                            rich_decoded, error) &&
               error == PayloadError::out_of_range,
           "a Modern PINV version must fail closed under the Classic core profile");

    std::int16_t wire_item{};
    openminerz::InventoryItemId internal_item{};
    expect(inventory_item_to_wire(openminerz::InventoryItemId::modern_chainsaw_1,
                                  ProtocolVersion::modern_v4, wire_item) &&
               wire_item == 123 &&
               inventory_item_to_wire(openminerz::InventoryItemId::modern_precision_laser,
                                      ProtocolVersion::modern_v4, wire_item) &&
               wire_item == 149 &&
               inventory_item_from_wire(154, ProtocolVersion::modern_v4, internal_item) &&
               internal_item == openminerz::InventoryItemId::modern_tech_door,
           "Modern's colliding item ordinals must translate only at the versioned wire boundary");
    expect(!inventory_item_to_wire(openminerz::InventoryItemId::modern_chainsaw_1,
                                   ProtocolVersion::classic_v3, wire_item) &&
               !inventory_item_to_wire(openminerz::InventoryItemId::grass_block,
                                       ProtocolVersion::classic_v3, wire_item) &&
               !inventory_item_from_wire(123, ProtocolVersion::classic_v3, internal_item),
           "Classic must reject both Modern ordinals and project-local convenience IDs");

    openminerz::PlayerInventory native_modern;
    native_modern.storage().fill(std::nullopt);
    for (auto &tray : native_modern.trays())
        tray.fill(std::nullopt);
    native_modern.set_profile(openminerz::InventoryProfile::modern);
    native_modern.tray(0)[3] =
        openminerz::InventoryStack{openminerz::InventoryItemId::modern_chainsaw_1, 1U};
    native_modern.tray(1)[6] =
        openminerz::InventoryStack{openminerz::InventoryItemId::modern_precision_laser, 1U};
    PlayerInventoryPayload converted_modern;
    expect(
        player_inventory_to_payload(native_modern, ProtocolVersion::modern_v4, converted_modern) &&
            converted_modern.trays.size() == 2U && converted_modern.trays[0][3].has_value() &&
            converted_modern.trays[0][3]->item_id == 123 &&
            converted_modern.trays[1][6].has_value() &&
            converted_modern.trays[1][6]->item_id == 149 &&
            converted_modern.trays[1][6]->rounds_in_clip == 10,
        "native Modern inventory serialization must preserve both trays and exact wire IDs");
}

void world_info_matches_original_classic_and_modern_binaries()
{
    using namespace openminerz::net;
    PayloadError error{};
    WorldInfoPayload world;
    world.terrain_version = 1;
    world.name = "Oracle World";
    world.owner_gamer_tag = "Owner";
    world.creator_gamer_tag = "Creator";
    world.created_date_ticks = 638'000'000'000'000'000LL;
    world.last_played_date_ticks = 638'000'000'000'000'123LL;
    world.seed = 123456;
    world.world_id = {0x78U, 0x56U, 0x34U, 0x12U, 0x34U, 0x12U, 0x78U, 0x56U,
                      0x90U, 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U, 0x45U, 0x67U};
    world.last_position = {8.0F, 128.0F, -8.0F};
    world.infinite_resource_mode = true;
    world.server_message = "Welcome";
    world.server_password = "pw";

    const auto classic = encode_world_info_payload(world, ProtocolVersion::classic_v3, error);
    expect(classic.has_value() &&
               *classic == bytes_from_hex("01000000010000000C4F7261636C6520576F726C64054F776E6572"
                                          "0743726561746F720000B3A69EA1DA087B00B3A69EA1DA0840E20100"
                                          "785634123412785690ABCDEF012345670000004100000043000000C1"
                                          "00000000010757656C636F6D65027077"),
           "Classic WorldInfo v1 must match the owned original-client oracle byte-for-byte");
    WorldInfoPayload decoded_classic;
    expect(
        decode_world_info_payload(*classic, ProtocolVersion::classic_v3, decoded_classic, error) &&
            decoded_classic.format_version == 1 && decoded_classic.name == world.name &&
            decoded_classic.world_id == world.world_id && decoded_classic.doors.empty() &&
            decoded_classic.spawners.empty(),
        "Classic WorldInfo must decode its complete source layout and no Modern tail");

    world.doors.push_back({{10, -20, 30}, 3U});
    world.spawners.push_back({{-4, 5, 6}, "spawner7"});
    world.hell_bosses_spawned = 7;
    world.max_hell_boss_spawns = 50;
    const auto modern = encode_world_info_payload(world, ProtocolVersion::modern_v4, error);
    expect(modern.has_value() &&
               *modern ==
                   bytes_from_hex("05000000010000000C4F7261636C6520576F726C64054F776E6572"
                                  "0743726561746F720000B3A69EA1DA087B00B3A69EA1DA0840E20100"
                                  "785634123412785690ABCDEF012345670000004100000043000000C1"
                                  "00000000010000000A000000ECFFFFFF1E0000000301000000FCFFFFFF"
                                  "050000000600000008737061776E657237010757656C636F6D65027077"
                                  "0700000032000000"),
           "Modern WorldInfo v5 must match doors, spawners, and boss counters exactly");
    WorldInfoPayload decoded_modern;
    expect(decode_world_info_payload(*modern, ProtocolVersion::modern_v4, decoded_modern, error) &&
               decoded_modern.format_version == 5 && decoded_modern.doors == world.doors &&
               decoded_modern.spawners == world.spawners &&
               decoded_modern.hell_bosses_spawned == 7 && decoded_modern.max_hell_boss_spawns == 50,
           "Modern WorldInfo must round-trip every version-five extension");

    WorldCratePayload crate;
    crate.location = {10, 20, -30};
    crate.inventory[0] = InventoryItemPayload{14, 16, 1.0F};
    crate.inventory[31] = InventoryItemPayload{51, 1, 0.75F, 7};
    WorldInfoPayload with_crate = world;
    with_crate.crates = {crate};
    const auto crate_bytes =
        encode_world_info_payload(with_crate, ProtocolVersion::modern_v4, error);
    WorldInfoPayload crate_decoded;
    expect(crate_bytes.has_value() &&
               decode_world_info_payload(*crate_bytes, ProtocolVersion::modern_v4, crate_decoded,
                                         error) &&
               crate_decoded.crates.size() == 1U &&
               crate_decoded.crates[0].inventory == crate.inventory,
           "WorldInfo crates must walk all 32 variable-length typed inventory slots");

    with_crate.crates.push_back(crate);
    expect(!encode_world_info_payload(with_crate, ProtocolVersion::modern_v4, error) &&
               error == PayloadError::out_of_range,
           "duplicate crate locations must fail before a dictionary overwrite can occur");
    auto corrupt_world = *modern;
    corrupt_world.resize(4U);
    expect(!decode_world_info_payload(corrupt_world, ProtocolVersion::modern_v4, decoded_modern,
                                      error),
           "truncated WorldInfo must never partially replace the active world metadata");
    world.last_position.x = std::numeric_limits<float>::infinity();
    expect(!encode_world_info_payload(world, ProtocolVersion::modern_v4, error) &&
               error == PayloadError::non_finite,
           "non-finite join positions must be rejected before terrain bootstrap");
}

void authoritative_late_join_bootstrap_applies_world_save_in_strict_order()
{
    using namespace openminerz;
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.protocol = ProtocolVersion::classic_v3;
    settings.server_message = "Transcript Server";
    settings.password = "pw";

    WorldSaveData save;
    save.seed = 123456789;
    save.infinite_resources = true;
    save.crates[{10, 20, -30}][0] = InventoryStack{InventoryItemId::torch, 16U};
    save.edits.push_back({17, 1, -1, static_cast<int>(world::BlockType::rock)});
    save.edits.push_back({17, 1, -1, static_cast<int>(world::BlockType::dirt)});

    BootstrapWorldIdentity identity;
    identity.name = "Transcript World";
    identity.owner_gamer_tag = "Owner";
    identity.creator_gamer_tag = "Creator";
    identity.created_date_ticks = 638'000'000'000'000'000LL;
    identity.last_played_date_ticks = 638'000'000'000'000'123LL;
    identity.world_id[0] = 0x42U;

    AuthoritativeBootstrapSnapshot snapshot;
    BootstrapSnapshotError snapshot_error{};
    PayloadError payload_error{};
    expect(make_authoritative_bootstrap_snapshot(save, settings, identity, false, snapshot,
                                                 snapshot_error) &&
               snapshot.world.seed == save.seed && snapshot.world.crates.size() == 1U &&
               snapshot.chunk_deltas.size() == 1U,
           "the authoritative adapter must snapshot metadata, inventory, crates, and latest edits");
    const IntVector3 changed_position{17, 1, -1};
    const IntVector3 changed_corner{16, -64, -16};
    std::int32_t expected_delta{};
    expect(encode_delta_entry(changed_position, static_cast<std::uint8_t>(world::BlockType::dirt),
                              ProtocolVersion::classic_v3, expected_delta) &&
               snapshot.chunk_deltas.at(changed_corner) ==
                   std::vector<std::int32_t>{expected_delta} &&
               chunk_id(changed_corner) == static_cast<std::int32_t>(0xFFFF0001U),
           "world edits must collapse last-write-wins into the original chunk/delta identifiers");

    AuthoritativeBootstrapHost direct_host{settings.protocol, snapshot};
    expect(direct_host.register_peer(80U, 6U),
           "the transport-neutral bootstrap handler requires an admitted peer generation");
    PacketError direct_packet_error{};
    const MessageDescriptor *direct_world_request =
        message_by_name(settings.protocol, "RequestWorldInfoMessage");
    const auto direct_world_packet =
        encode_packet(settings.protocol, direct_world_request->id, {}, direct_packet_error);
    const BootstrapHandleResult direct_world =
        direct_host.handle_packet(80U, *direct_world_packet, stock_gameplay_channel, true);
    DecodedPacket direct_decoded;
    expect(
        direct_world.handled() && direct_world.replies.size() == 1U &&
            direct_world.replies[0].peer == 80U && direct_world.replies[0].original_sender == 0U &&
            decode_packet(settings.protocol, direct_world.replies[0].application_packet,
                          direct_decoded, direct_packet_error) &&
            direct_decoded.message != nullptr && direct_decoded.message->name == "WorldInfoMessage",
        "the same authoritative snapshot must produce transport-neutral stock reply packets");
    expect(
        direct_host.handle_packet(80U, *direct_world_packet, stock_gameplay_channel, true).error ==
                BootstrapPumpError::out_of_order &&
            direct_host.handle_packet(80U, *direct_world_packet, 2U, true).error ==
                BootstrapPumpError::malformed_frame,
        "transport-neutral bootstrap handling must retain strict phase and channel gates");

    LoopbackSession session{settings, false, 1, 1, save.infinite_resources};
    PeerAdmissionRequest request;
    request.peer = 81U;
    request.password = settings.password;
    request.properties = session.admission().advertised_properties();
    expect(session.connect(request).status == AdmissionStatus::admitted,
           "the bootstrap transcript requires an admitted stock peer");
    AuthoritativeBootstrapHost host{settings.protocol, snapshot};
    expect(host.register_peer(request.peer, 7U),
           "an admitted transport peer must receive one fresh bootstrap generation");
    expect(!host.gameplay_ready(request.peer, 7U) &&
               !host.gameplay_ready(request.peer, 8U),
           "admission alone must not authorize ordinary gameplay before PlayerExists");

    PacketError packet_error{};
    const auto submit = [&](std::string_view name, std::span<const std::uint8_t> payload) {
        const MessageDescriptor *descriptor = message_by_name(settings.protocol, name);
        expect(descriptor != nullptr, "bootstrap input must exist in the selected stock manifest");
        const auto packet = encode_packet(settings.protocol, descriptor->id, payload, packet_error);
        expect(packet.has_value(), "bootstrap input must receive stock framing");
        expect(session
                   .submit_from_peer(request.peer, *packet, descriptor->transport.delivery,
                                     stock_gameplay_channel)
                   .accepted(),
               "typed bootstrap input must pass the admitted loopback boundary");
        return host.pump_one(session);
    };
    const auto receive = [&]() {
        const auto datagram = session.receive_for_peer(request.peer);
        expect(datagram.has_value(), "an accepted bootstrap request must queue its host response");
        DecodedPacket decoded;
        PacketError decode_error{};
        expect(decode_packet(settings.protocol, datagram->payload, decoded, decode_error) &&
                   decoded.message != nullptr,
               "bootstrap replies must retain valid stock framing");
        return std::tuple{std::string{decoded.message->name},
                          std::vector<std::uint8_t>{decoded.payload.begin(), decoded.payload.end()},
                          datagram->sender};
    };

    expect(submit("RequestInventoryMessage", {}).error == BootstrapPumpError::out_of_order &&
               !session.receive_for_peer(request.peer).has_value(),
           "inventory retrieval before world/player bootstrap must fail without leaking a reply");
    expect(submit("RequestWorldInfoMessage", {}).handled(),
           "the original transcript begins with RequestWorldInfo");
    auto [world_name, world_payload, world_sender] = receive();
    WorldInfoPayload received_world;
    expect(world_name == "WorldInfoMessage" &&
               decode_world_info_payload(world_payload, settings.protocol, received_world,
                                         payload_error) &&
               received_world.name == identity.name && received_world.seed == save.seed &&
               received_world.crates.size() == 1U && world_sender == 0U,
           "WorldInfo response must carry the immutable authoritative snapshot");

    PlayerExistsPayload presence;
    presence.request_response = true;
    presence.avatar_description = {1U, 2U, 3U};
    presence.gamer_tag = "Stock Client";
    presence.player_id = {4U, 5U, 6U};
    const auto presence_payload = encode_player_exists_payload(presence, payload_error);
    expect(presence_payload.has_value() &&
               submit("PlayerExistsMessage", *presence_payload).handled(),
           "PlayerExists request/echo must establish the player before inventory or chunks");
    expect(host.gameplay_ready(request.peer, 7U) &&
               !host.gameplay_ready(request.peer, 8U),
           "gameplay readiness must bind the accepted presence to the current GID generation");
    auto [presence_name, echoed_presence, presence_sender] = receive();
    expect(presence_name == "PlayerExistsMessage" && echoed_presence == *presence_payload &&
               presence_sender == request.peer,
           "the stock PlayerExists echo must preserve both bytes and originating peer identity");
    auto [time_name, time_payload, time_sender] = receive();
    float received_day{};
    expect(time_name == "TimeOfDayMessage" &&
               decode_time_of_day_payload(time_payload, received_day, payload_error) &&
               received_day == save.day && time_sender == 0U,
           "the host must follow presence with its reliable full-day authority value");

    expect(submit("RequestInventoryMessage", {}).handled(),
           "an announced player may retrieve its authoritative inventory once");
    auto [inventory_name, inventory_payload, inventory_sender] = receive();
    InventoryRetrievePayload received_inventory;
    expect(inventory_name == "InventoryRetrieveFromServerMessage" &&
               decode_inventory_retrieve_payload(inventory_payload, settings.protocol,
                                                 received_inventory, payload_error) &&
               received_inventory.player_id == 7U && !received_inventory.default_inventory &&
               inventory_sender == 0U,
           "inventory response must bind the saved typed inventory to the assigned stock GID");

    expect(submit("ClientReadyForChunksMessage", {}).handled(),
           "only an announced player may open the delta-list phase");
    auto [delta_name, delta_payload, delta_sender] = receive();
    std::vector<std::int32_t> delta_ids;
    expect(delta_name == "ProvideDeltaListMessage" &&
               decode_delta_list_payload(delta_payload, delta_ids, payload_error) &&
               delta_ids == std::vector<std::int32_t>{chunk_id(changed_corner)} &&
               delta_sender == 0U,
           "the ready response must enumerate exactly the modified authoritative chunks");

    const auto chunk_request = encode_request_chunk_payload({changed_position, 7U}, payload_error);
    expect(chunk_request.has_value() && submit("RequestChunkMessage", *chunk_request).handled(),
           "a post-delta-list peer may request a concrete modified chunk");
    auto [chunk_name, chunk_payload, chunk_sender] = receive();
    ProvideChunkPayload received_chunk;
    expect(chunk_name == "ProvideChunkMessage" &&
               decode_provide_chunk_payload(chunk_payload, received_chunk, payload_error) &&
               received_chunk.block_location == changed_corner && received_chunk.priority == 7U &&
               received_chunk.delta == std::vector<std::int32_t>{expected_delta} &&
               chunk_sender == 0U,
           "ProvideChunk must retain the canonical corner, request priority, and last-write delta");

    const auto status = host.status(request.peer);
    expect(status.has_value() && status->world_info_sent && status->player_announced &&
               status->inventory_sent && status->delta_list_sent && status->chunks_sent == 1U,
           "peer bootstrap state must advance only after each response enters the bounded queue");

    session.disconnect(request.peer);
    host.disconnect(request.peer);
    expect(session.connect(request).status == AdmissionStatus::admitted &&
               host.register_peer(request.peer, 8U),
           "reconnect must allocate a fresh GID and a fresh bootstrap generation");
    expect(!host.gameplay_ready(request.peer, 7U) &&
               !host.gameplay_ready(request.peer, 8U),
           "disconnect must revoke the old GID and require presence for the new generation");
    expect(submit("RequestChunkMessage", *chunk_request).error ==
                   BootstrapPumpError::out_of_order &&
               !session.receive_for_peer(request.peer).has_value(),
           "a reconnect cannot reuse stale ready/chunk state from the disconnected generation");
    expect(submit("RequestWorldInfoMessage", {}).handled() &&
               submit("PlayerExistsMessage", *presence_payload).handled() &&
               host.gameplay_ready(request.peer, 8U) &&
               !host.gameplay_ready(request.peer, 7U),
           "only a fresh source-ordered presence may re-enable gameplay after reconnect");

    WorldSaveData incompatible = save;
    incompatible.inventory.storage()[0] = InventoryStack{InventoryItemId::grass_block, 1U};
    expect(!make_authoritative_bootstrap_snapshot(incompatible, settings, identity, false, snapshot,
                                                  snapshot_error) &&
               snapshot_error == BootstrapSnapshotError::incompatible_inventory,
           "a Modern-only item must block a Classic host before any original client sees it");
}

void oversized_world_info_uses_ordered_item_crate_replay()
{
    using namespace openminerz;
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.protocol = ProtocolVersion::classic_v3;
    WorldSaveData save;
    save.seed = 91;
    for (int index{}; index < 100; ++index)
        static_cast<void>(save.crates[{index * 16, 0, 0}]);
    BootstrapWorldIdentity identity;
    identity.name = "Large Crate World";
    identity.owner_gamer_tag = "Owner";
    identity.creator_gamer_tag = "Owner";
    identity.created_date_ticks = 638'000'000'000'000'000LL;
    identity.last_played_date_ticks = identity.created_date_ticks;

    AuthoritativeBootstrapSnapshot snapshot;
    BootstrapSnapshotError snapshot_error{};
    expect(make_authoritative_bootstrap_snapshot(save, settings, identity, true, snapshot,
                                                 snapshot_error) &&
               snapshot.world.crates.empty() && snapshot.crate_replay.size() == 100U,
           "an oversized crate dictionary must use the dedicated server's bounded replay fallback");

    PayloadError payload_error{};
    const auto first_replay =
        encode_item_crate_payload(snapshot.crate_replay.front(), settings.protocol, payload_error);
    ItemCratePayload decoded_replay;
    expect(first_replay.has_value() && first_replay->size() == 17U &&
               decode_item_crate_payload(*first_replay, settings.protocol, decoded_replay,
                                         payload_error) &&
               decoded_replay.index == 0 && !decoded_replay.item,
           "an empty crate replay must use location, slot zero, and a false item marker");
    ItemCratePayload invalid_replay = decoded_replay;
    invalid_replay.index = 32;
    expect(!encode_item_crate_payload(invalid_replay, settings.protocol, payload_error) &&
               payload_error == PayloadError::out_of_range,
           "crate replay can never address outside the original 32-slot array");

    LoopbackSession session{settings, false, 1, 1, false};
    PeerAdmissionRequest admission;
    admission.peer = 82U;
    admission.properties = session.admission().advertised_properties();
    expect(session.connect(admission).status == AdmissionStatus::admitted,
           "large world replay requires an admitted stock peer");
    AuthoritativeBootstrapHost host{settings.protocol, snapshot};
    expect(host.register_peer(admission.peer, 1U),
           "large world replay must start from a fresh peer generation");
    const MessageDescriptor *request_descriptor =
        message_by_name(settings.protocol, "RequestWorldInfoMessage");
    PacketError packet_error{};
    const auto request_packet =
        encode_packet(settings.protocol, request_descriptor->id, {}, packet_error);
    expect(request_packet.has_value() &&
               session
                   .submit_from_peer(admission.peer, *request_packet,
                                     MessageDeliveryPolicy::reliable_ordered,
                                     stock_gameplay_channel)
                   .accepted() &&
               host.pump_one(session).handled(),
           "one world request must atomically enqueue metadata followed by its crate replay");

    std::set<IntVector3> locations;
    for (std::size_t index{}; index < 101U; ++index)
    {
        const auto datagram = session.receive_for_peer(admission.peer);
        expect(datagram.has_value(), "every crate replay record must reach the requesting peer");
        DecodedPacket decoded;
        PacketError decode_error{};
        expect(decode_packet(settings.protocol, datagram->payload, decoded, decode_error) &&
                   decoded.message != nullptr,
               "crate replay records must remain stock-framed");
        if (index == 0U)
        {
            WorldInfoPayload world;
            expect(decoded.message->name == "WorldInfoMessage" &&
                       decode_world_info_payload(decoded.payload, settings.protocol, world,
                                                 payload_error) &&
                       world.crates.empty(),
                   "fallback WorldInfo must omit crates so each replay creates it exactly once");
        }
        else
        {
            ItemCratePayload item;
            expect(decoded.message->name == "ItemCrateMessage" &&
                       decode_item_crate_payload(decoded.payload, settings.protocol, item,
                                                 payload_error) &&
                       !item.item && item.index == 0,
                   "each empty crate must arrive as one strict reliable-ordered ItemCrate update");
            locations.insert(item.location);
        }
    }
    expect(locations.size() == 100U && !session.receive_for_peer(admission.peer).has_value(),
           "fallback replay must reconstruct every crate without duplicates or trailing packets");
}

void modern_bootstrap_preserves_custom_doors_and_spawner_identity()
{
    using namespace openminerz;
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.protocol = ProtocolVersion::modern_v4;
    settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
    WorldSaveData source;
    source.seed = 314159;
    source.doors[{10, 2, -3}] = 2U;
    source.doors[{11, 2, -3}] = 4U;
    source.spawners[{-4, 5, 6}] = "spawner7";
    BootstrapWorldIdentity identity;
    identity.name = "Modern Objects";
    identity.owner_gamer_tag = "Host";
    identity.creator_gamer_tag = "Host";
    AuthoritativeBootstrapSnapshot snapshot;
    BootstrapSnapshotError snapshot_error{};
    expect(make_authoritative_bootstrap_snapshot(source, settings, identity, true, snapshot,
                                                 snapshot_error) &&
               snapshot.world.doors.size() == 2U && snapshot.world.spawners.size() == 1U &&
               snapshot.world.spawners.front().spawn_id == "spawner7",
           "Modern bootstrap snapshots must retain custom-door model bytes and spawner IDs");

    AuthoritativeBootstrapHost host{settings.protocol, snapshot};
    constexpr PeerId peer = 205U;
    expect(host.register_peer(peer, 1U) && host.set_custom_door({12, 2, -3}, 3U) &&
               host.set_custom_door({12, 2, -3}, 3U) && !host.set_custom_door({12, 2, -3}, 2U) &&
               !host.set_custom_door({13, 2, -3}, 0U) && !host.destroy_custom_block({99, 2, -3}) &&
               host.destroy_custom_block({12, 2, -3}) && host.destroy_custom_block({10, 2, -3}) &&
               host.update_spawner({7, 8, 9}, true) && host.update_spawner({7, 8, 9}, false) &&
               !host.update_spawner({99, 8, 9}, false),
           "live Modern object mutations must preserve exact door identity and reject conflicts, "
           "unknown removals, invalid models, and unknown spawner stops");

    const MessageDescriptor *request =
        message_by_name(settings.protocol, "RequestWorldInfoMessage");
    PacketError packet_error{};
    const auto packet = request == nullptr
                            ? std::optional<std::vector<std::uint8_t>>{}
                            : encode_packet(settings.protocol, request->id, {}, packet_error);
    expect(packet.has_value(), "the Modern object snapshot request must frame");
    const BootstrapHandleResult result =
        host.handle_packet(peer, *packet, stock_gameplay_channel, true);
    DecodedPacket decoded;
    WorldInfoPayload world;
    PayloadError payload_error{};
    expect(
        result.handled() && result.replies.size() == 1U &&
            decode_packet(settings.protocol, result.replies.front().application_packet, decoded,
                          packet_error) &&
            decoded.message != nullptr && decoded.message->name == "WorldInfoMessage" &&
            decode_world_info_payload(decoded.payload, settings.protocol, world, payload_error) &&
            world.doors.size() == 1U && world.doors.front().location == IntVector3{11, 2, -3} &&
            world.doors.front().model_name == 4U && world.spawners.size() == 2U &&
            std::ranges::any_of(world.spawners,
                                [](const WorldSpawnerPayload &spawner) {
                                    return spawner.location == IntVector3{7, 8, 9} &&
                                           spawner.spawn_id != "spawner7";
                                }),
        "a later Modern join must receive the committed door removal and unique spawner identity");
}

void authoritative_client_bootstrap_reconstructs_snapshot_and_fails_closed()
{
    using namespace openminerz;
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.protocol = ProtocolVersion::classic_v3;
    WorldSaveData source;
    source.seed = 0x1020304;
    source.infinite_resources = true;
    source.owner = "KikoTs";
    source.saved_unix_seconds = 1'776'000'000;
    source.has_player = true;
    source.player_position = {17.5F, 42.0F, -9.5F};
    source.inventory.tray()[2]->rounds_in_clip = 3U;
    source.inventory.tray()[2]->durability = 0.75F;
    source.crates[{4, 5, 6}][7] = InventoryStack{InventoryItemId::torch, 12U};
    source.edits = {{17, 1, -1, static_cast<int>(world::BlockType::dirt)},
                    {-1, 2, 31, static_cast<int>(world::BlockType::rock)}};

    BootstrapWorldIdentity identity;
    identity.name = "Client Transcript";
    identity.owner_gamer_tag = "Host";
    identity.creator_gamer_tag = "Host";
    identity.created_date_ticks = 638'000'000'000'000'000LL;
    identity.last_played_date_ticks = identity.created_date_ticks;
    AuthoritativeBootstrapSnapshot snapshot;
    BootstrapSnapshotError snapshot_error{};
    PayloadError payload_error{};
    expect(make_authoritative_bootstrap_snapshot(source, settings, identity, false, snapshot,
                                                 snapshot_error),
           "the client transcript fixture must produce an authoritative host snapshot");
    PlayerExistsPayload host_presence;
    host_presence.gamer_tag = "Host";
    host_presence.player_id = {0x48U, 0x4FU, 0x53U, 0x54U};
    snapshot.host_presence = host_presence;

    constexpr PeerId peer = 91U;
    constexpr PeerId existing_peer = 92U;
    constexpr std::uint8_t local_gid = 7U;
    AuthoritativeBootstrapHost host{settings.protocol, snapshot};
    expect(host.register_peer(peer, local_gid),
           "the transcript host must bind the client peer to its admitted GID");
    expect(host.register_peer(existing_peer, 2U),
           "the transcript host must retain an already admitted peer for presence fanout");
    PlayerInventoryPayload stored_inventory = snapshot.inventory;
    stored_inventory.storage[0] =
        InventoryItemPayload{14, 5, 1.0F, std::nullopt, std::nullopt, std::nullopt};
    expect(host.store_peer_inventory(peer, stored_inventory),
           "a validated player inventory store must replace only that admitted peer's inventory");
    const ItemCratePayload late_crate_item{
        {7, 8, 9}, 3, InventoryItemPayload{14, 3, 1.0F, std::nullopt, std::nullopt, std::nullopt}};
    expect(host.apply_crate_item(late_crate_item) && host.destroy_crate({4, 5, 6}),
           "live crate changes must atomically replace the late-join crate snapshot");
    AuthoritativeBootstrapClientOptions client_options;
    client_options.protocol = settings.protocol;
    client_options.local_gid = local_gid;
    client_options.avatar_description = {1U, 2U, 3U};
    client_options.gamer_tag = "Original Client";
    client_options.player_id = {4U, 5U, 6U, 7U};
    AuthoritativeBootstrapClient client{client_options};

    BootstrapClientError client_error{};
    auto outbound = client.begin(client_error);
    expect(outbound.has_value() &&
               outbound->recipient_gid == std::optional<std::uint8_t>{std::uint8_t{0}} &&
               outbound->delivery == MessageDeliveryPolicy::reliable_ordered,
           "the client transcript must begin with one reliable-ordered world request");
    BootstrapHandleResult host_result =
        host.handle_packet(peer, outbound->application_packet, stock_gameplay_channel, true);
    expect(host_result.handled() && host_result.replies.size() == 1U,
           "the host must answer the client's initial world request exactly once");
    const AuthoritativeBootstrapReply world_reply = host_result.replies.front();

    BootstrapClientHandleResult client_result =
        client.handle_packet(0U, world_reply.application_packet);
    expect(
        client_result.handled() && client_result.outbound.size() == 1U &&
            !client_result.outbound.front().recipient_gid.has_value() && !client_result.completed,
        "validated WorldInfo must advance the client to its exact broadcast presence announcement");
    host_result =
        host.handle_packet(peer, client_result.outbound.front().application_packet, 1U, true);
    expect(host_result.handled() && host_result.replies.size() == 6U &&
               host_result.replies.front().peer == peer &&
               host_result.replies.front().original_sender == peer &&
               host_result.replies[1].peer == existing_peer &&
               host_result.replies[1].original_sender == peer &&
               host_result.replies[2].peer == peer &&
               host_result.replies[2].original_sender == 0U &&
               host_result.replies[3].peer == existing_peer &&
               host_result.replies[3].original_sender == 0U &&
               host_result.replies[4].peer == peer && host_result.replies[5].peer == existing_peer,
           "the presence broadcast must self-echo first, announce the newcomer, fan out host "
           "presence, and then fan out host time authority");

    client_result = client.handle_packet(local_gid, host_result.replies.front().application_packet);
    expect(client_result.handled() && client_result.outbound.size() == 1U &&
               !client_result.outbound.front().recipient_gid.has_value(),
           "only the byte-exact local-GID presence echo may open broadcast inventory retrieval");
    DecodedPacket host_presence_packet;
    PacketError host_presence_error{};
    PlayerExistsPayload decoded_host_presence;
    expect(client.handle_packet(0U, host_result.replies[2].application_packet).error ==
                   BootstrapClientError::unexpected_message &&
               !client.failed() &&
               decode_packet(settings.protocol, host_result.replies[2].application_packet,
                             host_presence_packet, host_presence_error) &&
               decode_player_exists_payload(host_presence_packet.payload, decoded_host_presence,
                                            payload_error) &&
               decoded_host_presence == host_presence,
           "the host's response-only presence must remain ordinary gameplay during bootstrap");
    float bootstrap_day{};
    DecodedPacket bootstrap_time_packet;
    PacketError bootstrap_time_error{};
    expect(client.handle_packet(0U, host_result.replies[4].application_packet).error ==
                   BootstrapClientError::unexpected_message &&
               !client.failed() &&
               decode_packet(settings.protocol, host_result.replies[4].application_packet,
                             bootstrap_time_packet, bootstrap_time_error) &&
               decode_time_of_day_payload(bootstrap_time_packet.payload, bootstrap_day,
                                          payload_error) &&
               bootstrap_day == source.day,
           "host time authority must remain ordinary gameplay without disturbing the strict "
           "bootstrap phase");

    PlayerExistsPayload remote_presence;
    remote_presence.avatar_description = {9U};
    remote_presence.gamer_tag = "Existing Peer";
    remote_presence.player_id = {8U, 7U, 6U};
    const auto remote_presence_payload =
        encode_player_exists_payload(remote_presence, payload_error);
    const MessageDescriptor *presence_descriptor =
        message_by_name(settings.protocol, "PlayerExistsMessage");
    PacketError presence_packet_error{};
    const auto remote_presence_packet =
        remote_presence_payload.has_value() && presence_descriptor != nullptr
            ? encode_packet(settings.protocol, presence_descriptor->id, *remote_presence_payload,
                            presence_packet_error)
            : std::nullopt;
    expect(remote_presence_packet.has_value() &&
               client.handle_packet(2U, *remote_presence_packet).error ==
                   BootstrapClientError::unexpected_message &&
               !client.failed(),
           "another admitted player's presence response must remain ordinary gameplay during "
           "bootstrap");

    host_result =
        host.handle_packet(peer, client_result.outbound.front().application_packet, 1U, true);
    expect(host_result.handled() && host_result.replies.size() == 2U &&
               host_result.replies.front().peer == peer &&
               host_result.replies[1].peer == existing_peer,
           "the host must answer the ordered inventory request and mirror the stock broadcast to "
           "peers");

    client_result = client.handle_packet(0U, host_result.replies.front().application_packet);
    expect(client_result.handled() && client_result.outbound.size() == 1U &&
               client_result.outbound.front().recipient_gid ==
                   std::optional<std::uint8_t>{std::uint8_t{0}},
           "a GID-bound inventory must advance the client to chunk readiness");
    expect(host.apply_block_edit({17, 1, -1}, static_cast<std::uint8_t>(world::BlockType::sand)),
           "an accepted live block edit must replace the late-join snapshot atomically");
    const std::array removed_blocks{IntVector3{-1, 2, 31}, IntVector3{18, 1, -1}};
    expect(host.apply_block_removals(removed_blocks),
           "a validated batch removal must atomically update every touched late-join chunk");
    const std::array frozen_blocks{IntVector3{19, 1, -1}, IntVector3{20, 1, -1}};
    expect(host.apply_block_replacements(frozen_blocks,
                                         static_cast<std::uint8_t>(world::BlockType::ice)),
           "an ice-dragon batch must atomically preserve every replacement in late-join state");
    host_result = host.handle_packet(peer, client_result.outbound.front().application_packet,
                                     stock_gameplay_channel, true);
    expect(host_result.handled() && host_result.replies.size() == 1U,
           "chunk readiness must produce the authoritative modified-chunk list");

    client_result = client.handle_packet(0U, host_result.replies.front().application_packet);
    expect(client_result.handled() && client_result.outbound.size() == 2U,
           "the client must deduplicate and request both advertised canonical chunk corners");
    const std::vector<AuthoritativeBootstrapClientSend> chunk_requests = client_result.outbound;
    for (const AuthoritativeBootstrapClientSend &chunk_request : chunk_requests)
    {
        expect(chunk_request.recipient_gid == std::optional<std::uint8_t>{std::uint8_t{0}} &&
                   chunk_request.delivery == MessageDeliveryPolicy::reliable_unordered,
               "every original chunk request must retain reliable-unordered semantics");
        host_result = host.handle_packet(peer, chunk_request.application_packet,
                                         stock_gameplay_channel, true);
        expect(host_result.handled() && host_result.replies.size() == 1U,
               "each canonical chunk request must receive one bounded delta payload");
        client_result = client.handle_packet(0U, host_result.replies.front().application_packet);
        expect(client_result.handled(),
               "each host chunk must pass the client's canonical corner and delta checks");
    }

    const auto &restored = client.world_save();
    expect(
        client.complete() && restored.has_value() && restored->seed == source.seed &&
            restored->infinite_resources && restored->has_player &&
            restored->player_position.x == source.player_position.x &&
            restored->player_position.y == source.player_position.y &&
            restored->player_position.z == source.player_position.z &&
            !restored->crates.contains({4, 5, 6}) &&
            restored->crates.at({7, 8, 9})[3].has_value() &&
            restored->crates.at({7, 8, 9})[3]->item == InventoryItemId::torch &&
            restored->crates.at({7, 8, 9})[3]->count == 3U &&
            restored->inventory.tray()[2].has_value() &&
            restored->inventory.tray()[2]->rounds_in_clip == 3U &&
            restored->inventory.tray()[2]->durability == 0.75F &&
            restored->inventory.storage()[0].has_value() &&
            restored->inventory.storage()[0]->item == InventoryItemId::torch &&
            restored->inventory.storage()[0]->count == 5U && restored->edits.size() == 5U &&
            std::ranges::any_of(restored->edits,
                                [](const std::array<int, 4> &edit) {
                                    return edit ==
                                           std::array<int, 4>{
                                               17, 1, -1, static_cast<int>(world::BlockType::sand)};
                                }) &&
            std::ranges::any_of(restored->edits,
                                [](const std::array<int, 4> &edit) {
                                    return edit == std::array<int, 4>{-1, 2, 31, 0};
                                }) &&
            std::ranges::any_of(restored->edits,
                                [](const std::array<int, 4> &edit) {
                                    return edit == std::array<int, 4>{18, 1, -1, 0};
                                }) &&
            std::ranges::any_of(restored->edits,
                                [](const std::array<int, 4> &edit) {
                                    return edit ==
                                           std::array<int, 4>{
                                               19, 1, -1, static_cast<int>(world::BlockType::ice)};
                                }) &&
            std::ranges::any_of(restored->edits,
                                [](const std::array<int, 4> &edit) {
                                    return edit ==
                                           std::array<int, 4>{
                                               20, 1, -1, static_cast<int>(world::BlockType::ice)};
                                }) &&
            client.world_info().has_value() && client.native_inventory().has_value() &&
            client.native_inventory()->storage[0] == stored_inventory.storage[0],
        "completion must atomically expose the authoritative world, inventory, crates, and edits");

    host.disconnect(peer);
    constexpr std::uint8_t rejoined_gid = 8U;
    expect(host.register_peer(peer, rejoined_gid),
           "rejoining with the same stable peer identity must open a fresh bootstrap generation");
    AuthoritativeBootstrapClientOptions rejoined_options = client_options;
    rejoined_options.local_gid = rejoined_gid;
    AuthoritativeBootstrapClient rejoined{rejoined_options};
    auto rejoined_send = rejoined.begin(client_error);
    expect(rejoined_send.has_value(), "the rejoined client must restart at world discovery");
    host_result =
        host.handle_packet(peer, rejoined_send->application_packet, stock_gameplay_channel, true);
    expect(host_result.handled() && host_result.replies.size() == 1U,
           "the fresh rejoin generation must receive world authority");
    auto rejoined_result =
        rejoined.handle_packet(0U, host_result.replies.front().application_packet);
    expect(rejoined_result.handled() && rejoined_result.outbound.size() == 1U,
           "the rejoined client must advance to presence");
    host_result =
        host.handle_packet(peer, rejoined_result.outbound.front().application_packet, 1U, true);
    expect(host_result.handled() && !host_result.replies.empty(),
           "the rejoined presence must receive its local echo");
    rejoined_result =
        rejoined.handle_packet(rejoined_gid, host_result.replies.front().application_packet);
    expect(rejoined_result.handled() && rejoined_result.outbound.size() == 1U,
           "the rejoined local echo must request its inventory");
    host_result =
        host.handle_packet(peer, rejoined_result.outbound.front().application_packet, 1U, true);
    InventoryRetrievePayload rejoined_inventory;
    DecodedPacket rejoined_inventory_packet;
    PacketError rejoined_packet_error{};
    expect(
        host_result.handled() && !host_result.replies.empty() &&
            decode_packet(settings.protocol, host_result.replies.front().application_packet,
                          rejoined_inventory_packet, rejoined_packet_error) &&
            decode_inventory_retrieve_payload(rejoined_inventory_packet.payload, settings.protocol,
                                              rejoined_inventory, payload_error) &&
            rejoined_inventory.player_id == rejoined_gid && !rejoined_inventory.default_inventory &&
            rejoined_inventory.inventory.storage[0] == stored_inventory.storage[0],
        "the stored inventory must survive disconnect within the host lifetime and remain "
        "bound to the same stable peer identity");

    AuthoritativeBootstrapClient spoofed{client_options};
    expect(spoofed.begin(client_error).has_value() &&
               spoofed.handle_packet(2U, world_reply.application_packet).error ==
                   BootstrapClientError::unexpected_sender &&
               spoofed.failed() && !spoofed.world_save().has_value(),
           "a non-host WorldInfo packet must terminally fail without exposing partial state");
}

void client_gameplay_authority_is_route_strict_and_identity_bound()
{
    using namespace openminerz::net;

    constexpr ProtocolVersion protocol = ProtocolVersion::classic_v3;
    constexpr std::uint8_t sender_gid = 5U;
    PayloadError payload_error{};
    PacketError packet_error{};
    const auto packet = [&](std::string_view name, std::span<const std::uint8_t> payload) {
        const MessageDescriptor *descriptor = message_by_name(protocol, name);
        expect(descriptor != nullptr, "the authority fixture requires a deployed message");
        const auto encoded = encode_packet(protocol, descriptor->id, payload, packet_error);
        expect(encoded.has_value(), "the authority fixture packet must encode");
        return *encoded;
    };
    const auto inbound = [&](std::vector<std::uint8_t> application_packet, std::uint8_t channel,
                             std::uint8_t recipient_gid = 0U) {
        return LegacySteamHostInbound{LegacySteamInboundKind::gameplay,
                                      1005U,
                                      std::move(application_packet),
                                      MessageDeliveryPolicy::reliable_ordered,
                                      channel,
                                      sender_gid,
                                      recipient_gid};
    };

    const auto alter_payload = encode_alter_block_payload({{4, 5, 6}, 1}, protocol, payload_error);
    expect(alter_payload.has_value(), "the authority block-edit fixture must encode");
    const auto alter = inbound(packet("AlterBlockMessage", *alter_payload), 1U);
    expect(assess_client_gameplay_authority(protocol, alter).action == ClientGameplayAction::relay,
           "a bounded source-origin block edit must be eligible for broadcast relay");
    auto wrong_alter_route = alter;
    wrong_alter_route.channel = 0U;
    expect(assess_client_gameplay_authority(protocol, wrong_alter_route).error ==
               ClientGameplayAuthorityError::invalid_route,
           "a valid client broadcast on the direct-host route must fail closed");

    const auto world_request = inbound(packet("RequestWorldInfoMessage", {}), 0U);
    expect(assess_client_gameplay_authority(protocol, world_request).action ==
               ClientGameplayAction::consume_on_host,
           "the exact world request route must terminate at host authority");
    auto broadcast_world_request = world_request;
    broadcast_world_request.channel = 1U;
    expect(assess_client_gameplay_authority(protocol, broadcast_world_request).error ==
               ClientGameplayAuthorityError::invalid_route,
           "a direct host request must not be smuggled through peer broadcast");

    const auto pickup_payload = encode_request_pickup_payload({11, 12}, payload_error);
    expect(pickup_payload.has_value(), "the request-pickup authority fixture must encode");
    const auto pickup = inbound(packet("RequestPickupMessage", *pickup_payload), 1U);
    expect(assess_client_gameplay_authority(protocol, pickup).action ==
               ClientGameplayAction::relay_and_consume,
           "the original broadcast host request must be both relayed and consumed");

    const auto melee_payload =
        encode_melee_player_payload({1, {1.0F, 2.0F, 3.0F}}, protocol, payload_error);
    expect(melee_payload.has_value(), "the melee authority fixture must encode");
    const auto melee = inbound(packet("MeleePlayerMessage", *melee_payload), 1U, 7U);
    expect(assess_client_gameplay_authority(protocol, melee).action == ClientGameplayAction::relay,
           "melee must retain its source-defined proxy-direct route");
    const auto host_melee = inbound(packet("MeleePlayerMessage", *melee_payload), 0U, 0U);
    expect(assess_client_gameplay_authority(protocol, host_melee).action ==
               ClientGameplayAction::consume_on_host,
           "melee targeting GID zero must retain its source-defined direct-host route");
    auto broadcast_melee = melee;
    broadcast_melee.recipient_gid = 0U;
    expect(assess_client_gameplay_authority(protocol, broadcast_melee).error ==
               ClientGameplayAuthorityError::invalid_route,
           "a targeted melee payload must never become a broadcast");

    const auto kill_payload =
        encode_kill_enemy_payload({31, 32, 1, sender_gid}, protocol, payload_error);
    expect(kill_payload.has_value(), "the kill authority fixture must encode");
    const auto kill = inbound(packet("KillEnemyMessage", *kill_payload), 1U);
    expect(assess_client_gameplay_authority(protocol, kill).action == ClientGameplayAction::relay,
           "a kill attribution matching the admitted sender GID must relay");
    const auto spoofed_kill_payload =
        encode_kill_enemy_payload({31, 32, 1, 6U}, protocol, payload_error);
    expect(spoofed_kill_payload.has_value(), "the spoofed kill fixture must encode");
    const auto spoofed_kill = inbound(packet("KillEnemyMessage", *spoofed_kill_payload), 1U);
    expect(assess_client_gameplay_authority(protocol, spoofed_kill).error ==
               ClientGameplayAuthorityError::identity_spoof,
           "an embedded killer GID may never impersonate another admitted player");

    DigPayload dig_effect{false, {1.0F, 2.0F, 3.0F}, {0.0F, 1.0F, 0.0F}, 6};
    const auto dig_effect_payload = encode_dig_payload(dig_effect, protocol, payload_error);
    expect(dig_effect_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("DigMessage", *dig_effect_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "a bounded source-authored DigMessage face effect must retain its broadcast route");
    dig_effect.direction = {0.70710677F, 0.70710677F, 0.0F};
    const auto diagonal_dig_payload = encode_dig_payload(dig_effect, protocol, payload_error);
    expect(diagonal_dig_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("DigMessage", *diagonal_dig_payload), 1U))
                       .action == ClientGameplayAction::relay,
            "an original normalized collision-plane DigMessage normal must retain its route");
    dig_effect.direction = {0.5F, 0.5F, 0.0F};
    const auto scaled_dig_payload = encode_dig_payload(dig_effect, protocol, payload_error);
    dig_effect.direction = {};
    const auto zero_dig_payload = encode_dig_payload(dig_effect, protocol, payload_error);
    expect(scaled_dig_payload.has_value() && zero_dig_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("DigMessage", *scaled_dig_payload), 1U))
                       .error == ClientGameplayAuthorityError::invalid_packet &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("DigMessage", *zero_dig_payload), 1U))
                       .error == ClientGameplayAuthorityError::invalid_packet,
           "zero or materially scaled DigMessage normals must fail before effect relay");

    const auto observed_dragon_kill_payload =
        encode_kill_dragon_payload({{4.0F, 80.0F, 9.0F}, 50, 6U}, protocol, payload_error);
    expect(
        observed_dragon_kill_payload.has_value() &&
            assess_client_gameplay_authority(
                protocol, inbound(packet("KillDragonMessage", *observed_dragon_kill_payload), 1U))
                    .action == ClientGameplayAction::relay,
        "a stock observer may report that another shooter's replicated hit killed the dragon");

    SpawnEnemyPayload spawn;
    spawn.spawn_position = {12.0F, 3.0F, -8.0F};
    spawn.enemy_type = 0U;
    spawn.enemy_id = 31;
    spawn.random_seed = 77;
    spawn.initialization = {2'000, 4'000, 1'000, 12'000, 45'000};
    const auto spawn_payload = encode_spawn_enemy_payload(spawn, protocol, payload_error);
    expect(spawn_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("SpawnEnemyMessage", *spawn_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "stock clients must retain their source-defined targeted enemy-spawn broadcast");
    const auto speed_payload =
        encode_enemy_target_payload({31, static_cast<std::int32_t>(sender_gid)}, payload_error);
    expect(speed_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("SpeedUpEnemyMessage", *speed_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "the targeted player must be allowed to originate deterministic enemy decisions");
    const auto spoofed_speed_payload = encode_enemy_target_payload({31, 6}, payload_error);
    expect(spoofed_speed_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("SpeedUpEnemyMessage", *spoofed_speed_payload), 1U))
                       .error == ClientGameplayAuthorityError::identity_spoof,
           "a client may never retarget or accelerate another player's enemy identity");

    DragonWaypointPayload dragon_waypoint{
        {1.0F, 80.0F, 3.0F}, {-2.0F, 0.5F, -19.0F}, 4.0F, 0.25F, 1U, 0U};
    const auto dragon_update_payload = encode_update_dragon_payload(dragon_waypoint, payload_error);
    expect(dragon_update_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("UpdateDragonMessage", *dragon_update_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "the designated non-host dragon owner must be eligible to relay stock waypoints");
    const std::int32_t owner_fireball_id =
        static_cast<std::int32_t>((static_cast<std::uint32_t>(sender_gid) << 23U) | 17U);
    DragonAttackPayload dragon_attack{
        dragon_waypoint, {2.0F, 3.0F, 4.0F}, false, owner_fireball_id};
    const auto dragon_attack_payload = encode_dragon_attack_payload(dragon_attack, payload_error);
    expect(dragon_attack_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("DragonAttackMessage", *dragon_attack_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "a dragon attack must bind its owner-packed fireball identity to the admitted sender");
    dragon_attack.fireball_index = static_cast<std::int32_t>(6U << 23U);
    const auto spoofed_dragon_attack_payload =
        encode_dragon_attack_payload(dragon_attack, payload_error);
    expect(spoofed_dragon_attack_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol,
                   inbound(packet("DragonAttackMessage", *spoofed_dragon_attack_payload), 1U))
                       .error == ClientGameplayAuthorityError::identity_spoof,
           "a dragon owner may never launch a fireball under another peer's packed GID");
    DetonateFireballPayload fireball_detonation{
        {3.0F, 4.0F, 5.0F}, owner_fireball_id, 2U, {{1, 2, 3}}};
    const auto fireball_detonation_payload =
        encode_detonate_fireball_payload(fireball_detonation, payload_error);
    expect(fireball_detonation_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol,
                   inbound(packet("DetonateFireballMessage", *fireball_detonation_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "the owner-authoritative stock fireball detonation route must remain available");
    auto malformed_fireball_detonation = *fireball_detonation_payload;
    malformed_fireball_detonation.pop_back();
    expect(
        assess_client_gameplay_authority(
            protocol, inbound(packet("DetonateFireballMessage", malformed_fireball_detonation), 1U))
                .error == ClientGameplayAuthorityError::invalid_packet,
        "a truncated owner block list must fail before detonation relay");
    const auto existing_dragon_payload =
        encode_existing_dragon_payload({2U, 6U, true, 125.0F}, payload_error);
    expect(existing_dragon_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("ExistingDragonMessage", *existing_dragon_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "a dragon owner must be able to supply the source-defined late-join replica");
    MigrateDragonPayload migration;
    migration.migration.dragon_type = 2U;
    migration.target_gid = 6U;
    const auto migration_payload = encode_migrate_dragon_payload(migration, payload_error);
    expect(migration_payload.has_value() &&
               assess_client_gameplay_authority(
                   protocol, inbound(packet("MigrateDragonMessage", *migration_payload), 1U))
                       .action == ClientGameplayAction::relay,
           "the current non-host dragon brain must retain its ordered migration route");
    const auto remote_spawn = inbound(packet("SpawnDragonMessage", {}), 1U);
    expect(assess_client_gameplay_authority(protocol, remote_spawn).error ==
               ClientGameplayAuthorityError::host_only_message,
           "only host arbitration may introduce a new dragon encounter");

    PlayerExistsPayload response;
    response.avatar_description = {1U};
    response.gamer_tag = "Existing";
    response.player_id = {2U};
    const auto response_payload = encode_player_exists_payload(response, payload_error);
    expect(response_payload.has_value(), "the presence authority fixture must encode");
    const auto response_packet = packet("PlayerExistsMessage", *response_payload);
    expect(assess_client_gameplay_authority(protocol, inbound(response_packet, 1U)).action ==
               ClientGameplayAction::relay,
           "a response-only presence broadcast must reach the newcomer");
    response.request_response = true;
    const auto duplicate_payload = encode_player_exists_payload(response, payload_error);
    expect(duplicate_payload.has_value(), "the duplicate presence fixture must encode");
    expect(assess_client_gameplay_authority(
               protocol, inbound(packet("PlayerExistsMessage", *duplicate_payload), 1U))
                   .error == ClientGameplayAuthorityError::duplicate_presence,
           "a second response-requesting presence announcement must fail closed after bootstrap");

    const auto host_output = inbound(packet("WorldInfoMessage", {}), 1U);
    expect(assess_client_gameplay_authority(protocol, host_output).error ==
               ClientGameplayAuthorityError::host_only_message,
           "a remote client may never inject host-only world output");

    ClientGameplayRateLimiter limiter;
    bool first_ten_edits_allowed = true;
    for (int index = 0; index < 10; ++index)
        first_ten_edits_allowed =
            first_ten_edits_allowed && limiter.allow(1005U, "AlterBlockMessage", 0U);
    expect(first_ten_edits_allowed && !limiter.allow(1005U, "AlterBlockMessage", 0U) &&
               limiter.allow(1006U, "AlterBlockMessage", 0U) &&
               limiter.allow(1005U, "AlterBlockMessage", 1'000U),
           "the recovered ten-edit safety window must be isolated per peer and reset each second");
    expect(limiter.allow_spawner_trigger(10051U, 0U),
           "an exact decoded Modern trigger must open one bounded clear transcript");
    bool spawner_clear_allowed = true;
    for (int index = 0; index < 128; ++index)
        spawner_clear_allowed =
            spawner_clear_allowed && limiter.allow(10051U, "SpawnerClearAlterBlockMessage", 0U);
    expect(spawner_clear_allowed && !limiter.allow(10051U, "SpawnerClearAlterBlockMessage", 0U),
           "a trigger must authorize exactly the 128-edit bounded transcript budget");
    bool sixty_four_triggers_allowed = true;
    for (int index = 0; index < 64; ++index)
        sixty_four_triggers_allowed =
            sixty_four_triggers_allowed && limiter.allow_spawner_trigger(10052U, 0U);
    expect(sixty_four_triggers_allowed && !limiter.allow_spawner_trigger(10052U, 0U),
           "even exact linked-spawner announcements must remain bounded to 64 per peer-second");
    expect(limiter.allow(1005U, "DetonateRocketMessage", 1'000U) &&
               !limiter.allow(1005U, "DetonateGrenadeMessage", 1'000U),
           "rocket and grenade detonations must share the original one-per-second explosion cap");
    bool first_sixty_pickups_allowed = true;
    for (int index = 0; index < 60; ++index)
        first_sixty_pickups_allowed =
            first_sixty_pickups_allowed && limiter.allow(1007U, "CreatePickupMessage", 0U);
    expect(first_sixty_pickups_allowed && !limiter.allow(1007U, "CreatePickupMessage", 0U),
           "pickup creation must be capped independently at sixty messages per peer per second");
    bool first_thirty_requests_allowed = true;
    for (int index = 0; index < 30; ++index)
        first_thirty_requests_allowed =
            first_thirty_requests_allowed && limiter.allow(1008U, "RequestPickupMessage", 0U);
    expect(first_thirty_requests_allowed && !limiter.allow(1008U, "RequestPickupMessage", 0U),
           "pickup requests must be capped independently at thirty messages per peer per second");
    bool first_sixty_shots_allowed = true;
    for (int index = 0; index < 60; ++index)
        first_sixty_shots_allowed =
            first_sixty_shots_allowed && limiter.allow(1009U, "GunshotMessage", 0U);
    expect(first_sixty_shots_allowed && !limiter.allow(1009U, "GunshotMessage", 0U),
           "single-projectile gunfire must retain a bounded sixty-message peer window");
    bool first_ten_shotgun_shots_allowed = true;
    for (int index = 0; index < 10; ++index)
        first_ten_shotgun_shots_allowed =
            first_ten_shotgun_shots_allowed && limiter.allow(1010U, "ShotgunShotMessage", 0U);
    expect(first_ten_shotgun_shots_allowed && !limiter.allow(1010U, "ShotgunShotMessage", 0U),
           "five-pellet shotgun messages must have a separate ten-shot peer window");
    bool first_twelve_melee_attacks_allowed = true;
    for (int index = 0; index < 12; ++index)
        first_twelve_melee_attacks_allowed =
            first_twelve_melee_attacks_allowed && limiter.allow(10101U, "MeleePlayerMessage", 0U);
    expect(first_twelve_melee_attacks_allowed && !limiter.allow(10101U, "MeleePlayerMessage", 0U),
           "direct player melee must have a bounded twelve-hit peer window");
    expect(limiter.allow(1011U, "GrenadeMessage", 0U) &&
               limiter.allow(1011U, "FireRocketMessage", 0U) &&
               limiter.allow(1011U, "GrenadeMessage", 0U) &&
               limiter.allow(1011U, "FireRocketMessage", 0U) &&
               !limiter.allow(1011U, "GrenadeMessage", 0U),
           "grenade and rocket launches must share a four-projectile peer window");
    bool first_sixty_four_placed_detonations_allowed = true;
    for (int index = 0; index < 64; ++index)
        first_sixty_four_placed_detonations_allowed =
            first_sixty_four_placed_detonations_allowed &&
            limiter.allow(10111U, "DetonateExplosiveMessage", 0U);
    expect(first_sixty_four_placed_detonations_allowed &&
               !limiter.allow(10111U, "DetonateExplosiveMessage", 0U),
           "bounded TNT/C4 chains must retain up to 64 ordered child detonations per second");
    bool first_sixteen_explosive_effects_allowed = true;
    for (int index = 0; index < 16; ++index)
        first_sixteen_explosive_effects_allowed =
            first_sixteen_explosive_effects_allowed &&
            limiter.allow(
                10112U, index % 2 == 0 ? "AddExplosionEffectsMessage" : "AddExplosiveFlashMessage",
                0U);
    expect(first_sixteen_explosive_effects_allowed &&
               !limiter.allow(10112U, "AddExplosiveFlashMessage", 0U),
           "explosion and fuse visuals must share a bounded effects window");
    bool first_twelve_dig_effects_allowed = true;
    for (int index = 0; index < 12; ++index)
        first_twelve_dig_effects_allowed =
            first_twelve_dig_effects_allowed && limiter.allow(101121U, "DigMessage", 0U);
    expect(first_twelve_dig_effects_allowed && !limiter.allow(101121U, "DigMessage", 0U),
           "remote place/mine presentation must have a bounded twelve-effect peer window");
    bool first_twelve_door_events_allowed = true;
    for (int index = 0; index < 12; ++index)
        first_twelve_door_events_allowed =
            first_twelve_door_events_allowed && limiter.allow(10113U, "DoorOpenCloseMessage", 0U);
    expect(first_twelve_door_events_allowed && !limiter.allow(10113U, "DoorOpenCloseMessage", 0U),
           "door sound broadcasts must have a bounded per-peer window");
    bool first_sixteen_custom_blocks_allowed = true;
    for (int index = 0; index < 16; ++index)
        first_sixteen_custom_blocks_allowed =
            first_sixteen_custom_blocks_allowed &&
            limiter.allow(10114U, "DestroyCustomBlockMessage", 0U);
    expect(first_sixteen_custom_blocks_allowed &&
               !limiter.allow(10114U, "DestroyCustomBlockMessage", 0U),
           "Modern custom-door destruction must have a bounded per-peer window");
    bool first_sixty_four_enemy_spawns_allowed = true;
    for (int index = 0; index < 64; ++index)
        first_sixty_four_enemy_spawns_allowed =
            first_sixty_four_enemy_spawns_allowed && limiter.allow(1012U, "SpawnEnemyMessage", 0U);
    expect(first_sixty_four_enemy_spawns_allowed && !limiter.allow(1012U, "SpawnEnemyMessage", 0U),
           "linked Modern spawners must fit a bounded 64-spawn peer window");
    bool first_thirty_enemy_events_allowed = true;
    for (int index = 0; index < 30; ++index)
        first_thirty_enemy_events_allowed =
            first_thirty_enemy_events_allowed && limiter.allow(1013U, "KillEnemyMessage", 0U);
    expect(first_thirty_enemy_events_allowed && !limiter.allow(1013U, "EnemyGiveUpMessage", 0U),
           "kill, speed-up, and give-up traffic must share one bounded enemy-event window");
    bool first_eight_dragon_updates_allowed = true;
    for (int index = 0; index < 8; ++index)
        first_eight_dragon_updates_allowed =
            first_eight_dragon_updates_allowed && limiter.allow(1014U, "UpdateDragonMessage", 0U);
    expect(first_eight_dragon_updates_allowed && !limiter.allow(1014U, "UpdateDragonMessage", 0U),
           "the five-hertz dragon stream must retain bounded warm-up headroom per owner");
    bool first_eight_dragon_events_allowed = true;
    for (int index = 0; index < 7; ++index)
        first_eight_dragon_events_allowed =
            first_eight_dragon_events_allowed && limiter.allow(1015U, "DragonAttackMessage", 0U);
    expect(first_eight_dragon_events_allowed &&
               limiter.allow(1015U, "DetonateFireballMessage", 0U) &&
               !limiter.allow(1015U, "RemoveDragonMessage", 0U),
           "attack, detonation, existing-state, migration, and removal must share a bounded "
           "dragon-event window");
    limiter.disconnect(1005U);
    expect(limiter.allow(1005U, "DetonateGrenadeMessage", 1'000U),
           "disconnect must discard every rate-window generation for that peer");
}

void player_replication_packs_original_angles_half_velocity_and_delivery()
{
    using namespace openminerz;
    using namespace openminerz::net;

    std::uint16_t packed{};
    expect(pack_half_single(0.0F, packed) && packed == 0x0000U && pack_half_single(1.0F, packed) &&
               packed == 0x3C00U && pack_half_single(-2.0F, packed) && packed == 0xC000U &&
               pack_half_single(65504.0F, packed) && packed == 0x7BFFU &&
               near(unpack_half_single(0x3555U), 0.33325195F, 0.000001F),
           "HalfSingle conversion must match the XNA binary16 wire representation");
    expect(!pack_half_single(std::numeric_limits<float>::infinity(), packed) &&
               !pack_half_single(100000.0F, packed),
           "non-finite and binary16-overflow velocities must fail before framing");

    LocalPlayerReplicationInput input;
    input.position = {12.5F, 4.0F, -7.25F};
    input.velocity = {1.0F, -2.0F, 0.5F};
    input.yaw = std::numbers::pi_v<float> * 0.5F;
    input.torso_pitch = 0.0F;
    input.movement_x = 1.0F;
    input.movement_y = -1.0F;
    input.animation_mode = PlayerAnimationMode::pistol;
    input.using_tool = true;
    input.shouldering = true;
    PlayerUpdatePayload payload;
    PayloadError error{};
    expect(make_player_update(input, ProtocolVersion::classic_v3, payload, error) &&
               payload.yaw == 64U && payload.torso_pitch == 128U &&
               payload.packed_movement == 0x0EU && payload.player_mode == 6U &&
               payload.velocity_x == 0x3C00U && payload.velocity_y == 0xC000U &&
               payload.velocity_z == 0x3800U &&
               player_update_delivery(payload) == MessageDeliveryPolicy::reliable_ordered,
           "local state must quantize exactly like PlayerUpdateMessage.SendData");

    RemotePlayerReplicationState restored;
    expect(apply_player_update(payload, ProtocolVersion::classic_v3, restored) &&
               near(restored.position.x, input.position.x) &&
               near(restored.position.z, input.position.z) &&
               near(restored.velocity.x, input.velocity.x) &&
               near(restored.velocity.y, input.velocity.y) &&
               near(restored.yaw, input.yaw, 0.013F) && restored.using_tool &&
               restored.shouldering && restored.animation_mode == PlayerAnimationMode::pistol,
           "remote application must invert every packed movement field without state aliasing");
    payload.state_flags = 0U;
    expect(player_update_delivery(payload) == MessageDeliveryPolicy::unreliable_sequenced,
           "idle movement must retain the original InOrder/unreliable-sequenced path");
}

void paired_loopback_runs_admission_channels_typed_validation_and_delivery()
{
    using namespace openminerz::net;
    HostSessionSettings vanilla;
    LoopbackSession session{vanilla, false, 1, 1, false};
    PeerAdmissionRequest stock;
    stock.peer = 41U;
    stock.properties = session.admission().advertised_properties();
    expect(session.connect(stock).status == AdmissionStatus::admitted,
           "a vanilla stock-compatible peer must complete loopback admission");

    PayloadError payload_error{};
    PacketError packet_error{};
    const auto chat_payload = encode_chat_payload("hello host", payload_error);
    const MessageDescriptor *chat_descriptor =
        message_by_name(ProtocolVersion::classic_v3, "ChatMessage");
    expect(chat_payload.has_value() && chat_descriptor != nullptr,
           "chat fixture requires the exact v3 manifest entry");
    const auto chat_packet = encode_packet(ProtocolVersion::classic_v3, chat_descriptor->id,
                                           *chat_payload, packet_error);
    expect(chat_packet.has_value(), "chat fixture must receive stock framing");
    expect(session.submit_from_peer(stock.peer, *chat_packet,
                                    MessageDeliveryPolicy::reliable_ordered, stock_gameplay_channel)
                   .error == SessionPacketError::delivery_mismatch,
           "loopback must reject a structurally valid message on the wrong delivery class");
    expect(session
               .submit_from_peer(stock.peer, *chat_packet, MessageDeliveryPolicy::chat,
                                 stock_gameplay_channel)
               .accepted(),
           "admitted chat on the stock Chat channel must reach the host queue");
    const auto host_chat = session.receive_for_host();
    expect(host_chat.has_value() && host_chat->sender == stock.peer && host_chat->recipient == 0U &&
               host_chat->delivery == MessageDeliveryPolicy::chat &&
               host_chat->payload == *chat_packet,
           "paired loopback must retain direction, delivery, and exact application bytes");

    auto corrupt_chat = *chat_packet;
    corrupt_chat.back() ^= 1U;
    expect(session.submit_from_peer(stock.peer, corrupt_chat, MessageDeliveryPolicy::chat,
                                    stock_gameplay_channel)
                   .error == SessionPacketError::frame_rejected,
           "corrupt checksums must stop before typed parsing or host delivery");
    const MessageDescriptor *alter =
        message_by_name(ProtocolVersion::classic_v3, "DragonAttackMessage");
    const auto unsupported_packet =
        encode_packet(ProtocolVersion::classic_v3, alter->id, {}, packet_error);
    expect(unsupported_packet.has_value() &&
               session.submit_from_peer(stock.peer, *unsupported_packet,
                                        MessageDeliveryPolicy::reliable_ordered,
                                        stock_gameplay_channel)
                       .error == SessionPacketError::payload_rejected,
           "a malformed registered payload must fail closed in the session pump");

    PlayerUpdatePayload movement;
    movement.local_position = {1.0F, 2.0F, 3.0F};
    movement.packed_movement = 0x77U;
    const auto movement_payload =
        encode_player_update_payload(movement, ProtocolVersion::classic_v3, payload_error);
    const MessageDescriptor *movement_descriptor =
        message_by_name(ProtocolVersion::classic_v3, "PlayerUpdateMessage");
    const auto movement_packet = encode_packet(ProtocolVersion::classic_v3, movement_descriptor->id,
                                               *movement_payload, packet_error);
    expect(session
               .submit_from_peer(stock.peer, *movement_packet,
                                 MessageDeliveryPolicy::unreliable_sequenced,
                                 stock_gameplay_channel)
               .accepted(),
           "ordinary PlayerUpdate must use the original sequenced-unreliable path");
    movement.state_flags = PlayerUpdatePayload::using_flag;
    const auto using_payload =
        encode_player_update_payload(movement, ProtocolVersion::classic_v3, payload_error);
    const auto using_packet = encode_packet(ProtocolVersion::classic_v3, movement_descriptor->id,
                                            *using_payload, packet_error);
    expect(session.submit_from_peer(stock.peer, *using_packet,
                                    MessageDeliveryPolicy::unreliable_sequenced,
                                    stock_gameplay_channel)
                       .error == SessionPacketError::delivery_mismatch &&
               session
                   .submit_from_peer(stock.peer, *using_packet,
                                     MessageDeliveryPolicy::reliable_ordered,
                                     stock_gameplay_channel)
                   .accepted(),
           "using/reloading PlayerUpdate frames must switch to reliable ordered delivery");

    expect(
        session.submit_from_host(stock.peer, *chat_packet, MessageDeliveryPolicy::chat).accepted(),
        "host-to-peer typed gameplay must use the same framing and delivery gate");
    session.disconnect(stock.peer);
    expect(!session.receive_for_peer(stock.peer).has_value() &&
               !session.admission().may_forward_gameplay(stock.peer),
           "disconnect must purge queued packets so stale session data cannot reach a reconnect");

    std::array<std::uint8_t, 32U> digest{};
    digest.fill(0x66U);
    HostSessionSettings modded;
    modded.mod_manifest.mods.push_back({"openminerz.behavior", "1", digest, ModScope::server});
    LoopbackSession mod_session{modded, false, 1, 1, false};
    PeerAdmissionRequest openminer;
    openminer.peer = 52U;
    openminer.properties = mod_session.admission().advertised_properties();
    openminer.advertised_extension = true;
    expect(mod_session.connect(openminer).status == AdmissionStatus::awaiting_extension &&
               mod_session
                       .submit_from_peer(openminer.peer, *chat_packet, MessageDeliveryPolicy::chat,
                                         stock_gameplay_channel)
                       .error == SessionPacketError::peer_not_admitted,
           "gameplay must stay closed while a modded peer owes its extension hello");
    ExtensionMessage hello{ExtensionMessageKind::client_hello, 1234U, modded.mod_manifest, {}};
    const auto hello_packet = encode_extension_message(hello);
    expect(mod_session
               .submit_from_peer(openminer.peer, hello_packet,
                                 MessageDeliveryPolicy::reliable_ordered,
                                 openminerz_control_channel)
               .accepted(),
           "the capability-gated control channel must complete exact mod admission");
    const auto acceptance = mod_session.receive_for_peer(openminer.peer);
    ExtensionMessage acceptance_message;
    ExtensionDecodeError extension_error{};
    expect(acceptance.has_value() && acceptance->channel == openminerz_control_channel &&
               decode_extension_message(acceptance->payload, acceptance_message, extension_error) &&
               acceptance_message.kind == ExtensionMessageKind::server_accept &&
               mod_session
                   .submit_from_peer(openminer.peer, *chat_packet, MessageDeliveryPolicy::chat,
                                     stock_gameplay_channel)
                   .accepted(),
           "only the correlated server acceptance may open modded gameplay forwarding");
}

void legacy_random_and_atlas_match_original_contracts()
{
    openminerz::world::LegacyRandom random{1};
    constexpr int expected[]{63, 28, 119, 197, 168, 110, 90, 241};
    for (const int value : expected)
    {
        expect(random.next(256) == value, "legacy random must match .NET Framework seeded output");
    }
    const openminerz::world::PerlinNoise perlin{1337};
    expect(std::abs(perlin.noise(0.1F, 0.2F) - 0.0364714861F) < 0.000001F &&
               std::abs(perlin.noise(1.25F, -2.5F) - 0.0129394531F) < 0.000001F &&
               std::abs(perlin.noise(9.375F, 4.125F) - 0.0398383625F) < 0.000001F,
           "Perlin samples must match the installed DNA.Common implementation");
    const openminerz::world::IntegerNoise integer_noise{1337};
    expect(integer_noise.noise(4, -9, 17) == 11 && integer_noise.noise(1000, 1000, 1000) == 42,
           "integer-noise samples must match the installed DNA.Common implementation");
    using enum openminerz::world::FaceDirection;
    using openminerz::world::BlockType;
    expect(static_cast<int>(BlockType::slime) == 43,
           "block values must remain compatible with the original save contract");
    expect(static_cast<int>(BlockType::glass_basic) == 45 &&
               static_cast<int>(BlockType::alien_horde_dim) == 93 &&
               static_cast<int>(BlockType::uninitialized) == 94,
           "Modern block ordinals and its uninitialized sentinel must match the shipped table");
    expect(openminerz::world::light_transmission(BlockType::glass_basic) == 12U &&
               openminerz::world::light_transmission(BlockType::glass_strong) == 15U &&
               openminerz::world::self_illumination(BlockType::boss_spawn_off) == 13U &&
               openminerz::world::atlas_tile(BlockType::alien_horde_dim, positive_y) == 50U &&
               openminerz::world::bounces_lasers(BlockType::glass_mystery) &&
               !openminerz::world::blocks_player(BlockType::strong_lower_door_open_z),
           "Modern block lighting, atlas, laser and strong-door contracts must be data exact");
    expect(openminerz::world::atlas_tile(BlockType::grass, positive_y) == 2U &&
               openminerz::world::atlas_tile(BlockType::grass, positive_x) == 1U &&
               openminerz::world::atlas_tile(BlockType::grass, negative_y) == 0U,
           "grass must preserve the original top, side, and bottom atlas tiles");
    const auto wall_uv = openminerz::world::atlas_uv_corners(positive_x, 0.0F, 0.0F, 1.0F, 1.0F);
    const auto top_uv = openminerz::world::atlas_uv_corners(positive_y, 0.0F, 0.0F, 1.0F, 1.0F);
    expect(wall_uv[0][0] == 1.0F && wall_uv[0][1] == 1.0F && wall_uv[1][1] == 0.0F,
           "positive-X walls must keep vanilla's grass strip horizontal instead of rotating it");
    expect(top_uv[0][0] == 0.0F && top_uv[0][1] == 0.0F && top_uv[2][0] == 1.0F &&
               top_uv[2][1] == 1.0F,
           "top faces must use vanilla's unrotated atlas orientation");
    expect(openminerz::world::block_name(BlockType::lantern) == "LANTERN" &&
               openminerz::world::block_name(BlockType::tnt) == "TNT" &&
               openminerz::world::block_name(BlockType::diamond_ore) == "DIAMOND ORE",
           "gameplay widgets must expose specific names for original materials");
}

void world_generation_is_deterministic_and_layered()
{
    const openminerz::world::WorldGenerator generator{1337};
    const auto first = generator.generate_chunk(0, 0);
    const auto second = generator.generate_chunk(0, 0);
    expect(first.hash() == second.hash(), "same seed and location must produce identical chunks");
    expect(first.block(0, 124, 0) == openminerz::world::BlockType::fixed_lantern,
           "the origin tower must terminate in the original fixed lantern");
    expect(first.count(openminerz::world::BlockType::bedrock) > 100U,
           "bedrock and the origin tower must be deposited after caves and ores");
    const openminerz::world::WorldGenerator modern_scavenger{
        123456789, openminerz::world::TerrainProfile::modern,
        openminerz::GameMode::scavenger};
    std::size_t modern_loot_count{};
    std::size_t modern_lucky_count{};
    for (int chunk_z = -8; chunk_z <= 8 && modern_lucky_count == 0U; ++chunk_z)
        for (int chunk_x = -8; chunk_x <= 8 && modern_lucky_count == 0U; ++chunk_x)
        {
            const auto modern_chunk = modern_scavenger.generate_chunk(chunk_x, chunk_z);
            modern_loot_count += modern_chunk.count(openminerz::world::BlockType::loot_block);
            modern_lucky_count +=
                modern_chunk.count(openminerz::world::BlockType::lucky_loot_block);
        }
    expect(modern_scavenger.profile() == openminerz::world::TerrainProfile::modern &&
               modern_scavenger.game_mode() == openminerz::GameMode::scavenger &&
               modern_loot_count > 0U && modern_lucky_count > 0U &&
               first.count(openminerz::world::BlockType::loot_block) == 0U &&
               first.count(openminerz::world::BlockType::lucky_loot_block) == 0U,
           "Modern Scavenger must reconstruct its untouched Loot/Lucky baseline while Classic "
           "terrain retains the original pre-v4 block table");
    expect(generator.biome_at(100, 0) == openminerz::world::BiomeKind::classic &&
               generator.biome_at(600, 0) == openminerz::world::BiomeKind::lagoon &&
               generator.biome_at(1200, 0) == openminerz::world::BiomeKind::desert &&
               generator.biome_at(1900, 0) == openminerz::world::BiomeKind::mountain &&
               generator.biome_at(2600, 0) == openminerz::world::BiomeKind::arctic,
           "radial biome bands must retain the recovered original order");
    const auto crash_site = generator.find_crash_site_column(0, 0, 3600);
    expect(crash_site.has_value(),
           "the seeded crash-site field must expose a deterministic positive-rim column");
    if (crash_site.has_value())
    {
        const int world_x = (*crash_site)[0];
        const int world_z = (*crash_site)[1];
        const auto floor_chunk = [](int coordinate) {
            return coordinate >= 0 ? coordinate / openminerz::world::chunk_width
                                   : (coordinate - openminerz::world::chunk_width + 1) /
                                         openminerz::world::chunk_width;
        };
        const int chunk_x = floor_chunk(world_x);
        const int chunk_z = floor_chunk(world_z);
        const auto crash_chunk = generator.generate_chunk(chunk_x, chunk_z);
        const int local_x = world_x - chunk_x * openminerz::world::chunk_width;
        const int local_z = world_z - chunk_z * openminerz::world::chunk_depth;
        bool space_rock_column{};
        for (int y = 0; y < openminerz::world::chunk_height; ++y)
            space_rock_column = space_rock_column || crash_chunk.block(local_x, y, local_z) ==
                                                         openminerz::world::BlockType::space_rock;
        expect(space_rock_column,
               "the crash-site locator must point at a generated space-rock rim, not noise alone");
    }

    auto region = generator.generate_region(2, 2, 2, 2);
    const auto faces = region.visible_faces();
    expect(!faces.empty(), "generated voxel regions must produce an exposed-face mesh contract");
    const auto section_batches = region.visible_faces_by_chunk();
    expect(section_batches.size() ==
               region.chunks().size() *
                   static_cast<std::size_t>(openminerz::world::chunk_section_count),
           "terrain mesh work must be partitioned into bounded 16-block vertical sections");
    expect(
        std::all_of(faces.begin(), faces.end(), [](const auto &face) { return face.tile < 64U; }),
        "all visible terrain faces must address the original 8x8 atlas");

    openminerz::world::WorldRegion streaming_window{0, 0, 1, 1};
    streaming_window.set_block(0, 0, 0, openminerz::world::BlockType::rock);
    const auto boundary_faces = streaming_window.visible_faces();
    expect(std::none_of(boundary_faces.begin(), boundary_faces.end(),
                        [](const auto &face) {
                            return face.direction == openminerz::world::FaceDirection::negative_x ||
                                   face.direction == openminerz::world::FaceDirection::negative_z;
                        }),
           "unloaded streaming boundaries must remain sealed instead of exposing world walls");
    expect(near(openminerz::world::water_level, 1.5F),
           "renderer-owned water must retain the recovered world level");

    auto snapshot = region;
    constexpr int copy_x = 2 * openminerz::world::chunk_width + 1;
    constexpr int copy_z = 2 * openminerz::world::chunk_depth + 1;
    const auto snapshot_block = snapshot.block(copy_x, 0, copy_z);
    const auto replacement = snapshot_block == openminerz::world::BlockType::wood
                                 ? openminerz::world::BlockType::rock
                                 : openminerz::world::BlockType::wood;
    region.set_block(copy_x, 0, copy_z, replacement);
    expect(snapshot.block(copy_x, 0, copy_z) == snapshot_block &&
               region.block(copy_x, 0, copy_z) == replacement,
           "streaming snapshots must share untouched storage but isolate edits with copy-on-write");

    region.set_block(3 * openminerz::world::chunk_width + 1, 0,
                     2 * openminerz::world::chunk_depth + 1, openminerz::world::BlockType::wood);
    const std::uint64_t retained_hash = region.chunks()[1].hash();
    const auto shifted = generator.stream_region(region, 3, 2);
    expect(shifted.chunks()[0].hash() == retained_hash,
           "streaming must retain overlapping chunks and their player edits");
    expect(shifted.chunks()[1].hash() == generator.generate_chunk(4, 2).hash(),
           "streaming must deterministically generate only newly entered chunks");
}

void day_night_cycle_preserves_original_timing()
{
    openminerz::world::DayNightCycle cycle;
    expect(near(cycle.time_of_day(), 0.41F), "new worlds must start at the original day fraction");
    expect(cycle.clock_text() == "9:45 AM", "HUD clock must use original 15-minute quantization");
    cycle.set_time(19.5F / 24.0F);
    expect(cycle.state().period == "SUNSET", "19:30 must select the sunset lighting pack");
    cycle.set_time(12.0F / 24.0F);
    expect(near(cycle.state().specular.red, 1.0F) && near(cycle.state().specular.blue, 1.0F),
           "daylight must preserve vanilla's white SunSpecular pack");
    cycle.set_time(2.0F / 24.0F);
    expect(near(cycle.state().specular.red, 100.0F / 255.0F) &&
               near(cycle.state().specular.blue, 214.0F / 255.0F),
           "night lighting must preserve vanilla's cool blue SunSpecular pack");
    expect(cycle.state().sun_y > 0.0F,
           "the default/classic sky model must preserve vanilla's always-positive sun arc");

    cycle.set_time(12.0F / 24.0F);
    const auto rebirth_noon = cycle.state(openminerz::world::SkyModel::rebirth);
    const float noon_sun_length = std::sqrt(rebirth_noon.sun_x * rebirth_noon.sun_x +
                                            rebirth_noon.sun_y * rebirth_noon.sun_y +
                                            rebirth_noon.sun_z * rebirth_noon.sun_z);
    expect(near(noon_sun_length, 1.0F) && rebirth_noon.sun_y > 0.9F &&
               rebirth_noon.daylight > 0.99F && rebirth_noon.direct.red > 0.95F,
           "Rebirth noon must use a normalized high sun and full direct daylight");
    cycle.set_time(0.0F);
    const auto rebirth_midnight = cycle.state(openminerz::world::SkyModel::rebirth);
    expect(rebirth_midnight.sun_y < -0.9F && rebirth_midnight.daylight < 0.001F &&
               rebirth_midnight.direct.red < 0.001F && rebirth_midnight.specular.blue < 0.001F,
           "Rebirth midnight must put the sun below the horizon and remove direct/specular sun");
    cycle.set_time(6.0F / 24.0F);
    const auto rebirth_sunrise = cycle.state(openminerz::world::SkyModel::rebirth);
    cycle.set_time(18.0F / 24.0F);
    const auto rebirth_sunset = cycle.state(openminerz::world::SkyModel::rebirth);
    expect(std::abs(rebirth_sunrise.sun_y) < 0.001F && std::abs(rebirth_sunset.sun_y) < 0.001F &&
               rebirth_sunrise.sun_x > 0.99F && rebirth_sunset.sun_x < -0.99F,
           "Rebirth sun must cross opposite horizons at 06:00 and 18:00");
    const float before = cycle.day();
    cycle.update(960.0F);
    expect(near(cycle.day(), before + 1.0F), "one original day must last exactly 16 minutes");

    openminerz::world::LightningCycle weather{1337U};
    bool covered_flash{};
    bool covered_thunder{};
    for (int frame = 0; frame < 60 * 120; ++frame)
    {
        const auto event = weather.update(1.0F / 60.0F, 3, 10.0F);
        covered_flash = covered_flash || event.visible;
        covered_thunder = covered_thunder || event.thunder_low || event.thunder_high;
    }
    expect(!covered_flash && !covered_thunder,
           "more than two blocks of cover must freeze the original lightning timers");
    bool saw_flash{};
    bool heard_low{};
    bool heard_high{};
    for (int frame = 0; frame < 60 * 180; ++frame)
    {
        const auto event = weather.update(1.0F / 60.0F, 0, 10.0F);
        saw_flash = saw_flash || event.visible;
        heard_low = heard_low || event.thunder_low;
        heard_high = heard_high || event.thunder_high;
    }
    expect(saw_flash && heard_low && !heard_high,
           "surface storms must flash and preserve the shipped low-only thunder state machine");
}

void terrain_vertices_carry_original_style_lighting()
{
    using openminerz::world::BlockType;
    using openminerz::world::FaceDirection;
    openminerz::world::WorldRegion region{0, 0, 1, 1};
    region.set_block(8, 0, 8, BlockType::rock);
    region.set_block(8, 2, 8, BlockType::rock);
    region.set_block(7, 1, 8, BlockType::rock);
    region.set_block(8, 1, 7, BlockType::rock);

    const auto faces = region.visible_faces();
    const auto top = std::find_if(faces.begin(), faces.end(), [](const auto &face) {
        return face.x == 8 && face.y == 0 && face.z == 8 &&
               face.direction == FaceDirection::positive_y;
    });
    expect(top != faces.end(), "the lighting fixture block must retain its exposed top face");
    expect(top->ambient_occlusion[0] < top->ambient_occlusion[2],
           "two occupied corner neighbours must darken only the matching face vertex");
    expect(std::all_of(top->sky_light.begin(), top->sky_light.end(),
                       [](std::uint8_t light) { return light < 255U; }),
           "a ceiling must reduce skylight below a covered surface");
    const auto bounded = region.visible_faces_by_chunk({{0, 0, 4}});
    expect(bounded.size() == 1U,
           "a block edit must rebuild only its requested 16-high mesh section");
    const auto bounded_top = std::find_if(bounded.front().faces.begin(),
                                          bounded.front().faces.end(), [](const auto &face) {
                                              return face.x == 8 && face.y == 0 && face.z == 8 &&
                                                     face.direction == FaceDirection::positive_y;
                                          });
    expect(bounded_top != bounded.front().faces.end() &&
               bounded_top->ambient_occlusion == top->ambient_occlusion &&
               bounded_top->sky_light == top->sky_light &&
               bounded_top->indirect_light == top->indirect_light &&
               bounded_top->block_light_lobe == top->block_light_lobe,
           "compact edit lighting must match a full-region mesh without full-region allocation");
    expect(openminerz::world::emits_light(BlockType::fixed_lantern) &&
               openminerz::world::emits_light(BlockType::deep_lava) &&
               !openminerz::world::emits_light(BlockType::rock),
           "original lantern and lava materials must keep their full-bright contract");
}

void directional_indirect_light_bakes_color_and_emissive_bounce()
{
    using openminerz::world::BlockType;
    using openminerz::world::FaceDirection;
    using openminerz::world::WorldRegion;

    const auto sample_receiver = [](BlockType reflector, bool place_torch) {
        WorldRegion region{0, 0, 2, 1};
        // The receiver looks through a one-cell gap at a five-by-five panel. The
        // nine outward-hemisphere probe rays all hit the panel at their second
        // step across a chunk boundary, making both the reflected color and the
        // compact-rebuild apron deterministic.
        region.set_block(14, 8, 8, BlockType::rock);
        if (reflector != BlockType::empty)
            for (int y = 6; y <= 10; ++y)
                for (int z = 6; z <= 10; ++z)
                    region.set_block(16, y, z, reflector);
        if (place_torch)
            region.set_block(15, 8, 8, BlockType::torch);

        const auto faces = region.visible_faces();
        const auto receiver = std::find_if(faces.begin(), faces.end(), [](const auto &face) {
            return face.x == 14 && face.y == 8 && face.z == 8 &&
                   face.direction == FaceDirection::positive_x;
        });
        expect(receiver != faces.end(), "the indirect-light receiver face must remain exposed");
        const auto result =
            receiver == faces.end() ? std::array<std::uint8_t, 4>{} : receiver->indirect_light;
        if (reflector == BlockType::snow && !place_torch)
        {
            const auto compact = region.visible_faces_by_chunk({{0, 0, 4}});
            const auto compact_receiver =
                compact.empty()
                    ? std::vector<openminerz::world::WorldFace>::const_iterator{}
                    : std::find_if(compact.front().faces.begin(), compact.front().faces.end(),
                                   [](const auto &face) {
                                       return face.x == 14 && face.y == 8 && face.z == 8 &&
                                              face.direction == FaceDirection::positive_x;
                                   });
            expect(!compact.empty() && compact_receiver != compact.front().faces.end() &&
                       compact_receiver->indirect_light == result,
                   "a compact mesh section must reproduce cross-chunk indirect radiance exactly");
        }
        return result;
    };

    const auto open = sample_receiver(BlockType::empty, false);
    const auto snow = sample_receiver(BlockType::snow, false);
    const auto blood = sample_receiver(BlockType::blood_stone, false);
    const auto torch_rock = sample_receiver(BlockType::rock, true);
    const auto decode_radiance = [](std::uint8_t packed) {
        const float companded = static_cast<float>(packed) / 255.0F;
        return companded * companded;
    };
    expect(open == std::array<std::uint8_t, 4>{},
           "an open probe hemisphere must not invent reflected radiance");
    expect(snow[0] > 100U && snow[1] > 100U && snow[2] > 100U &&
               std::abs(static_cast<int>(snow[0]) - static_cast<int>(snow[2])) < 50,
           "a sunlit snow panel must bake a strong near-neutral sky bounce");
    expect(decode_radiance(blood[0]) > decode_radiance(blood[1]) * 3.0F &&
               decode_radiance(blood[0]) > decode_radiance(blood[2]) * 2.0F,
           "blood-stone reflection must bake directional red color bleeding");
    expect(torch_rock[3] > 0U,
           "an emissive source facing a nearby surface must bake a torch-bounce coefficient");

    const auto sample_two_surface_path = [](BlockType secondary_reflector) {
        WorldRegion region{0, 0, 2, 1};
        region.set_block(14, 8, 8, BlockType::rock);
        for (int y = 6; y <= 10; ++y)
            for (int z = 6; z <= 10; ++z)
                region.set_block(16, y, z, BlockType::snow);
        if (secondary_reflector != BlockType::empty)
        {
            // The snow panel sees this surface in its local outward hemisphere;
            // the centre stays the receiver itself. Light must therefore travel
            // source -> secondary panel -> snow panel -> receiver.
            for (int y = 6; y <= 10; ++y)
                for (int z = 6; z <= 10; ++z)
                    if (y != 8 || z != 8)
                        region.set_block(14, y, z, secondary_reflector);
        }
        const auto faces = region.visible_faces();
        const auto receiver = std::find_if(faces.begin(), faces.end(), [](const auto &face) {
            return face.x == 14 && face.y == 8 && face.z == 8 &&
                   face.direction == FaceDirection::positive_x;
        });
        expect(receiver != faces.end(), "the two-surface receiver must remain exposed");
        return receiver == faces.end() ? std::array<std::uint8_t, 4>{} : receiver->indirect_light;
    };
    const auto neutral_two_surface = sample_two_surface_path(BlockType::empty);
    const auto red_two_surface = sample_two_surface_path(BlockType::blood_stone);
    expect(decode_radiance(red_two_surface[0]) > decode_radiance(neutral_two_surface[0]) &&
               decode_radiance(red_two_surface[0]) - decode_radiance(neutral_two_surface[0]) >
                   decode_radiance(red_two_surface[1]) - decode_radiance(neutral_two_surface[1]),
           "a second reflected surface must add its color through a genuine two-bounce path");
}

void player_physics_and_block_interaction_are_voxel_deterministic()
{
    using openminerz::world::BlockType;
    expect(near(openminerz::world::water_depth(1.5F), 0.0F) &&
               near(openminerz::world::water_depth(0.5F), 1.0F) &&
               openminerz::world::is_underwater(true, 0.0F) &&
               !openminerz::world::is_underwater(true, 0.001F) &&
               !openminerz::world::is_underwater(false, -20.0F),
           "water submersion must use the original feet-depth and water-world gate");
    expect(near(openminerz::world::lava_submersion(-59.0F), 0.0F) &&
               near(openminerz::world::lava_submersion(-61.0F), 0.5F) &&
               near(openminerz::world::lava_submersion(-62.0F), 1.0F) &&
               openminerz::world::is_under_lava(-62.0F) &&
               !openminerz::world::is_under_lava(-61.999F),
           "lava submersion must preserve the original global -60 to -62 transition");
    expect(near(openminerz::world::oxygen_decay_per_second, 0.1F) &&
               near(openminerz::world::drowning_health_per_second, 20.0F) &&
               near(openminerz::world::lava_damage_period_seconds, 0.5F) &&
               near(openminerz::world::lava_damage_health, 25.0F),
           "environmental survival rates must match InGameHUD's normalized health timers");
    expect(openminerz::world::blocks_player(BlockType::rock) &&
               !openminerz::world::blocks_player(BlockType::leaves) &&
               !openminerz::world::blocks_player(BlockType::surface_lava) &&
               !openminerz::world::blocks_player(BlockType::deep_lava),
           "leaves and both lava variants must be pass-through player volumes");
    expect(openminerz::world::is_solid(BlockType::leaves) &&
               openminerz::world::is_solid(BlockType::surface_lava) &&
               openminerz::world::is_solid(BlockType::deep_lava),
           "pass-through voxels must remain rendered, touchable block identities");

    openminerz::world::WorldRegion trace_world{0, 0, 1, 1};
    trace_world.set_block(2, 2, 2, BlockType::leaves);
    trace_world.set_block(4, 2, 2, BlockType::rock);
    const openminerz::world::Vec3 trace_origin{0.5F, 2.5F, 2.5F};
    const openminerz::world::Vec3 trace_direction{1.0F, 0.0F, 0.0F};
    const auto touchable_hit = openminerz::world::PlayerSimulation::raycast(
        trace_world, trace_origin, trace_direction, 8.0F);
    const auto blocking_hit = openminerz::world::PlayerSimulation::raycast_blocking(
        trace_world, trace_origin, trace_direction, 8.0F);
    expect(touchable_hit && touchable_hit->block == BlockType::leaves && blocking_hit &&
               blocking_hit->block == BlockType::rock,
           "physics rays must pass through leaves while mining rays still select them");
    expect(openminerz::world::mined_drop(BlockType::grass) == BlockType::dirt &&
               openminerz::world::mined_drop(BlockType::surface_lava) == BlockType::rock &&
               openminerz::world::mined_drop(BlockType::space_rock) ==
                   BlockType::space_rock_inventory &&
               openminerz::world::mined_drop(BlockType::bedrock) == BlockType::empty,
           "mined drops must follow the original CreatesWhenDug conversion contract");

    openminerz::world::WorldRegion world{0, 0, 1, 1};
    for (int z = 0; z < openminerz::world::chunk_depth; ++z)
    {
        for (int x = 0; x < openminerz::world::chunk_width; ++x)
        {
            world.set_block(x, 0, z, openminerz::world::BlockType::rock);
        }
    }
    openminerz::world::PlayerSimulation player;
    player.spawn(world, 5, 5);
    expect(near(player.state().position.y, 1.001F, 0.002F),
           "player feet must spawn immediately above the highest safe voxel");
    for (int frame = 0; frame < 120; ++frame)
    {
        player.update(world, {}, 1.0F / 60.0F, false);
    }
    expect(player.state().grounded && near(player.state().position.y, 1.001F, 0.01F),
           "gravity and the original 0.7x1.7 AABB must settle on a voxel floor");

    const float walking_start = player.state().position.z;
    openminerz::world::PlayerInput vanilla_walk;
    vanilla_walk.forward = 1.0F;
    for (int frame = 0; frame < 60; ++frame)
    {
        player.update(world, vanilla_walk, 1.0F / 60.0F, false);
    }
    expect(near(walking_start - player.state().position.z, 5.0F, 0.02F),
           "vanilla keyboard movement must preserve FPSRig's five-unit speed");

    player.spawn(world, 5, 5);
    player.state().yaw = 3.14159265358979323846F;
    openminerz::world::PlayerInput sprint;
    sprint.forward = 1.0F;
    sprint.sprint = true;
    const float sprint_start = player.state().position.z;
    for (int frame = 0; frame < 30; ++frame)
        player.update(world, sprint, 1.0F / 60.0F, false);
    expect(near(player.state().position.z - sprint_start, 5.0F, 0.03F) &&
               near(player.state().stamina, 70.0F, 0.01F) && player.state().sprinting,
           "Modern sprint must double the five-unit surface speed and spend .6 stamina per second");

    player.spawn(world, 5, 5);
    for (int frame = 0; frame < 99; ++frame)
        player.update(world, sprint, 1.0F / 60.0F, false);
    expect(near(player.state().stamina, 1.0F, 0.01F) && player.state().sprinting,
           "the last affordable sprint frame must enter the exact exhaustion threshold");
    player.update(world, sprint, 1.0F / 60.0F, false);
    expect(!player.state().sprinting && near(player.state().stamina, 1.0F, 0.01F),
           "exhaustion must block sprint immediately without overspending stamina");
    for (int frame = 0; frame < 60; ++frame)
        player.update(world, {}, 1.0F / 60.0F, false);
    expect(player.state().stamina > 1.0F && player.state().stamina < 2.0F,
           "stamina recovery must wait one second after its last use");
    for (int frame = 0; frame < 60; ++frame)
        player.update(world, {}, 1.0F / 60.0F, false);
    const float recovered_stamina = player.state().stamina;
    player.update(world, sprint, 1.0F / 60.0F, false);
    expect(recovered_stamina > 25.0F && recovered_stamina < 27.0F && player.state().sprinting,
           "the two-second exhaustion block must expire while stamina recovers at .25 per second");

    player.spawn(world, 5, 5);
    player.state().fly_mode = true;
    player.update(world, sprint, 1.0F / 60.0F, false, false, 20.0F);
    expect(!player.state().sprinting && near(player.state().stamina, 100.0F),
           "Creative flight must never consume or activate surface sprint stamina");

    player.spawn(world, 5, 5);
    player.state().fly_mode = true;
    openminerz::world::PlayerInput fast_flight;
    fast_flight.forward = 1.0F;
    fast_flight.jump = true;
    player.update(world, fast_flight, 1.0F / 60.0F, false, false, 20.0F);
    expect(near(player.state().velocity.z, -20.0F, 0.001F) &&
               near(player.state().velocity.y, 15.0F, 0.001F),
           "Creative flight must use the configured cruise speed on both axes");

    player.state().fly_mode = false;
    player.spawn(world, 5, 5);
    const float jump_start = player.state().position.y;
    openminerz::world::PlayerInput vanilla_jump;
    vanilla_jump.jump = true;
    player.update(world, vanilla_jump, 1.0F / 60.0F, false);
    float jump_apex = player.state().position.y;
    for (int frame = 0; frame < 120 && !player.state().grounded; ++frame)
    {
        player.update(world, {}, 1.0F / 60.0F, false);
        jump_apex = std::max(jump_apex, player.state().position.y);
    }
    expect(near(jump_apex - jump_start, 2.416667F, 0.012F),
           "the recovered +10 impulse and -20 gravity must preserve vanilla's 60 Hz jump apex");

    // The shipped Modern constructor raises FPSRig.JumpCountLimit to two.
    // Player.GetJumpCost makes the grounded jump free and charges .25 stamina
    // only for the second, airborne impulse.
    player.spawn(world, 5, 5);
    player.update(world, vanilla_jump, 1.0F / 60.0F, false, false,
                  openminerz::world::PlayerSimulation::fly_speed, true);
    player.update(world, {}, 1.0F / 60.0F, false, false,
                  openminerz::world::PlayerSimulation::fly_speed, true);
    const float before_air_jump = player.state().velocity.y;
    player.update(world, vanilla_jump, 1.0F / 60.0F, false, false,
                  openminerz::world::PlayerSimulation::fly_speed, true);
    expect(near(player.state().velocity.y - before_air_jump,
                openminerz::world::PlayerSimulation::jump_impulse +
                    openminerz::world::PlayerSimulation::gravity / 60.0F,
                0.002F) &&
               near(player.state().stamina, 75.0F, 0.01F),
           "Modern's airborne second jump must add the stock impulse and spend 25 stamina");
    player.update(world, {}, 1.0F / 60.0F, false, false,
                  openminerz::world::PlayerSimulation::fly_speed, true);
    const float before_third_jump = player.state().velocity.y;
    player.update(world, vanilla_jump, 1.0F / 60.0F, false, false,
                  openminerz::world::PlayerSimulation::fly_speed, true);
    expect(near(player.state().velocity.y - before_third_jump,
                openminerz::world::PlayerSimulation::gravity / 60.0F, 0.002F) &&
               near(player.state().stamina, 75.0F, 0.01F),
           "Modern must reject a third airborne jump without spending stamina");

    player.spawn(world, 5, 5);
    player.update(world, vanilla_jump, 1.0F / 60.0F, false);
    player.update(world, {}, 1.0F / 60.0F, false);
    const float classic_before_second = player.state().velocity.y;
    player.update(world, vanilla_jump, 1.0F / 60.0F, false);
    expect(near(player.state().velocity.y - classic_before_second,
                openminerz::world::PlayerSimulation::gravity / 60.0F, 0.002F) &&
               near(player.state().stamina, 100.0F),
           "Classic must retain its one-jump contract");

    // Water keeps its original buoyancy, drag, and horizontal speed while held
    // vertical controls provide deliberate swimming instead of one-shot hops.
    player.spawn(world, 5, 5);
    player.state().position = {5.5F, -10.0F, 5.5F};
    player.state().velocity = {};
    player.state().grounded = false;
    openminerz::world::PlayerInput swim;
    swim.forward = 1.0F;
    swim.jump = true;
    const float swim_start_z = player.state().position.z;
    const float swim_start_y = player.state().position.y;
    player.update(world, swim, 1.0F / 60.0F, false, true);
    expect(near(swim_start_z - player.state().position.z,
                openminerz::world::PlayerSimulation::underwater_speed / 60.0F, 0.001F) &&
               near(player.state().position.y - swim_start_y,
                    (openminerz::world::PlayerSimulation::underwater_up_acceleration +
                     openminerz::world::PlayerSimulation::underwater_control_acceleration) /
                        3600.0F,
                    0.001F) &&
               near(player.state().velocity.y,
                    (openminerz::world::PlayerSimulation::underwater_up_acceleration +
                     openminerz::world::PlayerSimulation::underwater_control_acceleration) /
                        60.0F * 0.95F,
                    0.002F),
           "full submersion must slow, buoy, damp, and respond to held swim-up input");
    for (int frame = 0; frame < 59; ++frame)
        player.update(world, swim, 1.0F / 60.0F, false, true);
    expect(player.state().position.y > swim_start_y + 1.0F,
           "holding swim-up must keep lifting the player instead of firing only one impulse");

    player.spawn(world, 5, 5);
    player.state().position = {5.5F, -10.0F, 5.5F};
    player.state().velocity = {};
    player.state().grounded = false;
    openminerz::world::PlayerInput dive;
    dive.descend = true;
    const float dive_start_y = player.state().position.y;
    for (int frame = 0; frame < 60; ++frame)
        player.update(world, dive, 1.0F / 60.0F, false, true);
    expect(player.state().position.y < dive_start_y - 0.5F,
           "holding descend underwater must overcome buoyancy and swim downward");
    player.spawn(world, 5, 5);
    player.state().position = {5.5F, -10.0F, 5.5F};
    player.state().velocity = {};
    player.state().grounded = false;
    player.update(world, {}, 1.0F / 60.0F, false, true);
    expect(near(player.state().position.y, -10.0F + (4.0F / 60.0F) / 60.0F, 0.0002F) &&
               near(player.state().velocity.y, (4.0F / 60.0F) * 0.95F, 0.0002F),
           "a fully submerged idle player must rise under +4 acceleration and 3/s drag");

    player.spawn(world, 5, 5);

    world.set_block(5, 1, 4, BlockType::leaves);
    world.set_block(5, 2, 4, BlockType::leaves);
    world.set_block(5, 1, 3, BlockType::surface_lava);
    world.set_block(5, 2, 3, BlockType::surface_lava);
    world.set_block(5, 1, 2, BlockType::deep_lava);
    world.set_block(5, 2, 2, BlockType::deep_lava);
    player.refresh_target(world);
    expect(player.state().target.has_value() && player.state().target->block == BlockType::leaves,
           "pass-through leaves must remain targetable for mining and placement");
    openminerz::world::PlayerInput pass_through_forward;
    pass_through_forward.forward = 1.0F;
    for (int frame = 0; frame < 45; ++frame)
    {
        player.update(world, pass_through_forward, 1.0F / 60.0F, false);
    }
    expect(player.state().position.z < 2.8F && near(player.state().position.y, 1.001F, 0.01F),
           "player movement must pass through leaf and lava columns without climbing or snagging");
    for (int z = 2; z <= 4; ++z)
    {
        world.set_block(5, 1, z, BlockType::empty);
        world.set_block(5, 2, z, BlockType::empty);
    }
    player.spawn(world, 5, 5);

    player.state().yaw = 2.3561945F;
    openminerz::world::PlayerInput diagonal_forward;
    diagonal_forward.forward = 1.0F;
    player.update(world, diagonal_forward, 1.0F / 60.0F, false);
    const auto view = player.view_direction();
    const float forward_alignment =
        (player.state().velocity.x * view.x + player.state().velocity.z * view.z) /
        openminerz::world::PlayerSimulation::movement_speed;
    expect(forward_alignment > 0.99F,
           "forward movement must remain aligned with the camera at non-cardinal yaw angles");
    openminerz::world::PlayerInput diagonal_strafe;
    diagonal_strafe.strafe = 1.0F;
    player.update(world, diagonal_strafe, 1.0F / 60.0F, false);
    const float strafe_forward_dot =
        player.state().velocity.x * view.x + player.state().velocity.z * view.z;
    const float right_x = std::cos(player.state().yaw);
    const float right_z = std::sin(player.state().yaw);
    const float strafe_right_alignment =
        (player.state().velocity.x * right_x + player.state().velocity.z * right_z) /
        openminerz::world::PlayerSimulation::movement_speed;
    expect(std::abs(strafe_forward_dot) < 0.01F && strafe_right_alignment > 0.99F,
           "strafe movement must use the renderer camera's right vector");

    openminerz::world::PlayerInput jump;
    jump.jump = true;
    player.update(world, jump, 1.0F / 60.0F, false);
    player.update(world, {}, 1.0F / 60.0F, false);
    expect(player.state().position.y > 1.1F && player.state().velocity.y > 0.0F,
           "grounded jump must apply the recovered 10-unit impulse");

    player.spawn(world, 5, 5);
    for (int x = 4; x <= 6; ++x)
    {
        world.set_block(x, 1, 3, openminerz::world::BlockType::rock);
        world.set_block(x, 2, 3, openminerz::world::BlockType::rock);
    }
    openminerz::world::PlayerInput forward;
    forward.forward = 1.0F;
    for (int frame = 0; frame < 90; ++frame)
    {
        player.update(world, forward, 1.0F / 60.0F, false);
    }
    expect(player.state().position.z >= 4.34F,
           "horizontal movement must stop before penetrating a solid wall");
    for (int frame = 0; frame < 60; ++frame)
    {
        player.update(world, forward, 1.0F / 60.0F, true);
    }
    expect(player.state().position.y < 1.1F,
           "sloped sides must never walk the player up a flat two-high wall");

    for (int x = 4; x <= 6; ++x)
    {
        world.set_block(x, 1, 3, openminerz::world::BlockType::empty);
        world.set_block(x, 2, 3, openminerz::world::BlockType::empty);
    }
    player.spawn(world, 5, 5);
    world.set_block(5, 1, 4, openminerz::world::BlockType::rock);
    for (int frame = 0; frame < 8; ++frame)
    {
        player.update(world, forward, 1.0F / 60.0F, true);
    }
    expect(player.state().position.y > 1.05F && player.state().position.y < 1.95F,
           "the sloped-side climb must lift gradually at five units per second");
    for (int frame = 0; frame < 12; ++frame)
    {
        player.update(world, forward, 1.0F / 60.0F, true);
    }
    expect(player.state().position.y > 1.95F && player.state().grounded,
           "walking into a single step must slide the player onto it");

    // The signature original move: a 2.5-block jump against a three-high wall
    // slides up the face onto the top once vertical speed drops below five.
    world.set_block(5, 1, 4, openminerz::world::BlockType::empty);
    player.spawn(world, 5, 5);
    for (int x = 4; x <= 6; ++x)
        for (int y = 1; y <= 3; ++y)
            world.set_block(x, y, 3, openminerz::world::BlockType::rock);
    openminerz::world::PlayerInput leap;
    leap.forward = 1.0F;
    leap.jump = true;
    player.update(world, leap, 1.0F / 60.0F, true);
    float wall_peak{};
    for (int frame = 0; frame < 60; ++frame)
    {
        player.update(world, forward, 1.0F / 60.0F, true);
        wall_peak = std::max(wall_peak, player.state().position.y);
    }
    expect(wall_peak > 3.95F, "a 2.5-block jump plus the block slide must top a three-high wall");
    for (int x = 4; x <= 6; ++x)
        for (int y = 1; y <= 3; ++y)
            world.set_block(x, y, 3, openminerz::world::BlockType::empty);
    world.set_block(5, 1, 4, openminerz::world::BlockType::empty);

    for (int x = 4; x <= 6; ++x)
    {
        world.set_block(x, 1, 3, openminerz::world::BlockType::empty);
        world.set_block(x, 2, 3, openminerz::world::BlockType::empty);
    }
    player.spawn(world, 5, 5);
    world.set_block(5, 2, 2, openminerz::world::BlockType::dirt);
    player.refresh_target(world);
    expect(player.state().target.has_value() && player.state().target->z == 2 &&
               near(player.state().target->distance, 2.5F, 0.1F),
           "construction trace must hit the nearest voxel within the original five-block reach");
    expect(player.mine_target(world) && world.block(5, 2, 2) == openminerz::world::BlockType::empty,
           "mining must mutate the authoritative voxel cell");
    world.set_block(5, 2, 1, openminerz::world::BlockType::dirt);
    player.refresh_target(world);
    expect(player.place_target(world) && world.block(5, 2, 2) == openminerz::world::BlockType::rock,
           "placement must use the hit-face neighbor without intersecting the player");

    openminerz::world::WorldRegion break_world{0, 0, 1, 1};
    for (int z = 0; z < openminerz::world::chunk_depth; ++z)
    {
        for (int x = 0; x < openminerz::world::chunk_width; ++x)
        {
            break_world.set_block(x, 0, z, BlockType::rock);
        }
    }
    break_world.set_block(5, 2, 2, BlockType::dirt);
    player.spawn(break_world, 5, 5);
    int completed_frame = -1;
    int impacts{};
    bool impact_surface_contract{};
    for (int frame = 0; frame < 92; ++frame)
    {
        const auto update = player.update_breaking(break_world, true, 1.0F / 60.0F);
        if (update.has_value())
        {
            impacts += update->impact ? 1 : 0;
            if (update->impact)
            {
                impact_surface_contract =
                    impact_surface_contract ||
                    (update->normal_x == 0 && update->normal_y == 0 && update->normal_z == 1 &&
                     near(update->intersection.z, 3.0F, 0.01F));
            }
            if (update->completed)
            {
                completed_frame = frame;
                break;
            }
        }
    }
    expect(completed_frame >= 89 && completed_frame <= 90 && impacts == 4 &&
               impact_surface_contract && break_world.block(5, 2, 2) == BlockType::empty,
           "bare hands must crack dirt for 1.5 seconds at the original cadence and hit surface");

    break_world.set_block(5, 2, 2, BlockType::dirt);
    break_world.set_block(4, 2, 2, BlockType::dirt);
    player.state().target = openminerz::world::VoxelRaycastHit{
        .x = 5,
        .y = 2,
        .z = 2,
        .place_x = 5,
        .place_y = 2,
        .place_z = 3,
        .normal_z = 1,
        .distance = 2.5F,
        .block = BlockType::dirt,
    };
    const auto first_pick_impact =
        player.update_breaking(break_world, true, 1.0F / 60.0F, 0.3F, 1U);
    const float first_swing_phase = player.state().tool_swing_progress;
    player.state().target->x = 4;
    player.state().target->place_x = 4;
    const auto swept_target = player.update_breaking(break_world, true, 1.0F / 60.0F, 0.3F, 1U);
    const float swept_swing_phase = player.state().tool_swing_progress;
    const auto idle_update = player.update_breaking(break_world, false, 0.27F, 0.3F, 1U);
    const auto resumed_impact = player.update_breaking(break_world, true, 0.02F, 0.3F, 1U);
    expect(first_pick_impact.has_value() && first_pick_impact->impact &&
               !swept_target.has_value() && !idle_update.has_value() &&
               resumed_impact.has_value() && resumed_impact->impact &&
               swept_swing_phase > first_swing_phase,
           "item cooldown and visual swing must survive target changes without particle spam");
}

void inventory_matches_original_slot_and_stack_contracts()
{
    using openminerz::InventoryItemId;
    using openminerz::InventoryStack;
    using openminerz::PlayerAnimationMode;
    using openminerz::PlayerInventory;
    using openminerz::world::BlockType;

    PlayerInventory inventory;
    expect(inventory.storage().size() == 32U && inventory.tray().size() == 8U,
           "inventory must expose the original 32 storage and eight quick slots");
    expect(inventory.profile() == openminerz::InventoryProfile::classic &&
               inventory.trays().size() == 2U && inventory.current_tray_index() == 0U,
           "the shared inventory must retain a hidden second tray while Classic exposes tray zero");
    inventory.switch_current_tray();
    expect(inventory.current_tray_index() == 0U,
           "Classic inventory must never activate Modern's second tray");
    constexpr std::array expected_items{InventoryItemId::stone_pickaxe, InventoryItemId::compass,
                                        InventoryItemId::pistol,        InventoryItemId::knife,
                                        InventoryItemId::bullets,       InventoryItemId::torch};
    constexpr std::array<std::uint16_t, 6> expected_counts{1U, 1U, 1U, 1U, 200U, 16U};
    bool exact_loadout = true;
    for (std::size_t index = 0; index < expected_items.size(); ++index)
        exact_loadout = exact_loadout && inventory.tray()[index].has_value() &&
                        inventory.tray()[index]->item == expected_items[index] &&
                        inventory.tray()[index]->count == expected_counts[index];
    expect(exact_loadout && !inventory.tray()[6].has_value() && !inventory.tray()[7].has_value() &&
               inventory.tray()[2]->rounds_in_clip == 8U &&
               inventory.selected_item() == InventoryItemId::stone_pickaxe &&
               inventory.selected_block() == BlockType::empty,
           "fresh players must receive the original six-stack default loadout and two empty slots");
    expect(openminerz::item_stack_limit(InventoryItemId::stone_pickaxe) == 1U &&
               openminerz::item_stack_limit(InventoryItemId::compass) == 1U &&
               openminerz::item_stack_limit(InventoryItemId::pistol) == 1U &&
               openminerz::item_stack_limit(InventoryItemId::bullets) == 5000U &&
               openminerz::item_stack_limit(InventoryItemId::torch) == 64U,
           "default items must retain their original singleton, ammo, and block stack limits");
    expect(
        near(openminerz::item_use_cooldown_seconds(InventoryItemId::stone_pickaxe), 0.3F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::compass), 0.3F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::bullets), 0.3F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::knife), 0.5F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::torch), 0.1F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::gold_knife), 0.4F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::blood_stone_knife),
                 0.25F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::iron_laser_sword), 0.3F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::diamond_casing), 0.3F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::pump_shotgun), 1.0F) &&
            near(openminerz::item_use_cooldown_seconds(InventoryItemId::fired_rocket), 1.0F),
        "default-loadout action cooldowns must match each original InventoryItemClass");
    expect(
        near(openminerz::item_fire_animation_seconds(InventoryItemId::assault_rifle, false),
             0.12F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::pistol, true), 0.15F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::pump_shotgun, false),
                 1.42F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::iron_space_assault_rifle,
                                                         true),
                 0.10F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::iron_space_pump_shotgun,
                                                         false),
                 0.98F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::rocket_launcher, false),
                 0.98F) &&
            near(openminerz::item_fire_animation_seconds(InventoryItemId::guided_rocket_launcher,
                                                         true),
                 1.42F),
        "gun use must retain each authored body animation duration");
    expect(near(openminerz::item_enemy_damage(InventoryItemId::bare_hands), 0.025F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::dirt_block), 0.01F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::wood_block), 0.075F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::blood_stone_block), 0.15F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::slime), 0.075F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::stick), 0.05F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::pistol), 0.3F) &&
               near(openminerz::item_enemy_damage(InventoryItemId::fired_rocket), 100.0F),
           "bare hands and held blocks must retain their authored melee damage");
    expect(
        near(openminerz::item_player_melee_damage(InventoryItemId::bare_hands), 0.21F) &&
            near(openminerz::item_player_melee_damage(InventoryItemId::diamond_axe), 0.21F) &&
            near(openminerz::item_player_melee_damage(InventoryItemId::iron_laser_sword), 1.1F) &&
            near(openminerz::item_player_melee_damage(InventoryItemId::blood_stone_laser_sword),
                 1.1F),
        "player melee must preserve vanilla's fixed 21-percent hit and lethal laser-sword hit");
    expect(openminerz::item_is_melee_weapon(InventoryItemId::bare_hands) &&
               openminerz::item_is_melee_weapon(InventoryItemId::dirt_block) &&
               openminerz::item_is_melee_weapon(InventoryItemId::bullets) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::pistol) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::guided_rocket_launcher) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::fired_rocket) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::grenade) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::compass) &&
               !openminerz::item_is_melee_weapon(InventoryItemId::gps),
           "enemy construction probes must follow InventoryItemClass.IsMeleeWeapon exactly");
    expect(openminerz::item_grenade_type(InventoryItemId::grenade) == 0U &&
               openminerz::item_grenade_type(InventoryItemId::modern_sticky_grenade) == 3U &&
               !openminerz::item_grenade_type(InventoryItemId::modern_snowball).has_value() &&
               !openminerz::item_grenade_type(InventoryItemId::modern_iceball).has_value(),
           "only HE and the registered Modern sticky class may enter the grenade input path");
    expect(static_cast<std::uint8_t>(PlayerAnimationMode::generic) == 0U &&
               static_cast<std::uint8_t>(PlayerAnimationMode::space_assault) == 10U &&
               static_cast<std::uint8_t>(PlayerAnimationMode::rpg) == 16U &&
               openminerz::item_player_animation_mode(InventoryItemId::bare_hands) ==
                   PlayerAnimationMode::fist &&
               openminerz::item_player_animation_mode(InventoryItemId::stone_pickaxe) ==
                   PlayerAnimationMode::pick &&
               openminerz::item_player_animation_mode(InventoryItemId::dirt_block) ==
                   PlayerAnimationMode::block &&
               openminerz::item_player_animation_mode(InventoryItemId::clock) ==
                   PlayerAnimationMode::generic &&
               openminerz::item_player_animation_mode(InventoryItemId::gps) ==
                   PlayerAnimationMode::generic &&
               openminerz::item_player_animation_mode(InventoryItemId::iron_space_assault_rifle) ==
                   PlayerAnimationMode::space_assault &&
               openminerz::item_player_animation_mode(
                   InventoryItemId::diamond_space_bolt_action_rifle) ==
                   PlayerAnimationMode::space_bolt_rifle &&
               openminerz::item_player_animation_mode(InventoryItemId::guided_rocket_launcher) ==
                   PlayerAnimationMode::rpg,
           "item player modes must preserve the original one-byte network ordinals and poses");
    expect(inventory.consume_item(InventoryItemId::bullets, 2U) &&
               inventory.count(InventoryItemId::bullets) == 198U,
           "pistol reloads must consume their exact reserve-ammunition count");
    inventory.select(5U);
    expect(inventory.selected_block() == BlockType::torch && inventory.consume_selected() &&
               inventory.tray()[5]->count == 15U,
           "successful placement must consume exactly one selected placeable item");

    PlayerInventory pickup_inventory;
    pickup_inventory.storage().fill(std::nullopt);
    pickup_inventory.tray().fill(std::nullopt);
    pickup_inventory.storage()[0] = InventoryStack{BlockType::rock, 60U};
    // AddInventoryItem: every existing stack (tray then backpack) is topped up
    // BEFORE any empty slot is claimed, so pickups coalesce instead of
    // splitting into fresh slots.
    expect(pickup_inventory.add(BlockType::rock, 70U) == 0U &&
               pickup_inventory.storage()[0]->count == 64U &&
               pickup_inventory.tray()[0]->count == 64U && pickup_inventory.tray()[1]->count == 2U,
           "pickup must top off existing stacks before claiming empty slots");
    expect(pickup_inventory.count(BlockType::rock) == 130U,
           "inventory counts must include storage and the quick tray");

    pickup_inventory.tray()[7] = InventoryStack{BlockType::grass, 64U};
    pickup_inventory.storage()[1] = InventoryStack{BlockType::grass, 12U};
    expect(pickup_inventory.move_to_other_section(1U) &&
               pickup_inventory.count(BlockType::grass) == 76U,
           "shift-click must preserve all items while moving between inventory sections");
    pickup_inventory.select_relative(-1);
    expect(pickup_inventory.selected_index() == 7U,
           "quick-slot selection must wrap exactly across all eight tray cells");

    PlayerInventory modern_inventory;
    modern_inventory.storage().fill(std::nullopt);
    for (auto &tray : modern_inventory.trays())
        tray.fill(std::nullopt);
    modern_inventory.set_profile(openminerz::InventoryProfile::modern);
    modern_inventory.tray(0).fill(InventoryStack{InventoryItemId::modern_iron_door, 999U});
    expect(modern_inventory.add_item(InventoryItemId::gold_bullets, 12U) == 0U &&
               modern_inventory.tray(1)[0].has_value() &&
               modern_inventory.tray(1)[0]->item == InventoryItemId::gold_bullets &&
               modern_inventory.count(InventoryItemId::gold_bullets) == 12U,
           "Modern pickups must flow through both exact eight-slot trays");
    modern_inventory.switch_current_tray();
    expect(modern_inventory.current_tray_index() == 1U &&
               modern_inventory.selected_item() == InventoryItemId::gold_bullets,
           "Modern SwitchTray must wrap the active tray while preserving the selected cell");
    modern_inventory.switch_current_tray();
    expect(modern_inventory.current_tray_index() == 0U,
           "Modern SwitchTray must wrap from tray two back to tray one");
}

void voxel_lighting_flood_fills_and_never_leaks()
{
    using openminerz::world::BlockType;
    using openminerz::world::WorldRegion;

    WorldRegion region{0, 0, 1, 1};
    // Flat rock floor with a hollow chamber buried in a mound. The chamber wall is a
    // single block thick with open sky two cells beyond it: the old heightmap
    // heuristic leaked daylight straight through that wall.
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);
    for (int y = 1; y <= 5; ++y)
        for (int z = 5; z <= 11; ++z)
            for (int x = 5; x <= 10; ++x)
                region.set_block(x, y, z, BlockType::rock);
    for (int y = 2; y <= 3; ++y)
        for (int z = 7; z <= 9; ++z)
            for (int x = 7; x <= 9; ++x)
                region.set_block(x, y, z, BlockType::empty);

    const auto find_face =
        [](const std::vector<openminerz::world::WorldFace> &faces, int x, int y, int z,
           openminerz::world::FaceDirection direction) -> const openminerz::world::WorldFace * {
        for (const auto &face : faces)
            if (face.x == x && face.y == y && face.z == z && face.direction == direction)
                return &face;
        return nullptr;
    };
    const auto average = [](const std::array<std::uint8_t, 4> &values) {
        int total{};
        for (const std::uint8_t value : values)
            total += value;
        return total / 4;
    };

    const auto dark_faces = region.visible_faces();
    const auto *chamber_floor =
        find_face(dark_faces, 8, 1, 8, openminerz::world::FaceDirection::positive_y);
    const auto *outdoor_floor =
        find_face(dark_faces, 2, 0, 2, openminerz::world::FaceDirection::positive_y);
    expect(chamber_floor != nullptr && outdoor_floor != nullptr,
           "the sealed chamber and outdoor floor faces must both mesh");
    expect(average(outdoor_floor->sky_light) > 220,
           "open-sky floors must receive full flood-filled sunlight");
    expect(average(chamber_floor->sky_light) == 0,
           "a sealed chamber must be pitch dark: sunlight must not leak through walls");
    expect(average(chamber_floor->block_light) == 0,
           "an unlit chamber must carry no torch-channel light");
    expect(chamber_floor->block_light_lobe[3] == 0U,
           "an unlit chamber must not invent a directional block-light lobe");

    region.set_block(8, 2, 8, BlockType::torch);
    const auto lit_faces = region.visible_faces();
    const auto *lit_floor =
        find_face(lit_faces, 8, 1, 8, openminerz::world::FaceDirection::positive_y);
    const auto *lit_corner =
        find_face(lit_faces, 7, 1, 9, openminerz::world::FaceDirection::positive_y);
    const auto *lit_outdoor =
        find_face(lit_faces, 2, 0, 2, openminerz::world::FaceDirection::positive_y);
    expect(lit_floor != nullptr && lit_corner != nullptr && lit_outdoor != nullptr,
           "faces must remain meshed after placing the torch");
    expect(average(lit_floor->block_light) > 190,
           "a torch must flood its chamber with near-full block light");
    expect(average(lit_corner->block_light) > 60 &&
               average(lit_corner->block_light) < average(lit_floor->block_light),
           "torch light must fall off with flood-fill distance");
    expect(lit_corner->block_light_lobe[0] > 128U && lit_corner->block_light_lobe[2] < 128U &&
               lit_corner->block_light_lobe[3] > 0U,
           "a floor away from the torch must bake a confident direction toward its source");
    expect(average(lit_outdoor->block_light) == 0,
           "torch light must stay sealed inside the chamber");
    expect(lit_outdoor->block_light_lobe[3] == 0U,
           "sealed torch direction must not leak into the outdoor fixture");
    expect(average(lit_floor->sky_light) == 0,
           "torch light must never register on the sunlight channel");
    expect(openminerz::world::self_illumination(BlockType::lantern) == 15U &&
               openminerz::world::self_illumination(BlockType::surface_lava) == 15U &&
               openminerz::world::self_illumination(BlockType::slime) == 15U &&
               openminerz::world::self_illumination(BlockType::rock) == 0U,
           "lanterns, lava and space goo must carry the original SelfIllumination 15");

    // Tree canopies: sunlight passes THROUGH leaves and a leaf face is never
    // lit darker than the leaf block itself (RenderChunk.LightAsTranslucent).
    // Two bugs used to break this: the sky scan stopped dead at the first leaf
    // so a flat canopy top was never reached by the flood fill at all, and
    // face lighting averaged only the neighbours, so a leaf abutting another
    // leaf rendered near black. Together they turned canopies into blotches.
    WorldRegion canopy{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            canopy.set_block(x, 0, z, BlockType::rock);
    // A solid 7x7x2 slab of leaves: the middle blocks are surrounded by other
    // leaves on every side, which is the worst case for self-shadowing. It has
    // to be wide enough that the centre of the lower layer is genuinely dim,
    // otherwise light leaking in from the open sides masks the bug.
    for (int y = 6; y <= 7; ++y)
        for (int z = 3; z <= 9; ++z)
            for (int x = 3; x <= 9; ++x)
                canopy.set_block(x, y, z, BlockType::leaves);

    const auto canopy_faces = canopy.visible_faces();
    const auto *canopy_top =
        find_face(canopy_faces, 6, 7, 6, openminerz::world::FaceDirection::positive_y);
    const auto *canopy_side =
        find_face(canopy_faces, 3, 7, 6, openminerz::world::FaceDirection::negative_x);
    const auto *canopy_under =
        find_face(canopy_faces, 6, 6, 6, openminerz::world::FaceDirection::negative_y);
    expect(canopy_top != nullptr && canopy_side != nullptr && canopy_under != nullptr,
           "leaf faces must mesh on every side of a canopy slab");
    expect(average(canopy_top->sky_light) > 60,
           "a leaf under open sky must be lit: the sky scan has to seed it at "
           "LightTransmission - 1 instead of stopping above it");
    expect(average(canopy_side->sky_light) > 60,
           "a leaf face abutting other leaves must not go black; it takes at least "
           "the block's own light level");
    expect(average(canopy_under->sky_light) > 0,
           "light must pass through the canopy rather than being blocked outright");

    // RenderChunk compares the neighbor's block type before emitting a face,
    // even when the type passes light. Shared leaf/leaf faces must therefore
    // be absent: rendering those hidden layers is what made crowns look like
    // dense opaque cubes despite correct flood-filled sunlight.
    const auto *internal_leaf_face =
        find_face(canopy_faces, 6, 7, 6, openminerz::world::FaceDirection::negative_y);
    expect(internal_leaf_face == nullptr,
           "adjacent leaf blocks must cull their shared interior face while still passing light");

    const auto *ground_under_canopy =
        find_face(canopy_faces, 6, 0, 6, openminerz::world::FaceDirection::positive_y);
    const auto *ground_in_the_open =
        find_face(canopy_faces, 1, 0, 1, openminerz::world::FaceDirection::positive_y);
    expect(ground_under_canopy != nullptr && ground_in_the_open != nullptr,
           "ground faces must mesh under and beside the canopy");
    expect(average(ground_under_canopy->sky_light) < average(ground_in_the_open->sky_light),
           "a canopy must still cast a soft pool of shade on the ground below it");
}

void inventory_moves_never_duplicate_or_refill_items()
{
    using openminerz::InventoryItemId;
    using openminerz::InventoryStack;
    using openminerz::PlayerInventory;

    // A spent gun keeps its spent clip through every move path. Rebuilding stacks
    // from the item id used to refill the clip for free.
    PlayerInventory inventory;
    inventory.storage().fill(std::nullopt);
    inventory.tray().fill(std::nullopt);
    InventoryStack spent{InventoryItemId::pistol, 1U};
    spent.rounds_in_clip = 3U;
    inventory.tray()[5] = spent;
    expect(inventory.move_to_other_section(PlayerInventory::storage_size + 5U) &&
               inventory.storage()[0].has_value() && inventory.storage()[0]->rounds_in_clip == 3U &&
               !inventory.tray()[5].has_value(),
           "quick-moving a gun must preserve its loaded rounds");
    expect(inventory.count(InventoryItemId::pistol) == 1U,
           "quick-moving must never change the total item count");
    const InventoryStack &stored = *inventory.storage()[0];
    expect(stored.with_count(1U).rounds_in_clip == 3U &&
               stored.with_count(1U).item == InventoryItemId::pistol,
           "with_count must copy per-instance stack state");
    expect(inventory.add_stack(stored.with_count(1U)) == 0U,
           "re-adding a moved gun stack must fit into the open tray");
    std::uint32_t total_rounds{};
    for (const auto &slot : inventory.tray())
        if (slot.has_value() && slot->item == InventoryItemId::pistol)
            total_rounds += slot->rounds_in_clip;
    expect(total_rounds == 3U, "add_stack must keep the source stack's loaded rounds");

    // Fresh guns start with a full clip like GunInventoryItem's constructor.
    const InventoryStack fresh{InventoryItemId::assault_rifle, 1U};
    expect(fresh.rounds_in_clip == 30U,
           "a freshly created assault rifle must start with its 30-round clip");

    InventoryStack teleporter{InventoryItemId::teleport_gps, 1U};
    teleporter.waypoint = {41.0F, -12.0F, 93.0F};
    teleporter.waypoint_name = "Alpha Mine";
    teleporter.waypoint_valid = true;
    inventory.tray()[0] = teleporter;
    expect(inventory.move_to_other_section(PlayerInventory::storage_size) &&
               inventory.storage()[1].has_value() &&
               inventory.storage()[1]->waypoint == teleporter.waypoint &&
               inventory.storage()[1]->waypoint_name == "Alpha Mine" &&
               inventory.storage()[1]->waypoint_valid,
           "moving a locator must preserve its per-instance named waypoint");
    expect(teleporter.with_count(1U).waypoint == teleporter.waypoint &&
               teleporter.with_count(1U).waypoint_name == teleporter.waypoint_name,
           "splitting an item must preserve GPS payload as well as gun state");

    // Stackables merge without inventing items across sections.
    PlayerInventory merge_inventory;
    merge_inventory.storage().fill(std::nullopt);
    merge_inventory.tray().fill(std::nullopt);
    merge_inventory.tray()[0] = InventoryStack{InventoryItemId::bullets, 4900U};
    merge_inventory.storage()[0] = InventoryStack{InventoryItemId::bullets, 300U};
    expect(merge_inventory.move_to_other_section(0U) &&
               merge_inventory.count(InventoryItemId::bullets) == 5200U &&
               merge_inventory.tray()[0]->count == 5000U && merge_inventory.tray()[1].has_value() &&
               merge_inventory.tray()[1]->count == 200U &&
               !merge_inventory.storage()[0].has_value(),
           "merging ammo across sections must conserve the exact total");
}

void gun_stats_and_crafting_match_the_original()
{
    using openminerz::InventoryItemId;
    using openminerz::PlayerInventory;

    const auto *pistol = openminerz::gun_stats(InventoryItemId::pistol);
    const auto *assault = openminerz::gun_stats(InventoryItemId::assault_rifle);
    const auto *shotgun = openminerz::gun_stats(InventoryItemId::pump_shotgun);
    const auto *lmg = openminerz::gun_stats(InventoryItemId::blood_stone_lmg);
    const auto *bolt = openminerz::gun_stats(InventoryItemId::bolt_action_rifle);
    const auto *laser = openminerz::gun_stats(InventoryItemId::gold_space_smg);
    const auto *laser_shotgun = openminerz::gun_stats(InventoryItemId::iron_space_pump_shotgun);
    const auto *rpg = openminerz::gun_stats(InventoryItemId::rocket_launcher);
    const auto *guided_rpg = openminerz::gun_stats(InventoryItemId::guided_rocket_launcher);
    const auto *grenade_launcher =
        openminerz::gun_stats(InventoryItemId::modern_basic_grenade_launcher);
    const auto *laser_drill = openminerz::gun_stats(InventoryItemId::modern_laser_drill);
    const auto *precision_laser = openminerz::gun_stats(InventoryItemId::modern_precision_laser);
    expect(pistol != nullptr && pistol->clip_capacity == 8U && !pistol->automatic &&
               near(pistol->reload_seconds, 1.63F) && near(pistol->inaccuracy, 0.05F) &&
               near(pistol->recoil_radians, 0.05235988F) && near(pistol->tracer_velocity, 75.0F) &&
               near(pistol->tracer_flight_seconds, 1.0F) && pistol->needs_drop_compensation &&
               pistol->ammo_item == InventoryItemId::bullets,
           "the pistol must keep its original 8-round semi-automatic contract");
    expect(assault != nullptr && assault->clip_capacity == 30U && assault->automatic &&
               near(assault->shoulder_magnification, 2.0F) && near(assault->inaccuracy, 0.02F) &&
               assault->ammo_item == InventoryItemId::bullets,
           "the assault rifle must be a 30-round automatic with 2x shoulder zoom");
    expect(shotgun != nullptr && shotgun->clip_capacity == 6U && !shotgun->automatic &&
               shotgun->rounds_per_reload == 1U && near(shotgun->inaccuracy, 0.1F) &&
               near(shotgun->recoil_radians, 0.17453293F) &&
               near(shotgun->tracer_velocity, 50.0F) &&
               near(shotgun->tracer_flight_seconds, 0.4F) &&
               near(shotgun->fire_cooldown_seconds, 1.0F),
           "the pump shotgun must fire one shell per second from a 6-round tube");
    expect(lmg != nullptr && lmg->clip_capacity == 100U && lmg->automatic &&
               lmg->ammo_item == InventoryItemId::diamond_bullets && near(lmg->inaccuracy, 0.1F) &&
               near(lmg->tracer_flight_seconds, 1.0F) && near(lmg->reload_seconds, 9.7F),
           "bloodstone LMG must use diamond bullets with the original 9.7s reload");
    expect(bolt != nullptr && near(bolt->inaccuracy, 0.0F) &&
               near(bolt->recoil_radians, 0.20943952F) && near(bolt->tracer_velocity, 150.0F) &&
               near(bolt->tracer_flight_seconds, 2.0F) && bolt->needs_drop_compensation,
           "bolt rifles must keep their exact aim and original twelve-degree recoil");
    expect(laser != nullptr && laser->ammo_item == InventoryItemId::laser_bullets &&
               laser->clip_capacity == 20U && near(laser->inaccuracy, 0.1F) &&
               near(laser->tracer_velocity, 100.0F) && near(laser->tracer_flight_seconds, 2.0F) &&
               !laser->needs_drop_compensation,
           "space weapons must all consume laser bullets");
    expect(is_laser_gun(InventoryItemId::iron_space_assault_rifle) &&
               is_laser_gun(InventoryItemId::diamond_space_smg) &&
               !is_laser_gun(InventoryItemId::space_knife) &&
               !is_laser_gun(InventoryItemId::rocket_launcher) &&
               near(openminerz::laser_projectile_speed, 200.0F) &&
               near(openminerz::laser_projectile_lifetime_seconds, 3.0F) &&
               openminerz::laser_projectile_bounces == 3,
           "laser firearms must route through the original BlasterShot contract");
    expect(openminerz::world::bounces_lasers(openminerz::world::BlockType::bedrock) &&
               openminerz::world::bounces_lasers(openminerz::world::BlockType::blood_stone) &&
               openminerz::world::bounces_lasers(openminerz::world::BlockType::diamond_wall) &&
               !openminerz::world::bounces_lasers(openminerz::world::BlockType::rock) &&
               openminerz::world::can_be_dug(openminerz::world::BlockType::diamond_ore) &&
               !openminerz::world::can_be_dug(openminerz::world::BlockType::empty) &&
               !openminerz::world::can_be_dug(openminerz::world::BlockType::fixed_lantern) &&
               !openminerz::world::can_be_dug(openminerz::world::BlockType::bedrock),
           "laser impacts must use the exact block dig and ricochet flags");
    expect(openminerz::world::placed_explosive_kind(openminerz::world::BlockType::tnt) ==
                   openminerz::world::ExplosiveKind::tnt &&
               openminerz::world::placed_explosive_kind(openminerz::world::BlockType::c4) ==
                   openminerz::world::ExplosiveKind::c4 &&
               !openminerz::world::placed_explosive_kind(openminerz::world::BlockType::rock)
                    .has_value(),
           "ordinary tracer impacts must immediately detonate TNT and C4 only");
    expect(laser_shotgun != nullptr && laser_shotgun->clip_capacity == 8U &&
               laser_shotgun->rounds_per_reload == 1U,
           "the laser shotgun must reload its eight-round tube one shell at a time");
    const auto first_shell = openminerz::gun_reload_cycle(*shotgun, 0U, 10U);
    const auto last_shell = openminerz::gun_reload_cycle(*shotgun, 5U, 10U);
    const auto partial_magazine = openminerz::gun_reload_cycle(*pistol, 0U, 3U);
    expect(first_shell.loaded == 1U && first_shell.continue_reloading && last_shell.loaded == 1U &&
               !last_shell.continue_reloading && partial_magazine.loaded == 3U &&
               !partial_magazine.continue_reloading,
           "completed reload animations must loop shotgun shells but load magazines in one pass");
    const auto recoil_low = openminerz::gun_recoil(0.2F, 0.0F, 0.0F);
    const auto recoil_mid = openminerz::gun_recoil(0.2F, 0.5F, 0.5F);
    const auto recoil_high = openminerz::gun_recoil(0.2F, 1.0F, 1.0F);
    expect(near(recoil_low.yaw_radians, -0.05F) && near(recoil_low.pitch_radians, 0.1F) &&
               near(recoil_mid.yaw_radians, 0.0F) && near(recoil_mid.pitch_radians, 0.15F) &&
               near(recoil_high.yaw_radians, 0.05F) && near(recoil_high.pitch_radians, 0.2F),
           "gun recoil must preserve the original random horizontal and vertical ranges");
    expect(rpg != nullptr && rpg->clip_capacity == 1U &&
               rpg->ammo_item == InventoryItemId::rocket_ammo &&
               near(rpg->recoil_radians, 0.17453293F),
           "rocket launchers must hold one rocket");
    expect(guided_rpg != nullptr && near(guided_rpg->shoulder_magnification, 2.0F),
           "the guided launcher must use its authored two-times shoulder magnification");
    expect(grenade_launcher != nullptr && grenade_launcher->clip_capacity == 1U &&
               !grenade_launcher->automatic && grenade_launcher->rounds_per_reload == 1U &&
               grenade_launcher->ammo_item == InventoryItemId::rocket_ammo &&
               near(grenade_launcher->enemy_damage, 100.0F) &&
               near(grenade_launcher->tracer_velocity, 50.0F) &&
               near(grenade_launcher->tracer_flight_seconds, 0.4F) &&
               near(grenade_launcher->recoil_radians, 0.20943952F),
           "Modern item 141 must retain its one-round Gunshot grenade-launcher registration");
    expect(laser_drill != nullptr && laser_drill->clip_capacity == 200U && laser_drill->automatic &&
               near(laser_drill->reload_seconds, 3.0F) &&
               near(laser_drill->shoulder_magnification, 1.15F) &&
               laser_drill->ammo_item == InventoryItemId::gold_bullets &&
               near(laser_drill->enemy_damage, 0.2F) && !laser_drill->needs_drop_compensation &&
               is_laser_gun(InventoryItemId::modern_laser_drill),
           "Modern item 143 must retain its 200-round automatic harvesting-laser contract");
    expect(precision_laser != nullptr && precision_laser->clip_capacity == 10U &&
               !precision_laser->automatic && near(precision_laser->reload_seconds, 3.0F) &&
               near(precision_laser->shoulder_magnification, 3.45F) &&
               precision_laser->ammo_item == InventoryItemId::gold_bullets &&
               near(precision_laser->enemy_damage, 25.0F) &&
               !precision_laser->needs_drop_compensation &&
               is_laser_gun(InventoryItemId::modern_precision_laser),
           "Modern item 149 must retain its ten-round high-zoom precision-laser contract");
    expect(openminerz::gun_stats(InventoryItemId::modern_advanced_grenade_launcher) == nullptr &&
               openminerz::gun_stats(InventoryItemId::modern_multi_laser) == nullptr &&
               near(openminerz::item_enemy_damage(InventoryItemId::modern_chainsaw_1), 5.0F) &&
               near(openminerz::item_self_damage_per_use(InventoryItemId::modern_chainsaw_1),
                    0.0002F) &&
               near(openminerz::dig_time_seconds(InventoryItemId::modern_chainsaw_1,
                                                 openminerz::world::BlockType::log),
                    0.0F),
           "only registered Modern special items may gain behavior, including item 123's "
           "BloodStone chainsaw");
    expect(near(openminerz::guided_lock_seconds(0.0F, 0.0F, 1.0F), 1.5F) &&
               near(openminerz::guided_lock_seconds(125.0F, 0.5F, 1.0F), 5.5F) &&
               near(openminerz::guided_lock_seconds(250.0F, 1.0F, 1.0F), 9.5F),
           "guided rocket lock time must combine the original base, range, and angle terms");
    expect(near(openminerz::guided_lock_beep_seconds(9.5F, 0.0F), 1.0F) &&
               near(openminerz::guided_lock_beep_seconds(9.5F, 9.5F), 0.156F),
           "guided rocket pre-lock beeps must accelerate over the original 9.5-second span");
    expect(openminerz::gun_stats(InventoryItemId::stone_pickaxe) == nullptr &&
               openminerz::gun_stats(InventoryItemId::torch) == nullptr,
           "non-gun items must never resolve gun stats");

    const auto &book = openminerz::crafting_recipes();
    expect(book.size() == 107U,
           "the vanilla cookbook must carry all 107 authored Receipe.CookBook entries");
    for (const auto &recipe : book)
    {
        expect(recipe.result != InventoryItemId::bare_hands && recipe.result_count > 0U,
               "every recipe must produce a real item");
        bool has_ingredient = false;
        for (const auto &ingredient : recipe.ingredients)
            if (ingredient.count > 0U)
            {
                has_ingredient = true;
                expect(ingredient.item != InventoryItemId::bare_hands,
                       "recipe ingredients must be real items");
            }
        expect(has_ingredient, "every recipe must consume at least one ingredient");
    }

    // Craft a stone pickaxe: 4 rock + 2 sticks, removed from storage before tray.
    PlayerInventory inventory;
    inventory.storage().fill(std::nullopt);
    inventory.tray().fill(std::nullopt);
    const auto pickaxe_recipe =
        std::find_if(book.begin(), book.end(), [](const openminerz::CraftingRecipe &recipe) {
            return recipe.result == InventoryItemId::stone_pickaxe;
        });
    expect(pickaxe_recipe != book.end(), "the stone pickaxe recipe must exist");
    expect(!inventory.can_craft(*pickaxe_recipe), "an empty inventory must not satisfy any recipe");
    inventory.storage()[0] = openminerz::InventoryStack{InventoryItemId::rock_block, 3U};
    inventory.tray()[0] = openminerz::InventoryStack{InventoryItemId::rock_block, 2U};
    inventory.tray()[1] = openminerz::InventoryStack{InventoryItemId::stick, 2U};
    expect(inventory.can_craft(*pickaxe_recipe),
           "aggregate ingredient counts across sections must satisfy CanCraft");
    expect(inventory.craft(*pickaxe_recipe) == 0U,
           "crafting with space left must place the full result");
    expect(inventory.count(InventoryItemId::stone_pickaxe) == 1U,
           "crafting must place exactly one result");
    expect(inventory.count(InventoryItemId::rock_block) == 1U,
           "crafting must consume exactly four rock");
    expect(inventory.count(InventoryItemId::stick) == 0U,
           "crafting must consume exactly two sticks");
    expect(!inventory.storage()[0].has_value(),
           "crafting must consume storage ingredients before the tray");
    // No slot may ever hold a zero-count phantom stack; that ghost used to appear
    // whenever an add fully fit into the tray with storage slots still open.
    for (const auto &slot : inventory.storage())
        expect(!slot.has_value() || slot->count > 0U,
               "storage must never contain a zero-count phantom stack");
    for (const auto &slot : inventory.tray())
        expect(!slot.has_value() || slot->count > 0U,
               "the tray must never contain a zero-count phantom stack");

    // Crafted guns come out with a full clip.
    PlayerInventory gun_bench;
    gun_bench.storage().fill(std::nullopt);
    gun_bench.tray().fill(std::nullopt);
    gun_bench.tray()[0] = openminerz::InventoryStack{InventoryItemId::iron, 2U};
    gun_bench.tray()[1] = openminerz::InventoryStack{InventoryItemId::wood_block, 2U};
    const auto pistol_recipe =
        std::find_if(book.begin(), book.end(), [](const openminerz::CraftingRecipe &recipe) {
            return recipe.result == InventoryItemId::pistol;
        });
    expect(pistol_recipe != book.end() && gun_bench.craft(*pistol_recipe) == 0U,
           "the pistol recipe must craft from 2 iron bars and 2 wood");
    bool crafted_full_clip = false;
    for (const auto &slot : gun_bench.tray())
        if (slot.has_value() && slot->item == InventoryItemId::pistol)
            crafted_full_clip = slot->rounds_in_clip == 8U;
    expect(crafted_full_clip, "a crafted pistol must start with a full 8-round clip");

    // TimeToDig parity: tool tier tables plus the bare-hand fallback.
    using openminerz::dig_time_seconds;
    using openminerz::item_enemy_damage;
    using BT = openminerz::world::BlockType;
    expect(near(dig_time_seconds(InventoryItemId::stone_pickaxe, BT::rock), 2.0F) &&
               near(dig_time_seconds(InventoryItemId::iron_pickaxe, BT::rock), 0.5F) &&
               near(dig_time_seconds(InventoryItemId::diamond_pickaxe, BT::rock), 0.1F),
           "pick tiers must keep the original rock dig times");
    expect(std::isinf(dig_time_seconds(InventoryItemId::bare_hands, BT::iron_ore)) &&
               std::isinf(dig_time_seconds(InventoryItemId::stone_pickaxe, BT::gold_ore)) &&
               near(dig_time_seconds(InventoryItemId::iron_pickaxe, BT::gold_ore), 6.0F),
           "ore gating must require the right pick tier");
    expect(near(dig_time_seconds(InventoryItemId::bare_hands, BT::rock), 10.0F) &&
               near(dig_time_seconds(InventoryItemId::iron_axe, BT::log), 0.5F) &&
               near(dig_time_seconds(InventoryItemId::iron_spade, BT::dirt), 0.25F),
           "base, axe and spade tables must match the original");
    expect(std::isinf(dig_time_seconds(InventoryItemId::bare_hands, BT::tnt)) &&
               near(dig_time_seconds(InventoryItemId::stone_spade, BT::tnt), 1.0F),
           "only spades may remove TNT");
    expect(
        near(dig_time_seconds(InventoryItemId::blood_stone_laser_sword, BT::blood_stone), 3.0F) &&
            near(dig_time_seconds(InventoryItemId::iron_laser_sword, BT::wood), 0.01F),
        "laser swords must combine the laser and metal-tier tables");
    expect(
        near(dig_time_seconds(InventoryItemId::bare_hands, BT::crate_stone), 2.0F) &&
            near(dig_time_seconds(InventoryItemId::bloodstone_pickaxe, BT::crate_bloodstone), 2.0F),
        "every stock container must take two seconds through the base-item branch");
    // Melee EnemyDamage registrations.
    expect(near(item_enemy_damage(InventoryItemId::bloodstone_pickaxe), 3.0F) &&
               near(item_enemy_damage(InventoryItemId::iron_laser_sword), 8.0F) &&
               near(item_enemy_damage(InventoryItemId::diamond_axe), 2.0F) &&
               near(item_enemy_damage(InventoryItemId::knife), 0.5F) &&
               near(item_enemy_damage(InventoryItemId::bare_hands), 0.025F),
           "melee EnemyDamage must match the original registrations");
}

void enemy_roster_matches_the_original_tables()
{
    using namespace openminerz::world;

    const Vec3 ballistic_origin{0.0F, 1.5F, 0.0F};
    const Vec3 ballistic_target{10.0F, 0.5F, 0.0F};
    const auto ballistic =
        initial_ballistic_vector(ballistic_origin, ballistic_target, 25.0F, -10.0F);
    expect(ballistic.has_value(), "a normal archer target must have a ballistic solution");
    if (ballistic.has_value())
    {
        const float speed = std::sqrt(ballistic->x * ballistic->x + ballistic->y * ballistic->y +
                                      ballistic->z * ballistic->z);
        const float flight_time = 10.0F / ballistic->x;
        const float impact_y =
            ballistic_origin.y + ballistic->y * flight_time - 5.0F * flight_time * flight_time;
        expect(near(speed, 25.0F, 0.001F) && near(impact_y, ballistic_target.y, 0.001F),
               "archer ballistic velocity must retain speed and intersect the target height");
    }
    const auto vertical_ballistic =
        initial_ballistic_vector({1.0F, 0.0F, 1.0F}, {1.0F, 4.0F, 1.0F}, 25.0F, -10.0F);
    expect(vertical_ballistic.has_value() && near(vertical_ballistic->x, 0.0F) &&
               near(vertical_ballistic->y, 25.0F) && near(vertical_ballistic->z, 0.0F) &&
               !initial_ballistic_vector(ballistic_origin, {1000.0F, 0.5F, 0.0F}, 25.0F, -10.0F)
                    .has_value(),
           "the ballistic solver must preserve vertical shots and reject unreachable arcs");

    WorldRegion fireball_world{0, 0, 1, 1};
    fireball_world.set_block(8, 2, 6, BlockType::rock);
    PlayerState fireball_target;
    fireball_target.position = {8.0F, 1.0F, 8.0F};
    const auto player_first =
        fireball_sweep(fireball_world, fireball_target, {8.0F, 2.0F, 2.0F}, {8.0F, 2.0F, 9.0F});
    expect(player_first.kind == FireballSweepKind::player &&
               near(player_first.position.z, 7.08F, 0.001F),
           "fireballs must sweep their 0.57 box and preserve original player-first tracing");
    fireball_target.position.x = 1.0F;
    const auto terrain_hit =
        fireball_sweep(fireball_world, fireball_target, {8.0F, 2.0F, 2.0F}, {8.0F, 2.0F, 9.0F});
    expect(terrain_hit.kind == FireballSweepKind::terrain &&
               near(terrain_hit.position.z, 5.43F, 0.001F),
           "a fireball that misses the player must stop on the expanded blocking voxel");

    WorldRegion arrow_world{0, 0, 1, 1};
    PlayerState arrow_target;
    arrow_target.position = {8.0F, 1.0F, 8.0F};
    const auto arrow_player =
        arrow_sweep(arrow_world, arrow_target, true, {8.0F, 1.5F, 5.0F}, {8.0F, 1.5F, 9.0F});
    expect(arrow_player.kind == ArrowSweepKind::player &&
               near(arrow_player.position.z, 7.65F, 0.001F),
           "arrows must sweep the complete tracer segment into the exact player AABB");
    arrow_world.set_block(8, 1, 6, BlockType::rock);
    const auto covered_arrow =
        arrow_sweep(arrow_world, arrow_target, true, {8.0F, 1.5F, 5.0F}, {8.0F, 1.5F, 9.0F});
    expect(covered_arrow.kind == ArrowSweepKind::terrain &&
               near(covered_arrow.position.z, 6.0F, 0.001F),
           "nearer blocking terrain must intercept an arrow before the target player");
    arrow_world.set_block(8, 1, 6, BlockType::leaves);
    const auto through_leaves =
        arrow_sweep(arrow_world, arrow_target, true, {8.0F, 1.5F, 5.0F}, {8.0F, 1.5F, 9.0F});
    expect(
        through_leaves.kind == ArrowSweepKind::player &&
            !arrow_sweep(arrow_world, arrow_target, false, {8.0F, 1.5F, 5.0F}, {8.0F, 1.5F, 9.0F}),
        "arrows must pass leaves and omit invalid/dead targets from collision");

    // Zombie ramp: health 1..18 with the final tier at 1.5x, speed keyed to type/16.
    expect(near(enemy_stats(0).starting_health, 1.0F) &&
               near(enemy_stats(10).starting_health, 11.0F) &&
               near(enemy_stats(17).starting_health, 27.0F),
           "zombie tiers must keep the original 1..18 (x1.5 final) health ramp");
    expect(near(enemy_stats(0).base_slow_speed, 0.5F) &&
               near(enemy_stats(0).base_fast_speed, 6.5F) &&
               near(enemy_stats(0).base_run_activation_time, 4.0F) &&
               near(enemy_stats(0).fast_jump_speed, 13.0F) && enemy_stats(0).has_run_fast,
           "tier-zero zombies must keep their authored speed package");
    expect(enemy_stats(0).hardest_block_dug == 2 && enemy_stats(9).hardest_block_dug == 3 &&
               near(enemy_stats(0).digging_multiplier, 0.1F) &&
               near(enemy_stats(17).digging_multiplier, 1.0F),
           "zombie digging capability must escalate exactly like the original tiers");
    // Archers: +2.5 health per tier, huge 40-cell spawn radius, aboveground.
    expect(near(enemy_stats(18).starting_health, 1.0F) &&
               near(enemy_stats(25).starting_health, 18.5F) && enemy_stats(18).spawn_radius == 40 &&
               enemy_stats(18).habitat == EnemyHabitat::aboveground &&
               enemy_stats(18).enemy_class == EnemyClass::archer,
           "archer skeletons must match their original ramp and spawn radius");
    // Cave skeletons: +0.9 per step, cave habitat.
    expect(near(enemy_stats(26).starting_health, 1.0F) &&
               near(enemy_stats(49).starting_health, 1.0F + 0.9F * 23.0F) &&
               enemy_stats(26).habitat == EnemyHabitat::caves,
           "cave skeletons must keep the 0.9-per-step health ramp");
    expect(enemy_stats(31).enemy_class == EnemyClass::skeleton_sword &&
               enemy_stats(38).enemy_class == EnemyClass::skeleton_axes,
           "the sword/axes interleave must match EnemyType.Init ordering");
    expect(near(enemy_stats(enemy_felguard).starting_health, 140.0F) &&
               enemy_stats(enemy_felguard).habitat == EnemyHabitat::hell &&
               enemy_uses_cave_spawn_budget(enemy_stats(enemy_felguard).habitat) &&
               enemy_uses_cave_spawn_budget(enemy_stats(26).habitat) &&
               !enemy_uses_cave_spawn_budget(enemy_stats(0).habitat) &&
               near(enemy_stats(enemy_alien).starting_health, 70.0F) &&
               enemy_stats(enemy_alien).habitat == EnemyHabitat::crash_site,
           "the Felguard and Alien must keep 140 and 70 health");

    // Damage multipliers per class.
    expect(near(enemy_damage_multiplier(0, DamageFlags::blunt, false), 0.5F) &&
               near(enemy_damage_multiplier(0, DamageFlags::bullet, true), 2.5F) &&
               near(enemy_damage_multiplier(enemy_alien, DamageFlags::piercing, false), 0.25F) &&
               near(enemy_damage_multiplier(26, DamageFlags::shotgun, false), 1.5F) &&
               near(enemy_damage_multiplier(enemy_felguard, DamageFlags::piercing, true), 1.0F),
           "per-class damage multipliers must match the original tables");

    // Selection curve: at the origin only the first band is reachable.
    expect(find_enemy(188.88889F, 0.0F, 0, 17, 0.0F) == 0 &&
               find_enemy(188.88889F, 0.0F, 0, 17, 0.99F) == 0,
           "distance zero must always select the weakest zombie band");
    expect(find_enemy(188.88889F, 100000.0F, 0, 17, 0.0F) == 17,
           "far distances must clamp into the strongest band");
    expect(near(enemy_midnight_factor(0.0F, 0.0F, -50.0F), 0.85F),
           "underground midnight factor must be the fixed 0.85");
    expect(near(minimum_enemy_spawn_seconds(0.0F, false), 45.0F) &&
               near(minimum_enemy_spawn_seconds(3450.0F, false), 20.0F),
           "easy spawn cadence must lerp 45..20 seconds");
    expect(near(minimum_enemy_spawn_seconds(0.0F, true), 5.0F) &&
               near(minimum_enemy_spawn_seconds(3450.0F, true), 1.0F),
           "hard-branch spawn cadence must lerp 5..1 seconds");

    // Loot: hell pays explosive powder, crash sites pay bars/diamond.
    EnemyRng rng;
    const EnemyLoot hell_loot = enemy_loot(enemy_felguard, 0.0F, 10.0F, rng);
    expect(hell_loot.item_id == 108U && hell_loot.item_count >= 1U,
           "felguards must always drop explosive powder");
    bool crash_valid = true;
    for (int roll = 0; roll < 32; ++roll)
    {
        const EnemyLoot loot = enemy_loot(enemy_alien, 0.0F, 10.0F, rng);
        crash_valid =
            crash_valid && (loot.item_id == 19U || loot.item_id == 20U || loot.item_id == 21U);
    }
    expect(crash_valid, "aliens must drop copper, iron, or diamond");

    EnemyRng dragon_rng;
    bool dragon_loot_valid = true;
    for (int roll{}; roll < 64; ++roll)
    {
        const std::vector<EnemyLoot> loot = dragon_loot({0.0F, 80.0F, 0.0F}, dragon_rng);
        const std::size_t powder = static_cast<std::size_t>(
            std::ranges::count(loot, std::uint16_t{108U}, &EnemyLoot::item_id));
        dragon_loot_valid =
            dragon_loot_valid && loot.size() >= 4U && loot.size() <= 11U && powder >= 2U &&
            powder <= 4U && std::ranges::all_of(loot, [](const EnemyLoot &drop) {
                return drop.item_count == 1U &&
                       (drop.item_id == 108U || drop.item_id == 7U || drop.item_id == 19U ||
                        drop.item_id == 20U || drop.item_id == 21U || drop.item_id == 22U);
            });
    }
    expect(dragon_loot_valid,
           "near-origin dragon loot must preserve two-to-four powder and two-to-seven material "
           "pickups");
    const std::vector<EnemyLoot> far_dragon_loot = dragon_loot({5000.0F, 0.0F, 0.0F}, dragon_rng);
    expect(std::ranges::all_of(
               far_dragon_loot,
               [](const EnemyLoot &drop) { return drop.item_id == 108U || drop.item_id == 7U; }),
           "at the distance cap, the material roll must collapse to bloodstone exactly");

    // Attack tables.
    const EnemyAttackSet &zombie_attacks = enemy_attacks(EnemyClass::zombie);
    expect(zombie_attacks.attack_count == 5 && near(zombie_attacks.attacks[4].damage, 0.6F) &&
               near(zombie_attacks.attacks[1].hit_times[1], 1.466667F) &&
               zombie_attacks.attacks[1].hit_count == 2,
           "zombie attack table must match ZombieTryAttack");
    expect(enemy_attacks(EnemyClass::skeleton_axes).attack_count == 11 &&
               enemy_attacks(EnemyClass::skeleton_axes).attacks[7].hit_count == 4,
           "axe skeletons must keep their four-hit flurries");

    // Dragons: schedule and stats.
    expect(near(dragon_stats(DragonKind::fire).starting_health, 20.0F) &&
               near(dragon_stats(DragonKind::undead).starting_health, 1000.0F) &&
               near(dragon_stats(DragonKind::ice).fireball_velocity, 60.0F),
           "dragon health and fireball data must match DragonType.Init");
    expect(near(dragon_schedule_minutes(DragonKind::fire), 32.0F) &&
               near(dragon_schedule_minutes(DragonKind::sand), 64.0F) &&
               near(dragon_schedule_minutes(DragonKind::undead), 80.0F),
           "the timed-dragon schedule must escalate 32/48/64/80/80 minutes");
    Dragon collision_dragon;
    collision_dragon.position = {0.0F, 0.0F, 0.0F};
    const auto dragon_head =
        trace_dragon(collision_dragon, {0.0F, 1.0F, -15.0F}, {0.0F, 0.0F, 1.0F}, 30.0F);
    const auto dragon_body =
        trace_dragon(collision_dragon, {0.0F, -0.5F, -15.0F}, {0.0F, 0.0F, 1.0F}, 30.0F);
    expect(dragon_head.has_value() && dragon_head->head && dragon_head->explosive_headshot &&
               near(dragon_head->distance, 5.375F, 0.001F) && dragon_body.has_value() &&
               !dragon_body->head && !dragon_body->explosive_headshot &&
               near(dragon_body->distance, 10.8F, 0.001F) &&
               !trace_dragon(collision_dragon, {2.0F, 1.0F, -15.0F}, {0.0F, 0.0F, 1.0F}, 30.0F),
           "dragon rounds must use the authored head/body boxes instead of a broad sphere");
    collision_dragon.yaw = 1.57079632679F;
    const auto turned_dragon =
        trace_dragon(collision_dragon, {-15.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 30.0F);
    expect(turned_dragon.has_value() && turned_dragon->head &&
               near(turned_dragon->distance, 5.375F, 0.001F),
           "dragon hit volumes must rotate with its live flight attitude");
    expect(near(damage_transmission(BlockType::dirt), 0.8F) &&
               near(damage_transmission(BlockType::rock), 0.5F) &&
               near(damage_transmission(BlockType::space_rock), 0.1F),
           "DamageLOSProbe material transmission must match BlockType.blockTypes");
    WorldRegion dragon_los{0, 0, 1, 1};
    const Vec3 blast{2.5F, 2.5F, 8.5F};
    const Vec3 target{6.5F, 2.5F, 8.5F};
    expect(
        near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::fire, blast, target), 1.0F),
        "an unobstructed dragon blast must retain full line-of-sight damage");
    dragon_los.set_block(4, 2, 8, BlockType::dirt);
    expect(
        near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::fire, blast, target), 0.8F),
        "one full dirt voxel must transmit exactly 80 percent dragon damage");
    dragon_los.set_block(5, 2, 8, BlockType::dirt);
    expect(
        near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::fire, blast, target), 0.64F),
        "DamageLOSProbe must multiply attenuation across consecutive blocks");
    dragon_los.set_block(4, 2, 8, BlockType::rock);
    dragon_los.set_block(5, 2, 8, BlockType::empty);
    expect(
        near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::fire, blast, target),
             0.0F) &&
            near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::forest, blast, target),
                 0.5F),
        "fire-dragon-protected rock must stop damage while a stronger dragon transmits half");
    dragon_los.set_block(4, 2, 8, BlockType::diamond_wall);
    expect(near(dragon_fireball_damage_multiplier(dragon_los, DragonKind::undead, blast, target),
                0.0F),
           "diamond walls must shelter the player even from the undead dragon");
}

void dragon_attitude_and_spawn_heading_match_xna_flight()
{
    using namespace openminerz::world;

    constexpr float half_pi = 1.57079632679F;
    const DragonBasis north = dragon_attitude_basis(0.0F, 0.0F, 0.0F);
    const DragonBasis west = dragon_attitude_basis(half_pi, 0.0F, 0.0F);
    const DragonBasis climb = dragon_attitude_basis(0.0F, 0.4F, 0.0F);
    const DragonBasis bank = dragon_attitude_basis(0.35F, -0.2F, 0.7F);
    const auto dot = [](Vec3 left, Vec3 right) {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    };
    const auto length = [&](Vec3 value) { return std::sqrt(dot(value, value)); };
    expect(near(north.forward.x, 0.0F) && near(north.forward.y, 0.0F) &&
               near(north.forward.z, -1.0F) && near(west.forward.x, -1.0F) &&
               near(west.forward.z, 0.0F) && climb.forward.y > 0.38F,
           "dragon yaw/pitch must use XNA local -Z forward rather than the mirrored port basis");
    expect(near(length(bank.right), 1.0F) && near(length(bank.up), 1.0F) &&
               near(length(bank.forward), 1.0F) && near(dot(bank.right, bank.up), 0.0F) &&
               near(dot(bank.right, bank.forward), 0.0F) && near(dot(bank.up, bank.forward), 0.0F),
           "dragon attitude rows must remain orthonormal during combined pitch and roll");

    PlayerState player;
    player.position = {14.0F, 3.0F, -21.0F};
    EnemyRng rng;
    DragonFlight flight;
    flight.spawn(DragonKind::fire, player.position, rng);
    const Dragon &spawned = flight.dragon();
    const DragonBasis spawn_basis = dragon_attitude_basis(spawned.yaw, spawned.pitch, spawned.roll);
    const Vec3 toward_player{player.position.x - spawned.position.x, 0.0F,
                             player.position.z - spawned.position.z};
    const float approach_alignment =
        (spawn_basis.forward.x * toward_player.x + spawn_basis.forward.z * toward_player.z) /
        std::max(0.001F, std::hypot(toward_player.x, toward_player.z));
    expect(spawned.yaw >= -half_pi && spawned.yaw <= half_pi && approach_alignment > 0.999F &&
               near(spawned.velocity, 20.0F),
           "InitSpawnState must begin 750 metres out already facing its approach path");

    DragonFlight replica;
    replica.install_replica(DragonKind::sand, 123.0F);
    replica.apply_network_waypoint({0.0F, 70.0F, -5.0F}, {10.0F, 0.0F, 0.0F}, 1.0F, 0.4F, 1U);
    replica.apply_network_waypoint({10.0F, 70.0F, -5.0F}, {0.0F, 0.0F, 0.0F}, 2.0F, 0.4F, 1U);
    replica.extrapolate_network_replica(0.5F);
    expect(replica.active() && !replica.visible(),
           "a stock dragon client must stay hidden until three action waypoints are buffered");
    replica.apply_network_waypoint({10.0F, 70.0F, -5.0F}, {0.0F, 0.0F, 0.0F}, 3.0F, 0.4F, 1U);
    replica.extrapolate_network_replica(0.0F);
    expect(replica.visible() && near(replica.dragon().position.x, 0.0F) &&
               near(replica.dragon().position.y, 70.0F) && near(replica.dragon().position.z, -5.0F),
           "the third waypoint must reveal the replica at the first buffered host sample");
    replica.extrapolate_network_replica(0.5F);
    expect(replica.dragon().kind == DragonKind::sand && near(replica.dragon().health, 123.0F) &&
               near(replica.dragon().position.x, 6.25F) &&
               near(replica.dragon().position.y, 70.0F) &&
               near(replica.dragon().position.z, -5.0F) && near(replica.dragon().velocity, 12.5F) &&
               near(replica.dragon().yaw, -half_pi) && replica.dragon().clip == "fly_forward",
           "the non-owner replica must use the stock cubic Hermite position and exact derivative "
           "without running a second flight brain");

    DragonFlight action_replica;
    action_replica.install_replica(DragonKind::ice, 50.0F);
    action_replica.reset_network_replica({0.0F, 70.0F, 0.0F}, {0.0F, 0.0F, -10.0F}, 1.0F, 0.0F, 0U);
    expect(action_replica.apply_network_attack_waypoint({0.0F, 70.0F, -10.0F}, {0.0F, 0.0F, -10.0F},
                                                        2.0F, 0.0F, 0U, 1U, {0.0F, 70.0F, -40.0F},
                                                        123, false),
           "a valid quick-shot action waypoint must enter the replica queue");
    expect(action_replica.take_network_replica_events().empty(),
           "queued dragon actions must not execute when their packet arrives");
    action_replica.extrapolate_network_replica(1.0F);
    const std::vector<DragonReplicaEvent> quick_events =
        action_replica.take_network_replica_events();
    expect(quick_events.size() == 2U && quick_events[0].kind == DragonReplicaEventKind::fireball &&
               quick_events[0].fireball_id == 123 && near(quick_events[0].position.y, 69.0F) &&
               near(quick_events[0].position.z, -4.0F) &&
               quick_events[1].kind == DragonReplicaEventKind::cry,
           "QUICKSHOOT and CRY must execute together only when interpolation crosses the action "
           "waypoint");

    action_replica.reset_network_replica({0.0F, 70.0F, 0.0F}, {0.0F, 0.0F, -10.0F}, 10.0F, 0.0F,
                                         0U);
    expect(action_replica.apply_network_attack_waypoint({0.0F, 70.0F, -10.0F}, {0.0F, 0.0F, -10.0F},
                                                        11.0F, 0.0F, 0U, 0U, {0.0F, 70.0F, -40.0F},
                                                        124, true),
           "a valid animated-shot action waypoint must enter the replica queue");
    action_replica.extrapolate_network_replica(1.0F);
    expect(action_replica.take_network_replica_events().empty() &&
               action_replica.dragon().clip == "attack",
           "ANIMSHOOT must enter the attack clip without firing before the stock muzzle frame");
    action_replica.extrapolate_network_replica(0.14F);
    const std::vector<DragonReplicaEvent> animated_events =
        action_replica.take_network_replica_events();
    expect(animated_events.size() == 1U &&
               animated_events[0].kind == DragonReplicaEventKind::fireball &&
               animated_events[0].fireball_id == 124,
           "ANIMSHOOT must fire only after the recovered 1.133333-second attack threshold");
}

void enemy_network_identity_seed_and_target_authority_are_deterministic()
{
    using namespace openminerz::world;

    EnemyInstanceRng dotnet{0};
    expect(dotnet.next_sample() == 1'559'595'546 && dotnet.next_int(0, 100) == 81 &&
               dotnet.next_int(3, 5) == 4 && near(dotnet.next01(), 0.5581612F),
           "per-enemy randomness must reproduce the seeded .NET Framework System.Random stream");

    const EnemyInitialization initialization{2.0F, 4.0F, 1.0F, 0.0F, 0.0F};
    EnemyHorde horde;
    expect(horde.spawn_networked(17, {4.0F, 1.0F, 8.0F}, initialization, 7U, 1234, 5U) &&
               horde.spawn_networked(17, {10.0F, 1.0F, 8.0F}, initialization, 7U, 1234, 6U) &&
               !horde.spawn_networked(17, {6.0F, 1.0F, 8.0F}, initialization, 7U, 1234, 5U),
           "enemy identity must be the source pair (target GID, sender-local enemy ID)");
    expect(horde.enemies().size() == 2U && horde.enemies()[0].random_seed == 1234 &&
               horde.enemies()[0].target_gid == 5U && horde.enemies()[1].target_gid == 6U &&
               horde.enemies()[0].clip == horde.enemies()[1].clip &&
               near(horde.enemies()[0].yaw, horde.enemies()[1].yaw),
           "the packet seed and initialization must recreate identical per-enemy decisions");

    WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);
    PlayerState local;
    local.position = {12.0F, 1.0F, 8.0F};
    for (Enemy &enemy : horde.enemies())
        enemy.state = EnemyState::chase;
    const auto lookup = [](std::uint8_t gid) -> std::optional<PlayerState> {
        if (gid != 6U)
            return std::nullopt;
        PlayerState remote;
        remote.position = {2.0F, 1.0F, 8.0F};
        return remote;
    };
    const EnemyEvents events = horde.update(region, local, 5U, lookup, 1.0F / 60.0F);
    expect(horde.enemies()[0].velocity.x > 0.0F && horde.enemies()[1].velocity.x < 0.0F,
           "each peer must simulate an enemy against the player named by its target GID");
    expect(events.speed_up_requests.size() == 1U &&
               events.speed_up_requests.front() == EnemyIdentity{5U, 7U},
           "only the targeted player's local copy may originate a SpeedUpEnemy decision");
    expect(horde.speed_up_enemy({5U, 7U}) && horde.enemies()[0].running &&
               !horde.enemies()[1].running,
           "enemy decisions must resolve the complete identity rather than enemy ID alone");
    expect(horde.kill_enemy({6U, 7U}, false) && horde.enemies()[1].state == EnemyState::die &&
               horde.enemies()[0].state != EnemyState::die && !horde.enemies()[1].loot_pending,
           "a remote kill must affect only the exact target pair and never create local loot");
}

void enemy_horde_chases_attacks_and_dies()
{
    using namespace openminerz::world;

    WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);

    PlayerState player;
    player.position = {12.0F, 1.0F, 8.0F};

    EnemyHorde horde;
    horde.spawn(0, {4.0F, 1.1F, 8.0F}, 1.0F, 1U);
    expect(horde.enemies().size() == 1U && near(horde.enemies()[0].health, 1.0F) &&
               horde.enemies()[0].state == EnemyState::emerge,
           "a spawned tier-zero zombie must start emerging with one health");
    expect(!horde.trace_along_ray({0.0F, 1.8F, 8.0F}, {1.0F, 0.0F, 0.0F}, 8.0F),
           "an emerging enemy must remain unhittable until its chase state begins");

    float total_damage{};
    for (int tick = 0; tick < 60 * 30; ++tick)
    {
        const EnemyEvents events = horde.update(region, player, 1.0F / 60.0F);
        total_damage += events.player_damage;
        if (total_damage > 0.0F)
            break;
    }
    expect(!horde.enemies().empty(), "the zombie must survive its own approach");
    const Enemy &zombie = horde.enemies()[0];
    expect(std::abs(zombie.position.x - player.position.x) < 2.5F &&
               std::abs(zombie.position.z - player.position.z) < 2.5F,
           "the zombie must chase across the platform to the player");
    expect(total_damage > 0.0F, "a reachable player must eventually take melee damage");
    expect(zombie.on_ground && std::abs(zombie.position.y - 1.0F) < 0.2F,
           "voxel collision must keep the zombie standing on the rock floor");

    // Construction/melee ray queries are non-mutating, reach the original five
    // blocks, and respect a nearer terrain limit supplied by the caller.
    const Vec3 melee_origin{zombie.position.x - 4.5F, 1.8F, zombie.position.z};
    const auto melee_hit = horde.trace_along_ray(melee_origin, {1.0F, 0.0F, 0.0F}, 5.0F);
    expect(melee_hit && melee_hit.index == 0 && near(melee_hit.normal.x, -1.0F) &&
               near(horde.enemies()[0].health, 1.0F),
           "a five-block construction ray must acquire a zombie without damaging it");
    expect(!horde.trace_along_ray(melee_origin, {1.0F, 0.0F, 0.0F}, 5.0F, 0),
           "grenade follow-up traces must be able to ignore the enemy just bounced from");
    const auto blocked_melee = horde.trace_along_ray(melee_origin, {1.0F, 0.0F, 0.0F}, 1.0F);
    expect(!blocked_melee,
           "a solid block nearer than the zombie must clip the melee construction ray");

    const Vec3 upper_torso_origin{zombie.position.x - 4.5F, zombie.position.y + 1.4F,
                                  zombie.position.z};
    const Vec3 head_origin{zombie.position.x - 4.5F, zombie.position.y + 1.55F, zombie.position.z};
    const auto upper_torso = horde.trace_along_ray(upper_torso_origin, {1.0F, 0.0F, 0.0F}, 5.0F);
    const auto head = horde.trace_along_ray(head_origin, {1.0F, 0.0F, 0.0F}, 5.0F);
    expect(upper_torso && !upper_torso.headshot && head && head.headshot,
           "headshots must begin strictly 1.5 units above enemy feet, not at upper torso height");

    EnemyHorde armored;
    armored.spawn(18, {4.0F, 1.0F, 8.0F}, 1.0F, 2U);
    armored.enemies()[0].state = EnemyState::chase;
    const Vec3 armored_torso{0.0F, 2.4F, 8.0F};
    armored.rng().state = 11177516664432764457ULL; // next01 = 0.9583, above 0.6
    expect(!armored.trace_projectile_along_ray(armored_torso, {1.0F, 0.0F, 0.0F}, 10.0F) &&
               armored.trace_along_ray(armored_torso, {1.0F, 0.0F, 0.0F}, 10.0F),
           "a rejected 60-percent torso strike must pass through while melee still connects");
    armored.rng().state = 1ULL; // next01 = 0, accepted
    expect(static_cast<bool>(
               armored.trace_projectile_along_ray(armored_torso, {1.0F, 0.0F, 0.0F}, 10.0F)),
           "an accepted armored torso strike must acquire the enemy");
    armored.rng().state = 11177516664432764457ULL;
    const std::uint64_t head_rng = armored.rng().state;
    expect(armored.trace_projectile_along_ray({0.0F, 2.55F, 8.0F}, {1.0F, 0.0F, 0.0F}, 10.0F) &&
               armored.rng().state == head_rng,
           "headshots must bypass the body-strike roll without consuming it");

    // A bullet along +x hits the zombie and kills the one-health tier instantly.
    const int hit = horde.damage_along_ray({0.0F, 1.8F, zombie.position.z}, {1.0F, 0.0F, 0.0F},
                                           30.0F, 1.0F, DamageFlags::bullet);
    expect(hit == 0 && horde.enemies()[0].state == EnemyState::die,
           "a bullet ray must strike the zombie and drop it to the dying state");
    int died{};
    for (int tick = 0; tick < 60 * 8 && died == 0; ++tick)
        died += horde.update(region, player, 1.0F / 60.0F).died;
    expect(died == 1 && horde.enemies().empty(),
           "a dead zombie must play out its corpse and despawn");
}

void modern_loot_blocks_match_owned_catalog_and_rng_transcripts()
{
    using namespace openminerz;
    using namespace openminerz::world;

    const auto catalog = modern_loot_catalog();
    expect(catalog.size() == 49U && catalog[0].wire_item == 44 && catalog[0].weight == 100 &&
               catalog[0].tier == 1 && catalog[0].minimum_quantity == 50 &&
               catalog[0].maximum_quantity_exclusive == 400 && catalog[17].wire_item == 129 &&
               catalog[17].item == InventoryItemId::modern_iron_container &&
               catalog[25].wire_item == 126 &&
               catalog[25].item == InventoryItemId::modern_sticky_grenade &&
               catalog[42].wire_item == 143 &&
               catalog[42].item == InventoryItemId::modern_laser_drill &&
               catalog[48].wire_item == 38,
           "the 49-entry private Modern loot catalog must retain order, weights and sparse IDs");

    LegacyRandom common_random{12345};
    const ModernLootDrop first = select_modern_loot(common_random, 100, 1, 5, false);
    const ModernLootDrop second = select_modern_loot(common_random, 100, 1, 5, false);
    expect(first.stack.item == InventoryItemId::bullets && first.stack.count == 74U &&
               second.stack.item == InventoryItemId::wood_block && second.stack.count == 3U,
           "seeded common loot must match the owned GetRandomLoot oracle including quantities");
    expect(
        modern_loot_stack_matches({InventoryItemId::bullets, 399U}, 35, 1, 5, false) &&
            !modern_loot_stack_matches({InventoryItemId::bullets, 400U}, 35, 1, 5, false) &&
            modern_loot_stack_matches({InventoryItemId::modern_laser_drill, 1U}, 35, 4, 10, true) &&
            !modern_loot_stack_matches({InventoryItemId::gold_lmg, 1U}, 35, 4, 10, true),
        "host loot validation must enforce exclusive quantities and rare weight eligibility");

    LegacyRandom normal_random{12345};
    const auto normal = process_modern_loot_block(normal_random, BlockType::loot_block, 35);
    expect(normal.has_value() && normal->pre_roll == 6 && normal->statistic_id == 9 &&
               normal->drops.size() == 1U &&
               normal->drops[0].stack.item == InventoryItemId::bullets &&
               normal->drops[0].stack.count == 321U && !normal->drops[0].rare_roll &&
               near(normal->drops[0].spawn_velocity[0], 0.05550989F) &&
               near(normal->drops[0].spawn_velocity[1], 0.4764631F) &&
               near(normal->drops[0].spawn_velocity[2], 1.42123234F),
           "normal loot blocks must preserve pre-roll, item and upward-pickup transcript");

    LegacyRandom lucky_random{12345};
    const auto lucky = process_modern_loot_block(lucky_random, BlockType::lucky_loot_block, 35);
    expect(lucky.has_value() && lucky->pre_roll == 6 && lucky->statistic_id == 10 &&
               lucky->drops.size() == 3U &&
               lucky->drops[0].stack.item == InventoryItemId::laser_bullets &&
               lucky->drops[0].stack.count == 377U && lucky->drops[0].rare_roll &&
               lucky->drops[1].stack.item == InventoryItemId::gold_lmg &&
               lucky->drops[1].stack.count == 1U && !lucky->drops[1].rare_roll &&
               near(lucky->drops[1].spawn_velocity[0], 1.0111177F) &&
               near(lucky->drops[1].spawn_velocity[1], 0.426872045F) &&
               near(lucky->drops[1].spawn_velocity[2], -1.02245855F) &&
               lucky->drops[2].stack.item == InventoryItemId::grenade &&
               lucky->drops[2].stack.count == 1U && !lucky->drops[2].rare_roll &&
               near(lucky->drops[2].spawn_velocity[0], -0.525181234F) &&
               near(lucky->drops[2].spawn_velocity[1], 0.466942251F) &&
               near(lucky->drops[2].spawn_velocity[2], -1.32519794F),
           "lucky blocks must interleave item and trajectory calls in the stock RNG order");

    LegacyRandom bomb_random{12345};
    expect(!process_modern_loot_block(bomb_random, BlockType::bomb_block, 35).has_value() &&
               !process_modern_loot_block(bomb_random, BlockType::rock, 35).has_value(),
           "BombBlock and ordinary terrain must not enter ShouldDropLoot/ProcessLootBlockOutput");

    LegacyRandom fallback_random{7};
    const ModernLootDrop fallback = select_modern_loot(fallback_random, 0, 99, 100, false);
    expect(fallback.stack.item == InventoryItemId::snow && fallback.stack.count == 1U &&
               fallback.value == 500,
           "an empty eligible set must preserve the original Snow fallback");
}

void modern_spawner_catalog_and_initialization_match_owned_reference_vectors()
{
    using namespace openminerz;
    using namespace openminerz::world;

    const auto definitions = spawner_definitions();
    expect(definitions.size() == 6U,
           "Modern must expose exactly the six deployed spawner families");
    expect(definitions[0].off_block == BlockType::enemy_spawn_off &&
               definitions[0].on_block == BlockType::enemy_spawn_on &&
               definitions[0].dim_block == BlockType::enemy_spawn_dim &&
               definitions[0].spawn_points == 1000 &&
               near(definitions[0].spawn_rate_seconds, 2.0F) && definitions[0].wave_count == 4 &&
               near(definitions[0].wave_intermission_seconds, 5.0F) &&
               definitions[0].minimum_tier == 1 && definitions[0].maximum_tier == 2 &&
               definitions[0].base_lucky_loot_chance == 20 &&
               definitions[0].lucky_loot_chance_per_tier == 7 &&
               definitions[0].package_flags == SpawnerPackageFlags::none,
           "the common spawner definition must match the deployed table exactly");
    expect(definitions[3].off_block == BlockType::alien_horde_off &&
               definitions[3].on_block == BlockType::alien_horde_on &&
               definitions[3].dim_block == BlockType::alien_horde_dim &&
               definitions[3].spawn_points == 1200 && definitions[3].wave_count == 3 &&
               near(definitions[3].wave_intermission_seconds, 8.0F) &&
               definitions[3].package_flags == SpawnerPackageFlags::alien,
           "Alien Horde must retain its separate high-budget family");
    expect(
        definitions[5].off_block == BlockType::boss_spawn_off &&
            definitions[5].spawn_points == 1000 && definitions[5].wave_count == 1 &&
            definitions[5].package_flags ==
                (SpawnerPackageFlags::epic | SpawnerPackageFlags::hell | SpawnerPackageFlags::boss),
        "the boss definition must retain its compound package mask");
    expect(find_spawner_definition(BlockType::alien_horde_dim) == &definitions[3] &&
               is_spawner_on_block(BlockType::boss_spawn_on) &&
               !is_spawner_on_block(BlockType::boss_spawn_off) &&
               is_spawner_clickable(BlockType::boss_spawn_off) &&
               !is_spawner_clickable(BlockType::alien_horde_off) &&
               is_spawner_preserved_loot(BlockType::loot_block),
           "lookup helpers must preserve the shipped AlienHorde clickable omission");

    expect(spawner_spawn_entries().size() == 19U,
           "the weighted spawn table must contain all nineteen deployed packages");
    expect(is_spawner_spawn_candidate(spawner_spawn_entries()[0], 0, 1, 5, false, 100,
                                      SpawnerPackageFlags::none) &&
               !is_spawner_spawn_candidate(spawner_spawn_entries()[14], 0, 1, 5, false, 100,
                                           SpawnerPackageFlags::none) &&
               is_spawner_spawn_candidate(spawner_spawn_entries()[14], 0, 1, 5, false, 100,
                                          SpawnerPackageFlags::alien) &&
               !is_spawner_spawn_candidate(spawner_spawn_entries()[16], 0, 1, 5, false, 250,
                                           SpawnerPackageFlags::alien),
           "package filtering must use the source all-flags and common-only rules");

    EnemyInstanceRng zombie_random{12345};
    EnemyInstanceRng tier17_random{12345};
    EnemyInstanceRng alien_random{12345};
    const auto zombie = make_spawner_enemy_initialization(0U, 0.0F, zombie_random);
    const auto tier17 = make_spawner_enemy_initialization(17U, 0.0F, tier17_random);
    const auto alien = make_spawner_enemy_initialization(52U, 0.0F, alien_random);
    expect(zombie == net::EnemyInitPayload{533, 6285, 1500, 4000, 45000} &&
               tier17 == net::EnemyInitPayload{2127, 7878, 1500, 2937, 45000} &&
               alien == net::EnemyInitPayload{2233, 0, 1000, 0, 0} &&
               !make_spawner_enemy_initialization(56U, 0.0F, alien_random).has_value(),
           "spawner enemy initialization must match the captured Modern payload vectors");

    EnemyInstanceRng difficulty_random{12345};
    const auto runtime = apply_spawner_difficulty(definitions[0], 0, 0, difficulty_random);
    expect(runtime.has_value() && runtime->spawn_points >= 1040 && runtime->spawn_points <= 1559 &&
               runtime->maximum_tier == 2 &&
               near(spawner_soft_capped_value(3000.0F, 1000.0F, 2000.0F), 3722.2222F),
           "difficulty must retain the source soft/hard-cap ordering and 80-120 percent roll");
}

void modern_spawner_state_machine_preserves_light_waves_rewards_and_reset_quirks()
{
    using namespace openminerz;
    using namespace openminerz::world;

    const SpawnerDefinition *common_definition =
        find_spawner_definition(BlockType::enemy_spawn_off);
    expect(common_definition != nullptr, "the common spawner definition must exist");
    EnemyInstanceRng common_world_random{12345};
    EnemyInstanceRng common_enemy_random{12346};
    SpawnerSimulation common{
        {4, 5, 6}, *common_definition, common_world_random, common_enemy_random};
    const auto start = common.start(0, 0);
    expect(start.size() == 3U && std::holds_alternative<SpawnerClearSurroundingsAction>(start[0]) &&
               std::get<SpawnerClearSurroundingsAction>(start[0]).radius == 3 &&
               std::holds_alternative<SpawnerTriggerLinkedAction>(start[1]) &&
               std::get<SpawnerTriggerLinkedAction>(start[1]).radius == 10 &&
               std::holds_alternative<SpawnerActivateAction>(start[2]) &&
               common.snapshot().state == SpawnerState::start_triggered,
           "start must emit clear, linked-trigger, and activation actions in source order");
    const auto activated = common.complete_start();
    const auto *on =
        activated.size() == 1U ? std::get_if<SpawnerBlockAction>(&activated.front()) : nullptr;
    expect(on != nullptr && on->block == BlockType::enemy_spawn_on &&
               common.snapshot().state == SpawnerState::activated &&
               common.snapshot().light_state == SpawnerLightState::on &&
               near(common.snapshot().spawn_timer_seconds, 2.0F) &&
               near(common.snapshot().light_timer_seconds, 0.8F),
           "CompleteStart must turn on the block before arming the first wave");

    const SpawnerTarget fallback{0U, 0U, "Server"};
    const auto next_id = [](const SpawnerTarget &) -> std::optional<std::int32_t> { return 1; };
    expect(common.advance(0.79F, {}, fallback, next_id).empty(),
           "the light must remain on through the authored 0.8 second interval");
    const auto dim_actions = common.advance(0.02F, {}, fallback, next_id);
    const auto *dim =
        dim_actions.size() == 1U ? std::get_if<SpawnerBlockAction>(&dim_actions.front()) : nullptr;
    expect(dim != nullptr && dim->block == BlockType::enemy_spawn_dim &&
               common.snapshot().light_state == SpawnerLightState::off,
           "the light must switch to the dim block after 0.8 seconds");
    const auto light_actions = common.advance(0.31F, {}, fallback, next_id);
    const auto *light = light_actions.size() == 1U
                            ? std::get_if<SpawnerBlockAction>(&light_actions.front())
                            : nullptr;
    expect(light != nullptr && light->block == BlockType::enemy_spawn_on,
           "the dim phase must switch back on after the authored 0.3 seconds");

    int next_enemy_id = 2;
    const SpawnerTarget target{7U, 8U, "Target"};
    bool saw_enemy{};
    for (int tick{}; tick < 1000 && !saw_enemy; ++tick)
    {
        const auto actions = common.advance(
            0.1F, std::span<const SpawnerTarget>{&target, 1U}, target,
            [&](const SpawnerTarget &) -> std::optional<std::int32_t> { return next_enemy_id++; });
        for (const auto &action : actions)
            if (const auto *enemy = std::get_if<SpawnerEnemyAction>(&action))
                saw_enemy = enemy->target == target && enemy->payload.player_name == "Target" &&
                            enemy->payload.spawner_position == net::Vector3{4.0F, 5.0F, 6.0F} &&
                            enemy->payload.spawn_position == net::Vector3{4.0F, 6.0F, 6.0F} &&
                            enemy->payload.spawn_value > 0;
    }
    expect(saw_enemy && common.snapshot().active_enemy_count > 0,
           "a wave event must emit a target-bound Modern SpawnEnemy payload");
    const int budget_before_reset = common.snapshot().remaining_spawn_value;
    const auto reset = common.handle_enemy_removed();
    expect(reset.size() == 4U &&
               std::get<SpawnerBlockAction>(reset[0]).block == BlockType::enemy_spawn_off &&
               std::get<SpawnerBlockAction>(reset[1]).block == BlockType::enemy_spawn_off &&
               std::holds_alternative<SpawnerResetBroadcastAction>(reset[2]) &&
               std::holds_alternative<SpawnerResetEnemiesAction>(reset[3]) &&
               common.snapshot().state == SpawnerState::listening,
           "enemy removal must preserve the deployed duplicate-off reset transcript");
    (void)common.start(0, 0);
    (void)common.complete_start();
    expect(common.snapshot().remaining_spawn_value > budget_before_reset,
           "reset must preserve the source remaining-budget accumulation quirk");

    const SpawnerDefinition *boss_definition = find_spawner_definition(BlockType::boss_spawn_off);
    expect(boss_definition != nullptr, "the boss spawner definition must exist");
    EnemyInstanceRng boss_world_random{12345};
    EnemyInstanceRng boss_enemy_random{12346};
    SpawnerSimulation boss{{10, -20, 30}, *boss_definition, boss_world_random, boss_enemy_random};
    (void)boss.start(0, 0);
    (void)boss.complete_start();
    std::size_t spawned{};
    for (int tick{}; tick < 1000 && boss.snapshot().state != SpawnerState::depleted; ++tick)
    {
        const auto actions = boss.advance(
            0.1F, std::span<const SpawnerTarget>{&target, 1U}, target,
            [&](const SpawnerTarget &) -> std::optional<std::int32_t> { return next_enemy_id++; });
        spawned += static_cast<std::size_t>(std::ranges::count_if(actions, [](const auto &action) {
            return std::holds_alternative<SpawnerEnemyAction>(action);
        }));
    }
    expect(spawned > 0U && boss.snapshot().state == SpawnerState::depleted &&
               boss.snapshot().active_enemy_count == static_cast<int>(spawned) &&
               boss.snapshot().remaining_wave_count == 0,
           "the single boss wave must deplete while retaining active enemy ownership");
    std::vector<SpawnerAction> completion;
    for (std::size_t index{}; index < spawned; ++index)
        completion = boss.handle_enemy_defeated(9U);
    std::size_t reward_count{};
    bool center_reward{};
    bool positive_x_reward{};
    bool negative_x_reward{};
    bool completion_notice{};
    for (const auto &action : completion)
    {
        if (const auto *reward = std::get_if<SpawnerBlockAction>(&action))
        {
            ++reward_count;
            center_reward = center_reward || reward->location == net::IntVector3{10, -20, 30};
            positive_x_reward =
                positive_x_reward || reward->location == net::IntVector3{11, -20, 30};
            negative_x_reward =
                negative_x_reward || reward->location == net::IntVector3{9, -20, 30};
            expect(reward->block == BlockType::loot_block ||
                       reward->block == BlockType::lucky_loot_block,
                   "boss rewards must remain one of the two deployed loot blocks");
        }
        if (const auto *completed = std::get_if<SpawnerCompletedAction>(&action))
            completion_notice = completed->last_killer_gid == 9U;
    }
    expect(boss.snapshot().state == SpawnerState::completed && reward_count >= 3U &&
               reward_count <= 5U && center_reward && positive_x_reward && negative_x_reward &&
               completion_notice,
           "boss completion must preserve the three mandatory and two probabilistic rewards");
}

void dragon_reacquires_a_living_target_between_attack_runs()
{
    using namespace openminerz::world;

    PlayerState player;
    player.position = {0.0F, 1.0F, 0.0F};
    EnemyRng rng;
    DragonFlight flight;
    flight.spawn(DragonKind::fire, player.position, rng);

    std::size_t fireballs{};
    bool entered_leave{};
    constexpr float fixed_delta = 1.0F / 60.0F;
    for (int tick = 0; tick < 60 * 360 && flight.active(); ++tick)
    {
        const DragonEvents events = flight.update(player, fixed_delta, rng);
        fireballs += events.fireball_origins.size();
        entered_leave = entered_leave || flight.dragon().state == DragonState::leave;
    }

    expect(flight.active() && !entered_leave && fireballs >= 8U,
           "a dragon with a living engaged target must reacquire it after each pass instead of "
           "misreading MaxLoiters as a three-attack despawn counter");
}

void dragon_departure_requires_sustained_target_loss()
{
    using namespace openminerz::world;

    PlayerState player;
    player.position = {0.0F, 1.0F, 0.0F};
    EnemyRng rng;
    DragonFlight flight;
    flight.spawn(DragonKind::fire, player.position, rng);

    constexpr float fixed_delta = 1.0F / 60.0F;
    for (int tick = 0; tick < 60 * 165 && flight.active(); ++tick)
        (void)flight.update(player, fixed_delta, rng, true);
    expect(flight.active() && flight.dragon().state != DragonState::leave,
           "a visible player must hold the dragon encounter well beyond two minutes");

    bool entered_leave{};
    for (int tick = 0; tick < 60 * 70 && flight.active(); ++tick)
    {
        (void)flight.update(player, fixed_delta, rng, false);
        entered_leave = entered_leave || flight.dragon().state == DragonState::leave;
    }
    expect(entered_leave,
           "a dragon must eventually depart after losing sight of an established target");
}

void enemy_hit_and_corpse_states_match_authored_timing()
{
    using namespace openminerz::world;

    WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);
    PlayerState player;
    player.position = {12.0F, 1.0F, 8.0F};

    EnemyHorde reactions;
    reactions.set_clip_durations([](EnemyModelKind, std::string_view clip) {
        if (clip == "hit_reaction1")
            return 1.65F;
        if (clip == "hit_reaction3")
            return 1.65F;
        return 1.0F;
    });
    reactions.spawn(17, {4.0F, 1.0F, 8.0F}, 1.0F, 1U);
    Enemy &zombie = reactions.enemies().front();
    for (int strike = 0; strike < 12; ++strike)
    {
        zombie.state = EnemyState::chase;
        reactions.damage_enemy(0U, 0.01F, DamageFlags::none, false);
        expect(zombie.clip == "hit_reaction1" || zombie.clip == "hit_reaction3",
               "ZombieHit must never select the unused second reaction clip");
    }
    zombie.state = EnemyState::chase;
    reactions.damage_enemy(0U, 0.01F, DamageFlags::none, false);
    (void)reactions.update(region, player, 0.69F);
    expect(zombie.state == EnemyState::hit,
           "hit reactions must remain active before the 0.25-second blend boundary");
    (void)reactions.update(region, player, 0.02F);
    expect(zombie.state == EnemyState::chase,
           "hit reactions must restart at the original near-animation-end boundary");

    EnemyHorde skeleton_deaths;
    bool saw_high_death{};
    for (std::uint32_t id = 0; id < 24U; ++id)
    {
        skeleton_deaths.spawn(49, {4.0F, 1.0F, 8.0F}, 0.85F, id);
        skeleton_deaths.damage_enemy(skeleton_deaths.enemies().size() - 1U, 1000.0F,
                                     DamageFlags::none, false);
        const std::string_view clip = skeleton_deaths.enemies().back().clip;
        expect(clip == "death1" || clip == "death2" || clip == "death3" || clip == "death4" ||
                   clip == "death5" || clip == "death6" || clip == "death7",
               "SkeletonDie must select one of all seven authored death clips");
        saw_high_death = saw_high_death || clip == "death5" || clip == "death6" || clip == "death7";
    }
    expect(saw_high_death,
           "the deterministic death sequence must exercise the restored high clips");

    EnemyHorde corpse;
    corpse.set_clip_durations([](EnemyModelKind, std::string_view clip) {
        if (clip == "death1")
            return 2.0F;
        if (clip == "death2")
            return 2.333F;
        if (clip == "death3")
            return 2.667F;
        return 1.0F;
    });
    corpse.spawn(0, {4.0F, 1.0F, 8.0F}, 1.0F, 2U);
    corpse.damage_enemy(0U, 1000.0F, DamageFlags::none, false);
    const float clip_seconds = corpse.enemies().front().clip == "death1"   ? 2.0F
                               : corpse.enemies().front().clip == "death2" ? 2.333F
                                                                           : 2.667F;
    const EnemyEvents first_dead_tick = corpse.update(region, player, 0.01F);
    expect(!corpse.enemies().front().loot_pending,
           "the local kill loot roll must run on the first simulation tick after death");
    (void)first_dead_tick;
    (void)corpse.update(region, player, clip_seconds / 2.0F + 4.8F);
    expect(!corpse.enemies().empty(),
           "the corpse must remain for five seconds after its scaled death clip finishes");
    const EnemyEvents removed = corpse.update(region, player, 0.25F);
    expect(removed.died == 1 && corpse.enemies().empty(),
           "the corpse must despawn after the authored clip plus the five-second hold");
}

void enemy_melee_respects_the_locked_facing_cone()
{
    using namespace openminerz::world;

    WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);

    const auto attacking_horde = [] {
        EnemyHorde horde;
        Enemy enemy;
        enemy.type = 0;
        enemy.state = EnemyState::attack;
        enemy.position = {8.0F, 1.0F, 8.0F};
        enemy.yaw = 0.0F; // Authored forward is -Z.
        enemy.health = 1.0F;
        enemy.clip = "attack1";
        enemy.clip_speed = 1.0F;
        enemy.attack_index = 0;
        horde.enemies().push_back(enemy);
        return horde;
    };

    PlayerState behind;
    behind.position = {8.0F, 1.0F, 9.0F};
    EnemyHorde missed = attacking_horde();
    const EnemyEvents back_swing = missed.update(region, behind, 0.84F);
    expect(near(back_swing.player_damage, 0.0F),
           "a locked melee swing must not damage a player behind the enemy");

    PlayerState ahead;
    ahead.position = {8.0F, 1.0F, 7.0F};
    EnemyHorde landed = attacking_horde();
    const EnemyEvents front_swing = landed.update(region, ahead, 0.84F);
    expect(near(front_swing.player_damage, 0.4F),
           "the same melee swing must land inside the original 0.7 facing cone");
}

void enemy_animation_states_use_authored_clips_and_timing()
{
    using namespace openminerz::world;

    WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            region.set_block(x, 0, z, BlockType::rock);
    PlayerState player;
    player.position = {12.0F, 1.0F, 8.0F};

    EnemyHorde zombies;
    bool saw_third_or_fourth{};
    for (std::uint32_t id = 0; id < 24U; ++id)
    {
        zombies.spawn(0, {4.0F, 1.0F, 8.0F}, 1.0F, id);
        const std::string_view clip = zombies.enemies().back().clip;
        expect(clip == "arise_1" || clip == "arise_2" || clip == "arise_3" || clip == "arise_4",
               "ZombieEmerge must select only arise_1 through arise_4");
        saw_third_or_fourth = saw_third_or_fourth || clip == "arise_3" || clip == "arise_4";
    }
    expect(saw_third_or_fourth,
           "the deterministic spawn sequence must exercise the restored emerge clips");

    EnemyHorde skeletons;
    skeletons.spawn(26, {4.0F, 1.0F, 8.0F}, 0.85F, 1U);
    expect(skeletons.enemies().front().state == EnemyState::chase,
           "ordinary cave skeletons must enter Chase directly without a bind-pose emerge tick");

    EnemyHorde archer;
    archer.set_clip_durations([](EnemyModelKind, std::string_view clip) {
        if (clip == "standup")
            return 2.15F;
        if (clip == "standup2")
            return 3.5F;
        if (clip == "atack_archer1")
            return 1.667F;
        if (clip == "idle-archer1")
            return 3.317F;
        if (clip == "enraged")
            return 3.667F;
        return 1.0F;
    });
    archer.spawn(18, {4.0F, 1.0F, 8.0F}, 1.0F, 2U);
    Enemy &enemy = archer.enemies().front();
    expect(enemy.state == EnemyState::emerge &&
               (enemy.clip == "standup" || enemy.clip == "standup2") && enemy.swing_count >= 5 &&
               enemy.swing_count <= 9,
           "archers must roll a 5..9-shot budget and play one of both stand-up clips");
    const float emerge_duration = enemy.clip == "standup" ? 2.15F : 3.5F;
    (void)archer.update(region, player, emerge_duration - 0.3F);
    expect(enemy.state == EnemyState::emerge,
           "archer emergence must not skip the authored stand-up animation");
    (void)archer.update(region, player, 0.06F);
    expect(enemy.state == EnemyState::chase,
           "archer emergence must transition at the original 0.25-second blend boundary");

    enemy.swing_count = 1;
    (void)archer.update(region, player, 0.01F);
    expect(enemy.state == EnemyState::attack && enemy.swing_count == 0 &&
               enemy.clip == "atack_archer1",
           "ArcherSkeletonTryAttack must consume, not reroll, its emergence shot budget");
    const EnemyEvents arrow = archer.update(region, player, 1.2F);
    expect(arrow.arrows_fired == 1,
           "the archer must release exactly one arrow after 1.1 authored seconds");
    (void)archer.update(region, player, 0.25F);
    expect(enemy.state == EnemyState::give_up && enemy.clip == "enraged",
           "an exhausted archer must use the skeleton enraged give-up clip");
    (void)archer.update(region, player, 3.7F);
    expect(archer.enemies().empty(),
           "give-up enemies must be removed when their authored clip finishes");
}

void enemy_digging_uses_block_hardness_fuses_and_door_ownership()
{
    using namespace openminerz::world;

    expect(
        enemy_dig_hardness(BlockType::sand) == 1 &&
            enemy_dig_hardness(BlockType::torch_pos_x) == 1 &&
            enemy_dig_hardness(BlockType::lower_door_closed_z) == 1 &&
            enemy_dig_hardness(BlockType::tnt) == 1 && enemy_dig_hardness(BlockType::dirt) == 2 &&
            enemy_dig_hardness(BlockType::crate) == 2 && enemy_dig_hardness(BlockType::rock) == 3 &&
            enemy_dig_hardness(BlockType::surface_lava) == 3 &&
            enemy_dig_hardness(BlockType::crate_stone) == 2 &&
            enemy_dig_hardness(BlockType::crate_copper) == 2 &&
            enemy_dig_hardness(BlockType::crate_iron) == 3 &&
            enemy_dig_hardness(BlockType::crate_gold) == 3 &&
            enemy_dig_hardness(BlockType::crate_diamond) == 4 &&
            enemy_dig_hardness(BlockType::crate_bloodstone) == 4 &&
            enemy_dig_hardness(BlockType::blood_stone) == 4 &&
            enemy_dig_hardness(BlockType::slime) == 4 &&
            enemy_dig_hardness(BlockType::bedrock) == 5,
        "enemy digging must compare against the original BlockType.Hardness table");

    WorldRegion region{0, 0, 1, 1};
    region.set_block(6, 1, 5, BlockType::tnt);
    EnemyHorde horde;
    Enemy digger;
    digger.type = 17; // DiggingMultiplier 1.0, max hardness 3.
    digger.state = EnemyState::dig;
    digger.position = {5.5F, 1.0F, 5.5F};
    digger.health = enemy_stats(digger.type).starting_health;
    horde.enemies().push_back(digger);
    PlayerState player;
    player.position = {7.5F, 1.0F, 5.5F};

    // First swing has hits == hardness, so EnemyBreakBlocks fails the strict
    // hits > hardness check and lights the explosive's normal fuse.
    const EnemyEvents fuse = horde.update(region, player, 0.5F);
    expect(fuse.dug_blocks.empty() && fuse.fuse_blocks.size() == 1U &&
               fuse.fuse_blocks.front() == std::array<int, 3>{6, 1, 5},
           "a failed zombie break roll against TNT/C4 must start its four-second fuse");

    region.set_block(6, 1, 5, BlockType::lower_door_closed_z);
    region.set_block(6, 2, 5, BlockType::upper_door_closed);
    Enemy &enemy = horde.enemies().front();
    enemy.state = EnemyState::dig;
    enemy.dig_valid = false;
    enemy.dig_progress = 0.0F;
    bool lower_reported{};
    bool upper_reported{};
    for (int swing = 0; swing < 32 && !lower_reported; ++swing)
    {
        // Keep this focused on the dig state; movement/collision has its own
        // regression and a half-second test tick would otherwise fall farther
        // than a normal fixed simulation step.
        enemy.state = EnemyState::dig;
        enemy.position = {5.5F, 1.0F, 5.5F};
        enemy.velocity = {};
        const EnemyEvents result = horde.update(region, player, 0.5F);
        for (const std::array<int, 3> &cell : result.dug_blocks)
        {
            lower_reported = lower_reported || cell == std::array<int, 3>{6, 1, 5};
            upper_reported = upper_reported || cell == std::array<int, 3>{6, 2, 5};
        }
    }
    expect(lower_reported && !upper_reported,
           "enemy digging must remove doors through the lower owning half only");
}

void attached_fixtures_and_door_halves_follow_support_removal()
{
    using namespace openminerz::world;
    WorldRegion region{0, 0, 1, 1};
    region.set_block(8, 2, 8, BlockType::rock);
    region.set_block(9, 2, 8, BlockType::torch_pos_x);
    region.set_block(8, 2, 7, BlockType::torch_neg_z);
    region.set_block(7, 2, 8, BlockType::torch_neg_x);
    region.set_block(8, 2, 9, BlockType::torch_pos_z);
    region.set_block(8, 1, 8, BlockType::torch_neg_y);
    region.set_block(8, 3, 8, BlockType::lower_door_closed_z);
    region.set_block(8, 4, 8, BlockType::upper_door_closed);

    const std::vector<BlockRemovalEffect> effects = region.removal_effects(8, 2, 8);
    const auto find = [&](int x, int y, int z) {
        return std::find_if(effects.begin(), effects.end(), [&](const auto &effect) {
            return effect.x == x && effect.y == y && effect.z == z;
        });
    };
    expect(effects.size() == 8U &&
               std::count_if(effects.begin(), effects.end(),
                             [](const auto &effect) { return effect.drops_as_dependent; }) == 6 &&
               find(9, 2, 8) != effects.end() && find(8, 2, 7) != effects.end() &&
               find(7, 2, 8) != effects.end() && find(8, 2, 9) != effects.end() &&
               find(8, 1, 8) != effects.end() && find(8, 3, 8) != effects.end() &&
               find(8, 4, 8) != effects.end() && !find(8, 4, 8)->drops_as_dependent,
           "removing a support must detach all facing fixtures, drop them once, and pair doors");
    expect(attached_face(BlockType::torch_pos_x) == FaceDirection::positive_x &&
               attached_face(BlockType::torch_neg_y) == FaceDirection::negative_y &&
               attached_face(BlockType::lower_door_open_z) == FaceDirection::positive_y &&
               !attached_face(BlockType::upper_door_open).has_value(),
           "fixture Facing values must retain the original six-face block contract");
    expect(can_build_on(BlockType::rock) && can_build_on(BlockType::leaves) &&
               !can_build_on(BlockType::torch_pos_x) &&
               !can_build_on(BlockType::lower_door_closed_x),
           "construction must reject torches and doors as support faces");
    expect(is_container(BlockType::crate) && is_container(BlockType::crate_stone) &&
               is_container(BlockType::crate_copper) && is_container(BlockType::crate_iron) &&
               is_container(BlockType::crate_gold) && is_container(BlockType::crate_diamond) &&
               is_container(BlockType::crate_bloodstone) && !is_container(BlockType::crate_safe) &&
               !is_container(BlockType::rock),
           "IsContainer must match the seven exact stock block IDs and exclude safe crate");

    const std::vector<BlockRemovalEffect> upper = region.removal_effects(8, 4, 8);
    expect(upper.size() == 2U && upper[0].block == BlockType::upper_door_closed &&
               std::any_of(upper.begin(), upper.end(),
                           [](const auto &effect) {
                               return effect.block == BlockType::lower_door_closed_z &&
                                      !effect.drops_as_dependent;
                           }),
           "mining either door half must remove its partner without a duplicate door drop");
}

void modern_door_models_pieces_and_fallbacks_match_stock()
{
    using openminerz::block_item;
    using openminerz::door_item_from_model;
    using openminerz::door_model_name;
    using openminerz::InventoryItemId;
    using openminerz::world::BlockType;
    using openminerz::world::door_model_transform;
    using openminerz::world::door_piece;
    using openminerz::world::is_strong_door;
    using openminerz::world::renders_as_voxel;

    expect(door_model_name(InventoryItemId::door) == 1U &&
               door_model_name(InventoryItemId::modern_iron_door) == 2U &&
               door_model_name(InventoryItemId::modern_diamond_door) == 3U &&
               door_model_name(InventoryItemId::modern_tech_door) == 4U &&
               !door_model_name(InventoryItemId::rock_block).has_value(),
           "DoorEntity model ordinals must match Wood, Iron, Diamond, and Tech registrations");
    expect(door_item_from_model(1U) == InventoryItemId::door &&
               door_item_from_model(2U) == InventoryItemId::modern_iron_door &&
               door_item_from_model(3U) == InventoryItemId::modern_diamond_door &&
               door_item_from_model(4U) == InventoryItemId::modern_tech_door &&
               !door_item_from_model(0U).has_value() && !door_item_from_model(5U).has_value(),
           "DoorClasses must invert every registered model and reject the None/out-of-range slots");

    expect(door_piece(BlockType::lower_door, BlockType::lower_door_closed_x) ==
                   BlockType::lower_door_closed_x &&
               door_piece(BlockType::strong_lower_door, BlockType::lower_door_closed_z) ==
                   BlockType::strong_lower_door_closed_z &&
               door_piece(BlockType::strong_lower_door, BlockType::lower_door_closed_x) ==
                   BlockType::strong_lower_door_closed_x &&
               door_piece(BlockType::strong_lower_door, BlockType::upper_door_closed) ==
                   BlockType::strong_upper_door_closed &&
               door_piece(BlockType::strong_lower_door, BlockType::lower_door_open_z) ==
                   BlockType::strong_lower_door_open_z &&
               door_piece(BlockType::strong_lower_door, BlockType::lower_door_open_x) ==
                   BlockType::strong_lower_door_open_x &&
               door_piece(BlockType::strong_lower_door, BlockType::upper_door_open) ==
                   BlockType::strong_upper_door_open &&
               door_piece(BlockType::rock, BlockType::upper_door_closed) == BlockType::empty,
           "DoorInventoryitem.GetDoorPiece must translate every authored strong-door state");
    expect(is_strong_door(BlockType::strong_lower_door_closed_z) &&
               is_strong_door(BlockType::strong_upper_door_open) &&
               !is_strong_door(BlockType::upper_door_open) &&
               block_item(BlockType::strong_lower_door_closed_z) ==
                   InventoryItemId::modern_tech_door,
           "strong-door family detection and missing-metadata Tech fallback must match stock");

    const auto closed_x = door_model_transform(BlockType::lower_door_closed_x);
    const auto open_x = door_model_transform(BlockType::strong_lower_door_open_x);
    const auto closed_z = door_model_transform(BlockType::strong_lower_door_closed_z);
    const auto open_z = door_model_transform(BlockType::lower_door_open_z);
    expect(closed_x.has_value() && near(closed_x->offset_x, -0.5F) &&
               near(closed_x->offset_y, -0.5F) && near(closed_x->offset_z, 0.0F) &&
               near(closed_x->yaw_radians, 0.0F) && open_x.has_value() &&
               near(open_x->offset_x, -0.5F) && near(open_x->offset_y, -0.5F) &&
               near(open_x->offset_z, 0.0F) && near(open_x->yaw_radians, 1.5707964F) &&
               closed_z.has_value() && near(closed_z->offset_x, 0.0F) &&
               near(closed_z->offset_y, -0.5F) && near(closed_z->offset_z, -0.5F) &&
               near(closed_z->yaw_radians, -1.5707964F) && open_z.has_value() &&
               near(open_z->offset_x, 0.0F) && near(open_z->offset_y, -0.5F) &&
               near(open_z->offset_z, -0.5F) && near(open_z->yaw_radians, 0.0F) &&
               !door_model_transform(BlockType::upper_door_closed).has_value(),
           "DoorEntity.SetPosition must retain all four exact lower-voxel transforms");
    expect(!renders_as_voxel(BlockType::torch_pos_x) &&
               !renders_as_voxel(BlockType::lower_door_closed_z) &&
               !renders_as_voxel(BlockType::strong_upper_door_open) &&
               renders_as_voxel(BlockType::glass_basic) && renders_as_voxel(BlockType::rock),
           "item-block models must replace torch/door cubes without suppressing translucent glass");
}

void world_save_round_trips_crates_doors_and_navigation_payloads()
{
    using namespace openminerz;
    using openminerz::world::BlockType;

    expect(near(item_self_damage_per_use(InventoryItemId::gps), 0.1F) &&
               near(item_self_damage_per_use(InventoryItemId::teleport_gps), 1.0F),
           "locator durability damage must match GPSItemClass");
    InventoryStack survival_locator{InventoryItemId::gps, 1U};
    for (int use = 0; use < 9; ++use)
        expect(!inflict_item_damage(survival_locator, false),
               "the survival locator must retain its first nine uses");
    expect(inflict_item_damage(survival_locator, false),
           "the survival locator must deplete on its tenth use");
    InventoryStack creative_locator{InventoryItemId::gps, 1U};
    for (int use = 0; use < 4; ++use)
        expect(!inflict_item_damage(creative_locator, true),
               "the original InfiniteResourceMode double-damage rule must retain four uses");
    expect(inflict_item_damage(creative_locator, true),
           "the original InfiniteResourceMode locator must deplete on use five");
    InventoryStack teleporter{InventoryItemId::teleport_gps, 1U};
    expect(inflict_item_damage(teleporter, false),
           "a teleporter must be consumed after its first successful teleport");

    expect(near(item_self_damage_per_use(InventoryItemId::stone_pickaxe), 0.0025F) &&
               near(item_self_damage_per_use(InventoryItemId::iron_axe), 0.0005F) &&
               near(item_self_damage_per_use(InventoryItemId::bloodstone_pickaxe), 0.0002F) &&
               near(item_self_damage_per_use(InventoryItemId::iron_laser_sword), 0.005F),
           "tool material durability must match Pick/Spade/Axe item classes");
    expect(near(item_self_damage_per_use(InventoryItemId::knife), 0.02F) &&
               near(item_self_damage_per_use(InventoryItemId::blood_stone_knife), 0.0033333334F),
           "knife tiers must retain their authored 50-to-300-use durability");
    expect(near(item_self_damage_per_use(InventoryItemId::pistol), 0.001F) &&
               near(item_self_damage_per_use(InventoryItemId::gold_lmg), 0.00045454546F) &&
               near(item_self_damage_per_use(InventoryItemId::diamond_assault_rifle),
                    0.00023923445F) &&
               near(item_self_damage_per_use(InventoryItemId::blood_stone_smg), 0.0001F) &&
               near(item_self_damage_per_use(InventoryItemId::copper_space_pistol), 0.0005F),
           "conventional and laser firearm wear must match the registration table");
    InventoryStack worn_pick{InventoryItemId::stone_pickaxe, 1U};
    for (int impact = 0; impact < 100; ++impact)
        expect(!inflict_item_damage(worn_pick, false),
               "a stone tool must survive its first hundred mining impacts");
    expect(near(worn_pick.durability, 0.75F, 0.0001F),
           "stone tool wear must subtract once per impact");

    WorldSaveData source;
    for (auto &slot : source.inventory.storage())
        slot.reset();
    for (auto &slot : source.inventory.tray())
        slot.reset();
    source.inventory.set_profile(InventoryProfile::modern);
    source.seed = -2140321;
    source.mode = GameMode::exploration;
    source.difficulty = GameDifficulty::no_enemies;
    source.infinite_resources = true;
    source.day = 3.875F;
    source.player_position = {12.25F, -31.125F, 4096.75F};
    source.player_yaw = -1.234567F;
    source.player_pitch = 0.345678F;
    source.player_health = 47.25F;
    source.has_player = true;
    source.distance_origin_x = -80.5F;
    source.distance_origin_z = 91.25F;
    source.has_distance_origin = true;
    source.dragon_distance_index = 3;
    source.max_distance = 2671;
    source.next_song_index = 4;

    InventoryStack locator{InventoryItemId::teleport_gps, 1U};
    locator.waypoint = {123.5F, -42.0F, 901.25F};
    locator.waypoint_name = "Deep Cave \"Alpha\"";
    locator.waypoint_valid = true;
    locator.durability = 0.4F;
    source.inventory.storage()[3] = locator;
    InventoryStack pistol{InventoryItemId::pistol, 1U};
    pistol.rounds_in_clip = 3U;
    source.inventory.tray()[2] = pistol;
    InventoryStack precision_laser{InventoryItemId::modern_precision_laser, 1U};
    precision_laser.rounds_in_clip = 7U;
    precision_laser.durability = 0.625F;
    source.inventory.tray(1)[5] = precision_laser;

    const WorldSaveCrateKey crate_location{17, -8, 22};
    source.crates[crate_location][0] = InventoryStack{InventoryItemId::bullets, 200U};
    source.crates[crate_location][7] = locator.with_count(1U);
    source.crates[crate_location][7]->waypoint_name = "Crate Route Beta";
    source.crates[crate_location][7]->durability = 0.7F;
    source.doors[{19, -7, 22}] = 3U;
    source.spawners[{-24, 5, 31}] = "spawner7";
    source.edits = {
        {17, -8, 22, static_cast<int>(BlockType::crate)},
        {19, -7, 22, static_cast<int>(BlockType::lower_door_closed_x)},
        {19, -6, 22, static_cast<int>(BlockType::upper_door_closed)},
    };

    std::stringstream encoded;
    expect(write_world_save(encoded, source), "a complete offline world must serialize");
    WorldSaveData restored;
    expect(read_world_save(encoded, restored), "a serialized offline world must deserialize");
    expect(restored.version == 6 && restored.owner == source.owner &&
               restored.saved_unix_seconds == source.saved_unix_seconds &&
               restored.seed == source.seed && restored.mode == GameMode::exploration &&
               restored.difficulty == GameDifficulty::no_enemies && restored.infinite_resources &&
               near(restored.day, source.day) && restored.has_player &&
               near(restored.player_position.x, source.player_position.x) &&
               near(restored.player_position.y, source.player_position.y) &&
               near(restored.player_position.z, source.player_position.z) &&
               restored.has_distance_origin && restored.dragon_distance_index == 3 &&
               restored.max_distance == 2671 && restored.next_song_index == 4,
           "world/session/player progression must round-trip without field loss");
    expect(restored.inventory.storage()[3].has_value() &&
               restored.inventory.storage()[3]->waypoint_valid &&
               restored.inventory.storage()[3]->waypoint_name == "Deep Cave \"Alpha\"" &&
               restored.inventory.storage()[3]->waypoint == locator.waypoint &&
               near(restored.inventory.storage()[3]->durability, 0.4F) &&
               restored.inventory.tray()[2].has_value() &&
               restored.inventory.tray()[2]->rounds_in_clip == 3U &&
               restored.inventory.profile() == InventoryProfile::modern &&
               restored.inventory.tray(1)[5].has_value() &&
               restored.inventory.tray(1)[5]->item == InventoryItemId::modern_precision_laser &&
               restored.inventory.tray(1)[5]->rounds_in_clip == 7U &&
               near(restored.inventory.tray(1)[5]->durability, 0.625F),
           "both Modern trays, gun state, and quoted navigation payloads must round-trip");
    const auto crate = restored.crates.find(crate_location);
    expect(crate != restored.crates.end() && crate->second[0].has_value() &&
               crate->second[0]->item == InventoryItemId::bullets &&
               crate->second[0]->count == 200U && crate->second[7].has_value() &&
               crate->second[7]->waypoint_valid &&
               crate->second[7]->waypoint_name == "Crate Route Beta" &&
               near(crate->second[7]->durability, 0.7F),
           "non-empty crate slots and their item payloads must survive save/reload");
    expect(restored.edits == source.edits,
           "paired lower/upper door edits must survive save/reload together");
    expect(restored.doors == source.doors && restored.spawners == source.spawners,
           "Modern custom-door identities and spawner IDs must survive save/reload");

    std::stringstream legacy_v4;
    legacy_v4 << "omw 4\ninv 32 51 1 3\ninv 40 51 1 9\nprofile 1\n";
    WorldSaveData legacy_restored;
    expect(read_world_save(legacy_v4, legacy_restored) &&
               legacy_restored.inventory.profile() == InventoryProfile::classic &&
               legacy_restored.inventory.tray(0)[0].has_value() &&
               legacy_restored.inventory.tray(0)[0]->rounds_in_clip == 3U &&
               !legacy_restored.inventory.tray(1)[0].has_value(),
           "v4 saves must remain one-tray Classic files and reject new tray indices");
}

void rocket_explosion_matches_original_damage_and_protection()
{
    using namespace openminerz::world;
    expect(near(explosive_damage_fraction(0.0F, rocket_damage_bands), 1.0F) &&
               near(explosive_damage_fraction(2.999F, rocket_damage_bands), 1.0F) &&
               near(explosive_damage_fraction(4.5F, rocket_damage_bands), 0.5F) &&
               near(explosive_damage_fraction(6.0F, rocket_damage_bands), 0.0F),
           "rocket splash must use the original 3-unit kill and 6-unit damage bands");
    expect(rocket_damage_bands.enemy_range == 12.0F,
           "rocket enemy splash must retain the original 12-unit range");
    expect(rocket_breaks_block(BlockType::rock) && rocket_breaks_block(BlockType::diamond_wall) &&
               rocket_breaks_block(BlockType::tnt),
           "rockets must break ordinary, hardened, and chain-explosive blocks");
    expect(
        !rocket_breaks_block(BlockType::empty) && !rocket_breaks_block(BlockType::fixed_lantern) &&
            !rocket_breaks_block(BlockType::bedrock) &&
            !rocket_breaks_block(BlockType::space_rock) && !rocket_breaks_block(BlockType::slime),
        "rockets must preserve the original common explosive protection set");
    expect(explosive_definition(ExplosiveKind::tnt).destruction_range == 2 &&
               explosive_definition(ExplosiveKind::c4).destruction_range == 3 &&
               explosive_definition(ExplosiveKind::he_grenade).destruction_range == 1,
           "TNT, C4 and HE grenades must retain their original block scan radii");
    expect(tnt_damage_bands.kill_range == 3.0F && tnt_damage_bands.damage_range == 6.0F &&
               tnt_damage_bands.enemy_range == 12.0F && c4_damage_bands.kill_range == 6.0F &&
               c4_damage_bands.damage_range == 12.0F && c4_damage_bands.enemy_range == 24.0F &&
               he_grenade_damage_bands.kill_range == 2.5F &&
               he_grenade_damage_bands.damage_range == 5.0F &&
               he_grenade_damage_bands.enemy_range == 8.0F,
           "placed explosives and HE grenades must use the original splash tables");
    expect(
        explosive_breaks_block(ExplosiveKind::tnt, BlockType::dirt, 1, 1, 1) &&
            !explosive_breaks_block(ExplosiveKind::tnt, BlockType::dirt, 2, 0, 0) &&
            explosive_breaks_block(ExplosiveKind::c4, BlockType::dirt, 2, 0, 0) &&
            !explosive_breaks_block(ExplosiveKind::c4, BlockType::rock, 2, 0, 0),
        "TNT must break one level of soft material while C4 reaches soft material two cells out");
    expect(!explosive_breaks_block(ExplosiveKind::tnt, BlockType::diamond_ore, 0, 0, 0) &&
               explosive_breaks_block(ExplosiveKind::c4, BlockType::diamond_ore, 0, 0, 0) &&
               !explosive_breaks_block(ExplosiveKind::he_grenade, BlockType::diamond_wall, 0, 0, 0),
           "TNT and HE grenades must protect valuables while C4 may destroy them");
    expect(explosive_block_hardness(BlockType::sand) == 1U &&
               explosive_block_hardness(BlockType::wood) == 2U &&
               explosive_block_hardness(BlockType::crate_stone) == 2U &&
               explosive_block_hardness(BlockType::crate_copper) == 2U &&
               explosive_block_hardness(BlockType::rock) == 3U &&
               explosive_block_hardness(BlockType::crate_iron) == 3U &&
               explosive_block_hardness(BlockType::crate_gold) == 3U &&
               explosive_block_hardness(BlockType::blood_stone) == 4U &&
               explosive_block_hardness(BlockType::crate_diamond) == 4U &&
               explosive_block_hardness(BlockType::crate_bloodstone) == 4U &&
               explosive_block_hardness(BlockType::bedrock) == 5U &&
               explosive_block_hardness(BlockType::crate_safe) == 5U,
           "explosion hardness tiers must match BlockType.blockTypes");
    expect(near(grenade_seconds_left(0.0F), 5.0F) && near(grenade_seconds_left(2.5F), 2.5F) &&
               near(grenade_seconds_left(4.0F), 1.0F) && near(grenade_seconds_left(9.0F), 1.0F),
           "grenade cooking must preserve a one-second minimum thrown fuse");
    expect(grenade_collision_budget(0U) == 10 && grenade_collision_budget(3U) == 1 &&
               grenade_detonation_type(0U) == 0U && grenade_detonation_type(1U) == 1U &&
               grenade_detonation_type(2U) == 2U && grenade_detonation_type(3U) == 0U &&
               !grenade_remains_stuck(0U) && grenade_remains_stuck(3U),
           "sticky grenades must stop on first contact but detonate through the stock HE wire bug");
    expect(near(grenade_bounce_restitution(BlockType::sand), 0.1F) &&
               near(grenade_bounce_restitution(BlockType::tnt), 0.4F) &&
               near(grenade_bounce_restitution(BlockType::rock), 0.6F),
           "grenade restitution must follow the original block material table");
    WorldRegion grenade_world{0, 0, 1, 1};
    grenade_world.set_block(4, 2, 6, BlockType::rock);
    const auto centre_hit =
        grenade_terrain_sweep(grenade_world, {4.5F, 2.5F, 2.0F}, {4.5F, 2.5F, 9.0F});
    expect(centre_hit.has_value() && near(centre_hit->position.z, 5.9F) &&
               near(centre_hit->normal.z, -1.0F) && centre_hit->block == BlockType::rock,
           "grenades must sweep their original 0.1 half-extent and report the entry face");
    const auto edge_hit =
        grenade_terrain_sweep(grenade_world, {3.95F, 2.5F, 2.0F}, {3.95F, 2.5F, 9.0F});
    expect(edge_hit.has_value() && near(edge_hit->position.z, 5.9F),
           "the grenade AABB must catch a wall edge that a point ray would miss");
    const Vec3 wall_reflection =
        grenade_reflect_vector({10.0F, -2.0F, 3.0F}, {-1.0F, 0.0F, 0.0F}, 0.6F);
    const Vec3 floor_reflection =
        grenade_reflect_vector({3.0F, -2.0F, 4.0F}, {0.0F, 1.0F, 0.0F}, 0.4F);
    expect(near(wall_reflection.x, -6.0F) && near(wall_reflection.y, -2.0F) &&
               near(wall_reflection.z, 3.0F) && near(floor_reflection.x, 1.2F) &&
               near(floor_reflection.y, 0.8F) && near(floor_reflection.z, 1.6F),
           "grenade wall/floor restitution must preserve the original tangent scaling");
}

void world_start_matches_original_respawn_contract()
{
    using namespace openminerz::world;
    expect(near(default_start_location.x, 8.0F) && near(default_start_location.y, 128.0F) &&
               near(default_start_location.z, -8.0F),
           "WorldInfo.DefaultStartLocation must remain exactly (8, 128, -8)");

    WorldGenerator generator{1337};
    const WorldRegion region = generator.generate_region(-2, -2, 4, 4);
    PlayerSimulation player;
    player.spawn(region, static_cast<int>(default_start_location.x),
                 static_cast<int>(default_start_location.z));
    expect(
        std::abs(std::floor(player.state().position.x) - default_start_location.x) <= 10.0F &&
            std::abs(std::floor(player.state().position.z) - default_start_location.z) <= 10.0F,
        "surface placement must search around the original start rather than the streamed centre");
    expect(near(player.state().health, 100.0F),
           "RefreshPlayer must restore full health at respawn");
}

void legacy_steam_transport_codecs_match_deployed_wire_and_fail_closed()
{
    using namespace openminerz::net;

    LegacySteamCodecError error{};
    const SessionProperties modern_properties{4, 0, 2, 1, 0, 0, std::nullopt, std::nullopt};
    const SessionProperties classic_properties{3, 0, 2, 1, 0, 0, std::nullopt, std::nullopt};
    const LegacyConnectionRequest request{
        std::string{legacy_steam_game_name},
        4,
        123456789,
        std::string{"pw"},
        LegacyGamerIdentity{{1, 2, 3, 4}, "Alice"},
        modern_properties,
    };
    const auto approval = encode_legacy_connection_request(request, error);
    const auto approval_golden =
        bytes_from_hex("11436173746C654D696E65725A537465616D0400000015CD5B0701000000027077"
                       "040000000102030405416C6963650800000009000000020000001400000018000000"
                       "100000002000000000");
    expect(approval.has_value() && *approval == approval_golden &&
               error == LegacySteamCodecError::none,
           "the Modern approval must match the deployed NetBuffer bit layout byte for byte");
    const auto approval_wire = encode_legacy_steam_wire(
        SteamIncomingMessageType::connection_approval, 0, *approval, error);
    expect(approval_wire.has_value() &&
               *approval_wire ==
                   bytes_from_hex("0400000011436173746C654D696E65725A537465616D04000000"
                                  "15CD5B0701000000027077040000000102030405416C69636508"
                                  "0000000900000002000000140000001800000010000000200000"
                                  "0000"),
           "the stock connection approval must retain its four-byte Steam provider envelope");

    LegacyConnectionRequest decoded_request;
    expect(decode_legacy_connection_request(*approval, decoded_request, error) &&
               decoded_request.game_name == legacy_steam_game_name &&
               decoded_request.network_version == 4 && decoded_request.session_id == 123456789 &&
               decoded_request.password == std::optional<std::string>{"pw"} &&
               decoded_request.gamer == request.gamer &&
               decoded_request.session_properties == modern_properties,
           "the exact approval must round-trip every admission field");
    LegacyConnectionRequest classic_request = request;
    classic_request.network_version = 3;
    classic_request.session_properties = classic_properties;
    const auto classic_approval = encode_legacy_connection_request(classic_request, error);
    expect(classic_approval.has_value() &&
               *classic_approval ==
                   bytes_from_hex("11436173746C654D696E65725A537465616D0300000015CD5B07"
                                  "01000000027077040000000102030405416C6963650800000007"
                                  "000000020000001400000018000000100000002000000000"),
           "Classic approval must differ only at the source-confirmed protocol/property bits");

    const LegacyConnectedResponse connected{
        7,
        {LegacyGamerIdentity{{9, 9}, "Host"}, LegacyGamerIdentity{{8}, "Bob"}},
        {0, 3},
    };
    const auto connected_payload = encode_legacy_connected_response(connected, error);
    expect(connected_payload.has_value() &&
               *connected_payload ==
                   bytes_from_hex("01070200000002000000090904486F7374010000000803426F62"
                                  "020000000003"),
           "the response-to-connection peer array must match the deployed bytes");
    LegacyConnectedResponse decoded_connected;
    expect(decode_legacy_connected_response(*connected_payload, decoded_connected, error) &&
               decoded_connected.player_gid == 7 && decoded_connected.peers == connected.peers &&
               decoded_connected.peer_gids == connected.peer_gids,
           "the response-to-connection decoder must preserve peer identities and GIDs");

    const auto new_peer = encode_legacy_new_peer(5, LegacyGamerIdentity{{1, 2}, "New"}, error);
    expect(new_peer.has_value() &&
               *new_peer == std::vector<std::uint8_t>({0, 5, 2, 0, 0, 0, 1, 2, 3, 'N', 'e', 'w'}),
           "new-peer must retain internal marker zero and the Lidgren gamer layout");
    std::uint8_t new_gid{};
    LegacyGamerIdentity new_gamer;
    expect(decode_legacy_new_peer(*new_peer, new_gid, new_gamer, error) && new_gid == 5 &&
               new_gamer == LegacyGamerIdentity{{1, 2}, "New"},
           "new-peer must decode exactly without trailing bytes");
    const auto dropped = encode_legacy_drop_peer(5);
    std::uint8_t dropped_gid{};
    expect(dropped == std::vector<std::uint8_t>({2, 5}) &&
               decode_legacy_drop_peer(dropped, dropped_gid, error) && dropped_gid == 5,
           "drop-peer must remain the confirmed two-byte internal message");
    const auto changed = encode_legacy_session_properties_changed(modern_properties, error);
    expect(changed.has_value() &&
               *changed ==
                   bytes_from_hex("050800000009000000020000001400000018000000100000002000000000"),
           "Modern session-property mutation must retain the source bit packing");
    SessionProperties decoded_properties{};
    expect(decode_legacy_session_properties_changed(*changed, decoded_properties, error) &&
               decoded_properties == modern_properties,
           "session-property mutation must round-trip all eight nullable slots");

    const std::vector<std::uint8_t> application{0x35, 0x01, 0x41, 0x75};
    const LegacyDirectDataFrame direct{0, 7, application};
    const auto encoded_direct = encode_legacy_direct_frame(direct, error);
    expect(encoded_direct.has_value() &&
               *encoded_direct ==
                   std::vector<std::uint8_t>({0, 7, 4, 0, 0, 0, 0x35, 0x01, 0x41, 0x75}),
           "channel-zero direct data must use recipient/sender/int32-array layout");
    LegacyDirectDataFrame decoded_direct;
    expect(decode_legacy_direct_frame(*encoded_direct, decoded_direct, error) &&
               decoded_direct.recipient_gid == 0 && decoded_direct.sender_gid == 7 &&
               decoded_direct.application_packet == application,
           "channel-zero direct data must decode without route ambiguity");

    const auto broadcast = encode_legacy_client_broadcast(7, SteamDeliveryMethod::reliable_ordered,
                                                          application, error);
    expect(broadcast.has_value() &&
               *broadcast ==
                   std::vector<std::uint8_t>({4, 67, 7, 4, 0, 0, 0, 0x35, 0x01, 0x41, 0x75}),
           "client broadcast must retain marker/delivery/sender/int32-array layout");
    LegacyClientRouteFrame decoded_route;
    expect(decode_legacy_client_route(*broadcast, decoded_route, error) &&
               decoded_route.kind == LegacyClientRouteKind::broadcast &&
               !decoded_route.recipient_gid.has_value() && decoded_route.sender_gid == 7 &&
               decoded_route.delivery == SteamDeliveryMethod::reliable_ordered &&
               decoded_route.application_packet == application,
           "broadcast route decoding must preserve sender and delivery");
    const auto proxy = encode_legacy_client_proxy_direct(
        9, 7, SteamDeliveryMethod::reliable_unordered, application, error);
    expect(proxy.has_value() &&
               *proxy ==
                   std::vector<std::uint8_t>({3, 9, 34, 7, 4, 0, 0, 0, 0x35, 0x01, 0x41, 0x75}) &&
               decode_legacy_client_route(*proxy, decoded_route, error) &&
               decoded_route.kind == LegacyClientRouteKind::proxy_direct &&
               decoded_route.recipient_gid ==
                   std::optional<std::uint8_t>{static_cast<std::uint8_t>(9)} &&
               decoded_route.sender_gid == 7,
           "proxy-direct must retain its explicit target and reliable-unordered contract");

    LegacySteamWirePacket wire_packet;
    const auto reserved_wire = bytes_from_hex("080000CC01");
    const auto unknown_wire = bytes_from_hex("03000000");
    expect(decode_legacy_steam_wire(reserved_wire, wire_packet, error) &&
               wire_packet.payload == std::vector<std::uint8_t>({1}),
           "the original provider's reserved fourth envelope byte must be ignored");
    expect(!decode_legacy_steam_wire(unknown_wire, wire_packet, error) &&
               error == LegacySteamCodecError::invalid_message_type,
           "an undefined incoming message type must fail closed");
    expect(!decode_legacy_connection_request(
               std::span<const std::uint8_t>{approval->data(), approval->size() - 1U},
               decoded_request, error),
           "a truncated approval must never create a partial peer identity");
    const std::vector<std::uint8_t> bad_length{4, 67, 7, 5, 0, 0, 0, 0x35, 0x01, 0x41, 0x75};
    const std::vector<std::uint8_t> bad_delivery{4, 99, 7, 4, 0, 0, 0, 0x35, 0x01, 0x41, 0x75};
    expect(!decode_legacy_client_route(bad_length, decoded_route, error) &&
               error == LegacySteamCodecError::length_mismatch &&
               !decode_legacy_client_route(bad_delivery, decoded_route, error) &&
               error == LegacySteamCodecError::invalid_delivery,
           "route length and delivery bytes must be checked before application parsing");
    LegacyConnectionRequest oversized = request;
    oversized.gamer.player_id.resize(1025U);
    expect(!encode_legacy_connection_request(oversized, error).has_value() &&
               error == LegacySteamCodecError::out_of_range,
           "attacker-controlled gamer identity blobs must remain bounded before copying");
    LegacyConnectedResponse mismatched = connected;
    mismatched.peer_gids.pop_back();
    expect(!encode_legacy_connected_response(mismatched, error).has_value() &&
               error == LegacySteamCodecError::out_of_range,
           "mismatched peer/GID arrays must never reach a stock client");
    expect(uses_unreliable_native_send(SteamDeliveryMethod::unknown) &&
               uses_unreliable_native_send(SteamDeliveryMethod::unreliable) &&
               uses_unreliable_native_send(SteamDeliveryMethod::unreliable_sequenced) &&
               !uses_unreliable_native_send(SteamDeliveryMethod::reliable_unordered) &&
               !uses_unreliable_native_send(SteamDeliveryMethod::reliable_sequenced) &&
               !uses_unreliable_native_send(SteamDeliveryMethod::reliable_ordered),
           "Steam native send selection must match the source delivery split");
}

void legacy_steam_host_session_admits_routes_and_relays_only_valid_stock_traffic()
{
    using namespace openminerz::net;

    const auto make_options = [] {
        LegacySteamHostSessionOptions options;
        options.settings.server_name = "OpenMinerZ Interop";
        options.settings.server_message = "Stock transport test";
        options.settings.protocol = ProtocolVersion::modern_v4;
        options.settings.maximum_players = 16;
        options.settings.visibility = HostVisibility::public_lobby;
        options.settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
        options.game_mode = 1;
        options.difficulty = 0;
        options.host_gamer = LegacyGamerIdentity{{9, 9}, "Dedicated"};
        return options;
    };
    const auto properties = [] {
        HostSessionSettings settings;
        settings.protocol = ProtocolVersion::modern_v4;
        settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
        return make_session_properties(settings, false, 1, 0, false);
    }();
    const auto submit_approval = [&](LegacySteamHostSession &target, PeerId peer,
                                     std::string gamer_tag, std::int32_t version = 4,
                                     std::optional<SessionProperties> requested_properties =
                                         std::nullopt,
                                     std::optional<std::string> password = std::nullopt) {
        LegacyConnectionRequest request;
        request.network_version = version;
        request.session_id = 0;
        request.password = std::move(password);
        request.gamer = LegacyGamerIdentity{{1, 2, 3}, std::move(gamer_tag)};
        request.session_properties = requested_properties.value_or(properties);
        LegacySteamCodecError error{};
        const auto payload = encode_legacy_connection_request(request, error);
        expect(payload.has_value(), "test approval must encode");
        const auto wire = encode_legacy_steam_wire(SteamIncomingMessageType::connection_approval, 0,
                                                   *payload, error);
        expect(wire.has_value(), "test approval envelope must encode");
        return target.process_wire_packet(peer, *wire);
    };

    constexpr PeerId alice = 101U;
    constexpr PeerId bob = 202U;
    LegacySteamHostSession session{make_options()};
    const LegacySteamHostSessionResult alice_approval = submit_approval(session, alice, "Alice");
    expect(!alice_approval.rejection_reason.has_value() && alice_approval.sends.size() == 2U &&
               alice_approval.inbound.size() == 1U &&
               alice_approval.inbound[0].kind == LegacySteamInboundKind::peer_connected &&
               alice_approval.inbound[0].peer == alice &&
               alice_approval.inbound[0].sender_gid == 1U && session.connected(alice),
           "the first stock peer must receive status/roster only after complete approval");
    LegacySteamCodecError codec_error{};
    LegacySteamWirePacket status_wire;
    expect(decode_legacy_steam_wire(alice_approval.sends[0].payload, status_wire, codec_error) &&
               status_wire.message_type == SteamIncomingMessageType::status_changed &&
               status_wire.channel == 0U &&
               status_wire.payload == std::vector<std::uint8_t>({5, 0, 0, 0}) &&
               alice_approval.sends[0].delivery == SteamDeliveryMethod::reliable_ordered &&
               alice_approval.sends[0].p2p_channel == 0U,
           "stock approval must begin with the original connected status frame");
    LegacySteamWirePacket roster_wire;
    LegacyConnectedResponse alice_roster;
    expect(decode_legacy_steam_wire(alice_approval.sends[1].payload, roster_wire, codec_error) &&
               roster_wire.message_type == SteamIncomingMessageType::data &&
               roster_wire.channel == 1U &&
               decode_legacy_connected_response(roster_wire.payload, alice_roster, codec_error) &&
               alice_roster.player_gid == 1U &&
               alice_roster.peer_gids == std::vector<std::uint8_t>({0}) &&
               alice_roster.peers.size() == 1U && alice_roster.peers[0].gamer_tag == "Dedicated",
           "the first response roster must contain only the GID-zero host");

    const LegacySteamHostSessionResult bob_approval = submit_approval(session, bob, "Bob");
    expect(!bob_approval.rejection_reason.has_value() && bob_approval.sends.size() == 3U &&
               bob_approval.inbound[0].sender_gid == 2U && session.connected_peer_count() == 2U &&
               session.peer_for_gid(1U) == alice && session.peer_for_gid(2U) == bob &&
               !session.peer_for_gid(0U).has_value(),
           "a second peer must receive status/roster and announce exactly one new-peer frame");
    LegacySteamWirePacket bob_roster_wire;
    LegacyConnectedResponse bob_roster;
    expect(decode_legacy_steam_wire(bob_approval.sends[1].payload, bob_roster_wire, codec_error) &&
               decode_legacy_connected_response(bob_roster_wire.payload, bob_roster, codec_error) &&
               bob_roster.player_gid == 2U &&
               bob_roster.peer_gids == std::vector<std::uint8_t>({0, 1}) &&
               bob_roster.peers[0].gamer_tag == "Dedicated" &&
               bob_roster.peers[1].gamer_tag == "Alice" &&
               bob_approval.sends[2].remote_peer == alice,
           "the second roster must preserve host-first insertion order and existing GIDs");
    LegacySteamWirePacket new_peer_wire;
    std::uint8_t announced_gid{};
    LegacyGamerIdentity announced_gamer;
    expect(decode_legacy_steam_wire(bob_approval.sends[2].payload, new_peer_wire, codec_error) &&
               decode_legacy_new_peer(new_peer_wire.payload, announced_gid, announced_gamer,
                                      codec_error) &&
               announced_gid == 2U && announced_gamer.gamer_tag == "Bob",
           "existing peers must see the newly approved identity before gameplay");

    const LegacySteamHostSessionResult duplicate_name = submit_approval(session, 303U, "Alice");
    expect(
        duplicate_name.rejection_reason == std::optional<std::string>{"GamerAlreadyConnected"} &&
            duplicate_name.sends.size() == 1U && duplicate_name.sends[0].p2p_channel == 1U &&
            duplicate_name.sends[0].payload.front() == 0U &&
            duplicate_name.sends[0].payload.back() == 0U && !session.connected(303U),
        "duplicate gamer tags must receive the stock null-delimited denial on native channel one");
    const LegacySteamHostSessionResult old_version = submit_approval(session, 404U, "Old", 3);
    expect(old_version.rejection_reason == std::optional<std::string>{"ServerHasNewerVersion"} &&
               !session.connected(404U),
           "a Classic peer must receive the deployed newer-server reason from a Modern host");

    PayloadError payload_error{};
    PlayerExistsPayload presence;
    presence.request_response = true;
    presence.avatar_description = {1U, 2U};
    presence.gamer_tag = "Alice";
    presence.player_id = {3U, 4U};
    const auto presence_payload = encode_player_exists_payload(presence, payload_error);
    const MessageDescriptor *presence_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "DNA.CastleMinerZ.Net.PlayerExistsMessage");
    PacketError packet_error{};
    const auto presence_packet = encode_packet(ProtocolVersion::modern_v4, presence_descriptor->id,
                                               *presence_payload, packet_error);
    const auto presence_route = encode_legacy_client_broadcast(
        1U, SteamDeliveryMethod::reliable_ordered, *presence_packet, codec_error);
    const auto presence_wire =
        encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *presence_route, codec_error);
    const LegacySteamHostSessionResult received_presence =
        session.process_wire_packet(alice, *presence_wire);
    expect(received_presence.inbound.size() == 1U,
           "an admitted typed PlayerExists request must reach the authority boundary");
    LegacySteamOutbound presence_echo;
    presence_echo.application_packet = received_presence.inbound[0].payload;
    presence_echo.delivery = received_presence.inbound[0].delivery;
    presence_echo.channel = received_presence.inbound[0].channel;
    presence_echo.sender_gid = received_presence.inbound[0].sender_gid;
    presence_echo.recipient_gid = received_presence.inbound[0].sender_gid;
    presence_echo.client_relay = true;
    std::string outbound_error;
    const auto echoed_presence = session.create_outbound(presence_echo, outbound_error);
    expect(echoed_presence.has_value() && echoed_presence->size() == 1U &&
               echoed_presence->front().remote_peer == alice,
           "an explicitly targeted echo-designated presence packet must return to its source GID");

    const auto chat_payload = encode_chat_payload("hello", payload_error);
    const MessageDescriptor *chat =
        message_by_name(ProtocolVersion::modern_v4, "DNA.Net.ChatMessage");
    expect(chat_payload.has_value() && chat != nullptr, "chat test fixture must resolve");
    const auto chat_packet =
        encode_packet(ProtocolVersion::modern_v4, chat->id, *chat_payload, packet_error);
    expect(chat_packet.has_value(), "chat test packet must encode");
    const auto chat_route =
        encode_legacy_client_broadcast(1U, SteamDeliveryMethod::unknown, *chat_packet, codec_error);
    const auto chat_wire =
        chat_route.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *chat_route, codec_error)
            : std::nullopt;
    expect(chat_wire.has_value(), "chat route must encode");
    const LegacySteamHostSessionResult received_chat =
        session.process_wire_packet(alice, *chat_wire);
    expect(received_chat.sends.empty() && received_chat.inbound.size() == 1U &&
               received_chat.inbound[0].kind == LegacySteamInboundKind::gameplay &&
               received_chat.inbound[0].peer == alice &&
               received_chat.inbound[0].sender_gid == 1U &&
               received_chat.inbound[0].recipient_gid == 0U &&
               received_chat.inbound[0].delivery == MessageDeliveryPolicy::chat &&
               received_chat.inbound[0].payload == *chat_packet,
           "valid client broadcast must stop at the authority boundary rather than auto-relay");

    LegacySteamOutbound accepted_relay;
    accepted_relay.application_packet = received_chat.inbound[0].payload;
    accepted_relay.delivery = received_chat.inbound[0].delivery;
    accepted_relay.channel = received_chat.inbound[0].channel;
    accepted_relay.sender_gid = received_chat.inbound[0].sender_gid;
    accepted_relay.recipient_gid = received_chat.inbound[0].recipient_gid;
    accepted_relay.client_relay = true;
    const auto relay = session.create_outbound(accepted_relay, outbound_error);
    expect(relay.has_value() && relay->size() == 1U && (*relay)[0].remote_peer == bob &&
               (*relay)[0].delivery == SteamDeliveryMethod::unknown &&
               uses_unreliable_native_send((*relay)[0].delivery),
           "only an authority-accepted chat may relay to other peers using native unreliable send");
    LegacySteamWirePacket relayed_wire;
    LegacyDirectDataFrame relayed_direct;
    expect(decode_legacy_steam_wire((*relay)[0].payload, relayed_wire, codec_error) &&
               relayed_wire.channel == 0U &&
               decode_legacy_direct_frame(relayed_wire.payload, relayed_direct, codec_error) &&
               relayed_direct.recipient_gid == 2U && relayed_direct.sender_gid == 1U &&
               relayed_direct.application_packet == *chat_packet,
           "accepted broadcasts must become recipient-specific channel-zero direct frames");

    const auto wrong_delivery_route = encode_legacy_client_broadcast(
        1U, SteamDeliveryMethod::reliable_ordered, *chat_packet, codec_error);
    const auto wrong_delivery_wire = encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U,
                                                              *wrong_delivery_route, codec_error);
    const LegacySteamHostSessionResult wrong_delivery =
        session.process_wire_packet(alice, *wrong_delivery_wire);
    expect(wrong_delivery.inbound.empty() && wrong_delivery.sends.empty() &&
               wrong_delivery.audit.has_value() &&
               wrong_delivery.audit->stage == LegacySteamRejectionStage::application &&
               wrong_delivery.audit->reason == "InvalidApplicationPacketOrDelivery",
           "checksum-valid traffic with the wrong delivery class must fail before authority");
    const auto spoofed_route =
        encode_legacy_client_broadcast(2U, SteamDeliveryMethod::unknown, *chat_packet, codec_error);
    const auto spoofed_wire =
        encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *spoofed_route, codec_error);
    const LegacySteamHostSessionResult spoofed = session.process_wire_packet(alice, *spoofed_wire);
    expect(spoofed.inbound.empty() && spoofed.audit.has_value() &&
               spoofed.audit->stage == LegacySteamRejectionStage::routing &&
               spoofed.audit->reason == "InvalidClientRoute",
           "a lobby member may never claim another peer's session GID");

    const MessageDescriptor *world_request =
        message_by_name(ProtocolVersion::modern_v4, "DNA.CastleMinerZ.Net.RequestWorldInfoMessage");
    const auto world_packet =
        encode_packet(ProtocolVersion::modern_v4, world_request->id, {}, packet_error);
    LegacySteamOutbound server_broadcast;
    server_broadcast.application_packet = *world_packet;
    server_broadcast.delivery = MessageDeliveryPolicy::reliable_ordered;
    const auto server_sends = session.create_outbound(server_broadcast, outbound_error);
    expect(server_sends.has_value() && server_sends->size() == 2U,
           "a valid server broadcast must produce one bounded direct frame per admitted peer");
    for (const LegacySteamNativeSend &send : *server_sends)
    {
        LegacySteamWirePacket server_wire;
        LegacyDirectDataFrame server_direct;
        expect(decode_legacy_steam_wire(send.payload, server_wire, codec_error) &&
                   decode_legacy_direct_frame(server_wire.payload, server_direct, codec_error) &&
                   server_direct.sender_gid == 0U &&
                   server_direct.application_packet == *world_packet,
               "server-originated frames must use GID zero and preserve the validated packet");
    }

    const auto drops = session.remove_peer(alice);
    expect(drops.size() == 1U && drops[0].remote_peer == bob && !session.connected(alice) &&
               session.connected_peer_count() == 1U,
           "disconnect must remove the peer before broadcasting one drop frame");
    LegacySteamWirePacket drop_wire;
    std::uint8_t drop_gid{};
    expect(decode_legacy_steam_wire(drops[0].payload, drop_wire, codec_error) &&
               drop_wire.channel == 1U &&
               decode_legacy_drop_peer(drop_wire.payload, drop_gid, codec_error) && drop_gid == 1U,
           "remaining peers must receive the exact dropped GID");
    const LegacySteamHostSessionResult reconnect = submit_approval(session, alice, "Alice");
    LegacySteamWirePacket reconnect_wire;
    LegacyConnectedResponse reconnect_roster;
    expect(!reconnect.rejection_reason.has_value() && reconnect.inbound[0].sender_gid == 3U &&
               decode_legacy_steam_wire(reconnect.sends[1].payload, reconnect_wire, codec_error) &&
               decode_legacy_connected_response(reconnect_wire.payload, reconnect_roster,
                                                codec_error) &&
               reconnect_roster.player_gid == 3U &&
               reconnect_roster.peer_gids == std::vector<std::uint8_t>({0, 2}),
           "reconnect must allocate a fresh GID and never resurrect the removed roster entry");

    LegacySteamHostSessionOptions password_options = make_options();
    password_options.settings.password = "pw";
    LegacySteamHostSession password_session{password_options};
    const auto missing_password = submit_approval(password_session, 505U, "NoPassword");
    const auto correct_password =
        submit_approval(password_session, 506U, "Correct", 4, properties, std::string{"pw"});
    expect(missing_password.rejection_reason == std::optional<std::string>{"IncorrectPassword"} &&
               !correct_password.rejection_reason.has_value(),
           std::string{"stock password admission failed; missing="} +
               missing_password.rejection_reason.value_or("accepted") +
               " correct=" + correct_password.rejection_reason.value_or("accepted"));
    LegacySteamHostSessionOptions restricted_options = make_options();
    restricted_options.settings.join_policy = HostJoinPolicy::allow_list;
    restricted_options.platform_authorized = [](PeerId peer) { return peer != 606U; };
    LegacySteamHostSession restricted_session{restricted_options};
    expect(submit_approval(restricted_session, 606U, "Denied").rejection_reason ==
               std::optional<std::string>{"ConnectionDenied"},
           "platform membership policy must be checked before a peer enters the stock roster");
    restricted_options.platform_authorized = {};
    LegacySteamHostSession unavailable_policy_session{restricted_options};
    expect(submit_approval(unavailable_policy_session, 607U, "NoPolicyCallback").rejection_reason ==
               std::optional<std::string>{"ConnectionDenied"},
           "a restricted lobby must fail closed when platform authorization is unavailable");
    LegacySteamHostSessionOptions modded_options = make_options();
    std::array<std::uint8_t, 32> digest{};
    digest[0] = 0xA5U;
    modded_options.settings.mod_manifest.mods.push_back(
        {"openminerz.entities", "1.0", digest, ModScope::server});
    LegacySteamHostSession modded_session{modded_options};
    expect(submit_approval(modded_session, 707U, "Stock").rejection_reason ==
                   std::optional<std::string>{"ConnectionDenied"} &&
               !modded_session.connected(707U),
           "a stock client must never enter a server/core-modded session without negotiation");
    expect(modded_session.authorize_extension(708U) && !modded_session.authorize_extension(708U) &&
               !submit_approval(modded_session, 708U, "OpenMinerZ").rejection_reason.has_value() &&
               modded_session.connected(708U),
           "one successful pre-approval extension exchange must authorize exactly that peer");
}

void legacy_steam_client_session_completes_admission_and_rejects_spoofed_routes()
{
    using namespace openminerz::net;

    HostSessionSettings settings;
    settings.server_name = "OpenMinerZ Interop";
    settings.server_message = "Stock client state test";
    settings.protocol = ProtocolVersion::modern_v4;
    settings.maximum_players = 16;
    settings.visibility = HostVisibility::public_lobby;
    settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
    const SessionProperties properties = make_session_properties(settings, false, 1, 0, false);

    LegacySteamHostSessionOptions host_options;
    host_options.settings = settings;
    host_options.game_mode = 1;
    host_options.host_gamer = LegacyGamerIdentity{{9U, 9U}, "Dedicated"};
    LegacySteamHostSession host{host_options};

    constexpr PeerId host_peer = 900U;
    constexpr PeerId alice_peer = 901U;
    LegacySteamClientSessionOptions alice_options;
    alice_options.protocol = ProtocolVersion::modern_v4;
    alice_options.host_peer = host_peer;
    alice_options.expected_host_name = "Dedicated";
    alice_options.session_properties = properties;
    alice_options.local_gamer = LegacyGamerIdentity{{1U, 2U, 3U}, "Alice"};

    // Steam delivery classes can let a live broadcast or even the roster
    // response overtake status 5. Quarantine those packets without exposing
    // application data until both admission records have been validated.
    LegacySteamHostSession reordered_host{host_options};
    LegacySteamClientSession reordered_client{alice_options};
    std::string reordered_error;
    const auto reordered_approval = reordered_client.begin(reordered_error);
    const LegacySteamHostSessionResult reordered_admission =
        reordered_host.process_wire_packet(905U, reordered_approval->payload);
    PacketError reordered_packet_error{};
    const MessageDescriptor *reordered_world_request =
        message_by_name(ProtocolVersion::modern_v4,
                        "DNA.CastleMinerZ.Net.RequestWorldInfoMessage");
    const auto reordered_world_packet = encode_packet(
        ProtocolVersion::modern_v4, reordered_world_request->id, {}, reordered_packet_error);
    LegacySteamOutbound reordered_server_packet;
    reordered_server_packet.recipient = 905U;
    reordered_server_packet.application_packet = *reordered_world_packet;
    reordered_server_packet.delivery = MessageDeliveryPolicy::reliable_ordered;
    const auto reordered_send =
        reordered_host.create_outbound(reordered_server_packet, reordered_error);
    const LegacySteamClientSessionResult early_application =
        reordered_client.process_native_packet(host_peer, reordered_send->front().p2p_channel,
                                               reordered_send->front().payload);
    const LegacySteamClientSessionResult early_roster = reordered_client.process_native_packet(
        host_peer, reordered_admission.sends[1].p2p_channel,
        reordered_admission.sends[1].payload);
    const LegacySteamClientSessionResult reordered_status = reordered_client.process_native_packet(
        host_peer, reordered_admission.sends[0].p2p_channel,
        reordered_admission.sends[0].payload);
    expect(early_application.inbound.empty() && !early_application.audit.has_value() &&
               early_roster.inbound.empty() && !early_roster.audit.has_value() &&
               reordered_client.connected() && reordered_status.inbound.size() == 3U &&
               reordered_status.inbound[0].kind == LegacySteamClientInboundKind::connected &&
               reordered_status.inbound[1].kind == LegacySteamClientInboundKind::peer_joined &&
               reordered_status.inbound[2].kind == LegacySteamClientInboundKind::gameplay &&
               reordered_status.inbound[2].payload == *reordered_world_packet,
           "pre-admission Steam reordering must stay bounded and invisible until status and the "
           "host roster atomically admit the client");

    // Stock Steam maps SendDataOptions.Chat to NetDeliveryMethod.Unknown (zero),
    // then the host wraps the voice payload in the normal direct-data frame.
    // Exercise the full route with a capture-sized A-law buffer, not only the
    // standalone payload codec.
    PayloadError voice_payload_error{};
    const std::vector<std::uint8_t> voice_audio(400U, 0xD5U);
    const auto voice_payload = encode_voice_chat_payload(voice_audio, voice_payload_error);
    const MessageDescriptor *voice_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "VoiceChatMessage");
    PacketError voice_packet_error{};
    const auto voice_packet =
        voice_payload.has_value() && voice_descriptor != nullptr
            ? encode_packet(ProtocolVersion::modern_v4, voice_descriptor->id, *voice_payload,
                            voice_packet_error)
            : std::nullopt;
    LegacySteamOutbound voice_outbound;
    voice_outbound.recipient = 905U;
    if (voice_packet.has_value())
        voice_outbound.application_packet = *voice_packet;
    voice_outbound.delivery = MessageDeliveryPolicy::chat;
    const auto voice_send = reordered_host.create_outbound(voice_outbound, reordered_error);
    const LegacySteamClientSessionResult voice_receive =
        voice_send.has_value()
            ? reordered_client.process_native_packet(host_peer, voice_send->front().p2p_channel,
                                                     voice_send->front().payload)
            : LegacySteamClientSessionResult{};
    expect(voice_send.has_value() && voice_send->size() == 1U &&
               voice_send->front().delivery == SteamDeliveryMethod::unknown &&
               voice_receive.inbound.size() == 1U && !voice_receive.audit.has_value() &&
               voice_receive.inbound.front().kind == LegacySteamClientInboundKind::gameplay &&
               voice_receive.inbound.front().delivery == MessageDeliveryPolicy::chat &&
               voice_receive.inbound.front().payload == *voice_packet,
           "capture-sized stock voice chat must survive Unknown delivery and the host-direct route");

    // Gamer.PlayerID and Gamertag are not routing keys. Live Modern servers can
    // expose repeated opaque PlayerIDs, and the stock client locates the host by
    // GID zero rather than by roster order or stale lobby display metadata.
    LegacySteamClientSession shared_player_id_client{alice_options};
    std::string shared_player_id_error;
    expect(shared_player_id_client.begin(shared_player_id_error).has_value(),
           "the shared-PlayerID fixture must enter the admission wait");
    const LegacySteamClientSessionResult shared_status =
        shared_player_id_client.process_native_packet(
            host_peer, reordered_admission.sends[0].p2p_channel,
            reordered_admission.sends[0].payload);
    LegacyConnectedResponse shared_player_id_roster;
    shared_player_id_roster.player_gid = 3U;
    shared_player_id_roster.peers = {
        LegacyGamerIdentity{{9U, 9U}, "Existing Player"},
        LegacyGamerIdentity{{9U, 9U}, "Renamed Host"},
    };
    shared_player_id_roster.peer_gids = {2U, 0U};
    LegacySteamCodecError shared_player_id_codec_error{};
    const auto shared_player_id_payload = encode_legacy_connected_response(
        shared_player_id_roster, shared_player_id_codec_error);
    const auto shared_player_id_wire =
        shared_player_id_payload.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U,
                                       *shared_player_id_payload,
                                       shared_player_id_codec_error)
            : std::nullopt;
    expect(shared_player_id_wire.has_value(),
           "the shared-PlayerID stock roster must encode");
    const LegacySteamClientSessionResult shared_roster =
        shared_player_id_client.process_native_packet(host_peer, 0U, *shared_player_id_wire);
    expect(!shared_status.audit.has_value() && !shared_roster.audit.has_value() &&
               shared_player_id_client.connected() &&
               shared_player_id_client.gamer_for(0U) != nullptr &&
               shared_player_id_client.gamer_for(2U) != nullptr &&
               shared_roster.inbound.size() == 3U,
           "duplicate opaque PlayerID values must not reject a unique stock GID roster");

    const auto shared_new_peer_payload = encode_legacy_new_peer(
        4U, LegacyGamerIdentity{{9U, 9U}, "Existing Player"}, shared_player_id_codec_error);
    const auto shared_new_peer_wire =
        shared_new_peer_payload.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U,
                                       *shared_new_peer_payload,
                                       shared_player_id_codec_error)
            : std::nullopt;
    expect(shared_new_peer_wire.has_value(),
           "the shared-PlayerID new-peer frame must encode");
    const LegacySteamClientSessionResult shared_new_peer =
        shared_player_id_client.process_native_packet(host_peer, 0U, *shared_new_peer_wire);
    expect(!shared_new_peer.audit.has_value() && shared_new_peer.inbound.size() == 1U &&
               shared_new_peer.inbound[0].kind == LegacySteamClientInboundKind::peer_joined &&
               shared_new_peer.inbound[0].gid == 4U,
           "a later peer with a shared opaque PlayerID must still join under its unique GID");

    const auto shared_drop_payload = encode_legacy_drop_peer(4U);
    const auto shared_drop_wire = encode_legacy_steam_wire(
        SteamIncomingMessageType::data, 1U, shared_drop_payload, shared_player_id_codec_error);
    expect(shared_drop_wire.has_value(), "the stale-drop fixture must encode");
    const LegacySteamClientSessionResult shared_drop =
        shared_player_id_client.process_native_packet(host_peer, 0U, *shared_drop_wire);
    const LegacySteamClientSessionResult repeated_drop =
        shared_player_id_client.process_native_packet(host_peer, 0U, *shared_drop_wire);
    expect(shared_drop.inbound.size() == 1U && !shared_drop.audit.has_value() &&
               repeated_drop.inbound.empty() && !repeated_drop.audit.has_value(),
           "a duplicate or stale stock DropPeer must be an idempotent no-op");

    SessionProperties partial_properties{};
    partial_properties[2] = 5;
    partial_properties[6] = 12345;
    const auto partial_properties_payload = encode_legacy_session_properties_changed(
        partial_properties, shared_player_id_codec_error);
    const auto partial_properties_wire =
        partial_properties_payload.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U,
                                       *partial_properties_payload,
                                       shared_player_id_codec_error)
            : std::nullopt;
    expect(partial_properties_wire.has_value(),
           "the nullable property-update fixture must encode");
    const LegacySteamClientSessionResult partial_property_update =
        shared_player_id_client.process_native_packet(host_peer, 0U,
                                                      *partial_properties_wire);
    const SessionProperties merged_properties = shared_player_id_client.session_properties();
    expect(partial_property_update.inbound.size() == 1U &&
               !partial_property_update.audit.has_value() &&
               merged_properties[0] == properties[0] && merged_properties[2] == 5 &&
               !merged_properties[6].has_value(),
           "stock nullable property changes must merge present mutable slots without erasing or "
           "creating wildcard slots");

    LegacySteamClientSession transient_flood_client{alice_options};
    std::string transient_flood_error;
    expect(transient_flood_client.begin(transient_flood_error).has_value(),
           "the transient-flood fixture must enter the admission wait");
    PayloadError transient_payload_error{};
    PlayerUpdatePayload transient_update;
    transient_update.local_position = {1.0F, 2.0F, 3.0F};
    const auto transient_payload = encode_player_update_payload(
        transient_update, ProtocolVersion::modern_v4, transient_payload_error);
    const MessageDescriptor *transient_descriptor =
        message_by_name(ProtocolVersion::modern_v4, "PlayerUpdateMessage");
    PacketError transient_packet_error{};
    const auto transient_packet = encode_packet(ProtocolVersion::modern_v4,
                                                transient_descriptor->id,
                                                *transient_payload,
                                                transient_packet_error);
    LegacySteamOutbound transient_outbound;
    transient_outbound.recipient = 905U;
    transient_outbound.application_packet = *transient_packet;
    transient_outbound.delivery = MessageDeliveryPolicy::unreliable_sequenced;
    const auto transient_send =
        reordered_host.create_outbound(transient_outbound, transient_flood_error);
    expect(transient_send.has_value() && transient_send->size() == 1U,
           "the transient pre-admission frame must encode");
    bool transient_flood_clean = true;
    for (std::size_t index{}; index < 512U; ++index)
    {
        const LegacySteamClientSessionResult transient =
            transient_flood_client.process_native_packet(
                host_peer, transient_send->front().p2p_channel,
                transient_send->front().payload);
        transient_flood_clean = transient_flood_clean && transient.inbound.empty() &&
                                !transient.audit.has_value() &&
                                !transient.rejection_reason.has_value();
    }
    const LegacySteamClientSessionResult transient_roster =
        transient_flood_client.process_native_packet(
            host_peer, reordered_admission.sends[1].p2p_channel,
            reordered_admission.sends[1].payload);
    const LegacySteamClientSessionResult transient_status =
        transient_flood_client.process_native_packet(
            host_peer, reordered_admission.sends[0].p2p_channel,
            reordered_admission.sends[0].payload);
    expect(transient_flood_clean && transient_roster.inbound.empty() &&
               transient_flood_client.connected() && transient_status.inbound.size() == 2U &&
               !transient_status.audit.has_value(),
           "lossy pre-admission state must never exhaust the bounded reliable quarantine");

    LegacySteamClientSession reliable_flood_client{alice_options};
    std::string reliable_flood_error;
    expect(reliable_flood_client.begin(reliable_flood_error).has_value(),
           "the reliable-flood fixture must enter the admission wait");
    bool reliable_quarantine_clean = true;
    for (std::size_t index{}; index < 64U; ++index)
    {
        const LegacySteamClientSessionResult queued = reliable_flood_client.process_native_packet(
            host_peer, reordered_send->front().p2p_channel, reordered_send->front().payload);
        reliable_quarantine_clean = reliable_quarantine_clean && queued.inbound.empty() &&
                                    !queued.audit.has_value() &&
                                    !queued.rejection_reason.has_value();
    }
    const LegacySteamClientSessionResult reliable_overflow =
        reliable_flood_client.process_native_packet(host_peer, reordered_send->front().p2p_channel,
                                                    reordered_send->front().payload);
    const LegacySteamClientSessionResult reliable_roster =
        reliable_flood_client.process_native_packet(
            host_peer, reordered_admission.sends[1].p2p_channel,
            reordered_admission.sends[1].payload);
    const LegacySteamClientSessionResult reliable_status =
        reliable_flood_client.process_native_packet(
            host_peer, reordered_admission.sends[0].p2p_channel,
            reordered_admission.sends[0].payload);
    expect(reliable_quarantine_clean && reliable_overflow.audit.has_value() &&
               reliable_overflow.audit->reason == "PreAdmissionQuarantineOverflowDropped" &&
               !reliable_overflow.rejection_reason.has_value() && reliable_roster.inbound.empty() &&
               reliable_flood_client.connected() && reliable_status.inbound.size() == 66U,
           "reliable pre-admission overflow must drop excess traffic without denying the valid "
           "status/roster admission");

    LegacySteamClientSession one_byte_status_client{alice_options};
    std::string one_byte_error;
    expect(one_byte_status_client.begin(one_byte_error).has_value(),
           "the malformed-status fixture must enter the admission wait");
    LegacySteamCodecError one_byte_codec_error{};
    constexpr std::array<std::uint8_t, 1> one_byte_status{5U};
    const auto one_byte_wire = encode_legacy_steam_wire(SteamIncomingMessageType::status_changed,
                                                        0U, one_byte_status, one_byte_codec_error);
    expect(one_byte_wire.has_value(), "the malformed one-byte status fixture must encode");
    const LegacySteamClientSessionResult rejected_status =
        one_byte_status_client.process_native_packet(host_peer, 0U, *one_byte_wire);
    expect(rejected_status.audit.has_value() && rejected_status.rejection_reason.has_value() &&
               rejected_status.audit->stage == LegacySteamRejectionStage::session &&
               rejected_status.audit->reason.find("InvalidConnectedStatus") != std::string::npos &&
               !one_byte_status_client.connected(),
           "status 5 must retain the original four-byte BinaryWriter Int32 representation");

    LegacySteamClientSession alice{alice_options};

    std::string error;
    const auto approval = alice.begin(error);
    expect(approval.has_value() && approval->remote_peer == host_peer &&
               approval->p2p_channel == 0U &&
               approval->delivery == SteamDeliveryMethod::reliable_ordered,
           "a joining client must begin with one reliable channel-zero approval envelope");
    const LegacySteamHostSessionResult admitted =
        host.process_wire_packet(alice_peer, approval->payload);
    expect(admitted.sends.size() == 2U && !admitted.rejection_reason.has_value(),
           "the stock host must answer a valid client approval with status and roster");
    const LegacySteamClientSessionResult status = alice.process_native_packet(
        host_peer, admitted.sends[0].p2p_channel, admitted.sends[0].payload);
    const LegacySteamClientSessionResult roster = alice.process_native_packet(
        host_peer, admitted.sends[1].p2p_channel, admitted.sends[1].payload);
    expect(!status.audit.has_value() && roster.inbound.size() == 2U && alice.connected() &&
               alice.local_gid() == std::optional<std::uint8_t>{std::uint8_t{1}} &&
               alice.gamer_for(0U) != nullptr && alice.gamer_for(0U)->gamer_tag == "Dedicated",
           "status 5 plus the host-first roster must atomically admit the client and bind GIDs");

    PayloadError payload_error{};
    PacketError packet_error{};
    const MessageDescriptor *chat =
        message_by_name(ProtocolVersion::modern_v4, "DNA.Net.ChatMessage");
    const auto chat_payload = encode_chat_payload("client to host", payload_error);
    const auto chat_packet =
        encode_packet(ProtocolVersion::modern_v4, chat->id, *chat_payload, packet_error);
    const auto to_host = alice.create_outbound(std::optional<std::uint8_t>{std::uint8_t{0}},
                                               *chat_packet, MessageDeliveryPolicy::chat, error);
    expect(to_host.has_value() && to_host->p2p_channel == 0U &&
               to_host->delivery == SteamDeliveryMethod::unknown,
           "direct client-to-host traffic must use a channel-zero direct frame");
    const LegacySteamHostSessionResult received_chat =
        host.process_wire_packet(alice_peer, to_host->payload);
    expect(received_chat.inbound.size() == 1U && received_chat.inbound[0].payload == *chat_packet &&
               received_chat.inbound[0].sender_gid == 1U,
           "the host authority must receive the admitted client packet with its assigned GID");

    const MessageDescriptor *world_request =
        message_by_name(ProtocolVersion::modern_v4, "DNA.CastleMinerZ.Net.RequestWorldInfoMessage");
    const auto world_packet =
        encode_packet(ProtocolVersion::modern_v4, world_request->id, {}, packet_error);
    LegacySteamOutbound server_packet;
    server_packet.recipient = alice_peer;
    server_packet.application_packet = *world_packet;
    server_packet.delivery = MessageDeliveryPolicy::reliable_ordered;
    const auto server_send = host.create_outbound(server_packet, error);
    expect(server_send.has_value() && server_send->size() == 1U,
           "the host must encode one direct authority packet for the admitted client");
    const LegacySteamClientSessionResult received_world = alice.process_native_packet(
        host_peer, server_send->front().p2p_channel, server_send->front().payload);
    expect(received_world.inbound.size() == 1U &&
               received_world.inbound[0].kind == LegacySteamClientInboundKind::gameplay &&
               received_world.inbound[0].gid == 0U &&
               received_world.inbound[0].payload == *world_packet,
           "the client must expose only a typed direct packet from a known sender GID");

    constexpr PeerId bob_peer = 902U;
    LegacySteamClientSessionOptions bob_options = alice_options;
    bob_options.local_gamer = LegacyGamerIdentity{{4U, 5U, 6U}, "Bob"};
    LegacySteamClientSession bob{bob_options};
    const auto bob_approval = bob.begin(error);
    const LegacySteamHostSessionResult bob_admitted =
        host.process_wire_packet(bob_peer, bob_approval->payload);
    expect(bob_admitted.sends.size() == 3U && bob_admitted.sends.back().remote_peer == alice_peer,
           "a second client admission must announce exactly one new peer to the first client");
    const LegacySteamClientSessionResult bob_announcement = alice.process_native_packet(
        host_peer, bob_admitted.sends.back().p2p_channel, bob_admitted.sends.back().payload);
    expect(bob_announcement.inbound.size() == 1U &&
               bob_announcement.inbound[0].kind == LegacySteamClientInboundKind::peer_joined &&
               bob_announcement.inbound[0].gid == 2U &&
               bob_announcement.inbound[0].gamer.gamer_tag == "Bob",
           "new-peer frames must add one unique validated remote identity");
    const auto bob_drop = host.remove_peer(bob_peer);
    const LegacySteamClientSessionResult bob_departed = alice.process_native_packet(
        host_peer, bob_drop.front().p2p_channel, bob_drop.front().payload);
    expect(bob_departed.inbound.size() == 1U &&
               bob_departed.inbound[0].kind == LegacySteamClientInboundKind::peer_left &&
               bob_departed.inbound[0].gid == 2U && alice.gamer_for(2U) == nullptr,
           "drop-peer frames must remove the GID before reporting the departure");

    LegacySteamClientSession bob_reconnected{bob_options};
    const auto bob_reconnect_approval = bob_reconnected.begin(error);
    const LegacySteamHostSessionResult bob_readmitted =
        host.process_wire_packet(bob_peer, bob_reconnect_approval->payload);
    expect(bob_readmitted.sends.size() == 3U,
           "a disconnected identity may reconnect through a fresh admission transcript");
    const LegacySteamClientSessionResult bob_readmitted_announcement = alice.process_native_packet(
        host_peer, bob_readmitted.sends.back().p2p_channel, bob_readmitted.sends.back().payload);
    const auto fresh_bob_gid = host.gid_for(bob_peer);
    expect(fresh_bob_gid.has_value() && *fresh_bob_gid != 2U &&
               bob_readmitted_announcement.inbound.size() == 1U &&
               bob_readmitted_announcement.inbound[0].gid == *fresh_bob_gid,
           "reconnect must announce a fresh non-aliased GID to existing clients");

    LegacySteamCodecError reconnect_codec_error{};
    const auto stale_frame = encode_legacy_direct_frame(
        LegacyDirectDataFrame{1U, 2U, *chat_packet}, reconnect_codec_error);
    const auto stale_wire =
        stale_frame.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 0U, *stale_frame,
                                       reconnect_codec_error)
            : std::nullopt;
    const LegacySteamClientSessionResult stale_gameplay =
        alice.process_native_packet(host_peer, 0U, *stale_wire);
    expect(stale_gameplay.inbound.empty() && stale_gameplay.audit.has_value() &&
               stale_gameplay.audit->reason == "InvalidDirectApplicationPacket",
           "late packets from the disconnected GID must not mutate the reconnected player");

    const auto fresh_frame = encode_legacy_direct_frame(
        LegacyDirectDataFrame{1U, *fresh_bob_gid, *chat_packet}, reconnect_codec_error);
    const auto fresh_wire =
        fresh_frame.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 0U, *fresh_frame,
                                       reconnect_codec_error)
            : std::nullopt;
    const LegacySteamClientSessionResult fresh_gameplay =
        alice.process_native_packet(host_peer, 0U, *fresh_wire);
    expect(fresh_gameplay.inbound.size() == 1U &&
               fresh_gameplay.inbound[0].gid == *fresh_bob_gid &&
               fresh_gameplay.inbound[0].payload == *chat_packet,
           "the newly announced GID must resume validated gameplay without stale aliasing");

    LegacySteamClientSession duplicate{alice_options};
    const auto duplicate_approval = duplicate.begin(error);
    const LegacySteamHostSessionResult denied =
        host.process_wire_packet(903U, duplicate_approval->payload);
    expect(denied.sends.size() == 1U && denied.sends[0].p2p_channel == 1U,
           "a duplicate identity must use the source raw native-channel-one denial");
    const LegacySteamClientSessionResult denial = duplicate.process_native_packet(
        host_peer, denied.sends[0].p2p_channel, denied.sends[0].payload);
    expect(denial.rejection_reason == std::optional<std::string>{"GamerAlreadyConnected"},
           "the client must decode the bounded null-delimited stock denial reason");
    const LegacySteamClientSessionResult spoofed_sender = alice.process_native_packet(
        999U, server_send->front().p2p_channel, server_send->front().payload);
    expect(spoofed_sender.inbound.empty() && spoofed_sender.audit.has_value() &&
               spoofed_sender.audit->reason == "UnexpectedNativeSender",
           "even a valid authority frame must fail when its native sender is not the lobby host");
}

void steam_bridge_protocol_is_framed_bounded_and_corruption_safe()
{
    using namespace openminerz::net;

    const std::vector<std::uint8_t> crc_fixture{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    expect(steam_bridge_crc32(crc_fixture) == 0xCBF43926U,
           "bridge CRC32 must match the standard independent check vector");

    SteamBridgeProtocolError error = SteamBridgeProtocolError::trailing_data;
    const std::vector<std::uint8_t> frame_payload{0x10U, 0x20U, 0x30U};
    const auto encoded_frame = encode_steam_bridge_frame(SteamBridgeMessageType::start_host,
                                                         0x78563412U, frame_payload, error);
    expect(encoded_frame.has_value() && error == SteamBridgeProtocolError::none &&
               encoded_frame->size() == steam_bridge_header_size + frame_payload.size() &&
               std::equal(encoded_frame->begin(), encoded_frame->begin() + 16,
                          bytes_from_hex("4F4D5342020002000300000012345678").begin()),
           "bridge frames must expose the fixed OMSB/version/type/length/sequence header");
    SteamBridgeFrame decoded_frame;
    expect(decode_steam_bridge_frame(*encoded_frame, decoded_frame, error) &&
               error == SteamBridgeProtocolError::none &&
               decoded_frame.type == SteamBridgeMessageType::start_host &&
               decoded_frame.sequence == 0x78563412U && decoded_frame.payload == frame_payload,
           "a checksum-valid bridge frame must round-trip without rewriting payload bytes");

    auto corrupt = *encoded_frame;
    corrupt.back() ^= 0x80U;
    expect(!decode_steam_bridge_frame(corrupt, decoded_frame, error) &&
               error == SteamBridgeProtocolError::checksum_mismatch,
           "one corrupted payload bit must invalidate the whole bridge frame");
    auto bad_magic = *encoded_frame;
    bad_magic[0] = 'X';
    expect(!decode_steam_bridge_frame(bad_magic, decoded_frame, error) &&
               error == SteamBridgeProtocolError::wrong_magic,
           "foreign named-pipe traffic must fail before payload parsing");
    auto bad_version = *encoded_frame;
    bad_version[4] = 3U;
    expect(!decode_steam_bridge_frame(bad_version, decoded_frame, error) &&
               error == SteamBridgeProtocolError::unsupported_version,
           "an unsupported IPC protocol version must fail closed");
    auto bad_type = *encoded_frame;
    bad_type[6] = 0xFFU;
    bad_type[7] = 0x7FU;
    expect(!decode_steam_bridge_frame(bad_type, decoded_frame, error) &&
               error == SteamBridgeProtocolError::unknown_message_type,
           "unknown IPC message types must fail before checksum or typed dispatch");
    auto bad_length = *encoded_frame;
    bad_length[8] = 4U;
    expect(!decode_steam_bridge_frame(bad_length, decoded_frame, error) &&
               error == SteamBridgeProtocolError::length_mismatch,
           "advertised and received IPC lengths must agree exactly");
    expect(!decode_steam_bridge_frame(std::span<const std::uint8_t>{encoded_frame->data(), 19U},
                                      decoded_frame, error) &&
               error == SteamBridgeProtocolError::too_small,
           "partial IPC headers must never reach typed parsing");
    std::vector<std::uint8_t> oversized_payload(steam_bridge_maximum_payload_size + 1U, 0U);
    expect(
        !encode_steam_bridge_frame(SteamBridgeMessageType::heartbeat, 1U, oversized_payload, error)
                .has_value() &&
            error == SteamBridgeProtocolError::too_large,
        "the x64/x86 bridge must reject payloads beyond its fixed memory budget");
    expect(!encode_steam_bridge_frame(static_cast<SteamBridgeMessageType>(0xFFFFU), 1U, {}, error)
                   .has_value() &&
               error == SteamBridgeProtocolError::unknown_message_type,
           "the local encoder must not manufacture unsupported IPC message types");

    HostSessionSettings settings;
    settings.protocol = ProtocolVersion::modern_v4;
    settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
    SteamBridgeHostConfig host;
    host.maximum_players = 12U;
    host.join_policy = SteamBridgeJoinPolicy::friends_only;
    host.password_protected = true;
    host.server_name = "OpenMinerZ Modern Host";
    host.session_properties = make_session_properties(settings, false, 4, 2, true);
    ExtensionMessage extension_advertisement;
    extension_advertisement.kind = ExtensionMessageKind::server_accept;
    extension_advertisement.manifest = settings.mod_manifest;
    host.extension_metadata = encode_extension_message(extension_advertisement);
    const auto encoded_host = encode_steam_bridge_host_config(host, error);
    SteamBridgeHostConfig decoded_host;
    expect(encoded_host.has_value() &&
               decode_steam_bridge_host_config(*encoded_host, decoded_host, error) &&
               decoded_host.app_id == castle_miner_z_steam_app_id &&
               decoded_host.maximum_players == host.maximum_players &&
               decoded_host.join_policy == host.join_policy && decoded_host.password_protected &&
               decoded_host.server_name == host.server_name &&
               decoded_host.session_properties == host.session_properties &&
               decoded_host.extension_metadata == host.extension_metadata,
           "host startup data must preserve exact lobby and eight-slot session metadata");
    SteamBridgeHostConfig invalid_host = host;
    invalid_host.app_id = 1U;
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "the helper protocol must be pinned to CastleMiner Z app ID 253430");
    invalid_host = host;
    invalid_host.maximum_players = 0U;
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "a bridge host cannot advertise an empty player capacity");
    invalid_host = host;
    invalid_host.join_policy = static_cast<SteamBridgeJoinPolicy>(3U);
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "only stock lobby join-policy values may cross the bridge");
    invalid_host = host;
    invalid_host.session_properties[6] = 1;
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "unused stock session-property slots must remain absent");
    invalid_host = host;
    invalid_host.session_properties[2] = 6;
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "profile-specific game-mode bounds must be enforced before lobby publication");
    invalid_host = host;
    invalid_host.extension_metadata.resize(steam_bridge_maximum_extension_metadata_size + 1U);
    expect(!encode_steam_bridge_host_config(invalid_host, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "private lobby extension metadata must remain inside its 4096-byte wire budget");
    auto reserved_host = *encoded_host;
    reserved_host[7] = 1U;
    expect(!decode_steam_bridge_host_config(reserved_host, decoded_host, error) &&
               error == SteamBridgeProtocolError::malformed_payload,
           "nonzero reserved host-config bytes must fail closed");
    auto trailing_host = *encoded_host;
    trailing_host.push_back(0U);
    expect(!decode_steam_bridge_host_config(trailing_host, decoded_host, error) &&
               error == SteamBridgeProtocolError::trailing_data,
           "typed bridge payloads must consume every supplied byte");
    auto invalid_boolean_host = *encoded_host;
    invalid_boolean_host[6] = 2U;
    expect(!decode_steam_bridge_host_config(invalid_boolean_host, decoded_host, error) &&
               error == SteamBridgeProtocolError::malformed_payload,
           "bridge booleans must have one canonical encoding");

    const SteamBridgeHostReady ready{76561198000000001ULL, 109775241000000001ULL, "Host Persona"};
    const auto encoded_ready = encode_steam_bridge_host_ready(ready, error);
    SteamBridgeHostReady decoded_ready;
    expect(encoded_ready.has_value() &&
               decode_steam_bridge_host_ready(*encoded_ready, decoded_ready, error) &&
               decoded_ready.local_peer == ready.local_peer &&
               decoded_ready.lobby_id == ready.lobby_id &&
               decoded_ready.persona_name == ready.persona_name,
           "host-ready events must bind nonzero local Steam, lobby, and persona identities");
    SteamBridgeHostReady invalid_ready = ready;
    invalid_ready.lobby_id = 0U;
    expect(!encode_steam_bridge_host_ready(invalid_ready, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "a helper must not claim readiness before Steam supplies a lobby ID");

    const SteamBridgeInboundWire inbound{76561198000000002ULL, 1U, {4U, 0U, 0U, 0U}};
    const auto encoded_inbound = encode_steam_bridge_inbound_wire(inbound, error);
    SteamBridgeInboundWire decoded_inbound;
    expect(encoded_inbound.has_value() &&
               decode_steam_bridge_inbound_wire(*encoded_inbound, decoded_inbound, error) &&
               decoded_inbound.peer == inbound.peer &&
               decoded_inbound.p2p_channel == inbound.p2p_channel &&
               decoded_inbound.wire == inbound.wire,
           "native inbound datagrams must preserve sender, channel, and exact stock wire bytes");
    auto zero_inbound_peer = *encoded_inbound;
    std::fill(zero_inbound_peer.begin(), zero_inbound_peer.begin() + 8, std::uint8_t{0});
    expect(!decode_steam_bridge_inbound_wire(zero_inbound_peer, decoded_inbound, error) &&
               error == SteamBridgeProtocolError::out_of_range,
           "anonymous native datagrams must not cross into the authoritative x64 process");
    auto invalid_inbound_channel = *encoded_inbound;
    invalid_inbound_channel[8] = 2U;
    expect(!decode_steam_bridge_inbound_wire(invalid_inbound_channel, decoded_inbound, error) &&
               error == SteamBridgeProtocolError::out_of_range,
           "only source-observed native channels zero and one may cross into x64");
    auto invalid_inbound_reserved = *encoded_inbound;
    invalid_inbound_reserved[9] = 1U;
    expect(!decode_steam_bridge_inbound_wire(invalid_inbound_reserved, decoded_inbound, error) &&
               error == SteamBridgeProtocolError::malformed_payload,
           "reserved inbound-channel bytes must fail closed");
    SteamBridgeInboundWire oversized_inbound{
        1U, 0U, std::vector<std::uint8_t>(legacy_steam_maximum_wire_size + 1U, 0U)};
    expect(!encode_steam_bridge_inbound_wire(oversized_inbound, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "native datagrams must remain inside the deployed 65535-byte ceiling");

    const SteamBridgeOutboundWire outbound{76561198000000003ULL,
                                           SteamDeliveryMethod::reliable_sequenced,
                                           1U,
                                           {0U, 'D', 'e', 'n', 'i', 'e', 'd', 0U}};
    const auto encoded_outbound = encode_steam_bridge_outbound_wire(outbound, error);
    SteamBridgeOutboundWire decoded_outbound;
    expect(encoded_outbound.has_value() &&
               decode_steam_bridge_outbound_wire(*encoded_outbound, decoded_outbound, error) &&
               decoded_outbound.peer == outbound.peer &&
               decoded_outbound.delivery == outbound.delivery &&
               decoded_outbound.p2p_channel == outbound.p2p_channel &&
               decoded_outbound.wire == outbound.wire,
           "outbound bridge datagrams must preserve native delivery, channel, and denial bytes");
    auto invalid_delivery = *encoded_outbound;
    invalid_delivery[8] = 0xFFU;
    expect(!decode_steam_bridge_outbound_wire(invalid_delivery, decoded_outbound, error) &&
               error == SteamBridgeProtocolError::out_of_range,
           "unknown native delivery values must never reach SteamNetworking");
    auto invalid_channel = *encoded_outbound;
    invalid_channel[9] = 2U;
    expect(!decode_steam_bridge_outbound_wire(invalid_channel, decoded_outbound, error) &&
               error == SteamBridgeProtocolError::out_of_range,
           "the helper must expose only the source-derived native channels zero and one");
    auto invalid_reserved = *encoded_outbound;
    invalid_reserved[10] = 1U;
    expect(!decode_steam_bridge_outbound_wire(invalid_reserved, decoded_outbound, error) &&
               error == SteamBridgeProtocolError::malformed_payload,
           "nonzero reserved datagram bytes must fail closed");

    const auto encoded_peer = encode_steam_bridge_peer(76561198000000004ULL, error);
    PeerId decoded_peer{};
    expect(encoded_peer.has_value() &&
               decode_steam_bridge_peer(*encoded_peer, decoded_peer, error) &&
               decoded_peer == 76561198000000004ULL,
           "peer-left events must round-trip one exact nonzero Steam ID");
    expect(!encode_steam_bridge_peer(0U, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "the peer-event encoder must reject the same anonymous identity as its decoder");
    auto trailing_peer = *encoded_peer;
    trailing_peer.push_back(0U);
    expect(!decode_steam_bridge_peer(trailing_peer, decoded_peer, error) &&
               error == SteamBridgeProtocolError::malformed_payload,
           "peer events have an exact eight-byte typed payload");

    const SteamBridgeDiscoveryRequest discovery_request{ProtocolVersion::modern_v4, 50U};
    const auto encoded_discovery_request =
        encode_steam_bridge_discovery_request(discovery_request, error);
    SteamBridgeDiscoveryRequest decoded_discovery_request;
    expect(encoded_discovery_request.has_value() &&
               decode_steam_bridge_discovery_request(*encoded_discovery_request,
                                                     decoded_discovery_request, error) &&
               decoded_discovery_request.protocol == discovery_request.protocol &&
               decoded_discovery_request.maximum_results == 50U,
           "discovery requests must preserve the optional stock profile and 50-result ceiling");
    SteamBridgeDiscoveryRequest invalid_discovery_request{std::nullopt, 51U};
    expect(!encode_steam_bridge_discovery_request(invalid_discovery_request, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "the helper cannot be asked to allocate an unbounded lobby result set");
    SteamBridgeDiscoveredLobby discovered;
    discovered.lobby_id = 109775241000000001ULL;
    discovered.host_peer = 76561198000000001ULL;
    discovered.current_players = 2U;
    discovered.maximum_players = 12U;
    discovered.join_policy = SteamBridgeJoinPolicy::friends_only;
    discovered.password_protected = true;
    discovered.friend_count = 1U;
    discovered.host_is_friend = true;
    discovered.name = "Discovered Host";
    discovered.host_name = "Host Persona";
    discovered.created_at = "Thu, 27 Aug 2026 12:34:56 GMT";
    discovered.session_properties = host.session_properties;
    discovered.extension_metadata = host.extension_metadata;
    SteamBridgeDiscoveryResults discovery_results{
        ready.local_peer, ready.persona_name, {discovered}};
    discovery_results.reported_lobby_count = 2U;
    discovery_results.rejected_lobby_count = 1U;
    discovery_results.rejected_native_count = 1U;
    discovery_results.diagnostic = "lobby=109775241000000002 host=0 members=1 limit=16";
    const auto encoded_discovery_results =
        encode_steam_bridge_discovery_results(discovery_results, error);
    SteamBridgeDiscoveryResults decoded_discovery_results;
    expect(encoded_discovery_results.has_value() &&
               decode_steam_bridge_discovery_results(*encoded_discovery_results,
                                                     decoded_discovery_results, error) &&
               decoded_discovery_results.local_peer == ready.local_peer &&
               decoded_discovery_results.persona_name == ready.persona_name &&
               decoded_discovery_results.lobbies.size() == 1U &&
               decoded_discovery_results.reported_lobby_count == 2U &&
               decoded_discovery_results.rejected_lobby_count == 1U &&
               decoded_discovery_results.rejected_native_count == 1U &&
               decoded_discovery_results.rejected_metadata_count == 0U &&
               decoded_discovery_results.rejected_contract_count == 0U &&
               decoded_discovery_results.diagnostic == discovery_results.diagnostic &&
               decoded_discovery_results.lobbies[0].lobby_id == discovered.lobby_id &&
               decoded_discovery_results.lobbies[0].host_peer == discovered.host_peer &&
               decoded_discovery_results.lobbies[0].host_is_friend &&
               decoded_discovery_results.lobbies[0].session_properties ==
                   discovered.session_properties &&
               decoded_discovery_results.lobbies[0].extension_metadata ==
                   discovered.extension_metadata,
           "bounded discovery results must preserve Steam identity, lobby metadata, and slots");
    SteamBridgeDiscoveryResults duplicate_results{
        ready.local_peer, ready.persona_name, {discovered, discovered}};
    expect(!encode_steam_bridge_discovery_results(duplicate_results, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "duplicate lobby IDs must fail before entering the browser model");
    SteamBridgeDiscoveryResults invalid_identity{0U, ready.persona_name, {discovered}};
    expect(!encode_steam_bridge_discovery_results(invalid_identity, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "discovery must never authenticate an anonymous local Steam identity");
    invalid_identity.local_peer = ready.local_peer;
    invalid_identity.persona_name.clear();
    expect(!encode_steam_bridge_discovery_results(invalid_identity, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "discovery must carry a nonempty bounded Steam persona");
    SteamBridgeJoinRequest join_request{discovered.lobby_id};
    const auto encoded_join_request = encode_steam_bridge_join_request(join_request, error);
    SteamBridgeJoinRequest decoded_join_request;
    expect(
        encoded_join_request.has_value() &&
            decode_steam_bridge_join_request(*encoded_join_request, decoded_join_request, error) &&
            decoded_join_request.lobby_id == discovered.lobby_id,
        "join commands must identify exactly one nonzero Steam lobby");
    SteamBridgeJoinReady join_ready{ready.local_peer, ready.persona_name, discovered};
    const auto encoded_join_ready = encode_steam_bridge_join_ready(join_ready, error);
    SteamBridgeJoinReady decoded_join_ready;
    expect(encoded_join_ready.has_value() &&
               decode_steam_bridge_join_ready(*encoded_join_ready, decoded_join_ready, error) &&
               decoded_join_ready.local_peer == join_ready.local_peer &&
               decoded_join_ready.lobby.lobby_id == join_ready.lobby.lobby_id,
           "join readiness must bind local identity to one fully valid stock lobby contract");
    join_ready.lobby.session_properties[0] = 99;
    expect(!encode_steam_bridge_join_ready(join_ready, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "joining must fail closed when refreshed lobby properties are no longer compatible");

    const SteamBridgeFailure failure{SteamBridgeFailureCode::steam_init_failed,
                                     "SteamAPI_Init returned false"};
    const auto encoded_failure = encode_steam_bridge_failure(failure, error);
    SteamBridgeFailure decoded_failure;
    expect(encoded_failure.has_value() &&
               decode_steam_bridge_failure(*encoded_failure, decoded_failure, error) &&
               decoded_failure.code == failure.code && decoded_failure.detail == failure.detail,
           "bounded printable bridge failures must survive diagnostic transport");
    SteamBridgeFailure invalid_failure{static_cast<SteamBridgeFailureCode>(10U), "invalid"};
    expect(!encode_steam_bridge_failure(invalid_failure, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "only enumerated helper failures may cross the trust boundary");
    invalid_failure = {SteamBridgeFailureCode::protocol_error, std::string(513U, 'x')};
    expect(!encode_steam_bridge_failure(invalid_failure, error).has_value() &&
               error == SteamBridgeProtocolError::out_of_range,
           "helper diagnostic text must remain bounded");
}

void steam_bridge_x64_x86_pipe_is_secured_supervised_and_sequenced()
{
    using namespace openminerz::net;

    LegacySteamRuntimeHostOptions host_options;
    host_options.settings.server_name = "Runtime Host";
    host_options.settings.visibility = HostVisibility::public_lobby;
    host_options.settings.join_policy = HostJoinPolicy::anyone;
    host_options.settings.protocol = ProtocolVersion::modern_v4;
    host_options.settings.mod_manifest.protocol = ProtocolVersion::modern_v4;
    host_options.settings.maximum_players = 12;
    host_options.settings.password = "pw";
    host_options.game_mode = 4;
    host_options.difficulty = 2;
    host_options.infinite_resources = true;
    SteamBridgeHostConfig host_config;
    std::string error;
    expect(make_steam_bridge_host_config(host_options, host_config, error) &&
               host_config.join_policy == SteamBridgeJoinPolicy::anyone &&
               host_config.password_protected && host_config.maximum_players == 12U &&
               host_config.session_properties ==
                   make_session_properties(host_options.settings, true, 4, 2, true),
           "runtime host settings must map to the exact live Steam lobby contract");
    host_options.settings.visibility = HostVisibility::friends_only;
    expect(make_steam_bridge_host_config(host_options, host_config, error) &&
               host_config.join_policy == SteamBridgeJoinPolicy::friends_only,
           "friends visibility must never accidentally publish a public native lobby");
    host_options.settings.visibility = HostVisibility::public_lobby;
    host_options.settings.join_policy = HostJoinPolicy::invite_only;
    expect(make_steam_bridge_host_config(host_options, host_config, error) &&
               host_config.join_policy == SteamBridgeJoinPolicy::invite_only,
           "the most restrictive host admission policy must control native lobby type");
    host_options.settings.visibility = HostVisibility::lan;
    expect(!make_steam_bridge_host_config(host_options, host_config, error) && !error.empty(),
           "LAN visibility must not be misrepresented through the Steam lobby adapter");

    const std::filesystem::path helper =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "out" / "steam-bridge32" /
        "OpenMinerZ.SteamBridge32.exe";
    expect(std::filesystem::is_regular_file(helper),
           "the native build must produce the separately compiled x86 Steam helper");
    const bool pipe_certificate =
        SteamBridgeClient::protocol_self_test(helper, std::chrono::seconds{10}, error);
    expect(pipe_certificate, std::string{"the secured x64/x86 pipe certificate failed: "} + error);
    expect(!SteamBridgeClient::protocol_self_test(helper.parent_path() / "missing.exe",
                                                  std::chrono::seconds{1}, error) &&
               !error.empty(),
           "the x64 launcher must reject a missing or renamed helper before creating IPC state");
}

void protocol_trace_is_timestamped_decoded_bounded_and_identity_free()
{
    using namespace openminerz::net;

    ProtocolTraceRecord fixture;
    fixture.role = ProtocolTraceRole::host;
    fixture.direction = ProtocolTraceDirection::inbound;
    fixture.result = ProtocolTraceResult::rejected;
    fixture.protocol = ProtocolVersion::modern_v4;
    fixture.event = "PlayerUpdateMessage";
    fixture.sender_gid = 3U;
    fixture.recipient_gid = std::uint8_t{0};
    fixture.delivery = MessageDeliveryPolicy::unreliable_sequenced;
    fixture.channel = 1U;
    fixture.packet_bytes = 25U;
    fixture.reason = "SteamID=76561198000000000 password=hunter2 Gamer=SecretPlayer";
    const std::string encoded = encode_protocol_trace_record_json(7U, 1'700'000'000'123LL,
                                                                  42'000U, fixture);
    expect(encoded.find("\"sequence\":7") != std::string::npos &&
               encoded.find("\"unix_ms\":1700000000123") != std::string::npos &&
               encoded.find("\"elapsed_us\":42000") != std::string::npos &&
               encoded.find("\"event\":\"PlayerUpdateMessage\"") != std::string::npos &&
               encoded.find("\"sender_gid\":3") != std::string::npos &&
               encoded.find("\"recipient_gid\":0") != std::string::npos &&
               encoded.find("\"reason_hash\":\"") != std::string::npos,
           "interop traces must retain timestamp, direction, message, route, delivery, and result");
    expect(encoded.find("76561198000000000") == std::string::npos &&
               encoded.find("hunter2") == std::string::npos &&
               encoded.find("SecretPlayer") == std::string::npos,
           "interop traces must fingerprint diagnostics instead of exposing identities or secrets");

    fixture.event = "bad event with untrusted text";
    const std::string invalid_event =
        encode_protocol_trace_record_json(8U, 1'700'000'000'124LL, 42'001U, fixture);
    expect(invalid_event.find("\"event\":\"InvalidEventLabel\"") != std::string::npos &&
               invalid_event.find("bad event with untrusted text") == std::string::npos,
           "only bounded code/message labels may enter the JSONL evidence stream");

    const std::filesystem::path trace_path =
        std::filesystem::temp_directory_path() /
        std::format("openminerz-protocol-trace-{}.jsonl",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    std::string error;
    {
        ProtocolTraceWriter writer;
        expect(writer.open(trace_path, ProtocolTraceRole::client, ProtocolVersion::modern_v4,
                           error) &&
                   writer.active(),
               "an explicit certification path must open an append-only JSONL writer");
        PayloadError payload_error{};
        PacketError packet_error{};
        const MessageDescriptor *chat = message_by_name(ProtocolVersion::modern_v4, "ChatMessage");
        const auto payload = encode_chat_payload("never copied into trace", payload_error);
        const auto packet = chat != nullptr && payload.has_value()
                                ? encode_packet(ProtocolVersion::modern_v4, chat->id, *payload,
                                                packet_error)
                                : std::nullopt;
        expect(packet.has_value() &&
                   writer.record_packet(ProtocolTraceDirection::outbound,
                                        ProtocolTraceResult::accepted, *packet, 4U, std::nullopt,
                                        MessageDeliveryPolicy::chat, 1U),
               "the writer must decode the validated application packet name without storing it");
        expect(writer.record(ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                             "PacketRejected", 2U, std::uint8_t{4},
                             MessageDeliveryPolicy::reliable_ordered, 0U, 99U, fixture.reason),
               "a rejection fingerprint must be durable without retaining hostile diagnostic text");
    }
    std::ifstream input{trace_path, std::ios::binary};
    const std::string written{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::error_code remove_error;
    std::filesystem::remove(trace_path, remove_error);
    expect(!written.empty() && written.find("\"event\":\"TraceStarted\"") != std::string::npos &&
               written.find("\"event\":\"ChatMessage\"") != std::string::npos &&
               written.find("never copied into trace") == std::string::npos &&
               written.find("76561198000000000") == std::string::npos &&
               written.find("hunter2") == std::string::npos &&
               written.find("SecretPlayer") == std::string::npos,
           "the persisted trace must contain decoded metadata only, never payload or identity text");
}

void interop_state_snapshot_hashes_semantic_world_player_and_inventory_state()
{
    using namespace openminerz;

    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::modern);
    const std::uint64_t empty_inventory = fingerprint_interop_inventory(inventory);
    inventory.tray(1U)[2U] = InventoryStack{InventoryItemId::diamond_pickaxe, 1U};
    inventory.tray(1U)[2U]->durability = 0.625F;
    inventory.tray(1U)[2U]->waypoint_valid = true;
    inventory.tray(1U)[2U]->waypoint = {10.0F, -3.0F, 22.0F};
    inventory.tray(1U)[2U]->waypoint_name = "private label hashed, never serialized";
    inventory.select_tray(1U);
    inventory.select(2U);
    const std::uint64_t populated_inventory = fingerprint_interop_inventory(inventory);
    expect(populated_inventory != empty_inventory,
           "the interop inventory fingerprint must include profile, tray, selection, and item state");

    const std::vector<std::array<int, 4>> edits{{4, 2, -1, 7}, {-8, 1, 3, 9}};
    const std::vector<std::array<int, 4>> reordered{edits.rbegin(), edits.rend()};
    std::vector<std::array<int, 4>> changed = edits;
    changed[0][3] = 8;
    expect(fingerprint_interop_edits(edits) == fingerprint_interop_edits(reordered) &&
               fingerprint_interop_edits(edits) != fingerprint_interop_edits(changed),
           "world-edit evidence must be order-independent but sensitive to semantic mutations");

    world::WorldGenerator generator{12345};
    world::WorldRegion region = generator.generate_region(-2, -2, 5, 5);
    const InteropRegionFingerprint before = fingerprint_interop_region(region, 0, 0, 16);
    expect(interop_region_fully_loaded(region, 0, 0, 16) &&
               !interop_region_fully_loaded(region, region.maximum_x(), 0, 16) &&
               !interop_region_fully_loaded(region, std::numeric_limits<int>::max(),
                                            std::numeric_limits<int>::min(), 16),
           "fixed certification regions must be complete and overflow-safe before hashing");
    const world::BlockType previous = region.block(0, 0, 0);
    region.set_block(0, 0, 0, previous == world::BlockType::rock ? world::BlockType::dirt
                                                                 : world::BlockType::rock);
    const InteropRegionFingerprint after = fingerprint_interop_region(region, 0, 0, 16);
    expect(before.minimum_x == -16 && before.maximum_x == 16 && before.minimum_z == -16 &&
               before.maximum_z == 16 && before.hash != after.hash,
           "the fixed 33x128x33 region certificate must detect one authoritative block divergence");

    InteropStateSnapshot snapshot;
    snapshot.role = InteropStateRole::client;
    snapshot.phase = InteropStatePhase::connected;
    snapshot.protocol = net::ProtocolVersion::modern_v4;
    snapshot.world_seed = 12345;
    snapshot.region = after;
    snapshot.player_position = {1.25F, 2.5F, -3.75F};
    snapshot.player_yaw = 0.5F;
    snapshot.player_pitch = -0.25F;
    snapshot.player_health = 79.0F;
    snapshot.inventory_hash = populated_inventory;
    snapshot.edit_hash = fingerprint_interop_edits(edits);
    snapshot.edit_count = edits.size();
    snapshot.remote_player_count = 1U;
    snapshot.enemy_count = 3U;
    snapshot.pickup_count = 4U;
    snapshot.dragon_active = true;
    snapshot.players.push_back(
        {4U, true, snapshot.player_position, snapshot.player_yaw, snapshot.player_pitch,
         snapshot.player_health, false});
    snapshot.players.push_back({0U, false, {-2.0F, 3.0F, 5.0F}, 1.0F, 0.1F, std::nullopt, false});
    const auto encoded = encode_interop_state_snapshot_json(1'700'000'000'123LL, snapshot);
    expect(encoded.has_value() && encoded->find("\"role\":\"client\"") != std::string::npos &&
               encoded->find("\"phase\":\"connected\"") != std::string::npos &&
               encoded->find("\"players\":[{\"gid\":0") != std::string::npos &&
               encoded->find("\"gid\":4,\"local\":true") != std::string::npos &&
               encoded->find("\"state_hash\":\"") != std::string::npos &&
               encoded->find("private label") == std::string::npos,
           "state evidence must expose comparable hashes and bounded numeric state, not item labels");
    InteropStateSnapshot invalid = snapshot;
    invalid.player_health = std::numeric_limits<float>::infinity();
    expect(!encode_interop_state_snapshot_json(1'700'000'000'123LL, invalid).has_value(),
           "non-finite state must fail before a certification artifact is written");
    invalid = snapshot;
    invalid.players.back().gid = invalid.players.front().gid;
    expect(!encode_interop_state_snapshot_json(1'700'000'000'123LL, invalid).has_value(),
           "duplicate player GIDs must fail before a comparison barrier is written");

    const std::filesystem::path state_path =
        std::filesystem::temp_directory_path() /
        std::format("openminerz-interop-state-{}.json",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    std::string error;
    expect(write_interop_state_snapshot(state_path, snapshot, error),
           "an explicit state-dump path must receive one complete snapshot");
    std::ifstream input{state_path, std::ios::binary};
    const std::string written{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::error_code remove_error;
    std::filesystem::remove(state_path, remove_error);
    expect(written == *encoded + "\n" ||
               (written.find("\"schema\":\"openminerz.interop-state.v1\"") !=
                    std::string::npos &&
                written.find("private label") == std::string::npos),
           "the persisted snapshot must use the redacted versioned state schema");
}

struct TestCase final
{
    std::string_view name;
    std::function<void()> body;
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"canvas_normalizes_resolutions_and_fonts", canvas_normalizes_resolutions_and_fonts},
        {"pointer_input_contract_never_routes_secondary_to_jump_or_drop",
         pointer_input_contract_never_routes_secondary_to_jump_or_drop},
        {"dynamic_radiance_is_world_anchored_colored_and_occlusion_safe",
         dynamic_radiance_is_world_anchored_colored_and_occlusion_safe},
        {"biome_atmospheres_are_distinct_bounded_and_reversible",
         biome_atmospheres_are_distinct_bounded_and_reversible},
        {"settings_clamp_and_round_trip", settings_clamp_and_round_trip},
        {"advanced_settings_have_live_adjustments", advanced_settings_have_live_adjustments},
        {"main_menu_routes_without_scrolling_or_dead_actions",
         main_menu_routes_without_scrolling_or_dead_actions},
        {"settings_navigation_is_bounded_across_tabs", settings_navigation_is_bounded_across_tabs},
        {"host_menu_exposes_full_preflight_and_preserves_flow_origin",
         host_menu_exposes_full_preflight_and_preserves_flow_origin},
        {"join_menu_routes_browser_details_and_password_actions",
         join_menu_routes_browser_details_and_password_actions},
        {"network_protocol_manifest_and_framing_match_deployed_profiles",
         network_protocol_manifest_and_framing_match_deployed_profiles},
        {"mod_handshake_is_ordered_strict_and_stock_client_safe",
         mod_handshake_is_ordered_strict_and_stock_client_safe},
        {"mod_catalog_hashes_content_and_selects_ordered_profiles",
         mod_catalog_hashes_content_and_selects_ordered_profiles},
        {"host_session_settings_publish_original_session_properties",
         host_session_settings_publish_original_session_properties},
        {"session_browser_preflights_discovery_and_feeds_the_admission_gate",
         session_browser_preflights_discovery_and_feeds_the_admission_gate},
        {"admission_gate_never_forwards_before_authentication_and_mod_sync",
         admission_gate_never_forwards_before_authentication_and_mod_sync},
        {"bounded_loopback_transport_rejects_native_envelope_abuse",
         bounded_loopback_transport_rejects_native_envelope_abuse},
        {"legacy_steam_transport_codecs_match_deployed_wire_and_fail_closed",
         legacy_steam_transport_codecs_match_deployed_wire_and_fail_closed},
        {"legacy_steam_host_session_admits_routes_and_relays_only_valid_stock_traffic",
         legacy_steam_host_session_admits_routes_and_relays_only_valid_stock_traffic},
        {"legacy_steam_client_session_completes_admission_and_rejects_spoofed_routes",
         legacy_steam_client_session_completes_admission_and_rejects_spoofed_routes},
        {"steam_bridge_protocol_is_framed_bounded_and_corruption_safe",
         steam_bridge_protocol_is_framed_bounded_and_corruption_safe},
        {"steam_bridge_x64_x86_pipe_is_secured_supervised_and_sequenced",
         steam_bridge_x64_x86_pipe_is_secured_supervised_and_sequenced},
        {"protocol_trace_is_timestamped_decoded_bounded_and_identity_free",
         protocol_trace_is_timestamped_decoded_bounded_and_identity_free},
        {"interop_state_snapshot_hashes_semantic_world_player_and_inventory_state",
         interop_state_snapshot_hashes_semantic_world_player_and_inventory_state},
        {"typed_bootstrap_codecs_match_original_layouts_and_reject_hostile_values",
         typed_bootstrap_codecs_match_original_layouts_and_reject_hostile_values},
        {"combat_codecs_match_source_layouts_profiles_and_hostile_bounds",
         combat_codecs_match_source_layouts_profiles_and_hostile_bounds},
        {"pickup_codecs_preserve_variable_items_and_authority_fields",
         pickup_codecs_preserve_variable_items_and_authority_fields},
        {"enemy_codecs_match_profile_extensions_and_authority_domains",
         enemy_codecs_match_profile_extensions_and_authority_domains},
        {"dragon_codecs_preserve_waypoints_migration_and_bounded_detonation",
         dragon_codecs_preserve_waypoints_migration_and_bounded_detonation},
        {"modern_and_voice_codecs_are_profile_gated_and_bounded",
         modern_and_voice_codecs_are_profile_gated_and_bounded},
        {"inventory_bootstrap_matches_both_deployed_profiles_and_rejects_hostile_blobs",
         inventory_bootstrap_matches_both_deployed_profiles_and_rejects_hostile_blobs},
        {"world_info_matches_original_classic_and_modern_binaries",
         world_info_matches_original_classic_and_modern_binaries},
        {"authoritative_late_join_bootstrap_applies_world_save_in_strict_order",
         authoritative_late_join_bootstrap_applies_world_save_in_strict_order},
        {"oversized_world_info_uses_ordered_item_crate_replay",
         oversized_world_info_uses_ordered_item_crate_replay},
        {"modern_bootstrap_preserves_custom_doors_and_spawner_identity",
         modern_bootstrap_preserves_custom_doors_and_spawner_identity},
        {"authoritative_client_bootstrap_reconstructs_snapshot_and_fails_closed",
         authoritative_client_bootstrap_reconstructs_snapshot_and_fails_closed},
        {"client_gameplay_authority_is_route_strict_and_identity_bound",
         client_gameplay_authority_is_route_strict_and_identity_bound},
        {"player_replication_packs_original_angles_half_velocity_and_delivery",
         player_replication_packs_original_angles_half_velocity_and_delivery},
        {"paired_loopback_runs_admission_channels_typed_validation_and_delivery",
         paired_loopback_runs_admission_channels_typed_validation_and_delivery},
        {"legacy_random_and_atlas_match_original_contracts",
         legacy_random_and_atlas_match_original_contracts},
        {"world_generation_is_deterministic_and_layered",
         world_generation_is_deterministic_and_layered},
        {"day_night_cycle_preserves_original_timing", day_night_cycle_preserves_original_timing},
        {"terrain_vertices_carry_original_style_lighting",
         terrain_vertices_carry_original_style_lighting},
        {"directional_indirect_light_bakes_color_and_emissive_bounce",
         directional_indirect_light_bakes_color_and_emissive_bounce},
        {"player_physics_and_block_interaction_are_voxel_deterministic",
         player_physics_and_block_interaction_are_voxel_deterministic},
        {"inventory_matches_original_slot_and_stack_contracts",
         inventory_matches_original_slot_and_stack_contracts},
        {"voxel_lighting_flood_fills_and_never_leaks", voxel_lighting_flood_fills_and_never_leaks},
        {"inventory_moves_never_duplicate_or_refill_items",
         inventory_moves_never_duplicate_or_refill_items},
        {"gun_stats_and_crafting_match_the_original", gun_stats_and_crafting_match_the_original},
        {"enemy_roster_matches_the_original_tables", enemy_roster_matches_the_original_tables},
        {"dragon_attitude_and_spawn_heading_match_xna_flight",
         dragon_attitude_and_spawn_heading_match_xna_flight},
        {"enemy_network_identity_seed_and_target_authority_are_deterministic",
         enemy_network_identity_seed_and_target_authority_are_deterministic},
        {"modern_loot_blocks_match_owned_catalog_and_rng_transcripts",
         modern_loot_blocks_match_owned_catalog_and_rng_transcripts},
        {"modern_spawner_catalog_and_initialization_match_owned_reference_vectors",
         modern_spawner_catalog_and_initialization_match_owned_reference_vectors},
        {"modern_spawner_state_machine_preserves_light_waves_rewards_and_reset_quirks",
         modern_spawner_state_machine_preserves_light_waves_rewards_and_reset_quirks},
        {"enemy_horde_chases_attacks_and_dies", enemy_horde_chases_attacks_and_dies},
        {"dragon_reacquires_a_living_target_between_attack_runs",
         dragon_reacquires_a_living_target_between_attack_runs},
        {"dragon_departure_requires_sustained_target_loss",
         dragon_departure_requires_sustained_target_loss},
        {"enemy_animation_states_use_authored_clips_and_timing",
         enemy_animation_states_use_authored_clips_and_timing},
        {"enemy_hit_and_corpse_states_match_authored_timing",
         enemy_hit_and_corpse_states_match_authored_timing},
        {"enemy_melee_respects_the_locked_facing_cone",
         enemy_melee_respects_the_locked_facing_cone},
        {"enemy_digging_uses_block_hardness_fuses_and_door_ownership",
         enemy_digging_uses_block_hardness_fuses_and_door_ownership},
        {"attached_fixtures_and_door_halves_follow_support_removal",
         attached_fixtures_and_door_halves_follow_support_removal},
        {"modern_door_models_pieces_and_fallbacks_match_stock",
         modern_door_models_pieces_and_fallbacks_match_stock},
        {"world_save_round_trips_crates_doors_and_navigation_payloads",
         world_save_round_trips_crates_doors_and_navigation_payloads},
        {"rocket_explosion_matches_original_damage_and_protection",
         rocket_explosion_matches_original_damage_and_protection},
        {"world_start_matches_original_respawn_contract",
         world_start_matches_original_respawn_contract},
    };

    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
