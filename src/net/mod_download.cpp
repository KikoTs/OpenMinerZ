#include "openminerz/net/mod_download.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <random>
#include <set>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::array<std::uint8_t, 4U> download_magic{'O', 'M', 'Z', 'E'};
constexpr std::size_t download_header_bytes = 6U;
constexpr std::size_t maximum_archive_path_bytes = 1024U;
constexpr std::size_t maximum_refusal_detail_bytes = 192U;
constexpr std::size_t maximum_request_mods = maximum_synchronized_mods;

BoundedWriter begin_frame(ExtensionMessageKind kind)
{
    BoundedWriter writer{maximum_extension_packet_size - 4U};
    static_cast<void>(writer.write_bytes(download_magic));
    static_cast<void>(writer.write_u8(extension_protocol_version));
    static_cast<void>(writer.write_u8(static_cast<std::uint8_t>(kind)));
    return writer;
}

std::vector<std::uint8_t> finish_frame(BoundedWriter &writer, bool valid)
{
    if (!valid)
        return {};
    std::vector<std::uint8_t> packet = writer.take();
    const std::uint32_t checksum = extension_crc32(packet);
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        packet.push_back(static_cast<std::uint8_t>((checksum >> shift) & 0xFFU));
    return packet;
}

// The body of a well-formed download frame of `kind`, CRC checked.
std::optional<std::span<const std::uint8_t>> open_frame(std::span<const std::uint8_t> packet,
                                                        ExtensionMessageKind kind) noexcept
{
    if (packet.size() < download_header_bytes + 4U ||
        packet.size() > maximum_extension_packet_size || extension_message_kind(packet) != kind)
        return std::nullopt;
    const std::size_t tail = packet.size() - 4U;
    const std::uint32_t checksum = static_cast<std::uint32_t>(packet[tail]) |
                                   (static_cast<std::uint32_t>(packet[tail + 1U]) << 8U) |
                                   (static_cast<std::uint32_t>(packet[tail + 2U]) << 16U) |
                                   (static_cast<std::uint32_t>(packet[tail + 3U]) << 24U);
    if (extension_crc32(packet.first(tail)) != checksum)
        return std::nullopt;
    return packet.subspan(download_header_bytes, tail - download_header_bytes);
}

bool has_non_whitespace(std::string_view text) noexcept
{
    return std::any_of(text.begin(), text.end(),
                       [](unsigned char character) { return std::isspace(character) == 0; });
}

bool constant_time_equal(std::string_view expected, std::string_view actual) noexcept
{
    const std::size_t count = std::max(expected.size(), actual.size());
    std::size_t difference = expected.size() ^ actual.size();
    for (std::size_t index = 0U; index < count; ++index)
    {
        const unsigned char left =
            index < expected.size() ? static_cast<unsigned char>(expected[index]) : 0U;
        const unsigned char right =
            index < actual.size() ? static_cast<unsigned char>(actual[index]) : 0U;
        difference |= static_cast<std::size_t>(left ^ right);
    }
    return difference == 0U;
}

// A path segment Windows would map to a device, whatever its extension.
bool windows_device_name(std::string_view segment) noexcept
{
    const std::string_view stem = segment.substr(0U, segment.find('.'));
    for (const std::string_view device : {"con", "prn", "aux", "nul", "conin$", "conout$"})
        if (stem == device)
            return true;
    return stem.size() == 4U && (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem[3] >= '0' && stem[3] <= '9';
}

bool safe_archive_path(std::string_view path) noexcept
{
    if (path.empty() || path.size() > maximum_archive_path_bytes || path.front() == '/' ||
        path.back() == '/')
        return false;
    if (!std::all_of(path.begin(), path.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                   character == '-' || character == '/';
        }))
        return false;
    std::size_t start{};
    while (start <= path.size())
    {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        // Windows drops trailing dots, so "a." and "a" would be one file.
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
            windows_device_name(segment))
            return false;
        start = end + 1U;
    }
    return is_client_visible_mod_path(path);
}

std::uint32_t random_request_id()
{
    std::random_device random;
    std::uint32_t id = random();
    return id == 0U ? 1U : id;
}

std::string format_bytes(std::uint64_t bytes)
{
    if (bytes >= 1024U * 1024U)
        return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024U)
        return std::format("{:.0f} KB", static_cast<double>(bytes) / 1024.0);
    return std::format("{} bytes", bytes);
}

} // namespace

// --- Archive -------------------------------------------------------------------

bool build_client_archive(const std::filesystem::path &root, std::uint64_t maximum_bytes,
                          ModArchive &archive, std::string &error)
{
    std::vector<ModContentFile> files;
    if (!list_mod_content(root, files, error))
        return false;
    std::erase_if(
        files, [](const ModContentFile &file) { return !is_client_visible_mod_path(file.path); });
    std::uint64_t total{};
    for (const ModContentFile &file : files)
    {
        if (!safe_archive_path(file.path))
        {
            error = "A client-visible path cannot be sent safely: " + file.path;
            return false;
        }
        total += file.path.size() + 9U + file.size;
    }
    if (total > maximum_bytes)
    {
        error = std::format("The client part of this mod is {} (limit {})", format_bytes(total),
                            format_bytes(maximum_bytes));
        return false;
    }
    if (std::none_of(files.begin(), files.end(), [](const ModContentFile &file) {
            return file.path == mod_descriptor_filename;
        }))
    {
        error = "A mod without a descriptor cannot be sent";
        return false;
    }
    ModArchive built;
    built.bytes.reserve(static_cast<std::size_t>(total));
    for (const ModContentFile &file : files)
    {
        built.bytes.insert(built.bytes.end(), file.path.begin(), file.path.end());
        built.bytes.push_back(0U);
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
            built.bytes.push_back(static_cast<std::uint8_t>((file.size >> shift) & 0xFFU));
        const std::size_t offset = built.bytes.size();
        built.bytes.resize(offset + static_cast<std::size_t>(file.size));
        std::ifstream input{file.source, std::ios::binary};
        if (!input ||
            !input.read(reinterpret_cast<char *>(built.bytes.data() + offset),
                        static_cast<std::streamsize>(file.size)) ||
            input.peek() != std::ifstream::traits_type::eof())
        {
            error = "A mod content file changed or could not be read completely";
            return false;
        }
        built.files.push_back({file.path, offset, file.size});
    }
    built.client_digest = sha256_digest(built.bytes);
    archive = std::move(built);
    error.clear();
    return true;
}

