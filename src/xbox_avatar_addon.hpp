#pragma once

// The "Xbox Avatars" add-on, as the application sees it.
//
// One object owns everything between an `.ocavatar` on disk or on the wire and
// a skin the renderer can draw:
//
//   * the local player's avatar file: where it is, noticing that it was
//     re-exported or replaced, and loading it again without a restart;
//   * every other player's avatar: the add-on's network exchange
//     (net::AvatarSyncSession), verifying and decoding what arrives, and
//     dropping it when its owner leaves;
//   * the two switches - show my avatar, show other players' avatars - applied
//     the moment they change.
//
// Reading, hashing, parsing and PNG decoding all happen on a worker thread;
// the game thread only ever swaps finished, immutable skins in. A file that is
// refused (too large, malformed, hostile) costs its owner nothing but the
// avatar: the stock body is the fallback everywhere.
//
// Presentation only. Nothing here touches gameplay, hit boxes or any stock
// network message.

#include "openminerz/net/avatar_sync.hpp"
#include "openminerz/xbox_avatar.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openminerz
{

// Where the local player's avatar is looked for when no file is named, most
// specific first: OPENMINERZ_XBOX_AVATAR, %LOCALAPPDATA%\OpenMinerZ, then the
// folders the add-on itself keeps the export in under an installed game.
[[nodiscard]] std::vector<std::filesystem::path> xbox_avatar_search_paths();
// The newest existing file among those, or empty.
[[nodiscard]] std::filesystem::path find_xbox_avatar_file();
// SDL's PNG reader as an xbox_avatar::ImageDecoder. Thread-safe.
[[nodiscard]] bool decode_png_image(std::span<const std::uint8_t> png, xbox_avatar::Image &image);

class XboxAvatarAddon final
{
  public:
    struct Options final
    {
        bool show_mine{true};
        bool show_others{true};
        // An explicit file; empty looks in xbox_avatar_search_paths().
        std::filesystem::path file;
    };

    struct Status final
    {
        std::filesystem::path file;
        bool file_found{};
        bool loaded{};
        bool loading{};
        std::string error;
        std::size_t triangles{};
        float build{1.0F};
        bool in_session{};
        std::size_t capable_peers{};
        std::size_t remote_avatars{};
    };

    XboxAvatarAddon();
    ~XboxAvatarAddon();
    XboxAvatarAddon(const XboxAvatarAddon &) = delete;
    XboxAvatarAddon &operator=(const XboxAvatarAddon &) = delete;

    void configure(const Options &options);
    [[nodiscard]] const Options &options() const noexcept;
    // Read the local file again now, whether or not it looks changed.
    void reload();
    // Once per frame, on the game thread.
    void update();
    // Blocks until everything queued has been decoded and collected. For
    // deterministic captures and tests; never called while playing.
    void wait_idle();

    // What to draw. nullptr is the stock body.
    [[nodiscard]] std::shared_ptr<const xbox_avatar::Skin> local_skin() const noexcept;
    [[nodiscard]] std::shared_ptr<const xbox_avatar::Skin> remote_skin(std::uint8_t gid) const noexcept;

    // The session this client is in, once its own presence has been announced.
    void session_started(net::ProtocolVersion protocol);
    void session_ended();
    [[nodiscard]] bool in_session() const noexcept;
    void peer_capable(std::uint8_t gid, std::uint8_t message_id);
    void peer_left(std::uint8_t gid);
    void packet_received(std::uint8_t gid, std::span<const std::uint8_t> packet);
    // Somebody new announced themselves: they have not seen the marker.
    void request_advertisement() noexcept;
    // True when the capability marker should be broadcast now.
    [[nodiscard]] bool advertisement_due() const noexcept;
    void advertisement_sent() noexcept;
    [[nodiscard]] std::vector<net::AvatarSyncSend> take_sends();

    // Capture aid: dress one remote player from a file, through the same
    // verification and decoding a received avatar goes through.
    void diagnostic_remote(std::uint8_t gid, const std::filesystem::path &file);

    [[nodiscard]] Status status() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
