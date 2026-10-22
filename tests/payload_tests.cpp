// Offline unit tests for the Discord activity JSON builder
// (src/discord_ipc.cpp): the `name` and `buttons` fields, Discord's documented
// length limits, and the wire shape of the fields that already existed.
//
// No third-party framework and no network. Built by CMake as `payload_tests`
// and registered with CTest; run the whole suite with:
//   ctest --test-dir build -C Release --output-on-failure

#include "../src/discord_ipc.h"
#include "../src/presence_layout.h"
#include "../src/presence_rate_limit.h"
#include "../src/presence_throttle_plan.h"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& label) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

// --- Tiny JSON probes -------------------------------------------------------
//
// The builder emits flat objects and strings only, so a real parser would be
// more machinery than the assertions need.

bool HasField(const std::string& json, const std::string& key) {
  return json.find("\"" + key + "\":") != std::string::npos;
}

std::string FieldValue(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\":\"";
  const size_t start = json.find(token);
  if (start == std::string::npos) {
    return std::string();
  }
  const size_t value_start = start + token.size();
  const size_t end = json.find('"', value_start);
  if (end == std::string::npos) {
    return std::string();
  }
  return json.substr(value_start, end - value_start);
}

// Text of the `buttons` array, or an empty string when the field is absent.
std::string ButtonsJson(const std::string& json) {
  const std::string token = "\"buttons\":[";
  const size_t start = json.find(token);
  if (start == std::string::npos) {
    return std::string();
  }
  const size_t body = start + token.size();
  const size_t end = json.find(']', body);
  if (end == std::string::npos) {
    return std::string();
  }
  return json.substr(body, end - body);
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  size_t index = text.find(needle);
  while (index != std::string::npos) {
    ++count;
    index = text.find(needle, index + needle.size());
  }
  return count;
}

// --- UTF-8 probes -----------------------------------------------------------

size_t Utf8SequenceLength(const std::string& text, size_t index) {
  const unsigned char lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

bool IsValidUtf8(const std::string& text) {
  size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    size_t length = 1;
    if (lead < 0x80) {
      length = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (size_t offset = 1; offset < length; ++offset) {
      if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) {
        return false;
      }
    }
    index += length;
  }
  return true;
}

size_t CodepointCount(const std::string& text) {
  size_t count = 0;
  size_t index = 0;
  while (index < text.size()) {
    index += Utf8SequenceLength(text, index);
    ++count;
  }
  return count;
}

DiscordIpc::Activity DefaultButtonsActivity() {
  DiscordIpc::Activity activity;
  activity.buttons[0].label = "Listen on Spotify";
  activity.buttons[0].url = "https://open.spotify.com/search/";
  activity.buttons[1].label = "Find on YouTube";
  activity.buttons[1].url = "https://music.youtube.com/search?q=";
  return activity;
}

// --- (a) name ---------------------------------------------------------------

void TestNamePresentWhenSet() {
  DiscordIpc::Activity activity;
  activity.name = "Music";
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(HasField(json, "name"), "name present when set");
  Check(FieldValue(json, "name") == "Music", "name value is emitted verbatim");
}

void TestNameAbsentWhenEmpty() {
  DiscordIpc::Activity activity;
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(!HasField(json, "name"), "name omitted when empty");
}

void TestNameTruncatedTo128() {
  DiscordIpc::Activity activity;
  activity.name = std::string(200, 'n');
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string name = FieldValue(json, "name");

  Check(name.size() == 128, "name truncated to 128 characters");
  Check(CodepointCount(name) == 128, "name holds 128 codepoints");
}

// --- (b) two buttons --------------------------------------------------------

void TestTwoButtonsEmitted() {
  const std::string json =
      DiscordIpc::BuildActivityJson(DefaultButtonsActivity());

  const std::string expected =
      "{\"label\":\"Listen on Spotify\",\"url\":\"https://open.spotify.com/search/\"},"
      "{\"label\":\"Find on YouTube\",\"url\":\"https://music.youtube.com/search?q=\"}";
  Check(ButtonsJson(json) == expected, "two buttons carry label and url in order");
}

// --- (c) empty url drops the slot -------------------------------------------

