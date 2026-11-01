# BananaFront's test content

BananaFront is a frontend: what it costs is per frame (copy, commit, sound, input), nearly the same whatever the core
does, so its A/Bs against RetroArch run the systems' own stress ROMs where an agent made one (NES: BananaNES's
stress-nes.nes, GBA: BananaGBA's, SNES: BananaSNES's) and, for the systems without an agent, the content below.

| System | Content | Licence / source | What it stands for |
|---|---|---|---|
| PICO-8 | `pico8/out/bf-stress-p8.p8` (build.py, L1-L10) | own, GPL-3.0 | platformer / shmup / RPG town / effects carts, all 4 sound channels (see build.py) |
| GB | `build/host/bftest.gb` (tests/mkgbtest.py) | own, GPL-3.0 | saves/states/memory test cart, not a load |
| GBC | `gb-homebrew/ucity.gbc` | uCity v1.3, GPL-3.0, github.com/AntonioND/ucity | a real GBC game engine (city builder) |
| GB | `gb-homebrew/libbet.gb` | Libbet and the Magic Floor v0.08, Zlib, github.com/pinobatch/libbet | a real DMG game engine (puzzle platformer) |
| MD | `md-homebrew/kleleatoms-md-121.bin` | KleleAtoms-MD v1.2.1, MIT, github.com/Nightwolf-47/KleleAtoms-MD | a real SGDK game engine (puzzle) |
| PICO-8 | `p8-homebrew/buzzsaw-cat.p8` | "Buzzsaw Cat" by dps2004 (Ludum Dare 45), shipped in fake-08's MIT-licensed repository (carts/ld45.p8) | a real PICO-8 game (platformer) |

The downloaded ROMs stay out of git (.gitignore). No owner games for GB, GBC, MD or PICO-8 on either handheld
(findings file, 02:05): these homebrew games are the final game check for those systems.
