<#
.SYNOPSIS
  Drive the CSI probe console over the ST-LINK virtual COM port from a script.

.DESCRIPTION
  Sends commands and echoes everything the board says, so a measurement session can
  be run non-interactively - from a build script, or by an agent that has no
  terminal emulator. The port must be free: a terminal program holding it will make
  the open fail.

  Each command is followed by reading until the board has been quiet for -IdleMs.
  That is what makes the timing work without knowing how long a command takes: the
  probe prints continuously while it waits for a source power-cycle (a dot per
  second), so quiet really does mean finished rather than working.

.PARAMETER Commands
  Console commands to send, in order. Nothing is sent if empty - the script then
  just listens, which is the way to watch a boot banner.

.PARAMETER Sequence
  The same commands as one semicolon-separated string. Use this when the script is
  started with powershell.exe -File, which cannot pass an array.

.PARAMETER IdleMs
  How long the board has to stay quiet before the next command is sent. Default
  8000, which clears the gap between the data type walk and the geometry search.

.PARAMETER MaxSeconds
  Upper bound per command, in case the board never goes quiet.

.EXAMPLE
  ./Tools/debug/csi-console.ps1 -Commands 'probe 2500','vcs','grab 0 0x2f 64'

.EXAMPLE
  ./Tools/debug/csi-console.ps1 -MaxSeconds 20
  Listen only - for catching a reset banner.
#>
[CmdletBinding()]
param(
  [string]   $Port       = 'COM16',
  [int]      $Baud       = 115200,
  [string[]] $Commands   = @(),
  [string]   $Sequence,
  [int]      $IdleMs     = 8000,
  [int]      $CharDelayMs = 3,
  [int]      $MaxSeconds = 180,
  [string]   $Log
)

$ErrorActionPreference = 'Stop'

# powershell.exe -File flattens an array parameter into one comma-joined string,
# so a caller that cannot use -Command has no way to pass -Commands. -Sequence is
# that way in: one string, semicolons between commands.
if ($Sequence)
{
  $Commands = $Sequence -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
}

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$serial.ReadTimeout  = 200
$serial.WriteTimeout = 1000

try   { $serial.Open() }
catch { throw "cannot open $Port : $($_.Exception.Message). A terminal program is probably holding it." }

$transcript = New-Object System.Text.StringBuilder

function Read-Until-Idle
{
  param([int]$IdleMs, [int]$MaxSeconds)

  $lastData = Get-Date
  $deadline = (Get-Date).AddSeconds($MaxSeconds)

  while ((Get-Date) -lt $deadline)
  {
    if ($serial.BytesToRead -gt 0)
    {
      $chunk = $serial.ReadExisting()
      [void]$transcript.Append($chunk)
      Write-Host -NoNewline $chunk
      $lastData = Get-Date
    }
    else
    {
      if (((Get-Date) - $lastData).TotalMilliseconds -gt $IdleMs) { break }
      Start-Sleep -Milliseconds 50
    }
  }
}

try
{
  Start-Sleep -Milliseconds 200
  $serial.DiscardInBuffer()

  if ($Commands.Count -eq 0)
  {
    Read-Until-Idle -IdleMs ($MaxSeconds * 1000) -MaxSeconds $MaxSeconds
  }
  else
  {
    foreach ($command in $Commands)
    {
      Write-Host "`n----- > $command"
      [void]$transcript.AppendLine("----- > $command")

      # One character at a time, with a gap. The probe's console reads a single
      # byte per HAL_UART_Receive() call with no FIFO and no interrupt, so a
      # burst arriving while it is printing loses characters - which showed up
      # as commands like '4probe 2500' and 'gstatus', the leftovers of an
      # earlier line mixed into the next one.
      foreach ($ch in $command.ToCharArray())
      {
        $serial.Write([string]$ch)
        Start-Sleep -Milliseconds $CharDelayMs
      }
      $serial.Write("`r")
      Read-Until-Idle -IdleMs $IdleMs -MaxSeconds $MaxSeconds
    }
  }
}
finally
{
  $serial.Close()
  if ($Log) { $transcript.ToString() | Out-File -FilePath $Log -Encoding utf8 }
}
