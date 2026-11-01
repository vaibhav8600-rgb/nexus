<#
.SYNOPSIS
    NEXUS companion for Windows. Needs nothing installed.

.DESCRIPTION
    Pushes the clock, CPU and memory load and what is playing to a NEXUS
    dongle: over its host-link USB serial, over the Bluetooth connection this
    PC already has to it, or both. One way: the dongle never writes back, so
    this cannot type, press keys or read anything out of it.

    The Python companion does the same thing on Linux and macOS, but on
    Windows Python cannot open a COM port without pyserial. Windows can do
    all of this out of the box, and a companion you have to install things
    for is a companion you run once - so this exists.

.EXAMPLE
    .\nexus_host.ps1 -Install
    .\nexus_host.ps1 -List
    .\nexus_host.ps1
    .\nexus_host.ps1 -Port COM15 -Verbose
    .\nexus_host.ps1 -Transport Ble
    .\nexus_host.ps1 -Buddy
#>
[CmdletBinding()]
param(
    # Serial port. Found automatically when omitted.
    [string]$Port,
    # How to reach the dongle: its USB serial port, the Bluetooth connection
    # this PC already has to it, or - Auto - whichever of the two is there.
    [ValidateSet('Auto', 'Usb', 'Ble')]
    [string]$Transport = 'Auto',
    # List candidate ports and exit.
    [switch]$List,
    # Start with Windows from now on, and start now. Per-user, no admin.
    [switch]$Install,
    # Undo that.
    [switch]$Uninstall,
    # Seconds between updates.
    [double]$Interval = 1.0,
    # Be Buddy's brain (docs/buddy.md): Gemini with a key, canned lines from
    # persona.json without. The key comes from the GEMINI_API_KEY environment
    # variable or %LOCALAPPDATA%\NEXUS\gemini.json, and a key in that file
    # turns this on by itself. Never an argument: it would sit in the process
    # list.
    [switch]$Buddy,
    # Minutes between unprompted lines, 5-60.
    [ValidateRange(5, 60)]
    [int]$BuddyMinutes = 10,
    # The Gemini model to ask.
    [string]$GeminiModel = 'gemini-3.1-flash-lite'
)

$TEXT_MAX = 39          # NEXUS_HOST_TEXT - 1; the dongle truncates anyway
# Seconds between clock resends. Ten, not sixty: the dongle forgets the time
# whenever it loses power - moved to a charger, to another PC - and over
# Bluetooth this script does not always see that happen, because Windows
# holds a dead link open for several seconds and the dongle can be back
# within them. The link is one way, so nothing can ask for the time again;
# a minute without a clock looked like a host link that had stopped working.
# The dongle draws minutes, so a resend inside the same one costs no repaint.
$CLOCK_EVERY = 10
$NP_EVERY = 10          # seconds between resends of the track, changed or not

# The host link's Bluetooth service (docs/host-link.md): one characteristic,
# written without response.
$BLE_SERVICE = [guid]'bbb8ee45-91c5-40fb-8c33-1c8b9c4795cc'
$BLE_RX = [guid]'40064c5a-dc78-4b9e-b148-9e13bcbd7708'
$BLE_CHUNK = 20         # what every Bluetooth link carries in one write
$BLE_RESCAN = 10        # seconds between looks for the dongle over Bluetooth

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

# Buddy's Gemini key, kept beside the installed copy and never in a repo -
# a key in a file that gets pushed is a public key. A key here is all it
# takes: the companion becomes Buddy's brain without -Buddy.
$KEY_FILE = Join-Path $HOME_DIR 'gemini.json'
$KEY_HELP = 'Paste your key from https://aistudio.google.com/apikey into "key". This file stays on this PC.'

# @{ key; model } - the environment first, then gemini.json. $null for either
# that neither says.
function Get-GeminiConfig {
    $cfg = @{ key = "$env:GEMINI_API_KEY".Trim(); model = $null }
    if (Test-Path $KEY_FILE) {
        try {
            $j = Get-Content $KEY_FILE -Raw -ErrorAction Stop | ConvertFrom-Json
            if (-not $cfg.key -and $j.key) { $cfg.key = "$($j.key)".Trim() }
            if ($j.model) { $cfg.model = "$($j.model)".Trim() }
        } catch {
            Write-Warning "$KEY_FILE is not valid JSON - ignored"
        }
    }
    $cfg
}

