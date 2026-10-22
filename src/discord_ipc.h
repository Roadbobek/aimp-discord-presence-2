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

#ifndef AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_
#define AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal, self-contained Discord local RPC client.
//
// Speaks the local IPC protocol directly over the named pipes
// \\.\pipe\discord-ipc-{0..9}, so the plugin does not depend on the abandoned
// discord-rpc library (which cannot express the activity `type` field).
//
// The pipe index is not tied to a Discord flavour: Stable, PTB and Canary
// share the same index range, so a client can silently attach to an instance
// other than the one it expects. The flavour is therefore identified from
// `data.config.api_endpoint` of the READY dispatch, and logged together with
// the index that was actually attached.
//
// The wire protocol, as implemented by the reference clients (discord-rpc,
// pypresence, Lachee's C#, jagrosh's Java):
//   * every frame is [opcode:uint32 LE][length:uint32 LE][utf-8 json];
//   * HANDSHAKE (opcode 0) MUST be followed by reading inbound until the
//     server dispatches READY; a SET_ACTIVITY written before READY is lost;
//   * the server answers every command with an echo carrying the same nonce,
//     or with evt:"ERROR" carrying {code,message}. A client that never reads
//     inbound cannot tell "applied" from "rejected" from "connection closed",
//     which is why the connection is drained continuously for its whole life;
//   * PING (opcode 3) must be answered immediately with PONG (opcode 4);
//   * CLOSE (opcode 2) is fatal; the connection is re-established after a
//     backoff because only 2 connections per minute are allowed (60 on
//     Canary).
//
// Discord must already be running; this client never launches it. A background
// worker keeps the connection alive and coalesces presence updates so at most
// one SET_ACTIVITY is in flight at a time. All pipe I/O uses overlapped
// operations with a bounded wait, so a stalled Discord turns into a disconnect
// instead of blocking AIMP's message thread.
namespace DiscordIpc {

// Discord activity schema limits (characters, i.e. UTF-8 codepoints).
constexpr size_t kTextFieldMinLength = 2;
constexpr size_t kTextFieldMaxLength = 128;
constexpr size_t kImageFieldMaxLength = 300;
constexpr size_t kActivityNameMaxLength = 128;
constexpr size_t kButtonLabelMaxLength = 32;
constexpr size_t kButtonUrlMaxLength = 512;
constexpr size_t kMaxActivityButtons = 2;

// Opcodes of the local IPC protocol.
constexpr uint32_t kOpcodeHandshake = 0;
constexpr uint32_t kOpcodeFrame = 1;
constexpr uint32_t kOpcodeClose = 2;
constexpr uint32_t kOpcodePing = 3;
constexpr uint32_t kOpcodePong = 4;

// Presence updates are coalesced: at most one SET_ACTIVITY is in flight, and
// the next one waits for the server's echo of its nonce. A server that never
// echoes must not wedge the client, so an unanswered update is abandoned
// after this long (the pending value is kept and sent on the next echo).
constexpr int kEchoTimeoutSeconds = 10;

// The IPC server accepts 2 new connections per minute (60 on Canary), so a
// reconnect loop is throttled. Back off from this base and stop doubling at
// the maximum.
constexpr int kReconnectDelaySeconds = 10;
constexpr int kReconnectDelayMaxSeconds = 60;

// Inbound is drained on this interval for as long as a connection lives.
constexpr int kPollIntervalMilliseconds = 100;

// An idle connection logs a liveness line this often, so a silent read loop
// is distinguishable from a read loop that stopped running.
constexpr int kStarvationWarnSeconds = 30;

enum class ActivityType : int {
  kPlaying = 0,
  kListening = 2,
  kWatching = 3,
  kCompeting = 5,
};

enum class StatusDisplayType : int {
  kName = 0,
  kState = 1,
  kDetails = 2,
};

// One link button of the activity card. A button is present when its url is
// non-empty; only https:// urls are emitted (Discord rejects anything else).
struct ActivityButton {
  std::string label;
  std::string url;
};

struct Activity {
  int type = static_cast<int>(ActivityType::kListening);
  std::string details;  // track title
  std::string state;    // artist
  std::string large_image;
  std::string large_text;
  std::string small_image;
  std::string small_text;
  // Application name shown above the activity ("Listening to <name>").
  std::string name;
  // Up to two buttons; a slot with an empty url is skipped by the builder.
  ActivityButton buttons[kMaxActivityButtons];
  int64_t start_timestamp = 0;  // unix seconds, 0 omits the field
  int64_t end_timestamp = 0;    // unix seconds, 0 omits the field
  int status_display_type = static_cast<int>(StatusDisplayType::kDetails);
};

// A frame as it travels on the wire, before encoding.
struct OutboundFrame {
  uint32_t opcode = 0;
  std::string payload;
};

// --- Pure helpers, unit tested separately from the pipe plumbing -----------

// Escapes a UTF-8 string for a JSON string literal. Invalid UTF-8 bytes are
// replaced with U+FFFD so the payload is always valid UTF-8.
std::string EscapeJsonString(const std::string& utf8);

// Truncates valid UTF-8 to at most max_codepoints codepoints without ever
// splitting a multi-byte sequence.
std::string TruncateUtf8(const std::string& utf8, size_t max_codepoints);

// Normalizes a text field: empty stays empty, one codepoint is padded to the
// schema minimum, longer values are truncated to the schema maximum.
std::string NormalizeTextField(const std::string& utf8,
                               size_t max_codepoints = kTextFieldMaxLength);

std::string BuildHandshakePayload(const std::string& application_id,
                                  const std::string& nonce);
std::string BuildActivityJson(const Activity& activity);
std::string BuildSetActivityPayload(const Activity& activity, int64_t pid,
                                    const std::string& nonce, bool include_activity);

// Encodes [opcode][length][payload] exactly as it goes on the wire.
std::string EncodeFrame(uint32_t opcode, const std::string& payload);

// Reads an 8-byte frame header. Returns false when the buffer holds less than
// a header; a frame is then parsed from the accumulated inbound bytes.
bool ParseFrameHeader(const char* data, size_t size, uint32_t* opcode, uint32_t* length);

// Maps data.config.api_endpoint from the READY dispatch onto the flavour it
// identifies: "stable", "ptb", "canary", or "unknown" (logged as-is).
std::string FlavourFromEndpoint(const std::string& endpoint);

// --- Protocol state machine ------------------------------------------------
//
// Owns everything that decides *what* the client should say, with no pipe I/O
// at all, so the READY gate, the coalescing rule and the frame classification
// are unit testable. Client drives it; Session never touches a HANDLE.
class Session {
 public:
  Session(std::string application_id, int64_t pid);

