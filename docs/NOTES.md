# Dragon Ball Z: Tenkaichi Tag Team (ULUS-10537): technical notes

## The disc
- `PSP_GAME/SYSDIR/EBOOT.BIN`: encrypted (~PSP), decrypted with re/tools/prxdec. `BOOT.BIN` is all
  zeros. A plain executable (ELF ET_EXEC, module "DBTVS"): code 0x08804040..0x08a663ec, data up to
  0x08a8149d, a second segment (0x08aefb70) mostly bss; gp 0x08af7bf0. SHA-1
  eb7bffa629a689df3a35b19f14b508b005642735. `OPNSSMP.BIN` (a module) is not loaded.
- `USRDIR/PACKFILE.BIN` (627 MB): the game's archive. 16-byte header ("PacK", 0x3711 files), then a
  16-byte entry per file, scrambled (FUN_0882fb3c): sector = (w0 ^ header word 0) + the archive's
  first sector + 0x70, size = w1 ^ index, w2 ^ 0xaaaaaaaa, w3 ^ index (re/tools/packfile.py).
  The game reads the table through the raw UMD (`umd1:`, seek and read counted in sectors) from
  the sector `sceIoGetstat` gives in st_private[0], then each file as `disc0:/sce_lbnX_sizeY`.
- `INSDIR/DATAINST.BIN`: the data install (a PGD file, for the memory stick); the port answers the
  install dialog canceled. One PACKFILE file (sector 0xe98b, 1184 bytes) is PGD-encrypted, opened
  with flag 0x40000000 and key ioctl 0x04100001; not decrypted yet (the game goes on without it).

