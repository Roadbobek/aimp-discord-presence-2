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

#include "discord_ipc.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace DiscordIpc {
namespace {

constexpr size_t kFrameHeaderSize = 8;
constexpr size_t kMaxFrameBytes = 1024 * 1024;
constexpr size_t kMaxInboundBufferBytes = 2 * 1024 * 1024;

constexpr DWORD kPipeReadChunk = 64 * 1024;

// Bounded waits for the overlapped pipe I/O. A stalled Discord must never
// block AIMP's caller behind a write, so a timeout cancels the request and
// the caller disconnects.
constexpr DWORD kWriteTimeoutMilliseconds = 2000;
constexpr DWORD kReadTimeoutMilliseconds = 1000;
constexpr char kReplacementChar[] = "\xEF\xBF\xBD";  // U+FFFD

int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsContinuationByte(unsigned char value) { return (value & 0xC0) == 0x80; }

// Length in bytes of the UTF-8 sequence starting at `index`, or 1 when the
// sequence is invalid or truncated (callers replace such bytes).
size_t Utf8SequenceLength(const std::string& input, size_t index) {
  const unsigned char lead = static_cast<unsigned char>(input[index]);
  const size_t remaining = input.size() - index;

  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    if (remaining >= 2 && IsContinuationByte(static_cast<unsigned char>(input[index + 1]))) {
      return 2;
    }
    return 1;
  }
  if ((lead & 0xF0) == 0xE0) {
    if (remaining >= 3 && IsContinuationByte(static_cast<unsigned char>(input[index + 1])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 2]))) {
      return 3;
    }
    return 1;
  }
  if ((lead & 0xF8) == 0xF0) {
    if (remaining >= 4 && IsContinuationByte(static_cast<unsigned char>(input[index + 1])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 2])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 3]))) {
      return 4;
    }
    return 1;
  }
  return 1;
}

void AppendUint32LE(std::string* buffer, uint32_t value) {
  buffer->push_back(static_cast<char>(value & 0xFF));
  buffer->push_back(static_cast<char>((value >> 8) & 0xFF));
  buffer->push_back(static_cast<char>((value >> 16) & 0xFF));
  buffer->push_back(static_cast<char>((value >> 24) & 0xFF));
}

uint32_t ReadUint32LE(const char* data) {
  return static_cast<uint32_t>(static_cast<unsigned char>(data[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[3])) << 24);
}

std::string NextNonce() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t value = counter.fetch_add(1);
  const uint64_t ticks = static_cast<uint64_t>(GetTickCount64());
  return std::to_string(ticks) + "-" + std::to_string(value);
}

void AppendEscapedField(std::string& json, const char* key, const std::string& value) {
  json += ",\"";
  json += key;
  json += "\":\"";
  json += EscapeJsonString(value);
  json += "\"";
}

void AppendTextField(std::string& json, const char* key, const std::string& value) {
  const std::string normalized = NormalizeTextField(value, kTextFieldMaxLength);
  if (normalized.empty()) {
    return;
  }
  AppendEscapedField(json, key, normalized);
}

// An optional caption (large_text, small_text). Discord's text fields have a
// two-character minimum; unlike details/state, a one-character caption is
// omitted rather than padded, because an empty tooltip is better than an
// invalid one.
void AppendOptionalTextField(std::string& json, const char* key,
                             const std::string& value) {
  if (value.empty() || TruncateUtf8(value, 1).size() == value.size()) {
    return;
  }
  AppendTextField(json, key, value);
}

void AppendImageField(std::string& json, const char* key, const std::string& value) {
  const std::string normalized = TruncateUtf8(value, kImageFieldMaxLength);
  if (normalized.empty()) {
    return;
  }
  AppendEscapedField(json, key, normalized);
}

// The only URL scheme Discord accepts on an activity button (http is not
// accepted by the client).
constexpr char kButtonUrlScheme[] = "https://";
// Shown when a configured button label is empty after truncation: Discord
// rejects a button without a label.
constexpr char kDefaultButtonLabel[] = "Listen";

bool IsPresentButtonUrl(const std::string& url) {
  return url.compare(0, sizeof(kButtonUrlScheme) - 1, kButtonUrlScheme) == 0;
}

// --- Minimal JSON reader ---------------------------------------------------
//
// The transport only ever reads a handful of fields out of a frame, so a full
// JSON library would be an unnecessary dependency. These helpers walk the
// document string-aware and never match a key inside a string value.

bool IsJsonSpace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool IsJsonDelimiter(char value) {
  return value == ',' || value == '}' || value == ']' || IsJsonSpace(value);
}

