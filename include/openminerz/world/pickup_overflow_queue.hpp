#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace openminerz::world
{

// A bounded overflow queue for pickups (OpenClassic's PickupManager._createQueue;
// see live_limit for what the shipped game does).
//
// The original never refuses a pickup. In the mod, past the 1000-entity bound
// CreatePickup, CreateUpwardPickup and HandleCreatePickupMessage all enqueue the
// request instead, and RemovePickup releases queued requests as entities leave
// the world. That matters for conservation rather than for looks: by the time a
// spawn is attempted the stack has already left an inventory slot, a crate slot
// or a mined block, so refusing it destroys the item outright.
//
// `_suppressQueueing` is the original's guard against a released request queueing
// itself straight back; `releasing()` reports the same state here.
//
// `Request` is the caller's fully-formed spawn request.
template <typename Request>
class PickupOverflowQueue final
{
  public:
    // The shipped Classic and Modern PickupManager have NO pickup bound and no
    // create queue: HandleCreatePickupMessage always adds the entity. The
    // 1000-entity `_maxPickups` / `_createQueue` pair this class was modelled
    // on is the OpenClassic mod's. With a stock peer in the session a pickup
    // deferred here still exists on that peer's machine, where it can be
    // requested and never granted, so the bound is now only a hostile ceiling
    // far above one blast through a wall of full crates (124 x 32 stacks).
    static constexpr std::size_t live_limit = 16'384U;
    // The original queue is unbounded. A deferred request is a few dozen bytes,
    // so this hard bound sits far past anything reachable in play and exists
    // only so a hostile peer cannot grow the queue without limit.
    static constexpr std::size_t deferred_limit = 8192U;

    [[nodiscard]] static bool at_capacity(std::size_t live) noexcept
    {
        return live >= live_limit;
    }

    [[nodiscard]] bool releasing() const noexcept
    {
        return releasing_;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return deferred_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return deferred_.size();
    }

    // Takes ownership of a request that cannot be created yet. False is the one
    // outcome that loses an item, and only at the hard bound.
    [[nodiscard]] bool defer(Request request)
    {
        if (deferred_.size() >= deferred_limit)
            return false;
        deferred_.push_back(std::move(request));
        return true;
    }

    [[nodiscard]] bool any(auto &&predicate) const
    {
        for (const Request &request : deferred_)
            if (predicate(request))
                return true;
        return false;
    }

    // Releases requests in arrival order while the world has room. `create`
    // performs one spawn and returns the new live count, or nothing to put the
    // request back and stop. Re-entrant calls do nothing, so a released request
    // can never defer itself again.
    void release(std::size_t live, auto &&create)
    {
        if (releasing_ || deferred_.empty())
            return;
        releasing_ = true;
        while (!deferred_.empty() && !at_capacity(live))
        {
            Request request = std::move(deferred_.front());
            deferred_.pop_front();
            const std::optional<std::size_t> created = create(request);
            if (!created.has_value())
            {
                deferred_.push_front(std::move(request));
                break;
            }
            live = *created;
        }
        releasing_ = false;
    }

    void clear() noexcept
    {
        deferred_.clear();
        releasing_ = false;
    }

  private:
    std::deque<Request> deferred_;
    bool releasing_{};
};

} // namespace openminerz::world
