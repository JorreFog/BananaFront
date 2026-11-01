#!/usr/bin/env python3
"""build.py: the PICO-8 stress cart (out/bf-stress-p8.p8), own code, GPL-3.0 (BananaFront).

Four scenes from real PICO-8 cart patterns, 25 s each, looping (soak: forever), at _update60:
  1 platformer  - map() of a full screen + a parallax layer, 40 walkers with map collision (fget/mget), the player,
                  coins: Celeste Classic / platformer-jam carts.
  2 shmup       - 30 enemies on paths, 150 bullets, 220 particles, circfill explosions, screen shake: shmup carts
                  (e.g. "Cherry Bomb"-style bullet patterns).
  3 rpg town    - two map layers, 24 NPCs walking with simple AI, a text box with print() of wrapped dialogue,
                  y-sorted sprites (table.sort-like insertion): top-down RPG carts.
  4 effects     - fillp patterns, pal() swaps per band (raster-style), line()/rectfill gradient, sspr scaling:
                  demo/jam carts.
Sound: music on 2 channels + sfx on the other 2 all the time (all 4 channels busy), like most carts.
Deterministic: srand(1) per scene; per scene, the frames run and a checksum of game state go to cartdata
(dset), so a run shows the frontend/core rendered the same logic. stat(1) (CPU of a frame, 1.0 = all of it) is
averaged per scene into cartdata too: the load each scene really puts on the emulated budget.
Load levels: L = 1..10 multiplies the entity counts (L5 = the realistic default above).
"""
import os
import sys

LEVEL = int(sys.argv[1]) if len(sys.argv) > 1 else 5
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)
name = "bf-stress-p8" + ("" if LEVEL == 5 else f"-L{LEVEL}")