# Buddy's persona: beside the script once installed, else the repo's own copy.
function Get-PersonaPath {
    foreach ($p in @((Join-Path $PSScriptRoot 'persona.json'),
                     (Join-Path $PSScriptRoot '..\..\persona\persona.json'))) {
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
}

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
        # Everything but the Gemini key: reinstalling should not mean
        # finding and pasting it again. Delete gemini.json by hand to forget it.
        Get-ChildItem $HOME_DIR -Force | Where-Object { $_.FullName -ne $KEY_FILE } |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $KEY_FILE) {
            Write-Output "removed $HOME_DIR, except your Gemini key ($KEY_FILE)"
        } else {
            Remove-Item $HOME_DIR -Recurse -Force -ErrorAction SilentlyContinue
            Write-Output "removed $HOME_DIR"
        }
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
        # Buddy's persona goes beside it, the way it is looked for. The
        # Gemini key is not copied anywhere: it stays in the environment.
        $persona = Get-PersonaPath
        if ($persona -and $persona -ne (Join-Path $HOME_DIR 'persona.json')) {
            Copy-Item $persona (Join-Path $HOME_DIR 'persona.json') -Force -ErrorAction Stop
        }
        # An empty key file to paste into, once. One that exists is left
        # alone - it may hold a key.
        if (-not (Test-Path $KEY_FILE)) {
            @{ key = ''; help = $KEY_HELP } | ConvertTo-Json |
                Set-Content $KEY_FILE -Encoding ascii -ErrorAction Stop
            Write-Output "Gemini key file: $KEY_FILE (paste your key, then run this again)"
        }

        # $launch, not $args: $args is PowerShell's own automatic variable.
        $launch = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized " +
                  "-File `"$script`""
        if ($Port) { $launch += " -Port $Port" }
        if ($Transport -ne 'Auto') { $launch += " -Transport $Transport" }
        if ($Buddy) {
            $launch += " -Buddy -BuddyMinutes $BuddyMinutes -GeminiModel $GeminiModel"
        }

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

function Get-Reason($err) {
    <#  What actually went wrong, in its own words. PowerShell wraps a .NET
        failure in 'Exception calling "WriteLine" with "1" argument(s): ...'
        and a task's in "One or more errors occurred", and the one sentence
        worth reading - "The port is closed." - is at the bottom of the pile. #>
    $e = $err.Exception
    while ($e.InnerException) { $e = $e.InnerException }
    $e.Message
}

function Wait-WinRt($operation, $type, $ms = 2000) {
    $task = $script:asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    try {
        $done = $task.Wait($ms)
    } catch {
        throw (Get-Reason $_)
    }
    if (-not $done) { throw 'no answer in time' }
    $task.Result
}

# ---- Bluetooth ------------------------------------------------------------
#
# The dongle is already paired to this PC as a keyboard. With the host link's
# Bluetooth transport built in, it also has one small service to write these
# lines to - so the HOST screen works when the dongle is on a charger and
# reaches this PC over the air alone. Nothing is paired, connected or scanned
# for here: only devices Windows is connected to already are looked at.
#
# The same WinRT that names the track, so still nothing to install.
$script:bleReady = $false
if ($script:asTask) {
    try {
        [Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
        [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
        [Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
        $script:bleReady = $true
    } catch {
        Write-Verbose "bluetooth unavailable: $($_.Exception.Message)"
    }
}

# Devices already asked over the air this run. See Find-BleLink.
$script:bleAsked = @{}
# The service was there and Windows would not hand it over. See
# Get-BleCharacteristic.
$script:bleBusy = $false

function Get-BleConnected {
    <#  Every Bluetooth LE device Windows is connected to right now. #>
    $le = [Windows.Devices.Bluetooth.BluetoothLEDevice]
    $sel = $le::GetDeviceSelectorFromConnectionStatus(
        [Windows.Devices.Bluetooth.BluetoothConnectionStatus]::Connected)
    $found = Wait-WinRt ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($sel)) `
        ([Windows.Devices.Enumeration.DeviceInformationCollection]) 5000
    @($found)
}