// Index just past the closing quote of the JSON string starting at `start`
// (which must point at the opening quote), or npos when unterminated.
size_t JsonStringEnd(const std::string& json, size_t start) {
  size_t index = start + 1;
  while (index < json.size()) {
    if (json[index] == '\\') {
      index += 2;
      continue;
    }
    if (json[index] == '"') {
      return index + 1;
    }
    ++index;
  }
  return std::string::npos;
}

// Finds `"key"` followed by a colon and returns the index of its value.
bool FindKeyValue(const std::string& json, const std::string& key, size_t* value_start) {
  size_t index = 0;
  while (index < json.size()) {
    if (json[index] != '"') {
      ++index;
      continue;
    }

    const size_t end = JsonStringEnd(json, index);
    if (end == std::string::npos) {
      return false;
    }

    const std::string token = json.substr(index + 1, end - index - 2);
    size_t cursor = end;
    while (cursor < json.size() && IsJsonSpace(json[cursor])) {
      ++cursor;
    }
    if (token == key && cursor < json.size() && json[cursor] == ':') {
      ++cursor;
      while (cursor < json.size() && IsJsonSpace(json[cursor])) {
        ++cursor;
      }
      *value_start = cursor;
      return true;
    }

    index = end;
  }
  return false;
}

void AppendUtf8Codepoint(std::string* out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

uint32_t ParseHex4(const std::string& text, size_t start) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    const char current = text[start + i];
    uint32_t digit = 0;
    if (current >= '0' && current <= '9') {
      digit = static_cast<uint32_t>(current - '0');
    } else if (current >= 'a' && current <= 'f') {
      digit = static_cast<uint32_t>(current - 'a') + 10;
    } else if (current >= 'A' && current <= 'F') {
      digit = static_cast<uint32_t>(current - 'A') + 10;
    } else {
      return 0xFFFD;
    }
    value = (value << 4) | digit;
  }
  return value;
}

std::string UnescapeJsonString(const std::string& value) {
  std::string out;
  out.reserve(value.size());

  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out.push_back(value[i]);
      continue;
    }

    const char escape = value[++i];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        if (i + 4 >= value.size()) {
          AppendUtf8Codepoint(&out, 0xFFFD);
          break;
        }
        uint32_t codepoint = ParseHex4(value, i + 1);
        i += 4;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 6 < value.size() &&
            value[i + 1] == '\\' && value[i + 2] == 'u') {
          const uint32_t low = ParseHex4(value, i + 3);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        AppendUtf8Codepoint(&out, codepoint);
        break;
      }
      default:
        out.push_back(escape);
        break;
    }
  }

  return out;
}

bool JsonFindString(const std::string& json, const std::string& key, std::string* out) {
  size_t start = 0;
  if (!FindKeyValue(json, key, &start) || start >= json.size() || json[start] != '"') {
    return false;
  }
  const size_t end = JsonStringEnd(json, start);
  if (end == std::string::npos) {
    return false;
  }
  *out = UnescapeJsonString(json.substr(start + 1, end - start - 2));
  return true;
}

// Raw scalar token for a key (numbers, booleans); strings are unquoted.
bool JsonFindRaw(const std::string& json, const std::string& key, std::string* out) {
  size_t start = 0;
  if (!FindKeyValue(json, key, &start) || start >= json.size()) {
    return false;
  }
  if (json[start] == '"') {
    const size_t end = JsonStringEnd(json, start);
    if (end == std::string::npos) {
      return false;
    }
    *out = json.substr(start + 1, end - start - 2);
    return true;
  }
  size_t end = start;
  while (end < json.size() && !IsJsonDelimiter(json[end])) {
    ++end;
  }
  *out = json.substr(start, end - start);
  return true;
}

// Balanced object/array value for a key, braces included.
bool JsonFindObject(const std::string& json, const std::string& key, std::string* out) {
  size_t start = 0;
  if (!FindKeyValue(json, key, &start) || start >= json.size()) {
    return false;
  }
  const char open = json[start];
  if (open != '{' && open != '[') {
    return false;
  }
  const char close = (open == '{') ? '}' : ']';

  int depth = 0;
  size_t index = start;
  while (index < json.size()) {
    const char current = json[index];
    if (current == '"') {
      const size_t end = JsonStringEnd(json, index);
      if (end == std::string::npos) {
        return false;
      }
      index = end;
      continue;
    }
    if (current == open) {
      ++depth;
    } else if (current == close) {
      --depth;
      if (depth == 0) {
        *out = json.substr(start, index - start + 1);
        return true;
      }
    }
    ++index;
  }
  return false;
}

bool ParseUint32(const std::string& text, uint32_t* value) {
  if (text.empty()) {
    return false;
  }
  uint64_t result = 0;
  for (const char current : text) {
    if (current < '0' || current > '9') {
      return false;
    }
    result = result * 10 + static_cast<uint64_t>(current - '0');
    if (result > 0xFFFFFFFFull) {
      return false;
    }
  }
  *value = static_cast<uint32_t>(result);
  return true;
}

}  // namespace