  // Starts a connection attempt: resets the per-connection protocol state and
  // returns the HANDSHAKE payload to write. A buffered presence survives, and
  // nothing is sent until READY arrives.
  std::string BeginAttempt();

  // Tells the session the pipe is gone. The requested presence is kept so it
  // is published again once the next attempt reaches READY.
  void OnDisconnected();

  // Buffers a presence request. Rapid calls coalesce onto the latest value;
  // before READY the value waits instead of being dropped.
  void RequestActivity(const Activity& activity);
  void RequestClear();

  // Emits the next frame that may be written now, i.e. a SET_ACTIVITY once the
  // connection is READY and no previous update is still awaiting its echo.
  bool TakeOutbound(OutboundFrame* frame, int64_t now_ms);

  // Abandons an update whose echo never arrived after kEchoTimeoutSeconds, so
  // a silent server cannot block the next update forever. Returns true when an
  // in-flight update was abandoned (the caller logs it).
  bool ExpireInFlight(int64_t now_ms);

  // Classification of one decoded inbound frame.
  struct Inbound {
    std::vector<OutboundFrame> replies;  // frames to write back (PONG)
    bool recognized = false;             // matched a protocol case below
    bool ready = false;                  // DISPATCH READY: handshake complete
    bool accepted = false;               // echo of our SET_ACTIVITY nonce
    bool command_error = false;          // evt:"ERROR"
    bool handshake_error = false;        // ERROR before READY: abort attempt
    bool fatal = false;                  // CLOSE frame
    uint32_t opcode = 0;
    std::string cmd;
    std::string evt;
    std::string nonce;
    uint32_t close_code = 0;
    std::string close_message;
    std::string error_code;
    std::string error_message;
    std::string endpoint;          // READY data.config.api_endpoint
    std::string protocol_version;  // READY data.v
    std::string user_id;           // READY data.user.id
    std::string echoed_data;       // SET_ACTIVITY echo data (applied activity)
  };
  Inbound OnInbound(uint32_t opcode, const std::string& payload);

