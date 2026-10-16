cmake_minimum_required(VERSION 3.25)

project(OpenMinerZ VERSION 0.1.0 LANGUAGES CXX)

option(OPENMINERZ_BUILD_TESTS "Build OpenMinerZ tests" ON)
option(OPENMINERZ_BUILD_INTEROP_LAB
    "Build isolated original-client interoperability tools (never required by the game)" OFF)
option(OPENMINERZ_IMPORT_OWNED_CONTENT
    "Import locally owned CastleMiner Z content into the build tree" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Freetype CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
# Modding runtime area: the Luau script VM and compiler (vcpkg "luau").
find_package(unofficial-luau CONFIG REQUIRED)

set(OPENMINERZ_CLASSIC_GAME_DIR
    ""
    CACHE PATH "Path to the Classic CastleMiner Z build")
set(OPENMINERZ_MODERN_GAME_DIR
    ""
    CACHE PATH "Path to the Modern CastleMiner Z build")
set(OPENMINERZ_DEFAULT_STEAM_API_DLL "")
if(OPENMINERZ_MODERN_GAME_DIR)
    set(OPENMINERZ_DEFAULT_STEAM_API_DLL
        "${OPENMINERZ_MODERN_GAME_DIR}/steam_api.dll")
endif()
set(OPENMINERZ_STEAM_API_DLL
    "${OPENMINERZ_DEFAULT_STEAM_API_DLL}"
    CACHE FILEPATH "Owned PE32 CastleMiner Z Steam API DLL for local interoperability tools")
set(OPENMINERZ_AVATAR_ASSET
    "$ENV{LOCALAPPDATA}/OpenMinerZ/DerivedData/avatar.omavatar"
    CACHE FILEPATH "Optional locally exported XNA avatar shell used by both content packs")

add_library(openminerz_core
    src/atomic_file.cpp
    src/design_canvas.cpp
    src/frame_pacing.cpp
    src/hud_layout.cpp
    src/inventory.cpp
    src/player_progress.cpp
    src/remastered_layout.cpp
    src/retail_text.cpp
    src/screens.cpp
    src/sprite_font.cpp
    src/interop_state.cpp
    src/menu_look.cpp
    src/menu_model.cpp
    src/net/admission.cpp
    src/net/authoritative_bootstrap.cpp
    src/net/avatar_sync.cpp
    src/net/bootstrap_codec.cpp
    src/net/client_gameplay_authority.cpp
    src/net/combat_codec.cpp
    src/net/dragon_codec.cpp
    src/net/enemy_codec.cpp
    src/net/host_session.cpp
    src/net/legacy_steam_codec.cpp
    src/net/legacy_steam_client_session.cpp
    src/net/legacy_steam_host_session.cpp
    src/net/legacy_steam_runtime_client.cpp
    src/net/legacy_steam_runtime_host.cpp
    src/net/loopback_session.cpp
    src/net/misc_codec.cpp
    src/net/mod_catalog.cpp
    src/net/mod_handshake.cpp
    src/net/payload_codec.cpp
    src/net/peer_inventory_store.cpp
    src/net/inventory_save_barrier.cpp
    src/net/voice_codec.cpp
    src/net/pickup_codec.cpp
    src/net/player_replication.cpp
    src/net/protocol.cpp
    src/net/protocol_trace.cpp
    src/net/session_browser.cpp
    # Authority: the host decides health, damage, items, edits and movement in
    # sessions only OpenMinerZ players can join (net/session_authority.hpp).
    src/net/session_authority.cpp
    src/net/steam_bridge_client.cpp
    src/net/steam_bridge_protocol.cpp
    src/net/transport.cpp
    src/retail_character_driver.cpp
    src/retail_rig.cpp
    src/graphics_settings.cpp
    src/settings.cpp
    src/settings_menu.cpp
    src/world_save.cpp
    src/world/block.cpp
    src/world/dynamic_radiance.cpp
    src/world/dragon_brain.cpp
    src/world/enemy.cpp
    src/world/enemy_simulation.cpp
    src/world/encounters.cpp
    src/world/explosive.cpp
    src/world/far_terrain.cpp
    src/world/legacy_noise.cpp
    src/world/loot.cpp
    src/world/movement_probe.cpp
    src/world/player.cpp
    src/world/player_animation.cpp
    src/world/sky.cpp
    src/world/spawner.cpp
    src/world/world.cpp
    # mod-world: mod world generators, evaluated natively (custom_terrain.hpp).
    src/world/custom_terrain.cpp
    # mod-liquid: mod liquid blocks - collision, swimming, meshing (liquid.hpp).
    src/world/liquid.cpp
    src/xbox_avatar.cpp
    # The Enhanced presentation's effects layer (openminerz/vfx.hpp): the
    # simulation and the recipes are pure CPU and tested without a device.
    src/vfx/vfx_library.cpp
    src/vfx/vfx_system.cpp
    # Weapon, dragon, mining and creature recipes on it (openminerz/vfx_combat.hpp).
    src/vfx/vfx_library_combat.cpp
    src/vfx/vfx_combat_blocks.cpp
    # The modding API (openminerz/modding/api.hpp, docs/development.md).
    src/modding/api.cpp
    # Modding runtime area: Luau VMs, sandboxed mod environments, lifecycle,
    # core script libraries and mod packages (modding/runtime.hpp, package.hpp).
    src/modding/runtime/runtime.cpp
    src/modding/runtime/core_api.cpp
    src/modding/runtime/serialize.cpp
    src/modding/runtime/package.cpp
    # Add-Ons: installed mods, which are on, conflicts (modding/addons.hpp).
    src/modding/runtime/addons.cpp
    # Modding network area: mod channel framing, Value codec, RPC/replication
    # sessions, script bindings, client-visible mod download (modding/net.hpp).
    src/net/mod_channel.cpp
    src/net/mod_download.cpp
    src/modding/net/value_codec.cpp
    src/modding/net/mod_net.cpp
    src/modding/net/net_api.cpp
    # Modding UI area: themes, widgets, screens, HUD, story screens, menu rows,
    # key bindings and localization (modding/ui.hpp). Pure C++; the app draws.
    src/modding/ui/api.cpp
    src/modding/ui/bindings.cpp
    src/modding/ui/net_channel.cpp
    src/modding/ui/service.cpp
    src/modding/ui/text.cpp
    src/modding/ui/theme.cpp
    src/modding/ui/widgets.cpp
    # Modding gameplay area: world edits, structures, world generation, mod
    # blocks/items/recipes/loot, creatures and NPCs, stats, effects, quests,
    # dialogue and difficulty scaling (modding/gameplay.hpp). Pure C++.
    src/modding/gameplay/api.cpp
    # Integration: mod item identity in crates and network pickups.
    src/modding/gameplay/containers.cpp
    src/modding/gameplay/entities.cpp
    # (Items) held mod items, hold styles and mod weapons.
    src/modding/gameplay/items.cpp
    src/modding/gameplay/net_sync.cpp
    src/modding/gameplay/persistence.cpp
    src/modding/gameplay/quests.cpp
    src/modding/gameplay/registry.cpp
    src/modding/gameplay/retail_names.cpp
    src/modding/gameplay/service.cpp
    src/modding/gameplay/world_edits.cpp
    # mod-liquid: liquid mod blocks as the server sees them.
    src/modding/gameplay/liquids.cpp
    # mod-ai: creature AI (brains, senses, teams, attacks, movement, bosses),
    # projectiles and the session's retail enemy rules.
    src/modding/gameplay/ai.cpp
    src/modding/gameplay/ai_api.cpp
    src/modding/gameplay/ai_navigation.cpp
    src/modding/gameplay/ai_projectiles.cpp
    src/modding/gameplay/ai_retail.cpp
    # mod-world: the `terrain` and `noise` libraries and the world's generator
    # record (modding/worldgen.hpp).
    src/modding/worldgen/service.cpp
)
target_include_directories(openminerz_core PUBLIC include)
target_compile_features(openminerz_core PUBLIC cxx_std_20)
# Modding runtime area: scripts compile and run inside the core library; only
# src/modding/runtime/ includes Luau headers.
target_link_libraries(openminerz_core PRIVATE unofficial::luau::Luau.VM
                                              unofficial::luau::Luau.Compiler)

# Modding graphics area: asset packs, glTF models, animation packs, lighting
# props, atmosphere layers, mod sounds and camera effects
# (openminerz/modding/graphics.hpp). A library of its own so the stb and cgltf
# implementations stay private to it.
find_path(OPENMINERZ_STB_INCLUDE_DIR stb_image.h REQUIRED)
find_path(OPENMINERZ_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
add_library(openminerz_mod_graphics STATIC
    # (Animation packs) the authoring frame, export-rig and check-anim.
    src/modding/graphics/animation_authoring.cpp
    src/modding/graphics/animation_pack.cpp
    src/modding/graphics/assets.cpp
    src/modding/graphics/atmosphere.cpp
    src/modding/graphics/audio.cpp
    # Visuals (modding wave 2): mod blocks on the terrain, creature animation.
    src/modding/graphics/block_overlay.cpp
    src/modding/graphics/camera.cpp
    src/modding/graphics/creatures.cpp
    src/modding/graphics/effects.cpp
    # (GunFX) weapon parts, animation cues and procedural viewmodel motion.
    src/modding/graphics/gunfx.cpp
    # (Items) held and dropped mod items: grips, hold styles, item clips.
    src/modding/graphics/held_item.cpp
    src/modding/graphics/lights.cpp
    src/modding/graphics/models.cpp
    src/modding/graphics/service.cpp
)
target_link_libraries(openminerz_mod_graphics PUBLIC openminerz_core)
target_include_directories(openminerz_mod_graphics PRIVATE
    "${OPENMINERZ_STB_INCLUDE_DIR}" "${OPENMINERZ_CGLTF_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(openminerz_mod_graphics PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(openminerz_mod_graphics PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Modding SDK area: every area's script API registered as the game does, on
# stand-in backends (tools/ModSdk/sdk_session.hpp); the generated API
# reference and Luau types; mod validation; the openminerz_mod_sdk tool.
add_library(openminerz_mod_sdk_lib STATIC
    tools/ModSdk/reference.cpp
    tools/ModSdk/sdk_session.cpp
    tools/ModSdk/templates.cpp
    tools/ModSdk/typecheck.cpp
    tools/ModSdk/validate.cpp
    # mod-world: generator checks for validate, and preview-map.
    tools/ModSdk/world_tools.cpp
    # mod-ai: creature, brain and projectile checks; `run --creatures`.
    tools/ModSdk/ai_tools.cpp
)
target_include_directories(openminerz_mod_sdk_lib PUBLIC tools/ModSdk)
target_link_libraries(openminerz_mod_sdk_lib PUBLIC openminerz_mod_graphics
                                                    unofficial::luau::Luau.Analysis
                                                    unofficial::luau::Luau.Ast
                                                    unofficial::luau::Luau.Compiler)
add_executable(openminerz_mod_sdk tools/ModSdk/main.cpp
    # (Animation packs) export-rig and check-anim read the imported character
    # where the game does.
    tools/ModSdk/anim_commands.cpp tools/ModSdk/rig_lab.cpp src/asset_paths.cpp)
target_link_libraries(openminerz_mod_sdk PRIVATE openminerz_mod_sdk_lib)
if(MSVC)
    target_compile_options(openminerz_mod_sdk_lib PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(openminerz_mod_sdk PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(openminerz_mod_sdk_lib PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(openminerz_mod_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(openminerz
    src/app.cpp
    src/asset_paths.cpp
    src/avatar_viewmodel.cpp
    src/depth_voxel_renderer.cpp
    src/enemy_renderer.cpp
    src/far_terrain_renderer.cpp
    src/font_renderer.cpp
    src/hell_atmosphere.cpp
    src/main.cpp
    src/original_sound_bank.cpp
    src/post_processing.cpp
    src/present_control.cpp
    src/voice_chat_audio.cpp
    src/static_model_asset.cpp
    src/vfx/vfx_renderer.cpp
    src/world_preview_renderer.cpp
    src/world_preview_vfx.cpp
    src/world_preview_combat_vfx.cpp
    src/xbox_avatar_addon.cpp
    # Modding graphics area: the renderer, audio and screen side of
    # openminerz/modding/graphics.hpp (mod lights, models, packs, LUT, sound).
    src/modding/graphics/mod_light_gpu.cpp
    src/modding/graphics/mod_output.cpp
    src/modding/graphics/mod_surfaces.cpp
    src/modding/graphics/world_preview_creatures.cpp
    src/modding/graphics/world_preview_mod.cpp
    # mod-world: the world renderer's side of mod world generators.
    src/modding/worldgen/world_preview_terrain.cpp
    # mod-liquid: the world renderer's side of liquid mod blocks.
    src/world_preview_liquid.cpp
)
target_include_directories(openminerz PRIVATE include src)
# Modding graphics area.
target_link_libraries(openminerz PRIVATE openminerz_mod_graphics)
target_link_libraries(openminerz PRIVATE openminerz_core Freetype::Freetype SDL3::SDL3)
if(OPENMINERZ_MODERN_GAME_DIR)
    target_compile_definitions(openminerz PRIVATE
        OPENMINERZ_OWNED_GAME_DIR=L"${OPENMINERZ_MODERN_GAME_DIR}")
endif()

if(MSVC)
    target_compile_options(openminerz_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(openminerz PRIVATE /W4 /permissive- /EHsc)
    # app.cpp with every modding area's .inc exceeds the default COFF section
    # limit (C1128). (Added by the UI area.)
    target_compile_options(openminerz PRIVATE /bigobj)
else()
    target_compile_options(openminerz_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(openminerz PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Add-Ons: the example mods ship beside the game in examples/mods, where the
# Add-Ons screen lists them, off until the player turns one on
# (modding/addons.hpp). They are original content made by tools/ModSdk.
# Copied afresh on every build, so a file removed from an example does not
# linger in the copy.
add_custom_target(openminerz_bundled_mods ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:openminerz>/examples/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/mods"
        "$<TARGET_FILE_DIR:openminerz>/examples/mods"
    COMMENT "Copying the example mods beside the game (off until turned on)"
    VERBATIM)
add_dependencies(openminerz openminerz_bundled_mods)

if(WIN32 AND OPENMINERZ_BUILD_INTEROP_LAB)
    add_executable(openminerz_interop_launcher WIN32
        tools/OriginalClientInterop/original_client_interop.cpp)
    target_compile_features(openminerz_interop_launcher PRIVATE cxx_std_20)
    target_link_libraries(openminerz_interop_launcher PRIVATE advapi32 gdi32 user32)
    if(MSVC)
        target_compile_options(openminerz_interop_launcher PRIVATE /W4 /permissive- /EHsc)
    endif()
    set_target_properties(openminerz_interop_launcher PROPERTIES
        OUTPUT_NAME OpenMinerZInteropLauncher
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z")

    add_executable(openminerz_interop_supervisor_fixture WIN32
        tools/OriginalClientInterop/supervisor_fixture.cpp)
    target_compile_features(openminerz_interop_supervisor_fixture PRIVATE cxx_std_20)
    target_link_libraries(openminerz_interop_supervisor_fixture PRIVATE gdi32 user32)
    if(MSVC)
        target_compile_options(openminerz_interop_supervisor_fixture PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    set(OPENMINERZ_MANAGED_INTEROP_ROOT "${CMAKE_SOURCE_DIR}/out/interop-managed")
    set(OPENMINERZ_MANAGED_STATE_HOST
        "${OPENMINERZ_MANAGED_INTEROP_ROOT}/Host/OpenMinerZ.OriginalStateHost.exe")
    set(OPENMINERZ_MANAGED_STATE_FIXTURE
        "${OPENMINERZ_MANAGED_INTEROP_ROOT}/Fixture/FixtureCastleMinerZ.exe")
    add_custom_target(openminerz_interop_managed ALL
        COMMAND cmd.exe /d /c
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ManagedStateHost/build_managed_state_host.cmd"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENMINERZ_MANAGED_STATE_HOST}"
            "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z/OpenMinerZ.OriginalStateHost.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENMINERZ_MANAGED_STATE_HOST}.config"
            "${CMAKE_SOURCE_DIR}/out/interop-lab/CastleMiner Z/OpenMinerZ.OriginalStateHost.exe.config"
        BYPRODUCTS
            "${OPENMINERZ_MANAGED_STATE_HOST}"
            "${OPENMINERZ_MANAGED_STATE_HOST}.config"
            "${OPENMINERZ_MANAGED_STATE_FIXTURE}"
            "${OPENMINERZ_MANAGED_STATE_FIXTURE}.config"
        SOURCES
            tools/OriginalClientInterop/ManagedStateHost/OpenMinerZ.OriginalStateHost.csproj
            tools/OriginalClientInterop/ManagedStateHost/Program.cs
            tools/OriginalClientInterop/ManagedStateHost/State-Control.ps1
            tools/OriginalClientInterop/ManagedStateHost/Test-ManagedStateHost.ps1
            tools/OriginalClientInterop/ManagedStateHost/Test-SupervisorState.ps1
            tools/OriginalClientInterop/ManagedStateHost/Test-ContractProbe.ps1
            tools/OriginalClientInterop/ManagedStateHost/Fixture/FixtureCastleMinerZ.csproj
            tools/OriginalClientInterop/ManagedStateHost/Fixture/Program.cs
            tools/OriginalClientInterop/ManagedStateHost/build_managed_state_host.cmd
        COMMENT "Building the isolated x86 original-state host and fixture"
        VERBATIM)
    add_dependencies(openminerz_interop_launcher openminerz_interop_managed)

endif()

if(WIN32)
    target_link_libraries(openminerz_core PUBLIC advapi32 ole32)
    target_link_libraries(openminerz PRIVATE d3d11 d3dcompiler)

    set(OPENMINERZ_STEAM_BRIDGE_EXE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/steam-bridge32/OpenMinerZ.SteamBridge32.exe")
    add_custom_target(openminerz_steam_bridge ALL
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/build_bridge.cmd"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:openminerz>/SteamBridge"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENMINERZ_STEAM_BRIDGE_EXE}"
            "$<TARGET_FILE_DIR:openminerz>/SteamBridge/OpenMinerZ.SteamBridge32.exe"
        BYPRODUCTS "${OPENMINERZ_STEAM_BRIDGE_EXE}"
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/CMakeLists.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/build_bridge.cmd"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/main.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/steam_native.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/SteamBridge32/steam_native.hpp"
        COMMENT "Building and offline-testing the Windows x86 Steam bridge"
        VERBATIM)
    add_dependencies(openminerz openminerz_steam_bridge)
    add_custom_command(TARGET openminerz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:openminerz>/SteamBridge"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENMINERZ_STEAM_BRIDGE_EXE}"
            "$<TARGET_FILE_DIR:openminerz>/SteamBridge/OpenMinerZ.SteamBridge32.exe"
        VERBATIM)

    # Proprietary Steam files are installed locally by Import-GameAssets.ps1.
    # Public build outputs never stage them beside the executable.

    add_custom_command(TARGET openminerz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:openminerz>
            $<TARGET_FILE_DIR:openminerz>
        COMMAND_EXPAND_LISTS
    )

    find_program(DOTNET_EXECUTABLE dotnet)
    if(OPENMINERZ_IMPORT_OWNED_CONTENT)
        if(NOT DOTNET_EXECUTABLE)
            message(FATAL_ERROR
                "OPENMINERZ_IMPORT_OWNED_CONTENT requires the .NET SDK (dotnet).")
        endif()
        if(NOT OPENMINERZ_CLASSIC_GAME_DIR AND NOT OPENMINERZ_MODERN_GAME_DIR)
            message(FATAL_ERROR "Content import needs at least one owned game directory.")
        endif()
        foreach(style CLASSIC MODERN)
            if(OPENMINERZ_${style}_GAME_DIR AND NOT IS_DIRECTORY "${OPENMINERZ_${style}_GAME_DIR}/Content")
                message(FATAL_ERROR "OPENMINERZ_${style}_GAME_DIR is not a game installation.")
            endif()
        endforeach()
    endif()
    if(DOTNET_EXECUTABLE)
        # Keep the helper build independently dirty-aware. A POST_BUILD step only
        # reran when the C++ executable relinked, so editing Program.cs could leave
        # a stale bridge beside an otherwise up-to-date game binary.
        add_custom_target(openminerz_audio_bridge ALL
            COMMAND "${DOTNET_EXECUTABLE}" build
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/AudioBridge/OpenMinerZ.AudioBridge.csproj"
                --configuration Release
                --output "$<TARGET_FILE_DIR:openminerz>/AudioBridge"
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/AudioBridge/Program.cs"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/AudioBridge/OpenMinerZ.AudioBridge.csproj"
            COMMENT "Building the local original-sound-bank bridge"
            VERBATIM
        )
        add_dependencies(openminerz openminerz_audio_bridge)
        if(OPENMINERZ_IMPORT_OWNED_CONTENT)
            set(OPENMINERZ_IMPORTED_PACK_DIR "${CMAKE_BINARY_DIR}/content-packs")
            add_custom_target(openminerz_import_content)
            foreach(style CLASSIC MODERN)
                if(NOT OPENMINERZ_${style}_GAME_DIR)
                    continue()
                endif()
                string(TOLOWER "${style}" style_lower)
                set(pack "${OPENMINERZ_IMPORTED_PACK_DIR}/${style_lower}")
                add_custom_command(
                    OUTPUT "${pack}/.import-complete"
                    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/Import-GameAssets.ps1"
                        -GameDirectory "${OPENMINERZ_${style}_GAME_DIR}"
                        -Style "${style}"
                        -ContentPackRoot "${OPENMINERZ_IMPORTED_PACK_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E touch "${pack}/.import-complete"
                    DEPENDS
                        tools/Import-GameAssets.ps1
                        tools/ContentBridge/Program.cs
                        tools/ContentBridge/RetailCharacter.cs
                        tools/ContentBridge/RetailRegistrations.cs
                        tools/ContentBridge/ScreenAssets.cs
                        tools/ContentBridge/OpenMinerZ.ContentBridge.csproj
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    COMMENT "Importing locally owned ${style_lower} assets (private output)"
                    VERBATIM)
                add_custom_target(openminerz_import_${style_lower} DEPENDS "${pack}/.import-complete")
                add_dependencies(openminerz_import_content openminerz_import_${style_lower})
            endforeach()
            # Each importer builds the same helper and updates a shared local
            # Steam runtime; serialize the two owned generations.
            if(TARGET openminerz_import_classic AND TARGET openminerz_import_modern)
                add_dependencies(openminerz_import_modern openminerz_import_classic)
            endif()
            add_dependencies(openminerz openminerz_import_content)
        endif()
    endif()
endif()

if(OPENMINERZ_BUILD_TESTS)
    include(CTest)
    add_executable(openminerz_tests tests/test_main.cpp)
    target_link_libraries(openminerz_tests PRIVATE openminerz_core)
    if(WIN32)
        add_dependencies(openminerz_tests openminerz_steam_bridge)
    endif()
    if(MSVC)
        target_compile_options(openminerz_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(openminerz_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME openminerz_tests COMMAND openminerz_tests)
    add_executable(openminerz_multiplayer_tests tests/test_multiplayer.cpp)
    target_link_libraries(openminerz_multiplayer_tests PRIVATE openminerz_core)
    # Live Steam publication is opt-in, never part of the offline CTest suite.
    add_executable(openminerz_steam_discovery_check tools/SteamDiscoveryCheck.cpp)
    target_link_libraries(openminerz_steam_discovery_check PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_multiplayer_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_multiplayer_packets COMMAND openminerz_multiplayer_tests)
    add_executable(openminerz_net_latency_tests tests/test_net_latency.cpp)
    target_link_libraries(openminerz_net_latency_tests PRIVATE openminerz_core)
    if(WIN32)
        add_dependencies(openminerz_net_latency_tests openminerz_steam_bridge)
    endif()
    if(MSVC)
        target_compile_options(openminerz_net_latency_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_net_latency COMMAND openminerz_net_latency_tests)
    add_executable(openminerz_desync_soak_tests tests/test_desync_soak.cpp)
    target_link_libraries(openminerz_desync_soak_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_desync_soak_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_desync_soak COMMAND openminerz_desync_soak_tests)
    # Mixed-session semantics outside the dragon and the players: expectations
    # pinned from the original, and the native half of the differential that
    # SemanticsParityProbe runs against the owned assemblies.
    add_executable(openminerz_semantics_parity_tests tests/test_semantics_parity.cpp)
    target_link_libraries(openminerz_semantics_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_semantics_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_semantics_parity COMMAND openminerz_semantics_parity_tests)
    # Everything a peer simulates for itself (ordinary enemies, the local
    # player's physics and survival, thrown and fired objects): expectations
    # pinned from the original, and the native half of the differential that
    # SimulationParityProbe drives through the owned assemblies.
    add_executable(openminerz_simulation_parity_tests tests/test_simulation_parity.cpp)
    target_link_libraries(openminerz_simulation_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_simulation_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_simulation_parity COMMAND openminerz_simulation_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            # The original BaseZombie, Player and projectile classes stepped
            # over scripted frames, replayed through the native simulation.
            set(OPENMINERZ_SIMULATION_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/SimulationParityProbe")
            add_custom_target(openminerz_simulation_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/SimulationParityProbe/OpenMinerZ.SimulationParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_SIMULATION_PROBE_DIR}"
                COMMENT "Building the offline original simulation parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_simulation_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/SimulationParityProbe/Test-SimulationParity.ps1"
                    -Probe "${OPENMINERZ_SIMULATION_PROBE_DIR}/OpenMinerZ.SimulationParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_simulation_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/simulation-parity")
            # No graphics device (the original content reader needs one): skip.
            set_tests_properties(openminerz_original_simulation_parity PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    # The generated world, block for block. Retail peers build base terrain
    # locally from the WorldInfoMessage seed and receive only deltas, so the
    # native generator has to place exactly the blocks the original does.
    # Without arguments this checks hashes pinned from the original builder.
    add_executable(openminerz_terrain_parity_tests tests/test_terrain_parity.cpp)
    target_link_libraries(openminerz_terrain_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_terrain_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_terrain_parity COMMAND openminerz_terrain_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            # The owned original CastleMinerZBuilder executed chunk by chunk and
            # diffed against the native generator.
            set(OPENMINERZ_TERRAIN_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/TerrainParityProbe")
            add_custom_target(openminerz_terrain_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/TerrainParityProbe/OpenMinerZ.TerrainParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_TERRAIN_PROBE_DIR}"
                COMMENT "Building the offline original terrain parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_terrain_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/TerrainParityProbe/Test-TerrainParity.ps1"
                    -Probe "${OPENMINERZ_TERRAIN_PROBE_DIR}/OpenMinerZ.TerrainParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_terrain_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/terrain-parity")
        endif()
    endif()
    # The dragon and its fireballs as a mixed stock / OpenMinerZ session sees
    # them: what the owner sends, what every peer presents, who fires what.
    add_executable(openminerz_dragon_parity_tests tests/test_dragon_parity.cpp)
    target_link_libraries(openminerz_dragon_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_dragon_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_dragon_parity COMMAND openminerz_dragon_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            # Both directions through the owned original DragonEntity,
            # DragonClientEntity and EnemyManager, against an original baseline.
            set(OPENMINERZ_DRAGON_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/DragonParityProbe")
            add_custom_target(openminerz_dragon_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/DragonParityProbe/OpenMinerZ.DragonParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_DRAGON_PROBE_DIR}"
                COMMENT "Building the offline original dragon parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_dragon_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/DragonParityProbe/Test-DragonParity.ps1"
                    -Probe "${OPENMINERZ_DRAGON_PROBE_DIR}/OpenMinerZ.DragonParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_dragon_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/dragon-parity")
            # No graphics device (the original content reader needs one): skip.
            set_tests_properties(openminerz_original_dragon_parity PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    add_executable(openminerz_join_browser_tests tests/test_join_browser.cpp)
    target_link_libraries(openminerz_join_browser_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_join_browser_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_join_browser COMMAND openminerz_join_browser_tests)
    add_executable(openminerz_host_join_tests tests/test_host_join.cpp)
    target_link_libraries(openminerz_host_join_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_host_join_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_host_join COMMAND openminerz_host_join_tests)
    add_executable(openminerz_world_save_tests tests/test_world_save.cpp)
    target_link_libraries(openminerz_world_save_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_world_save_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_world_save_recovery COMMAND openminerz_world_save_tests)
    add_executable(openminerz_gameplay_tests tests/test_gameplay.cpp)
    target_link_libraries(openminerz_gameplay_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_gameplay_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_gameplay_progression COMMAND openminerz_gameplay_tests)
    # Settings persistence, migration, presets and the settings-menu model.
    add_executable(openminerz_settings_tests tests/test_settings.cpp)
    target_link_libraries(openminerz_settings_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_settings_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_settings COMMAND openminerz_settings_tests)
    # The Enhanced presentation's graphics options (rows, persistence, presets,
    # what the renderer is handed) and the arithmetic of the post-processing
    # chain: circle of confusion, autofocus, bloom weights, exposure.
    add_executable(openminerz_graphics_tests tests/test_graphics.cpp)
    target_link_libraries(openminerz_graphics_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_graphics_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_graphics COMMAND openminerz_graphics_tests)
    # The Enhanced presentation's deep underground: how lava lights Hell, the
    # glow and shimmer of its air, how far the eye may open there, and the lava
    # map the shaders read. Pure arithmetic; no graphics device.
    add_executable(openminerz_hell_tests tests/test_hell.cpp)
    target_link_libraries(openminerz_hell_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_hell_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_hell_atmosphere COMMAND openminerz_hell_tests)
    # The Enhanced presentation's effects layer (openminerz/vfx.hpp): curves,
    # emitter arithmetic, budgets and eviction, sort keys, light selection, the
    # near fade that keeps smoke off the lens, and every shipped recipe. Pure
    # arithmetic; no graphics device.
    add_executable(openminerz_vfx_tests tests/test_vfx.cpp)
    target_link_libraries(openminerz_vfx_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_vfx_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_vfx COMMAND openminerz_vfx_tests)
    # The weapon, dragon, mining and creature effects authored on that layer
    # (openminerz/vfx_combat.hpp): every surface and strike has a recipe, the
    # worst cases (a dragon fight, sustained laser fire, rapid mining) stay in
    # budget and die out, and no drawn size reaches the simulation.
    add_executable(openminerz_vfx_combat_tests tests/test_vfx_combat.cpp)
    target_link_libraries(openminerz_vfx_combat_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_vfx_combat_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_vfx_combat COMMAND openminerz_vfx_combat_tests)
    # Far terrain (the Enhanced presentation's distant landscape): the
    # single-column generator path against the full chunk generator, tile
    # summaries, level selection and stitching, known edits, the disk cache
    # (in a temporary directory) and the biome mist function. No device needed.
    add_executable(openminerz_far_terrain_tests tests/test_far_terrain.cpp)
    target_link_libraries(openminerz_far_terrain_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_far_terrain_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_far_terrain COMMAND openminerz_far_terrain_tests)
    # The in-world HUD of both retail builds: hud::layout() against draw lists
    # recorded from the original InGameHUD.OnDraw (pinned; no game files needed),
    # and, with the owned builds configured, against a fresh recording.
    add_executable(openminerz_hud_layout_tests tests/test_hud_layout.cpp)
    target_link_libraries(openminerz_hud_layout_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_hud_layout_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_hud_layout COMMAND openminerz_hud_layout_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            set(OPENMINERZ_HUD_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/HudParityProbe")
            add_custom_target(openminerz_hud_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/HudParityProbe/OpenMinerZ.HudParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_HUD_PROBE_DIR}"
                COMMENT "Building the offline original HUD parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_hud_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/HudParityProbe/Test-HudParity.ps1"
                    -Probe "${OPENMINERZ_HUD_PROBE_DIR}/OpenMinerZ.HudParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_hud_layout_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/hud-parity")
            # The probe exits 77 when the machine has no graphics device.
            set_tests_properties(openminerz_original_hud_parity PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    # The inventory, crate and crafting screens of both retail builds:
    # screens::Session's draw lists and input outcomes against recordings made
    # by running the shipped screens (pinned; no game files needed), and, with
    # the owned builds configured, against a fresh recording.
    add_executable(openminerz_screens_parity_tests tests/test_screens_parity.cpp)
    target_link_libraries(openminerz_screens_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_screens_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_screens_parity COMMAND openminerz_screens_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            set(OPENMINERZ_SCREENS_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/ScreensParityProbe")
            add_custom_target(openminerz_screens_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ScreensParityProbe/OpenMinerZ.ScreensParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_SCREENS_PROBE_DIR}"
                COMMENT "Building the offline original inventory/crate/crafting screen probe"
                VERBATIM)
            add_test(NAME openminerz_original_screens_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ScreensParityProbe/Test-ScreensParity.ps1"
                    -Probe "${OPENMINERZ_SCREENS_PROBE_DIR}/OpenMinerZ.ScreensParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_screens_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/screens-parity")
            # The probe exits 77 when the machine has no graphics device.
            set_tests_properties(openminerz_original_screens_parity PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    # The interface style is presentation only: the same scripted actions must
    # leave the same inventory and crate in the Remastered, Classic and Modern
    # styles, for a Classic-rules and a Modern-rules world. Also the Remastered
    # layout rule at every supported window size. Needs no game files.
    add_executable(openminerz_interface_styles_tests tests/test_interface_styles.cpp)
    target_link_libraries(openminerz_interface_styles_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_interface_styles_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_interface_styles COMMAND openminerz_interface_styles_tests)
    add_executable(openminerz_frame_pacing_tests tests/test_frame_pacing.cpp)
    target_link_libraries(openminerz_frame_pacing_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_frame_pacing_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_frame_pacing COMMAND openminerz_frame_pacing_tests)
    # Field of view: the two first-person projections, Hor+ conversion, zoom
    # ratio, viewmodel-to-world muzzle reconciliation, and the one menu slider.
    add_executable(openminerz_view_projection_tests tests/test_view_projection.cpp)
    target_link_libraries(openminerz_view_projection_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_view_projection_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_view_projection COMMAND openminerz_view_projection_tests)
    add_executable(openminerz_voice_tests tests/test_voice.cpp src/voice_chat_audio.cpp)
    target_include_directories(openminerz_voice_tests PRIVATE src)
    target_link_libraries(openminerz_voice_tests PRIVATE openminerz_core SDL3::SDL3)
    if(MSVC)
        target_compile_options(openminerz_voice_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_voice_playback COMMAND openminerz_voice_tests)
    # What a player reads: the string table, item / block / achievement text and
    # the sprite-font arithmetic, pinned from the original, and the native half
    # of the differential PresentationParityProbe runs against the owned builds.
    add_executable(openminerz_presentation_parity_tests tests/test_presentation_parity.cpp)
    target_link_libraries(openminerz_presentation_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_presentation_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_presentation_parity COMMAND openminerz_presentation_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            set(OPENMINERZ_PRESENTATION_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/PresentationParityProbe")
            add_custom_target(openminerz_presentation_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PresentationParityProbe/OpenMinerZ.PresentationParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_PRESENTATION_PROBE_DIR}"
                COMMENT "Building the offline original presentation parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_presentation_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PresentationParityProbe/Test-PresentationParity.ps1"
                    -Probe "${OPENMINERZ_PRESENTATION_PROBE_DIR}/OpenMinerZ.PresentationParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_presentation_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/presentation-parity"
                    -ContentPacks "${CMAKE_BINARY_DIR}/content-packs")
        endif()
    endif()
    # Which animation plays when: Player.UpdateAnimation and the avatar
    # animation clock, for the local player and for remote players driven by
    # PlayerUpdateMessage. The pinned half replays timelines recorded from both
    # shipped generations; the live half runs AnimationParityProbe.
    add_executable(openminerz_animation_parity_tests tests/test_animation_parity.cpp)
    target_link_libraries(openminerz_animation_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_animation_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_animation_parity COMMAND openminerz_animation_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            set(OPENMINERZ_ANIMATION_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/AnimationParityProbe")
            add_custom_target(openminerz_animation_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/AnimationParityProbe/OpenMinerZ.AnimationParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_ANIMATION_PROBE_DIR}"
                COMMENT "Building the offline original animation parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_animation_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/AnimationParityProbe/Test-AnimationParity.ps1"
                    -Probe "${OPENMINERZ_ANIMATION_PROBE_DIR}/OpenMinerZ.AnimationParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_animation_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/animation-parity")
            # No graphics device (the original content reader needs one): skip.
            set_tests_properties(openminerz_original_animation_parity PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    # Players as seen across a mixed stock / OpenMinerZ session: nametag
    # placement, roster names and PlayerUpdate/PlayerExists field meaning.
    add_executable(openminerz_player_parity_tests tests/test_player_parity.cpp)
    target_link_libraries(openminerz_player_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_player_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_player_parity COMMAND openminerz_player_parity_tests)
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            # The same states through the owned original assemblies, both ways.
            set(OPENMINERZ_PLAYER_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/PlayerParityProbe")
            add_custom_target(openminerz_player_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PlayerParityProbe/OpenMinerZ.PlayerParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_PLAYER_PROBE_DIR}"
                COMMENT "Building the offline original player parity probe"
                VERBATIM)
            add_test(NAME openminerz_original_player_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PlayerParityProbe/Test-PlayerParity.ps1"
                    -Probe "${OPENMINERZ_PLAYER_PROBE_DIR}/OpenMinerZ.PlayerParityProbe.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_player_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/player-parity")
        endif()
    endif()
    add_executable(openminerz_avatar_tests tests/test_avatar.cpp src/avatar_viewmodel.cpp
        src/asset_paths.cpp src/static_model_asset.cpp)
    target_include_directories(openminerz_avatar_tests PRIVATE src)
    target_link_libraries(openminerz_avatar_tests PRIVATE openminerz_core SDL3::SDL3)
    # Modding graphics area: the character loader applies animation packs.
    target_link_libraries(openminerz_avatar_tests PRIVATE openminerz_mod_graphics)
    if(WIN32)
        target_link_libraries(openminerz_avatar_tests PRIVATE d3d11 d3dcompiler)
    endif()
    if(MSVC)
        target_compile_options(openminerz_avatar_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    # The retail character rig. openminerz_rig_parity pins the engine port without
    # the game; openminerz_original_rig_parity imports the character from the
    # owned builds, runs the ORIGINAL engine through RigParityProbe and compares
    # every bone of every registered clip (and every held-item transform).
    add_executable(openminerz_rig_parity_tests tests/test_rig_parity.cpp)
    target_link_libraries(openminerz_rig_parity_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_rig_parity_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_rig_parity COMMAND openminerz_rig_parity_tests)
    # Xbox avatars as skins on that rig, and the owner's add-on protocol that
    # carries them between players (openminerz/xbox_avatar.hpp,
    # openminerz/net/avatar_sync.hpp). Pinned: needs neither the game nor the
    # add-on, and its avatar is synthetic.
    add_executable(openminerz_xbox_avatar_tests tests/test_xbox_avatar.cpp)
    target_link_libraries(openminerz_xbox_avatar_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_xbox_avatar_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_xbox_avatar COMMAND openminerz_xbox_avatar_tests)
    # Modding runtime area: Luau VMs, marshalling, sandbox, budgets, require,
    # lifecycle, storage and packages (openminerz/modding/runtime.hpp). Other
    # areas test their scripts through tests/modding_test_harness.hpp.
    add_executable(openminerz_mod_runtime_tests tests/test_mod_runtime.cpp)
    target_link_libraries(openminerz_mod_runtime_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_runtime_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_runtime COMMAND openminerz_mod_runtime_tests)
    # Polish (modding wave 3): mod items and mod recipes on the retail
    # crafting rules and screens.
    add_executable(openminerz_mod_polish_tests tests/test_mod_polish.cpp)
    target_link_libraries(openminerz_mod_polish_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_polish_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_polish COMMAND openminerz_mod_polish_tests)
    # Modding graphics area: packs, glTF, animation packs, lights, atmosphere,
    # LUTs, camera effects, sound decoding and mixing, effect recipes and the
    # script libraries (openminerz/modding/graphics.hpp). No device needed.
    add_executable(openminerz_mod_graphics_tests tests/test_mod_graphics.cpp)
    target_link_libraries(openminerz_mod_graphics_tests PRIVATE openminerz_mod_graphics)
    if(MSVC)
        target_compile_options(openminerz_mod_graphics_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_graphics COMMAND openminerz_mod_graphics_tests)
    # Modding visuals (wave 2): creature animation, mod blocks on the terrain
    # (with a mesh digest of retail faces), effects before the first frame.
    add_executable(openminerz_mod_visuals_tests tests/test_mod_visuals.cpp)
    target_link_libraries(openminerz_mod_visuals_tests PRIVATE openminerz_mod_graphics)
    if(MSVC)
        target_compile_options(openminerz_mod_visuals_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_visuals COMMAND openminerz_mod_visuals_tests)
    # (Animation packs) export-rig, the pack loader reading an export back and
    # check-anim, on a synthetic character; the round trip on the user's own
    # imported character is registered with the avatar tests below.
    add_executable(openminerz_mod_anim_tests tests/test_mod_anim.cpp src/asset_paths.cpp)
    target_link_libraries(openminerz_mod_anim_tests PRIVATE openminerz_mod_graphics)
    target_compile_definitions(openminerz_mod_anim_tests PRIVATE
        OPENMINERZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(openminerz_mod_anim_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_anim COMMAND openminerz_mod_anim_tests)
    # (GunFX) weapon parts, cues, reload stretch, clip variants and the
    # procedural first-person motion (openminerz/modding/gunfx.hpp).
    add_executable(openminerz_mod_gunfx_tests tests/test_mod_gunfx.cpp)
    target_link_libraries(openminerz_mod_gunfx_tests PRIVATE openminerz_mod_graphics)
    if(MSVC)
        target_compile_options(openminerz_mod_gunfx_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_gunfx COMMAND openminerz_mod_gunfx_tests)
    # Modding UI area: theme layering, layout, widget events and navigation,
    # localization, story messages and hostile input (openminerz/modding/ui.hpp).
    add_executable(openminerz_mod_ui_tests tests/test_mod_ui.cpp)
    target_link_libraries(openminerz_mod_ui_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_ui_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_ui COMMAND openminerz_mod_ui_tests)
    # Modding network area: mod channel framing, Value codec, RPC, replicated
    # state, registry sync, mod download, hostile input, the no-mods wire
    # guarantee and throughput (openminerz/modding/net.hpp).
    add_executable(openminerz_mod_net_tests tests/test_mod_net.cpp)
    target_link_libraries(openminerz_mod_net_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_net_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_net COMMAND openminerz_mod_net_tests)
    # Modding gameplay area: every service from C++ against a fake game, the
    # replication protocol between the halves, the sidecar, script-level mods
    # through the runtime harness, and the no-VM parity of the hooked
    # simulation (openminerz/modding/gameplay.hpp).
    add_executable(openminerz_mod_gameplay_tests tests/test_mod_gameplay.cpp)
    target_link_libraries(openminerz_mod_gameplay_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_gameplay_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_gameplay COMMAND openminerz_mod_gameplay_tests)
    # Modding SDK area: the example mods (validated, then played headless in
    # the SDK session), the `new` templates and the validator; and the
    # generated API reference / Luau types must match the registry.
    add_executable(openminerz_mod_examples_tests tests/test_mod_examples.cpp)
    target_link_libraries(openminerz_mod_examples_tests PRIVATE openminerz_mod_sdk_lib)
    target_compile_definitions(openminerz_mod_examples_tests PRIVATE
        OPENMINERZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(openminerz_mod_examples_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_examples COMMAND openminerz_mod_examples_tests)
    add_test(NAME openminerz_mod_sdk_reference
        COMMAND openminerz_mod_sdk reference --check --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    # Modding integration: the five areas as one system - a host and a joined
    # client over the real admission/extension path of LoopbackSession, one
    # player identity, mod item identity, stock and no-mods wire guarantees,
    # save/packet byte parity of InventoryStack::mod_item.
    add_executable(openminerz_mod_integration_tests tests/test_mod_integration.cpp)
    target_link_libraries(openminerz_mod_integration_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_integration_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_integration COMMAND openminerz_mod_integration_tests)
    # Authority: a host and two OpenMinerZ clients over LoopbackSession with
    # hostile clients (god mode, inflated hits, walls, rapid fire, item
    # spawning, double pickups, reach, speed, flying), honest ones at 200 ms
    # with jitter, and byte parity of every session that is not authoritative.
    add_executable(openminerz_mod_auth_tests tests/test_mod_auth.cpp)
    target_link_libraries(openminerz_mod_auth_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_auth_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_auth COMMAND openminerz_mod_auth_tests)
    # mod-ai: creature AI - brains, senses, teams, packs, attacks, projectiles,
    # movement, bosses, the think budget, a host and a joined client, hostile
    # claims, the inert no-mods path, and the load of 50/200/500 creatures.
    add_executable(openminerz_mod_ai_tests tests/test_mod_ai.cpp)
    target_link_libraries(openminerz_mod_ai_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_ai_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_ai COMMAND openminerz_mod_ai_tests)
    # mod-world: custom world generation (openminerz/world/custom_terrain.hpp,
    # openminerz/modding/worldgen.hpp) - the retail golden hashes, the native
    # evaluator, determinism across threads and peers, mod blocks, saves,
    # big distances.
    add_executable(openminerz_mod_world_tests tests/test_mod_world.cpp)
    target_link_libraries(openminerz_mod_world_tests PRIVATE openminerz_core)
    target_compile_definitions(openminerz_mod_world_tests PRIVATE
        OPENMINERZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(openminerz_mod_world_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_world COMMAND openminerz_mod_world_tests)
    # mod-liquid: liquid mod blocks (openminerz/world/liquid.hpp) - which cells
    # are liquid, meshing, swimming and climbing out, the crosshair and
    # building, the service's liquid cells, creatures, spawns, item.used's
    # target, the host's movement and edit checks, and retail parity.
    add_executable(openminerz_mod_liquid_tests tests/test_mod_liquid.cpp)
    target_link_libraries(openminerz_mod_liquid_tests PRIVATE openminerz_core)
    if(MSVC)
        target_compile_options(openminerz_mod_liquid_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_liquid COMMAND openminerz_mod_liquid_tests)
    # (Items) held and dropped mod items: model/hold/scale/weapon definitions,
    # the "held" messages, the server's and the authority's judgement of mod
    # weapons, hold styles, grips, item clips and the Wilderness broadsword.
    add_executable(openminerz_mod_items_tests tests/test_mod_items.cpp)
    target_link_libraries(openminerz_mod_items_tests PRIVATE openminerz_mod_graphics)
    target_compile_definitions(openminerz_mod_items_tests PRIVATE
        OPENMINERZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(openminerz_mod_items_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_items COMMAND openminerz_mod_items_tests)
    # Add-Ons: installed mods turned on and off - the roots read mod by mod,
    # the player's choices, conflicts, dependencies, the catalog of the mods
    # that are on, and the host's lobby with every mod off equal to no mods.
    add_executable(openminerz_mod_addons_tests tests/test_mod_addons.cpp)
    target_link_libraries(openminerz_mod_addons_tests PRIVATE openminerz_core)
    target_compile_definitions(openminerz_mod_addons_tests PRIVATE
        OPENMINERZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(openminerz_mod_addons_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME openminerz_mod_addons COMMAND openminerz_mod_addons_tests)
    # The avatar tests need a content pack (the character plus the held-item
    # models). OPENMINERZ_TEST_CONTENT_PACKS names a directory holding classic/
    # and modern/ packs; when the owned builds are configured the rig parity test
    # imports a private pair under the build tree and the avatar tests use that.
    set(OPENMINERZ_TEST_CONTENT_PACKS "" CACHE PATH
        "Directory holding classic/ and modern/ content packs for the avatar tests")
    set(OPENMINERZ_AVATAR_TEST_PACKS "${OPENMINERZ_TEST_CONTENT_PACKS}")
    set(OPENMINERZ_AVATAR_TEST_FIXTURE "")
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE AND DOTNET_EXECUTABLE AND
           EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
           EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
            set(OPENMINERZ_RIG_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/RigParityProbe")
            set(OPENMINERZ_RIG_BRIDGE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/RigContentBridge")
            add_custom_target(openminerz_rig_parity_probe ALL
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/RigParityProbe/OpenMinerZ.RigParityProbe.csproj"
                    --configuration Release --output "${OPENMINERZ_RIG_PROBE_DIR}"
                COMMAND "${DOTNET_EXECUTABLE}" build
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ContentBridge/OpenMinerZ.ContentBridge.csproj"
                    --configuration Release --output "${OPENMINERZ_RIG_BRIDGE_DIR}"
                COMMENT "Building the offline original character rig probe and the importer"
                VERBATIM)
            add_test(NAME openminerz_original_rig_parity
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/RigParityProbe/Test-RigParity.ps1"
                    -Probe "${OPENMINERZ_RIG_PROBE_DIR}/OpenMinerZ.RigParityProbe.exe"
                    -Bridge "${OPENMINERZ_RIG_BRIDGE_DIR}/OpenMinerZ.ContentBridge.exe"
                    -NativeTests "$<TARGET_FILE:openminerz_rig_parity_tests>"
                    -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                    -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/rig-parity")
            set_tests_properties(openminerz_original_rig_parity PROPERTIES
                SKIP_RETURN_CODE 77 TIMEOUT 900 FIXTURES_SETUP openminerz_rig_packs)
            if(NOT OPENMINERZ_AVATAR_TEST_PACKS)
                set(OPENMINERZ_AVATAR_TEST_PACKS "${CMAKE_BINARY_DIR}/rig-parity/packs")
                set(OPENMINERZ_AVATAR_TEST_FIXTURE openminerz_rig_packs)
            endif()
        endif()
    endif()
    # A build configured without the owned game folders still runs the avatar
    # tests when a pair of test packs has been imported once into the
    # repository's (untracked) out/ tree:
    #   tools/Import-GameAssets.ps1 -GameDirectory <dir> -Style Classic
    #       -ContentPackRoot out/test-content-packs
    # Repeat with the owned Modern installation and -Style Modern.
    # The override only takes effect for a style whose pack exists there, so an
    # empty out/ falls back to the installed packs and, failing those, a skip.
    if(NOT OPENMINERZ_AVATAR_TEST_PACKS)
        set(OPENMINERZ_AVATAR_TEST_PACKS "${CMAKE_SOURCE_DIR}/out/test-content-packs")
    endif()
    foreach(style classic modern)
        # (Animation packs) the round trip on the imported character: skipped
        # (77) when the content has not been imported.
        add_test(NAME openminerz_mod_anim_${style} COMMAND openminerz_mod_anim_tests ${style})
        set_tests_properties(openminerz_mod_anim_${style} PROPERTIES SKIP_RETURN_CODE 77)
        if(OPENMINERZ_AVATAR_TEST_PACKS)
            set_tests_properties(openminerz_mod_anim_${style} PROPERTIES
                ENVIRONMENT "OPENMINERZ_CONTENT_PACKS=${OPENMINERZ_AVATAR_TEST_PACKS}")
        endif()
        if(OPENMINERZ_AVATAR_TEST_FIXTURE)
            set_tests_properties(openminerz_mod_anim_${style} PROPERTIES
                FIXTURES_REQUIRED ${OPENMINERZ_AVATAR_TEST_FIXTURE})
        endif()
        add_test(NAME openminerz_avatar_${style} COMMAND openminerz_avatar_tests ${style})
        set_tests_properties(openminerz_avatar_${style} PROPERTIES SKIP_RETURN_CODE 77)
        if(OPENMINERZ_AVATAR_TEST_PACKS)
            set_tests_properties(openminerz_avatar_${style} PROPERTIES
                ENVIRONMENT "OPENMINERZ_CONTENT_PACKS=${OPENMINERZ_AVATAR_TEST_PACKS}")
        endif()
        if(OPENMINERZ_AVATAR_TEST_FIXTURE)
            set_tests_properties(openminerz_avatar_${style} PROPERTIES
                FIXTURES_REQUIRED ${OPENMINERZ_AVATAR_TEST_FIXTURE})
        endif()
    endforeach()
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE)
            if(DOTNET_EXECUTABLE AND
               EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
               EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
                set(OPENMINERZ_PACKET_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/PacketContractProbe")
                add_custom_target(openminerz_packet_contract_probe ALL
                    COMMAND "${DOTNET_EXECUTABLE}" build
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PacketContractProbe/OpenMinerZ.PacketContractProbe.csproj"
                        --configuration Release --output "${OPENMINERZ_PACKET_PROBE_DIR}"
                    COMMENT "Building the offline original packet serializer probe"
                    VERBATIM)
                add_test(NAME openminerz_original_packet_contracts
                    COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/PacketContractProbe/Test-PacketContracts.ps1"
                        -Probe "${OPENMINERZ_PACKET_PROBE_DIR}/OpenMinerZ.PacketContractProbe.exe"
                        -NativeTests "$<TARGET_FILE:openminerz_multiplayer_tests>"
                        -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                        -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                        -OutputDirectory "${CMAKE_BINARY_DIR}/packet-contracts")
                # Xbox Avatar add-on interop, proven by running the add-on: a
                # build of it is read into memory (never copied), hosted in the
                # original engine and made to hold a whole session with the
                # native avatar sync. The add-on is built per engine era and
                # ships two ways; every build named here that exists gets a
                # run, and the test is registered when at least one does.
                set(OPENMINERZ_XBOX_AVATAR_ADDON_CASTLEFORGE
                    "C:/Program Files (x86)/Steam/steamapps/common/CastleMiner Z/!Mods/XboxAvatar.dll"
                    CACHE FILEPATH "Xbox Avatar add-on, mod-loader build for 1.9.9+ (XboxAvatar.dll)")
                set(OPENMINERZ_XBOX_AVATAR_ADDON_CLASSIC "" CACHE FILEPATH
                    "Xbox Avatar add-on, standalone build for pre-1.9.9 clients (runtimes/legacy/XboxAvatarMod.dll)")
                set(OPENMINERZ_XBOX_AVATAR_ADDON_MODERN "" CACHE FILEPATH
                    "Xbox Avatar add-on, standalone build for 1.9.9+ clients (runtimes/modern/XboxAvatarMod.dll)")
                if(EXISTS "${OPENMINERZ_XBOX_AVATAR_ADDON_CASTLEFORGE}" OR
                   EXISTS "${OPENMINERZ_XBOX_AVATAR_ADDON_CLASSIC}" OR
                   EXISTS "${OPENMINERZ_XBOX_AVATAR_ADDON_MODERN}")
                    # An empty argument does not survive `powershell -File`.
                    foreach(build CLASSIC MODERN CASTLEFORGE)
                        set(OPENMINERZ_AVATAR_ADDON_ARGUMENT_${build} "none")
                        if(EXISTS "${OPENMINERZ_XBOX_AVATAR_ADDON_${build}}")
                            set(OPENMINERZ_AVATAR_ADDON_ARGUMENT_${build} "${OPENMINERZ_XBOX_AVATAR_ADDON_${build}}")
                        endif()
                    endforeach()
                    set(OPENMINERZ_AVATAR_ADDON_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/AvatarAddonProbe")
                    add_custom_target(openminerz_avatar_addon_probe ALL
                        COMMAND "${DOTNET_EXECUTABLE}" build
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/AvatarAddonProbe/OpenMinerZ.AvatarAddonProbe.csproj"
                            --configuration Release --output "${OPENMINERZ_AVATAR_ADDON_PROBE_DIR}"
                        COMMENT "Building the offline Xbox Avatar add-on interop probe"
                        VERBATIM)
                    add_test(NAME openminerz_original_avatar_addon
                        COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/AvatarAddonProbe/Test-AvatarAddon.ps1"
                            -Probe "${OPENMINERZ_AVATAR_ADDON_PROBE_DIR}/OpenMinerZ.AvatarAddonProbe.exe"
                            -NativeTests "$<TARGET_FILE:openminerz_xbox_avatar_tests>"
                            -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                            -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                            -OutputDirectory "${CMAKE_BINARY_DIR}/avatar-addon"
                            -ClassicAddon "${OPENMINERZ_AVATAR_ADDON_ARGUMENT_CLASSIC}"
                            -ModernAddon "${OPENMINERZ_AVATAR_ADDON_ARGUMENT_MODERN}"
                            -CastleForgeAddon "${OPENMINERZ_AVATAR_ADDON_ARGUMENT_CASTLEFORGE}")
                    set_tests_properties(openminerz_original_avatar_addon PROPERTIES
                        SKIP_RETURN_CODE 77 TIMEOUT 300)
                endif()
                # Executes the original's gameplay send and apply code offline
                # and compares it with the native client: what the bytes mean,
                # where the packet probe above only proves how they are laid out.
                set(OPENMINERZ_SEMANTICS_PROBE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/SemanticsParityProbe")
                add_custom_target(openminerz_semantics_parity_probe ALL
                    COMMAND "${DOTNET_EXECUTABLE}" build
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/SemanticsParityProbe/OpenMinerZ.SemanticsParityProbe.csproj"
                        --configuration Release --output "${OPENMINERZ_SEMANTICS_PROBE_DIR}"
                    COMMENT "Building the offline original gameplay semantics probe"
                    VERBATIM)
                add_test(NAME openminerz_original_semantics_parity
                    COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/SemanticsParityProbe/Test-SemanticsParity.ps1"
                        -Probe "${OPENMINERZ_SEMANTICS_PROBE_DIR}/OpenMinerZ.SemanticsParityProbe.exe"
                        -NativeTests "$<TARGET_FILE:openminerz_semantics_parity_tests>"
                        -ClassicGameDirectory "${OPENMINERZ_CLASSIC_GAME_DIR}"
                        -ModernGameDirectory "${OPENMINERZ_MODERN_GAME_DIR}"
                        -OutputDirectory "${CMAKE_BINARY_DIR}/semantics-parity")
            endif()
            add_test(NAME openminerz_protocol_trace_audit
                COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Test-Audit-Trace.ps1"
                    -AuditScript
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Audit-Trace.ps1")
            if(OPENMINERZ_BUILD_INTEROP_LAB)
                add_test(NAME openminerz_original_client_supervisor
                    COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Test-Supervisor.ps1"
                        -Launcher "$<TARGET_FILE:openminerz_interop_launcher>"
                        -Fixture "$<TARGET_FILE:openminerz_interop_supervisor_fixture>"
                        -ControlScript
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Interop-Control.ps1")
                add_test(NAME openminerz_original_managed_state_host
                    COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ManagedStateHost/Test-ManagedStateHost.ps1"
                        -HostExecutable "${OPENMINERZ_MANAGED_STATE_HOST}"
                        -FixtureExecutable "${OPENMINERZ_MANAGED_STATE_FIXTURE}"
                        -ControlScript
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ManagedStateHost/State-Control.ps1"
                        -CompareScript
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Compare-State.ps1")
                add_test(NAME openminerz_original_supervisor_state_forwarding
                    COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ManagedStateHost/Test-SupervisorState.ps1"
                        -Launcher "$<TARGET_FILE:openminerz_interop_launcher>"
                        -HostExecutable "${OPENMINERZ_MANAGED_STATE_HOST}"
                        -FixtureExecutable "${OPENMINERZ_MANAGED_STATE_FIXTURE}"
                        -ControlScript
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/Interop-Control.ps1")
                if(EXISTS "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe" AND
                   EXISTS "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
                    add_test(NAME openminerz_original_assembly_contracts
                        COMMAND "${OPENMINERZ_POWERSHELL_EXECUTABLE}" -NoProfile -File
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/OriginalClientInterop/ManagedStateHost/Test-ContractProbe.ps1"
                            -HostExecutable "${OPENMINERZ_MANAGED_STATE_HOST}"
                            -ClassicGame "${OPENMINERZ_CLASSIC_GAME_DIR}/CastleMinerZ.exe"
                            -ModernGame "${OPENMINERZ_MODERN_GAME_DIR}/CastleMinerZ.exe")
                endif()
            endif()
        endif()
    endif()
endif()