bool parse_client_archive(std::span<const std::uint8_t> bytes, std::vector<ModArchiveFile> &files,
                          std::string &error)
{
    std::vector<ModArchiveFile> parsed;
    std::set<std::string, std::less<>> paths;
    std::size_t offset{};
    bool descriptor = false;
    while (offset < bytes.size())
    {
        const auto terminator =
            std::find(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(std::min(
                                          bytes.size(), offset + maximum_archive_path_bytes + 1U)),
                      std::uint8_t{0});
        if (terminator == bytes.end() || *terminator != 0U)
        {
            error = "archive path is unterminated or too long";
            return false;
        }
        const std::size_t path_end = static_cast<std::size_t>(terminator - bytes.begin());
        std::string path{reinterpret_cast<const char *>(bytes.data() + offset), path_end - offset};
        if (!safe_archive_path(path))
        {
            error = "archive holds a forbidden path: " + path;
            return false;
        }
        if (!parsed.empty() && !(parsed.back().path < path))
        {
            error = "archive paths are not in strictly ascending order";
            return false;
        }
        // A file cannot also be a folder another file lives in.
        for (std::size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1U))
            if (paths.contains(std::string_view{path}.substr(0U, slash)))
            {
                error = "archive uses a file as a folder: " + path;
                return false;
            }
        offset = path_end + 1U;
        if (bytes.size() - offset < 8U)
        {
            error = "archive is truncated";
            return false;
        }
        std::uint64_t size{};
        for (std::uint32_t index = 0U; index < 8U; ++index)
            size |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
        offset += 8U;
        if (size > bytes.size() - offset || size > maximum_mod_file_bytes)
        {
            error = "archive file size is out of range";
            return false;
        }
        descriptor = descriptor || path == mod_descriptor_filename;
        paths.insert(path);
        parsed.push_back({std::move(path), offset, size});
        offset += static_cast<std::size_t>(size);
        if (parsed.size() > maximum_mod_files)
        {
            error = "archive holds too many files";
            return false;
        }
    }
    if (!descriptor)
    {
        error = "archive has no openminerz.mod";
        return false;
    }
    files = std::move(parsed);
    error.clear();
    return true;
}

bool install_downloaded_mod(const std::filesystem::path &cache_root,
                            const ModDescriptor &descriptor, const ModDigest &client_digest,
                            std::span<const std::uint8_t> archive, std::filesystem::path &installed,
                            std::string &error)
{
    if (cache_root.empty())
    {
        error = "no mod cache folder is configured";
        return false;
    }
    if (sha256_digest(archive) != client_digest)
    {
        error = "the download does not match its digest";
        return false;
    }
    std::vector<ModArchiveFile> files;
    if (!parse_client_archive(archive, files, error))
        return false;

    std::error_code fs_error;
    const std::string name = digest_hex(descriptor.content_digest);
    const std::filesystem::path target = cache_root / name;
    LoadedMod loaded;
    if (std::filesystem::exists(target, fs_error))
    {
        std::string ignored;
        if (load_cached_mod(target, descriptor.content_digest, loaded, ignored) &&
            loaded.descriptor == descriptor)
        {
            installed = target;
            error.clear();
            return true;
        }
        // A damaged entry of our own cache: replace it.
        std::filesystem::remove_all(target, fs_error);
    }
    std::filesystem::create_directories(cache_root, fs_error);
    const std::filesystem::path staging =
        cache_root / std::format(".staging-{}-{:08x}", name.substr(0U, 16U), random_request_id());
    if (!std::filesystem::create_directory(staging, fs_error))
    {
        error = "cannot create a staging folder in the mod cache";
        return false;
    }
    const auto abandon = [&](std::string message) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        error = std::move(message);
        return false;
    };
    const std::filesystem::path base = staging.lexically_normal();
    for (const ModArchiveFile &file : files)
    {
        const std::filesystem::path destination = (staging / file.path).lexically_normal();
        // parse_client_archive already refused anything that could escape;
        // this is the last line, checked on the real path.
        const auto mismatch =
            std::mismatch(base.begin(), base.end(), destination.begin(), destination.end());
        if (mismatch.first != base.end() || destination == base)
            return abandon("archive path escapes the cache: " + file.path);
        std::filesystem::create_directories(destination.parent_path(), fs_error);
        std::ofstream output{destination, std::ios::binary | std::ios::trunc};
        if (!output || !output.write(reinterpret_cast<const char *>(archive.data() + file.offset),
                                     static_cast<std::streamsize>(file.size)))
            return abandon("cannot write " + file.path + " into the mod cache");
    }
    {
        std::ofstream record{staging / mod_download_record_filename, std::ios::binary};
        const std::string text = format_mod_download_record(descriptor, client_digest);
        if (!record || !record.write(text.data(), static_cast<std::streamsize>(text.size())))
            return abandon("cannot write the download record");
    }
    if (!load_cached_mod(staging, descriptor.content_digest, loaded, error))
        return abandon("downloaded mod is invalid: " + error);
    if (loaded.descriptor != descriptor)
        return abandon("the downloaded descriptor does not match what the host offered");
    std::filesystem::rename(staging, target, fs_error);
    if (fs_error)
    {
        // Another join installed it first; keep that one if it is valid.
        std::string ignored;
        std::filesystem::remove_all(staging, fs_error);
        if (!load_cached_mod(target, descriptor.content_digest, loaded, ignored))
        {
            error = "cannot move the download into the mod cache";
            return false;
        }
    }
    installed = target;
    error.clear();
    return true;
}

