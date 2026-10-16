<p align="center">
  <img src="assets/icon.svg" width="120" height="120" alt="aimp-discord-presence2 icon" />
</p>
<h1 align="center">aimp-discord-presence2</h1>
<p align="center"><strong>What you are listening to in AIMP, live on your Discord profile.</strong></p>
<p align="center">
  <img src="https://img.shields.io/github/languages/top/nathwn12/aimp-discord-presence2" alt="Language" />
  <img src="https://img.shields.io/badge/platform-windows--64bit-2b3137?logo=windows&logoColor=white" alt="Platform" />
</p>

## Project name and description

aimp-discord-presence2 is an AIMP plugin that shows your current track in Discord as a Rich Presence — artist, song, album artwork, and optional elapsed time — the way Spotify does, appearing as *Listening to* AIMP. It is for AIMP users on Windows who run the Discord desktop client and want their playback visible without a settings window to click through. Configuration is a plain ini file, and cover art resolves through four local-and-online layers with a documented upload-host chain behind them.

## Technology stack

C++17 plugin built with CMake 3.28 or newer and Visual Studio 2022 (or newer) build tools, 64-bit Windows only. The AIMP SDK arrives as a git submodule (`lib/aimp-sdk`); the glue over it (`lib/aimp-glue/`) is written in-repo. There is no Discord RPC library to fetch — Discord integration is implemented in-repo, with `src/discord_ipc.*` speaking Discord's local IPC protocol over the `\\.\pipe\discord-ipc-N` named pipe directly.

## Architecture

The build produces one shared library, `aimp_discordPresence.dll`, from `src/*.cpp` plus the in-repo `aimp-glue` façade over the official AIMP SDK headers. Cover art resolves through four layers, stopping at the first usable image: art embedded in the file, a sidecar image in the track folder (up to four folders up), a keyless online lookup, and a plain black placeholder. The two local layers publish through an upload-host chain — an optional repo you own (used only when `CoverToken` is set), then catbox, uguu, and litterbox — so Discord can fetch the image; a host failure falls through to the online lookup, never straight to black. Presence is sent only when something changes, updates coalesce inside Discord's rate limit, and the plugin reconnects if Discord restarts without ever launching Discord itself.

## Getting started

Prerequisites: 64-bit Windows, the Discord desktop client running (the browser version will not work), and AIMP.

1. Download a built release archive from the [Releases page](https://github.com/nathwn12/aimp-discord-presence2/releases), or build it yourself (see below).
2. In AIMP open Settings, go to Plugins, click Install, and point it at the release archive (or your built `aimp_discordPresence.dll`).
3. Enable the plugin in the Plugins list, then restart AIMP.
4. In Discord, under Settings → Activity Privacy, turn on "Share your detected activities with others" — nothing shows without this.
5. Play something in AIMP; the presence appears on your profile.

Build from source (Windows, VS2022+ build tools, CMake ≥ 3.28):

```powershell
git clone --recursive https://github.com/nathwn12/aimp-discord-presence2.git
cd aimp-discord-presence2
cmake -B build -A x64 -DAUTOVERSIONING=ON
cmake --build build --config Release
```

Every configuration key is documented in [docs/BUILDING.md](docs/BUILDING.md).

## Project structure

- `src/` — plugin sources: presence core, four-layer art resolver, upload chain, Discord IPC, `version.rc` (copyright and version stamping).
- `lib/aimp-glue/` — in-repo `Aimp::` C++ façade over the AIMP SDK headers; replaces the retired upstream wrapper.
- `lib/aimp-sdk/` — official AIMP SDK, git submodule.
- `tests/` — cover-publisher suite (`cover_publisher_tests.cpp`).
- `scripts/` — `build-release.ps1` release-packaging helper.
- `docs/` — `BUILDING.md` (install, build, every config key) and `preview/` (cards rendered from real captured payloads).
- `.github/` — CMake build workflow (Win32 + x64, push/PR on `main`).
- `build/` / `output/` — CMake tree and built plugin (both gitignored).
- `cmakelists.txt` / `src/cmakelists.txt` / `src/product.version` — build roots and version stamp.
- `.gitmodules` / `.gitignore` — submodule pointer and build-output ignores.
- `README.md` — this file.

## Key features

- Discord Rich Presence while AIMP plays: artist, song, album, and artwork.
- The *Listening* activity type, so Discord says "Listening to AIMP".
- Optional elapsed or remaining time, counted down client-side by Discord.
- Small play, pause, and radio badges, each configurable.
- Four-layer artwork resolution: embedded art, sidecar files, keyless online lookup, black placeholder.
- Self-hosted cover publishing chain (own repo, then catbox, uguu, litterbox) for the local layers.
- Plain ini-file configuration — no settings window; every key in `docs/BUILDING.md`.
- No repeated text across presence lines, even for self-titled tracks.
- Reconnects when Discord restarts; never launches Discord for you.

## Development workflow

Work happens on `main`. Cut a release from the [Releases page](https://github.com/nathwn12/aimp-discord-presence2/releases) — that archive is what users install. CI builds Win32 and x64 with CMake on every push and pull request to `main`. The AIMP SDK submodule must be checked out (`--recursive`, or `git submodule update --init --recursive`) before configuring.

## Coding standards

C++17, compiled warning-clean under `/W4 /WX` (warnings are errors — set in the root `cmakelists.txt`). Cover files found on disk are read, never decoded or rewritten.

## License

No `LICENSE` file ships in this repo or upstream — original work © 2024 Exle, fork additions © 2026 nathwn12; free for personal use by anyone.

## Credits

The original plugin is by [Exle](https://github.com/Exle/aimp-discord-presence); upstream has been quiet since June 2024 and this fork is where development continues. It is not the official upstream and nothing here suggests Exle endorses it. The original design, the wrapper design, and the shared Discord application ID are Exle's — including the `src/version.rc` copyright line kept in this tree. One upstream piece is gone (the deleted `aimp-sdk-cpp-wrapper`), so this fork ships its own glue under `lib/aimp-glue/`; only the implementation was replaced.

<!-- nathwn12 house footer: identical in every repo. Do not edit. -->
<p align="center">
  <a href="https://github.com/nathwn12"><img src="assets/signature.svg" width="96" alt="nathwn12" /></a>
</p>
<p align="center">
  <sub>Built by <a href="https://github.com/nathwn12">nathwn12</a> · <a href="https://github.com/nathwn12?tab=repositories">All projects</a></sub>
</p>
