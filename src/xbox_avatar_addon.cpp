#include "xbox_avatar_addon.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace openminerz
{
namespace
{

using Clock = std::chrono::steady_clock;

// How often the local file is looked at, and how long it must hold still
// before it is read: an exporter replaces the file in one step, but a copy
// in progress grows for a moment.
constexpr double local_poll_seconds = 1.0;
constexpr double local_settle_seconds = 0.4;
// A decode that runs this long is abandoned (it is polled between batches and
// textures; a single image is bounded by the texture limits).
constexpr double decode_deadline_seconds = 20.0;
// The marker is re-broadcast at most this often, however many players join.
constexpr double advertisement_interval_seconds = 1.0;

std::string environment_value(const char *name)
{
    const char *value = SDL_getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

bool disabled_by_environment()
{
    const std::string value = environment_value("OPENMINERZ_XBOX_AVATAR");
    return value == "off" || value == "0" || value == "none";
}

std::filesystem::path utf8_path(const std::string &text)
{
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

struct FileStamp final
{
    std::filesystem::path path;
    std::uintmax_t size{};
    std::filesystem::file_time_type modified{};

    bool operator==(const FileStamp &) const = default;
};

bool stamp_of(const std::filesystem::path &path, FileStamp &stamp)
{
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) || error)
        return false;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
        return false;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error)
        return false;
    stamp = {path, size, modified};
    return true;
}

bool read_file(const std::filesystem::path &path, std::size_t maximum,
               std::vector<std::uint8_t> &bytes, std::string &error)
{
    std::error_code status;
    const std::uintmax_t size = std::filesystem::file_size(path, status);
    if (status)
    {
        error = "the avatar file could not be read";
        return false;
    }
    if (size == 0U || size > maximum)
    {
        error = size == 0U ? "the avatar file is empty" : "the avatar file is larger than an avatar may be";
        return false;
    }
    std::ifstream input{path, std::ios::binary};
    bytes.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<std::uintmax_t>(input.gcount()) != size)
    {
        // Still being written, or locked by its exporter: try again later.
        error = "the avatar file could not be read";
        return false;
    }
    return true;
}

} // namespace

std::vector<std::filesystem::path> xbox_avatar_search_paths()
{
    std::vector<std::filesystem::path> paths;
    if (disabled_by_environment())
        return paths;
    if (const std::string named = environment_value("OPENMINERZ_XBOX_AVATAR"); !named.empty())
        paths.push_back(utf8_path(named));
    if (const std::string local = environment_value("LOCALAPPDATA"); !local.empty())
        paths.push_back(utf8_path(local) / "OpenMinerZ" / "avatar.ocavatar");
    std::vector<std::filesystem::path> games;
#if defined(OPENMINERZ_OWNED_GAME_DIR)
    games.emplace_back(OPENMINERZ_OWNED_GAME_DIR);
#endif
#if defined(_WIN32)
    games.emplace_back(LR"(C:\Program Files (x86)\Steam\steamapps\common\CastleMiner Z)");
#endif
    for (const std::filesystem::path &game : games)
    {
        // Where each edition of the add-on keeps the player's export: the
        // CastleForge mod, the standalone installer, the OpenClassic add-on.
        paths.push_back(game / "!Mods" / "XboxAvatar" / "avatar.ocavatar");
        paths.push_back(game / "Xbox Avatar" / "avatar.ocavatar");
        paths.push_back(game / "OpenClassic Addons" / "Xbox Avatar" / "avatar.ocavatar");
    }
    return paths;
}

std::filesystem::path find_xbox_avatar_file()
{
    const std::vector<std::filesystem::path> paths = xbox_avatar_search_paths();
    // A file named by the environment is the answer whenever it exists.
    FileStamp stamp;
    if (!environment_value("OPENMINERZ_XBOX_AVATAR").empty() && !paths.empty() &&
        stamp_of(paths.front(), stamp))
        return paths.front();
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    for (const std::filesystem::path &path : paths)
        if (stamp_of(path, stamp) && stamp.size > 0U && (newest.empty() || stamp.modified > newest_time))
        {
            newest = path;
            newest_time = stamp.modified;
        }
    return newest;
}

