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

#ifndef AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_
#define AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_

#include <cstddef>
#include <string>
#include <string_view>

// The Discord presence field mapping, kept in one place:
//
//   details    (line 2)  the expanded `Details` template (default `{title}`);
//                        falls back to the artist, then to the literal "AIMP",
//                        so the line is never blank
//   state      (line 3)  the expanded `State` template (default `{artist}`);
//                        falls back to the album, and is omitted when that
//                        would repeat `details`
//   large_text           the expanded `LargeText` template (default
//                        `{album}`), shown as the cover-art tooltip; omitted
//                        when empty or shorter than Discord's 2-character
//                        minimum
//   large_image          the resolved album art URL; empty (omitted) when no
//                        art was resolved, so Discord renders its native
//                        placeholder (never an asset key, never a
//                        stand-in image URL)
//
// No field repeats a value already emitted by an earlier one; empty values
// count as absent.
//
// Templates understand `{title}`, `{artist}`, `{album}`, `{year}` and
// `{genre}`. An unknown `{token}` survives verbatim, a lone brace is literal,
// and an empty tag expands to nothing.
//
// With status_display_type = 2 (Details), Discord renders `details` - the
// track title - in the member-list status line.
namespace PresenceLayout {

// There is no stand-in image: when no album art URL was resolved the field
// stays empty and is omitted from the wire JSON (AppendImageField skips
// empties), so the card shows Discord's native placeholder. A bundled asset
// key such as `aimp` must never be used either: it belongs to a Discord
// application this plugin does NOT own, so it would resolve only while that
// foreign application happens to serve it.

// Literal shown on line 2 when both the artist and track title tags are empty.
constexpr const char* kFallbackDetails = "AIMP";

// Every tag value a template may substitute. Populated from the same AIMP
// file-info read that the card already uses, so a template never costs an
// extra metadata read.
struct TrackFields {
  std::string title;
  std::string artist;
  std::string album;
  std::string year;
  std::string genre;
};

namespace detail {

inline const std::string* FieldForToken(const TrackFields& fields,
                                        std::string_view token) {
  if (token == "title") {
    return &fields.title;
  }
  if (token == "artist") {
    return &fields.artist;
  }
  if (token == "album") {
    return &fields.album;
  }
  if (token == "year") {
    return &fields.year;
  }
  if (token == "genre") {
    return &fields.genre;
  }
  return nullptr;
}

inline bool IsUnreserved(unsigned char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '.' ||
         value == '_' || value == '~';
}

// RFC 3986 percent encoding over the UTF-8 bytes: unreserved ASCII stays raw,
// everything else becomes uppercase %XX. A space is therefore %20, never '+'.
inline std::string PercentEncode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";

  std::string encoded;
  encoded.reserve(value.size());
  for (const char current : value) {
    const unsigned char byte = static_cast<unsigned char>(current);
    if (IsUnreserved(byte)) {
      encoded.push_back(current);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(byte >> 4) & 0x0F]);
      encoded.push_back(kHex[byte & 0x0F]);
    }
  }
  return encoded;
}

// Number of UTF-8 codepoints, tolerating a truncated trailing sequence by
// counting it as one unit. Only the 2-character minimum is decided from this.
inline size_t Utf8CodepointCount(const std::string& text) {
  size_t count = 0;
  size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
    }
    if (length > text.size() - index) {
      length = text.size() - index;
    }
    index += length;
    ++count;
  }
  return count;
}

inline std::string AtLeastTwoCodepoints(const std::string& text) {
  return Utf8CodepointCount(text) >= 2 ? text : std::string();
}

}  // namespace detail

