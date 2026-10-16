#pragma once

// Downloading a host's server mods on join.
//
// A joining OpenMinerZ client reads the host's required manifest from the
// lobby (net/mod_handshake.hpp). Any server/core mod it lacks, or holds with a
// different content digest, can be fetched from the host BEFORE the extension
// hello, then the hello carries the host's manifest exactly and admission
// proceeds as for any matching client. Only the CLIENT-VISIBLE part ever
// travels: the descriptor and the script/data files under shared/, client/,
// assets/ and lang/ (net/mod_catalog.hpp). server/ never leaves the host.
//
// Digests
//   content digest  ModDescriptor::content_digest, unchanged: the catalog
//                   digest of the whole mod folder. Admission compares it.
//   client digest   the same digest computed over the client-visible files
//                   only. For a mod with nothing else (no server/ folder,
//                   nothing outside the four folders) it EQUALS the content
//                   digest. The host advertises both in its offer.
//   The transfer is one archive whose bytes ARE the client digest's input
//   (for each file in path order: path, NUL, u64 size, bytes), so the client
//   verifies the download with one SHA-256 against the offered client digest,
//   and the offered content digest must equal the one the lobby advertised.
//   The client cannot recompute a content digest that covers server/ files it
//   never sees; it trusts the host for that binding, which is sound because
//   the host is the authority that runs the server half anyway. The cache
//   keeps both digests and re-verifies the client digest whenever it loads.
//
// Frames (each one extension packet, "OMZE" version kind ... crc32):
//   download_request        u32 request, string password, u8 count, ids
//   download_offer          u32 request, u8 slot, u8 status, string id, and
//                           when available: u8 scope, string version,
//                           content digest, client digest, u64 bytes,
//                           u32 files, u32 chunks
//   download_chunk_request  u32 request, u8 slot, u32 first, u16 count
//   download_chunk          u32 request, u8 slot, u32 index, u16 length, bytes
//   download_refusal        u32 request, u8 reason, string detail
// Only lobby members of a host whose manifest carries a server/core mod can
// talk to its download server; the session password (when set) is required;
// per-peer and global byte budgets, a concurrency cap, idle expiry and a
// per-peer total-bytes cap bound what a member can make the host send.

#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_channel.hpp"
#include "openminerz/net/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

inline constexpr std::size_t mod_download_chunk_bytes = 4000U;
inline constexpr std::uint64_t default_maximum_mod_download_bytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t default_maximum_join_download_bytes = 256U * 1024U * 1024U;
inline constexpr std::size_t maximum_download_password_bytes = 256U;

// --- Archive ---------------------------------------------------------------

struct ModArchiveFile final
{
    std::string path;
    std::size_t offset{}; // of the file's bytes inside the archive
    std::uint64_t size{};
};

struct ModArchive final
{
    std::vector<std::uint8_t> bytes;
    ModDigest client_digest{};
    std::vector<ModArchiveFile> files;
};

// Packs the client-visible part of the mod folder at `root`.
[[nodiscard]] bool build_client_archive(const std::filesystem::path &root,
                                        std::uint64_t maximum_bytes, ModArchive &archive,
                                        std::string &error);
// Validates an archive from a host before anything touches the disk: strictly
// ascending unique paths, every path client-visible (so no server/, no
// executable types), only [a-z0-9._-/] characters, no empty, "." or ".."
// segments, no Windows device names, no trailing dots, sizes inside the
// archive, the descriptor present, and nothing after the last file.
[[nodiscard]] bool parse_client_archive(std::span<const std::uint8_t> bytes,
                                        std::vector<ModArchiveFile> &files, std::string &error);
// Verifies, extracts and records one downloaded mod into
// <cache_root>/<content digest hex>/ through a staging folder and a rename,
// then re-loads it like load_mod_cache does. An existing valid entry wins.
[[nodiscard]] bool install_downloaded_mod(const std::filesystem::path &cache_root,
                                          const ModDescriptor &descriptor,
                                          const ModDigest &client_digest,
                                          std::span<const std::uint8_t> archive,
                                          std::filesystem::path &installed, std::string &error);

