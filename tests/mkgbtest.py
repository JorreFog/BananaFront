#!/usr/bin/env python3
"""mkgbtest.py <out.gb>: a tiny Game Boy test cartridge for BananaFront's PC tests (own code, GPL-3.0).
MBC1 + 8 KiB battery RAM. Every frame (waits for LY=144): WRAM 0xC000 counter += 1, writes it to SRAM 0xA000 and
0xA000+counter, so a test can check that the save (.srm), states and READ_CORE_MEMORY see the game's memory.
Joypad: holding A adds 0x10 to the counter instead of 1 (input test). The LCD is on with a scrolling background."""
import sys

rom = bytearray(32 * 1024)
code = bytes([
    0xF3,                    # 0150 di
    0x3E, 0x0A, 0xEA, 0x00, 0x00,  # ld a,0x0A ; ld (0x0000),a   RAM enable
    0x31, 0xFE, 0xFF,        # ld sp,0xFFFE
    0x3E, 0x91, 0xE0, 0x40,  # ld a,0x91 ; ldh (0x40),a     LCD on, BG on
    0x3E, 0xE4, 0xE0, 0x47,  # ld a,0xE4 ; ldh (0x47),a     BGP
    0x3E, 0x80, 0xE0, 0x26,  # ld a,0x80 ; ldh (0x26),a     sound on
    # loop:                    0x0165
    0xF0, 0x44,              # ldh a,(0x44)  LY
    0xFE, 0x90,              # cp 144
    0x20, 0xFA,              # jr nz,-6
    0x3E, 0x10, 0xE0, 0x00,  # ld a,0x10 ; ldh (0x00),a     select the buttons (P15 low)
    0xF0, 0x00,              # ldh a,(0x00)
    0xE6, 0x01,              # and 1         A = bit 0 (0 = pressed)
    0x06, 0x01,              # ld b,1
    0x20, 0x02,              # jr nz,+2
    0x06, 0x10,              # ld b,0x10
    0xFA, 0x00, 0xC0,        # ld a,(0xC000)
    0x80,                    # add a,b
    0xEA, 0x00, 0xC0,        # ld (0xC000),a
    0xEA, 0x00, 0xA0,        # ld (0xA000),a
    0x26, 0xA0,              # ld h,0xA0
    0x6F,                    # ld l,a
    0x77,                    # ld (hl),a
    0xE0, 0x43,              # ldh (0x43),a   SCX scrolls
    # wait for LY to leave 144
    0xF0, 0x44, 0xFE, 0x90, 0x28, 0xFA,  # ldh a,(0x44); cp 144; jr z,-6
    0xC3, 0x65, 0x01,        # jp loop
])
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])  # nop; jp 0x150
logo = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
rom[0x104:0x134] = logo
rom[0x134:0x144] = b"BFTEST".ljust(16, b"\0")
rom[0x147] = 0x03  # MBC1+RAM+BATTERY
rom[0x148] = 0x00  # 32 KiB
rom[0x149] = 0x02  # 8 KiB RAM
c = 0
for b in rom[0x134:0x14D]:
    c = (c - b - 1) & 0xFF
rom[0x14D] = c
rom[0x150:0x150 + len(code)] = code
# a background pattern so the picture isn't blank: tile 0 striped
for i in range(16):
    rom[0x4000 - 64 + i] = 0
g = sum(rom) - rom[0x14E] - rom[0x14F]
rom[0x14E] = (g >> 8) & 0xFF
rom[0x14F] = g & 0xFF
open(sys.argv[1], "wb").write(rom)