std::string EscapeJsonString(const std::string& utf8) {
  static const char* kHexDigits = "0123456789abcdef";

  std::string escaped;
  escaped.reserve(utf8.size());

  for (size_t i = 0; i < utf8.size();) {
    const unsigned char value = static_cast<unsigned char>(utf8[i]);

    if (value == '"' || value == '\\') {
      escaped.push_back('\\');
      escaped.push_back(static_cast<char>(value));
      ++i;
    } else if (value < 0x20) {
      escaped += "\\u00";
      escaped.push_back(kHexDigits[(value >> 4) & 0x0F]);
      escaped.push_back(kHexDigits[value & 0x0F]);
      ++i;
    } else if (value < 0x80) {
      escaped.push_back(static_cast<char>(value));
      ++i;
    } else {
      const size_t length = Utf8SequenceLength(utf8, i);
      if (length > 1) {
        escaped.append(utf8, i, length);
        i += length;
      } else {
        escaped += kReplacementChar;
        ++i;
      }
    }
  }

  return escaped;
}

std::string TruncateUtf8(const std::string& utf8, size_t max_codepoints) {
  size_t codepoints = 0;
  size_t index = 0;

  while (index < utf8.size() && codepoints < max_codepoints) {
    index += Utf8SequenceLength(utf8, index);
    ++codepoints;
  }

  return utf8.substr(0, index);
}

std::string NormalizeTextField(const std::string& utf8, size_t max_codepoints) {
  if (utf8.empty()) {
    return std::string();
  }

  std::string normalized = TruncateUtf8(utf8, max_codepoints);
  if (normalized.empty()) {
    return normalized;
  }

  // Discord rejects details/state/assets text shorter than 2 characters, so a
  // single-character value is padded instead of being dropped.
  if (TruncateUtf8(normalized, 1).size() == normalized.size()) {
    normalized.push_back(' ');
  }

  return normalized;
}

std::string BuildHandshakePayload(const std::string& application_id,
                                  const std::string& nonce) {
  // Opcode 0 is not a command frame: Discord reads `v` and `client_id` at the
  // top level of a bare object and closes with 4000 (INVALID_CLIENTID) when
  // they arrive inside the cmd/nonce/args envelope used by opcode 1.
  (void)nonce;
  std::string payload = "{\"v\":1,\"client_id\":\"";
  payload += EscapeJsonString(application_id);
  payload += "\"}";
  return payload;
}

std::string BuildActivityJson(const Activity& activity) {
  std::string json = "{\"type\":";
  json += std::to_string(activity.type);

  AppendTextField(json, "details", activity.details);
  AppendTextField(json, "state", activity.state);

  if (activity.start_timestamp > 0 || activity.end_timestamp > 0) {
    json += ",\"timestamps\":{";
    bool needs_comma = false;
    if (activity.start_timestamp > 0) {
      json += "\"start\":";
      json += std::to_string(activity.start_timestamp);
      needs_comma = true;
    }
    if (activity.end_timestamp > 0) {
      if (needs_comma) {
        json += ",";
      }
      json += "\"end\":";
      json += std::to_string(activity.end_timestamp);
    }
    json += "}";
  }

  std::string assets;
  AppendImageField(assets, "large_image", activity.large_image);
  AppendImageField(assets, "small_image", activity.small_image);
  AppendOptionalTextField(assets, "small_text", activity.small_text);
  // large_text is the cover-art tooltip (the album by default). It is appended
  // after the pre-existing asset fields so their wire order is untouched; it is
  // emitted only when configured and at least two characters long.
  AppendOptionalTextField(assets, "large_text", activity.large_text);
  if (!assets.empty()) {
    json += ",\"assets\":{";
    json += assets.substr(1);  // drop the leading comma the helpers prepend
    json += "}";
  }

  if (activity.status_display_type >= 0 && activity.status_display_type <= 2) {
    json += ",\"status_display_type\":";
    json += std::to_string(activity.status_display_type);
  }

  // `name` and `buttons` are appended last so the wire order of every field
  // above stays byte-identical. A button is present only when its url is an
  // https:// URL; its label is truncated, and an empty label becomes "Listen"
  // because Discord drops a button that has no label.
  if (!activity.name.empty()) {
    AppendEscapedField(json, "name",
                       TruncateUtf8(activity.name, kActivityNameMaxLength));
  }

  std::string buttons;
  size_t emitted_buttons = 0;
  for (const ActivityButton& button : activity.buttons) {
    if (emitted_buttons >= kMaxActivityButtons) {
      break;
    }
    if (!IsPresentButtonUrl(button.url)) {
      continue;
    }

    std::string label = TruncateUtf8(button.label, kButtonLabelMaxLength);
    if (label.empty()) {
      label = kDefaultButtonLabel;
    }

    if (!buttons.empty()) {
      buttons += ",";
    }
    buttons += "{\"label\":\"";
    buttons += EscapeJsonString(label);
    buttons += "\",\"url\":\"";
    buttons += EscapeJsonString(TruncateUtf8(button.url, kButtonUrlMaxLength));
    buttons += "\"}";
    ++emitted_buttons;
  }
  if (!buttons.empty()) {
    json += ",\"buttons\":[";
    json += buttons;
    json += "]";
  }

  json += "}";
  return json;
}