## Code overlays
- 11 ELF files in PACKFILE.BIN (numbers 6, 10, 12, 16, 20, 23, 31, 47, 55, 64, 13932), type 0xffa8,
  one PT_LOAD at 0x08a814a0 (right after the executable's data) up to 450 KB; a 16-byte header
  (word 0: the overlay's number), code from +0x10. The game reads the ELF header, the program
  header, then the segment in place, and calls into it.
- The port: re/tools/overlays.py extracts them and makes executable + overlay ELFs for Ghidra
  (re/analyze_overlays.sh); recomp.py --overlay N translates each into generated/ov_N.c (functions
  o<N>_<addr>, calls out to the executable's f_ and the system); runtime/overlay.h + core.c
  rt_lookup: an address in an overlay goes to the overlay whose first bytes are at its base.
- Entries the analysis misses: the code after the header (+0x10), the code after every `jr ra`
  (functions reached only by addresses the game works out), the data's words pointing into it. The
  executable's functions the overlays call are made entries of the executable.

## The recompiler and WebAssembly
- A loop spanning function entries (a tail call around a cycle of functions) is a chain of C calls:
  GCC turns them into jumps, WebAssembly doesn't, and the browser's stack overflowed
  (FUN_08a577ac / FUN_08a57bd0 at the title). Each cycle of tail calls is merged into one C
  function entered by address; its members wrap it (recomp.py tail_cycles / merged_functions).

## Runtime additions
- Kernel: message boxes, sceKernelExtendThreadStack (the game's main runs in it: its stack is the
  thread's while it runs, as sceKernelCheckThreadStack measures; the game quits if under 4 KB),
  memset / memcpy, sceDisplayWaitVblankStartMultiCB (the game runs at 30 fps: two vblanks a frame),
  native threads (rt_native_thread) and guest calls with up to 8 arguments.
- VFPU vt4444 / vt5551 / vt5650 (colors to 16 bits, at the start of a fight). ATRAC3+ low-level
  decoding (sceAtracGetAtracID / LowLevelInitDecoder / LowLevelDecode).
- Utility: the on-screen keyboard (answers the text it was given, else DBZ_NICK or "Player"; the
  game then asks "Is this Player Name ok?" with No selected), the data install dialog (canceled).
- scePowerCheckWlanCoexistenceClock = 1 (a PSP-2000/3000; 0 shows "cannot operate at 333 MHz during
  online play" at boot).

## Renderer
- Blend factors DOUBLESRCALPHA / ONE_MINUS_DOUBLESRCALPHA (6, 7): the source alpha doubled, to 1 at
  most, for both. The stage is drawn with them and textures of alpha 0x80 ("opaque" here): src * 1
  + dst * 0. Doubling the color instead and keeping 1 - a for the destination washed everything out.
- The fight's shadows: each fighter sets one bit of a 4444 framebuffer (0x158000; color masks 0xbf,
  0xdf, 0xef...), which is then projected on the stage and the fighters as a CLUT16 texture (its
  pixels as palette indices). Neither per-bit color masks nor palette lookups from a framebuffer are
  done yet; drawn raw it painted the fighters black, so those draws are left out (DBZ_FBO_CLUT=1).
- The game's alpha 0x80 convention and the fighters turning see-through near the camera are the
  game's own; fighters looking pink in 4-player fights is still to check.

## Online battles (Multiplayer, ad hoc)
- Rooms: sceNetAdhocMatching (runtime/matching.c); a child sees the rooms (HELLO with 468 bytes of
  room data), asks to join, the host accepts ("Received request to join Room. Will you accept?"),
  confirms the members, then everyone moves to a new ad hoc group and the matching stops.
- The battle: PDP port 32, lockstep. Each copy sends every frame a packet of 3 records (the current
  frame and the 2 before: frame u32, a sync value u32, buttons u32, analog x, y) - type 3 from the
  guests to the host, type 4 from the host to everyone (+ 4 status bytes per player). Each battle
  step (FUN_08815610 / FUN_08814718) needs every player's record of the previous frame from its
  10-record ring (FUN_088174ac), waits for missing ones (FUN_0881548c, which times out into
  "Connection was lost") and checks the sync values agree (FUN_088155c0).
- Why it lags: a copy waits whenever a record takes more than a frame (33 ms) to arrive; and while
  waiting it keeps sending packets whose current record is empty (frame -1), which the receivers
  stored too (FUN_088174fc), pushing real records out of their rings: under latency the battle
  slowed to a third, stalled, or disconnected.
- mods/netdelay.c: empty records are not kept; the battle steps use the records DELAY frames back
  (DBZ_NET_INPUT_DELAY, default 3) for every player, the copy's own included (same inputs on the
  same frame on every copy), after a 60-frame warmup. Tested with copies on one machine and the
  native transport's simulated latency (DBZ_NET_DELAY_MS / JITTER_MS / LOSS): 2 players 120 ms +
  30 ms jitter each way with DELAY 6, 4 players 60 + 20 ms with DELAY 4, 2 players 60 ms + 2% loss:
  30 battle frames a second, the sync values equal.

## Mods (port/mods, hooks in port/hooks.txt)
- boot.c: the boot scene (autosave notice, logos, opening) runs unseen at full speed and the
  title's opening is skipped: the title in about a second. DBZ_MOD_SKIP_INTRO=0 keeps it.
- menus.c (DBZ_MOD_MENUS=0 for the game's menus): START on the title loads the save (or starts a
  new game without one), the New Game / Load Game / Install Data menu never shown. The Option
  menu keeps Volume Settings, Controller and Camera: its list (overlay 20, 7 items shown 4 rows at
  a time: cursor row +112, the item of each row +116.., the items +136.., +21 drawn) never turns
  its rows and its cursor goes round the 3 (FUN_08a8bc08 moves it, FUN_08a8c338 is asked every
  frame); Install Data, Change Player Name, Auto Save (on, the game's default) and Save are gone.
- menus.c, the main menu: only Free Battle and Multiplayer. Its list (overlay 16) is the Option
  menu's kind and round: FUN_08a83508(list, n) turns its rows, FUN_08a843c8(list, 1 up / 0 down)
  moves the cursor. As it is made it turns to the item of the mode the player comes back from
  (the scene manager *(0x08a7ea50), previous scene at +8); here always to Free Battle (rows: Free
  Battle, Multiplayer, the 3 after them not drawn), the cursor on Multiplayer when back from it,
  going round the 2 rows. The scroll arrows are still drawn.
- charsel.c: the character select's Custom step (Default / Customize 1..3) passed by with Default.
  Each player has a steps object (vtable 0x08a73e18): +120 the step (1 characters, 2 Select Form,
  3 Select Color, 4 Custom, 5 chosen), +124 the next (FUN_08a064b0). FUN_08a0ab08(select, action
  0 confirm / 1 back, steps) moves a player on (every mode): a confirm on 3 is followed at once by
  the step-4 confirm (the Custom panel select+0x98 opened and closed in one frame), a back that
  lands on 4 by the step-4 back. No turbo (the first version ran the panel in turbo: the music
  jumped). DBZ_MOD_CUSTOM=0.
- unlock.c: every character (save +0x184, 9 bytes) and map (+0x21a4, 10 bits) unlocked; the
  battle camera always Camera Type C (+0x52c6: 3 A, 4 B, 5 C; DBZ_MOD_CAMERA=0).
- runtime/sas.c: no sound starts in a stretch run in turbo (the title menu's confirm, the boot
  logos); DBZ_TURBO_SOUND=1 hears them.
- title.c: the title screen's bottom line (copyright, license) hidden: its texture (512x32, pack key
  fc6c12b4cd3bf663) made see-through as it is decoded (runtime/ge.c rt_texture_decoded, a weak hook
  for mods). DBZ_MOD_TITLE=0.
- room.c: Multiplayer's Create New Room makes the room at once (Free Battle, no custom characters). The
  panel (overlay 55, vtable 0x08abcbc8 at +0x40, found in the heap by it): +0x48 its state (0 opening,
  1 buttons), +0xa4 mode (0 Free Battle), +0xa5 custom; START = FUN_088115f8(FUN_08811a58(), 4, 0), sound
  (1,6), +0x9c = 3 (2 cancelled). The opening is passed by (+0x50 = 1). DBZ_MOD_ROOM=0, DBZ_ROOM_LOG=1.
- clock.c: Multiplayer's question about the PSP-1000's 222 MHz clock answered No, never shown.
- menupad.c: the analog stick moves through every menu as the d-pad does (the pause menu too).
- profile.c: the player's name is the site's account name (DBZ_NICK), written into the save.
- netdelay.c: the online battles' input delay (above).
