#include "openminerz/net/mod_handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <stdexcept>
#include <unordered_set>

namespace openminerz::net
{
namespace
{

constexpr std::array<std::uint8_t, 4U> extension_magic{'O', 'M', 'Z', 'E'};
// The handshake's flags byte: synchronized mods follow; (Authority) the host
// admits OpenMinerZ players only. A manifest without the second encodes as it
// always did.
constexpr unsigned extension_flag_mods = 1U;
constexpr unsigned extension_flag_openminerz_only = 2U;

bool supported_protocol(ProtocolVersion protocol) noexcept
{
    return protocol == ProtocolVersion::classic_v3 || protocol == ProtocolVersion::modern_v4;
}

bool valid_mod_id(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= maximum_mod_id_bytes &&
           std::all_of(id.begin(), id.end(), [](unsigned char value) {
               return std::isalnum(value) != 0 || value == '.' || value == '_' || value == '-';
           });
}

bool valid_version(std::string_view version) noexcept
{
    return !version.empty() && version.size() <= maximum_mod_version_bytes &&
           std::none_of(version.begin(), version.end(),
                        [](unsigned char value) { return value < 0x20U || value == 0x7FU; });
}

bool all_zero(std::span<const std::uint8_t> bytes) noexcept
{
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0U; });
}

std::size_t seven_bit_prefix_size(std::size_t value) noexcept
{
    std::size_t result{1U};
    while (value >= 0x80U)
    {
        value >>= 7U;
        ++result;
    }
    return result;
}

// The bitwise CRC32 below, one byte at a time: mod-channel frames are hashed
// on every send and receive, so the table form keeps that cheap.
constexpr std::array<std::uint32_t, 256U> crc32_table = [] {
    std::array<std::uint32_t, 256U> table{};
    for (std::uint32_t index = 0U; index < 256U; ++index)
    {
        std::uint32_t crc = index;
        for (unsigned bit = 0U; bit < 8U; ++bit)
        {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
        table[index] = crc;
    }
    return table;
}();

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t value : bytes)
        crc = (crc >> 8U) ^ crc32_table[(crc ^ value) & 0xFFU];
    return ~crc;
}