void TestButtonWithEmptyUrlOmitted() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].url.clear();
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string buttons = ButtonsJson(json);

  Check(CountOccurrences(buttons, "{\"label\":") == 1,
        "only one button survives an empty url");
  Check(buttons.find("Listen on Spotify") == std::string::npos,
        "the empty-url button label is not emitted");
  Check(buttons.find("Find on YouTube") != std::string::npos,
        "the remaining button is emitted");
}

// Both slots empty must omit the field entirely: `"buttons":[]` is not the
// same wire shape as an absent key, and Discord rejects the empty array.
void TestButtonsKeyAbsentWhenBothUrlsEmpty() {
  DiscordIpc::Activity activity;
  activity.buttons[0].label = "Listen on Spotify";
  activity.buttons[1].label = "Find on YouTube";
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(!HasField(json, "buttons"),
        "both button urls empty emits no buttons key, not an empty array");
  Check(json.find("\"buttons\"") == std::string::npos,
        "the buttons token is absent from the payload");
}

// The positive counterpart: one non-empty url yields exactly one object, and
// the array closes right after it with no trailing comma.
void TestSingleButtonArrayShape() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].url.clear();
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string expected =
      "{\"label\":\"Find on YouTube\","
      "\"url\":\"https://music.youtube.com/search?q=\"}";

  Check(ButtonsJson(json) == expected,
        "one surviving url serialises as an array of exactly one object");
  Check(json.find("\"buttons\":[" + expected + "]") != std::string::npos,
        "the single-object buttons array has no trailing comma");
}

// --- (d) non-https url drops the slot ---------------------------------------

void TestNonHttpsUrlDropped() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].url = "http://insecure.example.com/x";
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string buttons = ButtonsJson(json);

  Check(CountOccurrences(buttons, "{\"label\":") == 1,
        "a non-https button is dropped");
  Check(buttons.find("http://") == std::string::npos,
        "no http:// url reaches the payload");
  Check(buttons.find("Find on YouTube") != std::string::npos,
        "the https button after it is still emitted");
}

// --- (e) label truncation ---------------------------------------------------

void TestLabelTruncatedTo32() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].label = std::string(40, 'x');
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string label = FieldValue(ButtonsJson(json), "label");

  Check(label.size() == 32, "label truncated to 32 characters");
  Check(label == std::string(32, 'x'), "truncated label keeps the leading text");
}

void TestEmptyLabelFallsBackToListen() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].label.clear();
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string buttons = ButtonsJson(json);

  Check(buttons.find("\"label\":\"Listen\"") != std::string::npos,
        "an empty label becomes Listen");
}

// --- (f) url truncation -----------------------------------------------------

void TestUrlTruncatedTo512() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.buttons[0].url = "https://example.com/" + std::string(600, 'a');
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string url = FieldValue(ButtonsJson(json), "url");

  Check(url.size() == 512, "url truncated to 512 characters");
  Check(url.compare(0, 20, "https://example.com/") == 0,
        "truncated url keeps its https prefix");
}

// --- (g) UTF-8 truncation ---------------------------------------------------

void TestUtf8TruncationDoesNotSplitCharacter() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  // 31 ASCII characters, then a 5-character CJK word: codepoint 32 is the
  // first byte of a 3-byte sequence, the exact boundary that a byte-wise
  // truncation would corrupt.
  const std::string cjk = "\xE5\x90\x9B\xE3\x81\xA8\xE3\x81\x84\xE3\x81\x86"
                          "\xE8\x8A\xB1";
  activity.buttons[0].label = std::string(31, 'a') + cjk;
  const std::string json = DiscordIpc::BuildActivityJson(activity);
  const std::string label = FieldValue(ButtonsJson(json), "label");

  Check(IsValidUtf8(json), "payload stays valid UTF-8 at a CJK boundary");
  Check(CodepointCount(label) == 32, "label holds exactly 32 codepoints");
  Check(label.size() == 34, "label keeps 31 ASCII bytes plus one 3-byte CJK codepoint");
  Check(label == std::string(31, 'a') + "\xE5\x90\x9B",
        "label ends on a whole CJK codepoint, never a split sequence");
}