  bool ready() const { return state_ == State::kReady; }
  bool awaiting_ready() const { return state_ == State::kAwaitingReady; }
  bool in_flight() const { return in_flight_; }
  bool has_pending() const { return has_pending_; }
  const std::string& in_flight_nonce() const { return in_flight_nonce_; }
  const std::string& endpoint() const { return endpoint_; }
  const std::string& protocol_version() const { return protocol_version_; }

 private:
  enum class State { kIdle, kAwaitingReady, kReady };

  bool BuildPresenceFrame(OutboundFrame* frame, int64_t now_ms);

  std::string application_id_;
  int64_t pid_ = 0;
  State state_ = State::kIdle;

  bool has_pending_ = false;
  bool pending_is_clear_ = false;
  Activity pending_;

  // Last published intent, re-queued when a new connection reaches READY
  // because the server forgets the presence when the pipe drops.
  bool has_last_intent_ = false;
  bool last_intent_was_clear_ = true;
  Activity last_intent_;

  bool in_flight_ = false;
  std::string in_flight_nonce_;
  int64_t in_flight_at_ms_ = 0;

  std::string endpoint_;
  std::string protocol_version_;
};

// --- Connection ------------------------------------------------------------

class Client {
 public:
  explicit Client(const std::string& application_id);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Requests a connection attempt when disconnected and reports whether the
  // pipe is currently usable. Never blocks.
  bool EnsureConnected();

  // Queues an activity update. The worker coalesces rapid calls onto the
  // latest value and publishes it once the connection is READY and the
  // previous update has been acknowledged.
  bool SetActivity(const Activity& activity);

  // Queues a presence clear (SET_ACTIVITY without an activity object).
  void Clear();

  // Stops the worker, sends a deliberate presence clear on a READY connection
  // when possible, and closes the pipe.
  void Shutdown();

  bool Connected() const { return connected_.load(); }

  // Optional frame log, added for diagnosing the transport. Off by default:
  // nothing is written anywhere until SetLogPath() names a file. The log holds
  // protocol frames only; the local IPC payloads carry no credential or token.
  // Cheap when disabled (one relaxed atomic load per call site).
  void SetLogPath(const std::string& utf8_path);
  void SetLogging(bool enabled);

  // Appends one caller-supplied line to the existing frame log (same file,
  // same enabled flag, same format and mutex as the protocol lines). No-op
  // while logging is off. Thread-safe.
  void LogLine(const std::string& message);

 private:
  using Clock = std::chrono::steady_clock;

  void WorkerMain();
  bool Connect();
  void Disconnect(const char* reason, unsigned long error);
  void ScheduleReconnect(const char* reason);

  bool WriteFrame(uint32_t opcode, const std::string& payload);
  // Overlapped pipe I/O with a bounded wait; a timeout cancels the request and
  // reports failure so the caller disconnects instead of blocking forever.
  bool WriteAll(const char* data, size_t size);
  bool ReadChunk(char* buffer, DWORD size, DWORD* read);
  // Drains every byte the pipe currently holds, then parses whole frames.
  void PumpInbound();
  bool ProcessInbound();
  bool HandleInbound(uint32_t opcode, const std::string& payload);

  bool LoggingEnabled() const { return log_wanted_.load(std::memory_order_relaxed); }
  void Log(const std::string& message);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::thread worker_;

  // Shared between the caller and the worker.
  mutable std::mutex mutex_;
  mutable std::mutex log_mutex_;
  std::condition_variable wake_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> log_wanted_{false};
  std::string log_path_;
  std::string application_id_;

  bool has_pending_ = false;
  bool pending_is_clear_ = false;
  Activity pending_;

  Clock::time_point next_connect_attempt_{};
  int reconnect_delay_seconds_ = kReconnectDelaySeconds;

  // Worker-owned; never touched by the caller.
  bool connected_now_ = false;
  Session session_;
  std::string inbound_buffer_;
  int64_t last_inbound_ms_ = 0;
  int64_t next_starvation_log_ms_ = 0;
  int64_t attempt_started_ms_ = 0;
  int64_t in_flight_sent_ms_ = 0;
  int64_t frames_in_ = 0;
  int64_t frames_out_ = 0;
  uint32_t last_close_code_ = 0;
};

}  // namespace DiscordIpc

#endif  // AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_