function Get-BleCharacteristic($dev, [string]$mode) {
    <#  The host link on $dev, as @{ Service; Rx } - its service and the
        characteristic to write to - or nothing. $mode is Cached - what
        Windows remembers, no radio - or Uncached, which asks the device.

        Whoever gets a service back has to Dispose it. Windows keeps a
        service reserved for the object that opened it, for as long as that
        object lives - and in PowerShell that is until the garbage collector
        happens by, not until the variable is gone. A service left behind
        locks this very process out of it: the next look finds the service
        and is refused its characteristic, forever, which is how a companion
        that had lost its Bluetooth link once never got it back. So a
        service that is not being returned is closed here.

        Windows gives a Bluetooth service to one program at a time, as it
        does a serial port: with a companion already running, the service is
        found and its characteristic is AccessDenied. That is a different
        answer from "this firmware has no such service", and reporting the
        one as the other sends you off to re-pair a dongle that is working -
        so it is remembered in $script:bleBusy for whoever asked. #>
    $gatt = 'Windows.Devices.Bluetooth.GenericAttributeProfile'
    $cache = [Windows.Devices.Bluetooth.BluetoothCacheMode]::$mode
    $svcs = Wait-WinRt ($dev.GetGattServicesForUuidAsync($BLE_SERVICE, $cache)) `
        ([type]"$gatt.GattDeviceServicesResult") 8000
    if ("$($svcs.Status)" -ne 'Success' -or $svcs.Services.Count -eq 0) { return }
    $svc = $svcs.Services[0]
    $rx = $null
    try {
        $chars = Wait-WinRt ($svc.GetCharacteristicsForUuidAsync($BLE_RX, $cache)) `
            ([type]"$gatt.GattCharacteristicsResult") 8000
        if ("$($chars.Status)" -eq 'AccessDenied') { $script:bleBusy = $true }
        if ("$($chars.Status)" -eq 'Success' -and $chars.Characteristics.Count -gt 0) {
            $rx = $chars.Characteristics[0]
        }
    } finally {
        if (-not $rx) { try { $svc.Dispose() } catch {} }
    }
    if ($rx) { @{ Service = $svc; Rx = $rx } }
}

function Find-BleLink {
    <#  A connected Bluetooth device with the host link's service, as
        @{ Name; Device; Service; Rx } - or nothing. Close-BleLink gives it
        back.

        What Windows remembers of a device is asked first: no radio, so
        looking again every few seconds disturbs nobody's mouse. Windows
        remembers a paired device's services from when it paired, though, and
        a dongle reflashed since then has one more - so each device is also
        asked over the air, once per run. #>
    if (-not $script:bleReady) { return }
    $script:bleBusy = $false
    foreach ($d in Get-BleConnected) {
        $dev = $null
        $got = $null
        try {
            $le = [Windows.Devices.Bluetooth.BluetoothLEDevice]
            $dev = Wait-WinRt ($le::FromIdAsync($d.Id)) $le 5000
            if ($dev) {
                $got = Get-BleCharacteristic $dev 'Cached'
                if (-not $got -and -not $script:bleAsked[$d.Id]) {
                    $script:bleAsked[$d.Id] = $true
                    $got = Get-BleCharacteristic $dev 'Uncached'
                }
            }
        } catch {
            Write-Verbose "bluetooth $($d.Name): $(Get-Reason $_)"
        }
        if ($got) {
            return @{ Name = $d.Name; Device = $dev; Service = $got.Service; Rx = $got.Rx }
        }
        if ($dev) { try { $dev.Dispose() } catch {} }
    }
}

