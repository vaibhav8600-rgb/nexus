<#
.SYNOPSIS
    NEXUS companion for Windows. Needs nothing installed.

.DESCRIPTION
    Pushes the clock, CPU and memory load and what is playing to a NEXUS
    dongle over its host-link USB serial. One way: the dongle never writes
    back, so this cannot type, press keys or read anything out of it.

    The Python companion does the same thing on Linux and macOS, but on
    Windows Python cannot open a COM port without pyserial. Windows can do
    all of this out of the box, and a companion you have to install things
    for is a companion you run once - so this exists.

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
# The per-user Run key: no admin, no UAC, no service, no scheduled task, and it
# is listed under Task Manager -> Startup apps, where it can be switched off
# like anything else. Minimised rather than hidden - a companion you cannot
# find is a companion you cannot troubleshoot, and this one has things to say
# when a port is busy.
#
# Not a shortcut in the Startup folder, which is what this first did. That
# folder is wherever the registry says it is, and on a real machine the
# registry said a temp directory that had since been deleted: .NET returned an
# empty path, the shortcut could not be made, and autostart silently did not
# happen. The Run key has no folder to go missing.
$RUN_KEY = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RUN_NAME = 'NEXUS companion'
# Installed as a copy, not pointed at wherever you ran it from. A repo
# unzipped into Downloads gets cleaned up, and an autostart pointing into it
# dies silently the next time you log in.
$HOME_DIR = Join-Path $env:LOCALAPPDATA 'NEXUS'

# The Startup-folder shortcut an earlier version made, so -Uninstall still
# removes it. Empty when that folder does not exist.
function Get-OldStartupLink {
    $dir = [Environment]::GetFolderPath('Startup')
    if ($dir) { Join-Path $dir 'NEXUS companion.lnk' }
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
    $found = $false
    if (Get-ItemProperty $RUN_KEY -Name $RUN_NAME -ErrorAction SilentlyContinue) {
        Remove-ItemProperty $RUN_KEY -Name $RUN_NAME
        Write-Output 'removed from Startup apps'
        $found = $true
    }
    $lnk = Get-OldStartupLink
    if ($lnk -and (Test-Path $lnk)) {
        Remove-Item $lnk
        Write-Output "removed $lnk"
        $found = $true
    }
    if (-not $found) { Write-Output 'was not set to start with Windows' }
    Stop-Companions
    if (Test-Path $HOME_DIR) {
        Remove-Item $HOME_DIR -Recurse -Force -ErrorAction SilentlyContinue
        Write-Output "removed $HOME_DIR"
    }
    return
}

if ($Install) {
    # Every step or none of them reported as done. The first version carried
    # on past a failed step and printed "installed" under a screen of red.
    try {
        Stop-Companions     # so the one started below is the only one
        $script = Join-Path $HOME_DIR 'nexus_host.ps1'
        New-Item -ItemType Directory -Force $HOME_DIR -ErrorAction Stop | Out-Null
        if ($PSCommandPath -ne $script) {
            Copy-Item $PSCommandPath $script -Force -ErrorAction Stop
        }

        # $launch, not $args: $args is PowerShell's own automatic variable.
        $launch = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized " +
                  "-File `"$script`""
        if ($Port) { $launch += " -Port $Port" }

        $exe = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        Set-ItemProperty $RUN_KEY -Name $RUN_NAME -Value "`"$exe`" $launch" `
            -ErrorAction Stop
        Write-Output 'installed: starts with Windows (Task Manager -> Startup apps)'

        # And start it now, because "works after you reboot" is not what
        # anyone means when they ask for this.
        Start-Process $exe -WindowStyle Minimized -ArgumentList $launch -ErrorAction Stop
    } catch {
        Write-Error "install failed: $($_.Exception.Message)"
        exit 1
    }
    Write-Output 'running. Open SETTINGS -> COMPANION on the dongle.'
    Write-Output 'undo with uninstall.cmd, or -Uninstall.'
    return
}

function Get-NexusPorts {
    <#  ZMK's USB id is 1D50:615E. With Studio built in, the dongle has two
        serial interfaces: ZMK Studio's RPC and the host link. #>
    Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'COM(\d+)' -and
                       $_.DeviceID -match 'VID_1D50&PID_615E' } |
        ForEach-Object {
            [pscustomobject]@{
                Port = [regex]::Match($_.Name, 'COM\d+').Value
                Interface = [int][regex]::Match($_.DeviceID, 'MI_(\d+)').Groups[1].Value
            }
        } | Sort-Object Interface
}

