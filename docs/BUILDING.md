# Building, installing and configuring

This is the long-form companion to the [README](../README.md). It covers what
you need, how to install a release or build from source, every configuration
key, and what to try when the presence does not appear.

## Requirements
- **Windows** (64-bit).
- The **Discord desktop client**, running. The plugin talks to Discord locally; it
  does not work with the browser-only version.
- For building from source only: **Visual Studio 2022 or newer build tools** and
  **CMake >= 3.28**.

## Installation (prebuilt)

1. **Get the plugin.** Download a built release archive from the
   [Releases page](https://github.com/nathwn12/aimp-discord-presence2/releases)
   of this fork, or build it yourself (see [Build from source](#build-from-source)
   below).
2. **Open the plugin installer in AIMP.** In AIMP, open **Settings** (the gear
   icon), or **right-click the title bar ▸ Settings**. Go to the **Plugins**
   section and click **Install**.
3. **Select the archive or the `.dll`.** Point AIMP at the release archive, or at
   `aimp_discordPresence.dll` if you built it yourself.
4. **Let AIMP extract it.** AIMP installs the plugin into its own per-plugin
   folder, `AIMP\Plugins\aimp_discordPresence\`.
5. **Enable it and restart.** Make sure the plugin is enabled in the Plugins list,
   then restart AIMP.
6. **Enable activity sharing in Discord.** Open Discord, go to **Settings ▸
   Activity Privacy**, and enable **"Share your detected activities with
   others"**. **Presence will not appear without this.**
7. **Play something.** Start playback in AIMP; the presence should appear on your
   Discord profile.

## Build from source

The repository builds with CMake. Its one external dependency, the **AIMP SDK**,
is a git submodule (`lib/aimp-sdk`) and must be checked out too. There is no
Discord RPC library to fetch: Discord integration is implemented in-repo, with
`src/discord_ipc.*` speaking Discord's local IPC protocol over the
`\\.\pipe\discord-ipc-N` named pipe directly. The AIMP SDK glue is likewise
in-repo, under `lib/aimp-glue/` (see the fork note in the [README](../README.md)).

**Prerequisites:** Windows, Visual Studio 2022 or newer build tools, CMake >= 3.28.

```powershell
# Clone this fork together with its AIMP SDK submodule.
git clone --recursive https://github.com/nathwn12/aimp-discord-presence2.git
cd aimp-discord-presence2

# If you cloned without --recursive, initialize the submodule instead:
# git submodule update --init --recursive

# Configure (64-bit, version stamping on).
cmake -B build -A x64 -DAUTOVERSIONING=ON

# Build the Release configuration.
cmake --build build --config Release
```

The built plugin lands at:

```
output/Release/aimp_discordPresence/x64/aimp_discordPresence.dll
```

Install that DLL using the manual steps above (AIMP ▸ Settings ▸ Plugins ▸
Install).

### Tests

The offline payload tests build as a separate `payload_tests` target and are
registered with CTest. They need no network, AIMP, or Discord client:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Configuration

Configuration is **ini-only** - there is no settings UI. Settings live in AIMP's
config file at `Profile/AIMP.ini`, in the `[DiscordPresence]` section. (You can
find the Profile folder location in AIMP's settings.)

> **AIMP must be closed while you edit `Profile/AIMP.ini`.** Changes are written
> by AIMP on exit and are not picked up while it is running.

### Keys

| Key | Type | Description |
| --- | --- | --- |
| `ApplicationID` | integer | Discord application ID used to publish the presence. The default is the shared upstream ID. You may substitute your own ID (create one in the [Discord developer portal](https://discord.com/developers/applications)). |
| `Timestamp` | bool | Show playback timestamps. `0` = elapsed time, `1` = remaining time. |
| `UseAlbumArt` | bool | Use the album art as the large image. `0` = off, `1` = on. |
| `UseAlbumArtOnline` | bool | Allow **network** album-art lookups (Deezer, then iTunes, then MusicBrainz + Cover Art Archive). `0` = never go online; the card keeps the plugin's built-in placeholder image. `1` = allow online lookups. |
| `LocalCover`, `CoverCache` | bool, string | Publish the track's own (offline) cover so Discord can fetch the real artwork. `LocalCover`: `0` = off, `1` = on (default); a published cover is used only after the upload proves retrievable. The local image is uploaded through a host chain - GitHub (only when `CoverToken` is set), catbox, uguu, then litterbox - and a host failure falls through to the online lookup, never straight to the placeholder. `CoverCache`: optional file that remembers published cover URLs by image hash. Empty (the default) resolves to `cover-cache.txt` beside the plugin DLL; a bare filename resolves there too, an absolute path is used as given. |
| `CoverToken`, `CoverRepo` | string, string | **Optional** permanent GitHub upload host. `CoverToken` is a fine-grained personal access token with `contents: write` for the repository. An **empty token (the default) is fine**: it simply omits the GitHub host, and the keyless hosts handle the upload with no behaviour change. When set, GitHub is tried first, and the token is sent **only** to `api.github.com`; it is never logged, printed, or written to `CoverCache`. `CoverRepo`: the `owner/name` repository that receives the covers; empty (the default) uses `nathwn12/aimp-discord-presence-art`. |
| `StatusDisplayType` | int | What the member-list status text shows: `0` = app name, `1` = **artist** (the `state` line), `2` = **track title** (the `details` line). Default: `2`. |
| `Name` | string | Application name shown above the activity card. Default: `Music`. Truncated to 128 characters; an empty value omits the field. |
| `Details` | string | Template for line 2 of the card. Default: `{title}`. An empty expansion falls back to the artist, then to `AIMP`, so the line is never blank. |
| `State` | string | Template for line 3 of the card. Default: `{artist}`. An empty expansion falls back to the album; the line is omitted when it would repeat `Details`. |
| `LargeText` | string | Template for the cover-art tooltip. Default: `{album}`. Omitted when it expands to fewer than two characters. |
| `Button1.Label`, `Button1.Url` | string, string | First link button. Defaults: `Listen on Spotify` and `https://open.spotify.com/search/`. A button is shown only when its url is an `https://` URL; the label is truncated to 32 characters (an empty label becomes `Listen`) and the url to 512. Both fields accept template tokens; **Url token values are percent-encoded, labels are not**. |
| `Button2.Label`, `Button2.Url` | string, string | Second link button. Defaults: `Find on YouTube` and `https://music.youtube.com/search?q=`. Same https-only rule, templates and limits as the first button. |
| `ThrottleMs` | int | Minimum milliseconds between presence updates. Default: `4000` (five updates per 20 seconds, Discord's cap). `0` disables the limit; any non-zero value below `1000` - negative included - is raised to `1000`; values `>= 1000` are used as-is. An update that arrives early is remembered and the **latest** state is sent when the interval expires. |
| `DebugLog` | string | Optional file that receives the Discord IPC frame log, for diagnosing connection problems. Empty (the default) disables it. A bare filename is written next to the plugin DLL; an absolute path is used as given. |
| `State.UsePlay` | bool | Show a small play badge. `0` = off, `1` = on. |
| `State.PlayImage` | string | Image used for the play badge (a bundled asset name or a URL). |
| `State.PlayText` | string | Tooltip text for the play badge. Default: `Playing`. Omitted when shorter than two characters. |
| `State.UsePause` | bool | Show a small pause badge. `0` = off, `1` = on. |
| `State.PauseImage` | string | Image used for the pause badge. |
| `State.PauseText` | string | Tooltip text for the pause badge. Default: `Paused`. Omitted when shorter than two characters. |
| `State.UseRadio` | bool | Show the radio badge for URL streams. `0` = off, `1` = on. |
| `State.RadioImage` | string | Image used for the radio badge. |
| `State.RadioText` | string | Tooltip text for the radio badge. Default: empty, which omits `small_text` in radio state. Omitted when shorter than two characters. |

### Templates and encoding

`Details`, `State`, `LargeText`, `Button1.Label`, `Button1.Url`,
`Button2.Label` and `Button2.Url` accept these tokens:

| Token | Substituted with |
| --- | --- |
| `{title}` | The track title tag. |
| `{artist}` | The artist tag. Multi-value tags are normalised to a comma followed by one space (`Billie Eilish, Khalid`). |
| `{album}` | The album tag. |
| `{year}` | The year/date tag, verbatim. |
| `{genre}` | The genre tag. |

An unknown `{token}` survives verbatim, braces included. A lone `{` or `}` is
literal text, and a tag that is empty expands to nothing.

When a button **Url** template is expanded, the substituted tag values are
percent-encoded as UTF-8: RFC 3986 unreserved bytes (`A-Z a-z 0-9 - . _ ~`) stay
raw and everything else becomes an uppercase `%XX`, so a space is `%20` (never
`+`). The literal template text is **not** encoded, so an already-encoded `%20`
in the template survives unchanged. Button **labels** are never encoded.

### Example `Profile/AIMP.ini`

```ini
[DiscordPresence]
; Discord application ID. Default is the shared upstream ID; replace with your own if you like.
ApplicationID=429559336982020107

; Show playback timestamps (0 - elapsed time / 1 - time remaining). Default: 1
Timestamp=1

; Use the album art as the large image. Default: 1
UseAlbumArt=1

; Allow network album-art lookups (Deezer, then iTunes, then MusicBrainz). 0 = always use the offline asset. Default: 1
UseAlbumArtOnline=1

; Optional permanent GitHub cover host. Leave CoverToken empty for the default
; keyless chain; the token is sent only to api.github.com and is never logged or
; cached. CoverRepo defaults to the owner's art repository.
CoverToken=
CoverRepo=nathwn12/aimp-discord-presence-art

; Member-list status text: 0 = app name / 1 = artist / 2 = track title. Default: 2
StatusDisplayType=2

; Card text templates. Tokens: {title} {artist} {album} {year} {genre}
Details={title}
State={artist}
LargeText={album}

; Link buttons. Url token values are percent-encoded; labels are not.
Button1.Label=Listen on Spotify
Button1.Url=https://open.spotify.com/search/{title}
Button2.Label=Find on YouTube
Button2.Url=https://music.youtube.com/search?q={title}

; Minimum milliseconds between updates; 0 disables the limit. Default: 4000
ThrottleMs=4000

; Small status badges.
State.UsePlay=0          ; Show the play badge. Default: 0
State.PlayImage=aimp_play ; Image for the play badge
State.PlayText=Playing   ; Tooltip for the play badge
State.UsePause=0         ; Show the pause badge. Default: 0
State.PauseImage=aimp_pause ; Image for the pause badge
State.PauseText=Paused   ; Tooltip for the pause badge
State.UseRadio=1         ; Show the radio badge for URL streams. Default: 1
State.RadioImage=aimp_radio ; Image for the radio badge
State.RadioText=         ; Tooltip for the radio badge; empty omits it
```

## Troubleshooting

**Presence does not appear.**
Check Discord first: **Discord ▸ Settings ▸ Activity Privacy ▸ "Share your
detected activities with others"** must be enabled. This is the most common cause.
Also make sure the **Discord desktop client** is running, and that the plugin is
enabled and AIMP has been restarted.

**Album art is missing.**
The large image is filled from online lookups only when both `UseAlbumArt=1` and
`UseAlbumArtOnline=1`. If `UseAlbumArtOnline=0`, or the track cannot be matched by
the keyless providers (Deezer `/search/album`, iTunes Search, MusicBrainz + Cover
Art Archive), the card shows the plugin's built-in placeholder image instead of a
real cover.

**My configuration changes do nothing.**
AIMP must be **closed** while you edit `Profile/AIMP.ini`. Reopen AIMP after
saving.

**I changed `ApplicationID` and now nothing shows.**
The replacement ID must be a valid Discord application with Rich Presence
enabled in the [Discord developer portal](https://discord.com/developers/applications).

## Limitations

- **Windows-only.** The plugin is built for the Windows AIMP desktop app.
- **Ini-only configuration.** There is no settings UI, and AIMP must be closed to
  edit the config.
- **Requires the Discord desktop client.** A browser-only Discord will not work,
  and the plugin never launches Discord for you.
- **Shared application ID.** The default `ApplicationID` is the upstream one, so
  everyone using the default appears under the same Discord application.
- **Not in the AIMP catalogue.** This plugin is not listed on AIMP's website.
- **Reimplemented glue layer.** Exle's original `aimp-sdk-cpp-wrapper` is gone from
  GitHub, so the AIMP SDK is reached through this fork's own `lib/aimp-glue/`
  façade - a reimplementation of that vanished wrapper, not the original code.