lua = r"""
-- bf-stress-p8: BananaFront's PICO-8 stress cart (see build.py). level LV
lv=LV/5
scene_len=25*60
scene=1 sf=0 tf=0 sum=0 cpu_acc=0 cpu_n=0
names={"platformer","shmup","rpg town","effects"}
cartdata("bananafront_stress_p8")
function _init() start_scene(1) music(0) end
function start_scene(n)
 if scene and cpu_n>0 then
  dset(scene*4-4,flr(cpu_acc/cpu_n*1000))
  dset(scene*4-3,sum)
  dset(scene*4-2,sf)
  dset(scene*4-1,dget(scene*4-1)+1)
 end
 scene=n sf=0 sum=0 cpu_acc=0 cpu_n=0
 srand(n)
 ents={} buls={} parts={} npcs={}
 if n==1 then
  for i=1,flr(40*lv) do add(ents,{x=rnd(240),y=8+rnd(100),dx=rnd(1)-.5,dy=0}) end
  px=60 py=60 cam=0
 elseif n==2 then
  for i=1,flr(30*lv) do add(ents,{x=rnd(128),y=-rnd(128),t=rnd(1),hp=3}) end
  shake=0
 elseif n==3 then
  for i=1,flr(24*lv) do add(npcs,{x=rnd(240),y=rnd(240),d=flr(rnd(4)),t=0}) end
  msg="the old banana keeper says: the dark caves lie north of town. bring a torch, and some bread for the long walk. "
 end
end
function solid(x,y) return fget(mget(flr(x/8)%128,flr(y/8)%64),0) end
function upd_platformer()
 cam=(cam+.5)%120
 for e in all(ents) do
  e.dy=min(e.dy+.2,3)
  if solid(e.x,e.y+8) then e.dy=0 e.y=flr(e.y/8)*8 if rnd(1)<.02 then e.dy=-3 end end
  if solid(e.x+e.dx*8,e.y) then e.dx=-e.dx end
  e.x+=e.dx e.y+=e.dy
  if e.y>120 then e.y=8 end
  if e.x<0 or e.x>250 then e.dx=-e.dx end
  sum=(sum+flr(e.x)+flr(e.y))%32768
 end
 px=60+sin(sf/240)*40 py=60+cos(sf/180)*20
 if sf%30==0 then sfx(1,2) end
end
function drw_platformer()
 cls(12)
 map(0,16,-(cam/2)%128,0,32,16) -- parallax layer
 map(flr(cam/8),0,-(cam%8),0,17,16)
 for e in all(ents) do spr(2+flr(sf/8)%2,e.x-cam,e.y,1,1,e.dx<0) end
 spr(1,px,py)
 for i=0,15 do spr(4,(i*24-sf)%136-8,4+sin(i/7+sf/60)*3) end
end
function upd_shmup()
 for e in all(ents) do
  e.t+=1/120 e.y+=.6 e.x=64+sin(e.t)*50
  if e.y>136 then e.y=-8 e.hp=3 end
  if sf%20==flr(e.t*7)%20 and #buls<150*lv then
   local a=atan2(64-e.x,110-e.y)
   for k=-1,1 do add(buls,{x=e.x,y=e.y,dx=cos(a+k*.04)*1.5,dy=sin(a+k*.04)*1.5}) end
  end
 end
 for b in all(buls) do
  b.x+=b.dx b.y+=b.dy
  if b.x<-4 or b.x>132 or b.y<-4 or b.y>132 then del(buls,b) end
 end
 if sf%6==0 then
  local e=ents[1+flr(rnd(#ents))]
  if e then e.hp-=1 if e.hp<=0 then
   for i=1,12 do add(parts,{x=e.x,y=e.y,dx=rnd(2)-1,dy=rnd(2)-1,l=20+rnd(20)}) end
   e.y=-8 e.hp=3 shake=4 sfx(2,3) end end
 end
 while #parts>220*lv do deli(parts,1) end
 for p in all(parts) do p.x+=p.dx p.y+=p.dy p.l-=1 if p.l<0 then del(parts,p) end end
 shake=max(0,shake-1)
 sum=(sum+#buls*7+#parts)%32768
end
function drw_shmup()
 cls(1)
 camera(rnd(shake)-shake/2,0)
 for i=0,40 do pset((i*37)%128,(i*53+sf)%128,7) end
 for e in all(ents) do spr(5,e.x-4,e.y-4) end
 for b in all(buls) do circfill(b.x,b.y,1,8) end
 for p in all(parts) do pset(p.x,p.y,8+flr(p.l/10)) end
 for p in all(parts) do if p.l>30 then circ(p.x,p.y,(40-p.l)/3,10) end end
 camera()
 spr(1,60,110)
 print("score "..sum,2,2,7)
end
function upd_rpg()
 for n in all(npcs) do
  n.t+=1
  if n.t>30 then n.t=0 n.d=flr(rnd(4)) end
  local dx=({1,-1,0,0})[n.d+1] local dy=({0,0,1,-1})[n.d+1]
  if not solid(n.x+dx*4+4,n.y+dy*4+4) then n.x+=dx*.5 n.y+=dy*.5 end
  sum=(sum+flr(n.x)*3+flr(n.y))%32768
 end
 -- y-sort (insertion sort, as carts do it)
 for i=2,#npcs do local j=i while j>1 and npcs[j-1].y>npcs[j].y do npcs[j],npcs[j-1]=npcs[j-1],npcs[j] j-=1 end end
end
function drw_rpg()
 cls(3)
 local cx=60+sin(sf/600)*50 local cy=60+cos(sf/500)*50
 camera(cx,cy)
 map(32,0,0,0,32,32)
 map(64,0,0,0,32,32)
 for n in all(npcs) do spr(16+n.d,n.x,n.y) end
 camera()
 rectfill(4,88,123,124,0) rect(4,88,123,124,7)
 local k=flr(sf/2)%#msg
 local s=sub(msg,1,k)
 local y=92
 while #s>0 and y<120 do print(sub(s,1,28),8,y,7) s=sub(s,29) y+=7 end
end
function upd_fx() sum=(sum+sf*13)%32768 end
function drw_fx()
 cls()
 for y=0,127,4 do
  pal(7,(y/4+sf/4)%16)
  fillp(({0x5a5a,0xa5a5,0x0f0f,0xf0f0})[1+(y/4)%4])
  rectfill(0,y,127,y+3,7+((y+sf)/8)%8)
 end
 fillp() pal()
 for i=0,63 do line(64,64,64+cos(i/64+sf/300)*90,64+sin(i/64+sf/300)*90,8+i%8) end
 sspr(8,0,8,8,32+sin(sf/90)*20,32,64,64)
 for i=1,40 do circfill(64+cos(i/40+sf/200)*50,64+sin(i/40+sf/200)*50,3,i%16) end
end
upds={upd_platformer,upd_shmup,upd_rpg,upd_fx}
drws={drw_platformer,drw_shmup,drw_rpg,drw_fx}
function _update60()
 sf+=1 tf+=1
 if sf>=scene_len then start_scene(scene%4+1) end
 upds[scene]()
 cpu_acc+=stat(1) cpu_n+=1
end
function _draw()
 drws[scene]()
 rectfill(0,121,127,127,0)
 print(scene.." "..names[scene].." f"..tf.." cpu"..flr(stat(1)*100),1,122,11)
end
""".replace("LV", str(LEVEL))

