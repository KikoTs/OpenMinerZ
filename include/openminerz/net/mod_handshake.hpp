#pragma once

#include "openminerz/net/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::net
{

enum class ModScope : std::uint8_t
{
    client = 0,
    server = 1,
    core = 2
};

struct ModDescriptor final
{
    std::string id;
    std::string version;
    std::array<std::uint8_t, 32U> content_digest{};
    ModScope scope{ModScope::client};

    auto operator<=>(const ModDescriptor &) const = default;
};

struct ModManifest final
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    std::vector<ModDescriptor> mods;
};

enum class ExtensionMessageKind : std::uint8_t
{
    client_hello = 1,
    server_accept = 2,
    server_reject = 3
};

struct ExtensionMessage final
{
    ExtensionMessageKind kind{ExtensionMessageKind::client_hello};
    std::uint64_t nonce{};
    ModManifest manifest;
    std::string detail;
};

inline constexpr std::uint8_t extension_protocol_version = 1U;
inline constexpr std::size_t maximum_extension_packet_size = 4096U;
inline constexpr std::size_t maximum_synchronized_mods = 64U;
inline constexpr std::size_t maximum_mod_id_bytes = 96U;
inline constexpr std::size_t maximum_mod_version_bytes = 48U;
inline constexpr std::size_t maximum_extension_detail_bytes = 192U;

enum class ExtensionDecodeError : std::uint8_t
{
    none,
    too_small,
    too_large,
    wrong_magic,
    unsupported_extension_version,
    unknown_message_kind,
    unsupported_game_protocol,
    invalid_flags,
    too_many_mods,
    invalid_mod_scope,
    invalid_mod_order,
    invalid_manifest,
    invalid_string,
    invalid_detail,
    trailing_data,
    checksum_mismatch
};

[[nodiscard]] bool is_extension_packet(std::span<const std::uint8_t> packet) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encode_extension_message(const ExtensionMessage &message);
[[nodiscard]] bool decode_extension_message(std::span<const std::uint8_t> packet,
                                            ExtensionMessage &message, ExtensionDecodeError &error);

enum class ModCompatibility : std::uint8_t
{
    compatible,
    extension_required,
    protocol_mismatch,
    missing_mod,
    unexpected_mod,
    order_mismatch,
    scope_mismatch,
    version_mismatch,
    digest_mismatch,
    invalid_manifest
};

struct ModCompatibilityResult final
{
    ModCompatibility status{ModCompatibility::compatible};
    std::string detail;

    [[nodiscard]] bool compatible() const noexcept
    {
        return status == ModCompatibility::compatible;
    }
};

[[nodiscard]] bool validate_mod_manifest(const ModManifest &manifest, std::string &error) noexcept;
[[nodiscard]] bool requires_extension(const ModManifest &manifest) noexcept;

// A peer with no advertised OpenMinerZ capability is treated as a stock
// client. Stock clients are accepted only when the host has no synchronized
// server/core mods; no extension packet is ever sent to them.
[[nodiscard]] ModCompatibilityResult compare_mod_manifests(const ModManifest &host,
                                                           const ModManifest *peer,
                                                           bool peer_advertised_extension);

} // namespace openminerz::net