// Expands `{title}`, `{artist}`, `{album}`, `{year}` and `{genre}` in `tmpl`.
// An unknown token survives verbatim (braces included), a lone `{` or `}` is
// literal text, and an empty tag substitutes to an empty string. With
// `url_encode` the substituted *values* are percent-encoded as UTF-8 (RFC 3986
// unreserved bytes stay raw, everything else becomes uppercase `%XX`); the
// literal template text is never encoded, so a template may carry an
// already-encoded `%20`.
inline std::string FormatTemplate(std::string_view tmpl,
                                  const TrackFields& fields, bool url_encode) {
  std::string expanded;
  expanded.reserve(tmpl.size());

  size_t index = 0;
  while (index < tmpl.size()) {
    if (tmpl[index] != '{') {
      expanded.push_back(tmpl[index]);
      ++index;
      continue;
    }

    const size_t end = tmpl.find('}', index + 1);
    if (end == std::string_view::npos) {
      // A lone opening brace is literal text.
      expanded.push_back('{');
      ++index;
      continue;
    }

    const std::string_view token = tmpl.substr(index + 1, end - index - 1);
    const std::string* value = detail::FieldForToken(fields, token);
    if (value == nullptr) {
      // Unknown tokens survive verbatim, braces included.
      expanded.append(tmpl.data() + index, end - index + 1);
    } else if (url_encode) {
      expanded += detail::PercentEncode(*value);
    } else {
      expanded += *value;
    }
    index = end + 1;
  }

  return expanded;
}

// AIMP stores a multi-value artist tag comma-joined without a space
// ("Billie Eilish,Khalid"). This normalises each comma to ", " without
// reordering, splitting or de-duplicating the values.
inline std::string NormalizeArtistSeparators(const std::string& artist) {
  std::string normalized;
  normalized.reserve(artist.size() + 8);

  for (size_t index = 0; index < artist.size(); ++index) {
    if (artist[index] != ',') {
      normalized.push_back(artist[index]);
      continue;
    }

    if (index + 1 == artist.size()) {
      normalized.push_back(',');
      continue;
    }

    normalized += ", ";
    while (index + 1 < artist.size() &&
           (artist[index + 1] == ' ' || artist[index + 1] == '\t')) {
      ++index;
    }
  }

  return normalized;
}

struct TextFields {
  std::string details;     // expanded Details template, else artist, else AIMP
  std::string state;       // expanded State template, else album; empty omits
  std::string large_text;  // expanded LargeText template; empty omits
};

// The small badge and its text for the current player state. A configured text
// shorter than Discord's 2-character minimum is cleared, so the field is
// omitted instead of sending an invalid value.
struct SmallBadgeSettings {
  bool use_play = false;
  std::string play_image;
  std::string play_text;
  bool use_pause = false;
  std::string pause_image;
  std::string pause_text;
  bool use_radio = false;
  std::string radio_image;
  std::string radio_text;
};

struct SmallBadge {
  std::string image;
  std::string text;
};

// `state` is AIMP's player state (1 = paused, 2 = playing); any other value
// yields no badge. A URL stream prefers the radio badge, exactly like the
// existing image switch.
inline SmallBadge SelectSmallBadge(bool is_url, int state,
                                   const SmallBadgeSettings& settings) {
  SmallBadge badge;

  if (state == 1) {  // Paused
    if (settings.use_pause && !settings.pause_image.empty()) {
      badge.image = settings.pause_image;
      badge.text = detail::AtLeastTwoCodepoints(settings.pause_text);
    }
  } else if (state == 2) {  // Playing
    if (is_url && settings.use_radio && !settings.radio_image.empty()) {
      badge.image = settings.radio_image;
      badge.text = detail::AtLeastTwoCodepoints(settings.radio_text);
    } else if (settings.use_play && !settings.play_image.empty()) {
      badge.image = settings.play_image;
      badge.text = detail::AtLeastTwoCodepoints(settings.play_text);
    }
  }

  return badge;
}

// Expands the three text templates against `track`, then applies the layout
// rules: `details` is never empty (template, then artist, then
// kFallbackDetails); `state` falls back to the album when the template expands
// empty, and is omitted when it would repeat `details`; `large_text` is omitted
// when it is empty or shorter than two codepoints. Truncation stays downstream
// (NormalizeTextField, 128 codepoints), so this builder does no cutting itself.
inline TextFields BuildTextFields(const TrackFields& track,
                                  const std::string& details_template,
                                  const std::string& state_template,
                                  const std::string& large_text_template) {
  TextFields fields;

  fields.details = FormatTemplate(details_template, track, false);
  if (fields.details.empty()) {
    fields.details = track.artist;
  }
  if (fields.details.empty()) {
    fields.details = kFallbackDetails;
  }

  fields.state = FormatTemplate(state_template, track, false);
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state = track.album;
  }
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state.clear();
  }

  fields.large_text = FormatTemplate(large_text_template, track, false);
  if (detail::Utf8CodepointCount(fields.large_text) < 2) {
    fields.large_text.clear();
  }

  return fields;
}