// --- (h) pre-existing fields ------------------------------------------------

void TestExistingFieldsUnchanged() {
  DiscordIpc::Activity activity;
  activity.type = 2;
  activity.details = "lovely";
  activity.state = "Billie Eilish, Khalid";
  activity.large_image = "https://example.com/cover.jpg";
  activity.large_text = "lovely (with Khalid)";
  activity.small_image = "aimp_play";
  activity.small_text = "Playing";
  activity.start_timestamp = 100;
  activity.end_timestamp = 200;
  activity.status_display_type = 2;

  const std::string expected =
      R"({"type":2,"details":"lovely","state":"Billie Eilish, Khalid",)"
      R"("timestamps":{"start":100,"end":200},)"
      R"("assets":{"large_image":"https://example.com/cover.jpg",)"
      R"("small_image":"aimp_play","small_text":"Playing",)"
      R"json("large_text":"lovely (with Khalid)")json"
      R"(},"status_display_type":2})";

  Check(DiscordIpc::BuildActivityJson(activity) == expected,
        "pre-existing fields keep their exact values and order");
  Check(FieldValue(DiscordIpc::BuildActivityJson(activity), "large_text") ==
            "lovely (with Khalid)",
        "large_text is emitted when set");
}

// The fixture quoted in the ticket's wire-shape report.
std::string BuildWireFixture() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.type = 2;
  activity.details = "lovely";
  activity.state = "Billie Eilish, Khalid";
  activity.large_text = "lovely (with Khalid)";
  activity.name = "Music";
  activity.status_display_type = 2;
  return DiscordIpc::BuildActivityJson(activity);
}

void TestWireFixtureShape() {
  const std::string json = BuildWireFixture();

  Check(IsValidUtf8(json), "fixture stays valid UTF-8");
  Check(FieldValue(json, "name") == "Music", "fixture carries the app name");
  Check(CountOccurrences(ButtonsJson(json), "{\"label\":") == 2,
        "fixture carries both buttons");
}

// --- (i) template expansion -------------------------------------------------

PresenceLayout::TrackFields SampleFields() {
  PresenceLayout::TrackFields fields;
  fields.title = "lovely";
  fields.artist = "Billie Eilish, Khalid";
  fields.album = "lovely (with Khalid)";
  fields.year = "2018";
  fields.genre = "Pop";
  return fields;
}

void TestTemplatePlainSubstitution() {
  const PresenceLayout::TrackFields fields = SampleFields();
  Check(PresenceLayout::FormatTemplate("{title}", fields, false) == "lovely",
        "a single token expands to its tag value");
}

void TestTemplateMultipleTokens() {
  const PresenceLayout::TrackFields fields = SampleFields();
  Check(PresenceLayout::FormatTemplate("{title} - {artist}", fields, false) ==
            "lovely - Billie Eilish, Khalid",
        "multiple tokens expand in place");
  Check(PresenceLayout::FormatTemplate("{album} ({year}) [{genre}]", fields,
                                       false) ==
            "lovely (with Khalid) (2018) [Pop]",
        "album, year and genre all expand");
}

void TestTemplateUnknownTokenSurvives() {
  const PresenceLayout::TrackFields fields = SampleFields();
  Check(PresenceLayout::FormatTemplate("{title} {bpm}", fields, false) ==
            "lovely {bpm}",
        "an unknown token survives verbatim");
}

void TestTemplateEmptyValueExpandsEmpty() {
  PresenceLayout::TrackFields fields = SampleFields();
  fields.album.clear();
  Check(PresenceLayout::FormatTemplate("[{album}]", fields, false) == "[]",
        "an empty tag expands to nothing");
}

void TestTemplateLoneBracesAreLiteral() {
  const PresenceLayout::TrackFields fields = SampleFields();
  Check(PresenceLayout::FormatTemplate("a { b", fields, false) == "a { b",
        "a lone opening brace is literal");
  Check(PresenceLayout::FormatTemplate("a } b", fields, false) == "a } b",
        "a lone closing brace is literal");
}

void TestTemplateUrlEncodingOffLeavesSpacesRaw() {
  PresenceLayout::TrackFields fields = SampleFields();
  fields.title = "a b";
  Check(PresenceLayout::FormatTemplate("{title}", fields, false) == "a b",
        "urlEncode=false leaves spaces raw");
}

