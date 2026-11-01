# BananaFront

BananaOS's own frontend for libretro cores on the Anbernic RG DS (RK3566) and RG DS Plus (RK3568): what RetroArch
did for BananaOS, nothing more, and leaner. KMS straight to the game panel (banana-ui keeps the other through its
DRM lease), the display plane scales the core's picture (no GPU, no compositor), ALSA at the amps' 48 kHz with
dynamic rate control against the display, evdev input with BananaOS's hotkeys, RetroArch's save and state files
(same names and format: a game goes on in either), banana-ui's game-panel commands (RetroArch's words on stdin),
the universal companion export (`~/banana-emu/common/COMPANION-SPEC.md`).

    make                  # BOARD=rgds-plus (default) or BOARD=rgds: build/<board>/bananafront
    make host test        # PC build with no panel/sound + unit tests (correctness only, never performance)
    tests/host-test.sh <core.so> <rom>   # PC end-to-end: saves, states, thumbnails, commands, quick resume
    tools/devrun.sh <dev> <system> <core> <rom> <seconds> [tag]   # on a handheld through the menu (see the file)

What it has (2026-11-01): the in-game menu (BananaDS's, one panel; Quick settings: volume, brightness, shader,
picture size), RetroAchievements (rcheevos rc_client, RetroArch's token, offline answer cache), GPU shaders (BananaOS's
GLSL set; the GPU's fence in the commit), the screen border and pop-ups on an overlay plane, low latency (each frame
started as late as it still makes its refresh), its own clock governor (option), 2-4 players, quick resume, power-off
resume (SIGUSR1), PR #151's button jobs, the direct framebuffer for cores that ask, saves/states written on threads,
pads connected while playing (players 2-4), frames a slow refresh left behind caught up when they fit, the sound
device opened on a thread (the Plus's amps take 0.3 s), RetroArch hotkeys off (ra_hotkeys) honoured.

Measured against RetroArch through the same menu launch (tools/ab.sh, CPU% x clock = cycles, both handhelds): GBC
-50/-57 %, GBA -40 %, MD -33/-25 %, SNES -23 %, PICO-8 -6/-7 %, NES -4/-3 %; RSS 37-53 MB -> 7-27 MB. Saves and states
carry over both ways (tools/xcompat.sh: GB, NES, MD, PICO-8 on both handhelds).

Launcher: BananaLinux `run-bananafront` (branch front/bananafront); `run-retroarch` hands the systems in its
`BF_SYSTEMS` (Settings > Other systems > Frontend) to it.

Licence: GPL-3.0 (parts follow BananaDS and RetroArch, both GPL-3.0). Third party: libretro.h (MIT, RetroArch team),
rcheevos (MIT, RetroAchievements.org), stb_image_write (public domain / MIT, Sean Barrett), bcomp (MIT, BananaOS).
Memory-map resolution follows RetroArch 1.22's runloop.c/command.c (GPL-3.0, RetroArch team).
