#include "app.hpp"
#include "openminerz/asset_paths.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/legacy_steam_runtime_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{

bool parse_size(std::string_view value, int &width, int &height)
{
    const auto separator = value.find_first_of("xX");
    if (separator == std::string_view::npos)
    {
        return false;
    }
    try
    {
        width = std::stoi(std::string{value.substr(0U, separator)});
        height = std::stoi(std::string{value.substr(separator + 1U)});
        return width > 0 && height > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool parse_state_region(std::string_view value, std::array<int, 2> &center)
{
    const auto separator = value.find(',');
    if (separator == std::string_view::npos || separator == 0U ||
        separator + 1U >= value.size() || value.find(',', separator + 1U) != std::string_view::npos)
        return false;
    const auto parse_coordinate = [](std::string_view token, int &coordinate) {
        try
        {
            std::size_t consumed{};
            const long long value = std::stoll(std::string{token}, &consumed, 10);
            constexpr long long maximum_world_coordinate = 1'000'000;
            if (consumed != token.size() || value < -maximum_world_coordinate ||
                value > maximum_world_coordinate)
                return false;
            coordinate = static_cast<int>(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    };
    return parse_coordinate(value.substr(0U, separator), center[0]) &&
           parse_coordinate(value.substr(separator + 1U), center[1]);
}

} // namespace

int main(int argc, char **argv)
{
    openminerz::LaunchOptions options;
    bool steam_probe{};
    std::optional<openminerz::net::ProtocolVersion> steam_probe_protocol;
    std::optional<std::uint64_t> steam_join_probe_lobby;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--settings")
        {
            options.open_settings = true;
        }
        else if (argument == "--host-setup")
        {
            options.open_host_settings = true;
        }
        else if (argument == "--host-details")
        {
            options.open_host_settings = true;
            options.host_settings_section = 1;
        }
        else if (argument == "--host-access")
        {
            options.open_host_settings = true;
            options.host_settings_section = 2;
        }
        else if (argument == "--host-rules")
        {
            options.open_host_settings = true;
            options.host_settings_section = 3;
        }
        else if (argument == "--join-browser")
        {
            options.open_join_browser = true;
        }
        else if (argument == "--join-details")
        {
            options.open_join_details = true;
        }
        else if (argument == "--ui-join-probe" && index + 1 < argc)
        {
            options.open_join_browser = true;
            try
            {
                options.ui_join_probe_lobby = std::stoull(argv[++index]);
            }
            catch (...)
            {
                std::cerr << "Invalid UI join probe lobby ID\n";
                return 2;
            }
        }
        else if (argument == "--diagnostic-join-browser")
        {
            options.diagnostic_join_browser = true;
        }
        else if (argument == "--diagnostic-join-details")
        {
            options.diagnostic_join_details = true;
        }
        else if (argument == "--diagnostic-online-loading")
        {
            options.diagnostic_online_loading = true;
        }
        else if (argument == "--diagnostic-multiplayer")
        {
            options.diagnostic_multiplayer = true;
            options.open_world = true;
        }
        else if (argument == "--steam-probe")
        {
            steam_probe = true;
        }
        else if (argument == "--steam-probe-classic")
        {
            steam_probe = true;
            steam_probe_protocol = openminerz::net::ProtocolVersion::classic_v3;
        }
        else if (argument == "--steam-probe-modern")
        {
            steam_probe = true;
            steam_probe_protocol = openminerz::net::ProtocolVersion::modern_v4;
        }
        else if (argument == "--steam-join-probe-modern" && index + 1 < argc)
        {
            steam_probe = true;
            steam_probe_protocol = openminerz::net::ProtocolVersion::modern_v4;
            try
            {
                steam_join_probe_lobby = std::stoull(argv[++index]);
            }
            catch (...)
            {
                std::cerr << "Invalid Steam lobby ID\n";
                return 2;
            }
        }
        else if (argument == "--world")
        {
            options.open_world = true;
        }
        else if (argument == "--world-list")
        {
            options.open_world_select = true;
        }
        else if (argument == "--inventory")
        {
            options.open_world = true;
            options.open_inventory = true;
        }
        else if (argument == "--no-enemies")
        {
            options.open_world = true;
            options.no_enemies = true;
        }
        else if (argument == "--lightning")
        {
            options.open_world = true;
            options.force_lightning = true;
        }
        else if (argument == "--time" && index + 1 < argc)
        {
            try
            {
                const float hour = std::stof(argv[++index]);
                if (hour < 0.0F || hour >= 24.0F)
                {
                    throw std::out_of_range{"hour"};
                }
                options.world_time = hour / 24.0F;
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --time value; expected an hour in [0, 24)\n";
                return 2;
            }
        }
        else if (argument == "--biome" && index + 1 < argc)
        {
            const std::string_view biome{argv[++index]};
            constexpr std::string_view names[]{"classic",  "lagoon", "desert",
                                               "mountain", "arctic", "descent"};
            const auto iterator = std::find(std::begin(names), std::end(names), biome);
            if (iterator == std::end(names))
            {
                std::cerr << "Invalid --biome value\n";
                return 2;
            }
            options.world_biome = static_cast<int>(iterator - std::begin(names));
            options.open_world = true;
        }
        else if (argument == "--tonemap" && index + 1 < argc)
        {
            const std::string_view curve{argv[++index]};
            constexpr std::string_view names[]{"legacy", "reinhard", "filmic", "aces"};
            const auto iterator = std::find(std::begin(names), std::end(names), curve);
            if (iterator == std::end(names))
            {
                std::cerr << "Invalid --tonemap value\n";
                return 2;
            }
            options.tone_mapping = static_cast<int>(iterator - std::begin(names));
        }
        else if (argument == "--view-distance" && index + 1 < argc)
        {
            try
            {
                options.view_distance = std::stoi(argv[++index]);
                if (options.view_distance < 4 || options.view_distance > 32)
                {
                    throw std::out_of_range{"view distance"};
                }
            }
            catch (...)
            {
                std::cerr << "Invalid --view-distance value; expected an integer in [4, 32]\n";
                return 2;
            }
        }
        else if (argument == "--pitch" && index + 1 < argc)
        {
            try
            {
                const float degrees = std::stof(argv[++index]);
                if (degrees < -85.0F || degrees > 85.0F)
                {
                    throw std::out_of_range{"pitch"};
                }
                constexpr float radians_per_degree = 0.01745329251994329577F;
                options.world_pitch = degrees * radians_per_degree;
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --pitch value; expected degrees in [-85, 85]\n";
                return 2;
            }
        }
        else if (argument == "--yaw" && index + 1 < argc)
        {
            try
            {
                const float degrees = std::stof(argv[++index]);
                constexpr float radians_per_degree = 0.01745329251994329577F;
                options.world_yaw = degrees * radians_per_degree;
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --yaw value; expected degrees\n";
                return 2;
            }
        }
        else if (argument == "--environment" && index + 1 < argc)
        {
            const std::string value{argv[++index]};
            constexpr std::string_view names[]{"surface", "water",  "cave",   "hell",
                                               "lava",    "crater", "foliage"};
            if (std::find(std::begin(names), std::end(names), value) == std::end(names))
            {
                std::cerr << "Invalid --environment value; expected surface, water, cave, hell, "
                             "lava, crater, or foliage\n";
                return 2;
            }
            options.preview_environment = value;
            if (value == "water")
                options.world_biome = 1; // Lagoon is BlockTerrain.IsWaterWorld.
            options.open_world = true;
        }
        else if (argument == "--item" && index + 1 < argc)
        {
            try
            {
                options.preview_item = std::stoi(argv[++index]);
                if (*options.preview_item < 0 || *options.preview_item >= 132)
                    throw std::out_of_range{"item"};
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --item value; expected an original item id in [0, 131]\n";
                return 2;
            }
        }
        else if (argument == "--pickup-item" && index + 1 < argc)
        {
            try
            {
                options.preview_pickup_item = std::stoi(argv[++index]);
                if (*options.preview_pickup_item < 1 || *options.preview_pickup_item >= 132)
                    throw std::out_of_range{"pickup item"};
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr
                    << "Invalid --pickup-item value; expected an original item id in [1, 131]\n";
                return 2;
            }
        }
        else if (argument == "--viewmodel" && index + 1 < argc)
        {
            const std::string value{argv[++index]};
            constexpr std::string_view names[]{"idle",       "use",  "aim",      "aim-idle",
                                               "unshoulder", "fire", "aim-fire", "reload"};
            if (std::find(std::begin(names), std::end(names), value) == std::end(names))
            {
                std::cerr << "Invalid --viewmodel state\n";
                return 2;
            }
            options.preview_viewmodel = value;
            options.open_world = true;
        }
        else if (argument == "--viewmodel-progress" && index + 1 < argc)
        {
            try
            {
                options.preview_viewmodel_progress = std::stof(argv[++index]);
                if (options.preview_viewmodel_progress < 0.0F ||
                    options.preview_viewmodel_progress > 1.0F)
                    throw std::out_of_range{"viewmodel progress"};
            }
            catch (...)
            {
                std::cerr << "Invalid --viewmodel-progress value; expected [0, 1]\n";
                return 2;
            }
        }
        else if (argument == "--size" && index + 1 < argc)
        {
            if (!parse_size(argv[++index], options.width, options.height))
            {
                std::cerr << "Invalid --size value; expected WIDTHxHEIGHT\n";
                return 2;
            }
        }
        else if (argument == "--screenshot" && index + 1 < argc)
        {
            options.screenshot_path = argv[++index];
        }
        else if (argument == "--network-trace" && index + 1 < argc)
        {
            options.network_trace_path = argv[++index];
            if (options.network_trace_path->empty())
            {
                std::cerr << "Invalid --network-trace path\n";
                return 2;
            }
        }
        else if (argument == "--state-dump" && index + 1 < argc)
        {
            options.state_dump_path = argv[++index];
            if (options.state_dump_path->empty())
            {
                std::cerr << "Invalid --state-dump path\n";
                return 2;
            }
        }
        else if (argument == "--state-region" && index + 1 < argc)
        {
            std::array<int, 2> center{};
            if (!parse_state_region(argv[++index], center))
            {
                std::cerr << "Invalid --state-region value; expected X,Z in [-1000000, 1000000]\n";
                return 2;
            }
            options.state_region_center = center;
        }
        else if (argument == "--craft-style" && index + 1 < argc)
        {
            const std::string_view style{argv[++index]};
            if (style != "classic" && style != "modern")
            {
                std::cerr << "Invalid --craft-style value; expected classic or modern\n";
                return 2;
            }
            options.craft_modern = style == "modern";
        }
        else if (argument == "--texture-style" && index + 1 < argc)
        {
            const std::string_view style{argv[++index]};
            if (style != "classic" && style != "modern")
            {
                std::cerr << "Invalid --texture-style value; expected classic or modern\n";
                return 2;
            }
            options.asset_style = style == "modern" ? 1 : 0;
        }
        else if (argument == "--presentation" && index + 1 < argc)
        {
            const std::string_view profile{argv[++index]};
            if (profile != "classic" && profile != "rebirth")
            {
                std::cerr << "Invalid --presentation value; expected classic or rebirth\n";
                return 2;
            }
            options.presentation_profile = profile == "rebirth" ? 1 : 0;
        }
        else if (argument == "--enemy" && index + 1 < argc)
        {
            try
            {
                options.spawn_enemy = std::stoi(argv[++index]);
                if (options.spawn_enemy < 0 || options.spawn_enemy > 51)
                    throw std::out_of_range{"enemy"};
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --enemy value; expected an enemy type in [0, 51]\n";
                return 2;
            }
        }
        else if (argument == "--dragon" && index + 1 < argc)
        {
            try
            {
                options.spawn_dragon = std::stoi(argv[++index]);
                if (options.spawn_dragon < 0 || options.spawn_dragon > 4)
                    throw std::out_of_range{"dragon"};
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --dragon value; expected a dragon kind in [0, 4]\n";
                return 2;
            }
        }
        else if (argument == "--bench" && index + 1 < argc)
        {
            try
            {
                options.bench_frames = std::stoi(argv[++index]);
                if (options.bench_frames < 10 || options.bench_frames > 100000)
                    throw std::out_of_range{"bench"};
                options.open_world = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --bench value; expected a frame count in [10, 100000]\n";
                return 2;
            }
        }
        else if (argument == "--help")
        {
            std::cout
                << "OpenMinerZ frontend\n"
                   "  --size WIDTHxHEIGHT   Override initial window size\n"
                   "  --settings            Open the settings screen\n"
                   "  --host-setup          Open multiplayer host preflight\n"
                   "  --host-details        Open host identity and capacity settings\n"
                   "  --host-access         Open host access and privacy settings\n"
                   "  --host-rules          Open host gameplay and safety settings\n"
                   "  --join-browser        Open the live Steam multiplayer browser\n"
                   "  --join-details        Open live Steam discovery and select the first result\n"
                   "  --ui-join-probe ID    Drive the real menu join path for one live lobby\n"
                   "  --diagnostic-join-browser  Open deterministic browser fixtures\n"
                   "  --diagnostic-join-details  Open deterministic session details\n"
                   "  --diagnostic-online-loading  Preview authoritative join progress\n"
                   "  --steam-probe         Authenticate through staged steam_api.dll and exit\n"
                   "  --steam-probe-classic Query only original Classic v3 lobbies\n"
                   "  --steam-probe-modern  Query only Modern v4 lobbies\n"
                   "  --steam-join-probe-modern ID  Trace Modern admission for one lobby\n"
                   "  --world               Open the generated world preview\n"
                   "  --world-list          Open the saved-world card browser\n"
                   "  --inventory           Open the world with the inventory visible\n"
                   "  --no-enemies          Disable hostile spawns in a world preview\n"
                   "  --lightning           Force the sky-only lightning frame for capture\n"
                   "  --time HOUR           Fix preview time to an hour in [0, 24)\n"
                   "  --biome NAME          classic|lagoon|desert|mountain|arctic|descent\n"
                   "  --view-distance N     Override terrain visibility setting (4..32)\n"
                   "  --tonemap CURVE       legacy|reinhard|filmic|aces\n"
                   "  --pitch DEGREES       Fix preview camera pitch in [-85, 85]\n"
                   "  --environment NAME    surface|water|cave|hell|lava|crater|foliage diagnostic "
                   "spawn\n"
                   "  --item ID             Equip an original item id (0..131) for capture\n"
                   "  --viewmodel STATE     idle|use|aim|aim-idle|unshoulder|fire|aim-fire|reload\n"
                   "  --viewmodel-progress N  Fix the selected viewmodel clip at [0, 1]\n"
                   "  --texture-style NAME  classic|modern startup content-pack override\n"
                   "  --presentation NAME   classic|rebirth render-profile override\n"
                    "  --screenshot PATH     Render two frames, save BMP, and exit\n"
                    "  --pickup-item ID      Place one dropped-item visual for capture\n"
                    "  --network-trace PATH  Append redacted protocol JSONL for interop testing\n"
                    "  --state-dump PATH     Write a redacted final interop state snapshot\n"
                    "  --state-region X,Z   Use one exact 33x128x33 world-space state region\n"
                    "  --bench N             Render N world frames, print timings, exit\n";
            return 0;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }

    try
    {
        if (steam_probe)
        {
            const std::filesystem::path steam_api = openminerz::installed_steam_api_path();
            if (steam_api.empty())
            {
                std::cerr << "steam.probe.error=owned x86 steam_api.dll was not found\n";
                return 1;
            }
            openminerz::net::LegacySteamRuntimeClientOptions probe_options;
            probe_options.discovery_protocol = steam_probe_protocol;
            probe_options.helper_path = openminerz::application_directory() / "SteamBridge" /
                                        "OpenMinerZ.SteamBridge32.exe";
            probe_options.steam_api_path = steam_api;
            probe_options.trace_path = options.network_trace_path;
            openminerz::net::LegacySteamRuntimeClient probe;
            openminerz::net::SteamBridgeDiscoveryResults discovery;
            std::string error;
            if (!probe.start_browser(probe_options, discovery, error))
            {
                std::cerr << "steam.probe.error=" << error << '\n';
                return 1;
            }
            std::cout << "steam.probe.loaded=" << steam_api.string() << '\n'
                      << "steam.probe.persona=" << discovery.persona_name << '\n'
                      << "steam.probe.lobbies=" << discovery.lobbies.size() << '\n'
                      << "steam.probe.reported=" << discovery.reported_lobby_count << '\n'
                      << "steam.probe.rejected=" << discovery.rejected_lobby_count << '\n'
                      << "steam.probe.rejected_native=" << discovery.rejected_native_count << '\n'
                      << "steam.probe.rejected_metadata=" << discovery.rejected_metadata_count
                      << '\n'
                      << "steam.probe.rejected_contract=" << discovery.rejected_contract_count
                      << '\n'
                      << "steam.probe.diagnostic=" << discovery.diagnostic << '\n';
            for (const auto &lobby : discovery.lobbies)
                std::cout << "steam.probe.lobby=" << lobby.lobby_id << " name=" << lobby.name
                          << " host=" << lobby.host_name
                          << " protocol=" << lobby.session_properties[0].value_or(-1)
                          << " players=" << static_cast<int>(lobby.current_players) << '/'
                          << static_cast<int>(lobby.maximum_players) << '\n';
            if (steam_join_probe_lobby.has_value())
            {
                openminerz::net::ModManifest manifest;
                manifest.protocol = openminerz::net::ProtocolVersion::modern_v4;
                openminerz::net::SteamBridgeJoinReady ready;
                if (!probe.join(*steam_join_probe_lobby,
                                openminerz::net::ProtocolVersion::modern_v4, manifest, {}, ready,
                                error))
                {
                    std::cerr << "steam.join.error=" << error << '\n';
                    return 1;
                }
                std::cout << "steam.join.ready host_peer=" << ready.lobby.host_peer
                          << " host_name=" << ready.lobby.host_name << '\n';
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{12};
                while (std::chrono::steady_clock::now() < deadline && !probe.connected())
                {
                    std::vector<openminerz::net::LegacySteamRuntimeClientEvent> events;
                    if (!probe.pump(events, error))
                    {
                        std::cerr << "steam.join.pump_error=" << error << '\n';
                        return 1;
                    }
                    for (const auto &event : events)
                        std::cout << "steam.join.event=" << static_cast<int>(event.kind)
                                  << " gid=" << static_cast<int>(event.gid)
                                  << " detail=" << event.detail << '\n';
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                std::cout << "steam.join.connected=" << probe.connected() << '\n';
                if (probe.connected())
                {
                    const auto local_gid = probe.local_gid();
                    const auto *local_gamer = probe.local_gamer();
                    if (!local_gid.has_value() || local_gamer == nullptr)
                    {
                        std::cerr << "steam.sync.error=admission omitted local identity\n";
                        return 1;
                    }
                    openminerz::net::AuthoritativeBootstrapClientOptions bootstrap_options;
                    bootstrap_options.protocol = openminerz::net::ProtocolVersion::modern_v4;
                    bootstrap_options.local_gid = *local_gid;
                    bootstrap_options.avatar_description.assign(10U, 0U);
                    bootstrap_options.gamer_tag = local_gamer->gamer_tag;
                    bootstrap_options.player_id = local_gamer->player_id;
                    openminerz::net::AuthoritativeBootstrapClient bootstrap{bootstrap_options};
                    const auto send_bootstrap =
                        [&](std::vector<openminerz::net::AuthoritativeBootstrapClientSend>
                                pending) {
                            for (std::size_t index{}; index < pending.size(); ++index)
                            {
                                const auto &send = pending[index];
                                if (!probe.send(send.recipient_gid, send.application_packet,
                                                send.delivery, error))
                                    return false;
                                openminerz::net::DecodedPacket decoded;
                                openminerz::net::PacketError packet_error{};
                                if (!openminerz::net::decode_packet(
                                        openminerz::net::ProtocolVersion::modern_v4,
                                        send.application_packet, decoded, packet_error) ||
                                    decoded.message == nullptr)
                                {
                                    error = "bootstrap local-echo decode failed";
                                    return false;
                                }
                                if (send.recipient_gid.has_value() ||
                                    decoded.message->name != "PlayerExistsMessage")
                                    continue;
                                auto echoed =
                                    bootstrap.handle_packet(*local_gid, send.application_packet);
                                if (!echoed.handled())
                                {
                                    error = std::string{"bootstrap local echo failed on "} +
                                            std::string{echoed.input_message};
                                    return false;
                                }
                                std::cout << "steam.sync.local_echo=" << echoed.input_message
                                          << '\n';
                                pending.insert(pending.end(), echoed.outbound.begin(),
                                               echoed.outbound.end());
                            }
                            return true;
                        };
                    openminerz::net::BootstrapClientError bootstrap_error{};
                    const auto request = bootstrap.begin(bootstrap_error);
                    if (!request.has_value() || !send_bootstrap({*request}))
                    {
                        std::cerr << "steam.sync.error="
                                  << (error.empty() ? "could not begin world request" : error)
                                  << '\n';
                        return 1;
                    }
                    const auto sync_deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds{30};
                    while (std::chrono::steady_clock::now() < sync_deadline &&
                           !bootstrap.complete())
                    {
                        std::vector<openminerz::net::LegacySteamRuntimeClientEvent> events;
                        if (!probe.pump(events, error))
                        {
                            std::cerr << "steam.sync.pump_error=" << error << '\n';
                            return 1;
                        }
                        for (const auto &event : events)
                        {
                            if (event.kind !=
                                openminerz::net::LegacySteamRuntimeClientEventKind::gameplay)
                                continue;
                            auto handled = bootstrap.handle_packet(event.gid, event.payload);
                            if (handled.error ==
                                openminerz::net::BootstrapClientError::unexpected_message)
                                continue;
                            if (!handled.handled())
                            {
                                std::cerr << "steam.sync.error=bootstrap contract "
                                          << static_cast<int>(handled.error) << " on "
                                          << handled.input_message << '\n';
                                return 1;
                            }
                            std::cout << "steam.sync.message=" << handled.input_message << '\n';
                            if (!send_bootstrap(std::move(handled.outbound)))
                            {
                                std::cerr << "steam.sync.send_error=" << error << '\n';
                                return 1;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    }
                    const auto &world = bootstrap.world_save();
                    std::cout << "steam.sync.complete=" << bootstrap.complete() << '\n';
                    if (world.has_value())
                        std::cout << "steam.sync.world seed=" << world->seed
                                  << " edits=" << world->edits.size()
                                  << " crates=" << world->crates.size() << '\n';
                }
            }
            if (!probe.stop(error))
            {
                std::cerr << "steam.probe.stop_error=" << error << '\n';
                return 1;
            }
            return 0;
        }
        openminerz::Application application{std::move(options)};
        return application.run();
    }
    catch (const std::exception &error)
    {
        std::cerr << "OpenMinerZ failed: " << error.what() << '\n';
        return 1;
    }
}