std::string BuildSetActivityPayload(const Activity& activity, int64_t pid,
                                    const std::string& nonce, bool include_activity) {
  std::string args = "{\"pid\":";
  args += std::to_string(pid);
  if (include_activity) {
    args += ",\"activity\":";
    args += BuildActivityJson(activity);
  }
  args += "}";

  std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"";
  payload += EscapeJsonString(nonce);
  payload += "\",\"args\":";
  payload += args;
  payload += "}";
  return payload;
}

std::string EncodeFrame(uint32_t opcode, const std::string& payload) {
  std::string frame;
  frame.reserve(payload.size() + kFrameHeaderSize);
  AppendUint32LE(&frame, opcode);
  AppendUint32LE(&frame, static_cast<uint32_t>(payload.size()));
  frame.append(payload);
  return frame;
}

bool ParseFrameHeader(const char* data, size_t size, uint32_t* opcode, uint32_t* length) {
  if (data == nullptr || opcode == nullptr || length == nullptr || size < kFrameHeaderSize) {
    return false;
  }
  *opcode = ReadUint32LE(data);
  *length = ReadUint32LE(data + 4);
  return true;
}

std::string FlavourFromEndpoint(const std::string& endpoint) {
  if (endpoint.find("canary.discord.com") != std::string::npos) {
    return "canary";
  }
  if (endpoint.find("ptb.discord.com") != std::string::npos) {
    return "ptb";
  }
  if (endpoint.find("discord.com") != std::string::npos) {
    return "stable";
  }
  return "unknown";
}

// --- Session ---------------------------------------------------------------

Session::Session(std::string application_id, int64_t pid)
    : application_id_(std::move(application_id)), pid_(pid) {}

std::string Session::BeginAttempt() {
  state_ = State::kAwaitingReady;
  in_flight_ = false;
  in_flight_nonce_.clear();
  in_flight_at_ms_ = 0;
  endpoint_.clear();
  protocol_version_.clear();
  return BuildHandshakePayload(application_id_, NextNonce());
}

void Session::OnDisconnected() {
  state_ = State::kIdle;
  in_flight_ = false;
  in_flight_nonce_.clear();
  in_flight_at_ms_ = 0;
}

void Session::RequestActivity(const Activity& activity) {
  pending_ = activity;
  pending_is_clear_ = false;
  has_pending_ = true;
}

void Session::RequestClear() {
  pending_ = Activity();
  pending_is_clear_ = true;
  has_pending_ = true;
}

bool Session::TakeOutbound(OutboundFrame* frame, int64_t now_ms) {
  if (frame == nullptr || state_ != State::kReady || in_flight_ || !has_pending_) {
    return false;
  }
  return BuildPresenceFrame(frame, now_ms);
}

bool Session::BuildPresenceFrame(OutboundFrame* frame, int64_t now_ms) {
  const std::string nonce = NextNonce();
  frame->opcode = kOpcodeFrame;
  frame->payload = BuildSetActivityPayload(pending_, pid_, nonce, !pending_is_clear_);

  in_flight_ = true;
  in_flight_nonce_ = nonce;
  in_flight_at_ms_ = now_ms;
  has_pending_ = false;

  // Remember the latest intent: Discord forgets the presence when the pipe
  // drops, so it has to be republished after a reconnect.
  has_last_intent_ = !pending_is_clear_;
  last_intent_was_clear_ = pending_is_clear_;
  if (!pending_is_clear_) {
    last_intent_ = pending_;
  }
  return true;
}

bool Session::ExpireInFlight(int64_t now_ms) {
  if (!in_flight_) {
    return false;
  }
  const int64_t timeout_ms = static_cast<int64_t>(kEchoTimeoutSeconds) * 1000;
  if (now_ms - in_flight_at_ms_ < timeout_ms) {
    return false;
  }
  in_flight_ = false;
  in_flight_nonce_.clear();
  in_flight_at_ms_ = 0;
  return true;
}

