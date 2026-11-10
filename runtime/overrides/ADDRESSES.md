# Commandos2-ModernPatch → comm2.exe 1.2 address map

The ModernPatch (Windows `ddraw.dll`, v1.3.0) targets the Steam `Comm2.exe`. Our recompilation uses the official
1.2 patch exe (`gamedata/comm2.exe`, SHA-1 `7b3eb230…c1c6`, no SafeDisc), whose code is laid out differently.
The addresses were first matched by hand against the 2001 retail exe (`analysis/comm2.exe`, see git history before the
1.2 switch), then carried over to 1.2 with `recomp/remap.py`: each function matched uniquely by its bytes with
relocations and call rel32 masked, every call site's target cross-checked against its mapped callee, globals by
unanimous relocation pairs. WinMain, the camera/face draws, the blitters and the frame limiter are byte-identical to
retail modulo relocations, so all struct and stack-frame offsets carried over unchanged. The retail matching used: REA/Ghidra and capstone over `analysis/functions.json`, using the patch's own expected bytes,
constants, strings, call shapes and runtime call-chain traces (`C2_MODETRACE`, `C2_PRESENTTRACE`).

Hook kinds (`recomp/overrides.txt`): a plain address replaces a function (`f_X` in C, original kept as `orig_f_X`);
`site` redirects one call instruction to a C hook; `patch` changes bytes at lift time and in guest memory.

## Globals / structures
| What | Steam | 1.2 | Notes |
|---|---|---|---|
| CJuego* | 0xBCBAD4 | 0xb58488 | +0x2c screen state, +0x30 previous, +0xc4 limiter flag, +0xea cheats, +0xec transition |
| options (video settings) | 0xBCBADC | 0xb58490 | +0x54 mode index |
| mission* | 0xBD57EC | 0xb5a650 | |
| video object* | 0xC15BDC | 0xb58654 | get mode 0x40ee50, set mode 0x40ee70 (thiscall mode*) |
| loading screen object* | 0xBCBB14 | 0xb584c8 | |
| current screen getter | 0x790f40 | 0x4522e0 | |
| screen switch | | 0x454cb0 | sets +0x2c, calls old vtbl[8] / new vtbl[7] (activate) |

## Main loop (WinMain 0x402200, loop 0x4031ae–0x40347d)
| What | Steam | 1.2 |
|---|---|---|
| CJuego::Update (site) | 0x7956f0 (0x405239) | 0x4523d0 (0x40323a) |
| CJuego::Frame (site) | 0x792e10 (0x40529b) | 0x4526f0 (0x4032a9) |
| canvas init | 0x7caf00 | 0x420dc0 (this = esp+0x44, arg esp+0x11c) |
| video lock / unlock | 0x7cde80 / 0x7cdf00 | 0x421510 / 0x421590 (this = esp+0x90, arg esp+0x18) |
| present | 0x7ce430 | 0x422220 (this = esp+0x90, arg 0) |
| canvas reset (site) | 0x7cacd0 (0x40535c) | 0x420f60 (0x403465) |
| game SetMode(w,h) cdecl | 0x402a00 | 0x402120 |
| CJuego notify new size | 0x7920d0 | 0x4546e0 |

## Resolution / HD
| What | Steam | 1.2 |
|---|---|---|
| mode size helpers | 0x413f20–0x4140b0, 0x7b38c0 | 0x4ecf30 / 0x4ecf80 (by index), 0x4ecfd0 / 0x4ed010 (options width/height), 0x4ed070 (size) — `resolution.c` |
| video options resolution label | — | 0x476110: key R640/R800/R024 (0xb0bf44/4c/54) → lookup 0x414ef0 at 0x47614a — `site` → slot size text (`resolution.c`) |
| mode setter (clamps 0..3) | — | 0x4ecf00 → `resolution.c` |
| COMM2.CFG load clamp (MODOVIDEO 0..3) | — | 0x4ec4cf / 0x4ec4d5 — `patch` to the list size |
| options menu resolution selector | — | mouse 0x4762b0 (inline left arrow), prev 0x476d20, next 0x476de0, apply 0x476ec0 (hardcoded 640x480/800x600/1024x756 notify), label 0x476110 — `resolution.c` |
| video options list | — | 0x474f90: SetRange(0,3,cur) 0x8dfb90 at 0x475028, SetLabels(R640..R280, 4) 0x8dff50 at 0x475066 — `site` |
| menu size getters (640/480) | 0x414020 / 0x414030 | 0x4ed050 / 0x4ed060 |
| pause menu activate getter calls | 0x7ab1a9 / 0x7ab1b5 | 0x46427d / 0x464289 (in 0x464270) — `patch` |
| mission activate → SetMode | 0x7abc2d → 0x7add80 | 0x464e9d → 0x474f60 (in 0x464e80) — `site` |
| mission activate → reload | 0x7abc46 → 0x447640 | 0x464eb7 → 0x518ea0 (gated by byte this+9) — `site` |
| mission loading begin | 0x44c901 → 0x4079d0 | 0x517935 → 0x4e1490 (in mission load 0x517880) — `site` |
| generic loading begin | 0x44d4c5 / 0x44e076 → 0x4075c0 | 0x518fbf → 0x4e0d40 (in saved-game load 0x518f50) — `site` |
| loading screen end | | 0x4e12f0 |
| popup dialog setup (1.2 exit confirm etc.) | — | 0x8e2de0: centres the box with the 640x480 menu getters at 0x8e2ecf / 0x8e2eef — `site` → current display size (not in ModernPatch; its scaled pause menu hides it) |
| main menu version label | — | format `Ver 1.%d%d` 0xb0cc5c pushed at 0x490b34 / 0x491781 → `Ver 1.%d%d-Recomp` at 0xba9500 (padding after .data); x 0x24e / 0x221 moved left — `patch` |