// --- Wire --------------------------------------------------------------------

std::vector<std::uint8_t> encode_download_request(const ModDownloadRequest &message)
{
    BoundedWriter writer = begin_frame(ExtensionMessageKind::download_request);
    bool valid = !message.mod_ids.empty() && message.mod_ids.size() <= maximum_request_mods &&
                 writer.write_u32(message.request) &&
                 writer.write_dotnet_string(message.password, maximum_download_password_bytes) &&
                 writer.write_u8(static_cast<std::uint8_t>(message.mod_ids.size()));
    for (const std::string &id : message.mod_ids)
        valid = valid && writer.write_dotnet_string(id, maximum_mod_id_bytes);
    return finish_frame(writer, valid);
}

std::vector<std::uint8_t> encode_download_offer(const ModDownloadOffer &message)
{
    BoundedWriter writer = begin_frame(ExtensionMessageKind::download_offer);
    bool valid = writer.write_u32(message.request) && writer.write_u8(message.slot) &&
                 writer.write_u8(static_cast<std::uint8_t>(message.status)) &&
                 writer.write_dotnet_string(message.descriptor.id, maximum_mod_id_bytes);
    if (message.status == ModOfferStatus::available)
        valid = valid && writer.write_u8(static_cast<std::uint8_t>(message.descriptor.scope)) &&
                writer.write_dotnet_string(message.descriptor.version, maximum_mod_version_bytes) &&
                writer.write_bytes(message.descriptor.content_digest) &&
                writer.write_bytes(message.client_digest) && writer.write_u64(message.bytes) &&
                writer.write_u32(message.files) && writer.write_u32(message.chunks);
    return finish_frame(writer, valid);
}

std::vector<std::uint8_t> encode_chunk_request(const ModChunkRequest &message)
{
    BoundedWriter writer = begin_frame(ExtensionMessageKind::download_chunk_request);
    const bool valid = writer.write_u32(message.request) && writer.write_u8(message.slot) &&
                       writer.write_u32(message.first) && writer.write_u16(message.count);
    return finish_frame(writer, valid);
}

std::vector<std::uint8_t> encode_chunk(const ModChunk &message)
{
    BoundedWriter writer = begin_frame(ExtensionMessageKind::download_chunk);
    const bool valid = message.bytes.size() <= mod_download_chunk_bytes &&
                       writer.write_u32(message.request) && writer.write_u8(message.slot) &&
                       writer.write_u32(message.index) &&
                       writer.write_u16(static_cast<std::uint16_t>(message.bytes.size())) &&
                       writer.write_bytes(message.bytes);
    return finish_frame(writer, valid);
}

std::vector<std::uint8_t> encode_download_refusal(const ModDownloadRefusal &message)
{
    BoundedWriter writer = begin_frame(ExtensionMessageKind::download_refusal);
    const bool valid =
        writer.write_u32(message.request) &&
        writer.write_u8(static_cast<std::uint8_t>(message.reason)) &&
        writer.write_dotnet_string(message.detail.substr(0U, maximum_refusal_detail_bytes),
                                   maximum_refusal_detail_bytes);
    return finish_frame(writer, valid);
}

bool decode_download_request(std::span<const std::uint8_t> packet,
                             ModDownloadRequest &message) noexcept
{
    try
    {
        message = {};
        const auto body = open_frame(packet, ExtensionMessageKind::download_request);
        if (!body)
            return false;
        BoundedReader reader{*body};
        std::uint8_t count{};
        if (!reader.read_u32(message.request) ||
            !reader.read_dotnet_string(message.password, maximum_download_password_bytes) ||
            !reader.read_u8(count) || count == 0U || count > maximum_request_mods)
            return false;
        for (std::uint8_t index = 0U; index < count; ++index)
        {
            std::string id;
            if (!reader.read_dotnet_string(id, maximum_mod_id_bytes) || id.empty())
                return false;
            message.mod_ids.push_back(std::move(id));
        }
        return message.request != 0U && reader.exhausted();
    }
    catch (...)
    {
        return false;
    }
}