Session::Inbound Session::OnInbound(uint32_t opcode, const std::string& payload) {
  Inbound result;
  result.opcode = opcode;

  if (opcode == kOpcodePing) {
    OutboundFrame pong;
    pong.opcode = kOpcodePong;
    pong.payload = payload;  // the PONG carries the same payload back
    result.replies.push_back(pong);
    result.recognized = true;
    return result;
  }

  if (opcode == kOpcodeClose) {
    result.recognized = true;
    result.fatal = true;
    std::string code;
    if (JsonFindRaw(payload, "code", &code)) {
      ParseUint32(code, &result.close_code);
    }
    JsonFindString(payload, "message", &result.close_message);
    OnDisconnected();
    return result;
  }

  if (opcode != kOpcodeFrame) {
    return result;
  }

  JsonFindString(payload, "cmd", &result.cmd);
  JsonFindString(payload, "evt", &result.evt);
  JsonFindString(payload, "nonce", &result.nonce);

  if (result.evt == "READY" && (result.cmd.empty() || result.cmd == "DISPATCH")) {
    result.recognized = true;
    result.ready = true;
    JsonFindRaw(payload, "v", &result.protocol_version);

    std::string data;
    if (JsonFindObject(payload, "data", &data)) {
      std::string config;
      if (JsonFindObject(data, "config", &config)) {
        JsonFindString(config, "api_endpoint", &result.endpoint);
      }
      if (result.endpoint.empty()) {
        JsonFindString(data, "api_endpoint", &result.endpoint);
      }
      std::string user;
      if (JsonFindObject(data, "user", &user)) {
        JsonFindString(user, "id", &result.user_id);
      }
    }

    endpoint_ = result.endpoint;
    protocol_version_ = result.protocol_version;
    state_ = State::kReady;

    // A presence requested before READY was buffered; it is published now. If
    // nothing is buffered, the last intent is republished because a reconnect
    // means the server forgot it.
    if (!has_pending_ && has_last_intent_) {
      has_pending_ = true;
      pending_is_clear_ = last_intent_was_clear_;
      pending_ = last_intent_;
    }
    return result;
  }

  if (result.evt == "ERROR") {
    result.recognized = true;
    result.command_error = true;
    result.handshake_error = (state_ != State::kReady);

    std::string data;
    if (JsonFindObject(payload, "data", &data)) {
      JsonFindRaw(data, "code", &result.error_code);
      JsonFindString(data, "message", &result.error_message);
    }
    if (result.error_code.empty()) {
      JsonFindRaw(payload, "code", &result.error_code);
    }

    if (!result.nonce.empty() && result.nonce == in_flight_nonce_) {
      in_flight_ = false;
      in_flight_nonce_.clear();
      in_flight_at_ms_ = 0;
    }
    if (result.handshake_error) {
      state_ = State::kIdle;
    }
    return result;
  }

  if (result.cmd == "SET_ACTIVITY") {
    result.recognized = true;
    if (!result.nonce.empty() && result.nonce == in_flight_nonce_) {
      result.accepted = true;
      std::string data;
      if (JsonFindObject(payload, "data", &data)) {
        result.echoed_data = data;
      }
      in_flight_ = false;
      in_flight_nonce_.clear();
      in_flight_at_ms_ = 0;
    }
    return result;
  }

  return result;
}

// --- Client ----------------------------------------------------------------

Client::Client(const std::string& application_id)
    : application_id_(application_id),
      session_(application_id, static_cast<int64_t>(GetCurrentProcessId())) {
  next_connect_attempt_ = Clock::now();
  worker_ = std::thread(&Client::WorkerMain, this);
}

Client::~Client() { Shutdown(); }

void Client::SetLogPath(const std::string& utf8_path) {
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_path_ = utf8_path;
  }
  log_wanted_.store(!utf8_path.empty(), std::memory_order_relaxed);
}

void Client::SetLogging(bool enabled) {
  log_wanted_.store(enabled, std::memory_order_relaxed);
}

void Client::LogLine(const std::string& message) { Log(message); }

void Client::Log(const std::string& message) {
  if (!LoggingEnabled()) {
    return;
  }

  std::lock_guard<std::mutex> lock(log_mutex_);
  if (log_path_.empty()) {
    return;
  }

  std::FILE* file = nullptr;
  if (fopen_s(&file, log_path_.c_str(), "ab") != 0 || file == nullptr) {
    return;
  }

  SYSTEMTIME local = {};
  GetLocalTime(&local);
  char stamp[64] = {};
  _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
              local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
              local.wSecond, local.wMilliseconds);

  std::string line = stamp;
  line += " [t";
  line += std::to_string(GetCurrentThreadId());
  line += "] discord-ipc: ";
  line += message;
  line += "\r\n";
  std::fwrite(line.data(), 1, line.size(), file);
  std::fclose(file);
}

bool Client::EnsureConnected() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next_connect_attempt_ = Clock::now();
  }
  wake_.notify_all();
  return connected_.load();
}

