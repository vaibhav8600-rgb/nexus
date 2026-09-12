<#
.SYNOPSIS
    NEXUS companion for Windows. Needs nothing installed.

.DESCRIPTION
    Pushes the clock, CPU and memory load and what is playing to a NEXUS
    dongle over its host-link USB serial. One way: the dongle never writes
    back, so this cannot type, press keys or read anything out of it.

    The Python companion does the same thing and runs everywhere, but it
    needs pyserial and psutil. Windows can do all of this out of the box,
    and a companion you have to install things for is a companion you run
    once - so this exists.

.EXAMPLE
    .\nexus_host.ps1 -List
    .\nexus_host.ps1
    .\nexus_host.ps1 -Port COM15 -Verbose
#>
[CmdletBinding()]
param(
    # Serial port. Found automatically when omitted.
    [string]$Port,
    # List candidate ports and exit.
    [switch]$List,
    # Seconds between updates.
    [double]$Interval = 1.0
)

$TEXT_MAX = 39          # NEXUS_HOST_TEXT - 1; the dongle truncates anyway
$CLOCK_EVERY = 60       # seconds between clock resyncs

function Get-NexusPorts {
    <#  ZMK's USB id is 1D50:615E. The dongle exposes two serial interfaces -
        one is ZMK Studio's RPC, one is this - and which is which depends on
        the order the USB stack registered them, so both are candidates. #>
    Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'COM(\d+)' -and
                       $_.DeviceID -match 'VID_1D50&PID_615E' } |
        ForEach-Object {
            [pscustomobject]@{
                Port = [regex]::Match($_.Name, 'COM\d+').Value
                Interface = [regex]::Match($_.DeviceID, 'MI_(\d+)').Groups[1].Value
            }
        } | Sort-Object Interface
}

# Now playing, from the same Windows media session the volume flyout shows.
# Anything that reports to it works - browsers, Spotify, Apple Music - with no
# per-app support and nothing installed.
#
# The WinRT plumbing is set up once here rather than per call: resolving the
# type and building the AsTask shim every second would cost more than the
# reading is worth.
$script:npReady = $false
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop
    $script:asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        })[0]
    # One line, deliberately: PowerShell will not parse a type literal split
    # across lines, whatever the line length rules say.
    [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media, ContentType = WindowsRuntime] | Out-Null
    $script:npReady = $true
} catch {
    Write-Warning "now playing unavailable: $($_.Exception.Message)"
}

function Wait-WinRt($operation, $type) {
    $task = $script:asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    if (-not $task.Wait(2000)) { throw 'WinRT call timed out' }
    $task.Result
}

function Get-NowPlaying {
    if (-not $script:npReady) { return '' }
    try {
        $mgrType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]
        $mgr = Wait-WinRt ($mgrType::RequestAsync()) $mgrType

        # Prefer whatever is actually playing. With a paused YouTube tab and
        # music running, the "current" session is not always the one making
        # the noise, and the noise is what you want named.
        $session = $null
        foreach ($s in $mgr.GetSessions()) {
            if ($s.GetPlaybackInfo().PlaybackStatus -eq 'Playing') {
                $session = $s; break
            }
        }
        if (-not $session) { $session = $mgr.GetCurrentSession() }
        if (-not $session) { return '' }

        $propType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
        $p = Wait-WinRt ($session.TryGetMediaPropertiesAsync()) $propType

        $artist = "$($p.Artist)".Trim()
        $title = "$($p.Title)".Trim()
        $text = if ($artist -and $title) { "$artist - $title" }
                elseif ($title) { $title } else { $artist }

        # Fold to what the panel's font covers: ASCII 32..90, upper case only.
        # The firmware does this too and does not trust us to - but doing it
        # here keeps the wire carrying only what can be drawn, and means a
        # title reads correctly on firmware built before that fold existed.
        return -join ($text.ToUpper().ToCharArray() |
            Where-Object { [int]$_ -ge 32 -and [int]$_ -le 90 })
    } catch {
        # A session can vanish between listing it and asking about it.
        Write-Verbose "now playing: $($_.Exception.Message)"
        return ''
    }
}

if ($List) {
    Write-Output 'NEXUS candidates (VID 1D50, PID 615E):'
    Get-NexusPorts | Format-Table -AutoSize
    Write-Output 'All serial ports:'
    Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'COM\d+' } |
        Select-Object Name | Format-Table -AutoSize
    return
}

# One sample to prime the counter - the first read of % Processor Time is
# always 0 and would put a lie on the screen for the first second.
$cpuCounter = '\Processor(_Total)\% Processor Time'
try { Get-Counter $cpuCounter -ErrorAction Stop | Out-Null } catch {}

