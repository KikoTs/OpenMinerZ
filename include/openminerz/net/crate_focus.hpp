#pragma once

#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/combat_codec.hpp"

#include <array>
#include <optional>

namespace openminerz::net
{
// Original CrateScreen.IsSlotLocked: a peer's focused slot cannot be changed
// by another player, including quick-transfer destinations. Zero location is
// the stock protocol's "not selecting a crate" sentinel.
//
// The focused slot is `FocusCrateItem.X + FocusCrateItem.Y * columns`, and the
// column count is the reader's, not the message's:
//   1.6.3.8  CrateScreen and Crate.IsSlotLocked: 8 (an 8 x 4 grid).
//   1.9.9.8  CrateScreen: 4 (a 4 x 8 grid, so Y runs to 7);
//            Crate.IsSlotLocked (EjectContents): still 8.
// So nothing about X and Y is rejected here; a point that names no slot of
// the asked grid simply matches none.
class CrateFocusTracker final
{
  public:
    void set(std::uint8_t gid, const CrateFocusPayload &focus) noexcept
    {
        if (focus.location == IntVector3{})
            focus_[gid].reset();
        else
            focus_[gid] = focus;
    }
    void forget(std::uint8_t gid) noexcept
    {
        focus_[gid].reset();
    }
    void clear() noexcept
    {
        focus_ = {};
    }
    [[nodiscard]] bool locked(IntVector3 location, std::size_t slot,
                              std::optional<std::uint8_t> except = std::nullopt,
                              int columns = 8) const noexcept
    {
        for (std::size_t gid = 0; gid < focus_.size(); ++gid)
            if ((!except || gid != *except) && focus_[gid] && focus_[gid]->location == location &&
                static_cast<std::int64_t>(focus_[gid]->item_x) +
                        static_cast<std::int64_t>(focus_[gid]->item_y) * columns ==
                    static_cast<std::int64_t>(slot))
                return true;
        return false;
    }

  private:
    std::array<std::optional<CrateFocusPayload>, 256> focus_{};
};
} // namespace openminerz::net