bool Client::SetActivity(const Activity& activity) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) {
      return false;
    }
    pending_ = activity;
    pending_is_clear_ = false;
    has_pending_ = true;
  }
  wake_.notify_all();
  return true;
}

void Client::Clear() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) {
      return;
    }
    pending_ = Activity();
    pending_is_clear_ = true;
    has_pending_ = true;
  }
  wake_.notify_all();
}

void Client::Shutdown() {
  stopping_.store(true);
  wake_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  // The worker's exit path clears the presence deliberately and closes the
  // pipe; nothing is left to send or close here.
  connected_now_ = false;
  connected_.store(false);
}

bool Client::Connect() {
  static const wchar_t kPipePrefix[] = L"\\\\.\\pipe\\discord-ipc-";

  attempt_started_ms_ = NowMilliseconds();
  in_flight_sent_ms_ = 0;
  frames_in_ = 0;
  frames_out_ = 0;
  last_close_code_ = 0;
  last_inbound_ms_ = attempt_started_ms_;
  next_starvation_log_ms_ = attempt_started_ms_ + kStarvationWarnSeconds * 1000;
  inbound_buffer_.clear();

  // The pipe index is not tied to a flavour: every Discord instance shares the
  // range, so the first index that opens wins and the flavour is identified
  // from the READY dispatch instead.
  for (int index = 0; index < 10; ++index) {
    const std::wstring pipe_name = std::wstring(kPipePrefix) + std::to_wstring(index);
    const HANDLE handle = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (LoggingEnabled()) {
        Log("connect pipe=discord-ipc-" + std::to_string(index) + " result=fail error=" +
            std::to_string(error));
      }
      continue;
    }

    if (LoggingEnabled()) {
      Log("connect pipe=discord-ipc-" + std::to_string(index) + " result=ok");
    }
    pipe_ = handle;
    connected_now_ = true;
    connected_.store(true);

    // The HANDSHAKE is all that may be written before READY is read back.
    if (!WriteFrame(kOpcodeHandshake, session_.BeginAttempt())) {
      const DWORD error = GetLastError();
      if (LoggingEnabled()) {
        Log("handshake write failed on discord-ipc-" + std::to_string(index) +
            " error=" + std::to_string(error));
      }
      CancelIoEx(pipe_, nullptr);
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
      connected_now_ = false;
      connected_.store(false);
      continue;
    }

    if (LoggingEnabled()) {
      Log("attached to pipe=discord-ipc-" + std::to_string(index) +
          "; awaiting READY before any SET_ACTIVITY");
    }
    return true;
  }

  ScheduleReconnect("no discord-ipc pipe could be opened");
  return false;
}

void Client::Disconnect(const char* reason, unsigned long error) {
  const bool was_connected = connected_now_;
  const bool was_ready = session_.ready();
  const std::string endpoint = session_.endpoint();

  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  inbound_buffer_.clear();
  session_.OnDisconnected();
  connected_now_ = false;
  connected_.store(false);

  if (LoggingEnabled()) {
    Log(std::string("disconnect reason=") + reason + " error=" + std::to_string(error) +
        " ready=" + (was_ready ? "true" : "false") +
        " close_code=" + std::to_string(last_close_code_) +
        " endpoint=" + endpoint + " flavour=" + FlavourFromEndpoint(endpoint));
  }

  if (was_connected && !stopping_.load()) {
    ScheduleReconnect(reason);
  }
}

void Client::ScheduleReconnect(const char* reason) {
  const int delay_seconds = reconnect_delay_seconds_;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next_connect_attempt_ = Clock::now() + std::chrono::seconds(delay_seconds);
  }
  if (LoggingEnabled()) {
    Log(std::string("reconnect scheduled in ") + std::to_string(delay_seconds) +
        "s reason=" + reason);
  }
  reconnect_delay_seconds_ = (std::min)(reconnect_delay_seconds_ * 2, kReconnectDelayMaxSeconds);
}

bool Client::WriteFrame(uint32_t opcode, const std::string& payload) {
  if (pipe_ == INVALID_HANDLE_VALUE || payload.size() > 0xFFFFFFFFull) {
    return false;
  }

  const std::string frame = EncodeFrame(opcode, payload);
  const bool success = WriteAll(frame.data(), frame.size());
  if (LoggingEnabled()) {
    std::string line = "out opcode=" + std::to_string(opcode) +
                       " len=" + std::to_string(payload.size()) + " ";
    if (success) {
      line += payload;
    } else {
      line += "failed error=" + std::to_string(GetLastError());
    }
    Log(line);
  }
  if (success) {
    ++frames_out_;
  }
  return success;
}

