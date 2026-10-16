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
[[nodiscard]] std::filesystem::path content_pack_path(AssetStyle style);
// The DerivedData directory holding one retail build's INTERFACE art: its
// sprite fonts (font_*.omfnt), HUD sprites (hud_*.bmp), inventory screen
// sprites (screen_*.bmp) and item-icon atlas. Unlike content_pack_path this
// does not require a complete pack: the interface of a build is usable as soon
// as its art has been imported, whichever pack supplies the world.
[[nodiscard]] std::filesystem::path interface_data_path(AssetStyle style);
[[nodiscard]] std::filesystem::path active_derived_data_path();
// The picture shown behind the menus, if the player has one: a still rendered
// from their own content by tools/Capture-MenuBackground.ps1. It lives in local
// data (%LOCALAPPDATA%\OpenMinerZ\MenuBackground\menu-background.png, or a
// MenuBackground folder beside the executable) and never in the repository.
// OPENMINERZ_MENU_BACKGROUND names a specific file. Empty when there is none.
//
// The script renders each picture twice, because a 16:9 frame cropped to a
// 32:9 window keeps only half its height: menu-background.png is 16:9 and
// menu-background-wide.png is the same camera rendered 32:9. `wide` asks for
// the second and answers with the first when only that exists.
[[nodiscard]] std::filesystem::path menu_background_path(bool wide = false);
// Where that script writes it and its alternatives (the folder may not exist).
[[nodiscard]] std::filesystem::path menu_background_directory();
// Finds the user's owned 2013 x86 Steam runtime. An explicit
// OPENMINERZ_STEAM_API path wins; no proprietary binary is copied into sources.
[[nodiscard]] std::filesystem::path installed_steam_api_path();

} // namespace openminerz
