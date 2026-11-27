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
| White / Black | Type WRITETYPER; with the cheat on, teleport to previous / next room |
| Left-stick click | Diagnostics overlay |
| Right-stick click | 4:3 / 16:9 (saved) |
| LT + RT + Back + Start (or Black) | Return to the dashboard |

Hold X or Y at start-up for the untouched 1984 release (code card, original bugs).

## Building
`pwsh -File ../../tools/rxdk-run.ps1 -Project ports/jsw-rxdk -NoRun` from the RXDK workspace.

MIT licensed (see LICENSE).
