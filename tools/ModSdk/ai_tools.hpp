#pragma once

// mod-ai: the SDK's creature AI tools.
//
//   check_creature_ai   what `validate` adds for a mod that defines creatures,
//                       brains or projectiles or sets retail enemy rules: the
//                       definitions loaded the way the game loads them, then
//                       checked together (a brain or projectile that is not
//                       defined, a boss phase's state the brain lacks, a
//                       retail: look that is not a retail enemy).
//   creature_summary    what `run --creatures` prints after a headless run:
//                       the AI's load and every creature the client sees.

#include "validate.hpp"

#include <string>

namespace openminerz::mod_sdk
{

class Sandbox;

void check_creature_ai(ValidationResult &result);
[[nodiscard]] std::string creature_summary(Sandbox &sandbox);

} // namespace openminerz::mod_sdk
