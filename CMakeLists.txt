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
    CACHE FILEPATH "Owned PE32 CastleMiner Z Steam API DLL copied into Windows deployments")
set(OPENMINERZ_AVATAR_ASSET
    "$ENV{LOCALAPPDATA}/OpenMinerZ/DerivedData/avatar.omavatar"
    CACHE FILEPATH "Optional locally exported XNA avatar shell used by both content packs")

add_library(openminerz_core
    src/design_canvas.cpp
    src/inventory.cpp
    src/interop_state.cpp
    src/menu_model.cpp
    src/net/admission.cpp
    src/net/authoritative_bootstrap.cpp
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
    src/net/pickup_codec.cpp
    src/net/player_replication.cpp
    src/net/protocol.cpp
    src/net/protocol_trace.cpp
    src/net/session_browser.cpp
    src/net/steam_bridge_client.cpp
    src/net/steam_bridge_protocol.cpp
    src/net/transport.cpp
    src/settings.cpp
    src/settings_menu.cpp
    src/world_save.cpp
    src/world/block.cpp
    src/world/dynamic_radiance.cpp
    src/world/enemy.cpp
    src/world/explosive.cpp
    src/world/legacy_noise.cpp
    src/world/loot.cpp
    src/world/player.cpp
    src/world/sky.cpp
    src/world/spawner.cpp
    src/world/world.cpp
)
target_include_directories(openminerz_core PUBLIC include)
target_compile_features(openminerz_core PUBLIC cxx_std_20)

add_executable(openminerz
    src/app.cpp
    src/asset_paths.cpp
    src/avatar_viewmodel.cpp
    src/depth_voxel_renderer.cpp
    src/enemy_renderer.cpp
    src/font_renderer.cpp
    src/main.cpp
    src/original_sound_bank.cpp
    src/static_model_asset.cpp
    src/world_preview_renderer.cpp
)
target_include_directories(openminerz PRIVATE include src)
target_link_libraries(openminerz PRIVATE openminerz_core Freetype::Freetype SDL3::SDL3)
if(OPENMINERZ_MODERN_GAME_DIR)
    target_compile_definitions(openminerz PRIVATE
        OPENMINERZ_OWNED_GAME_DIR=L"${OPENMINERZ_MODERN_GAME_DIR}")
endif()

if(MSVC)
    target_compile_options(openminerz_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(openminerz PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(openminerz_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(openminerz PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

    # The legacy helper loads the owned 2013 PE32 Steam ABI. Keep that DLL at
    # the deployment root so a copied server/client build does not silently
    # depend on the developer machine's Steam installation.
    if(EXISTS "${OPENMINERZ_STEAM_API_DLL}")
        add_custom_command(TARGET openminerz POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENMINERZ_STEAM_API_DLL}"
                "$<TARGET_FILE_DIR:openminerz>/steam_api.dll"
            VERBATIM)
    else()
        message(WARNING
            "Owned CastleMiner Z steam_api.dll was not found; the Windows deployment will require OPENMINERZ_STEAM_API to be set")
    endif()

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
        if(NOT IS_DIRECTORY "${OPENMINERZ_CLASSIC_GAME_DIR}/Content")
            message(FATAL_ERROR
                "OPENMINERZ_CLASSIC_GAME_DIR must point to a locally owned Classic build.")
        endif()
        if(NOT IS_DIRECTORY "${OPENMINERZ_MODERN_GAME_DIR}/Content")
            message(FATAL_ERROR
                "OPENMINERZ_MODERN_GAME_DIR must point to a locally owned Modern build.")
        endif()
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
            set(OPENMINERZ_CLASSIC_PACK "${OPENMINERZ_IMPORTED_PACK_DIR}/classic")
            set(OPENMINERZ_MODERN_PACK "${OPENMINERZ_IMPORTED_PACK_DIR}/modern")

            set(OPENMINERZ_AVATAR_COPY_COMMANDS)
            get_filename_component(OPENMINERZ_AVATAR_SUPPORT_DIR
                "${OPENMINERZ_AVATAR_ASSET}" DIRECTORY)
            foreach(OPENMINERZ_AVATAR_SUPPORT_FILE
                    "${OPENMINERZ_AVATAR_ASSET}"
                    "${OPENMINERZ_AVATAR_SUPPORT_DIR}/proxy_hand.omhc"
                    "${OPENMINERZ_AVATAR_SUPPORT_DIR}/proxy_hand_part_0.png"
                    "${OPENMINERZ_AVATAR_SUPPORT_DIR}/proxy_hand_manifest.txt")
                if(EXISTS "${OPENMINERZ_AVATAR_SUPPORT_FILE}")
                    get_filename_component(OPENMINERZ_AVATAR_SUPPORT_NAME
                        "${OPENMINERZ_AVATAR_SUPPORT_FILE}" NAME)
                    list(APPEND OPENMINERZ_AVATAR_COPY_COMMANDS
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${OPENMINERZ_AVATAR_SUPPORT_FILE}"
                            "${OPENMINERZ_CLASSIC_PACK}/DerivedData/${OPENMINERZ_AVATAR_SUPPORT_NAME}"
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${OPENMINERZ_AVATAR_SUPPORT_FILE}"
                            "${OPENMINERZ_MODERN_PACK}/DerivedData/${OPENMINERZ_AVATAR_SUPPORT_NAME}")
                endif()
            endforeach()

            add_custom_command(
                OUTPUT
                    "${OPENMINERZ_CLASSIC_PACK}/.import-complete"
                    "${OPENMINERZ_MODERN_PACK}/.import-complete"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${OPENMINERZ_CLASSIC_PACK}/DerivedData"
                    "${OPENMINERZ_MODERN_PACK}/DerivedData"
                COMMAND "${DOTNET_EXECUTABLE}" run
                    --project "${CMAKE_CURRENT_SOURCE_DIR}/tools/ContentBridge/OpenMinerZ.ContentBridge.csproj"
                    --configuration Release
                    -- "${OPENMINERZ_CLASSIC_GAME_DIR}"
                       "${OPENMINERZ_CLASSIC_PACK}/DerivedData"
                COMMAND "${DOTNET_EXECUTABLE}" run
                    --project "${CMAKE_CURRENT_SOURCE_DIR}/tools/ContentBridge/OpenMinerZ.ContentBridge.csproj"
                    --configuration Release
                    -- "${OPENMINERZ_MODERN_GAME_DIR}"
                       "${OPENMINERZ_MODERN_PACK}/DerivedData"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${OPENMINERZ_CLASSIC_GAME_DIR}/Content"
                    "${OPENMINERZ_CLASSIC_PACK}/Content"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${OPENMINERZ_MODERN_GAME_DIR}/Content"
                    "${OPENMINERZ_MODERN_PACK}/Content"
                ${OPENMINERZ_AVATAR_COPY_COMMANDS}
                COMMAND ${CMAKE_COMMAND} -E touch
                    "${OPENMINERZ_CLASSIC_PACK}/.import-complete"
                    "${OPENMINERZ_MODERN_PACK}/.import-complete"
                DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ContentBridge/Program.cs"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ContentBridge/OpenMinerZ.ContentBridge.csproj"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                COMMENT "Importing complete Classic and Modern content packs"
                VERBATIM)
            add_custom_target(openminerz_import_content ALL
                DEPENDS
                    "${OPENMINERZ_CLASSIC_PACK}/.import-complete"
                    "${OPENMINERZ_MODERN_PACK}/.import-complete")
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
    if(WIN32)
        find_program(OPENMINERZ_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(OPENMINERZ_POWERSHELL_EXECUTABLE)
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
