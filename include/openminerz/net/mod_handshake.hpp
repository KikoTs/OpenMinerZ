#pragma once

#include "openminerz/net/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    // (Authority) The host chose "OpenMinerZ players only": admission needs
    // the extension hello even with no server/core mod, and the host owns
    // health, damage, items and edits (net/session_authority.hpp). Bit 1 of
    // the extension flags byte; false leaves every byte as it was.
    bool openminerz_only{};
};

enum class ExtensionMessageKind : std::uint8_t
{
    client_hello = 1,
    server_accept = 2,
    server_reject = 3,
    // Mod channel (net/mod_channel.hpp): one fragment of a mod message. Only
    // ever exchanged with a peer that completed the hello for a manifest that
    // carries a server/core mod.
    mod_data = 16,
    // Client-visible mod download before the hello (net/mod_download.hpp).
    download_request = 17,
    download_offer = 18,
    download_chunk_request = 19,
    download_chunk = 20,
    download_refusal = 21
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
// The kind byte of an extension packet of this extension version, without
// validating anything else; nullopt for any other packet. Handshake kinds are
// decoded by decode_extension_message, mod-channel and download kinds by
// their own codecs (which reject what this lets through).
[[nodiscard]] std::optional<ExtensionMessageKind> extension_message_kind(
    std::span<const std::uint8_t> packet) noexcept;
[[nodiscard]] bool is_mod_channel_packet(std::span<const std::uint8_t> packet) noexcept;
[[nodiscard]] bool is_mod_download_packet(std::span<const std::uint8_t> packet) noexcept;
// The CRC32 (IEEE, reflected) every extension packet ends with, little-endian.
[[nodiscard]] std::uint32_t extension_crc32(std::span<const std::uint8_t> bytes) noexcept;
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
// Only OpenMinerZ peers may join: a server/core mod, or openminerz_only.
// (Authority) Exactly the sessions whose host is authoritative.
[[nodiscard]] bool requires_extension(const ModManifest &manifest) noexcept;

// A peer with no advertised OpenMinerZ capability is treated as a stock
// client. Stock clients are accepted only when the host has no synchronized
// server/core mods; no extension packet is ever sent to them.
[[nodiscard]] ModCompatibilityResult compare_mod_manifests(const ModManifest &host,
                                                           const ModManifest *peer,
                                                           bool peer_advertised_extension);

} // namespace openminerz::net