Write-Output 'NEXUS companion running. Ctrl-C to stop.'
$open = @{}
$script:moaned = @{}      # ports already complained about, so it is said once
$script:waiting = $false

try {
  # Outer loop: reconnect rather than exit. The dongle disappears from USB on
  # every reflash and every reboot, and a companion that has to be restarted
  # by hand each time is a companion you stop bothering with.
  while ($true) {
    if ($open.Count -eq 0) {
        # Which port? If not told, try every NEXUS interface at once: the one
        # that is the host link shows up on the dongle, and the other is
        # Studio's and ignores us. Writing a line it cannot parse costs
        # Studio nothing - but pass -Port if you use Studio at the same time.
        $targets = if ($Port) { @($Port) } else { (Get-NexusPorts).Port }
        foreach ($name in $targets) {
            try {
                $sp = New-Object System.IO.Ports.SerialPort($name, 115200)
                $sp.WriteTimeout = 2000
                $sp.DtrEnable = $true
                $sp.Open()
                $open[$name] = $sp
                Write-Output "open $name"
            } catch {
                # Said once per port, not once per retry: a port that is
                # busy stays busy, and three lines a second of that is not
                # a log, it is noise. Silence would be worse though - a
                # companion that says "running" while quietly failing to
                # open anything is the most confusing state it can be in,
                # and "Access is denied" here almost always means another
                # copy of this script already has the port.
                if (-not $script:moaned[$name]) {
                    Write-Warning "$name : $($_.Exception.Message)"
                    $script:moaned[$name] = $true
                }
            }
        }
        if ($open.Count -eq 0) {
            if (-not $script:waiting) {
                Write-Output 'waiting for the dongle'
                $script:waiting = $true
            }
            Start-Sleep -Seconds 3
            continue
        }
        $script:waiting = $false
        $script:moaned = @{}
        # A fresh link knows nothing about us: resend everything.
        $lastClock = [datetime]::MinValue
        $lastNp = $null
    }

    while ($open.Count -gt 0) {
        $lines = New-Object System.Collections.Generic.List[string]

        if (((Get-Date) - $lastClock).TotalSeconds -ge $CLOCK_EVERY) {
            $now = Get-Date
            $lines.Add('T ' + [int]($now - $now.Date).TotalSeconds)
            $lastClock = $now
        }

        try {
            $cpu = (Get-Counter $cpuCounter -ErrorAction Stop
                   ).CounterSamples[0].CookedValue
            $lines.Add('C ' + [int][math]::Round($cpu))
        } catch {}

        try {
            $os = Get-CimInstance Win32_OperatingSystem
            $used = 100 * (1 - $os.FreePhysicalMemory / $os.TotalVisibleMemorySize)
            $lines.Add('M ' + [int][math]::Round($used))
        } catch {}

        $np = Get-NowPlaying
        if ($np.Length -gt $TEXT_MAX) { $np = $np.Substring(0, $TEXT_MAX) }
        if ($np -ne $lastNp) { $lines.Add("N $np"); $lastNp = $np }

        # Never nothing. The dongle drops the link after a few silent seconds,
        # so a round where every reading failed and the track did not change
        # would show NO LINK while this is plainly still running - the one
        # state that sends you looking for a hardware fault that is not there.
        # A clock resend is the cheapest line to say it with: the dongle draws
        # minutes, so landing in the same one costs no repaint.
        if ($lines.Count -eq 0) {
            $now = Get-Date
            $lines.Add('T ' + [int]($now - $now.Date).TotalSeconds)
        }

        foreach ($name in @($open.Keys)) {
            foreach ($line in $lines) {
                try {
                    $open[$name].WriteLine($line)
                    Write-Verbose "$name > $line"
                } catch {
                    Write-Warning "$name lost: $($_.Exception.Message)"
                    try { $open[$name].Close() } catch {}
                    $open.Remove($name)
                    break
                }
            }
        }
        Start-Sleep -Milliseconds ([int]($Interval * 1000))
    }

    # Dropped out of the inner loop: the dongle went away. Say so once, then
    # go back round and wait for it to come back.
    Write-Output 'link lost; waiting for the dongle'
    Start-Sleep -Seconds 3
  }
} finally {
    # "I am going away", so the dongle shows dashes rather than numbers that
    # stopped being true the moment this stopped.
    foreach ($name in @($open.Keys)) {
        try { $open[$name].WriteLine('X'); $open[$name].Close() } catch {}
    }
    Write-Output 'stopped'
}
