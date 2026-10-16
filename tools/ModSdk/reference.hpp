#pragma once

// The API reference, Luau type definitions and JSON dump, written from an
// ApiRegistry (sdk_session.hpp registers it exactly as the game does). Every
// output is deterministic: the same registry gives the same bytes, which is
// what the stale-reference check compares.

#include "openminerz/modding/api.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace openminerz::mod_sdk
{

struct GeneratedFiles final
{
    std::string markdown; // docs/modding/api-reference.md
    std::string luau;     // tools/ModSdk/types/openminerz.d.luau
    std::string json;     // tools/ModSdk/types/openminerz-api.json
};

[[nodiscard]] GeneratedFiles generate_reference(const modding::ApiRegistry &registry);

// Who may call a function: a client-scope mod (on the client VM), and a
// server/core mod on each VM. Shared with the validator.
struct Availability final
{
    bool client_mod{};
    bool server_mod_server{};
    bool server_mod_client{};
};
[[nodiscard]] Availability availability(const modding::FunctionSpec &spec) noexcept;
[[nodiscard]] std::string capability_names(modding::CapabilityMask mask);
[[nodiscard]] std::string_view side_text(modding::SideMask sides) noexcept;
// The first segment of a dotted name ("storage" for "storage.world.get").
[[nodiscard]] std::string_view library_of(std::string_view name) noexcept;
// Luau globals a library may not shadow (the runtime skips such names).
[[nodiscard]] bool reserved_global(std::string_view name) noexcept;

} // namespace openminerz::mod_sdk
