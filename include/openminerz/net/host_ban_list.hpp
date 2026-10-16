#pragma once

#include "openminerz/atomic_file.hpp"
#include "openminerz/net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace openminerz::net
{

// CastleMinerZPlayerStats.BanList, the host's ban list in both shipped
// generations: `Dictionary<ulong, DateTime>` keyed by the banned gamer's
// AlternateAddress (its 64-bit Steam ID) with the UTC time of the ban.
//
// What the shipped host does with it (read from GameScreen and
// CastleMinerZGame, 1.6.3.8 and 1.9.9.8 alike):
//   * HostOptions > Ban Player sends the notice and KickMessage(banned), adds
//     the entry and calls SaveData() at once;
//   * HostOptions > Clear Ban List clears it and calls SaveData() at once, and
//     the row is only visible while the list is not empty;
//   * OnGamerJoined answers a listed gamer with another KickMessage(banned).
// It lives in the PLAYER's statistics (stats.sav: a count, then each Steam ID
// as an Int64 followed by DateTime.Ticks), not in a world, so a ban follows the
// host into every world they serve and survives a restart of the game.
//
// stats.sav itself is the original's tamper-proofed, per-account file and is
// never written here. This keeps the same facts -- Steam ID and .NET ticks --
// beside this project's own settings, one "<steam id> <ticks>" pair per line,
// and saves on every change exactly as the original does.
class HostBanList final
{
  public:
    // DateTime.Ticks: 100 ns intervals since 0001-01-01T00:00:00 UTC.
    static constexpr std::int64_t ticks_at_unix_epoch = 621'355'968'000'000'000LL;
    static constexpr std::size_t maximum_entries = 4096U;

    HostBanList() = default;
    explicit HostBanList(std::filesystem::path path) : path_(std::move(path))
    {
        load();
    }

    [[nodiscard]] static std::int64_t utc_ticks_now()
    {
        const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
        return ticks_at_unix_epoch +
               std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
                   since_epoch)
                   .count();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return entries_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }
    [[nodiscard]] bool contains(PeerId peer) const noexcept
    {
        return entries_.contains(peer);
    }
    // When the entry was made, as DateTime.Ticks; zero if the peer is not banned.
    [[nodiscard]] std::int64_t banned_at(PeerId peer) const noexcept
    {
        const auto found = entries_.find(peer);
        return found == entries_.end() ? 0 : found->second;
    }

    // BanList[AlternateAddress] = DateTime.UtcNow; SaveData(). Banning somebody
    // again only refreshes the time. Returns whether the list reached the disk
    // (always true for a list that has no file).
    bool ban(PeerId peer, std::int64_t utc_ticks = utc_ticks_now())
    {
        if (peer == 0U || (!entries_.contains(peer) && entries_.size() >= maximum_entries))
            return false;
        entries_.insert_or_assign(peer, utc_ticks);
        return save();
    }

    // BanList.Clear(); SaveData().
    bool clear()
    {
        entries_.clear();
        return save();
    }

    // A missing or damaged file is an empty list; a damaged line is skipped.
    void load()
    {
        entries_.clear();
        if (path_.empty())
            return;
        std::ifstream input{path_};
        std::string line;
        while (std::getline(input, line) && entries_.size() < maximum_entries)
        {
            if (line.empty() || line.front() == '#')
                continue;
            std::istringstream fields{line};
            PeerId peer{};
            std::int64_t ticks{};
            std::string trailing;
            if (fields >> peer >> ticks && !(fields >> trailing) && peer != 0U && ticks >= 0)
                entries_.insert_or_assign(peer, ticks);
        }
    }

  private:
    bool save() const
    {
        if (path_.empty())
            return true;
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        std::string text{"# OpenMinerZ host ban list v1: <steam id> <utc DateTime.Ticks>\n"};
        for (const auto &[peer, ticks] : entries_)
            text += std::to_string(peer) + ' ' + std::to_string(ticks) + '\n';
        return write_file_atomically(
            path_, std::span<const std::uint8_t>{
                       reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
    }

    std::filesystem::path path_;
    std::map<PeerId, std::int64_t> entries_;
};

} // namespace openminerz::net
