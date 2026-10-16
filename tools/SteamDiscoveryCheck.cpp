#include "openminerz/net/legacy_steam_runtime_host.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// Explicitly opt-in: creates short-lived, password-protected public Steam lobbies.
// Never added to CTest; no gameplay admission, user saves, or identity output.
int main(int argc, char **argv)
{
    using namespace openminerz::net;
    using namespace std::chrono_literals;
    if (argc != 4 || std::string_view(argv[1]) != "--live")
    {
        std::cerr << "Usage: openminerz_steam_discovery_check --live <x86 helper> <owned steam_api.dll>\n";
        return 2;
    }
    try
    {
        SteamBridgeClientOptions bridge_options;
        bridge_options.helper_path = argv[2];
        bridge_options.steam_api_path = argv[3];
        for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
        {
            LegacySteamRuntimeHostOptions options;
            options.settings.protocol = protocol;
            options.settings.mod_manifest.protocol = protocol;
            options.settings.visibility = HostVisibility::public_lobby;
            options.settings.server_name = "OpenMinerZ discovery check";
            options.settings.password = "temporary-listing-check-no-gameplay";
            SteamBridgeHostConfig open;
            std::string error;
            if (!make_steam_bridge_host_config(options, open, error)) throw std::runtime_error(error);
            auto closed = open;
            closed.session_properties[1] = 1; // Reproduce the original regression.
            SteamBridgeClient host, browser;
            SteamBridgeHostReady ready;
            if (!host.start_host(bridge_options, closed, ready, error)) throw std::runtime_error(error);
            SteamBridgeDiscoveryRequest query;
            query.protocol = protocol;
            query.maximum_results = 50;
            SteamBridgeDiscoveryResults results;
            if (!browser.start_discovery(bridge_options, query, results, error)) throw std::runtime_error(error);
            const auto matching = [&]() {
                return std::find_if(results.lobbies.begin(), results.lobbies.end(),
                    [&](const auto &lobby) { return lobby.lobby_id == ready.lobby_id; });
            };
            if (matching() != results.lobbies.end())
                throw std::runtime_error("Original search filters exposed the closed control lobby");
            std::cout << protocol_label(protocol) << ": closed control excluded\n" << std::flush;
            if (!host.update_host(open, error)) throw std::runtime_error(error);
            const auto deadline = std::chrono::steady_clock::now() + 45s;
            bool found{};
            while (std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(500ms);
                if (!browser.refresh_discovery(query, results, error)) throw std::runtime_error(error);
                if (const auto lobby = matching(); lobby != results.lobbies.end())
                {
                    if (lobby->session_properties != open.session_properties ||
                        lobby->join_policy != SteamBridgeJoinPolicy::anyone || !lobby->password_protected)
                        throw std::runtime_error("Steam returned different host metadata");
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("Open lobby did not appear in the original filtered Steam query");
            std::cout << protocol_label(protocol) << ": open host found; properties match\n" << std::flush;
            if (!browser.stop(error) || !host.stop(error)) throw std::runtime_error(error);
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