// --- Wire --------------------------------------------------------------------

enum class ModOfferStatus : std::uint8_t
{
    available = 0,
    unknown_mod = 1,
    too_large = 2,
    unavailable = 3
};

enum class ModDownloadRefusalReason : std::uint8_t
{
    disabled = 1,
    wrong_password = 2,
    busy = 3,
    quota_exceeded = 4,
    protocol = 5
};

struct ModDownloadRequest final
{
    std::uint32_t request{};
    std::string password;
    std::vector<std::string> mod_ids;
};

struct ModDownloadOffer final
{
    std::uint32_t request{};
    std::uint8_t slot{};
    ModOfferStatus status{ModOfferStatus::available};
    ModDescriptor descriptor; // id always; the rest when available
    ModDigest client_digest{};
    std::uint64_t bytes{};
    std::uint32_t files{};
    std::uint32_t chunks{};
};

struct ModChunkRequest final
{
    std::uint32_t request{};
    std::uint8_t slot{};
    std::uint32_t first{};
    std::uint16_t count{};
};

struct ModChunk final
{
    std::uint32_t request{};
    std::uint8_t slot{};
    std::uint32_t index{};
    std::vector<std::uint8_t> bytes;
};

struct ModDownloadRefusal final
{
    std::uint32_t request{};
    ModDownloadRefusalReason reason{ModDownloadRefusalReason::protocol};
    std::string detail;
};

[[nodiscard]] std::vector<std::uint8_t> encode_download_request(const ModDownloadRequest &message);
[[nodiscard]] std::vector<std::uint8_t> encode_download_offer(const ModDownloadOffer &message);
[[nodiscard]] std::vector<std::uint8_t> encode_chunk_request(const ModChunkRequest &message);
[[nodiscard]] std::vector<std::uint8_t> encode_chunk(const ModChunk &message);
[[nodiscard]] std::vector<std::uint8_t> encode_download_refusal(const ModDownloadRefusal &message);
[[nodiscard]] bool decode_download_request(std::span<const std::uint8_t> packet,
                                           ModDownloadRequest &message) noexcept;
[[nodiscard]] bool decode_download_offer(std::span<const std::uint8_t> packet,
                                         ModDownloadOffer &message) noexcept;
[[nodiscard]] bool decode_chunk_request(std::span<const std::uint8_t> packet,
                                        ModChunkRequest &message) noexcept;
[[nodiscard]] bool decode_chunk(std::span<const std::uint8_t> packet, ModChunk &message) noexcept;
[[nodiscard]] bool decode_download_refusal(std::span<const std::uint8_t> packet,
                                           ModDownloadRefusal &message) noexcept;

[[nodiscard]] constexpr std::uint32_t mod_download_chunk_count(std::uint64_t bytes) noexcept
{
    return static_cast<std::uint32_t>((bytes + mod_download_chunk_bytes - 1U) /
                                      mod_download_chunk_bytes);
}

// --- Host --------------------------------------------------------------------

struct ModDownloadServerOptions final
{
    // The session password; "" when the session has none.
    std::string password;
    std::uint64_t maximum_mod_bytes{default_maximum_mod_download_bytes};
    std::size_t maximum_concurrent_peers{4U};
    double peer_bytes_per_second{2.0 * 1024.0 * 1024.0};
    double peer_burst_bytes{256.0 * 1024.0};
    double total_bytes_per_second{8.0 * 1024.0 * 1024.0};
    double request_frames_per_second{64.0};
    double request_frame_burst{128.0};
    double idle_timeout_seconds{120.0};
    std::uint16_t maximum_window_chunks{64U};
    // Requests (download_request frames) one peer may make per session.
    std::uint32_t maximum_requests_per_peer{6U};
    // A peer may pull this many times the size of what it was offered.
    double peer_quota_factor{3.0};
};

struct ModDownloadSend final
{
    PeerId peer{};
    std::vector<std::uint8_t> frame;
};

