cmake_minimum_required(VERSION 3.25)

project(OpenMinerZSteamBridge32 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "OpenMinerZSteamBridge32 must be configured for Windows x86")
endif()

get_filename_component(OPENMINERZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(openminerz_steam_bridge32
    main.cpp
    steam_native.cpp
    "${OPENMINERZ_ROOT}/src/net/steam_bridge_protocol.cpp")
target_include_directories(openminerz_steam_bridge32 PRIVATE
    "${OPENMINERZ_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(openminerz_steam_bridge32 PRIVATE cxx_std_20)
target_compile_definitions(openminerz_steam_bridge32 PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0601)
if(MSVC)
    target_compile_options(openminerz_steam_bridge32 PRIVATE
        /W4 /WX /permissive- /EHsc /utf-8)
endif()
set_target_properties(openminerz_steam_bridge32 PROPERTIES
    OUTPUT_NAME OpenMinerZ.SteamBridge32
    RUNTIME_OUTPUT_DIRECTORY "${OPENMINERZ_ROOT}/out/steam-bridge32"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OPENMINERZ_ROOT}/out/steam-bridge32"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OPENMINERZ_ROOT}/out/steam-bridge32"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OPENMINERZ_ROOT}/out/steam-bridge32"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OPENMINERZ_ROOT}/out/steam-bridge32")