# graphics: a few 8x8 sprites (player, walker x2, coin, enemy, NPCs facing 4 ways) + map tiles
spr = {
    0: ["00000000"] * 8,
    1: ["00aaaa00", "0aaaaaa0", "aa1aa1aa", "aaaaaaaa", "a2aaaa2a", "aa2222aa", "0aaaaaa0", "00a00a00"],
    2: ["00888800", "08888880", "88188188", "88888888", "08888880", "00888800", "08000080", "80000008"],
    3: ["00888800", "08888880", "88188188", "88888888", "08888880", "00888800", "00800800", "08000080"],
    4: ["00099000", "009aa900", "09a77a90", "09a77a90", "09a77a90", "09a77a90", "009aa900", "00099000"],
    5: ["e000000e", "0e2222e0", "02e88e20", "028ee820", "028ee820", "02e88e20", "0e2222e0", "e000000e"],
    16: ["00444400", "04ffff40", "04f1f140", "04ffff40", "00cccc00", "0cccccc0", "00c00c00", "00400400"],
    17: ["00444400", "04ffff40", "041f1f40", "04ffff40", "00cccc00", "0cccccc0", "00c00c00", "00400400"],
    18: ["00444400", "04ffff40", "04f1f140", "04ffff40", "00dddd00", "0dddddd0", "00d00d00", "00400400"],
    19: ["00444400", "04444440", "04444440", "04444440", "00dddd00", "0dddddd0", "00d00d00", "00400400"],
    32: ["33333333", "3b333b33", "33333333", "333b3333", "33333333", "b3333b33", "33333333", "3333b333"],  # grass
    33: ["44444444", "49444944", "44444444", "44494444", "44444444", "94444944", "44444444", "44449444"],  # ground, solid
    34: ["66666666", "65555556", "65666656", "65666656", "65666656", "65666656", "65555556", "66666666"],  # wall, solid
    35: ["cccccccc", "c7cccccc", "cccc7ccc", "cccccccc", "cc7ccccc", "cccccc7c", "cccccccc", "c7cccccc"],  # sky
}
gfx = [["0"] * 128 for _ in range(128)]
for n, rows in spr.items():
    sx, sy = (n % 16) * 8, (n // 16) * 8
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            gfx[sy + y][sx + x] = c
# pico-8 stores each byte's low nibble first per pixel pair: in the .p8 text, one hex digit per pixel, left to right
gfx_txt = "\n".join("".join(r) for r in gfx)
# flags: 33 and 34 solid (flag 0)
flags = ["00"] * 256
flags[33] = flags[34] = "01"
gff_txt = "\n".join("".join(flags[i * 128:(i + 1) * 128]) for i in range(2))
# map 128x32: platformer level (rows 0-15), parallax hills (16-31), towns at x 32-63 and 64-95
import random
random.seed(5)
m = [[0] * 128 for _ in range(32)]
for x in range(128):
    h = 12 + (x // 9) % 3
    for y in range(16):
        m[y][x] = 35 if y < h else 33
    if x % 11 == 5:
        for k in range(3):
            m[8][(x + k) % 128] = 34
    for y in range(16, 32):
        m[y][x] = 35 if y < 26 + (x // 5) % 4 else 32
for x in range(32, 96):
    for y in range(32):
        m[y][x] = 32 if (x * 7 + y * 3) % 23 else 34
        if x >= 64 and (x + y) % 9 and m[y][x] == 32:
            m[y][x] = 0  # second layer: mostly empty, walls drawn over the first
map_txt = "\n".join("".join(f"{v:02x}" for v in row) for row in m)
# sfx: 0 bass loop, 1 jump, 2 explosion, 3 lead loop; music pattern 0 loops 0+3 on channels 0-1
def sfx_line(speed, notes, loop=(0, 0)):
    s = f"00{speed:02x}{loop[0]:02x}{loop[1]:02x}"
    for (pitch, wave, vol, eff) in notes:
        s += f"{pitch:02x}{wave:x}{vol:x}{eff:x}"
    return s
bass = [((24 + [0, 0, 7, 5][i // 8]) if i % 2 == 0 else 0, 1, 5 if i % 2 == 0 else 0, 0) for i in range(32)]
lead = [((36 + [0, 4, 7, 12, 7, 4][i % 6]), 3, 4, 0) for i in range(32)]
jump = [((30 + i) if i < 8 else 0, 2, 6 if i < 8 else 0, 0) for i in range(32)]
boom = [((20 - i % 10) if i < 12 else 0, 6, (7 - i // 2) if i < 12 else 0, 0) for i in range(32)]
sfx = [sfx_line(16, bass, (0, 32)), sfx_line(4, jump), sfx_line(6, boom), sfx_line(8, lead, (0, 32))]
sfx += [sfx_line(16, [(0, 0, 0, 0)] * 32)] * 60
sfx_txt = "\n".join(sfx)
music_txt = "\n".join(["03 00034040"] + ["00 40404040"] * 63)

cart = f"""pico-8 cartridge // http://www.pico-8.com
version 41
__lua__
{lua.strip()}
__gfx__
{gfx_txt}
__gff__
{gff_txt}
__map__
{map_txt}
__sfx__
{sfx_txt}
__music__
{music_txt}
"""
open(os.path.join(OUT, name + ".p8"), "w").write(cart)
with open(os.path.join(OUT, name + ".phases.tsv"), "w") as f:
    for i, n in enumerate(["platformer", "shmup", "rpg-town", "effects"]):
        f.write(f"{i * 25}\t{n}\n")
print(os.path.join(OUT, name + ".p8"))
