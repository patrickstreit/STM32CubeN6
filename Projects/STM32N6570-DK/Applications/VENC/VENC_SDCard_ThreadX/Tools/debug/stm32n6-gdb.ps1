<#
.SYNOPSIS
  Build, flash and debug the STM32N6570-DK VENC images from a console.

.DESCRIPTION
  The console equivalent of the cortex-debug configurations in .vscode/launch.json:
  it starts ST-LINK_gdbserver, attaches arm-none-eabi-gdb to the FSBL, adds the
  Appli symbols on top and runs to BOOT_Application, which is where the LRUN boot
  scheme hands over.

  What it does NOT do implicitly is write the application slot. GDB's 'load' only
  covers the executable it was given - the FSBL - exactly as '-target-download'
  does in the launch configuration. Pass -FlashAppli to have the programmer write
  Appli-trusted.bin to 0x70100000 first.

  Everything is scriptable: -Ex runs GDB commands after the entry point and, unless
  -Interactive is given, GDB exits afterwards and the server is stopped again. That
  is what makes this usable from a build script or an agent rather than only by hand.

.PARAMETER Preset
  Appli preset whose symbols to load and, with -Build / -FlashAppli, to build and
  write. Default CsiProbe.

.PARAMETER FsblPreset
  Preset the FSBL executable comes from. Default Debug: the FSBL is unaffected by
  the Appli compile definitions the Appli presets differ in, so it is normally the
  Debug one no matter which Appli is running.

.PARAMETER Ex
  GDB commands to run once the target sits at the entry point. Quoted strings, one
  per array element.

.PARAMETER Interactive
  Leave GDB attached on the console instead of detaching and exiting. Without it
  the script detaches, which resumes the target: a board left halted by a script
  nobody is attached to looks exactly like a board that crashed.

.PARAMETER NoFsblLoad
  Skip GDB's 'load'. The FSBL in external flash is left as it is - use this when
  only the application changed, which is the common case.

.PARAMETER DryRun
  Print the resolved tool paths and command lines and do nothing else. Touches
  neither the build nor the board.

.EXAMPLE
  ./Tools/debug/stm32n6-gdb.ps1 -Build -FlashAppli -Preset CsiProbe
  Rebuild the probe, write it to the application slot, run to BOOT_Application,
  let it run, exit.

.EXAMPLE
  ./Tools/debug/stm32n6-gdb.ps1 -NoFsblLoad -Ex 'info threads','bt' -Interactive
  Attach to what is already flashed and stay on the GDB prompt.

.NOTES
  Tools are looked up in the STM32Cube bundle directory and can be overridden with
  the environment variables STM32_GDB, STM32_GDBSERVER and STM32_PRG_PATH.
#>
[CmdletBinding()]
param(
  [string]   $Preset      = 'CsiProbe',
  [string]   $FsblPreset  = 'Debug',
  [int]      $Port        = 61234,
  [string[]] $Ex          = @(),
  [switch]   $Build,
  [switch]   $FlashAppli,
  [switch]   $NoFsblLoad,
  [switch]   $Interactive,
  [switch]   $DryRun
)

$ErrorActionPreference = 'Stop'

# Repository paths are derived from the script's own location, so the script works
# from any working directory - an agent's shell rarely sits where a human's does.
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppliDir    = Join-Path $ProjectRoot 'Appli'
$FsblElf     = Join-Path $ProjectRoot "FSBL\build\$FsblPreset\VENC_SDCard_ThreadX_FSBL.elf"
$AppliElf    = Join-Path $ProjectRoot "Appli\build\$Preset\VENC_SDCard_ThreadX_Appli.elf"
$AppliBin    = Join-Path $ProjectRoot "Appli\build\$Preset\VENC_SDCard_ThreadX_Appli-trusted.bin"

<#
  .SYNOPSIS Newest version directory of a STM32Cube bundle that contains bin\<exe>.
  .NOTES    Sorted by parsed version, not by name: "7.9.0" sorts above "7.14.0"
            as a string, and that is the wrong gdbserver.
#>
function Find-Bundle
{
  param([string]$Bundle, [string]$Exe)

  $root = Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$Bundle"
  if (-not (Test-Path $root)) { return $null }

  $candidates = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
      $numeric = ($_.Name -replace '^(\d+(\.\d+)*).*$', '$1')
      $version = $null
      if (-not [version]::TryParse($numeric, [ref]$version)) { $version = [version]'0.0' }
      [pscustomobject]@{ Version = $version; Path = (Join-Path $_.FullName "bin\$Exe") }
    } |
    Where-Object { Test-Path $_.Path } |
    Sort-Object Version -Descending

  if ($candidates) { return $candidates[0].Path }
  return $null
}

function Resolve-Tool
{
  param([string]$FromEnv, [string]$Bundle, [string]$Exe, [string]$What)

  if ($FromEnv -and (Test-Path $FromEnv)) { return $FromEnv }

  $found = Find-Bundle -Bundle $Bundle -Exe $Exe
  if (-not $found)
  {
    throw "$What not found. Install the STM32Cube $Bundle bundle or set the environment variable for it."
  }
  return $found
}