void TestTemplateUrlEncodingOnEncodesSpaces() {
  PresenceLayout::TrackFields fields = SampleFields();
  fields.title = "a b";
  Check(PresenceLayout::FormatTemplate("{title}", fields, true) == "a%20b",
        "urlEncode=true encodes a space as %20");
}

void TestTemplateUrlEncodingOnEncodesCjk() {
  PresenceLayout::TrackFields fields = SampleFields();
  fields.title = "\xE5\x90\x9B\xE3\x81\xA8\xE3\x81\x84\xE3\x81\x86\xE8\x8A\xB1";
  Check(PresenceLayout::FormatTemplate("{title}", fields, true) ==
            "%E5%90%9B%E3%81%A8%E3%81%84%E3%81%86%E8%8A%B1",
        "a CJK title becomes UTF-8 percent escapes");
}

void TestTemplateUrlEncodingKeepsLiteralPercent() {
  PresenceLayout::TrackFields fields = SampleFields();
  fields.title = "a b";
  Check(PresenceLayout::FormatTemplate("https://x/%2F?q={title}", fields,
                                       true) ==
            "https://x/%2F?q=a%20b",
        "literal template text is not encoded, so %2F survives");
  fields.title = "A-z_~.9";
  Check(PresenceLayout::FormatTemplate("{title}", fields, true) == "A-z_~.9",
        "RFC 3986 unreserved bytes stay raw");
}

// --- (j) large_text ---------------------------------------------------------

void TestLargeTextRoundTrips() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.large_text = "lovely (with Khalid)";
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(HasField(json, "large_text"), "large_text is present when non-empty");
  Check(FieldValue(json, "large_text") == "lovely (with Khalid)",
        "large_text value round-trips verbatim");
}

void TestLargeTextAbsentWhenEmpty() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.large_text.clear();
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(!HasField(json, "large_text"), "large_text is absent when empty");
}

void TestLargeTextOneCharacterOmitted() {
  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.large_text = "x";
  const std::string json = DiscordIpc::BuildActivityJson(activity);

  Check(!HasField(json, "large_text"),
        "a one-character large_text is omitted, not padded");
}

// --- (k) small_text per state ----------------------------------------------

PresenceLayout::SmallBadgeSettings BadgeSettings() {
  PresenceLayout::SmallBadgeSettings settings;
  settings.use_play = true;
  settings.play_image = "aimp_play";
  settings.play_text = "Playing";
  settings.use_pause = true;
  settings.pause_image = "aimp_pause";
  settings.pause_text = "Paused";
  settings.use_radio = true;
  settings.radio_image = "aimp_radio";
  settings.radio_text = "Live";
  return settings;
}

void TestSmallTextPerState() {
  const PresenceLayout::SmallBadgeSettings settings = BadgeSettings();

  const PresenceLayout::SmallBadge playing =
      PresenceLayout::SelectSmallBadge(false, 2, settings);
  Check(playing.image == "aimp_play" && playing.text == "Playing",
        "playing state carries the play image and PlayText");

  const PresenceLayout::SmallBadge paused =
      PresenceLayout::SelectSmallBadge(false, 1, settings);
  Check(paused.image == "aimp_pause" && paused.text == "Paused",
        "paused state carries the pause image and PauseText");

  const PresenceLayout::SmallBadge radio =
      PresenceLayout::SelectSmallBadge(true, 2, settings);
  Check(radio.image == "aimp_radio" && radio.text == "Live",
        "radio state carries the radio image and RadioText");
}

void TestSmallTextShortValueOmitted() {
  PresenceLayout::SmallBadgeSettings settings = BadgeSettings();
  settings.play_text = "x";

  const PresenceLayout::SmallBadge badge =
      PresenceLayout::SelectSmallBadge(false, 2, settings);
  Check(badge.image == "aimp_play",
        "the badge image survives a too-short text");
  Check(badge.text.empty(), "a one-character small_text is cleared");

  DiscordIpc::Activity activity = DefaultButtonsActivity();
  activity.small_text = badge.text;
  Check(!HasField(DiscordIpc::BuildActivityJson(activity), "small_text"),
        "small_text is absent when the configured value is too short");
}