bool decode_download_offer(std::span<const std::uint8_t> packet, ModDownloadOffer &message) noexcept
{
    try
    {
        message = {};
        const auto body = open_frame(packet, ExtensionMessageKind::download_offer);
        if (!body)
            return false;
        BoundedReader reader{*body};
        std::uint8_t status{};
        if (!reader.read_u32(message.request) || !reader.read_u8(message.slot) ||
            !reader.read_u8(status) ||
            status > static_cast<std::uint8_t>(ModOfferStatus::unavailable) ||
            !reader.read_dotnet_string(message.descriptor.id, maximum_mod_id_bytes))
            return false;
        message.status = static_cast<ModOfferStatus>(status);
        if (message.status == ModOfferStatus::available)
        {
            std::uint8_t scope{};
            if (!reader.read_u8(scope) ||
                (scope != static_cast<std::uint8_t>(ModScope::server) &&
                 scope != static_cast<std::uint8_t>(ModScope::core)) ||
                !reader.read_dotnet_string(message.descriptor.version, maximum_mod_version_bytes) ||
                !reader.read_bytes(message.descriptor.content_digest) ||
                !reader.read_bytes(message.client_digest) || !reader.read_u64(message.bytes) ||
                !reader.read_u32(message.files) || !reader.read_u32(message.chunks))
                return false;
            message.descriptor.scope = static_cast<ModScope>(scope);
        }
        return reader.exhausted();
    }
    catch (...)
    {
        return false;
    }
}

bool decode_chunk_request(std::span<const std::uint8_t> packet, ModChunkRequest &message) noexcept
{
    message = {};
    const auto body = open_frame(packet, ExtensionMessageKind::download_chunk_request);
    if (!body)
        return false;
    BoundedReader reader{*body};
    return reader.read_u32(message.request) && reader.read_u8(message.slot) &&
           reader.read_u32(message.first) && reader.read_u16(message.count) &&
           message.count != 0U && reader.exhausted();
}

bool decode_chunk(std::span<const std::uint8_t> packet, ModChunk &message) noexcept
{
    try
    {
        message = {};
        const auto body = open_frame(packet, ExtensionMessageKind::download_chunk);
        if (!body)
            return false;
        BoundedReader reader{*body};
        std::uint16_t length{};
        if (!reader.read_u32(message.request) || !reader.read_u8(message.slot) ||
            !reader.read_u32(message.index) || !reader.read_u16(length) || length == 0U ||
            length > mod_download_chunk_bytes || reader.remaining() != length)
            return false;
        message.bytes.resize(length);
        return reader.read_bytes(message.bytes) && reader.exhausted();
    }
    catch (...)
    {
        return false;
    }
}

bool decode_download_refusal(std::span<const std::uint8_t> packet,
                             ModDownloadRefusal &message) noexcept
{
    try
    {
        message = {};
        const auto body = open_frame(packet, ExtensionMessageKind::download_refusal);
        if (!body)
            return false;
        BoundedReader reader{*body};
        std::uint8_t reason{};
        if (!reader.read_u32(message.request) || !reader.read_u8(reason) ||
            reason < static_cast<std::uint8_t>(ModDownloadRefusalReason::disabled) ||
            reason > static_cast<std::uint8_t>(ModDownloadRefusalReason::protocol) ||
            !reader.read_dotnet_string(message.detail, maximum_refusal_detail_bytes))
            return false;
        message.reason = static_cast<ModDownloadRefusalReason>(reason);
        return reader.exhausted();
    }
    catch (...)
    {
        return false;
    }
}

// --- Host --------------------------------------------------------------------

ModDownloadServer::ModDownloadServer(const ModManifest &manifest, std::vector<LoadedMod> sources,
                                     ModDownloadServerOptions options)
    : enabled_{requires_extension(manifest)}, options_{std::move(options)},
      total_{options_.total_bytes_per_second, options_.total_bytes_per_second}
{
    // Only what the session requires is ever offered, from the host's own copy.
    for (const ModDescriptor &wanted : manifest.mods)
    {
        if (wanted.scope == ModScope::client)
            continue;
        const auto found = std::find_if(sources.begin(), sources.end(), [&](const LoadedMod &mod) {
            return mod.descriptor == wanted;
        });
        if (found != sources.end())
            sources_.push_back({*found, nullptr, {}, false});
    }
}

bool ModDownloadServer::prepare(std::string &error)
{
    for (std::size_t index = 0U; index < sources_.size(); ++index)
        if (archive_for(index) == nullptr)
        {
            error = sources_[index].mod.descriptor.id + ": " + sources_[index].error;
            return false;
        }
    error.clear();
    return true;
}

const ModArchive *ModDownloadServer::archive_for(std::size_t source)
{
    Source &entry = sources_[source];
    if (!entry.built)
    {
        entry.built = true;
        auto archive = std::make_shared<ModArchive>();
        if (build_client_archive(entry.mod.root, options_.maximum_mod_bytes, *archive, entry.error))
            entry.archive = std::move(archive);
    }
    return entry.archive.get();
}

void ModDownloadServer::refuse(PeerId peer, std::uint32_t request, ModDownloadRefusalReason reason,
                               std::string detail)
{
    ++counters_.refusals;
    sends_.push_back({peer, encode_download_refusal({request, reason, std::move(detail)})});
}