$Gdb       = Resolve-Tool -FromEnv $env:STM32_GDB       -Bundle 'gnu-tools-for-stm32' -Exe 'arm-none-eabi-gdb.exe'   -What 'arm-none-eabi-gdb'
$GdbServer = Resolve-Tool -FromEnv $env:STM32_GDBSERVER -Bundle 'stlink-gdbserver'    -Exe 'ST-LINK_gdbserver.exe'   -What 'ST-LINK_gdbserver'

$PrgPath = $env:STM32_PRG_PATH
if (-not $PrgPath)
{
  $PrgPath = Get-ChildItem 'C:\Program Files\ST\STM32CubeProgrammer_*\bin' -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $PrgPath) { throw 'STM32CubeProgrammer not found. Set STM32_PRG_PATH.' }

$PrgCli = Join-Path $PrgPath 'STM32_Programmer_CLI.exe'
# The external loader is what makes the octo-SPI flash at 0x70000000 writable and
# readable at all; without it the server connects and every access fails.
$Stldr  = Join-Path $PrgPath 'ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr'

foreach ($required in @($Stldr, $FsblElf, $AppliElf))
{
  if (-not (Test-Path $required)) { Write-Warning "missing: $required" }
}

# GDB parses backslashes in its own command strings as escapes, so every path that
# ends up inside -ex has to use forward slashes.
function ConvertTo-GdbPath { param([string]$Path) return ($Path -replace '\\', '/') }

$serverArgs = @(
  '-p', "$Port",
  '-d',                       # SWD
  '-s',                       # verify what is written
  '-e',                       # persistent: survives GDB detaching
  '-cp', $PrgPath,
  '-m', '1',                  # core selection, as in launch.json
  '-el', $Stldr
)

$gdbCommands = @("target extended-remote localhost:$Port")
if (-not $NoFsblLoad) { $gdbCommands += 'load' }
$gdbCommands += "add-symbol-file $(ConvertTo-GdbPath $AppliElf)"
$gdbCommands += 'tbreak BOOT_Application'
$gdbCommands += 'continue'
$gdbCommands += $Ex
# 'detach' rather than 'continue': GDB's detach hands the target back running, and
# it is the only way to end a batch run without leaving the core halted.
if (-not $Interactive) { $gdbCommands += @('detach', 'quit') }

$gdbArgs = @()
if ($Interactive) { $gdbArgs += '-q' } else { $gdbArgs += '-batch' }
$gdbArgs += (ConvertTo-GdbPath $FsblElf)
foreach ($c in $gdbCommands) { $gdbArgs += @('-ex', $c) }

if ($DryRun)
{
  Write-Host "project     : $ProjectRoot"
  Write-Host "gdb         : $Gdb"
  Write-Host "gdbserver   : $GdbServer"
  Write-Host "programmer  : $PrgCli"
  Write-Host "loader      : $Stldr"
  Write-Host "fsbl elf    : $FsblElf"
  Write-Host "appli elf   : $AppliElf"
  Write-Host "appli bin   : $AppliBin"
  Write-Host ''
  if ($Build)      { Write-Host "build       : cmake --build --preset $Preset   (in $AppliDir)" }
  if ($FlashAppli) { Write-Host "flash       : `"$PrgCli`" -c port=SWD mode=UR -el `"$Stldr`" -d `"$AppliBin`" 0x70100000 -v" }
  Write-Host "server      : `"$GdbServer`" $($serverArgs -join ' ')"
  Write-Host "gdb         : `"$Gdb`" $($gdbArgs -join ' ')"
  exit 0
}

if ($Build)
{
  Write-Host "==> building preset $Preset"
  Push-Location $AppliDir
  try
  {
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed with $LASTEXITCODE" }
  }
  finally { Pop-Location }
}

if ($FlashAppli)
{
  Write-Host '==> writing the application slot at 0x70100000'
  & $PrgCli -c port=SWD mode=UR -el $Stldr -d $AppliBin 0x70100000 -v
  if ($LASTEXITCODE -ne 0) { throw "flashing failed with $LASTEXITCODE" }
}

Write-Host "==> starting ST-LINK_gdbserver on port $Port"
$server  = Start-Process $GdbServer -ArgumentList $serverArgs -PassThru -WindowStyle Hidden
$gdbExit = 1

try
{
  # Wait for the port rather than for a fixed delay: the server needs a moment to
  # claim the probe, and how long depends on whether the target has to be reset.
  $deadline = (Get-Date).AddSeconds(20)
  $up       = $false
  while ((Get-Date) -lt $deadline)
  {
    if ($server.HasExited) { throw "gdbserver exited with $($server.ExitCode) - is another debug session holding the ST-LINK?" }
    try
    {
      $probe = New-Object System.Net.Sockets.TcpClient
      $probe.Connect('localhost', $Port)
      $probe.Close()
      $up = $true
      break
    }
    catch { Start-Sleep -Milliseconds 200 }
  }
  if (-not $up) { throw "gdbserver did not accept a connection on port $Port" }

  Write-Host '==> gdb'
  & $Gdb @gdbArgs
  $gdbExit = $LASTEXITCODE
}
finally
{
  if ($server -and (-not $server.HasExited))
  {
    Write-Host '==> stopping the gdb server'
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
  }
}

exit $gdbExit