# A ZMK Studio RPC request: SOF 0xAB, Request { request_id: 1, core {
# get_lock_state: true } }, EOF 0xAD. Read-only - it changes nothing on the
# dongle. The newline after it makes the bytes one ignored line if this lands
# on the host link instead.
$STUDIO_PING = [byte[]](0xAB, 0x08, 0x01, 0x1A, 0x02, 0x10, 0x01, 0xAD, 0x0A)

function Test-StudioPort([string]$name) {
    <#  $true if the port answers a Studio request, $false if it stays
        silent, $null if it cannot be opened (Studio connected, usually). #>
    try {
        $p = New-Object System.IO.Ports.SerialPort($name, 115200)
        $p.DtrEnable = $true
        $p.Open()
    } catch {
        return $null
    }
    try {
        Start-Sleep -Milliseconds 200
        $p.DiscardInBuffer()
        $p.Write($STUDIO_PING, 0, $STUDIO_PING.Length)
        $end = (Get-Date).AddMilliseconds(1000)
        while ((Get-Date) -lt $end) {
            if ($p.BytesToRead -gt 0) { return $true }
            Start-Sleep -Milliseconds 50
        }
        return $false
    } catch {
        return $null
    } finally {
        try { $p.Close() } catch {}
    }
}

function Get-HostLinkPort {
    <#  The host link, and only the host link.

        With Studio built in the dongle has two serial ports. Opening both
        held Studio's - Windows gives a port to one program at a time - so
        ZMK Studio could not connect while this ran, and these lines went
        into Studio's RPC channel.

        Which is which cannot be guessed from the order: a guess that the
        host link was the higher interface was wrong on the first real
        dongle it met. So ask. Studio's port answers a Studio request; the
        host link is one way and never answers anything. The port that stays
        silent is ours. A port that will not open is skipped - that is
        Studio with the app connected. With no Studio, the one port is
        silent and is picked. -Port skips all of this. #>
    foreach ($c in Get-NexusPorts) {
        $studio = Test-StudioPort $c.Port
        Write-Verbose "$($c.Port) (MI_$($c.Interface)): studio=$studio"
        if ($studio -eq $false) { return $c.Port }
    }
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

# Title, artist - each already folded - and whether it is playing (1) or
# paused (0). Two empty strings and 0 for nothing.
function Get-NowPlaying {
    if (-not $script:npReady) { return @('', '', 0) }
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
        if (-not $session) { return @('', '', 0) }

        $propType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
        $p = Wait-WinRt ($session.TryGetMediaPropertiesAsync()) $propType

        # Separately, not "Artist - Title": the panel sets them on two lines,
        # and splitting a joined string back apart breaks on every title
        # that has a dash in it.
        $playing = if ($session.GetPlaybackInfo().PlaybackStatus -eq 'Playing') { 1 } else { 0 }
        return @((ConvertTo-Panel "$($p.Title)"), (ConvertTo-Panel "$($p.Artist)"), $playing)
    } catch {
        # A session can vanish between listing it and asking about it.
        Write-Verbose "now playing: $($_.Exception.Message)"
        return @('', '', 0)
    }
}

if ($List) {
    Write-Output 'NEXUS serial ports (VID 1D50, PID 615E):'
    Get-NexusPorts | Format-Table -AutoSize
    foreach ($c in Get-NexusPorts) {
        $studio = Test-StudioPort $c.Port
        $what = if ($studio -eq $true) { 'ZMK Studio (answered a Studio request)' }
                elseif ($studio -eq $false) { 'host link (silent) <- the companion uses this' }
                else { 'busy (Studio connected, or a companion already running)' }
        Write-Output "$($c.Port): $what"
    }
    Write-Output ''
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
        # The host link port only - never Studio's. See Get-HostLinkPort.
        $targets = if ($Port) { @($Port) } else { @(Get-HostLinkPort) }
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
                # Two different waits, and saying which saves a lot of
                # guessing: no dongle at all, or a dongle whose host link
                # port something else is holding.
                if (-not $Port -and @(Get-NexusPorts).Count -gt 0) {
                    Write-Output ('dongle found, but no free host link port - ' +
                                  'is another copy running? (-List shows the ports)')
                } else {
                    Write-Output 'waiting for the dongle'
                }
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

        $title, $artist, $playing = Get-NowPlaying
        $np = "$title|$artist|$playing"
        if ($np -ne $lastNp) {
            $lines.Add("N $title")
            $lines.Add("A $artist")
            # Playing or paused: the dongle moves its level bars only while
            # a track plays. Firmware from before P ignores it.
            $lines.Add("P $playing")
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
