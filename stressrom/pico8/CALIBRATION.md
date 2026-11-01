# PICO-8 stress cart: calibration

| Scene | Pattern it copies | Evidence | Status |
|---|---|---|---|
| platformer | full-screen map() + parallax map, 40 walkers with fget/mget collision, coins | common jam-cart structure (Celeste Classic draws map() + ~30 objects); fake-08 stat(1) 0.50 at L5 | synthetic (no owner carts, no reference run yet) |
| shmup | 30 enemies, up to 150 bullets, 220 particles, circfill/circ explosions | bullet/particle counts of typical PICO-8 shmups; stat(1) 0.48 at L5 | synthetic |
| rpg town | two map layers, 24 y-sorted NPCs, print() dialogue box | top-down RPG carts | synthetic |
| effects | fillp + pal() per band, 64 lines, sspr scaling, 40 circfills | demo/jam carts | synthetic |

stat(1) is fake-08's own estimate of the frame's CPU (1.0 = the whole budget). No PICO-8 carts are on either handheld
(owner, 2026-11-01), so the scenes stay synthetic until checked against freely licensed carts (e.g. from the BBS with
a CC licence) - not done tonight.