bool Client::WriteAll(const char* data, size_t size) {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  // The pipe is opened with FILE_FLAG_OVERLAPPED, so every write must carry an
  // OVERLAPPED structure. Waiting with a timeout turns a stalled Discord into
  // a disconnect instead of a hung AIMP message thread.
  OVERLAPPED overlapped = {};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return false;
  }

  DWORD written = 0;
  bool success = false;
  const BOOL started = WriteFile(pipe_, data, static_cast<DWORD>(size), &written, &overlapped);
  if (started) {
    success = written == static_cast<DWORD>(size);
  } else if (GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(overlapped.hEvent, kWriteTimeoutMilliseconds) == WAIT_OBJECT_0) {
      DWORD transferred = 0;
      success = GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) != FALSE &&
                transferred == static_cast<DWORD>(size);
    } else {
      // The wait must not outlive the OVERLAPPED it waits on.
      CancelIoEx(pipe_, &overlapped);
      WaitForSingleObject(overlapped.hEvent, kWriteTimeoutMilliseconds);
    }
  }

  CloseHandle(overlapped.hEvent);
  return success;
}

bool Client::ReadChunk(char* buffer, DWORD size, DWORD* read) {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return false;
  }

  *read = 0;
  bool success = false;
  const BOOL started = ReadFile(pipe_, buffer, size, read, &overlapped);
  if (started) {
    success = *read > 0;
  } else if (GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(overlapped.hEvent, kReadTimeoutMilliseconds) == WAIT_OBJECT_0) {
      success = GetOverlappedResult(pipe_, &overlapped, read, FALSE) != FALSE && *read > 0;
    } else {
      CancelIoEx(pipe_, &overlapped);
      WaitForSingleObject(overlapped.hEvent, kReadTimeoutMilliseconds);
    }
  }

  CloseHandle(overlapped.hEvent);
  return success;
}

void Client::PumpInbound() {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return;
  }

  // Reading only bytes PeekNamedPipe reports cannot block and never needs a
  // timeout cancel, so no partially received frame can be thrown away. This
  // loop is the connection's continuous inbound read.
  for (;;) {
    if (pipe_ == INVALID_HANDLE_VALUE) {
      return;
    }

    DWORD available = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
      Disconnect("peek failed (pipe closed)", GetLastError());
      return;
    }
    if (available == 0) {
      return;
    }

    const DWORD chunk = (available < kPipeReadChunk) ? available : kPipeReadChunk;
    std::vector<char> buffer(chunk);
    DWORD read = 0;
    if (!ReadChunk(buffer.data(), chunk, &read)) {
      Disconnect("read failed", GetLastError());
      return;
    }
    if (read == 0) {
      return;
    }

    inbound_buffer_.append(buffer.data(), read);
    last_inbound_ms_ = NowMilliseconds();
    if (inbound_buffer_.size() > kMaxInboundBufferBytes) {
      Disconnect("inbound frame buffer overflow", 0);
      return;
    }
    if (!ProcessInbound()) {
      return;
    }
  }
}

bool Client::ProcessInbound() {
  size_t offset = 0;
  while (inbound_buffer_.size() - offset >= kFrameHeaderSize) {
    uint32_t opcode = 0;
    uint32_t length = 0;
    if (!ParseFrameHeader(inbound_buffer_.data() + offset, inbound_buffer_.size() - offset,
                          &opcode, &length)) {
      break;
    }
    if (static_cast<size_t>(length) > kMaxFrameBytes) {
      Disconnect("inbound frame exceeds the size limit", 0);
      return false;
    }
    const size_t payload_offset = offset + kFrameHeaderSize;
    if (inbound_buffer_.size() - payload_offset < static_cast<size_t>(length)) {
      break;  // the rest of the frame has not arrived yet
    }

    const std::string payload = inbound_buffer_.substr(payload_offset, length);
    offset = payload_offset + length;
    if (!HandleInbound(opcode, payload)) {
      return false;
    }
  }

  if (offset > 0) {
    inbound_buffer_.erase(0, offset);
  }
  return true;
}

