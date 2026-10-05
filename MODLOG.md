# SC3U "GTA mode" — mod journal

Game: SimCity 3000 Unlimited, Steam app 2741560 (depot 2741561), Windows build run under Homebrew Wine 11 on
macOS (Apple Silicon, Rosetta) with cnc-ddraw 7.1.0.0. Offline single-player, no anti-cheat.

Goal: a mode that works in every city: possess a citizen, walk with WASD, Enter steals the nearest car, drive
and steer it, Enter again to get out, fight other citizens.

Paths
- Game: `prefix/drive_c/Games/SimCity 3000 Unlimited/Apps/` (SC3U.exe + GZCOM DLLs)
- Mod source: `mods/gta-mode/` (this folder)
- Decompiled output (never publish): `~/SimCity3000-decomp/` (`c/<bin>.c`, `c/<bin>.strings.txt`, Ghidra projects in `ghidra/g1..g6`)
- Ghidra 12.1.4 with JDK 21; decompiled output is kept outside the repository and never published

## TODO
- [x] Recon: binaries, imports, data files
- [x] Decompile all binaries (Ghidra headless, 6 batches)
- [x] Plugin loading: how SC3U.exe loads `*.dll` / `PlugIn\`, the GZCOM director contract
- [x] Main window, input, per-frame tick on the main thread
- [x] Rendering path: composite at cnc-ddraw upload (overlay.c)
- [x] Camera: view origin, zoom, rotation, world→screen
- [x] Map data: altitude (view tiles), roads/rail/power (SIMNTWRK pieces)
- [ ] Automata: the game's OWN traffic (stealing/possessing real STRTSIM agents) - partly mapped, see "Agents"
- [x] Assets: player/citizen/car sprites from People.DAT / Vehicles.DAT at runtime; fight frames from the game's own sets
- [x] Vertical slice: DLL loads, draws a marker at a tile under the camera
- [x] On foot: WASD walking, Shift run, camera follow, building collision
- [x] Cars: Enter to steal, drive and steer, Enter to exit; NPC traffic follows real roads
- [x] Citizens + fighting (punch, fight back, knockdown), police + BUSTED
- [x] Works in every city (Madison, Berlin, Sacramento, Mount Herrang, Roadless Paradise tested)
- [x] Document modding in CLAUDE.md

## Facts (confirmed)
- SC3U.exe: PE32 i386, image base 0x400000, has relocs. Imports DDRAW only via GZGraphicD.dll
  (`DirectDrawCreate`, `DirectDrawEnumerateA`). USER32 `GetAsyncKeyState`/`GetKeyState` used by the exe and GZWIND.DLL.
- Every game DLL (and SC3U.exe) exports a single symbol `GZDllGetGZCOMDirector`: Maxis GZCOM framework
  (the ancestor of the SimCity 4 one). MaxisAddOn.dll (5 KB of code) is a minimal GZCOM component.
- SC3U.exe strings: `*.dll`, `PlugIn\`, `PlugIn`, `%PluginDirectory%`, `addon-enabled`: the framework scans for DLLs.
- RTTI is stripped (only std exception classes): no free class names.
- Sprite data: `Apps/Res/Sprites/*.DAT` (00000006_Vehicles.DAT 7.5 MB, 00000008_Other.DAT, Boats, EffectSprites...).

## Plugin contract (confirmed in game)
- `FUN_0040b00f` (SC3U.exe) FindFirstFile `Apps\*.dll` (skips CLCD16/CLCD32/DPLAYERX/DRVMGT) -> cGZCOM::AddLibrary
  (`FUN_0048a920`, cGZCOM vtable 0x4d6d18 +0x14) -> `FUN_0048a575`: LoadLibraryA, GetProcAddress
  `GZDllGetGZCOMDirector`, director->InitializeCOM(com, path) [vt+0x0c]; then EnumClassObjects [vt+0x14],
  then OnStart(com) [vt+0x10]. Same IIDs as SC4: cIGZCOMDirector 0xA21EE941, cIGZFrameWorkHooks 0x03FA40BF.
- cIGZCOM vt+0x10 = FrameWork(). cIGZFrameWork vtable 0x4d6bf4: +0x0c AddSystemService, +0x10 GetSystemService
  (srvid, iid, out), +0x18 AddHook, +0x20 AddToTick, +0x24 RemoveFromTick, +0x28 AddToOnIdle, +0x38 OnTick(n),
  +0x3c OnIdle(). Main loop `FUN_0046135a`: PeekMessage x<=500, then fw->OnIdle() -> OnTick(counter) -> each
  tick service vt[11](counter) (list at fw+0x28); idle services vt[10] every N (list fw+0x24).
- gta_mode.dll registers a 13-slot service with AddToTick in OnStart: OnTick runs on the main thread every loop.

## Runtime facts
- Game DLLs are relocated (e.g. SIMSPR at 0x02D70000, not 0x10000000): always use module base + RVA.
- Window class `Gonzo`, title `SimCity 3000`, game resolution 800x600 (cnc-ddraw scales to the Mac window).
- Input injection: cnc-ddraw (devmode=false) drops mouse messages unless its cursor lock is active. Calling the
  class WndProc (`GetClassLong(GCL_WNDPROC)`, GZGraphicD+0x17e11) with game coordinates works.
  GZGraphicD `FUN_100178a6` maps WM_* to GZ events (LBUTTONDOWN 7, LBUTTONUP 9, KEYDOWN 5, KEYUP 6, CHAR 4).
- `SC3U.exe -intro:off "C:\...\Cities\Madison, WI.sc3"` loads a city directly (tips dialog: click 522,403).
- UI (game coords): zoom in 667,545; zoom out 773,545; rotate CW 752,572; rotate CCW 686,572; minimap ~660-790,440-560.

## Camera (SIMSPR.DLL, Ghidra base 0x10000000)
- World coords: float x, y(height), z; 256 units per tile, map space 0..65535 (256 tiles).
- `FUN_100241fb` project(world)->(sx,sy): rotate (x,z) by `FUN_10024193` (rot0 (x,z); rot1 (0xffff-z, x);
  rot2 (0xffff-x, 0xffff-z); rot3 (z, 0xffff-x)), then sx = A[zoom]*(z'-x')/256, sy = B[zoom]*(z'+x')/256 -
  C[zoom]*y/256 with A=[4,8,16,32,64] (0x1007183c), B=[2,4,8,16,32] (0x10071850), C=[1,2,4,8,16] (0x10071864).
  Zoom 4 = closest: a tile is a 128x64 diamond.
- Globals: rotation 0x10072b94, zoom 0x10072b98 (0..4), view object ptr 0x10072bec.
- View object (vtable SIMSPR 0x1006250c): +0x14/+0x18 map size in tiles, +0x24 tile grid, +0x28 zoom, +0x2c rotation,
  +0x54 visible rect (left, top, right, bottom) in projected pixels (800x600; copy at +0x64).
  screen = project(world) - (left, top) + (A, -B): the extra one-tile term was found by comparing with SIMSPR's own
  screen->tile picker `FUN_1000a48a(view, projx, projy, &tx, &tz, 0, 0, 0)` (which takes projected coords).
  Earlier "aligned" checks (water grid, power tower) were periodic and hid the offset.
- view vt[11] (+0x2c) `FUN_10006226` = ScrollBy(dx, dy, redraw): moves the rect (rounded to even at zoom 4) and redraws.

## Agents (the game's own automata) - partly mapped, not used by the mod yet
- STRTSIM.DLL street agents: base ctor `FUN_10019750` (0x15c bytes), ~11 subclasses (ctor list in the decomp:
  callers of FUN_10019750). Pooled: destroyed objects keep the base vtable 0x1002b95c. Spawner `FUN_1001f112`
  (cap 199, count at +0xc). Owner/spawner pointer at agent+0x44 (set by vt+0x140).
  Layout (base class): +0x24 type bits, +0x58 progress, +0x5c segment length, +0x60 segment start (int x,y,z),
  +0x6c segment end, +0x78 direction (float), +0x84.. sprite ids. Position = start + dir * progress.
- Boats (vtable 0x1002a758, 15 in Madison) use that layout. Street pedestrians are class 0x1002b79c/0x1002b738
  (int position x,y,z just before the 0x1002b79c vtable pointer). Cars appear to be classes 0x1002bbe0/0x1002af90
  with a different layout (not decoded).
- SIMSPR keeps 44-byte dynamic sprite records (screen bbox in projected coords, world x/z/y, link, float depth,
  flags, owner ptr at [10]) in arrays referenced from the view (e.g. vector at view+0x3a8). Clearing the flags word
  did not visibly hide a sprite (test was on off-screen peds: inconclusive).
- Next step to steal the game's own cars: decode the car class layout, find the agent removal path (behaviour
  stack at agent+0xf0, `FUN_1001b340`), then replace the agent with a mod car.
- SIMSPR `FUN_10032305`/`FUN_10031c4f`: movement update of SIMSPR-side sprite agents (planes etc.).

## Buildings and flora (confirmed)
- SIMGEOM occupants use the same packed position word. Class vtable 0x10029ba4 (tag at +0x10) = trees/flora,
  0x1002bb80 (tag at +0x14, low byte 1..8 = lot state/size) = buildings, 0x1002b058 (tag +0x0c) = other (88 tiles).
  `geo_scan()` builds g_geo[x][z]; buildings block walking/driving unless a network piece shares the tile.

## Fight animations (from the game's own People.DAT)
- 0x2ef0 fitness guy: jog 0-3, kick 7, punches 8-9. 0x2efa police officer: walk 0-2, baton swing 3-5.
- 0x2ee5 man who trips: walk 0-2, falls 3-8 (8 lying). 0x2ef9: officer tackling a suspect, 15 frames (used for BUSTED).
- Sets without a fall animation are shown lying by rotating their standing sprite 90 degrees.

## Overlay rendering (confirmed)
- cnc-ddraw (OpenGL renderer) uploads the 800x600 RGB565 primary surface each frame with glTexSubImage2D through a
  function pointer in its .data (resolved with GetProcAddress). gta_mode swaps that pointer (`overlay.c`), copies the
  frame, draws sprites/text into the copy and uploads it. No trails; game surfaces untouched. `grab` saves the frame.

## Native sprites: SIMSPR's dynamic sprite system (confirmed, used by src/native.c)
People and cars are drawn by the game's own renderer, so buildings, trees, pylons, highways and road props in front
of them hide them. Found 2026-10-04 (user: "cars and people walk through buildings and trees").
- Region render `FUN_1000d0f5` (zoom >= 3; `FUN_1000bafa`/`FUN_1000be25` below) collects every object overlapping
  a dirty rect from the view's screen-cell grid (view+0x380, cell size floats +0x39c/+0x3a0) into 0x2c-byte draw
  records at view+0x500 (type 0/1 static tile objects: buildings, roads; type 2 dynamic sprites) with a world
  footprint box at rec+0x1c..0x28. `FUN_1000c6f0` sorts them (depth key rec+0x14, comparator `FUN_1000c7a6`: layer
  byte rec+0x18 decides when footprints overlap), the frame render `FUN_1000dc17` draws them in that order with
  image vtable +0x40. Depth key `FUN_1000c962` = 0x40000000 - (x'+z')*64 - y/4 (rotated world coords).
- Dynamic sprite API, __thiscall on the view: `FUN_1000e52a` AddSprite(img, x, z, y, invalidate, layer, flags),
  `FUN_1000e83d` MoveSprite(img, oldx, oldz, newx, newz, y or -1) (updates the grid, redraws old+new rects via
  vt+0x130), `FUN_1000e6d5` RemoveSprite(img, x, z, invalidate). Records: 44 bytes, [0] image, [1..4] bbox in
  projected px, [5] x, [6] z, [7] y, [8] next in the tile's list (view+0x24 tile grid, +0xc per 20-byte tile),
  [9] depth key, byte 40 layer, byte 41 flags (bits 0-1: tile-aligned box; 0 = point +-100). All live sprites
  are in the hash set at view+0x3a4 (buckets +0x3a8..+0x3ac, nodes {next, rec}); `sprs` lists them by resource.
- Road traffic in SC3K is not agents: it is ~500 animated Roads.DAT sprites (set 0x2bc4) in this same system, like
  trees (Landscape.DAT sets), pylons (Utilities.DAT 0x4683) and other props.
- Image (0x14 bytes): vtable, id (low 17 bits = blit flags, bits 17-29 = refcount, top 2 = orientation),
  resource, child image, byte +0x10 frame base, byte +0x11 offset mode. Vtable slots used by the view: +4 AddRef,
  +8 Release (deletes via +0x64 at zero), +0x14 GetBounds(zoom, rot, out[4]), +0x34 Load(zoom, rot, 0),
  +0x38 Unload(zoom, rot), +0x40 Draw(surface, x, y, zoom, rot, clip); others: +0x28 SetId, +0x48 SetResource,
  +0x68 FrameKey(zoom, rot). Prop class 0x7312 (vtable 0x10062b80): key = ((orient + rot) & 3) + base + zoom*4.
  SIMSPR registers image classes 0x7300, 0x7312, 0x7315 (`FUN_1004c611`); the people/vehicle resources name
  0x7310/0x7302, which nothing registers, so `resource->CreateImage` (+0x7c) fails for them.
- Sprite resource = all frames of one set: resource manager `FUN_1005a97e()`, vt+0x14
  GetResource({0x6300, 0x6400, set}, IID 0x6100, &res) (any DAT file). Methods (thiscall, key = frame index):
  +0x20 GetBounds(key, out[4]) (= the DAT anchor record), +0x34 Load(key), +0x38 Unload(key),
  +0x40 Draw(surface, x, y, key, flags, clip), +0x7c CreateImage(&img). **Frame index = DAT instance number**
  (people: frame*16 + (zoom4 ? 8 : 0) + dir; cars: 0x60/0x30/0x00 + heading32 for zoom 4/3/2).
- The mod's image (`NatImg`) copies vtable 0x10062b80 and replaces FrameKey (returns the key we chose for that
  zoom), AddRef and Release (own counter; the game would `delete` our static object). Frames we switch to are
  loaded once with resource Load(key) and kept. Rotated poses (tumbling, lying without a fall animation) stay in
  the overlay; markers, punch flashes and the HUD too.
- **Add sprites on layer 1** (the buildings' layer), not 2. Where two footprints overlap, the comparator sorts by
  layer before depth; road props use 2, so at layer 2 a ped on a sidewalk or a car in the outer lane (whose
  +-100 footprint reaches into the next tile) was drawn on top of the building beside it. Layer 1 = depth order.
- **X-ray silhouette** (`OV_GHOST` in overlay.c): with correct occlusion the player's car was hidden 65-75% of the
  time in dense cities. For the player and their car the overlay finds where the game blitted the sprite (best
  of +-3 px alignment against the frame, exact RGB565 compare) and paints the covered pixels as a green outline +
  translucent fill, like isometric games do. Fully visible sprites get nothing.

## Fonts: Res/Text/<language>/*.FBF (confirmed, renderer in video/tools/fbf.py)
- u32 1, u32 atlas width (640), u32 atlas height, u32 1; atlas = width*height bytes, one level per pixel:
  1 = full ink .. 0x10 = no ink, 0x11 = outside a glyph cell; then 256 x u16 RGB565 palette (pre-coloured for the
  blue UI); then 256 x (i32 x0, y0, x1, y1, advance) glyph boxes indexed by the Windows-1252 code.
- MAIN9/MAIN10: bold pixel UI font (codes 3/4/5/11 are the game's left/right arrow, bullet and check mark),
  SYSTEM9, TITLE9/TITLE12: rounded antialiased title font, SERIF17: newspaper serif. The ad's text uses these.

## Sprites (confirmed, decoder in tools/sc3spr.py and src/sprites.c)
- Res\Sprites\*.DAT: IXF index (magic D7 81 C3 80, 20-byte records type/group/instance/offset/size).
  inst 0 = 20-byte header (0x107, ?, w, h, csize) + RefPack(QFS, 10 FB) body; inst 1 = s16 left, up, right, down (anchor = left, up).
  Body: u32 total, u16 w, h, u16 u1, u2, u32 key 0xF81F, u32 0, one run per row (u16 x, u16 len|flag, u32 cumulative
  pixel offset after the run; absent on the last row), RGB565 pixels.
- People.DAT (type 7): set = group>>16 (0x2EE1..0x2F36); low = frame*16 + (zoom4 ? 8 : 0) + dir. Peds ~13 px tall at zoom 4.
- Heading convention (the game's own formula, STRTSIM `FUN_1000b5bf`: dir8 = (int)((atan2(dz, dx) + pi) / (pi/4) + 0.5),
  8 wraps to 0; constants pi, pi/4, 0.5 at 0x1002b6d0/d8/0x1002baa0). Rotation 0: dir 0 = -x (screen up-right),
  1 up, 2 -z (up-left), 3 left, 4 +x (down-left), 5 down, 6 +z (down-right), 7 right: counter-clockwise on screen.
  Vehicles use 32 headings with the same origin and direction (index = 4 * dir8). Apply it to the view-rotated vector.
  Confirmed with the delivery truck (cab = front): index 4 shows its back (driving up), 20 its front (driving down).
  (An earlier guess "0 = +z, clockwise" drew cars 90 degrees off along roads: the "car slides sideways" bug.)
- Vehicles.DAT (type 6): 144 sprites per car = 3 zooms x (32 headings + 16 slope variants): zoom2 0x00, zoom3 0x30,
  zoom4 0x60 + heading. Cars: 2c89 police, 2cc9 taxi, 2cc7 red car, ...
  2c8d/2c8e = rioter crowds, 2ced/2cef/2cf5/5a4e planes, 2cee helicopter.

## Map data (confirmed)
- View tile records (SIMSPR view+0x24 -> cols[x] -> 20-byte rec[z]): +0xb altitude (world y = alt*256). Render data only.
- Networks: SIMNTWRK factory FUN_1000bdcd creates one heap object per network tile, class by type (22 types).
  +0x10 byte2 = type, +0x14 = x | z<<11 | alt<<22 | variant<<30, +0x18 piece code. `net.c` scans the heap for them
  (15 ms for 10.5k tiles in Madison). Types: 1 road, 2 highway (alt = ground under the deck), 3 rail, 9 power line.

## Gotchas
1. The Ghidra 12 release has no mac_arm_64 decompiler: `support/gradle/gradlew buildNatives`, copy build/os/mac_arm_64.
2. Killing analyzeHeadless JVMs does not stop a `for f in ...` batch loop: kill the loop shell first.
3. The memory scanner found its own candidate array (runaway to the cap): candidates live in a separate
   VirtualAlloc reservation that scans skip.
4. cnc-ddraw drops posted mouse messages when its cursor lock is off: call the class window proc directly.
5. SIMSPR picker/projection: the screen origin is one tile off the stored rect (see Camera).
6. Network piece x/z decode: bits 0-10 / 11-21 (not 8-bit fields); the low 11 bits looked like noise at first.
7. Building tags have low byte 1..8 (Madison 2, Berlin 1): filtering on ==1 missed all of Madison's buildings.
8. HUD text/rotated sprite caches must not free images a recent draw list still references (render thread).
9. Sprite heading order: don't infer it from a sprite sheet of a symmetric car. Read the game's formula or use a
   vehicle with an obvious front (the delivery truck), then check in game with `gta arrows` (heading lines).
10. `tools/gtacmd.sh` must write commands with `print -r`: zsh `echo` turned `\t` in `Z:\private\tmp` into a tab.
11. A sporadic access violation in SIMSPR+0xcfeb (FUN_1000cedb, the view's dirty-tile list) crashed the game twice
    during long recording sessions in Madrid; it was not traced. `main.c` now logs first-chance exceptions with
    module+offset to `gta_mode.log`. Restart the city (`tools/start_city.sh`) and re-shoot.
12. Year-end budget dialogs pop up over the city view in long sessions; close them with `click 570 488` (Done).
13. The `call`/`wr32` commands parse plain numbers as **decimal**: write addresses and IDs with `0x`
    (`call [[0x010a1df0]+14] 0x010a1df0 0x7a67c060 0x6100 ...`). `7a67c060` became 7 and crashed GZResourceD.
14. The game also crashed on its own once while idle with GTA mode off: STRTSIM.DLL+0x94db, null pointer.
15. Madrid is the crash-prone city: in a 4-minute driving test it crashed after 45 s (London ran 2 min clean), and
    most filming crashes were in Madrid, including one before native sprites existed. The mod log now appends
    (`==== session` markers, rotated at 1 MB) so the next crash's EXCEPTION line survives the relaunch.

## Log
- 2026-10-03: recon, installed Ghidra 12.1.4 + openjdk@21, headless decompile of all 30 binaries.
- 2026-10-03: GZCOM plugin loads; command channel, scanner, overlay hook, camera, sprites, roads, buildings.
- 2026-10-03: GTA mode playable: choose/possess citizen, WASD, fights, police, road traffic, stealing, driving.
  Smoke-tested in Madison, Berlin, Sacramento, Mount Herrang, Roadless Paradise (tools/test_city.sh).
- 2026-10-03: user saw the car "side-moving": sprite heading mapping was 90/180 degrees off. Switched to the game's
  formula (cars and people); verified with `gta arrows` at 8 headings and in a turn. Wall scrapes now swing the car
  parallel to the wall instead of sliding it sideways.
- 2026-10-04: filming tools for the 40 s Twitter ad (`video/`): a 30 fps raw frame recorder in the overlay hook
  (`rec start <file>` / `rec stop`; encode with `ffmpeg -f rawvideo -pix_fmt rgb565le -s 800x600 -r 30`),
  `gta crowd <n> [tiles]` (people dodge or get thrown tumbling into the air, `COMBO xN`), `gta autodrive <speed>|off`
  (the player's car uses the traffic brain, sticks to straight runs, rams other cars), `gta slowmo <scale> <secs>`
  (armed by the next run-over), `gta timescale`, `gta follow 0|1`, `gta focus <x> <y>`, `gta carat <tx> <tz> <dir>`,
  `gta at <tx> <tz>`, `gta spawncar <set> <tx> <tz> <dir> <speed>`, `gta carset <set> [car]`, `gta tpahead`,
  `gta bring <ped> [dist]`, `gta face <dx> <dz>`, `gta hp <n>`, `gta speed <v>`, `gta clear`, `gta mapdump <file>`
  (g_net u32[256][256] + g_geo + g_net_alt). MAX_PEDS 28 -> 64. Good stages: Madrid's tree-lined boulevard x=105
  (dual carriageway, tiles z 128..156), Madrid x=95 straight street, London's plaza intersection at tile 117,185.
- 2026-10-04: people and cars are now native SIMSPR sprites (src/native.c), so buildings, trees, pylons and
  highways in front of them hide them; verified in London (car behind the Euro Buros tower, player behind a
  house), Madrid, Berlin, Mount Herrang, Madison. Trees and other occupants now block like buildings, spawns move
  to the nearest free spot, traffic stops/re-aims at buildings and trees (despawns if wedged 1.5 s), car pushes
  never shove into solids, choose mode uses the same collision. Commands: `nat`, `sprs [m]`, `gta native 0|1`.
- 2026-10-05: native sprites moved to layer 1 (layer 2 put sidewalk peds and outer-lane cars over the building
  next to them); x-ray silhouette for the player and their car; FBF font format decoded. Twitter ad re-cut with
  footage re-shot on this build and all text in the game's own fonts (video/tools/fbf.py, make_text.py).