## Game bug fixes
| What | Steam | 1.2 |
|---|---|---|
| scaled map blitter | 0x856880 | 0x95ef80 — override, `tileblit.c` |
| scaled map blitter (blend) | 0x856ea0 | 0x95f7f0 — override, `tileblit.c` |
| blitter caller (map background draw) | 0x857570 | 0x953890 (zoom 1.0 blitter 0x95c140) |
| tile cache column cap `mov eax,0x18` | 0x8587ce (in 0x858730) | 0x9522ce (in 0x952220) — `patch` → 96 |
| inventory grid handler / event byte table | 0x930790 / 0x93092c | 0x5d5390 / 0x5d5540 — `patch` [7] 03→01 |

## Frame interpolation (`mainloop.c`)
| What | Steam | 1.2 |
|---|---|---|
| main loop hooks | 0x405239 / 0x40529b / 0x40535c | `site` 0x40323a (Update) / 0x4032a9 (Frame) / 0x403465 (canvas reset = end of iteration) |
| WinMain frame offsets | canvas +0x44, video +0x5c, lock +0x14, surf +0xe8, have video +0x68, no draw +0x1b | canvas +0x44, video +0x90, lock +0x18, surf +0x11c, have video +0x9c, no draw +0x17 |
| adaptive frame skip / drew frame / skip present | 0xBB432C / 0xBB432D / 0xBD57E4 | 0xb0925c / 0xb09268 / 0xb5a648 |
| frame limiter (uses CJuego+0xc4) | 0x7922b0 | 0x4548e0 |
| mission target fps / draw id | mission+0x5b0 / +0x634 | same offsets |
| world viewer draw (CVentanaVisorMundo) | vtbl 0xAF37C4[2] = 0x484820 | 0x520930 (vtable entry at 0xaeb914) — override |
| camera | viewer+0xdc; +0x68/+0x6c pos, +0x74 zoom, +0x70 = 1/zoom, +0x80/+0x84 window, +0x94/+0x98 view size | same (verified at run time) |
| camera map draw | 0x7e7de0 | 0x90e030 (world draw 0x90d3e0) |
| mission screen vtable / draw | | 0xae8eb0 / 0x464c00 → 0x51c810 |
| arrow cursor animation (15 frames) | 0x44bd10 → 0x437d30 | `site` 0x51c839 → 0x516080 → 0x64a9f0 |
| 3D face sequence draw (CVisorSecuencias3D) | 0x92a5f0, draw-only tail 0x92a673 | 0x6ced50 (advance: 0x59a6f0 render next keyframe, 0x6ce8b0 next sequence; tail 0x6cedd2 blits [this+0xc] via 0x4325c0) — override |

Verified with a guest-memory diff across one extra frame (`C2_DIAG=N`): after the cursor guard only counters change
(frame stats 0xb5a62c, 3D render stats 0xb77f40/0xb77f4c, adaptive 3D detail 0xba1148, draw id).

Found by tracing guest call chains into the 3D renderer during extra frames: the face window draw (retail 0x8694e0, changed in 1.2) calls
0x6ced50. Before the fix the face changed in every presented frame (a 21-frame keyframe loop at 5x speed); after it,
only once per tick.

Not yet mapped (Steam-only guards / features): inventory pick-up shine
(0x92fd70 / 0x92fe20), text caret (menus get no extra frames here), object interpolation (0x7f7240), per-frame
arrow poll (the game moves the arrow by DirectInput deltas once per tick), map background cache.

## Debug switches
`C2_SPLIT_TEST=N` (force map blits into NxN pieces), `C2_CAMLOG` (camera fields / interpolation), `C2_DIAG=N` (memory
diff across the Nth extra frame; `C2_DIAG_PTR=addr` finds who points at an address), `C2_MODETRACE`,
`C2_PRESENTTRACE` (guest call chains of mode changes / presents). Script actions: `kdown VK DIK`, `kup VK DIK`, `wheel N`, `find A B` (log guest addresses holding dwords A, B).

## Not ported
Cheats (matcher 0x718fc0 called on 0xb652a8, password `GONZOANDJON`; retail call site 0x60c39a / string 0xb1f6e4, not mapped), cnc-ddraw,
crash VEH, debug watchpoints, profiler, in-game settings menu, UIScale, pause-menu scaling, water optimisations.