void TestSmallTextAbsentWhenRadioTextEmpty() {
  PresenceLayout::SmallBadgeSettings settings = BadgeSettings();
  settings.radio_text.clear();

  const PresenceLayout::SmallBadge badge =
      PresenceLayout::SelectSmallBadge(true, 2, settings);
  Check(badge.image == "aimp_radio", "the radio image is still selected");
  Check(badge.text.empty(), "an empty RadioText leaves no small_text");
}

// --- (l) artist normalisation ----------------------------------------------

void TestArtistSeparatorNormalised() {
  Check(PresenceLayout::NormalizeArtistSeparators("Billie Eilish,Khalid") ==
            "Billie Eilish, Khalid",
        "a bare comma becomes comma-space");
  Check(PresenceLayout::NormalizeArtistSeparators("Billie Eilish,Khalid,"
                                                  "Vince Staples") ==
            "Billie Eilish, Khalid, Vince Staples",
        "every separator gains one space");
}

void TestArtistSingleValueUntouched() {
  Check(PresenceLayout::NormalizeArtistSeparators("Billie Eilish") ==
            "Billie Eilish",
        "a single artist is untouched");
  Check(PresenceLayout::NormalizeArtistSeparators("A, B") == "A, B",
        "an already-correct pair is untouched");
}

// --- (m) rate limit ---------------------------------------------------------

void TestShouldSendNow() {
  Check(PresenceRateLimit::ShouldSendNow(12345, 0, 4000),
        "the first send is allowed from lastSent=0");
  Check(!PresenceRateLimit::ShouldSendNow(8999, 5000, 4000),
        "a send one millisecond before the deadline is blocked");
  Check(PresenceRateLimit::ShouldSendNow(9000, 5000, 4000),
        "a send exactly at the deadline is allowed");
  Check(PresenceRateLimit::ShouldSendNow(5001, 5000, 0),
        "minInterval=0 always allows a send");
}

void TestNormalizeThrottleMs() {
  // Exactly 0 disables the limit; anything else below the 1000 ms floor is
  // clamped up to it, so a negative ThrottleMs can never disable the cap.
  struct Case {
    int configured;
    uint32_t expected;
    const char* label;
  };
  const Case cases[] = {
      {-5, 1000, "a negative value clamps up to the floor, it does not disable"},
      {-1, 1000, "the smallest negative value clamps up to the floor"},
      {0, 0, "exactly 0 disables the limit"},
      {1, 1000, "a positive value below the floor is raised to 1000"},
      {999, 1000, "one millisecond below the floor is raised to 1000"},
      {1000, 1000, "the floor passes through unchanged"},
      {4000, 4000, "the default interval passes through"},
      {5000, 5000, "a value above the default passes through"},
  };
  for (const Case& c : cases) {
    Check(PresenceRateLimit::NormalizeThrottleMs(c.configured) == c.expected,
          c.label);
  }
}

// --- (n) single-writer throttle plan ---------------------------------------
//
// The throttle worker is the only thread that calls into the Discord client.
// These checks model the revision/interval decisions it makes, including the
// two orderings that used to be bugs: a clear that arrives while a send is in
// flight, and newer state that arrives while the worker waits for a deadline.
// The plan is pure, so the checks need no client, no clock and no threads.

PresenceThrottlePlan::Request PlanRequest(bool valid, uint64_t desired_revision,
                                          uint64_t published_revision,
                                          uint64_t gate_ms) {
  PresenceThrottlePlan::Request request;
  request.desired_valid = valid;
  request.desired_revision = desired_revision;
  request.last_sent_revision = published_revision;
  request.last_sent_ms = gate_ms;
  return request;
}

void TestThrottlePlanFirstRequestSends() {
  using namespace PresenceThrottlePlan;

  const Decision decision = Decide(PlanRequest(true, 1, 0, 0), 1000, 4000);
  Check(decision.action == Action::kSend, "the first request of a session sends");
  Check(decision.wait_ms == 0, "an allowed send carries no wait");
}