bool Client::HandleInbound(uint32_t opcode, const std::string& payload) {
  ++frames_in_;
  const int64_t now_ms = NowMilliseconds();
  last_inbound_ms_ = now_ms;

  const Session::Inbound inbound = session_.OnInbound(opcode, payload);

  if (LoggingEnabled()) {
    std::string line = "in opcode=" + std::to_string(opcode) +
                       " len=" + std::to_string(payload.size());
    if (opcode == kOpcodeFrame) {
      line += " cmd=" + inbound.cmd + " evt=" + inbound.evt + " nonce=" + inbound.nonce;
    }
    Log(line);
  }

  for (size_t i = 0; i < inbound.replies.size(); ++i) {
    if (!WriteFrame(inbound.replies[i].opcode, inbound.replies[i].payload)) {
      Disconnect("pong write failed", GetLastError());
      return false;
    }
  }

  if (inbound.ready) {
    reconnect_delay_seconds_ = kReconnectDelaySeconds;
    if (LoggingEnabled()) {
      Log("ready v=" + inbound.protocol_version + " endpoint=" + inbound.endpoint +
          " flavour=" + FlavourFromEndpoint(inbound.endpoint) +
          " user_id=" + inbound.user_id +
          " latency_ms=" + std::to_string(now_ms - attempt_started_ms_));
      Log("read-loop attached: frame_in=" + std::to_string(frames_in_) +
          " frame_out=" + std::to_string(frames_out_));
    }
  }

  if (inbound.accepted) {
    if (LoggingEnabled()) {
      Log("echo cmd=SET_ACTIVITY nonce=" + inbound.nonce + " accepted" +
          (in_flight_sent_ms_ > 0
               ? " echo_latency_ms=" + std::to_string(now_ms - in_flight_sent_ms_)
               : std::string()) +
          " data=" + inbound.echoed_data);
    }
  }

  if (inbound.command_error) {
    if (LoggingEnabled()) {
      Log("error code=" + inbound.error_code + " message=\"" + inbound.error_message +
          "\" nonce=" + inbound.nonce +
          (inbound.handshake_error ? " phase=handshake" : " phase=ready"));
    }
    if (inbound.handshake_error) {
      Disconnect("handshake rejected by the server", 0);
      return false;
    }
  }

  if (inbound.fatal) {
    last_close_code_ = inbound.close_code;
    if (LoggingEnabled()) {
      Log("close code=" + std::to_string(inbound.close_code) + " message=\"" +
          inbound.close_message + "\"");
    }
    Disconnect("server sent CLOSE", 0);
    return false;
  }

  if (!inbound.recognized && LoggingEnabled()) {
    Log("in unhandled opcode=" + std::to_string(opcode) +
        " cmd=" + inbound.cmd + " evt=" + inbound.evt);
  }

  return true;
}

void Client::WorkerMain() {
  for (;;) {
    if (stopping_.load()) {
      break;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (has_pending_) {
        if (pending_is_clear_) {
          session_.RequestClear();
        } else {
          session_.RequestActivity(pending_);
        }
        has_pending_ = false;
      }
    }

    if (!connected_now_) {
      Clock::time_point next_attempt;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        next_attempt = next_connect_attempt_;
      }
      if (Clock::now() >= next_attempt) {
        Connect();
      }
    }

    if (connected_now_) {
      // Read before write: READY, PING and CLOSE all have to be consumed
      // before another command may be issued.
      PumpInbound();

      if (connected_now_) {
        const int64_t now_ms = NowMilliseconds();

        if (session_.ExpireInFlight(now_ms) && LoggingEnabled()) {
          Log("echo timeout after " + std::to_string(kEchoTimeoutSeconds) +
              "s; abandoning the in-flight update so the next one can be sent");
        }

        OutboundFrame frame;
        if (session_.TakeOutbound(&frame, now_ms)) {
          in_flight_sent_ms_ = now_ms;
          if (!WriteFrame(frame.opcode, frame.payload)) {
            Disconnect("set_activity write failed", GetLastError());
          }
        }

        if (connected_now_ && now_ms >= next_starvation_log_ms_) {
          if (LoggingEnabled()) {
            Log("read-loop alive: no inbound frame for " +
                std::to_string(now_ms - last_inbound_ms_) + "ms (frames_in=" +
                std::to_string(frames_in_) +
                " ready=" + (session_.ready() ? "true" : "false") +
                " in_flight=" + (session_.in_flight() ? "true" : "false") + ")");
          }
          next_starvation_log_ms_ = now_ms + kStarvationWarnSeconds * 1000;
        }
      }

      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_.load()) {
        lock.unlock();
        break;
      }
      wake_.wait_until(lock, Clock::now() + std::chrono::milliseconds(kPollIntervalMilliseconds));
      continue;
    }

    Clock::time_point next_attempt;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      next_attempt = next_connect_attempt_;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_.load()) {
      lock.unlock();
      break;
    }
    wake_.wait_until(lock, next_attempt);
  }

  // Unloading must not leave the last track shown on the profile: on a live
  // READY connection the presence is cleared deliberately (SET_ACTIVITY with
  // activity:null, which is what an omitted activity means). The write is
  // bounded, and the echo is not awaited, so unload stays prompt.
  if (connected_now_ && session_.ready()) {
    OutboundFrame clear;
    clear.opcode = kOpcodeFrame;
    clear.payload = BuildSetActivityPayload(Activity(),
                                            static_cast<int64_t>(GetCurrentProcessId()),
                                            NextNonce(), false);
    if (WriteFrame(clear.opcode, clear.payload) && LoggingEnabled()) {
      Log("shutdown: presence cleared deliberately (activity:null)");
    }
  }

  Disconnect("worker stopped", 0);
}

}  // namespace DiscordIpc
