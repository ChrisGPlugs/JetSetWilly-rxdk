# jsw-rxdk (Jet Set Willy on RXDK + SDL3)

Follow the workspace rules in `../../CLAUDE.md`. Port-specific notes:

- `../../ref/JetSet_Willy_NXDK` is the read-only nxdk reference. Don't edit it.
- Game code is the nxdk port's, unchanged: `zx.c`, `jsw.c`, `jsw_code.c`, `jsw128.c`, `jsw128_code.c`,
  `z80c_*`, `tape.c`, `ay.c`, `zx_font.c`. Keep it in sync with nxdk rather than changing it here.
- Port layer (from manicminer-rxdk): `main.c` (front end, JetSetWillyX combined build only),
  `platform_rxdk.cpp` (video/D3D8, log, save-file ops, IGR, exit).
- **Game data is never committed.** `datafiles/` is git-ignored apart from its README; deployed to
  `D:\datafiles\`. Delete `out/Release` after changing it.
- Build is slow (~8 min): `jsw128_code.c` is 18.8k lines.
- Test: `pwsh -File tools/rxdk-run.ps1 -Project ports/jsw-rxdk -Seconds 60 -Until 'JSW READY'`.
- Title ID 0x4A535758 ("JSWX"), shared with the nxdk XBEs.