// Whether the keyless online chain should be asked for the current track: both
// artwork settings are on and there is an artist to search with. The album tag
// is deliberately not part of this decision - an album-less track has a
// file-path identity, so an empty album must not suppress the lookup.
inline bool ShouldRequestOnlineArtwork(bool use_albumart, bool use_online,
                                       const std::string& artist,
                                       const std::string& album) {
  (void)album;
  return use_albumart && use_online && !artist.empty();
}

// The large_image for `album_key` from the resolved sources: a published local
// cover wins over the online chain's URL; when neither resolved the field is
// empty (omitted on the wire). Each source is adopted only when its own key
// equals `album_key`, so a value resolved for one track identity can never be
// shown for another. `album_key` is the only identity this needs, so an
// album-less track resolves exactly like an albumed one. `source` names the
// winner for the DebugLog when non-null.
inline std::string ResolveLargeImage(const std::string& album_key,
                                     const std::string& local_cover_key,
                                     const std::string& local_cover_url,
                                     const std::string& online_key,
                                     const std::string& online_url,
                                     std::string* source = nullptr) {
  if (!local_cover_url.empty() && local_cover_key == album_key) {
    if (source != nullptr) {
      *source = "local";
    }
    return local_cover_url;
  }
  if (!online_url.empty() && online_key == album_key) {
    if (source != nullptr) {
      *source = "online";
    }
    return online_url;
  }
  if (source != nullptr) {
    *source = "none";
  }
  return std::string();
}

// The cover chain as four explicit layers, in priority order. This is the
// single statement of the required fallback order; the resolver above is its
// URL-producing form. A track descends the chain only by surviving every layer
// above it - a host failure inside kLocalEmbedded/kLocalSidecar is still that
// same layer and must not skip kOnlineKeyless.
enum class CoverLayer {
  // 1. The track's own embedded art, published to a host.
  kLocalEmbedded = 1,
  // 2. A sidecar image in the track folder (or up to 4 ancestors), published.
  kLocalSidecar = 2,
  // 3. The keyless online chain (Deezer / iTunes / MusicBrainz+CoverArtArchive).
  kOnlineKeyless = 3,
  // 4. No image. Only reached when 1, 2 and 3 all fail; the field is omitted
  // and Discord renders its native placeholder.
  kNoImage = 4,
};

inline const char* CoverLayerName(CoverLayer layer) {
  switch (layer) {
    case CoverLayer::kLocalEmbedded:
      return "local-embedded";
    case CoverLayer::kLocalSidecar:
      return "local-sidecar";
    case CoverLayer::kOnlineKeyless:
      return "online-keyless";
    case CoverLayer::kNoImage:
      return "no-image";
  }
  return "no-image";
}

// The layer that actually supplies the image, given what each layer produced.
// `embedded_url` and `sidecar_url` are published local URLs (empty means that
// layer did not deliver - extraction failed, or the upload did not prove
// retrievable); `online_url` is the keyless chain's URL (empty means it did not
// resolve). Every URL is already key-guarded by the caller, so this function is
// pure ordering. No image is returned only when all three URLs are empty.
inline CoverLayer ResolveCoverLayer(const std::string& embedded_url,
                                    const std::string& sidecar_url,
                                    const std::string& online_url) {
  if (!embedded_url.empty()) {
    return CoverLayer::kLocalEmbedded;
  }
  if (!sidecar_url.empty()) {
    return CoverLayer::kLocalSidecar;
  }
  if (!online_url.empty()) {
    return CoverLayer::kOnlineKeyless;
  }
  return CoverLayer::kNoImage;
}

// Whether the online rung must be asked for this track given the local layers'
// state. The online rung is requested whenever the artist is known and online
// is enabled - including when local art was found, because a local extract can
// still fail to publish and must then fall through to online. If online also
// resolves nothing, no image is sent.
// `local_art_found` and `local_published` therefore both keep the request live:
// they never suppress it. Only the settings and an empty artist do.
inline bool ShouldRequestOnlineForTrack(bool use_albumart, bool use_online,
                                        const std::string& artist,
                                        bool local_art_found,
                                        bool local_published) {
  (void)local_art_found;
  (void)local_published;
  return ShouldRequestOnlineArtwork(use_albumart, use_online, artist, std::string());
}

}  // namespace PresenceLayout

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_
