# Dragon Ball Z: Tenkaichi Tag Team — PC & browser port

If you enjoy this port and feel like it, you can [buy me a coffee](https://buymeacoffee.com/ewtjacquier). Thank you!

A fan-made port of **Dragon Ball Z: Tenkaichi Tag Team** (PSP, 2010) that runs natively on Linux
and **in the browser**, at high resolution, with HD textures, accounts for each player and
**battles with up to four friends online** (the game's Multiplayer mode). It is not an emulator:
the game's own program was translated, function by function, into C code that compiles for your
PC or for WebAssembly.

> **Bring your own game.** This repository has no part of the game: no ISO, no game code, no game
> files. You build the port from your own copy, dumped from your own UMD. Not affiliated with
> Bandai Namco Entertainment, Spike or anyone who owns Dragon Ball.

Made by fans, for fans: to keep a game we love alive, playable on today's machines and in a
browser tab, and to fight with our friends again.

## Contents

1. [What you get](#what-you-get)
2. [How it works](#how-it-works)
3. [Technologies](#technologies)
4. [What you need](#what-you-need)
5. [Step by step](#step-by-step)
   1. [Get the code](#1-get-the-code)
   2. [Get the decrypted EBOOT](#2-get-the-decrypted-eboot)
   3. [Take out the code overlays](#3-take-out-the-code-overlays)
   4. [Translate the game to C](#4-translate-the-game-to-c)
   5. [Get FFmpeg](#5-get-ffmpeg)
   6. [Build and play on Linux](#6-build-and-play-on-linux)
   7. [Build and play in the browser](#7-build-and-play-in-the-browser)
   8. [HD textures](#8-hd-textures)
6. [Online play: Ad Hoc only, no game server](#online-play-ad-hoc-only-no-game-server)
7. [Sharing it with friends (Cloudflare Tunnel)](#sharing-it-with-friends-cloudflare-tunnel)
8. [Accounts and saves: where everything is kept](#accounts-and-saves-where-everything-is-kept)
9. [Things to know](#things-to-know)
10. [Repository layout](#repository-layout)
11. [Credits](#credits)

## What you get

- **The whole game**, translated to native code: the program and its 11 code overlays, every
  mode, fighter, map, music and voice.
- **Up to 4x internal resolution** (1920x1088), natively and in the browser.
- **HD textures**: the interface at 4x (the character select's arts and faces, the menus, the map
  select, the loading screens, the battle's HUD, the result screens and the game's font) and some
  of the fighters' textures. The character select's arts and the art where a plain upscale fell
  short were redrawn with OpenAI's image models; the words (menu titles, counters, WIN / LOSE, the
  buttons) and the game's font drawn again with a font, in the original's colors, places and
  spacing; the rest upscaled with the [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) anime
  model. Each one was checked against its original. It comes as a separate download and never
  changes the game's files: turn it off and on in the settings menu.
- **A browser version**: the game in a tab, with a settings menu (Esc): fullscreen, volume,
  keyboard keys, gamepad buttons and which stick is the PSP's analog stick, internal resolution, HD
  textures, how the original textures are filtered.
- **Players**: each player logs in and keeps their own save; their name is their name in the game
  and online.
- **Online battles for up to 4 players** through the game's **Multiplayer** (Ad Hoc) mode: one
  player creates a room, the others join. There is no game server: see
  [Online play](#online-play-ad-hoc-only-no-game-server).
- **Mods** (C code in `port/mods/`, each can be turned off):
  - straight to the title: no logos, no opening (the title in about a second), and its bottom line
    of small print hidden; START loads the save (or starts a new one);
  - a shorter main menu: **Free Battle** and **Multiplayer**; the Option menu keeps Volume,
    Controller and Camera (the port handles saving and installing);
  - every character and every map unlocked from the start; the battle camera always Type C (the
    farthest);
  - the character select goes from the color straight to the battle (the Custom step, Default /
    Customize 1-3, passed by with Default);
  - Multiplayer: Create New Room makes a Free Battle room at once, the PSP-1000 clock question is
    never asked, and the battles get a small input delay that keeps them smooth over the internet;
  - the analog stick moves through every menu, as the d-pad does;
  - the player's name is the account's name.
- **Fixed along the way** (the port behaves as the PSP, not as a naive translation): the stage's
  "double source alpha" blending, the PSP's stack checks, the overlays' code that only the running
  game reveals, and the long chains of tail calls that overflowed the browser's stack.

## How it works

The PSP runs MIPS machine code. Emulators interpret it (or compile it on the fly) while the game
runs. This port does it once, ahead of time: **static recompilation**.

```
your UMD ─▶ ISO ─▶ EBOOT.BIN (encrypted) ─▶ EBOOT_decrypted.elf
           └─▶ PACKFILE.BIN ─▶ re/overlays/ovN.bin (the 11 code overlays)
                                              │
           re/export/program.json ────────────┤  where each function and jump table is
           re/export/overlays/ovN.json        │  (addresses only: no game code)
           (made once with Ghidra)            │
                                              ▼
                            tools/prx_load.py + tools/recomp.py
                                              │
                                              ▼
                  port/generated/*.c  ── the game's functions as C code (stays on your PC)
                                              │
              port/runtime/*.c  ──────────────┤  the PSP's system, rewritten: threads, files,
              port/mods/*.c                   │  graphics, sound, movies, saves, network
                                              ▼
                        native binary (GCC)  or  WebAssembly (Emscripten)
```

- Each MIPS function becomes a C function; registers live in a struct, the PSP's memory is one
  block of memory, branches become `goto`s. The VFPU (the PSP's vector unit) is translated inline.
- Much of the game's code is not in the EBOOT: it comes in 11 overlays inside the game's archive,
  loaded one at a time at the same address. Each is translated apart, and the runtime calls the
  functions of whichever overlay is loaded there.
- The PSP system functions the game calls are implemented by this project's runtime (nothing comes
  from an emulator): graphics go to OpenGL ES 3 / WebGL 2, sound to SDL or WebAudio, the music and
  voices through FFmpeg, the game's files are read straight from your ISO.
- Mods replace game functions by address (`port/hooks.txt`) with hand-written C that can still
  call the original.

`docs/NOTES.md` has the technical notes: the disc and its archive, the overlays, the online
battles' lockstep protocol, the renderer, every mod.

## Technologies

| What | Used for |
|---|---|
| **C** (GNU C11), **GCC**, **Make** | the generated game code, the runtime, the mods |
| **Python 3** | the recompiler, the build tools, the local web server (standard library only) |
| **SDL2** | window, input and sound in the native build |
| **OpenGL ES 3 / WebGL 2** | the PSP's graphics chip, rendered at up to 4x |
| **Emscripten** (WebAssembly, Asyncify, SIMD) | the browser build; the PSP's threads become fibers |
| **FFmpeg** (PPSSPP's builds) | ATRAC3+ music and voices |
| **LiveKit** (WebRTC) | online play: the game's Ad Hoc messages between players |
| **Ghidra** (optional) | the analysis that found every function (already done: `re/export/`) |
| **Real-ESRGAN**, **OpenAI image models**, fonts | how the HD textures were made (`realesrgan-x4plus-anime` at 4x; `gpt-image` redraws; words and letters drawn with the Ubuntu font) |

## What you need

**The game**: *Dragon Ball Z: Tenkaichi Tag Team* (USA), **ULUS-10537**, as an ISO dumped from
your own UMD (with a PSP running custom firmware, for example).

**To build** (Linux; on Windows use WSL2 with Ubuntu):

```bash
sudo apt install build-essential pkg-config git python3 libsdl2-dev libgles-dev
```

- For the browser build, the **Emscripten SDK** (tested with 3.1.46):
  ```bash
  git clone https://github.com/emscripten-core/emsdk ~/emsdk
  cd ~/emsdk && ./emsdk install 3.1.46 && ./emsdk activate 3.1.46
  export EMSDK=~/emsdk          # the build scripts use it (or: source ~/emsdk/emsdk_env.sh)
  ```
- About **8 GB of RAM** and **2 GB of disk** for building (plus ~200 MB for the HD textures).

**To play**: Linux for the native build; for the browser build, any desktop browser with WebGL 2
(Chrome, Edge, Firefox) on any system. A gamepad is recommended.

## Step by step

### 1. Get the code

```bash
git clone https://github.com/<you>/tenkaichi-port.git
```

Put your ISO **inside** the `tenkaichi-port` folder (git ignores it), named
`Dragon Ball Z - Tenkaichi Tag Team (USA) (En,Fr,Es).iso` (or pass its path to the commands below):

```
tenkaichi-port/
├── Dragon Ball Z - Tenkaichi Tag Team (USA) (En,Fr,Es).iso
├── EBOOT_decrypted.elf      ← step 2
├── port/
└── re/
    └── overlays/            ← step 3
```

### 2. Get the decrypted EBOOT

The game's program is `PSP_GAME/SYSDIR/EBOOT.BIN` inside the ISO, and it is encrypted. Ways to
decrypt it:

- **With [pspdecrypt](https://github.com/John-K/pspdecrypt)**: take `EBOOT.BIN` out of the ISO
  (any ISO tool, e.g. `7z x "Dragon Ball Z - Tenkaichi Tag Team (USA) (En,Fr,Es).iso" PSP_GAME/SYSDIR/EBOOT.BIN`),
  then `pspdecrypt EBOOT.BIN` writes the decrypted file.
- **With [PPSSPP](https://www.ppsspp.org)**: in *Settings → Tools → Developer tools*, turn on the
  option that dumps the decrypted EBOOT when a game boots, then start the game once; the file
  lands in PPSSPP's memory stick folder, under `PSP/SYSTEM/DUMP/`.
- **With `re/tools/prxdec`**: PPSSPP's own decrypter built into a small command line tool
  (`PPSSPP=path/to/ppsspp-source re/tools/prxdec/build.sh`, then
  `re/tools/prxdec/prxdec EBOOT.BIN EBOOT_decrypted.elf`).

Save it as `EBOOT_decrypted.elf` in the `tenkaichi-port` folder (git ignores it). Its SHA-1 must be
`eb7bffa629a689df3a35b19f14b508b005642735` (step 4 checks it).

### 3. Take out the code overlays

```bash
cd tenkaichi-port
python3 -I re/tools/overlays.py "Dragon Ball Z - Tenkaichi Tag Team (USA) (En,Fr,Es).iso" re/overlays
```

It reads the game's archive (`PSP_GAME/USRDIR/PACKFILE.BIN`) from your ISO and writes the 11 code
overlays into `re/overlays/` (git ignores them: they are the game's code).

### 4. Translate the game to C

```bash
cd port
tools/generate.sh            # or: tools/generate.sh path/to/EBOOT_decrypted.elf
```

This is the "decompiling" part, done for you: it loads the program the way the PSP's kernel
would (`tools/prx_load.py`) and translates every function of the program and of each overlay to C
(`tools/recomp.py`), using the maps in `re/export/`. It takes a couple of minutes and writes
`port/generated/`. **That folder is the game's code: it is yours to build, never commit or share
it.**

Redoing the analysis yourself is optional: it needs Ghidra, see `re/README.md`.

### 5. Get FFmpeg

```bash
tools/get_ffmpeg.sh
```

It downloads [PPSSPP's FFmpeg builds](https://github.com/hrydgard/ppsspp-ffmpeg) (only the
decoding libraries), uses the prebuilt ones for Linux and builds the WebAssembly ones with
Emscripten (a few minutes, once).

### 6. Build and play on Linux

```bash
make -j8
./build/dbztag               # or: ./build/dbztag path/to/the.iso
```

The first build takes a while (`-j8` is fine with 16 GB of RAM, use fewer jobs with less). Keys:
**arrows** = the analog stick (moving, and the menus), **I J K L** = the d-pad, **Z** = ✕,
**X** = ◯, **A** = □, **S** = △, **Q**/**W** = L/R, **Enter** = Start, **Backspace** = Select; USB
gamepads work too. Saves go to `build/ms0/PSP/SAVEDATA`.

### 7. Build and play in the browser

```bash
tools/build_web.sh                 # writes build/web/
python3 tools/serve_web.py         # http://127.0.0.1:8645/
```

Open **http://127.0.0.1:8645/**. The server gives the page the ISO (only the parts the game
reads), keeps the saves in the project folder and handles the players' accounts.

- **Log in** or create a player (a new one starts from a copy of the main save), or "Play without
  an account" (the main save). Each browser *window* is one player: two windows, two players. The
  sign-in screen shows the game's art, read from your ISO.
- **Esc** (or the gear in the corner, or Home on a gamepad) opens the settings: fullscreen (also
  F11 or a double click), the volume, internal resolution (1x to 4x), how the original textures
  are filtered, HD textures, keys, gamepad buttons and which stick is the PSP's analog stick.
- Opened from another computer (a friend, through a tunnel), the page first copies the game into
  that browser: the ISO and the HD textures, about 1.5 GB once, with a progress bar, so the game
  never waits on the network after that (`?preload=0` in the address skips it).
- Without the server (the page opened some other way) the page asks for the ISO file and keeps
  the save in the browser.

### 8. HD textures

Download the pack (the `dbztag-hd-textures*.zip` file on this repository's Releases page) and
install it, from `port/`:

```bash
python3 tools/textures.py install ~/Downloads/dbztag-hd-textures*.zip
python3 tools/textures.py status
```

It has the interface, where the PSP's low resolution shows the most: the character select's arts
(the transformations too) and faces, the menus, the map select, the loading screens, the battle's
HUD, the result screens, the Multiplayer rooms and the game's font; and some of the fighters'
textures. The maps keep the game's own textures for now. Its 615 textures were remade at 4x and
compared with their originals one by one:

- 126 (the character select's 70 arts, its face sheets, menu, title and HUD art where the plain
  upscale fell short) were redrawn with OpenAI's image models (`gpt-image`), fitted back onto the
  original's place and outline; the face sheets laid back on the game's grid, cell by cell;
- 200 are words and letters drawn again with a font (the Ubuntu font), in the original's colors,
  places and spacing, since an AI upscale blurs small letters: the game's font, one picture per
  character, redrawn letter by letter at each letter's own width, and the words baked into the
  interface (titles, counters, WIN / LOSE / PERFECT!, the OK buttons);
- 173 come from Real-ESRGAN's anime model (`realesrgan-x4plus-anime`), the fighters' among them;
- 116 plain ones (bars, frames, small pieces of the HUD) are a sharp resize.

The pack only **adds** files in `build/textures`: the game looks each texture up while it runs and
draws the HD version in its place, so your ISO and the game's files never change.

**Going back to the originals** (the rollback):

- in the browser, Esc → **HD textures: Off** (instant, and back On whenever you like);
- or remove the pack: `python3 tools/textures.py uninstall`.

No copy of the original textures is needed for that: they always come from your ISO.

## Online play: Ad Hoc only, no game server

**What works**: the game's **Multiplayer** mode, the Ad Hoc one PSPs played side by side: one
player creates a room in their own game, up to three others find it and join, and they battle.

**What doesn't**: there is **no game server** anywhere. Nobody runs a server for this port, and it
doesn't need one: the players' own games talk to each other, exactly as on the PSP.

**How the players reach each other**: Ad Hoc is Wi-Fi between consoles in the same room. The port
emulates that network (`port/runtime/net.c`, `matching.c`) and, in the browser, carries its
messages through a **LiveKit room**: LiveKit is a service for real-time apps (WebRTC); here it is
only the "Wi-Fi", passing the game's small messages along, so its **free plan** is plenty.

The game's battles run in lockstep: every copy waits for every player's buttons of the frame
before it goes on, which on the PSP's Wi-Fi took no time. Over the internet that made them slow
down and stall, so a mod (`port/mods/netdelay.c`) plays everyone's buttons a few frames later on
every copy at once (`DBZ_NET_INPUT_DELAY`, 3 frames by default; more for players far apart).

1. Create a free account at **[cloud.livekit.io](https://cloud.livekit.io)** and a project.
2. In the project's settings, create an **API key**. Note the three values: the project's
   **URL** (`wss://<your-project>.livekit.cloud`), the **API Key** and the **API Secret**.
3. Write them in `port/.env` (the file is ignored by git: never commit it):
   ```ini
   LIVEKIT_URL=wss://your-project.livekit.cloud
   LIVEKIT_API_KEY=<your API key>
   LIVEKIT_API_SECRET=<your API secret>
   ```
4. Restart `serve_web.py`. The login screen shows **Network play: ready**.
5. In the game, everyone opens **Multiplayer**: one creates the room, the others join it.

How it fits together: the page asks *your* server for a pass (`/livekit-token`), and your server
signs it with the secret, which never leaves your PC. Everyone who opens the game from the same
server meets in the same room (`?DBZ_ROOM=name` in the address makes a separate one). To try it
alone, open two browser windows and log in as two players.

## Sharing it with friends (Cloudflare Tunnel)

To play with friends who are not on your network, one person (**the host of the page**) runs the
server and shares it through a **[Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/)**:
a secure HTTPS address for your local server, with no router setup.

What happens then: your friends open the address; their browser loads the game from your server
and runs it **on their own computer**; their saves and accounts live on yours; online battles go
through LiveKit with passes your server signs (they need no LiveKit account).

**Quick test** (a temporary random address):

```bash
# install cloudflared: https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/
cloudflared tunnel --url http://127.0.0.1:8645
```

It prints an address like `https://something-random.trycloudflare.com`: send it to your friends.
It lasts while the command runs.

**Your own address, open only to your friends** (free, needs a domain on Cloudflare):

```bash
cloudflared tunnel login                                         # pick your domain
cloudflared tunnel create dbztag                                 # prints the tunnel's id
cloudflared tunnel route dns dbztag dbztag.your-domain.com
```

`~/.cloudflared/config.yml`:

```yaml
tunnel: dbztag
credentials-file: /home/<you>/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: dbztag.your-domain.com
    service: http://127.0.0.1:8645
  - service: http_status:404
```

```bash
cloudflared tunnel run dbztag
```

Then, in Cloudflare's dashboard, **Zero Trust → Access → Applications → Add a self-hosted
application** for `dbztag.your-domain.com`, with a policy that allows your friends' e-mail
addresses: each one logs in with a code sent to their e-mail before the game opens.

**Please read this before sharing:**

- Whoever opens the address downloads the game's data from **your** ISO and the translated game
  code. Share it **only with friends who own the game**, never publicly: use the Cloudflare Access
  policy above for anything longer than a quick test.
- Anyone who reaches the page can create a player on your server: another reason for Access.
- The game streams from your PC: your upload speed is everyone's first loading time (about 1.5 GB
  per friend, once; after that their browser keeps it).

## Accounts and saves: where everything is kept

There is no database server: everything is plain files in your `port/build/` folder, written by
`serve_web.py` (and by the native build, for the main save).

| What | Where |
|---|---|
| Main save (native build, and "Play without an account") | `build/ms0/PSP/SAVEDATA/` |
| A player's account (name, password hash) | `build/profiles/<name>/account.json` |
| A player's save | `build/profiles/<name>/ms0/PSP/SAVEDATA/` |
| Logged-in sessions | `build/profiles/sessions.json` |
| HD textures | `build/textures/` |
| Settings of the page (keys, gamepad, switches) | the browser's `localStorage` |
| The game copied into a friend's browser | that browser's Cache Storage (site data) |
| Saves when the page runs without the server | the browser's IndexedDB |

- Passwords are never stored: only a salted **PBKDF2-SHA256** hash (200,000 rounds).
- Every time a save is written, the previous one is kept as `.bak` next to it.
- A new player starts from a copy of the main save; the game shows their account's name.
- **Back up**: copy `build/ms0` and `build/profiles`.
- **Forgotten password**: the host deletes `build/profiles/<name>/account.json`; the player then
  creates the same name again with a new password, and keeps their save.

## Things to know

- **Legal**: this repository holds only the port's own code, the analysis data (addresses and
  names) and icons drawn for the project. The game, its code and its art belong to their owners.
  Don't publish builds, generated code, overlays, ISOs or EBOOTs. The HD texture pack is derived
  from the game's art and meant for people who own the game.
- **Native builds**: Linux only for now (the browser build runs everywhere). Native online play
  only works between copies on the same machine (for testing); online play is a browser feature.
- **Not done yet**: the fighters' shadows on the ground (the PSP draws them through a trick of
  its framebuffer formats that the port doesn't do yet: they are left out) and the opening movie
  (skipped by the boot mod anyway).
- **Online play is the newest part** of this port: if a battle or a room misbehaves, tell us.
- **Performance**: full speed at 4x on a laptop with integrated graphics (where it was made). The
  HD textures need more video memory; lower the internal resolution if a weak machine stutters.
- **Phones and tablets** get a note asking for a computer instead of the game (nothing of the
  game is downloaded there): it needs a keyboard or a gamepad, and a computer's memory.
- **Sound** starts after the first click or key press (browsers' rule).
- **Saves** are the game's own files: the native build and the browser build share the main one.
- **Switches** (environment variables natively, `?NAME=value` in the browser's address):
  `DBZ_MOD_SKIP_INTRO=0`, `DBZ_MOD_TITLE=0`, `DBZ_MOD_MENUS=0`, `DBZ_MOD_UNLOCK=0`,
  `DBZ_MOD_CAMERA=0`, `DBZ_MOD_CUSTOM=0`, `DBZ_MOD_ROOM=0`, `DBZ_MOD_NAME=0` and
  `DBZ_MOD_NETDELAY=0` turn the mods off, `DBZ_NET_INPUT_DELAY=N` sets the online battles' delay,
  `DBZ_SCALE=1..4` sets the render scale, `DBZ_FPS=1` logs the frame rate.
- Something wrong? In the browser, **F9** writes a diagnostic of the current frame into
  `build/webdump/` on the host (only from the host's own browser). The browser's console (F12)
  shows the game's log.

## Repository layout

```
port/
  runtime/        the PSP's system, rewritten (CPU helpers, VFPU, threads, files, overlays,
                  graphics, sound, saves, network, texture packs, the HD font)
  mods/           the mods (C), hooked by address in hooks.txt
  web/            the browser page (shell.html), audio and LiveKit glue
  tools/          generate.sh, recomp.py, prx_load.py, serve_web.py, textures.py, build scripts
  third_party/    FFmpeg's headers (the libraries come from tools/get_ffmpeg.sh), stb_truetype
docs/
  NOTES.md        technical notes: the disc, the overlays, the online battles, the renderer, the mods
re/
  export/         the analysis: program.json (functions, jump tables), the overlays', imports
  ghidra_scripts/ how the analysis was made (Ghidra)
  tools/          the EBOOT decrypter, the archive and overlay tools, Ghidra helpers
```

## Credits

- **Bandai Namco** (Namco Bandai Games) and **Spike**, for Dragon Ball Z: Tenkaichi Tag Team;
  Akira Toriyama, for Dragon Ball.
- [PPSSPP](https://www.ppsspp.org): the reference for how the PSP behaves, its FFmpeg builds, its
  tables of PSP system function names and its EBOOT decrypter.
- [Ghidra](https://ghidra-sre.org) and [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex),
  [Emscripten](https://emscripten.org), [SDL](https://www.libsdl.org), [FFmpeg](https://ffmpeg.org),
  [LiveKit](https://livekit.io) (client library, Apache-2.0),
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (BSD-3-Clause),
  [stb_truetype](https://github.com/nothings/stb) (public domain / MIT); the HD texture pack's
  words are drawn with the [Ubuntu font](https://design.ubuntu.com/font) (Ubuntu Font Licence) and
  [DejaVu](https://dejavu-fonts.github.io).