void ModDownloadServer::handle_request(PeerId peer, PeerState &state,
                                       const ModDownloadRequest &request, double now)
{
    if (has_non_whitespace(options_.password) &&
        !constant_time_equal(options_.password, request.password))
    {
        refuse(peer, request.request, ModDownloadRefusalReason::wrong_password,
               "Incorrect session password.");
        peers_.erase(peer);
        return;
    }
    state.request = request.request;
    state.slots.assign(request.mod_ids.size(), sources_.size());
    state.offered_bytes = 0U;
    state.queue.clear();
    state.queue_head = 0U;
    state.last_activity = now;
    for (std::size_t slot = 0U; slot < request.mod_ids.size(); ++slot)
    {
        ModDownloadOffer offer;
        offer.request = request.request;
        offer.slot = static_cast<std::uint8_t>(slot);
        offer.descriptor.id = request.mod_ids[slot];
        const auto source =
            std::find_if(sources_.begin(), sources_.end(), [&](const Source &entry) {
                return entry.mod.descriptor.id == request.mod_ids[slot];
            });
        if (source == sources_.end())
            offer.status = ModOfferStatus::unknown_mod;
        else
        {
            const std::size_t index = static_cast<std::size_t>(source - sources_.begin());
            const ModArchive *archive = archive_for(index);
            if (archive == nullptr)
                offer.status = source->error.starts_with("The client part of this mod is")
                                   ? ModOfferStatus::too_large
                                   : ModOfferStatus::unavailable;
            else
            {
                offer.descriptor = source->mod.descriptor;
                offer.client_digest = archive->client_digest;
                offer.bytes = archive->bytes.size();
                offer.files = static_cast<std::uint32_t>(archive->files.size());
                offer.chunks = mod_download_chunk_count(offer.bytes);
                state.slots[slot] = index;
                state.offered_bytes += offer.bytes;
            }
        }
        sends_.push_back({peer, encode_download_offer(offer)});
    }
}

void ModDownloadServer::receive(PeerId peer, std::span<const std::uint8_t> frame, double now)
{
    if (!enabled_ || peer == 0U)
        return;
    const auto kind = extension_message_kind(frame);
    auto found = peers_.find(peer);
    if (found != peers_.end() && !found->second.frames.take(1.0, now))
    {
        ++counters_.frames_rejected;
        return;
    }
    if (kind == ExtensionMessageKind::download_request)
    {
        ModDownloadRequest request;
        if (!decode_download_request(frame, request))
        {
            ++counters_.frames_rejected;
            return;
        }
        ++counters_.requests;
        if (request_history_.size() > 4096U)
            request_history_.clear();
        if (++request_history_[peer] > options_.maximum_requests_per_peer)
        {
            refuse(peer, request.request, ModDownloadRefusalReason::quota_exceeded,
                   "Too many download requests.");
            if (found != peers_.end())
                peers_.erase(found);
            return;
        }
        if (found == peers_.end())
        {
            if (peers_.size() >= options_.maximum_concurrent_peers)
            {
                refuse(peer, request.request, ModDownloadRefusalReason::busy,
                       "The host is serving other downloads. Try again shortly.");
                return;
            }
            PeerState state;
            state.frames = {options_.request_frames_per_second, options_.request_frame_burst};
            state.bytes = {options_.peer_bytes_per_second, options_.peer_burst_bytes};
            static_cast<void>(state.frames.take(1.0, now));
            static_cast<void>(state.bytes.available(now));
            found = peers_.emplace(peer, std::move(state)).first;
        }
        handle_request(peer, found->second, request, now);
        return;
    }
    if (kind == ExtensionMessageKind::download_chunk_request)
    {
        ModChunkRequest request;
        // Without a request on file nothing is answered, so a stray or spoofed
        // chunk request cannot make the host send anything.
        if (found == peers_.end() || !decode_chunk_request(frame, request) ||
            request.request != found->second.request)
        {
            ++counters_.frames_rejected;
            return;
        }
        PeerState &state = found->second;
        const bool valid_slot =
            request.slot < state.slots.size() && state.slots[request.slot] < sources_.size();
        const ModArchive *archive = valid_slot ? archive_for(state.slots[request.slot]) : nullptr;
        const std::uint32_t chunks =
            archive == nullptr ? 0U : mod_download_chunk_count(archive->bytes.size());
        const std::size_t queued = state.queue.size() - state.queue_head;
        if (archive == nullptr || request.count > options_.maximum_window_chunks ||
            request.first >= chunks || request.count > chunks - request.first ||
            queued + request.count > 2U * options_.maximum_window_chunks)
        {
            refuse(peer, request.request, ModDownloadRefusalReason::protocol,
                   "Invalid chunk request.");
            peers_.erase(found);
            return;
        }
        for (std::uint32_t index = 0U; index < request.count; ++index)
            state.queue.emplace_back(request.slot, request.first + index);
        state.last_activity = now;
        return;
    }
    ++counters_.frames_rejected;
}

void ModDownloadServer::update(double now)
{
    if (!enabled_)
        return;
    std::erase_if(peers_, [&](const auto &entry) {
        return now - entry.second.last_activity > options_.idle_timeout_seconds;
    });
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (auto peer = peers_.begin(); peer != peers_.end();)
        {
            PeerState &state = peer->second;
            if (state.queue_head >= state.queue.size())
            {
                state.queue.clear();
                state.queue_head = 0U;
                ++peer;
                continue;
            }
            const auto [slot, index] = state.queue[state.queue_head];
            const ModArchive *archive = archive_for(state.slots[slot]);
            const std::size_t offset = static_cast<std::size_t>(index) * mod_download_chunk_bytes;
            const std::size_t size =
                std::min(mod_download_chunk_bytes, archive->bytes.size() - offset);
            const double cost = static_cast<double>(size);
            if (state.bytes.available(now) < cost || total_.available(now) < cost)
            {
                ++peer;
                continue;
            }
            if (static_cast<double>(state.served_bytes + size) >
                static_cast<double>(state.offered_bytes) * options_.peer_quota_factor + 65536.0)
            {
                refuse(peer->first, state.request, ModDownloadRefusalReason::quota_exceeded,
                       "Download quota exceeded.");
                peer = peers_.erase(peer);
                continue;
            }
            state.bytes.consume(cost, now);
            total_.consume(cost, now);
            ModChunk chunk;
            chunk.request = state.request;
            chunk.slot = slot;
            chunk.index = index;
            chunk.bytes.assign(archive->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                               archive->bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
            sends_.push_back({peer->first, encode_chunk(chunk)});
            ++counters_.chunks_sent;
            counters_.bytes_sent += size;
            state.served_bytes += size;
            state.last_activity = now;
            ++state.queue_head;
            progress = true;
            ++peer;
        }
    }
}

