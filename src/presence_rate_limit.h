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

#ifndef AIMPDISCORDPRESENCE_SRC_PRESENCE_RATE_LIMIT_H_
#define AIMPDISCORDPRESENCE_SRC_PRESENCE_RATE_LIMIT_H_

#include <cstdint>

// Discord accepts at most 5 activity updates per 20 seconds. The plugin sends
// on a trailing edge: an update that arrives before the interval has elapsed is
// remembered (the latest state wins) and sent when the interval expires, so a
// track change is delayed but never dropped.
namespace PresenceRateLimit {

// `last_sent_ms` is a monotonic millisecond timestamp; 0 means nothing has been
// sent yet, which is always allowed. `min_interval_ms` 0 disables the limit.
inline bool ShouldSendNow(uint64_t now_ms, uint64_t last_sent_ms,
                          uint32_t min_interval_ms) {
  if (min_interval_ms == 0 || last_sent_ms == 0) {
    return true;
  }
  if (now_ms < last_sent_ms) {
    return false;
  }
  return now_ms - last_sent_ms >= min_interval_ms;
}

// `ThrottleMs` is expressed in milliseconds. Exactly 0 disables the limit;
// every other value below the 1000 ms floor — a negative value included — is
// raised to 1000, which keeps the default (4000) and the configured intent
// intact while never allowing the five-per-twenty-seconds cap to be approached
// by mistake. A negative value therefore clamps, it does not disable.
inline uint32_t NormalizeThrottleMs(int configured_ms) {
  if (configured_ms == 0) {
    return 0;
  }
  if (configured_ms < 1000) {
    return 1000;
  }
  return static_cast<uint32_t>(configured_ms);
}

}  // namespace PresenceRateLimit

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_RATE_LIMIT_H_