bool decode_png_image(std::span<const std::uint8_t> png, xbox_avatar::Image &image)
{
    SDL_IOStream *stream = SDL_IOFromConstMem(png.data(), png.size());
    if (stream == nullptr)
        return false;
    SDL_Surface *surface = SDL_LoadPNG_IO(stream, true);
    if (surface == nullptr)
        return false;
    if (surface->format != SDL_PIXELFORMAT_RGBA32)
    {
        SDL_Surface *converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        surface = converted;
        if (surface == nullptr)
            return false;
    }
    bool decoded = false;
    if (surface->w > 0 && surface->h > 0 && surface->pixels != nullptr)
    {
        const std::size_t row_bytes = static_cast<std::size_t>(surface->w) * 4U;
        image.width = surface->w;
        image.height = surface->h;
        image.rgba.resize(row_bytes * static_cast<std::size_t>(surface->h));
        for (int row = 0; row < surface->h; ++row)
            std::memcpy(image.rgba.data() + static_cast<std::size_t>(row) * row_bytes,
                        static_cast<const std::uint8_t *>(surface->pixels) +
                            static_cast<std::size_t>(row) * static_cast<std::size_t>(surface->pitch),
                        row_bytes);
        decoded = true;
    }
    SDL_DestroySurface(surface);
    return decoded;
}

class XboxAvatarAddon::Impl final
{
  public:
    Impl() : start_{Clock::now()}, worker_{[this] { run(); }} {}

