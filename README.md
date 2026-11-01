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

Launcher: BananaLinux `run-bananafront` (branch front/bananafront).

Licence: GPL-3.0 (parts follow BananaDS and RetroArch, both GPL-3.0). Third party: libretro.h (MIT, RetroArch team),
rcheevos (MIT, RetroAchievements.org), stb_image_write (public domain / MIT, Sean Barrett), bcomp (MIT, BananaOS).
Memory-map resolution follows RetroArch 1.22's runloop.c/command.c (GPL-3.0, RetroArch team).
