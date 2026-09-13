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
    .\nexus_host.ps1 -Install
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
    # Start with Windows from now on, and start now. Per-user, no admin.
    [switch]$Install,
    # Undo that.
    [switch]$Uninstall,
    # Seconds between updates.
    [double]$Interval = 1.0
)

$TEXT_MAX = 39          # NEXUS_HOST_TEXT - 1; the dongle truncates anyway
$CLOCK_EVERY = 60       # seconds between clock resyncs

# ---- starting with Windows ------------------------------------------------
#
# The dongle cannot get the time out of USB. There is no request for it: a HID
# keyboard can ask the host nothing, and the host volunteers nothing but LED
# state. So something has to run here, and the honest thing to do about that is
# make it run itself.
#
# A shortcut in the per-user Startup folder, which is the oldest and least
# clever way Windows has of doing this: no admin, no UAC, no service, no
# scheduled task, and you can see and delete it in Explorer. Minimised rather
# than hidden - a companion you cannot find is a companion you cannot
# troubleshoot, and this one has things to say when a port is busy.
$LINK_NAME = 'NEXUS companion.lnk'
# Installed as a copy, not a shortcut to wherever you ran it from. A repo
# unzipped into Downloads gets cleaned up, and an autostart pointing into it
# dies silently the next time you log in.
$HOME_DIR = Join-Path $env:LOCALAPPDATA 'NEXUS'

function Get-StartupLink {
    Join-Path ([Environment]::GetFolderPath('Startup')) $LINK_NAME
}

function Stop-Companions {
    <#  Any other copy of this script. Two of them fight over the port and the
        loser reports "Access is denied", which looks like a driver problem and
        is not. Killed rather than asked to stop: it has no way to be asked,
        and the dongle handles a companion vanishing already - that is what the
        staleness timer is for. #>
    Get-CimInstance Win32_Process `
            -Filter "Name = 'powershell.exe' OR Name = 'pwsh.exe'" |
        Where-Object { $_.CommandLine -like '*nexus_host.ps1*' -and
                       $_.ProcessId -ne $PID } |
        ForEach-Object {
            Write-Output "stopping the copy already running (pid $($_.ProcessId))"
            try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch {}
        }
}

if ($Uninstall) {
    $lnk = Get-StartupLink
    if (Test-Path $lnk) { Remove-Item $lnk; Write-Output "removed $lnk" }
    else { Write-Output 'not installed' }
    Stop-Companions
    if (Test-Path $HOME_DIR) {
        Remove-Item $HOME_DIR -Recurse -Force -ErrorAction SilentlyContinue
        Write-Output "removed $HOME_DIR"
    }
    return
}

if ($Install) {
    Stop-Companions     # so the one started below is the only one
    $script = Join-Path $HOME_DIR 'nexus_host.ps1'
    New-Item -ItemType Directory -Force $HOME_DIR | Out-Null
    if ($PSCommandPath -ne $script) {
        Copy-Item $PSCommandPath $script -Force
    }

    # $launch, not $args: $args is PowerShell's own automatic variable.
    $launch = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized " +
              "-File `"$script`""
    if ($Port) { $launch += " -Port $Port" }

    $lnk = Get-StartupLink
    $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sc.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $sc.Arguments = $launch
    $sc.WorkingDirectory = $HOME_DIR
    $sc.Description = 'Pushes the clock, load and now playing to a NEXUS dongle.'
    $sc.Save()
    Write-Output "installed $lnk"

    # And start it now, because "works after you reboot" is not what anyone
    # means when they ask for this.
    Start-Process powershell -WindowStyle Minimized -ArgumentList $launch
    Write-Output 'running. Open SETTINGS -> COMPANION on the dongle.'
    Write-Output 'undo with -Uninstall.'
    return
}

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

function ConvertTo-Panel([string]$text) {
    # Fold to what the panel's font covers: ASCII 32..90, upper case only.
    # The firmware does this too and does not trust us to - but doing it
    # here keeps the wire carrying only what can be drawn, and means a title
    # reads correctly on firmware built before that fold existed.
    $s = -join ($text.Trim().ToUpper().ToCharArray() |
        Where-Object { [int]$_ -ge 32 -and [int]$_ -le 90 })
    if ($s.Length -gt $TEXT_MAX) { $s = $s.Substring(0, $TEXT_MAX) }
    $s
}

# Title and artist, each already folded, or two empty strings.
function Get-NowPlaying {
    if (-not $script:npReady) { return @('', '') }
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
        if (-not $session) { return @('', '') }

        $propType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
        $p = Wait-WinRt ($session.TryGetMediaPropertiesAsync()) $propType

        # Separately, not "Artist - Title": the panel sets them on two lines,
        # and splitting a joined string back apart breaks on every title
        # that has a dash in it.
        return @((ConvertTo-Panel "$($p.Title)"), (ConvertTo-Panel "$($p.Artist)"))
    } catch {
        # A session can vanish between listing it and asking about it.
        Write-Verbose "now playing: $($_.Exception.Message)"
        return @('', '')
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
            # After T, from the same instant: the dongle files the date
            # against the day that clock counts from.
            $lines.Add('D ' + [int]($now.Date - [datetime]::new(1970, 1, 1)).TotalDays)
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

        $title, $artist = Get-NowPlaying
        $np = "$title|$artist"
        if ($np -ne $lastNp) {
            $lines.Add("N $title")
            $lines.Add("A $artist")
            $lastNp = $np
        }

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
