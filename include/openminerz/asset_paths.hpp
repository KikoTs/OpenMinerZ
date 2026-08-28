#pragma once

#include "openminerz/settings.hpp"

#include <filesystem>

namespace openminerz
{

// Asset style is selected once, before any renderer or audio object is created.
// Changing the persisted setting deliberately does not mutate the live pack.
void set_active_asset_style(AssetStyle style) noexcept;
[[nodiscard]] AssetStyle active_asset_style() noexcept;
[[nodiscard]] std::filesystem::path application_directory();
[[nodiscard]] std::filesystem::path active_content_pack_path();
[[nodiscard]] std::filesystem::path active_derived_data_path();
// Finds the user's owned 2013 x86 Steam runtime. An explicit
// OPENMINERZ_STEAM_API path wins; no proprietary binary is copied into sources.
[[nodiscard]] std::filesystem::path installed_steam_api_path();

} // namespace openminerz