    ~Impl()
    {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
            cancel_.store(true);
        }
        wake_.notify_all();
        worker_.join();
    }

    enum class JobKind : std::uint8_t
    {
        local,
        remote,
        diagnostic
    };

    struct Job final
    {
        JobKind kind{JobKind::local};
        std::filesystem::path path;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        xbox_avatar::Digest expected{};
        std::uint8_t gid{};
        std::uint32_t epoch{};
        std::uint32_t generation{};
    };

    struct Result final
    {
        JobKind kind{JobKind::local};
        std::uint8_t gid{};
        std::uint32_t epoch{};
        std::uint32_t generation{};
        std::shared_ptr<const xbox_avatar::Skin> skin;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        xbox_avatar::Digest hash{};
        bool hash_valid{};
        bool read{};
        std::string error;
        double seconds{};
    };

    [[nodiscard]] double now() const
    {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

    void run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_)
                    return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                busy_ = true;
                cancel_.store(false);
                running_since_ = Clock::now();
            }
            Result result = execute(job);
            {
                const std::lock_guard lock{mutex_};
                results_.push_back(std::move(result));
                busy_ = false;
            }
            idle_.notify_all();
        }
    }

    [[nodiscard]] Result execute(const Job &job) const
    {
        const Clock::time_point began = Clock::now();
        Result result;
        result.kind = job.kind;
        result.gid = job.gid;
        result.epoch = job.epoch;
        result.generation = job.generation;
        try
        {
            xbox_avatar::Limits limits;
            std::shared_ptr<const std::vector<std::uint8_t>> bytes = job.bytes;
            if (job.kind != JobKind::remote)
            {
                limits.file_bytes = xbox_avatar::local_file_bytes;
                auto loaded = std::make_shared<std::vector<std::uint8_t>>();
                if (!read_file(job.path, limits.file_bytes, *loaded, result.error))
                    return result;
                bytes = std::move(loaded);
            }
            else
                limits.file_bytes = xbox_avatar::network_file_bytes;
            result.read = true;
            result.bytes = bytes;
            result.hash = xbox_avatar::sha256(*bytes);
            // What arrived must be what its sender promised, byte for byte,
            // before a single field of it is believed.
            result.hash_valid = job.kind != JobKind::remote || result.hash == job.expected;
            if (!result.hash_valid)
            {
                result.error = "the avatar does not match the hash its sender announced";
                return result;
            }
            result.skin = xbox_avatar::load(*bytes, decode_png_image, result.error, limits, &cancel_);
        }
        catch (const std::exception &)
        {
            result.skin.reset();
            result.error = "out of memory while reading the avatar";
        }
        result.seconds = std::chrono::duration<double>(Clock::now() - began).count();
        return result;
    }

    void enqueue(Job job)
    {
        {
            const std::lock_guard lock{mutex_};
            // The newest request for a player replaces any still waiting.
            std::erase_if(jobs_, [&](const Job &queued) {
                return queued.kind == job.kind && (job.kind == JobKind::local || queued.gid == job.gid);
            });
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

    [[nodiscard]] std::filesystem::path resolve_local() const
    {
        if (disabled_by_environment())
            return {};
        if (!options_.file.empty())
            return options_.file;
        return find_xbox_avatar_file();
    }

    void forget_local()
    {
        if (local_skin_ != nullptr || local_bytes_ != nullptr)
            std::cout << "xbox-avatar.local released\n";
        local_skin_.reset();
        local_bytes_.reset();
        local_error_.clear();
        loaded_stamp_ = {};
        pending_stamp_ = {};
        sync_.set_local_avatar(nullptr, {}, now());
    }

    void poll_local(bool immediately)
    {
        const double time = now();
        if (!immediately && !reload_requested_ && time < next_poll_)
            return;
        next_poll_ = time + local_poll_seconds;
        const std::filesystem::path path = resolve_local();
        FileStamp stamp;
        if (!stamp_of(path, stamp) || stamp.size == 0U)
        {
            local_path_ = path;
            local_found_ = false;
            if (loaded_stamp_.path != std::filesystem::path{} || local_skin_ != nullptr)
                forget_local();
            reload_requested_ = false;
            return;
        }
        local_path_ = path;
        local_found_ = true;
        if (stamp == loaded_stamp_ && !reload_requested_)
            return;
        if (!(stamp == pending_stamp_))
        {
            pending_stamp_ = stamp;
            pending_since_ = time;
        }
        if (!immediately && !reload_requested_ && time - pending_since_ < local_settle_seconds)
        {
            next_poll_ = time + local_settle_seconds;
            return;
        }
        if (stamp == queued_stamp_ && !reload_requested_)
            return; // already on its way through the worker
        reload_requested_ = false;
        queued_stamp_ = stamp;
        Job job;
        job.kind = JobKind::local;
        job.path = path;
        job.generation = ++local_generation_;
        local_loading_ = true;
        enqueue(std::move(job));
    }

    void collect()
    {
        std::vector<Result> results;
        {
            const std::lock_guard lock{mutex_};
            results.swap(results_);
        }
        for (Result &result : results)
        {
            if (result.kind == JobKind::local)
            {
                if (result.generation != local_generation_)
                    continue; // superseded by a newer read
                local_loading_ = false;
                if (!result.read)
                {
                    // Not readable right now (mid-copy, locked): look again.
                    queued_stamp_ = {};
                    pending_stamp_ = {};
                    local_error_ = result.error;
                    continue;
                }
                loaded_stamp_ = queued_stamp_;
                if (result.skin == nullptr)
                {
                    // A bad export never replaces the avatar already worn.
                    local_error_ = result.error;
                    std::cerr << "xbox-avatar.local rejected file=" << local_path_.string()
                              << " reason=" << result.error << '\n';
                    continue;
                }
                local_error_.clear();
                local_skin_ = std::move(result.skin);
                local_hash_ = result.hash;
                // A file too large for the add-on's transfer is worn locally
                // and simply not offered.
                local_bytes_ = result.bytes != nullptr && result.bytes->size() <= net::avatar_maximum_asset_bytes
                                   ? result.bytes
                                   : nullptr;
                sync_.set_local_avatar(local_bytes_, local_hash_, now());
                std::cout << "xbox-avatar.local loaded file=" << local_path_.string()
                          << " bytes=" << (result.bytes != nullptr ? result.bytes->size() : 0U)
                          << " layers=" << local_skin_->layers.size()
                          << " triangles=" << local_skin_->triangles << " build=" << local_skin_->build
                          << " sha256=" << xbox_avatar::digest_text(local_hash_)
                          << " decode_ms=" << result.seconds * 1000.0 << '\n';
                continue;
            }
            if (result.epoch != epoch_[result.gid])
                continue; // that player has left since
            if (result.kind == JobKind::remote)
                sync_.transfer_verified(result.gid, result.hash, result.hash_valid);
            if (result.skin == nullptr)
            {
                // Refused: this player keeps the stock body (or the avatar
                // already shown), and nothing else is affected.
                std::cerr << "xbox-avatar.remote rejected gid=" << static_cast<int>(result.gid)
                          << " reason=" << result.error << '\n';
                continue;
            }
            std::cout << "xbox-avatar.remote applied gid=" << static_cast<int>(result.gid)
                      << " bytes=" << (result.bytes != nullptr ? result.bytes->size() : 0U)
                      << " triangles=" << result.skin->triangles
                      << " sha256=" << xbox_avatar::digest_text(result.hash)
                      << " decode_ms=" << result.seconds * 1000.0 << '\n';
            remote_skins_[result.gid] = std::move(result.skin);
        }
    }

    void apply_policy()
    {
        const bool disabled = disabled_by_environment();
        sync_.set_policy(options_.show_mine && !disabled, options_.show_others && !disabled, now());
        if (!options_.show_others || disabled)
            remote_skins_.clear();
        if (!enabled())
            sync_.set_advertised(false);
    }

    [[nodiscard]] bool enabled() const
    {
        return (options_.show_mine || options_.show_others) && !disabled_by_environment();
    }

    void update()
    {
        poll_local(false);
        collect();
        if (in_session_)
        {
            for (net::AvatarReceived &done : sync_.take_received())
            {
                if (done.bytes == nullptr)
                    continue;
                Job job;
                job.kind = JobKind::remote;
                job.gid = done.gid;
                job.epoch = epoch_[done.gid];
                job.bytes = std::move(done.bytes);
                job.expected = done.hash;
                enqueue(std::move(job));
            }
            sync_.update(now());
        }
        // A decode that will not finish is abandoned.
        const std::lock_guard lock{mutex_};
        if (busy_ && std::chrono::duration<double>(Clock::now() - running_since_).count() >
                         decode_deadline_seconds)
            cancel_.store(true);
    }

    void wait_idle()
    {
        poll_local(true);
        {
            std::unique_lock lock{mutex_};
            idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
        }
        collect();
    }

    Clock::time_point start_;
    Options options_{};
    // Local avatar (game thread).
    std::filesystem::path local_path_;
    bool local_found_{};
    bool local_loading_{};
    bool reload_requested_{true};
    double next_poll_{};
    FileStamp loaded_stamp_;
    FileStamp pending_stamp_;
    FileStamp queued_stamp_;
    double pending_since_{};
    std::uint32_t local_generation_{};
    std::shared_ptr<const xbox_avatar::Skin> local_skin_;
    std::shared_ptr<const std::vector<std::uint8_t>> local_bytes_;
    xbox_avatar::Digest local_hash_{};
    std::string local_error_;
    // Remote avatars (game thread).
    std::unordered_map<std::uint8_t, std::shared_ptr<const xbox_avatar::Skin>> remote_skins_;
    std::array<std::uint32_t, 256> epoch_{};
    net::AvatarSyncSession sync_;
    bool in_session_{};
    bool advertise_requested_{};
    double last_advertisement_{-1.0e9};
    // Worker.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    bool busy_{};
    bool stopping_{};
    mutable std::atomic<bool> cancel_{false};
    Clock::time_point running_since_{};
    std::thread worker_;
};

XboxAvatarAddon::XboxAvatarAddon() : impl_{std::make_unique<Impl>()} {}

XboxAvatarAddon::~XboxAvatarAddon() = default;

void XboxAvatarAddon::configure(const Options &options)
{
    const bool file_changed = options.file != impl_->options_.file;
    impl_->options_ = options;
    impl_->apply_policy();
    if (file_changed)
        impl_->reload_requested_ = true;
    if (impl_->in_session_ && impl_->enabled())
        impl_->advertise_requested_ = true;
}

const XboxAvatarAddon::Options &XboxAvatarAddon::options() const noexcept
{
    return impl_->options_;
}

void XboxAvatarAddon::reload()
{
    impl_->reload_requested_ = true;
}

void XboxAvatarAddon::update()
{
    impl_->update();
}

void XboxAvatarAddon::wait_idle()
{
    impl_->wait_idle();
}

std::shared_ptr<const xbox_avatar::Skin> XboxAvatarAddon::local_skin() const noexcept
{
    return impl_->options_.show_mine && !disabled_by_environment() ? impl_->local_skin_ : nullptr;
}

std::shared_ptr<const xbox_avatar::Skin> XboxAvatarAddon::remote_skin(std::uint8_t gid) const noexcept
{
    if (!impl_->options_.show_others || disabled_by_environment())
        return nullptr;
    const auto found = impl_->remote_skins_.find(gid);
    return found == impl_->remote_skins_.end() ? nullptr : found->second;
}

void XboxAvatarAddon::session_started(net::ProtocolVersion protocol)
{
    session_ended();
    impl_->sync_.reset(protocol);
    impl_->in_session_ = true;
    impl_->apply_policy();
    impl_->sync_.set_local_avatar(impl_->local_bytes_, impl_->local_hash_, impl_->now());
    impl_->advertise_requested_ = impl_->enabled();
    impl_->last_advertisement_ = -1.0e9;
    std::cout << "xbox-avatar.session started protocol=" << static_cast<int>(protocol) << '\n';
}

void XboxAvatarAddon::session_ended()
{
    if (!impl_->in_session_)
        return;
    impl_->in_session_ = false;
    impl_->advertise_requested_ = false;
    // Nobody from that session is here any more: release what they wore.
    impl_->remote_skins_.clear();
    for (std::uint32_t &epoch : impl_->epoch_)
        ++epoch;
    impl_->sync_.reset(net::ProtocolVersion::classic_v3);
    std::cout << "xbox-avatar.session ended\n";
}

bool XboxAvatarAddon::in_session() const noexcept
{
    return impl_->in_session_;
}

void XboxAvatarAddon::peer_capable(std::uint8_t gid, std::uint8_t message_id)
{
    if (!impl_->in_session_)
        return;
    if (!impl_->sync_.peer_ready(gid))
        std::cout << "xbox-avatar.peer capable gid=" << static_cast<int>(gid)
                  << " message_id=" << static_cast<int>(message_id) << '\n';
    impl_->sync_.peer_capable(gid, message_id, impl_->now());
}

void XboxAvatarAddon::peer_left(std::uint8_t gid)
{
    ++impl_->epoch_[gid];
    if (impl_->remote_skins_.erase(gid) > 0U)
        std::cout << "xbox-avatar.remote released gid=" << static_cast<int>(gid) << '\n';
    impl_->sync_.peer_left(gid);
}

void XboxAvatarAddon::packet_received(std::uint8_t gid, std::span<const std::uint8_t> packet)
{
    if (impl_->in_session_)
        impl_->sync_.receive(gid, packet, impl_->now());
}

void XboxAvatarAddon::request_advertisement() noexcept
{
    if (impl_->in_session_ && impl_->enabled())
        impl_->advertise_requested_ = true;
}

bool XboxAvatarAddon::advertisement_due() const noexcept
{
    return impl_->in_session_ && impl_->advertise_requested_ && impl_->enabled() &&
           impl_->now() - impl_->last_advertisement_ >= advertisement_interval_seconds;
}

void XboxAvatarAddon::advertisement_sent() noexcept
{
    impl_->advertise_requested_ = false;
    impl_->last_advertisement_ = impl_->now();
    impl_->sync_.set_advertised(true);
}

std::vector<net::AvatarSyncSend> XboxAvatarAddon::take_sends()
{
    return impl_->sync_.take_sends();
}

void XboxAvatarAddon::diagnostic_remote(std::uint8_t gid, const std::filesystem::path &file)
{
    Impl::Job job;
    job.kind = Impl::JobKind::diagnostic;
    job.gid = gid;
    job.epoch = impl_->epoch_[gid];
    job.path = file;
    impl_->enqueue(std::move(job));
}

XboxAvatarAddon::Status XboxAvatarAddon::status() const
{
    Status status;
    status.file = impl_->local_path_;
    status.file_found = impl_->local_found_;
    status.loaded = impl_->local_skin_ != nullptr;
    status.loading = impl_->local_loading_;
    status.error = impl_->local_error_;
    if (impl_->local_skin_ != nullptr)
    {
        status.triangles = impl_->local_skin_->triangles;
        status.build = impl_->local_skin_->build;
    }
    status.in_session = impl_->in_session_;
    status.remote_avatars = impl_->remote_skins_.size();
    for (std::size_t gid = 0; gid < 256U; ++gid)
        status.capable_peers += impl_->sync_.peer_ready(static_cast<std::uint8_t>(gid)) ? 1U : 0U;
    return status;
}

} // namespace openminerz