std::vector<ModDownloadSend> ModDownloadServer::take_sends()
{
    return std::exchange(sends_, {});
}

void ModDownloadServer::forget(PeerId peer) noexcept
{
    peers_.erase(peer);
}

// --- Client ------------------------------------------------------------------

const char *mod_download_failure_name(ModDownloadFailure failure) noexcept
{
    switch (failure)
    {
    case ModDownloadFailure::none:
        return "none";
    case ModDownloadFailure::declined:
        return "declined";
    case ModDownloadFailure::host_refused:
        return "host_refused";
    case ModDownloadFailure::wrong_password:
        return "wrong_password";
    case ModDownloadFailure::mod_unavailable:
        return "mod_unavailable";
    case ModDownloadFailure::too_large:
        return "too_large";
    case ModDownloadFailure::offer_mismatch:
        return "offer_mismatch";
    case ModDownloadFailure::digest_mismatch:
        return "digest_mismatch";
    case ModDownloadFailure::invalid_archive:
        return "invalid_archive";
    case ModDownloadFailure::timeout:
        return "timeout";
    case ModDownloadFailure::cache_error:
        return "cache_error";
    case ModDownloadFailure::transport:
        return "transport";
    case ModDownloadFailure::cancelled:
        return "cancelled";
    }
    return "unknown";
}

ModDownloadClient::ModDownloadClient(std::vector<ModDescriptor> needed,
                                     ModDownloadClientOptions options)
    : options_{std::move(options)}
{
    if (options_.window_chunks == 0U)
        options_.window_chunks = 1U;
    for (ModDescriptor &descriptor : needed)
    {
        ModDownloadItem item;
        item.descriptor = std::move(descriptor);
        status_.mods.push_back(std::move(item));
    }
}

void ModDownloadClient::fail(ModDownloadFailure failure, std::string detail)
{
    if (status_.finished())
        return;
    status_.phase = failure == ModDownloadFailure::declined ? ModDownloadPhase::declined
                                                            : ModDownloadPhase::failed;
    status_.failure = failure;
    status_.detail = std::move(detail);
    buffer_.clear();
    buffer_.shrink_to_fit();
}

void ModDownloadClient::start(double now)
{
    if (status_.phase != ModDownloadPhase::idle)
        return;
    if (status_.mods.empty())
    {
        status_.phase = ModDownloadPhase::complete;
        return;
    }
    if (status_.mods.size() > maximum_request_mods)
    {
        fail(ModDownloadFailure::too_large, "The host requires too many mods.");
        return;
    }
    request_ = random_request_id();
    ModDownloadRequest request;
    request.request = request_;
    request.password = options_.password.substr(0U, maximum_download_password_bytes);
    for (const ModDownloadItem &item : status_.mods)
        request.mod_ids.push_back(item.descriptor.id);
    sends_.push_back(encode_download_request(request));
    status_.phase = ModDownloadPhase::requesting;
    deadline_ = now + options_.response_timeout_seconds;
}