std::uint32_t read_tail_crc(std::span<const std::uint8_t> packet) noexcept
{
    const std::size_t offset = packet.size() - 4U;
    return static_cast<std::uint32_t>(packet[offset]) |
           (static_cast<std::uint32_t>(packet[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(packet[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(packet[offset + 3U]) << 24U);
}

std::vector<const ModDescriptor *> synchronized_mods(const ModManifest &manifest)
{
    std::vector<const ModDescriptor *> result;
    result.reserve(manifest.mods.size());
    for (const ModDescriptor &mod : manifest.mods)
    {
        if (mod.scope != ModScope::client)
            result.push_back(&mod);
    }
    return result;
}

} // namespace

bool is_extension_packet(std::span<const std::uint8_t> packet) noexcept
{
    return packet.size() >= extension_magic.size() &&
           std::equal(extension_magic.begin(), extension_magic.end(), packet.begin());
}

std::optional<ExtensionMessageKind> extension_message_kind(
    std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < extension_magic.size() + 2U || !is_extension_packet(packet) ||
        packet[extension_magic.size()] != extension_protocol_version)
        return std::nullopt;
    const std::uint8_t kind = packet[extension_magic.size() + 1U];
    const bool handshake = kind >= static_cast<std::uint8_t>(ExtensionMessageKind::client_hello) &&
                           kind <= static_cast<std::uint8_t>(ExtensionMessageKind::server_reject);
    const bool channel = kind >= static_cast<std::uint8_t>(ExtensionMessageKind::mod_data) &&
                         kind <= static_cast<std::uint8_t>(ExtensionMessageKind::download_refusal);
    if (!handshake && !channel)
        return std::nullopt;
    return static_cast<ExtensionMessageKind>(kind);
}

bool is_mod_channel_packet(std::span<const std::uint8_t> packet) noexcept
{
    return extension_message_kind(packet) == ExtensionMessageKind::mod_data;
}

bool is_mod_download_packet(std::span<const std::uint8_t> packet) noexcept
{
    const auto kind = extension_message_kind(packet);
    return kind.has_value() && *kind >= ExtensionMessageKind::download_request &&
           *kind <= ExtensionMessageKind::download_refusal;
}

std::uint32_t extension_crc32(std::span<const std::uint8_t> bytes) noexcept
{
    return crc32(bytes);
}

bool validate_mod_manifest(const ModManifest &manifest, std::string &error) noexcept
{
    if (!supported_protocol(manifest.protocol))
    {
        error = "unsupported game protocol";
        return false;
    }

    std::unordered_set<std::string_view> synchronized_ids;
    std::size_t synchronized_count{};
    // Fixed header, empty detail prefix, and CRC32.
    std::size_t encoded_size = 23U;
    for (const ModDescriptor &mod : manifest.mods)
    {
        if (mod.scope != ModScope::client && mod.scope != ModScope::server &&
            mod.scope != ModScope::core)
        {
            error = "invalid mod scope";
            return false;
        }
        if (!valid_mod_id(mod.id))
        {
            error = "mod ids must use 1-96 ASCII letters, digits, dots, underscores, or dashes";
            return false;
        }
        if (!valid_version(mod.version))
        {
            error = "mod versions must contain 1-48 printable bytes";
            return false;
        }
        if (mod.scope == ModScope::client)
            continue;
        ++synchronized_count;
        if (synchronized_count > maximum_synchronized_mods)
        {
            error = "too many synchronized mods";
            return false;
        }
        if (!synchronized_ids.insert(mod.id).second)
        {
            error = "duplicate synchronized mod id";
            return false;
        }
        if (all_zero(mod.content_digest))
        {
            error = "synchronized mods require a nonzero content digest";
            return false;
        }
        encoded_size += 3U + seven_bit_prefix_size(mod.id.size()) + mod.id.size() +
                        seven_bit_prefix_size(mod.version.size()) + mod.version.size() +
                        mod.content_digest.size();
        if (encoded_size > maximum_extension_packet_size)
        {
            error = "synchronized mod manifest exceeds the extension wire limit";
            return false;
        }
    }
    error.clear();
    return true;
}

bool requires_extension(const ModManifest &manifest) noexcept
{
    return manifest.openminerz_only ||
           std::any_of(manifest.mods.begin(), manifest.mods.end(), [](const ModDescriptor &mod) {
               return mod.scope == ModScope::server || mod.scope == ModScope::core;
           });
}

std::vector<std::uint8_t> encode_extension_message(const ExtensionMessage &message)
{
    std::string manifest_error;
    if (!validate_mod_manifest(message.manifest, manifest_error))
        throw std::invalid_argument("invalid mod manifest: " + manifest_error);
    if (message.detail.size() > maximum_extension_detail_bytes)
        throw std::invalid_argument("extension detail exceeds the protocol limit");
    if (message.kind == ExtensionMessageKind::client_hello && !message.detail.empty())
        throw std::invalid_argument("client hello cannot contain a detail string");

    const auto mods = synchronized_mods(message.manifest);
    BoundedWriter writer{maximum_extension_packet_size - 4U};
    bool valid = writer.write_bytes(extension_magic) &&
                 writer.write_u8(extension_protocol_version) &&
                 writer.write_u8(static_cast<std::uint8_t>(message.kind)) &&
                 writer.write_u8(static_cast<std::uint8_t>(message.manifest.protocol)) &&
                 writer.write_u8(static_cast<std::uint8_t>(
                     (mods.empty() ? 0U : extension_flag_mods) |
                     (message.manifest.openminerz_only ? extension_flag_openminerz_only : 0U))) &&
                 writer.write_u64(message.nonce) &&
                 writer.write_u16(static_cast<std::uint16_t>(mods.size()));
    for (std::size_t index = 0U; valid && index < mods.size(); ++index)
    {
        const ModDescriptor &mod = *mods[index];
        valid = writer.write_u8(static_cast<std::uint8_t>(mod.scope)) &&
                writer.write_u16(static_cast<std::uint16_t>(index)) &&
                writer.write_dotnet_string(mod.id, maximum_mod_id_bytes) &&
                writer.write_dotnet_string(mod.version, maximum_mod_version_bytes) &&
                writer.write_bytes(mod.content_digest);
    }
    valid = valid && writer.write_dotnet_string(message.detail, maximum_extension_detail_bytes);
    if (!valid)
        throw std::length_error("extension message exceeds the 4096-byte wire limit");

    std::vector<std::uint8_t> packet = writer.take();
    const std::uint32_t checksum = crc32(packet);
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        packet.push_back(static_cast<std::uint8_t>((checksum >> shift) & 0xFFU));
    return packet;
}

bool decode_extension_message(std::span<const std::uint8_t> packet, ExtensionMessage &message,
                              ExtensionDecodeError &error)
{
    message = {};
    constexpr std::size_t minimum_size = 23U;
    if (packet.size() < minimum_size)
    {
        error = ExtensionDecodeError::too_small;
        return false;
    }
    if (packet.size() > maximum_extension_packet_size)
    {
        error = ExtensionDecodeError::too_large;
        return false;
    }
    if (!is_extension_packet(packet))
    {
        error = ExtensionDecodeError::wrong_magic;
        return false;
    }
    if (crc32(packet.first(packet.size() - 4U)) != read_tail_crc(packet))
    {
        error = ExtensionDecodeError::checksum_mismatch;
        return false;
    }

    BoundedReader reader{packet.first(packet.size() - 4U)};
    std::array<std::uint8_t, 4U> magic{};
    std::uint8_t extension_version{};
    std::uint8_t kind_value{};
    std::uint8_t protocol_value{};
    std::uint8_t flags{};
    std::uint16_t mod_count{};
    if (!reader.read_bytes(magic) || !reader.read_u8(extension_version) ||
        !reader.read_u8(kind_value) || !reader.read_u8(protocol_value) || !reader.read_u8(flags) ||
        !reader.read_u64(message.nonce) || !reader.read_u16(mod_count))
    {
        error = ExtensionDecodeError::too_small;
        return false;
    }
    if (extension_version != extension_protocol_version)
    {
        error = ExtensionDecodeError::unsupported_extension_version;
        return false;
    }
    if (kind_value < static_cast<std::uint8_t>(ExtensionMessageKind::client_hello) ||
        kind_value > static_cast<std::uint8_t>(ExtensionMessageKind::server_reject))
    {
        error = ExtensionDecodeError::unknown_message_kind;
        return false;
    }
    if (protocol_value != static_cast<std::uint8_t>(ProtocolVersion::classic_v3) &&
        protocol_value != static_cast<std::uint8_t>(ProtocolVersion::modern_v4))
    {
        error = ExtensionDecodeError::unsupported_game_protocol;
        return false;
    }
    if ((flags & ~(extension_flag_mods | extension_flag_openminerz_only)) != 0U ||
        ((flags & extension_flag_mods) != 0U) != (mod_count != 0U))
    {
        error = ExtensionDecodeError::invalid_flags;
        return false;
    }
    if (mod_count > maximum_synchronized_mods)
    {
        error = ExtensionDecodeError::too_many_mods;
        return false;
    }

    message.kind = static_cast<ExtensionMessageKind>(kind_value);
    message.manifest.protocol = static_cast<ProtocolVersion>(protocol_value);
    message.manifest.openminerz_only = (flags & extension_flag_openminerz_only) != 0U;
    message.manifest.mods.reserve(mod_count);
    for (std::uint16_t index = 0U; index < mod_count; ++index)
    {
        ModDescriptor mod;
        std::uint8_t scope{};
        std::uint16_t order{};
        if (!reader.read_u8(scope))
        {
            error = ExtensionDecodeError::too_small;
            return false;
        }
        if (scope != static_cast<std::uint8_t>(ModScope::server) &&
            scope != static_cast<std::uint8_t>(ModScope::core))
        {
            error = ExtensionDecodeError::invalid_mod_scope;
            return false;
        }
        if (!reader.read_u16(order) || order != index)
        {
            error = ExtensionDecodeError::invalid_mod_order;
            return false;
        }
        mod.scope = static_cast<ModScope>(scope);
        if (!reader.read_dotnet_string(mod.id, maximum_mod_id_bytes) ||
            !reader.read_dotnet_string(mod.version, maximum_mod_version_bytes) ||
            !reader.read_bytes(mod.content_digest))
        {
            error = ExtensionDecodeError::invalid_string;
            return false;
        }
        message.manifest.mods.push_back(std::move(mod));
    }
    if (!reader.read_dotnet_string(message.detail, maximum_extension_detail_bytes))
    {
        error = ExtensionDecodeError::invalid_detail;
        return false;
    }
    if (message.kind == ExtensionMessageKind::client_hello && !message.detail.empty())
    {
        error = ExtensionDecodeError::invalid_detail;
        return false;
    }
    if (!reader.exhausted())
    {
        error = ExtensionDecodeError::trailing_data;
        return false;
    }
    std::string manifest_error;
    if (!validate_mod_manifest(message.manifest, manifest_error))
    {
        error = ExtensionDecodeError::invalid_manifest;
        return false;
    }
    error = ExtensionDecodeError::none;
    return true;
}

ModCompatibilityResult compare_mod_manifests(const ModManifest &host, const ModManifest *peer,
                                             bool peer_advertised_extension)
{
    std::string error;
    if (!validate_mod_manifest(host, error))
        return {ModCompatibility::invalid_manifest, "Host manifest is invalid: " + error};

    if (!peer_advertised_extension)
    {
        if (requires_extension(host))
            return {ModCompatibility::extension_required,
                    synchronized_mods(host).empty()
                        ? "This host admits OpenMinerZ players only."
                        : "This host requires OpenMinerZ server/core mods."};
        return {};
    }
    if (peer == nullptr || !validate_mod_manifest(*peer, error))
        return {ModCompatibility::invalid_manifest, "Peer manifest is invalid: " + error};
    if (host.protocol != peer->protocol)
        return {ModCompatibility::protocol_mismatch,
                std::format("Protocol {} is required; peer offered {}.",
                            static_cast<unsigned>(host.protocol),
                            static_cast<unsigned>(peer->protocol))};

    const auto required = synchronized_mods(host);
    const auto offered = synchronized_mods(*peer);
    const std::size_t common = std::min(required.size(), offered.size());
    for (std::size_t index = 0U; index < common; ++index)
    {
        const ModDescriptor &expected = *required[index];
        const ModDescriptor &actual = *offered[index];
        if (expected.id != actual.id)
        {
            const auto expected_later = std::find_if(
                offered.begin() + static_cast<std::ptrdiff_t>(index + 1U), offered.end(),
                [&expected](const ModDescriptor *mod) { return mod->id == expected.id; });
            const auto actual_later = std::find_if(
                required.begin() + static_cast<std::ptrdiff_t>(index + 1U), required.end(),
                [&actual](const ModDescriptor *mod) { return mod->id == actual.id; });
            if (expected_later != offered.end() || actual_later != required.end())
                return {
                    ModCompatibility::order_mismatch,
                    std::format("Mod order differs at slot {} (expected {}).", index, expected.id)};
            return {ModCompatibility::missing_mod,
                    std::format("Required mod {} is missing at slot {}.", expected.id, index)};
        }
        if (expected.scope != actual.scope)
            return {ModCompatibility::scope_mismatch,
                    std::format("Mod {} has the wrong synchronization scope.", expected.id)};
        if (expected.version != actual.version)
            return {ModCompatibility::version_mismatch,
                    std::format("Mod {} requires version {}.", expected.id, expected.version)};
        if (expected.content_digest != actual.content_digest)
            return {ModCompatibility::digest_mismatch,
                    std::format("Mod {} has different content.", expected.id)};
    }
    if (required.size() > offered.size())
        return {ModCompatibility::missing_mod,
                std::format("Required mod {} is missing.", required[offered.size()]->id)};
    if (offered.size() > required.size())
        return {
            ModCompatibility::unexpected_mod,
            std::format("Peer has unexpected synchronized mod {}.", offered[required.size()]->id)};
    return {};
}

} // namespace openminerz::net
