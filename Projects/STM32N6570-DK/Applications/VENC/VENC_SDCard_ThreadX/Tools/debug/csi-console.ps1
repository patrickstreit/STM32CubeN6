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

.PARAMETER WaitFor
  Regex that ends a command's read as soon as the output matches it, instead of
  waiting for the board to fall quiet. Needed for commands that work in silence -
  'link' prints its prompt and then says nothing until the source appears.

.PARAMETER MaxSeconds
  Upper bound per command, in case the board never goes quiet.

.PARAMETER OnPromptCommand
  Command line to run when the board's output matches -OnPrompt. This is how the
  manual power-cycle disappears: the probe asks for the source to be restarted,
  and whatever can do that runs here. The script knows nothing about what that
  is - a programmable supply, a relay, a switchable hub are all just a command.
  Nothing is run if this is empty, which keeps the default behaviour unchanged.

.PARAMETER OnPrompt
  Regex that triggers -OnPromptCommand. Defaults to the probe's own request.

.PARAMETER OnPromptCooldownMs
  The probe reprints its prompt while it waits; one power cycle per second would
  be worse than none. Further matches are ignored for this long after a trigger.

.EXAMPLE
  ./Tools/debug/csi-console.ps1 -Commands 'probe 2500','vcs','grab 0 0x2f 64'

.EXAMPLE
  ./Tools/debug/csi-console.ps1 -MaxSeconds 20
  Listen only - for catching a reset banner.

.EXAMPLE
  ./Tools/debug/csi-console.ps1 -OnPromptCommand 'dutpower cycle' `
      -WaitFor "first frame after" -Sequence "link 280000"
  Unattended: the source is restarted whenever the probe asks for it.
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
  [string]   $WaitFor,
  [string]   $Log,
  [string]   $OnPrompt           = 'power-cycle the source now',
  [string]   $OnPromptCommand,
  [int]      $OnPromptCooldownMs = 5000
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

$script:lastTrigger = [datetime]::MinValue

function Invoke-OnPrompt
{
  param([string]$Matched)

  if (((Get-Date) - $script:lastTrigger).TotalMilliseconds -lt $OnPromptCooldownMs) { return }
  $script:lastTrigger = Get-Date

  Write-Host "`n----- ! $OnPromptCommand (matched '$Matched')"
  [void]$transcript.AppendLine("----- ! $OnPromptCommand (matched '$Matched')")

  # The serial port stays open across this. The probe counts seconds while it
  # waits for a clock and prints a dot per second; closing the port would lose
  # exactly the output the caller is waiting for.
  try
  {
    $output = Invoke-Expression $OnPromptCommand | Out-String
    if ($output) { Write-Host $output.TrimEnd() }
    if (($LASTEXITCODE -ne $null) -and ($LASTEXITCODE -ne 0))
    {
      Write-Host "----- ! command exited $LASTEXITCODE"
      [void]$transcript.AppendLine("----- ! command exited $LASTEXITCODE")
    }
  }
  catch
  {
    # A source that cannot be restarted is worth reporting, but not worth
    # abandoning the session over - the operator can still do it by hand.
    Write-Host "----- ! command failed: $($_.Exception.Message)"
    [void]$transcript.AppendLine("----- ! command failed: $($_.Exception.Message)")
  }
}

function Read-Until-Idle
{
  param([int]$IdleMs, [int]$MaxSeconds, [string]$WaitFor)

  $lastData = Get-Date
  $deadline = (Get-Date).AddSeconds($MaxSeconds)
  $seen     = ''

  while ((Get-Date) -lt $deadline)
  {
    if ($serial.BytesToRead -gt 0)
    {
      $chunk = $serial.ReadExisting()
      [void]$transcript.Append($chunk)
      Write-Host -NoNewline $chunk
      $lastData = Get-Date

      # Accumulated whether or not -WaitFor is set: the power-cycle trigger has
      # to work in an idle-timeout run too. Bounded, because a long session
      # would otherwise keep every byte the board ever said.
      $seen += $chunk
      if ($seen.Length -gt 8192) { $seen = $seen.Substring($seen.Length - 8192) }

      if ($OnPromptCommand -and $OnPrompt -and ($seen -match $OnPrompt))
      {
        # Drop what has already been matched, or the same prompt fires again on
        # the next chunk that arrives.
        $matched = $Matches[0]
        $seen    = $seen.Substring($seen.IndexOf($matched) + $matched.Length)
        Invoke-OnPrompt -Matched $matched
      }

      # Some commands say nothing at all while they work - 'link' prints its
      # prompt and then waits in silence for a source that may be minutes away.
      # Going quiet is not the same as being finished, so those need a pattern
      # to wait for rather than an idle timeout.
      if ($WaitFor -and ($seen -match $WaitFor)) { break }
    }
    else
    {
      if ((-not $WaitFor) -and (((Get-Date) - $lastData).TotalMilliseconds -gt $IdleMs)) { break }
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
      $before = $transcript.Length

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
      Read-Until-Idle -IdleMs $IdleMs -MaxSeconds $MaxSeconds -WaitFor $WaitFor

      # The board loses the occasional character - it polls one byte at a time
      # with no FIFO - and a command that arrives as 'esingle 0' is simply gone.
      # It says so itself, which makes the retry cheap and unambiguous.
      if ($transcript.ToString().Substring($before) -match 'unknown command')
      {
        Write-Host "`n----- > $command (retry: a character was lost)"
        foreach ($ch in $command.ToCharArray())
        {
          $serial.Write([string]$ch)
          Start-Sleep -Milliseconds $CharDelayMs
        }
        $serial.Write("`r")
        Read-Until-Idle -IdleMs $IdleMs -MaxSeconds $MaxSeconds -WaitFor $WaitFor
      }
    }
  }
}
finally
{
  $serial.Close()
  if ($Log) { $transcript.ToString() | Out-File -FilePath $Log -Encoding utf8 }
}