void ModDownloadClient::receive(std::span<const std::uint8_t> frame, double now)
{
    if (status_.finished() || status_.phase == ModDownloadPhase::idle)
        return;
    const auto kind = extension_message_kind(frame);
    if (kind == ExtensionMessageKind::download_refusal)
    {
        ModDownloadRefusal refusal;
        if (!decode_download_refusal(frame, refusal) || refusal.request != request_)
            return;
        fail(refusal.reason == ModDownloadRefusalReason::wrong_password
                 ? ModDownloadFailure::wrong_password
                 : ModDownloadFailure::host_refused,
             refusal.detail.empty() ? "The host refused the download." : refusal.detail);
        return;
    }
    if (kind == ExtensionMessageKind::download_offer)
    {
        ModDownloadOffer offer;
        if (status_.phase != ModDownloadPhase::requesting || !decode_download_offer(frame, offer) ||
            offer.request != request_ || offer.slot >= status_.mods.size())
            return;
        ModDownloadItem &item = status_.mods[offer.slot];
        if (item.offered || offer.descriptor.id != item.descriptor.id)
        {
            fail(ModDownloadFailure::offer_mismatch, "The host answered for the wrong mod.");
            return;
        }
        if (offer.status != ModOfferStatus::available)
        {
            fail(offer.status == ModOfferStatus::too_large ? ModDownloadFailure::too_large
                                                           : ModDownloadFailure::mod_unavailable,
                 std::format("The host cannot send {}.", item.descriptor.id));
            return;
        }
        // The offer must be exactly the mod the lobby advertised.
        if (offer.descriptor != item.descriptor)
        {
            fail(ModDownloadFailure::offer_mismatch,
                 std::format("The host offered a different {} than it advertises.",
                             item.descriptor.id));
            return;
        }
        if (offer.bytes == 0U || offer.files == 0U ||
            offer.chunks != mod_download_chunk_count(offer.bytes))
        {
            fail(ModDownloadFailure::offer_mismatch, "The host's offer is malformed.");
            return;
        }
        if (offer.bytes > options_.maximum_mod_bytes)
        {
            fail(ModDownloadFailure::too_large,
                 std::format("{} is {} (limit {}).", item.descriptor.id, format_bytes(offer.bytes),
                             format_bytes(options_.maximum_mod_bytes)));
            return;
        }
        item.offered = true;
        item.client_digest = offer.client_digest;
        item.bytes = offer.bytes;
        item.files = offer.files;
        deadline_ = now + options_.response_timeout_seconds;
        if (std::any_of(status_.mods.begin(), status_.mods.end(),
                        [](const ModDownloadItem &entry) { return !entry.offered; }))
            return;
        status_.total_bytes = 0U;
        for (const ModDownloadItem &entry : status_.mods)
            status_.total_bytes += entry.bytes;
        if (status_.total_bytes > options_.maximum_total_bytes)
        {
            fail(ModDownloadFailure::too_large,
                 std::format("The server's mods total {} (limit {}).",
                             format_bytes(status_.total_bytes),
                             format_bytes(options_.maximum_total_bytes)));
            return;
        }
        status_.phase = ModDownloadPhase::awaiting_consent;
        deadline_ = now + options_.consent_timeout_seconds;
        if (options_.auto_accept)
            accept(now);
        return;
    }
    if (kind == ExtensionMessageKind::download_chunk)
    {
        ModChunk chunk;
        if (status_.phase != ModDownloadPhase::downloading || !decode_chunk(frame, chunk) ||
            chunk.request != request_)
            return;
        ModDownloadItem &item = status_.mods[current_];
        const std::uint32_t chunks = mod_download_chunk_count(item.bytes);
        const std::size_t expected = static_cast<std::size_t>(std::min<std::uint64_t>(
            mod_download_chunk_bytes,
            item.bytes - static_cast<std::uint64_t>(next_chunk_) * mod_download_chunk_bytes));
        // The transport is reliable and ordered: anything else is the host's fault.
        if (chunk.slot != current_ || chunk.index != next_chunk_ || next_chunk_ >= chunks ||
            chunk.bytes.size() != expected)
        {
            fail(ModDownloadFailure::invalid_archive, "The host sent an unexpected chunk.");
            return;
        }
        buffer_.insert(buffer_.end(), chunk.bytes.begin(), chunk.bytes.end());
        item.received += chunk.bytes.size();
        status_.received_bytes += chunk.bytes.size();
        ++next_chunk_;
        deadline_ = now + options_.response_timeout_seconds;
        if (next_chunk_ == chunks)
            finish_mod(now);
        else
            request_more(now);
    }
}

void ModDownloadClient::request_more(double)
{
    const ModDownloadItem &item = status_.mods[current_];
    const std::uint32_t chunks = mod_download_chunk_count(item.bytes);
    const std::uint32_t window = options_.window_chunks;
    if (requested_until_ - next_chunk_ > window / 2U || requested_until_ >= chunks)
        return;
    const std::uint32_t count =
        std::min(window - (requested_until_ - next_chunk_), chunks - requested_until_);
    ModChunkRequest request;
    request.request = request_;
    request.slot = static_cast<std::uint8_t>(current_);
    request.first = requested_until_;
    request.count = static_cast<std::uint16_t>(count);
    sends_.push_back(encode_chunk_request(request));
    requested_until_ += count;
}

void ModDownloadClient::finish_mod(double now)
{
    ModDownloadItem &item = status_.mods[current_];
    if (sha256_digest(buffer_) != item.client_digest)
    {
        fail(ModDownloadFailure::digest_mismatch,
             std::format("{} does not match the digest the host offered.", item.descriptor.id));
        return;
    }
    std::vector<ModArchiveFile> files;
    std::string error;
    if (!parse_client_archive(buffer_, files, error))
    {
        fail(ModDownloadFailure::invalid_archive,
             std::format("{} was refused: {}.", item.descriptor.id, error));
        return;
    }
    if (files.size() != item.files)
    {
        fail(ModDownloadFailure::invalid_archive,
             std::format("{} does not hold the files the host offered.", item.descriptor.id));
        return;
    }
    std::filesystem::path installed;
    if (!install_downloaded_mod(options_.cache_root, item.descriptor, item.client_digest, buffer_,
                                installed, error))
    {
        fail(ModDownloadFailure::cache_error,
             std::format("{} could not be saved: {}.", item.descriptor.id, error));
        return;
    }
    item.installed = true;
    installed_.push_back(std::move(installed));
    buffer_.clear();
    buffer_.shrink_to_fit();
    const auto next = std::find_if(status_.mods.begin(), status_.mods.end(),
                                   [](const ModDownloadItem &entry) { return !entry.installed; });
    if (next == status_.mods.end())
    {
        status_.phase = ModDownloadPhase::complete;
        return;
    }
    current_ = static_cast<std::size_t>(next - status_.mods.begin());
    next_chunk_ = 0U;
    requested_until_ = 0U;
    buffer_.reserve(static_cast<std::size_t>(next->bytes));
    request_more(now);
}

void ModDownloadClient::update(double now)
{
    if (status_.finished() || status_.phase == ModDownloadPhase::idle)
        return;
    if (now <= deadline_)
        return;
    if (status_.phase == ModDownloadPhase::awaiting_consent)
        fail(ModDownloadFailure::declined, "No answer to the download prompt.");
    else
        fail(ModDownloadFailure::timeout, "The host stopped answering.");
}

void ModDownloadClient::accept(double now)
{
    if (status_.phase != ModDownloadPhase::awaiting_consent)
        return;
    status_.phase = ModDownloadPhase::downloading;
    current_ = 0U;
    next_chunk_ = 0U;
    requested_until_ = 0U;
    buffer_.reserve(static_cast<std::size_t>(status_.mods.front().bytes));
    deadline_ = now + options_.response_timeout_seconds;
    request_more(now);
}

