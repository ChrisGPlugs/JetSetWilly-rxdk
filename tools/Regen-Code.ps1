<#
.SYNOPSIS
  Regenerate src/jsw_code.c and src/jsw128_code.c (Jet Set Willy translated
  from Z80 to C) from the game tapes and the SkoolKit disassembly.

.DESCRIPTION
  Needs python and the RXDK clang (for the host tool that rebuilds the JSW128
  memory image), plus a clone of https://github.com/skoolkid/jetsetwilly
  (only sources/jsw.skool is read). The tapes are the user's own data files
  in datafiles/; nothing derived from them is written to the repo except the
  generated C (as before).

  The --variant options build the cheat-menu POKEs into the code (see
  the cheat menu in src/main.c): the translated code tests those bytes at run time, so the
  front end can apply and remove the POKEs.

.EXAMPLE
  pwsh -File tools/Regen-Code.ps1 -Skool ..\..\ref\jetsetwilly\sources\jsw.skool
#>
param(
    [Parameter(Mandatory)] [string] $Skool,
    [string] $Tape48  = 'datafiles/Jet Set Willy.tzx',
    [string] $Tape128 = 'datafiles/Jet Set Willy 128k v. 0.04 HL 9.tzx',
    [string] $Clang   = 'C:\ProgramData\RXDK\llvm\xbox-windows-x64\bin\clang.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $build = Join-Path $root 'out/regen'
    New-Item -ItemType Directory -Force $build | Out-Null
    $img48  = Join-Path $build 'jsw48.bin'
    $img128 = Join-Path $build 'jsw128_ram.bin'
    $tool   = Join-Path $build 'jsw128_image.exe'

    python -I tools/jsw48_image.py $Tape48 $img48
    if ($LASTEXITCODE) { throw 'jsw48_image failed' }
    & $Clang --target=x86_64-pc-windows-msvc -O2 -D_CRT_SECURE_NO_WARNINGS -o $tool `
        tools/jsw128_image.c src/jsw128.c src/tape.c src/zx.c
    if ($LASTEXITCODE) { throw 'building jsw128_image failed' }
    & $tool $Tape128 $img128
    if ($LASTEXITCODE) { throw 'jsw128_image failed' }

    # Cheat-menu POKEs whose opcode (or jump target) changes; see the cheat menu in src/main.c.
    $variants = @(
        '--variant', '38447:201',        # Nice nasties: RET
        '--variant', '37505:24',         # Agreeable arrows: JR
        '--variant', '36601:0',          # No conveyors: NOP
        '--variant', '35899:0',          # Infinite lives: NOP
        '--variant', '35408:24',         # Infinite time: JR
        '--variant', '36403:194,44,143'  # Superjump: POKE 36404,44 (JP NZ,36652)
    )
    $hooks48  = '34620 34631 34762 35068 35245 35914 35696 34463'.Split(' ') | ForEach-Object { '--hook', $_ }
    $hooks128 = '34762 35068 35245 35914 35696'.Split(' ') | ForEach-Object { '--hook', $_ }

    python -I tools/z80c/z80c.py $Skool $img48 32768 src/jsw_code.c @hooks48 @variants
    if ($LASTEXITCODE) { throw 'z80c (48K) failed' }
    python -I tools/z80c/z80c.py $Skool $img128 0 src/jsw128_code.c `
        --map tools/z80c/maps/jsw128_hl9_trace.txt --jumptable 34002:16 --descend --interrupts `
        --func jsw128_z80_run --skool48 $Skool --image48 $img48 @hooks128 @variants
    if ($LASTEXITCODE) { throw 'z80c (128) failed' }
} finally {
    Pop-Location
}