function Send-BleLines($link, $lines) {
    <#  The same lines the serial port gets, in writes of $BLE_CHUNK bytes:
        the dongle assembles lines from a stream, wherever the cuts fall.
        Throws when the link is gone. #>
    $bytes = [Text.Encoding]::ASCII.GetBytes((@($lines) -join "`n") + "`n")
    $option = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteOption]::WriteWithoutResponse
    $status = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]
    for ($at = 0; $at -lt $bytes.Length; $at += $BLE_CHUNK) {
        $n = [Math]::Min($BLE_CHUNK, $bytes.Length - $at)
        $part = New-Object byte[] $n
        [Array]::Copy($bytes, $at, $part, 0, $n)
        # AsBuffer, not CryptographicBuffer: PowerShell hands that one's
        # result over as a bare COM object and then cannot find the
        # WriteValueAsync overload that takes it.
        $buf = [System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($part)
        $result = Wait-WinRt ($link.Rx.WriteValueAsync($buf, $option)) $status
        # Unreachable is the ordinary one: the dongle was unplugged, reset,
        # or this PC is on its way to sleep.
        if ("$result" -ne 'Success') { throw "the dongle did not take the write ($result)" }
    }
}

function Close-BleLink($link) {
    <#  Give the service back, then the device. Both, and at once: see
        Get-BleCharacteristic for what a service left open costs. The
        collection after it is for the objects PowerShell made along the way
        and still holds - a result list, the characteristic - which keep
        their own grip on the service until they are finalised. #>
    if (-not $link) { return }
    foreach ($part in 'Service', 'Device') {
        if ($link[$part]) { try { $link[$part].Dispose() } catch {} }
    }
    $link.Rx = $null
    $link.Service = $null
    $link.Device = $null
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
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

# ---- Buddy's brain -----------------------------------------------------------
#
# The same rules as the Python companion's Brain: when to speak, what Gemini
# is told, and what is never sent to the dongle. Gemini hears the time of day,
# how busy this PC is (low / medium / high), the track and why it is being
# asked - never keystrokes, windows or files.

$GEMINI_URL = 'https://generativelanguage.googleapis.com/v1beta/models/{0}:generateContent'
$LINE_MAX = 24        # the persona's limit, and what the bubble fits
$ASK_GAP = 20         # seconds between Gemini requests, at the least
$LEASE_EVERY = 10     # B 1 resend; the dongle hands the face back after 60
$SONG_GAP = 300       # one remark per five minutes about the music, at most
$BACKOFF_MAX = 1800
$REASONS = @{
    hello = 'the owner just sat down at the computer; greet them'
    tick  = 'a few quiet minutes have passed; say something small'
    music = 'a new song just started'
    busy  = 'the computer has been very busy for a minute'
    late  = 'it is late at night'
}

function ConvertTo-BuddyLine([string]$text) {
    $s = -join ($text.Trim().ToUpper().ToCharArray() |
        Where-Object { [int]$_ -ge 32 -and [int]$_ -le 90 })
    if ($s.Length -gt $LINE_MAX) { $s = $s.Substring(0, $LINE_MAX) }
    $s
}

function Get-BuddyPrompt($p) {
    $v = $p.voice
    $owner = if ($p.owner) { $p.owner } else { 'the owner' }
    $out = @("You are $($p.name), a tiny animated face on $owner's keyboard dongle. Pronoun: $($p.pronoun).", 'Traits:')
    $out += $p.traits | ForEach-Object { "- $_" }
    $out += "Voice: $($v.style). A line is at most $($v.max_chars) characters, $($v.case) case, $($v.charset)."
    $out += 'Never:'
    $out += $p.never | ForEach-Object { "- $_" }
    $out += 'Reply only as JSON matching the schema: "emotion" is how the face looks, "line" goes on the dongle, "reply" is a slightly longer version for a screen.'
    $out -join "`n"
}

# (emotion, line) from a generateContent response, or $null for anything the
# dongle should not be sent: bad JSON, an emotion it does not have, a line too
# long or with characters the font cannot draw.
function Read-GeminiReply([string]$raw, $p) {
    try {
        $text = ($raw | ConvertFrom-Json).candidates[0].content.parts[0].text
        $obj = $text | ConvertFrom-Json
        $emotion = "$($obj.emotion)".ToLower()
        $line = "$($obj.line)".Trim()
    } catch { return $null }
    if ($p.emotions -notcontains $emotion) { return $null }
    if (-not $line -or $line.Length -gt $LINE_MAX -or
        (ConvertTo-BuddyLine $line) -ne $line.ToUpper()) { return $null }
    return @($emotion, $line.ToUpper())
}

function Get-Bucket($pct) {
    if ($null -eq $pct) { 'unknown' } elseif ($pct -lt 40) { 'low' }
    elseif ($pct -lt 80) { 'medium' } else { 'high' }
}

function Get-Canned([string]$reason) {
    $pool = $script:persona.canned.$reason
    if (-not $pool) { $pool = $script:persona.canned.tick }
    # By index: piping the [emotion, line] pairs unrolls them into strings.
    $idx = @(0..($pool.Count - 1) | Where-Object { $pool[$_][1] -ne $script:bud.said })
    if ($idx.Count -eq 0) { $idx = @(0..($pool.Count - 1)) }
    $c = $pool[($idx | Get-Random)]
    @($c[0], (ConvertTo-BuddyLine $c[1]))
}

function Write-BuddyWarning([string]$kind, [string]$text) {
    # Once per kind of failure. Never the key.
    if (-not $script:bud.complained.ContainsKey($kind)) {
        $script:bud.complained[$kind] = $true
        Write-Warning $text
    }
}

# Ask Gemini in a second runspace, so the round that sends C and M never waits
# on an answer: a HOST screen gone stale for a slow reply is a regression.
function Start-BuddyAsk([string]$reason, $cpu, $mem, $np) {
    $now = Get-Date
    $ctx = @("Why: $($REASONS[$reason]).")
    $h = $now.Hour
    $part = if ($h -lt 5 -or $h -ge 23) { 'night' } elseif ($h -lt 12) { 'morning' }
            elseif ($h -lt 18) { 'afternoon' } else { 'evening' }
    $ctx += "Time of day: $part."
    $ctx += "Computer load: CPU $(Get-Bucket $cpu), memory $(Get-Bucket $mem)."
    $ctx += if ($np[2] -and $np[0]) { "Now playing: $($np[0]) by $(if ($np[1]) { $np[1] } else { 'unknown' })." }
            else { 'Now playing: nothing.' }
    if ($script:bud.saidAt) {
        $mins = [int](($now - $script:bud.saidAt).TotalMinutes)
        $ctx += "Minutes since your last line: $mins. It was `"$($script:bud.said)`"; do not repeat it."
    }
    $body = @{
        systemInstruction = @{ parts = @(@{ text = (Get-BuddyPrompt $script:persona) }) }
        contents = @(@{ role = 'user'; parts = @(@{ text = ($ctx -join "`n") }) })
        generationConfig = @{
            responseMimeType = 'application/json'
            responseSchema = @{
                type = 'OBJECT'
                properties = @{
                    emotion = @{ type = 'STRING'; enum = @($script:persona.emotions | ForEach-Object { $_.ToUpper() }) }
                    line = @{ type = 'STRING' }
                    reply = @{ type = 'STRING' }
                }
                required = @('emotion', 'line', 'reply')
            }
            temperature = 1.0
            maxOutputTokens = 200
        }
    } | ConvertTo-Json -Depth 10 -Compress

    $ps = [powershell]::Create()
    [void]$ps.AddScript({
        param($url, $key, $json)
        try {
            $r = Invoke-WebRequest -UseBasicParsing -Uri $url -Method Post `
                -Headers @{ 'x-goog-api-key' = $key } `
                -ContentType 'application/json; charset=utf-8' `
                -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 15
            @{ ok = $true; raw = $r.Content }
        } catch {
            $code = 0
            try { $code = [int]$_.Exception.Response.StatusCode } catch {}
            @{ ok = $false; code = $code; err = $_.Exception.Message }
        }
    }).AddArgument(($GEMINI_URL -f $GeminiModel)).AddArgument($script:bud.key).AddArgument($body)
    $script:bud.job = @{ ps = $ps; handle = $ps.BeginInvoke(); reason = $reason }
}

function Receive-BuddyAsk {
    $job = $script:bud.job
    $res = $job.ps.EndInvoke($job.handle) | Select-Object -First 1
    $job.ps.Dispose()
    $script:bud.job = $null
    $got = $null
    if ($res -and $res.ok) {
        $got = Read-GeminiReply $res.raw $script:persona
        $script:bud.backoff = 0
    } elseif ($res -and $res.code -eq 429) {
        # Out of quota: rest, longer each time, and say so once.
        $script:bud.backoff = [math]::Min([math]::Max(60, $script:bud.backoff * 2), $BACKOFF_MAX)
        $script:bud.nextAsk = (Get-Date).AddSeconds($script:bud.backoff)
        Write-BuddyWarning 'quota' "Gemini quota reached - $($script:persona.name) is resting for $($script:bud.backoff) s"
    } else {
        $script:bud.nextAsk = (Get-Date).AddSeconds(120)
        $why = if ($res) { if ($res.code) { "HTTP $($res.code)" } else { $res.err } } else { 'no answer' }
        Write-BuddyWarning "$why" "Gemini unreachable: $why"
    }
    if (-not $got) { $got = Get-Canned $job.reason }
    $got
}

function Get-BuddyTrigger($cpu, $np) {
    $now = Get-Date
    if (-not $script:bud.saidAt) { return 'hello' }
    $song = "$($np[0])|$($np[1])"
    if ($np[2] -and $np[0] -and $song -ne $script:bud.song) {
        $script:bud.song = $song
        if (($now - $script:bud.songAt).TotalSeconds -ge $SONG_GAP) {
            $script:bud.songAt = $now
            return 'music'
        }
    }
    if ($null -ne $cpu -and $cpu -ge 90) {
        if (-not $script:bud.busySince) { $script:bud.busySince = $now }
        if (($now - $script:bud.busySince).TotalSeconds -ge 60 -and
            ($now - $script:bud.busyAt).TotalSeconds -ge 600) {
            $script:bud.busyAt = $now
            return 'busy'
        }
    } else {
        $script:bud.busySince = $null
    }
    if (($now - $script:bud.saidAt).TotalMinutes -ge $BuddyMinutes) {
        if ($now.Hour -ge 23 -or $now.Hour -lt 5) { return 'late' }
        return 'tick'
    }
    $null
}

# Buddy's lines for this round, in the same write as the rest.
function Get-BuddyLines($cpu, $mem, $np) {
    $out = New-Object System.Collections.Generic.List[string]
    $now = Get-Date
    if (($now - $script:bud.leaseAt).TotalSeconds -ge $LEASE_EVERY) {
        $out.Add('B 1')
        $script:bud.leaseAt = $now
    }
    $got = $null
    if ($script:bud.job -and $script:bud.job.handle.IsCompleted) { $got = Receive-BuddyAsk }
    if (-not $script:bud.job -and -not $got -and $now -ge $script:bud.nextAsk) {
        $reason = Get-BuddyTrigger $cpu $np
        if ($reason) {
            if (-not $script:bud.saidAt) { $script:bud.saidAt = $now }
            $script:bud.nextAsk = $now.AddSeconds($ASK_GAP)
            if ($script:bud.key) {
                Start-BuddyAsk $reason $cpu $mem $np
                $out.Add('E THINKING')
            } else {
                $got = Get-Canned $reason
            }
        }
    }
    if ($got) {
        $script:bud.said = $got[1]
        $script:bud.saidAt = $now
        $out.Add('E ' + $got[0].ToUpper())
        $out.Add('S ' + $got[1])
    }
    , $out
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
    Write-Output 'Connected Bluetooth LE devices:'
    if (-not $script:bleReady) {
        Write-Output '  (Bluetooth is not available to this PowerShell)'
    } else {
        $any = $false
        foreach ($d in Get-BleConnected) {
            $any = $true
            $has = $false
            $script:bleBusy = $false
            try {
                $le = [Windows.Devices.Bluetooth.BluetoothLEDevice]
                $dev = Wait-WinRt ($le::FromIdAsync($d.Id)) $le 5000
                if ($dev) {
                    $got = Get-BleCharacteristic $dev 'Uncached'
                    $has = [bool]$got
                    Close-BleLink @{ Device = $dev; Service = $(if ($got) { $got.Service }) }
                }
            } catch {}
            $what = if ($has) { 'host link service <- the companion uses this' }
                    elseif ($script:bleBusy) { 'host link service, in use (a companion is already running)' }
                    else { 'no host link service' }
            Write-Output "  $($d.Name): $what"
        }
        if (-not $any) { Write-Output '  none' }
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
$useUsb = $Transport -ne 'Ble'
$useBle = $Transport -ne 'Usb'
if ($useBle -and -not $script:bleReady) {
    Write-Warning 'Bluetooth is not available to this PowerShell; USB only.'
    $useBle = $false
}
$open = @{}
$ble = $null
$script:moaned = @{}      # ports already complained about, so it is said once
$script:waiting = $false
$nextUsb = [datetime]::MinValue
$nextBle = [datetime]::MinValue
$lastClock = [datetime]::MinValue
$lastNp = $null
$lastNpAt = [datetime]::MinValue

# Buddy's state, kept whether or not it is on: a key pasted into gemini.json
# while this runs turns it on at the next look, with no restart.
$script:bud = @{
    key = ''; job = $null; said = ''; saidAt = $null
    nextAsk = [datetime]::MinValue; leaseAt = [datetime]::MinValue; backoff = 0
    song = ''; songAt = [datetime]::MinValue; busySince = $null
    busyAt = [datetime]::MinValue; complained = @{}
}
$nextKeyLook = [datetime]::MinValue
$modelGiven = $PSBoundParameters.ContainsKey('GeminiModel')

# Look for a key: at start, then every 30 s until there is one. Never printed.
function Update-BuddyKey {
    $gem = Get-GeminiConfig
    if (-not $gem.key -or $gem.key -eq $script:bud.key) { return }
    $script:bud.key = $gem.key
    if ($gem.model -and -not $modelGiven) { $script:GeminiModel = $gem.model }
    if (-not $script:Buddy) {
        $script:Buddy = [switch]::Present
        $script:bud.leaseAt = [datetime]::MinValue
    }
    Write-Output "buddy: Gemini ($script:GeminiModel)"
}

Update-BuddyKey
if ($Buddy) {
    $script:persona = Get-Content (Get-PersonaPath) -Raw -ErrorAction Stop | ConvertFrom-Json
    if (-not $script:bud.key) {
        Write-Output "buddy: canned lines - put a Gemini key in $KEY_FILE for Gemini"
    }
}

try {
  # Reconnect rather than exit. The dongle disappears from USB on every
  # reflash and every reboot, and from Bluetooth whenever this PC sleeps, and
  # a companion that has to be restarted by hand each time is a companion you
  # stop bothering with. Each way in is looked for on its own clock, so one
  # being there does not stop the other being found.
  while ($true) {
    $fresh = $false

    if ($useUsb -and $open.Count -eq 0 -and (Get-Date) -ge $nextUsb) {
        # Every 3 s while there is nothing at all, so a replugged dongle is
        # picked up quickly. While Bluetooth is carrying the data there is no
        # hurry, and asking Windows for its serial ports is not free: every
        # $BLE_RESCAN seconds, the same as the other way round.
        $nextUsb = (Get-Date).AddSeconds($(if ($ble) { $BLE_RESCAN } else { 3 }))
        # The host link port only - never Studio's. See Get-HostLinkPort.
        $targets = if ($Port) { @($Port) } else { @(Get-HostLinkPort) }
        foreach ($name in $targets) {
            try {
                $sp = New-Object System.IO.Ports.SerialPort($name, 115200)
                $sp.WriteTimeout = 2000
                $sp.DtrEnable = $true
                $sp.Open()
                $open[$name] = $sp
                $fresh = $true
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
                    Write-Warning "$name : $(Get-Reason $_)"
                    $script:moaned[$name] = $true
                }
            }
        }
    }

    if ($useBle -and -not $ble -and (Get-Date) -ge $nextBle) {
        # Every $BLE_RESCAN seconds, not every round: firmware without the
        # Bluetooth transport never has the service, and asking once a
        # second for something that is not there is a busy loop.
        $nextBle = (Get-Date).AddSeconds($BLE_RESCAN)
        $ble = Find-BleLink
        if ($ble) {
            $fresh = $true
            Write-Output "open bluetooth $($ble.Name)"
        }
    }

    if ($open.Count -eq 0 -and -not $ble) {
        if (-not $script:waiting) {
            # Two different waits, and saying which saves a lot of
            # guessing: no dongle at all, or a dongle whose host link
            # port something else is holding.
            if ($useUsb -and -not $Port -and @(Get-NexusPorts).Count -gt 0) {
                Write-Output ('dongle found, but no free host link port - ' +
                              'is another copy running? (-List shows the ports)')
            } elseif ($script:bleBusy) {
                Write-Output ('dongle found over Bluetooth, but its host link service ' +
                              'is in use - is another copy running?')
            } else {
                Write-Output 'waiting for the dongle'
            }
            $script:waiting = $true
        }
        Start-Sleep -Seconds 1
        continue
    }
    $script:waiting = $false
    if ($fresh) {
        $script:moaned = @{}
        # A fresh link knows nothing about us: resend everything.
        $lastClock = [datetime]::MinValue
        $lastNp = $null
        if ($Buddy) { $script:bud.leaseAt = [datetime]::MinValue }
    }

    $lines = New-Object System.Collections.Generic.List[string]
    $cpu = $null
    $used = $null

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
    # When it changes, and every $NP_EVERY seconds whether it has or not. The
    # link is one way: when you switch the keyboard to another host and back,
    # the dongle forgets the track, and nothing can tell this script to say
    # it again. The dongle ignores a value it already has, so the resend
    # costs it no repaint.
    if ($np -ne $lastNp -or ((Get-Date) - $lastNpAt).TotalSeconds -ge $NP_EVERY) {
        $lines.Add("N $title")
        $lines.Add("A $artist")
        # Playing or paused: the dongle moves its level bars only while
        # a track plays. Firmware from before P ignores it.
        $lines.Add("P $playing")
        $lastNp = $np
        $lastNpAt = Get-Date
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

    if (-not $script:bud.key -and (Get-Date) -ge $nextKeyLook) {
        $nextKeyLook = (Get-Date).AddSeconds(30)
        Update-BuddyKey
        if ($Buddy -and -not $script:persona) {
            $script:persona = Get-Content (Get-PersonaPath) -Raw -ErrorAction Stop | ConvertFrom-Json
        }
    }

    # Buddy's lines go in the same write as the rest, so they share a packet.
    if ($Buddy) {
        foreach ($l in (Get-BuddyLines $cpu $used @($title, $artist, $playing))) {
            $lines.Add($l)
        }
    }

    foreach ($name in @($open.Keys)) {
        foreach ($line in $lines) {
            try {
                $open[$name].WriteLine($line)
                Write-Verbose "$name > $line"
            } catch {
                Write-Warning "$name lost: $(Get-Reason $_)"
                try { $open[$name].Close() } catch {}
                $open.Remove($name)
                break
            }
        }
    }

    # To Bluetooth as well when both are there. The dongle listens to the
    # one that matches where its keys are going, and drops the other.
    if ($ble) {
        try {
            Send-BleLines $ble $lines
            Write-Verbose "bluetooth > $($lines -join ' | ')"
        } catch {
            Write-Warning "bluetooth $($ble.Name) lost: $(Get-Reason $_)"
            Close-BleLink $ble
            $ble = $null
            # Look again at the next rescan, not at once: a PC going to
            # sleep fails every write until it is awake.
            $nextBle = (Get-Date).AddSeconds($BLE_RESCAN)
        }
    }

    if ($open.Count -eq 0 -and -not $ble) {
        # The dongle went away. Say so once, then wait for it to come back.
        Write-Output 'link lost; waiting for the dongle'
        $script:waiting = $true
    }
    Start-Sleep -Milliseconds ([int]($Interval * 1000))
  }
} finally {
    # "I am going away", so the dongle shows dashes rather than numbers that
    # stopped being true the moment this stopped - and B 0 first, so the face
    # is Buddy's own again now rather than in a minute.
    $bye = if ($Buddy) { @('B 0', 'X') } else { @('X') }
    foreach ($name in @($open.Keys)) {
        try {
            foreach ($l in $bye) { $open[$name].WriteLine($l) }
            $open[$name].Close()
        } catch {}
    }
    if ($ble) {
        try { Send-BleLines $ble $bye } catch {}
        Close-BleLink $ble
    }
    Write-Output 'stopped'
}