void ModDownloadClient::decline()
{
    if (status_.phase == ModDownloadPhase::awaiting_consent)
        fail(ModDownloadFailure::declined, "You declined the server's mods.");
}

void ModDownloadClient::cancel()
{
    fail(ModDownloadFailure::cancelled, "The join was cancelled.");
}

std::vector<std::vector<std::uint8_t>> ModDownloadClient::take_sends()
{
    return std::exchange(sends_, {});
}

// --- Join glue ---------------------------------------------------------------

ModDownloadStatus ModJoinDownloads::status() const
{
    const std::lock_guard lock{mutex_};
    return status_;
}

bool ModJoinDownloads::awaiting_consent() const
{
    const std::lock_guard lock{mutex_};
    return status_.phase == ModDownloadPhase::awaiting_consent && !decision_.has_value();
}

void ModJoinDownloads::accept()
{
    const std::lock_guard lock{mutex_};
    decision_ = true;
}

void ModJoinDownloads::decline()
{
    const std::lock_guard lock{mutex_};
    decision_ = false;
}

void ModJoinDownloads::cancel()
{
    const std::lock_guard lock{mutex_};
    cancelled_ = true;
}

void ModJoinDownloads::publish(const ModDownloadStatus &status)
{
    const std::lock_guard lock{mutex_};
    status_ = status;
}

std::optional<bool> ModJoinDownloads::decision() const
{
    const std::lock_guard lock{mutex_};
    return decision_;
}

bool ModJoinDownloads::cancelled() const
{
    const std::lock_guard lock{mutex_};
    return cancelled_;
}

bool run_mod_join_download(ModJoinDownloads &shared, ModDownloadClient &client,
                           ModJoinDownloadIo &io, std::string &error)
{
    client.start(io.now());
    for (;;)
    {
        const double now = io.now();
        if (shared.cancelled())
            client.cancel();
        if (client.status().phase == ModDownloadPhase::awaiting_consent)
            if (const std::optional<bool> decision = shared.decision(); decision.has_value())
            {
                if (*decision)
                    client.accept(now);
                else
                    client.decline();
            }
        for (std::size_t count = 0U; count < 256U && !client.status().finished(); ++count)
        {
            std::optional<std::vector<std::uint8_t>> frame;
            if (!io.poll(frame, error))
            {
                client.cancel();
                shared.publish(client.status());
                return false;
            }
            if (!frame.has_value())
                break;
            client.receive(*frame, now);
        }
        client.update(now);
        for (const std::vector<std::uint8_t> &frame : client.take_sends())
            if (!io.send(frame, error))
            {
                client.cancel();
                shared.publish(client.status());
                return false;
            }
        shared.publish(client.status());
        if (client.status().finished())
            break;
        if (io.idle)
            io.idle();
    }
    const ModDownloadStatus &status = client.status();
    if (status.phase == ModDownloadPhase::complete)
    {
        error.clear();
        return true;
    }
    error = status.phase == ModDownloadPhase::declined ? std::string{"ModDownloadDeclined"}
                                                       : "ModDownloadFailed: " + status.detail;
    return false;
}

ModDownloadPromptText describe_mod_download(const ModDownloadStatus &status)
{
    ModDownloadPromptText text;
    const auto list = [&](bool sizes) {
        std::string lines;
        for (const ModDownloadItem &item : status.mods)
        {
            lines += std::format("  {} {} ({})", item.descriptor.id, item.descriptor.version,
                                 item.descriptor.scope == ModScope::core ? "core" : "server");
            if (sizes && item.offered)
                lines += " - " + format_bytes(item.bytes);
            lines += "\n";
        }
        return lines;
    };
    switch (status.phase)
    {
    case ModDownloadPhase::idle:
    case ModDownloadPhase::requesting:
        text.title = "CHECKING SERVER MODS";
        text.body = "This server runs mods you do not have:\n" + list(false) +
                    "Asking the host what they need...";
        text.decline_label = "CANCEL";
        break;
    case ModDownloadPhase::awaiting_consent:
        text.title = "THIS SERVER USES MODS";
        text.body = "To join, download these mods from the host:\n" + list(true) +
                    std::format("Total {}. Only scripts and data are sent (never programs); they "
                                "are kept in your mod cache for next time.",
                                format_bytes(status.total_bytes));
        text.accept_label = "DOWNLOAD AND JOIN";
        text.decline_label = "CANCEL";
        break;
    case ModDownloadPhase::downloading: {
        const double fraction = status.total_bytes == 0U
                                    ? 0.0
                                    : static_cast<double>(status.received_bytes) /
                                          static_cast<double>(status.total_bytes);
        text.title = "DOWNLOADING SERVER MODS";
        text.body = std::format("{} of {} ({:.0f}%)", format_bytes(status.received_bytes),
                                format_bytes(status.total_bytes), fraction * 100.0);
        text.decline_label = "CANCEL";
        break;
    }
    case ModDownloadPhase::complete:
        text.title = "SERVER MODS READY";
        text.body = "Joining...";
        break;
    case ModDownloadPhase::declined:
        text.title = "JOIN CANCELLED";
        text.body = status.detail.empty() ? "You declined the server's mods." : status.detail;
        break;
    case ModDownloadPhase::failed:
        text.title = "MOD DOWNLOAD FAILED";
        text.body = status.detail.empty() ? std::string{mod_download_failure_name(status.failure)}
                                          : status.detail;
        break;
    }
    return text;
}

} // namespace openminerz::net
