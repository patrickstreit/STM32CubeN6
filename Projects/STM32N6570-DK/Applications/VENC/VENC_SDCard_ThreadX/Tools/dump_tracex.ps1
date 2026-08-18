<#
.SYNOPSIS
    Dump the TraceX ring buffer from a running STM32N6570-DK over SWD.

.DESCRIPTION
    Resolves the address and size of `tracex_buffer` from the linker map so the
    script keeps working after a rebuild, then uploads that memory region with
    STM32_Programmer_CLI.

    The resulting file is the raw TraceX buffer: it can be opened with the
    TraceX tooling as before, and converted for Perfetto with

        python Tools/trace/trace_convert.py trace.trx

.PARAMETER Output
    Destination file. Defaults to Tools/trace/out/trace.trx.

.PARAMETER Config
    Build configuration whose map file is used. Defaults to Debug.

.PARAMETER Mode
    Connection mode. HOTPLUG (default) attaches without resetting, which is
    required: the buffer lives in external PSRAM that is only reachable while
    the application keeps XSPI in memory-mapped mode. UR would reset the target
    and both the mapping and the trace contents would be lost.

.EXAMPLE
    ./dump_tracex.ps1
    ./dump_tracex.ps1 -Output out/run1.trx -Config DebugWithOptions
#>
[CmdletBinding()]
param(
    [string]$Output,
    [string]$Config = "Debug",
    [ValidateSet("HOTPLUG", "UR", "NORMAL")]
    [string]$Mode = "HOTPLUG",
    [string]$MapFile,
    [string]$ProgrammerCli
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not $MapFile) {
    $MapFile = Join-Path $projectRoot "Appli/build/$Config/VENC_SDCard_ThreadX_Appli.map"
}
if (-not (Test-Path $MapFile)) {
    throw "Map file not found: $MapFile. Build the $Config configuration first."
}

if (-not $Output) {
    $Output = Join-Path $PSScriptRoot "trace/out/trace.trx"
}

# --- locate STM32_Programmer_CLI -------------------------------------------
if (-not $ProgrammerCli) {
    $candidates = @()
    if ($env:STM32_PRG_PATH) {
        $candidates += Join-Path $env:STM32_PRG_PATH "STM32_Programmer_CLI.exe"
    }
    $candidates += (Get-Command STM32_Programmer_CLI.exe -ErrorAction SilentlyContinue |
                    Select-Object -ExpandProperty Source)
    $candidates += (Get-ChildItem "C:/Program Files/ST" -Recurse -Filter STM32_Programmer_CLI.exe `
                        -ErrorAction SilentlyContinue |
                    Sort-Object FullName -Descending |
                    Select-Object -ExpandProperty FullName)
    $ProgrammerCli = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $ProgrammerCli) {
    throw "STM32_Programmer_CLI.exe not found. Pass -ProgrammerCli or set STM32_PRG_PATH."
}

# --- resolve the buffer from the map ---------------------------------------
# The map records the section placement line followed by the symbol line:
#     .psram_bss     0x90c3f800   0x300000 .../app_azure_rtos.c.obj
#                    0x90c3f800                tracex_buffer
$lines = Get-Content $MapFile
$symbolIndex = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s+0x([0-9a-fA-F]+)\s+tracex_buffer\s*$') {
        $symbolIndex = $i
        $address = [Convert]::ToUInt32($Matches[1], 16)
        break
    }
}
if ($symbolIndex -lt 0) {
    throw "Symbol 'tracex_buffer' not found in $MapFile. Is TX_ENABLE_EVENT_TRACE defined?"
}

$size = 0
for ($i = $symbolIndex - 1; $i -ge [Math]::Max(0, $symbolIndex - 5); $i--) {
    if ($lines[$i] -match '0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s') {
        $size = [Convert]::ToUInt32($Matches[2], 16)
        break
    }
}
if ($size -le 0) {
    throw "Could not determine the size of tracex_buffer from $MapFile."
}

$outDir = Split-Path -Parent $Output
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
# STM32_Programmer_CLI resolves relative paths against its own working directory.
$Output = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Output))

$records = [Math]::Floor(($size - 48 - 32 * 48) / 32)
Write-Host ("tracex_buffer : 0x{0:X8}, {1:N0} bytes (~{2:N0} records)" -f $address, $size, $records)
Write-Host "programmer    : $ProgrammerCli"
Write-Host "output        : $Output"
Write-Host "Reading over SWD, this takes a while for a multi-megabyte buffer..."

# Note: do not name this $args, which is a PowerShell automatic variable.
# STM32_Programmer_CLI only accepts .bin/.hex/.srec/.s19 for -u, so always
# upload to a .bin and move it to the requested name afterwards.
$tempBin = [System.IO.Path]::ChangeExtension($Output, ".upload.bin")
$cliArgs = @(
    "-c", "port=SWD", "mode=$Mode",
    "-u", ("0x{0:X8}" -f $address), ("0x{0:X}" -f $size), $tempBin
)
& $ProgrammerCli @cliArgs
if ($LASTEXITCODE -ne 0) {
    throw "STM32_Programmer_CLI failed with exit code $LASTEXITCODE."
}
if (-not (Test-Path $tempBin)) {
    throw "STM32_Programmer_CLI reported success but produced no file at $tempBin."
}
Move-Item -Path $tempBin -Destination $Output -Force

Write-Host ""
Write-Host "Convert with:"
Write-Host "    python `"$(Join-Path $PSScriptRoot 'trace/trace_convert.py')`" `"$Output`""