void TestThrottlePlanIdleAfterPublish() {
  using namespace PresenceThrottlePlan;

  Check(Decide(PlanRequest(true, 7, 7, 4000), 5000, 4000).action == Action::kIdle,
        "a published card leaves the worker idle");
  // Idle is about revisions, not about whether the published state was a card
  // or a clear: a published clear must not be repeated every interval.
  Check(Decide(PlanRequest(false, 7, 7, 0), 5000, 4000).action == Action::kIdle,
        "a published clear leaves the worker idle");
}

void TestThrottlePlanWaitsForTheInterval() {
  using namespace PresenceThrottlePlan;

  const Decision early = Decide(PlanRequest(true, 2, 1, 5000), 5100, 4000);
  Check(early.action == Action::kWait, "a newer card inside the interval waits");
  Check(early.wait_ms == 3900, "the wait is the exact remaining interval");

  const Decision last_ms = Decide(PlanRequest(true, 2, 1, 5000), 8999, 4000);
  Check(last_ms.action == Action::kWait && last_ms.wait_ms == 1,
        "one millisecond before the deadline still waits");

  Check(Decide(PlanRequest(true, 2, 1, 5000), 9000, 4000).action == Action::kSend,
        "the deadline sends the latest state");
}

// The B3 hole, as a state sequence: the worker has chosen a send and released
// the lock to perform it; a stop arrives before the call returns.
void TestThrottlePlanClearBeatsInFlightSend() {
  using namespace PresenceThrottlePlan;

  // Revision 2 is chosen for sending at t=9000, with the gate open.
  const Request in_flight = PlanRequest(true, 2, 1, 5000);
  Check(Decide(in_flight, 9000, 4000).action == Action::kSend,
        "the revision chosen while the gate is open is sent");

  // While that call is in flight the user stops: revision 3, invalid, and the
  // clear request reset the gate. The completion then stores the gate it finds.
  const uint64_t gate_after_completion =
      GateAfterAction(Action::kSend, 5000, 0, 9000);
  Check(gate_after_completion == 0,
        "a send completing after a clear leaves the clear's reset gate intact");

  // Because the clear bumped the revision, the next decision is the clear
  // itself: not a wait, and not the stale send again.
  const Decision next = Decide(
      PlanRequest(false, 3, 2, gate_after_completion), 9001, 4000);
  Check(next.action == Action::kClear,
        "a clear that arrived during a send is issued on the next iteration");
  Check(next.wait_ms == 0, "the clear is not held behind the superseded send");

  // Counterfactual kept as a regression guard: if the completion had overwritten
  // the reset, the same state would carry a full interval on the gate. Clears are
  // ungated, so the clear is still issued at once - which is exactly why the
  // clear path no longer depends on the gate preservation above.
  Check(Decide(PlanRequest(false, 3, 2, 9000), 9001, 4000).action == Action::kClear,
        "a clear survives an overwritten reset, because a clear ignores the gate");
}

void TestThrottlePlanNewerStateReplacesPendingWait() {
  using namespace PresenceThrottlePlan;

  const Decision waiting = Decide(PlanRequest(true, 2, 1, 5000), 6000, 4000);
  Check(waiting.action == Action::kWait && waiting.wait_ms == 3000,
        "the worker waits out the interval for a pending card");

  // A new track bumps the revision during that wait. The remaining time is
  // measured from the current gate, never carried over from the old wait.
  const Decision replaced = Decide(PlanRequest(true, 3, 1, 5000), 6100, 4000);
  Check(replaced.action == Action::kWait && replaced.wait_ms == 2900,
        "newer state is measured against the gate, not the previous wait");

  // At the deadline the decision sends; the desired snapshot is read from the
  // shared state at that moment, so it is revision 3 that goes out.
  Check(Decide(PlanRequest(true, 3, 1, 5000), 9000, 4000).action == Action::kSend,
        "the deadline sends the newest state, not the first one queued");
}

void TestThrottlePlanClearDuringWaitIsImmediate() {
  using namespace PresenceThrottlePlan;

  Check(Decide(PlanRequest(true, 2, 1, 5000), 6000, 4000).action == Action::kWait,
        "a pending card is waiting for its deadline");

  // A stop resets the gate as part of the clear request, so the clear does not
  // inherit the wait that belonged to the card.
  Check(Decide(PlanRequest(false, 3, 1, 0), 6010, 4000).action == Action::kClear,
        "a clear during a pending wait is published without waiting");
}

