#pragma once

// `openminerz_mod_sdk export-rig` and `check-anim`: authoring animation packs
// against the player character imported from the user's own game
// (openminerz/modding/animation_authoring.hpp, docs/modding/animation.md).

#include <string>
#include <vector>

namespace openminerz::mod_sdk
{

int command_export_rig(std::vector<std::string> args);
int command_check_anim(std::vector<std::string> args);

} // namespace openminerz::mod_sdk
