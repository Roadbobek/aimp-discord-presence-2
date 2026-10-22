# AIMP Discord Presence

![AIMP Discord Presence](.github/aimp-discord-312.png)

AIMP Discord Presence shows what you are listening to in [AIMP](https://www.aimp.ru) as a
Discord Rich Presence, the way Spotify does: playing, pausing or switching tracks updates
Discord with the artist, the song and the album artwork, and your profile shows *Listening to*
AIMP. Playback time is optional, the badges are configurable, and it reconnects itself.

## Requirements

- **Windows x64.**
- **AIMP** — desktop build, 64-bit (built against the AIMP SDK vendored in `lib/aimp-sdk`).
- **The Discord desktop app, running.** The browser version will not work; the plugin never
  launches Discord for you.
- Discord ▸ **Settings ▸ Activity Privacy ▸ "Share your detected activities with others"** must be **on** — nothing appears without it.

## Install

1. Get `aimp_discordPresence.dll` — from a release, or build it yourself.
2. Put it in AIMP's plugin folder — `<AIMP install dir>\Plugins\aimp_discordPresence\` in a
   standard install (confirm on yours), or use **Settings ▸ Plugins ▸ Install** on the `.dll`.
3. Confirm the plugin is **enabled** in **Settings ▸ Plugins**.
4. **Restart AIMP**, then play a track; the presence appears on your Discord profile.

## Minimal configuration

Configuration is ini-only — there is no settings window. Settings live in AIMP's config file,
`Profile/AIMP.ini` (the default location of a standard AIMP install — confirm on your own), in
the `[DiscordPresence]` section. AIMP must be **closed** while you edit them.

```ini
[DiscordPresence]
; Album art as the large image, and your own (local) cover published so Discord
; can fetch it. Both default to 1.
UseAlbumArt=1
LocalCover=1
; Keyless online artwork lookup. Default 1.
UseAlbumArtOnline=1
; Optional permanent GitHub cover host; empty token means the keyless hosts
; (catbox, uguu) upload instead. Default: empty.
CoverToken=
CoverRepo=your-username/your-covers-repo
```

`CoverToken` is **optional**: empty means the GitHub host is skipped and the keyless hosts carry the upload. `CoverHost` selects which host publishes covers (`auto` by default) — see [docs/GUIDE.md](docs/GUIDE.md), section 6.

## Troubleshooting

- **Nothing appears.** Check the Discord toggle above, that the desktop client is running, and that AIMP was restarted after enabling the plugin.
- **Config changes do nothing.** AIMP must be closed while you edit its config file (`Profile/AIMP.ini` by default); it writes the file on exit and ignores edits made while it runs.
- **No cover art on some tracks.** Every layer failed for that track. The online lookup
  needs `UseAlbumArt=1` and `UseAlbumArtOnline=1`; set `DebugLog=discord.log` and read the
  `local-cover` lines to see which layer and host were tried.
- **`CoverToken` rejected.** An expired or under-scoped token only loses the GitHub host — the keyless hosts still publish. Clear the token to silence it.
- **Updates feel lagged.** `ThrottleMs` (default `4000`) is the floor between updates, kept under Discord's five-per-20-seconds cap; the latest state follows when it expires.

## Full setup

Building from source, every configuration key, your own cover repository and token, privacy,
and a troubleshooting table: **[docs/GUIDE.md](docs/GUIDE.md)**.

## Credits and licence

Fork of **Exle**'s [aimp-discord-presence](https://github.com/Exle/aimp-discord-presence);
this fork adds the multi-layer cover-art chain and the presence card. Upstream has been
quiet since June 2024, and this repository continues development — it is not the official
upstream, and nothing here suggests Exle endorses it. This plugin is for personal use and
is free to use by anyone. There is no licence file in this repository; the statement above
is the whole of it. Credit for the original work belongs to Exle.