struct ModDownloadServerCounters final
{
    std::uint64_t requests{};
    std::uint64_t refusals{};
    std::uint64_t chunks_sent{};
    std::uint64_t bytes_sent{};
    std::uint64_t frames_rejected{};
};

class ModDownloadServer final
{
  public:
    // `manifest` is the session manifest; `sources` holds the LoadedMod of
    // every synchronized mod in it (the host's own catalog entries).
    ModDownloadServer(const ModManifest &manifest, std::vector<LoadedMod> sources,
                      ModDownloadServerOptions options = {});

    // False when the manifest has no server/core mod: nothing is ever served.
    [[nodiscard]] bool enabled() const noexcept
    {
        return enabled_;
    }
    // Builds every archive now (host start) instead of on first request.
    [[nodiscard]] bool prepare(std::string &error);
    // A download frame from lobby member `peer`, as the transport identified it.
    void receive(PeerId peer, std::span<const std::uint8_t> frame, double now);
    // Serves queued chunks within the byte budgets; expires idle peers.
    void update(double now);
    [[nodiscard]] std::vector<ModDownloadSend> take_sends();
    // The member left the lobby (or was admitted): drop its download state.
    void forget(PeerId peer) noexcept;
    [[nodiscard]] std::size_t active_peers() const noexcept
    {
        return peers_.size();
    }
    [[nodiscard]] const ModDownloadServerCounters &counters() const noexcept
    {
        return counters_;
    }

  private:
    struct Source final
    {
        LoadedMod mod;
        std::shared_ptr<const ModArchive> archive;
        std::string error;
        bool built{};
    };
    struct PeerState final
    {
        std::uint32_t request{};
        std::vector<std::size_t> slots; // request slot -> source index
        std::uint64_t offered_bytes{};
        std::uint64_t served_bytes{};
        std::uint32_t requests{};
        std::vector<std::pair<std::uint8_t, std::uint32_t>> queue;
        std::size_t queue_head{};
        TokenBucket frames;
        TokenBucket bytes;
        double last_activity{};
    };

    [[nodiscard]] const ModArchive *archive_for(std::size_t source);
    void refuse(PeerId peer, std::uint32_t request, ModDownloadRefusalReason reason,
                std::string detail);
    void handle_request(PeerId peer, PeerState &state, const ModDownloadRequest &request,
                        double now);

    bool enabled_{};
    ModDownloadServerOptions options_;
    std::vector<Source> sources_;
    std::map<PeerId, PeerState> peers_;
    // Requests and refusals stay counted after a peer's state expires.
    std::map<PeerId, std::uint32_t> request_history_;
    TokenBucket total_;
    std::vector<ModDownloadSend> sends_;
    ModDownloadServerCounters counters_;
};

// --- Client ------------------------------------------------------------------

enum class ModDownloadPhase : std::uint8_t
{
    idle,
    requesting,       // waiting for the host's offers
    awaiting_consent, // sizes known; the player decides
    downloading,
    complete,
    declined,
    failed
};

enum class ModDownloadFailure : std::uint8_t
{
    none,
    declined,
    host_refused,
    wrong_password,
    mod_unavailable,
    too_large,
    offer_mismatch,
    digest_mismatch,
    invalid_archive,
    timeout,
    cache_error,
    transport,
    cancelled
};

[[nodiscard]] const char *mod_download_failure_name(ModDownloadFailure failure) noexcept;

struct ModDownloadItem final
{
    ModDescriptor descriptor; // what the lobby advertised
    ModDigest client_digest{};
    std::uint64_t bytes{};
    std::uint64_t received{};
    std::uint32_t files{};
    bool offered{};
    bool installed{};
};

struct ModDownloadStatus final
{
    ModDownloadPhase phase{ModDownloadPhase::idle};
    std::vector<ModDownloadItem> mods;
    std::uint64_t total_bytes{};
    std::uint64_t received_bytes{};
    ModDownloadFailure failure{ModDownloadFailure::none};
    std::string detail;

