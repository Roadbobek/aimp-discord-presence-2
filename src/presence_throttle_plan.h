//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#ifndef AIMPDISCORDPRESENCE_SRC_PRESENCE_THROTTLE_PLAN_H_
#define AIMPDISCORDPRESENCE_SRC_PRESENCE_THROTTLE_PLAN_H_

#include <cstdint>

#include "presence_rate_limit.h"

// The throttle worker is the only thread that talks to the Discord client. The
// message thread records what the card should be ("desired state") and bumps a
// revision for every request - a send or a clear - then wakes the worker. The
// worker publishes against the revision it last acted on, so a request that
// arrives while a client call is in flight can never be lost: the stale call
// only advances the *published* revision, the next decision sees the newer one,
// and the clear that superseded the send is issued next. Keeping that decision
// here - no client, no real clock, no locks - makes the ordering rule testable
// as a pure function.
namespace PresenceThrottlePlan {

enum class Action {
  kIdle,   // the published revision is the desired one
  kWait,   // newer state exists, but the interval has not elapsed yet
  kSend,   // publish the desired activity
  kClear,  // publish a presence clear
};

// The worker's inputs, read together under the throttle lock.
struct Request {
  bool desired_valid = false;
  // Bumped on every desired-state change, send and clear alike.
  uint64_t desired_revision = 0;
  // The revision the last completed client call acted on.
  uint64_t last_sent_revision = 0;
  // Monotonic time of the last publish that consumed interval budget; 0 means
  // nothing has been sent yet. A clear resets it to 0, so a clear is never
  // held behind the interval that belonged to the track that just ended.
  uint64_t last_sent_ms = 0;
};

struct Decision {
  Action action = Action::kIdle;
  // kWait only: milliseconds until `last_sent_ms + min_interval_ms`.
  uint64_t wait_ms = 0;
};

inline Decision Decide(const Request& request, uint64_t now_ms,
                       uint32_t min_interval_ms) {
  if (request.desired_revision == request.last_sent_revision) {
    return Decision{Action::kIdle, 0};
  }
  // A clear is a terminal state change: it cannot be coalesced into a later
  // send, and holding it behind the interval leaves a stale card visible with
  // nothing playing - the exact user-visible bug this design exists to
  // prevent. The interval exists to stop track-change churn, so it gates a
  // send only and a pending clear is returned at once.
  if (!request.desired_valid) {
    return Decision{Action::kClear, 0};
  }
  if (!PresenceRateLimit::ShouldSendNow(now_ms, request.last_sent_ms,
                                        min_interval_ms)) {
    const uint64_t deadline = request.last_sent_ms + min_interval_ms;
    return Decision{Action::kWait, deadline > now_ms ? deadline - now_ms : 0};
  }
  return Decision{Action::kSend, 0};
}

// The interval gate to store after a completed client call. `gate_before` is
// the value read when the action was chosen, `gate_now` the value found when
// the lock is re-taken. They differ only when a request reset the gate while
// the call was in flight - a clear does exactly that - and that reset must win:
// overwriting it with the send's completion time would hold the newer clear
// behind an interval that no longer applies. A clear never consumes interval
// budget, so it stores the gate it finds.
inline uint64_t GateAfterAction(Action action, uint64_t gate_before,
                                uint64_t gate_now, uint64_t sent_at_ms) {
  if (action != Action::kSend) {
    return gate_now;
  }
  return gate_now == gate_before ? sent_at_ms : gate_now;
}

}  // namespace PresenceThrottlePlan

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_THROTTLE_PLAN_H_