// B5: a clear is a terminal state change. It cannot be coalesced into a later
// send, so the interval - which exists to stop track-change churn - gates a
// send only. The two decisions below share every input except `desired_valid`,
// which isolates the gate as the cause: the card waits, the clear does not.
void TestThrottlePlanClearIgnoresFreshGate() {
  using namespace PresenceThrottlePlan;

  const Decision send = Decide(PlanRequest(true, 2, 1, 9000), 9001, 4000);
  Check(send.action == Action::kWait && send.wait_ms == 3999,
        "a send 1 ms after the gate is stamped waits out the interval");

  const Decision clear = Decide(PlanRequest(false, 2, 1, 9000), 9001, 4000);
  Check(clear.action == Action::kClear,
        "the same fresh gate does not hold a pending clear");
  Check(clear.wait_ms == 0, "a clear is returned with no wait at all");
}

// B5, the verifier's exact sequence. The gate is 0 because nothing has been
// sent yet; a send is chosen and released to the client; a clear arrives while
// that call is in flight and resets the gate to 0. The reset is invisible to
// GateAfterAction - 0 and 0 compare equal - so the completion stamps the send's
// own time. Under the old policy that stamp put the clear behind a full
// interval; clears are ungated now, so the clear still goes out 1 ms later.
void TestThrottlePlanClearAfterZeroGateReset() {
  using namespace PresenceThrottlePlan;

  Check(Decide(PlanRequest(true, 2, 1, 0), 9000, 4000).action == Action::kSend,
        "an open gate sends the first card while nothing is on screen");

  const uint64_t gate_after_completion =
      GateAfterAction(Action::kSend, 0, 0, 9000);
  Check(gate_after_completion == 9000,
        "a 0 -> 0 reset is indistinguishable from startup, so the send stamps the gate");

  const Decision next = Decide(PlanRequest(false, 3, 2, gate_after_completion),
                               9001, 4000);
  Check(next.action == Action::kClear,
        "the clear is still issued at once, 1 ms after the send");
  Check(next.wait_ms == 0,
        "the clear carries no wait even though the gate says 3999 ms remain");
}

// Ungated clears must not make a published clear repeat: revision equality is
// still the first rule, so a clear that is already on screen is not re-sent.
void TestThrottlePlanPublishedClearIsIdle() {
  using namespace PresenceThrottlePlan;

  const Decision decision = Decide(PlanRequest(false, 4, 4, 9000), 9001, 4000);
  Check(decision.action == Action::kIdle,
        "a published clear leaves the worker idle");
  Check(decision.wait_ms == 0, "idle carries no wait");
}

void TestThrottlePlanSendStillWaitsOnFreshGate() {
  using namespace PresenceThrottlePlan;

  const Decision early = Decide(PlanRequest(true, 5, 4, 9000), 9001, 4000);
  Check(early.action == Action::kWait,
        "the interval still gates a send after B5");
  Check(early.wait_ms == 3999,
        "the send's wait is the exact remaining interval");

  const Decision at_deadline = Decide(PlanRequest(true, 5, 4, 9000), 13000, 4000);
  Check(at_deadline.action == Action::kSend,
        "the send is released exactly at its deadline");
}

void TestThrottlePlanZeroIntervalSendsImmediately() {
  using namespace PresenceThrottlePlan;

  const Decision decision = Decide(PlanRequest(true, 6, 5, 9000), 9001, 0);
  Check(decision.action == Action::kSend,
        "interval 0 sends a pending card at once");
  Check(decision.wait_ms == 0, "an immediate send carries no wait");
}

