# JetSetWillyX for the original Xbox (RXDK)

RXDK + SDL3 port of [JetSet_Willy_NXDK](https://github.com/ChrisGPlugs/JetSet_Willy_NXDK):
Jet Set Willy (48K, 1984) and Jet Set Willy 128 (v0.04 Hacklevel 9) in one XBE, with the
original Z80 game code translated to C. No game data is included.

## Playing
Put your own tapes next to `default.xbe` (or in a `datafiles\` folder beside it):
`JetSetWilly.tzx` (or `Jet Set Willy.tzx`, `.tap`, 32K `.bin`) and/or `JetSetWilly128.tzx`
(or `Jet Set Willy 128k v. 0.04 HL 9.tzx`). Optional `48.rom` for the original font.
With both, a chooser appears on the 48K title screen.

| Pad | Action |
|---|---|
| D-pad / left stick | Walk; choose on the chooser |
| A or B | Jump |
| LT / RT | Jump left / right |
| Start | Start game (title); pause (in play) |
| X | Pause · Y: tune on/off |
| Back | In play: END GAME? (A yes, B no); twice on a title screen: back to the chooser |
| White | Type WRITETYPER (only where the original accepts it: First Landing, standing on the floor at the foot of the stairs); with the cheat on, the room menu: teleport to any room |
| Black | Cheat menu (the classic POKEs, on / off) |
| Left-stick click | Diagnostics overlay |
| Right-stick click | 4:3 / 16:9 (saved) |
| LT + RT + Back + Start (or Black) | Return to the dashboard |

Hold X or Y at start-up for the untouched 1984 release (code card, original bugs).

### Cheats
**Black** opens the cheat menu in play: D-pad to choose, A to switch on or off, B to leave.
Each entry is one of the well-known POKEs, applied to the game's memory as on a Spectrum
(48K and 128 alike): Gentle guardians (POKE 37392,0), Nice nasties (38447,201),
Agreeable arrows (37505,24), Harmless heights (36578,16), No conveyors (36601,0),
Cheat mode (34275,10, the same as typing WRITETYPER), Infinite lives (35899,0),
Infinite time (35408,24), Superjump (36404,44: hold jump to keep rising until something solid stops you;
press it again in the air to rise again) and Deal with Maria
(38207,24: no Maria, so the bedroom is open without collecting the items).

With cheat mode on, **White** opens the room menu: D-pad up / down by one room, left / right
by ten, A to teleport there (the game's own teleport, so Willy keeps his place on the screen
and may fall), B to leave. The room names are read from the game.

## Building
`pwsh -File ../../tools/rxdk-run.ps1 -Project ports/jsw-rxdk -NoRun` from the RXDK workspace.

`src/jsw_code.c` and `src/jsw128_code.c` are generated: `pwsh -File tools/Regen-Code.ps1 -Skool <jsw.skool>`
rebuilds them from your tapes and the SkoolKit disassembly
([skoolkid/jetsetwilly](https://github.com/skoolkid/jetsetwilly), `sources/jsw.skool`). The
translator, `tools/z80c/z80c.py`, builds in both decodings of each POKEd instruction
(`--variant`), so the cheat menu can switch opcode POKEs on and off.

MIT licensed (see LICENSE).