    [[nodiscard]] bool finished() const noexcept
    {
        return phase == ModDownloadPhase::complete || phase == ModDownloadPhase::declined ||
               phase == ModDownloadPhase::failed;
    }
};

struct ModDownloadClientOptions final
{
    std::filesystem::path cache_root;
    std::string password;
    std::uint64_t maximum_mod_bytes{default_maximum_mod_download_bytes};
    std::uint64_t maximum_total_bytes{default_maximum_join_download_bytes};
    std::uint16_t window_chunks{32U};
    // No answer from the host for this long fails the download.
    double response_timeout_seconds{20.0};
    // No answer from the player for this long declines it.
    double consent_timeout_seconds{90.0};
    // Skip the prompt (tests, dedicated tooling, a remembered "always").
    bool auto_accept{};
};

// The joining side, transport-independent: time is a monotonic clock in
// seconds, frames come in through receive() and go out through take_sends().
class ModDownloadClient final
{
  public:
    ModDownloadClient(std::vector<ModDescriptor> needed, ModDownloadClientOptions options);

    void start(double now);
    void receive(std::span<const std::uint8_t> frame, double now);
    void update(double now);
    void accept(double now);
    void decline();
    void cancel();
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_sends();
    [[nodiscard]] const ModDownloadStatus &status() const noexcept
    {
        return status_;
    }
    [[nodiscard]] const std::vector<std::filesystem::path> &installed() const noexcept
    {
        return installed_;
    }

  private:
    void fail(ModDownloadFailure failure, std::string detail);
    void request_more(double now);
    void finish_mod(double now);

    ModDownloadClientOptions options_;
    ModDownloadStatus status_;
    std::uint32_t request_{};
    std::size_t current_{};
    std::uint32_t next_chunk_{};
    std::uint32_t requested_until_{};
    std::vector<std::uint8_t> buffer_;
    double deadline_{};
    std::vector<std::vector<std::uint8_t>> sends_;
    std::vector<std::filesystem::path> installed_;
};

// The UI's view of one join's downloads, shared with the join worker thread.
// The UI polls status(), shows the needed mods, their sizes and progress, and
// calls accept() / decline(); a cancelled join calls cancel().
class ModJoinDownloads final
{
  public:
    [[nodiscard]] ModDownloadStatus status() const;
    [[nodiscard]] bool awaiting_consent() const;
    void accept();
    void decline();
    void cancel();

    // Join worker side.
    void publish(const ModDownloadStatus &status);
    [[nodiscard]] std::optional<bool> decision() const;
    [[nodiscard]] bool cancelled() const;

  private:
    mutable std::mutex mutex_;
    ModDownloadStatus status_;
    std::optional<bool> decision_;
    bool cancelled_{};
};

// What a join needs to offer downloads. Owned by the app; the join worker
// fills `selection` with the mods this session runs (local or cached).
struct ModJoinDownloadContext final
{
    std::shared_ptr<ModJoinDownloads> shared;
    ModCatalog local;
    ModDownloadClientOptions options;
    SessionModSelection selection;
};

// Drives one download to its end on the join worker thread. `send` writes a
// frame to the host, `poll` yields the next download frame from the host (or
// nothing yet), `now` is a monotonic clock in seconds, `idle` waits briefly.
struct ModJoinDownloadIo final
{
    std::function<bool(std::span<const std::uint8_t>, std::string &)> send;
    std::function<bool(std::optional<std::vector<std::uint8_t>> &, std::string &)> poll;
    std::function<double()> now;
    std::function<void()> idle;
};

[[nodiscard]] bool run_mod_join_download(ModJoinDownloads &shared, ModDownloadClient &client,
                                         ModJoinDownloadIo &io, std::string &error);

// The minimal text prompt (the UI area draws the real one from status()).
struct ModDownloadPromptText final
{
    std::string title;
    std::string body;
    std::string accept_label;
    std::string decline_label;
};
[[nodiscard]] ModDownloadPromptText describe_mod_download(const ModDownloadStatus &status);

} // namespace openminerz::net