// A newer send replacing a pending send must not inherit the earlier card's
// remaining wait: the decision is recomputed from the gate every time, and the
// state read at the deadline is the latest one.
void TestThrottlePlanNewerSendWinsDuringWait() {
  using namespace PresenceThrottlePlan;

  const Decision first = Decide(PlanRequest(true, 7, 6, 9000), 9100, 4000);
  Check(first.action == Action::kWait && first.wait_ms == 3900,
        "the first pending card waits out the interval");

  const Decision replaced = Decide(PlanRequest(true, 8, 6, 9000), 9200, 4000);
  Check(replaced.action == Action::kWait && replaced.wait_ms == 3800,
        "a newer send is measured against the gate, not the previous wait");

  Check(Decide(PlanRequest(true, 8, 6, 9000), 13000, 4000).action == Action::kSend,
        "the deadline sends the latest revision, not the one first queued");
}

void TestThrottleGateAfterAction() {
  using namespace PresenceThrottlePlan;

  Check(GateAfterAction(Action::kSend, 4000, 4000, 9000) == 9000,
        "an undisturbed send advances the gate to its completion time");
  Check(GateAfterAction(Action::kSend, 0, 0, 9000) == 9000,
        "the first send of a session opens the gate");
  Check(GateAfterAction(Action::kClear, 0, 0, 9500) == 0,
        "a clear does not consume interval budget");
}

// --- (o) default template layout -------------------------------------------

void TestDefaultTemplatesProduceTheAlbumTooltip() {
  const PresenceLayout::TrackFields fields = SampleFields();
  const PresenceLayout::TextFields text =
      PresenceLayout::BuildTextFields(fields, "{title}", "{artist}", "{album}");

  Check(text.details == "lovely", "Details={title} fills line 2 with the title");
  Check(text.state == "Billie Eilish, Khalid",
        "State={artist} fills line 3 with the artist");
  Check(text.large_text == "lovely (with Khalid)",
        "LargeText={album} fills the cover tooltip with the album");
}

}  // namespace

int main() {
  TestNamePresentWhenSet();
  TestNameAbsentWhenEmpty();
  TestNameTruncatedTo128();
  TestTwoButtonsEmitted();
  TestButtonWithEmptyUrlOmitted();
  TestButtonsKeyAbsentWhenBothUrlsEmpty();
  TestSingleButtonArrayShape();
  TestNonHttpsUrlDropped();
  TestLabelTruncatedTo32();
  TestEmptyLabelFallsBackToListen();
  TestUrlTruncatedTo512();
  TestUtf8TruncationDoesNotSplitCharacter();
  TestExistingFieldsUnchanged();
  TestWireFixtureShape();
  TestTemplatePlainSubstitution();
  TestTemplateMultipleTokens();
  TestTemplateUnknownTokenSurvives();
  TestTemplateEmptyValueExpandsEmpty();
  TestTemplateLoneBracesAreLiteral();
  TestTemplateUrlEncodingOffLeavesSpacesRaw();
  TestTemplateUrlEncodingOnEncodesSpaces();
  TestTemplateUrlEncodingOnEncodesCjk();
  TestTemplateUrlEncodingKeepsLiteralPercent();
  TestLargeTextRoundTrips();
  TestLargeTextAbsentWhenEmpty();
  TestLargeTextOneCharacterOmitted();
  TestSmallTextPerState();
  TestSmallTextShortValueOmitted();
  TestSmallTextAbsentWhenRadioTextEmpty();
  TestArtistSeparatorNormalised();
  TestArtistSingleValueUntouched();
  TestShouldSendNow();
  TestNormalizeThrottleMs();
  TestThrottlePlanFirstRequestSends();
  TestThrottlePlanIdleAfterPublish();
  TestThrottlePlanWaitsForTheInterval();
  TestThrottlePlanClearBeatsInFlightSend();
  TestThrottlePlanNewerStateReplacesPendingWait();
  TestThrottlePlanClearDuringWaitIsImmediate();
  TestThrottlePlanClearIgnoresFreshGate();
  TestThrottlePlanClearAfterZeroGateReset();
  TestThrottlePlanPublishedClearIsIdle();
  TestThrottlePlanSendStillWaitsOnFreshGate();
  TestThrottlePlanZeroIntervalSendsImmediately();
  TestThrottlePlanNewerSendWinsDuringWait();
  TestThrottleGateAfterAction();
  TestDefaultTemplatesProduceTheAlbumTooltip();

  std::printf("fixture: %s\n", BuildWireFixture().c_str());
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
