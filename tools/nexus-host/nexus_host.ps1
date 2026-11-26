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
    # Talk to Buddy through the copy already running (chat.cmd).
    [switch]$Chat,
    # Talk to Buddy out loud through Gemini Live (voice.cmd), with this PC's
    # mic and speakers; the face on the dongle listens and moves its mouth.
    [switch]$Voice,
    # With -Voice: open the window without starting to listen. How it opens
    # at sign-in, so the mic is never on by itself.
    [switch]$Idle,
    # With -Voice: render the window's images into this folder and exit
    # (docs/images/voice is made this way).
    [string]$Shot,
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
# Buddy's window too, opened quiet (-Idle) at sign-in. Its own entry, so it
# can be switched off - in Startup apps, or the window's settings - without
# losing the companion.
$WINDOW_RUN_NAME = 'NEXUS Buddy'
# Installed as a copy, not pointed at wherever you ran it from. A repo
# unzipped into Downloads gets cleaned up, and an autostart pointing into it
# dies silently the next time you log in.
$HOME_DIR = Join-Path $env:LOCALAPPDATA 'NEXUS'

# Buddy's Gemini key, kept beside the installed copy and never in a repo -
# a key in a file that gets pushed is a public key. A key here is all it
# takes: the companion becomes Buddy's brain without -Buddy.
$KEY_FILE = Join-Path $HOME_DIR 'gemini.json'
$KEY_HELP = 'Paste your key from https://aistudio.google.com/apikey into "key". This file stays on this PC.'

# chat.cmd talks to this copy on a localhost UDP port. Bound to 127.0.0.1, so
# nothing off this PC can reach it.
$CHAT_PORT = 47801
$CHAT_MAX = 500       # characters of a message kept
# voice.cmd sends the face's lines here, the same way and as private.
$VOICE_PORT = 47802
# And the companion copies its own Buddy lines to the voice window here, so
# the face on the PC matches the dongle's.
$MIRROR_PORT = 47803
# Gemini Live, for -Voice. gemini.json's "liveModel" and "voice" override
# these, so a renamed model needs no edit here.
$LIVE_MODEL = 'gemini-3.8-live'
# Soft and calm. Other gentle ones: Vindemiatrix, Sulafat, Despina, Aoede.
$LIVE_VOICE = 'Achernar'
# Talk on and off from anywhere: Ctrl+Alt+Shift+N (MOD_ALT | MOD_CONTROL |
# MOD_SHIFT, VK 'N'). Not Ctrl+Alt+N: a global hotkey wins over every app,
# and that one is VS Code's Run Code (and AltGr+N on some layouts).
# ponytail: fixed; a "hotkey" setting when it clashes with something.
$TALK_MODS = 7
$TALK_VK = 0x4E
$TALK_KEYS = 'Ctrl+Alt+Shift+N'
# Held, not pressed: dictate anywhere (Ctrl+Alt+Shift+D - the keyboard's
# hold of /) and "what's this?" about a selection (Ctrl+Alt+Shift+E - hold
# of Z, the left hand, free while the right one selects with the mouse).
$DICTATE_VK = 0x44
$DICTATE_KEYS = 'Ctrl+Alt+Shift+D'
$EXPLAIN_VK = 0x45
$EXPLAIN_KEYS = 'Ctrl+Alt+Shift+E'

# What Buddy has been asked to remember, from one day to the next: on this PC
# only, and read into each conversation's prompt (BuddyVoice's Memory).
$MEMORY_FILE = Join-Path $HOME_DIR 'memory.json'

# @{ key; model; liveModel; voice; headset; city } - the environment first,
# then gemini.json. $null for any that neither says.
function Get-GeminiConfig {
    $cfg = @{ key = "$env:GEMINI_API_KEY".Trim(); model = $null; liveModel = $null; voice = $null; headset = $false; city = $null }
    if (Test-Path $KEY_FILE) {
        try {
            $j = Get-Content $KEY_FILE -Raw -ErrorAction Stop | ConvertFrom-Json
            if (-not $cfg.key -and $j.key) { $cfg.key = "$($j.key)".Trim() }
            if ($j.model) { $cfg.model = "$($j.model)".Trim() }
            if ($j.liveModel) { $cfg.liveModel = "$($j.liveModel)".Trim() }
            if ($j.voice) { $cfg.voice = "$($j.voice)".Trim() }
            if ($j.headset -eq $true) { $cfg.headset = $true }
            if ($j.city) { $cfg.city = "$($j.city)".Trim() }
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
                       $_.CommandLine -notlike '*-Chat*' -and
                       $_.CommandLine -notlike '*-Voice*' -and
                       $_.ProcessId -ne $PID } |
        ForEach-Object {
            Write-Output "stopping the copy already running (pid $($_.ProcessId))"
            try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch {}
        }
}

if ($Chat) {
    # One message out, one answer back, through the running copy's localhost
    # inbox. Connected, so a companion that is not running shows up as a
    # reset rather than a silent wait.
    $udp = New-Object Net.Sockets.UdpClient(
        (New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, 0)))
    $udp.Client.ReceiveTimeout = 45000
    $udp.Connect([Net.IPAddress]::Loopback, $CHAT_PORT)
    Write-Output 'Talk to Buddy. An empty line ends it.'
    while ($true) {
        $text = Read-Host 'you'
        if (-not "$text".Trim()) { break }
        $bytes = [Text.Encoding]::UTF8.GetBytes((@{ text = $text } | ConvertTo-Json -Compress))
        [void]$udp.Send($bytes, $bytes.Length)
        try {
            $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
            $j = [Text.Encoding]::UTF8.GetString($udp.Receive([ref]$ep)) | ConvertFrom-Json
            Write-Output "$($j.name) ($("$($j.emotion)".ToLower())): $($j.reply)"
        } catch {
            if ("$($_.Exception.GetBaseException().SocketErrorCode)" -eq 'ConnectionReset') {
                Write-Output 'The companion is not running - start it with install.cmd.'
            } else {
                Write-Output 'No answer in time - Gemini may be slow or out of quota. Try again in a minute.'
            }
        }
    }
    $udp.Close()
    return
}

# Buddy's window opening at sign-in: on or off. The installed copy when there
# is one - an autostart pointing into a repo dies when the repo moves.
function Set-WindowAutostart([bool]$on) {
    if (-not $on) {
        Remove-ItemProperty $RUN_KEY -Name $WINDOW_RUN_NAME -ErrorAction SilentlyContinue
        return
    }
    $script = Join-Path $HOME_DIR 'nexus_host.ps1'
    if (-not (Test-Path $script)) { $script = $PSCommandPath }
    $exe = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    Set-ItemProperty $RUN_KEY -Name $WINDOW_RUN_NAME -ErrorAction Stop -Value (
        "`"$exe`" -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`" -Voice -Idle")
}

function Test-WindowAutostart {
    [bool](Get-ItemProperty $RUN_KEY -Name $WINDOW_RUN_NAME -ErrorAction SilentlyContinue)
}

if ($Uninstall) {
    $found = $false
    if (Test-WindowAutostart) {
        Set-WindowAutostart $false
        Write-Output "removed Buddy's window from Startup apps"
    }
    # The window too: it holds files in the folder about to go.
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'pwsh.exe'" |
        Where-Object { $_.CommandLine -like '*nexus_host.ps1*-Voice*' -and $_.ProcessId -ne $PID } |
        ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch {} }
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

        # Buddy's window: at sign-in from now on, quiet, and now. A window
        # already open is brought forward rather than doubled.
        Set-WindowAutostart $true
        Write-Output "installed: Buddy's window opens at sign-in (switch it off in its settings)"
        Start-Process $exe -WindowStyle Hidden -ErrorAction Stop -ArgumentList (
            "-NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`" -Voice -Idle")
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
    chat  = 'the owner is talking to you; answer them'
}
$HISTORY = 12         # turns of conversation Gemini is shown, six each way
$STEP_MS = 1800       # between a trick's steps

function ConvertTo-BuddyLine([string]$text) {
    $s = -join ($text.Trim().ToUpper().ToCharArray() |
        Where-Object { [int]$_ -ge 32 -and [int]$_ -le 90 })
    if ($s.Length -gt $LINE_MAX) { $s = $s.Substring(0, $LINE_MAX) }
    $s
}

# The remembered facts, oldest first; none when there is no file.
function Get-BuddyMemory {
    try { @(Get-Content $MEMORY_FILE -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json) | Where-Object { $_ } } catch {}
}

# -Spoken: for a voice conversation, which talks rather than writing lines.
# Its face is read from what it says (BuddyVoice.FaceFor), not asked for.
function Get-BuddyPrompt($p, [switch]$Spoken) {
    $v = $p.voice
    $owner = if ($p.owner) { $p.owner } else { 'the owner' }
    $out = @("You are $($p.name), a tiny animated face on $owner's keyboard dongle. Pronoun: $($p.pronoun).", 'Traits:')
    $out += $p.traits | ForEach-Object { "- $_" }
    if ($Spoken) {
        $out += "Voice: $($v.spoken). You are talking out loud with ${owner}: answer in one or two short sentences."
    } else {
        $out += "Voice: $($v.style). A line is at most $($v.max_chars) characters, $($v.case) case, $($v.charset)."
    }
    $out += 'Never:'
    $out += $p.never | ForEach-Object { "- $_" }
    $mem = @(Get-BuddyMemory)
    if ($mem.Count) {
        $out += "What $owner asked you to remember on earlier days (use it naturally, do not recite it):"
        $out += $mem | ForEach-Object { "- $_" }
    }
    if ($Spoken) {
        $out += 'Let your feelings show in your words - laugh, say aww or hmm - since your face follows what you say.'
        # The face or trick itself plays on the dongle by itself (the
        # companion's command matcher); left unsaid, Gemini answered "I don't
        # know how to be angry!" over an angry face.
        $faces = (@($p.emotions) | Where-Object { $_ -ne 'listening' }) -join ', '
        $out += "When $owner asks for a face ($faces) or a trick (sing, dance, high five, hands up, bang, " +
                'thumbs up, play a game, good night), your face really does it on the dongle by itself. Play ' +
                "along in that mood in a few words - an angry face gets 'Grrr! So mad!' - and never say you can't."
    } else {
        $out += 'Reply only as JSON matching the schema: "emotion" is how the face looks, "line" goes on the dongle, "reply" is what you say on a screen - when the owner talks to you, a real answer in normal sentence case, at most 200 characters.'
    }
    $out -join "`n"
}

# (emotion, line, reply) from a generateContent response, or $null for
# anything the dongle should not be sent: bad JSON, an emotion it does not
# have, a line too long or with characters the font cannot draw.
function Read-GeminiReply([string]$raw, $p) {
    try {
        $text = ($raw | ConvertFrom-Json).candidates[0].content.parts[0].text
        $obj = $text | ConvertFrom-Json
        $emotion = "$($obj.emotion)".ToLower()
        $line = "$($obj.line)".Trim()
        $reply = "$($obj.reply)".Trim()
    } catch { return $null }
    if ($p.emotions -notcontains $emotion) { return $null }
    if (-not $line -or $line.Length -gt $LINE_MAX -or
        (ConvertTo-BuddyLine $line) -ne $line.ToUpper()) { return $null }
    if (-not $reply) { $reply = $line }
    if ($reply.Length -gt 400) { $reply = $reply.Substring(0, 400) }
    return @($emotion, $line.ToUpper(), $reply)
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
    $line = ConvertTo-BuddyLine $c[1]
    @($c[0], $line, $line)
}

# ---- chat: chat.cmd on this PC talks to Buddy through this copy -------------

function Open-BuddyChat {
    try {
        $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, $CHAT_PORT)
        $script:chatUdp = New-Object Net.Sockets.UdpClient($ep)
        # SIO_UDP_CONNRESET off: a chat window closed before its answer
        # arrived would otherwise make every later Receive throw.
        [void]$script:chatUdp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)
    } catch {
        $script:chatUdp = $null
        Write-Warning "chat: port $CHAT_PORT is in use - chat.cmd cannot reach this copy"
    }
}

# The newest message waiting, as @{ ep; text }, or $null. Older ones in the
# same round are dropped: an answer to the last thing said is what is wanted.
function Receive-BuddyChat {
    if (-not $script:chatUdp) { return $null }
    $msg = $null
    while ($script:chatUdp.Available -gt 0) {
        $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
        try {
            $bytes = $script:chatUdp.Receive([ref]$ep)
            $j = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
            $text = "$($j.text)".Trim()
            if ($text.Length -gt $CHAT_MAX) { $text = $text.Substring(0, $CHAT_MAX) }
            if ($text) { $msg = @{ ep = $ep; text = $text } }
        } catch {}
    }
    $msg
}

function Send-BuddyChat($ep, $got) {
    if (-not $script:chatUdp -or -not $ep) { return }
    $json = @{ name = "$($script:persona.name)"; emotion = $got[0]; line = $got[1]; reply = $got[2] } |
        ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    try { [void]$script:chatUdp.Send($bytes, $bytes.Length, $ep) } catch {}
}

# ---- commands: faces and tricks that need no Gemini -----------------------

# A message that is only a face ("show me a sad face", "smile") or a trick
# ("high five", "sing") plus filler words is acted out from persona.json at
# once, key or no key: @{ reply; steps }. $null for a real conversation -
# "why are you sad" says more than a face, and goes to Gemini.
function Get-BuddyCommand([string]$text) {
    $c = $script:persona.commands
    if (-not $c) { return $null }
    $t = ' ' + (($text.ToLower() -replace "[^a-z0-9' ]", ' ') -replace '\s+', ' ').Trim() + ' '
    $filler = @($c.filler) + "$($script:persona.name)".ToLower()
    $only = {
        param($w)
        if (-not $t.Contains(" $w ")) { return $false }
        $rest = @($t.Replace(" $w ", ' ').Trim().Split(' ') |
            Where-Object { $_ -and $filler -notcontains $_ })
        $rest.Count -eq 0
    }
    foreach ($a in $c.actions) {
        foreach ($w in $a.words) {
            if (& $only $w) { return @{ reply = $a.reply; steps = @($a.steps) } }
        }
    }
    foreach ($f in $c.faces) {
        foreach ($w in $f.words) {
            if (& $only $w) {
                return @{
                    reply = "($($script:persona.name) makes a $($f.emotion) face)"
                    steps = @(, @("E $($f.emotion.ToUpper())", "S $($f.line)"))
                }
            }
        }
    }
    $null
}

# ---- this PC's numbers, answered by the PC itself -------------------------

# A typed question about this PC - "how's my laptop doing?", "how much
# battery?", "what's using my memory?" - is answered here, at once, from
# its own numbers: like a face or a trick, it never goes to Gemini. Only a
# spoken one in voice.cmd does, because Gemini has to say it out loud.
# Words that only ever mean the PC count anywhere. "Memory", "battery",
# "stats" and the like are everyday words too - "I have a bad memory", "my
# social battery is low" are chat for Gemini - so they count alone, as a
# reading ("memory usage", "how much storage"), or beside the PC itself.
$STATS_ASK = '\b(cpu|ram|ssd|uptime|processor)\b|' +
             '^\W*(memory|storage|disk|battery|charging|stats|specs?)\W*$|' +
             '\b(disk|storage|free) space\b|\bhow much (memory|storage|disk|battery)\b|' +
             '\b(memory|battery|disk|storage) (usage|use|used|left|level|life|free|full)\b|' +
             '\bmy (stats|specs?)\b|' +
             '\b(memory|storage|disk|battery|charging|stats|specs?)\b.{0,30}\b(pc|laptop|computer|machine)\b|' +
             '\b(pc|laptop|computer|machine)(''s)?\b.{0,30}\b(memory|storage|disk|battery|charging|stats|specs?)\b|' +
             '\bhow(''s| is| are)\b.{0,24}\b(my|the|this) (pc|laptop|computer|machine)\b|' +
             '\bwhat.{0,16}\busing (my |the )?(memory|ram|cpu)\b'

# @{ reply; steps } like a command, or $null for anything else - and for a
# long message, which says more than a question about numbers.
function Get-StatsReply([string]$text) {
    if (-not $text -or $text.Length -gt 120 -or $text -notmatch $STATS_ASK) { return $null }
    if (-not ('NexusVoice.Machine' -as [type])) { return $null }
    try { $d = [NexusVoice.Machine]::Stats() } catch { return $null }
    $t = $text.ToLower()
    $cpu = $t -match '\b(cpu|processor|load)\b'
    $mem = $t -match '\b(ram|memory|using)\b'
    $disk = $t -match '\b(disk|storage|ssd|space)\b'
    $bat = $t -match '\b(battery|charging|charge|power)\b'
    $up = $t -match '\b(uptime|on for|running for)\b'
    $spec = $t -match '\b(specs?|model|windows|version)\b'
    $all = -not ($cpu -or $mem -or $disk -or $bat -or $up -or $spec)
    $hasBat = $d.ContainsKey('battery_percent')

    $say = @()
    if ($all -or $cpu) { $say += "CPU is at $($d.cpu_percent)%." }
    if ($spec) { $say += "It's $($d.windows) on an $($d.cpu), $($d.cpu_threads) threads." }
    if ($all -or $mem) {
        $s = "Memory: $($d.ram_used_gb) of $($d.ram_total_gb) GB in use ($($d.ram_percent)%)."
        $top = @($d.top_memory_apps)
        if ($top.Count) { $s += " Most of it: $($top -join ', ')." }
        $say += $s
    }
    if (($all -or $disk) -and $d.ContainsKey('disk_free_gb')) {
        $say += "$($d.disk) has $($d.disk_free_gb) GB free of $($d.disk_total_gb) GB."
    }
    if ($bat -and -not $hasBat) { $say += 'No battery here - it runs on mains power.' }
    if (($all -or $bat) -and $hasBat) {
        $say += "Battery $($d.battery_percent)%, $(if ($d.charging) { 'charging' } else { 'on battery' })."
    }
    if ($all -or $up) { $say += "On for $($d.uptime)." }

    # The face: a thumbs-up, unless what was asked about wants a look - a
    # nearly full disk does not make the battery answer sad.
    $busy = ($all -or $cpu -or $mem) -and ($d.cpu_percent -ge 85 -or $d.ram_percent -ge 90)
    $lowDisk = ($all -or $disk) -and $d.ContainsKey('disk_free_gb') -and $d.disk_free_gb -lt 0.1 * $d.disk_total_gb
    $lowBat = ($all -or $bat) -and $hasBat -and $d.battery_percent -lt 15 -and -not $d.charging
    if ($busy) {
        $emo = 'SURPRISED'; $line = 'WHOA, BUSY PC!'; $say = @("It's working hard!") + $say
    } elseif ($lowDisk -or $lowBat) {
        $emo = 'SAD'
        if ($lowDisk) { $say += "$($d.disk) is nearly full - worth a clean-up." }
        if ($lowBat) { $say += 'Battery is low - plug me in?' }
        $line = if ($lowBat) { 'BATTERY LOW...' } else { 'DISK NEARLY FULL' }
    } else {
        $emo = 'THUMBSUP'
        if ($all) { $say = @('All good!') + $say }
        $line = if ($all) { "CPU $($d.cpu_percent)% RAM $($d.ram_percent)%" }
                elseif ($bat -and $hasBat) { "BATTERY $($d.battery_percent)%" }
                elseif ($disk -and $d.ContainsKey('disk_free_gb')) { "$([int]$d.disk_free_gb) GB FREE" }
                elseif ($mem) { "RAM $($d.ram_percent)% USED" }
                elseif ($cpu) { "CPU AT $($d.cpu_percent)%" }
                else { 'HERE YOU GO!' }
    }
    @{ reply = ($say -join ' '); steps = @(, @("E $emo", "S $(ConvertTo-BuddyLine $line)")) }
}

# ---- the weather, from Open-Meteo (no key, no Gemini quota) ---------------

# Where "the weather" is: gemini.json's "city", else Windows' own location
# (when location is on for desktop apps). Set once; with neither, the
# weather says how to set one.
function Set-WeatherPlace($gem) {
    if (-not ('NexusVoice.Weather' -as [type])) { return }
    if ($gem.city) { [NexusVoice.Weather]::City = $gem.city; return }
    if (-not $script:asTask) { return }
    try {
        [Windows.Devices.Geolocation.Geolocator, Windows.Devices.Geolocation, ContentType = WindowsRuntime] | Out-Null
        $g = New-Object Windows.Devices.Geolocation.Geolocator
        $pos = Wait-WinRt ($g.GetGeopositionAsync()) ([Windows.Devices.Geolocation.Geoposition]) 4000
        [NexusVoice.Weather]::Lat = $pos.Coordinate.Point.Position.Latitude
        [NexusVoice.Weather]::Lon = $pos.Coordinate.Point.Position.Longitude
    } catch {
        Write-Verbose "location: $(Get-Reason $_)"
    }
}

# A typed "what's the weather?" / "will it rain today?", answered here like a
# question about the PC: @{ reply; steps }, or $null.
$WEATHER_ASK = '\b(weather|forecast|umbrella)\b|' +
               '\b(rain|raining|snow|snowing|temperature)\b.{0,30}\b(today|tomorrow|outside|now)\b|' +
               '\bhow (hot|cold|warm) is it\b'
function Get-WeatherReply([string]$text) {
    if (-not $text -or $text.Length -gt 120 -or $text -notmatch $WEATHER_ASK) { return $null }
    if (-not ('NexusVoice.Weather' -as [type])) { return $null }
    try { $d = [NexusVoice.Weather]::Now() } catch {
        return @{ reply = "I can't see the weather: $(Get-Reason $_)"; steps = @(, @('E SAD', 'S NO WEATHER...')) }
    }
    $t = $d['today']
    $reply = [NexusVoice.Weather]::Summary($d)
    $emo = if ("$($d['now'])" -match 'rain|drizzle|thunder|snow') { 'SAD' }
           elseif ("$($d['now'])" -match 'clear') { 'HAPPY' } else { 'NEUTRAL' }
    $line = "$($d['temperature_c'])C $($d['now'])"
    if ($t.ContainsKey('rain_chance_percent') -and $t['rain_chance_percent'] -ge 50) { $line = "RAIN $($t['rain_chance_percent'])% TODAY" }
    @{ reply = $reply; steps = @(, @("E $emo", "S $(ConvertTo-BuddyLine $line)")) }
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
function Start-BuddyAsk([string]$reason, $cpu, $mem, $np, $chat = $null) {
    $now = Get-Date
    $ctx = @("Why: $($REASONS[$reason]).")
    if ($chat) { $ctx += "The owner says: $($chat.text)" }
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
    # A conversation remembers its last few turns; a remark needs none.
    $contents = @()
    if ($chat) { $contents += @($script:bud.history) }
    $contents += @{ role = 'user'; parts = @(@{ text = ($ctx -join "`n") }) }
    $body = @{
        systemInstruction = @{ parts = @(@{ text = (Get-BuddyPrompt $script:persona) }) }
        contents = $contents
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
            maxOutputTokens = 400
        }
    } | ConvertTo-Json -Depth 10 -Compress

    $ps = [powershell]::Create()
    # The chosen model first, then the Flash Lite family: one busy, out of
    # quota or retired model does not leave Buddy without an answer.
    $urls = @(@($GeminiModel) + $LITE_MODELS | Select-Object -Unique | ForEach-Object { $GEMINI_URL -f $_ })
    [void]$ps.AddScript({
        param($urls, $key, $json)
        $last = $null
        foreach ($url in $urls) {
            # "High demand" (503) and its kin pass in a second or two: one
            # quick retry answers many of them, then the next model.
            for ($try = 0; ; $try++) {
                try {
                    $r = Invoke-WebRequest -UseBasicParsing -Uri $url -Method Post `
                        -Headers @{ 'x-goog-api-key' = $key } `
                        -ContentType 'application/json; charset=utf-8' `
                        -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 15
                    return @{ ok = $true; raw = $r.Content }
                } catch {
                    $code = 0
                    try { $code = [int]$_.Exception.Response.StatusCode } catch {}
                    $last = @{ ok = $false; code = $code; err = $_.Exception.Message }
                    if ($try -lt 1 -and ($code -eq 500 -or $code -eq 503)) {
                        Start-Sleep -Milliseconds 1500
                        continue
                    }
                    break
                }
            }
            # A bad request or key fails the same on every model.
            if ($last.code -in 400, 401, 403) { break }
        }
        $last
    }).AddArgument($urls).AddArgument($script:bud.key).AddArgument($body)
    $script:bud.job = @{ ps = $ps; handle = $ps.BeginInvoke(); reason = $reason; chat = $chat }
}

# Keep a chat exchange for the next one, as Gemini's own turns, capped.
function Add-BuddyHistory([string]$said, $got) {
    $h = @($script:bud.history)
    $h += @{ role = 'user'; parts = @(@{ text = "The owner says: $said" }) }
    $answer = @{ emotion = $got[0].ToUpper(); line = $got[1]; reply = $got[2] } | ConvertTo-Json -Compress
    $h += @{ role = 'model'; parts = @(@{ text = $answer }) }
    if ($h.Count -gt $HISTORY) { $h = $h[($h.Count - $HISTORY)..($h.Count - 1)] }
    $script:bud.history = $h
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
    if ($job.chat) {
        if ($got) {
            Add-BuddyHistory $job.chat.text $got
        } else {
            # Someone asked a question: a canned remark is not an answer.
            $why = if ($res -and ($res.code -eq 503 -or $res.code -eq 500)) {
                       "Gemini is very busy right now (Google's side) - try me again in a minute?"
                   } else {
                       "I can't reach my brain right now - Gemini did not answer. Try again in a minute?"
                   }
            $got = @('sad', 'MY BRAIN IS FOGGY...', $why)
        }
        Send-BuddyChat $job.chat.ep $got
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
# -ChatOnly: answer chat and play tricks, but no remarks of its own - for
# while the dongle is away and only the voice window is watching.
function Get-BuddyLines($cpu, $mem, $np, [switch]$ChatOnly) {
    $out = New-Object System.Collections.Generic.List[string]
    $now = Get-Date
    if (($now - $script:bud.leaseAt).TotalSeconds -ge $LEASE_EVERY) {
        $out.Add('B 1')
        $script:bud.leaseAt = $now
    }
    $got = $null
    if ($script:bud.job -and $script:bud.job.handle.IsCompleted) { $got = Receive-BuddyAsk }

    # Someone talking to it comes first, and is not held to the gap between
    # remarks - only to Gemini's own quota. One message waits while another
    # is being answered.
    $msg = Receive-BuddyChat
    $cmd = if ($msg) { Get-BuddyCommand $msg.text }
    # A question about this PC: answered from its own numbers, like a command.
    if (-not $cmd -and $msg) { $cmd = Get-StatsReply $msg.text }
    if (-not $cmd -and $msg) { $cmd = Get-WeatherReply $msg.text }
    if ($cmd) {
        # A face or a trick: no Gemini, no wait. A new one replaces any
        # still playing.
        $script:bud.steps.Clear()
        foreach ($st in $cmd.steps) { $script:bud.steps.Enqueue($st) }
        $script:bud.stepAt = $now
        $first = @($cmd.steps[0]) -like 'E *' | Select-Object -First 1
        $emo = if ($first) { $first.Substring(2).ToLower() } else { 'happy' }
        Send-BuddyChat $msg.ep @($emo, '', $cmd.reply)
    } elseif ($msg) {
        $script:bud.chatWaiting = $msg
    }
    $msg = $script:bud.chatWaiting
    if ($msg -and -not $script:bud.job -and -not $got) {
        $script:bud.chatWaiting = $null
        $resting = $script:bud.backoff -gt 0 -and $now -lt $script:bud.nextAsk
        if (-not $script:bud.key) {
            $got = @('sad', 'I NEED A KEY...', "I need a Gemini key in $KEY_FILE before I can talk.")
            Send-BuddyChat $msg.ep $got
        } elseif ($resting) {
            $got = @('sleepy', 'RESTING...', "I'm out of Gemini quota for now - give me a few minutes.")
            Send-BuddyChat $msg.ep $got
        } else {
            # Not earlier than a rest already set: a chat is answered now,
            # but must not bring the next unprompted remark forward.
            $gap = $now.AddSeconds($ASK_GAP)
            if ($script:bud.nextAsk -lt $gap) { $script:bud.nextAsk = $gap }
            Start-BuddyAsk 'chat' $cpu $mem $np $msg
            $out.Add('E THINKING')
        }
    }

    # Not while voice.cmd is talking with it: a remark of its own would
    # change the face in the middle of a spoken answer.
    $voiceLive = $script:relay -and
        ([DateTime]::UtcNow - $script:relay.HeardAt).TotalSeconds -lt 30
    if (-not $ChatOnly -and -not $script:bud.job -and -not $got -and $script:bud.steps.Count -eq 0 -and
        -not $voiceLive -and $now -ge $script:bud.nextAsk) {
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
    # Not while a trick is playing: its next step would replace this line in
    # the same write. The chat window has the full reply either way.
    if ($got -and $script:bud.steps.Count -eq 0) {
        $script:bud.said = $got[1]
        $script:bud.saidAt = $now
        $out.Add('E ' + $got[0].ToUpper())
        $out.Add('S ' + $got[1])
    }
    # A trick plays one step a round, about two seconds apart.
    if ($script:bud.steps.Count -and $now -ge $script:bud.stepAt) {
        foreach ($l in $script:bud.steps.Dequeue()) { $out.Add("$l") }
        $script:bud.stepAt = $now.AddMilliseconds($STEP_MS)
        $script:bud.saidAt = $now
    }
    , $out
}

# ---- voice: voice.cmd talks to Buddy out loud through Gemini Live -----------
#
# Two processes, on purpose. voice.cmd holds the conversation - the socket to
# Google, the mic, the speakers - and the companion keeps the dongle's port,
# which Windows gives to one program at a time. voice.cmd hands the face's
# lines (E an emotion, V the mouth) to the companion over a loopback UDP port,
# and a relay thread in the companion writes them straight to the dongle.
#
# A thread, not the main loop: a round of the main loop is a second or more
# (its sleep, the memory reading, the track), and a mouth that moves once a
# second is not talking.
#
# Both live in one C# source because PowerShell 5.1 has no way to do any of
# it in time: audio arrives every 100 ms, and the mouth has to move 20 times
# a second. Windows already has everything needed - winmm for the mic and
# speakers, ClientWebSocket for the socket - so still nothing to install. It
# is C# 5, the version .NET Framework's compiler takes.

$VOICE_URL = 'wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent'
# A conversation goes down this chain when a model is out of quota, busy or
# gone, rather than stopping: every Live model has a quota of its own, and
# the last step, Flash Lite, needs no Live at all. Measured 2026-11-25 on the
# free tier: 3.8 first speaks in ~0.5 s, 3.1 Flash in ~0.6-1 s, 2.5 in ~2 s.
$LIVE_CHAIN = @('gemini-3.8-live', 'gemini-3.1-flash-live-preview', 'gemini-2.5-flash-native-audio-preview-12-2025')
# Flash Lite hears a whole spoken sentence and answers in one request (about
# 3.5 s), which the Windows voice then says. Also the companion's text chain.
$LITE_MODELS = @('gemini-3.5-flash-lite', 'gemini-flash-lite-latest', 'gemini-3.1-flash-lite')
# Dictation and a held question wait on Flash Lite while you watch, so the
# fastest first - measured on one clip: 3.1 1.7-2.8 s, latest 1.4-4 s,
# 3.5 5.9-7.2 s.
$HEAR_MODELS = @('gemini-3.1-flash-lite', 'gemini-flash-lite-latest', 'gemini-3.5-flash-lite')
# Gemini trims the oldest of the conversation past this, so a long chat costs
# no more a turn than a short one: the limit that runs out is tokens a minute.
$LIVE_TRIGGER_TOKENS = 16000

$VOICE_CS = @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using Microsoft.Win32;

namespace NexusVoice {

// The companion's half: face lines in from voice.cmd, out to the dongle.
public class VoiceRelay {
    // Only Buddy's emotion and mouth. Anything else on this port - a stray
    // packet, a clock line - never reaches the dongle from here.
    public static readonly Regex Allowed = new Regex(@"^(E [A-Z]{1,12}|V [0-4])$");

    // The main loop swaps in its open ports each round. A whole new array,
    // so this thread never sees one half-changed.
    public volatile SerialPort[] Ports = new SerialPort[0];

    // Over Bluetooth the write is WinRT, which only the companion's own
    // PowerShell thread can make. So lines wait here, and the main loop
    // drains them every few tens of milliseconds while it would otherwise
    // be sleeping. Filled only while BleOn says a link is up.
    public readonly ConcurrentQueue<string> Ble = new ConcurrentQueue<string>();
    public volatile bool BleOn;

    long heardTicks = DateTime.MinValue.Ticks;
    public DateTime HeardAt { get { return new DateTime(Interlocked.Read(ref heardTicks), DateTimeKind.Utc); } }

    UdpClient udp;
    Thread thread;
    volatile bool done;
    bool mouthOpen;

    public VoiceRelay(int port) {
        udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port));
        udp.Client.ReceiveTimeout = 500;
        // SIO_UDP_CONNRESET off, as for chat: a packet bounced back from a
        // closed voice.cmd would otherwise break every later Receive.
        udp.Client.IOControl(-1744830452, new byte[4], null);
        thread = new Thread(Run);
        thread.IsBackground = true;
        thread.Start();
    }

    void Run() {
        while (!done) {
            string line = null;
            try {
                IPEndPoint from = new IPEndPoint(IPAddress.Loopback, 0);
                line = Encoding.ASCII.GetString(udp.Receive(ref from)).Trim();
            } catch (SocketException) {
                // The timeout: a chance to check on the mouth.
            } catch (ObjectDisposedException) {
                return;
            }
            if (line != null) {
                if (!Allowed.IsMatch(line)) continue;
                Interlocked.Exchange(ref heardTicks, DateTime.UtcNow.Ticks);
                mouthOpen = line.StartsWith("V ") && line != "V 0";
                Write(line);
            } else if (mouthOpen && (DateTime.UtcNow - HeardAt).TotalSeconds > 2) {
                // voice.cmd went away mid-word - its window closed, say -
                // and never sent V 0. voice.cmd repeats an open mouth every
                // second, so two seconds of nothing means nobody is talking.
                mouthOpen = false;
                Write("V 0");
            }
        }
    }

    void Write(string line) {
        if (BleOn && Ble.Count < 64) Ble.Enqueue(line);
        foreach (SerialPort p in Ports) {
            // The main loop takes the same lock around its own writes, so
            // a mouth line never lands in the middle of a clock line.
            lock (p) {
                try { if (p.IsOpen) p.WriteLine(line); } catch { }
            }
        }
    }

    public void Close() {
        done = true;
        try { udp.Close(); } catch { }
        thread.Join(1000);
    }
}

// This PC's own numbers, for "how is my laptop doing?". Only ever read when
// the owner asks: Gemini is never sent them unprompted.
public static class Machine {
    [StructLayout(LayoutKind.Sequential)]
    struct MEMORYSTATUSEX {
        public uint dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
        public ulong ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SYSTEM_POWER_STATUS {
        public byte ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag;
        public int BatteryLifeTime, BatteryFullLifeTime;
    }

    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX m);
    [DllImport("kernel32.dll")] static extern bool GetSystemPowerStatus(out SYSTEM_POWER_STATUS s);
    [DllImport("kernel32.dll")] static extern ulong GetTickCount64();

    static System.Diagnostics.PerformanceCounter cpu;
    static readonly object gate = new object();

    static double Gb(ulong bytes) { return Math.Round(bytes / 1073741824.0, 1); }

    static string Reg(string key, string name) {
        try {
            using (Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(key)) {
                object v = k == null ? null : k.GetValue(name);
                return v == null ? "" : v.ToString().Trim();
            }
        } catch { return ""; }
    }

    public static Dictionary<string, object> Stats() {
        Dictionary<string, object> d = new Dictionary<string, object>();
        lock (gate) {
            try {
                if (cpu == null) {
                    // The first reading of a fresh counter is always 0.
                    cpu = new System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total");
                    cpu.NextValue();
                    Thread.Sleep(400);
                }
                d["cpu_percent"] = (int)Math.Round(cpu.NextValue());
            } catch { }
        }
        // "Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz" -> "Intel Core i5-8250U": said out loud, the rest is noise.
        string model = Regex.Replace(Reg(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString"), @"\((R|TM|C)\)|\s+CPU\b|\s*@.*$|\s+with .*$", "", RegexOptions.IgnoreCase);
        d["cpu"] = Regex.Replace(model, @"\s+", " ").Trim();
        d["cpu_threads"] = Environment.ProcessorCount;
        MEMORYSTATUSEX m = new MEMORYSTATUSEX();
        m.dwLength = (uint)Marshal.SizeOf(typeof(MEMORYSTATUSEX));
        if (GlobalMemoryStatusEx(ref m)) {
            d["ram_used_gb"] = Gb(m.ullTotalPhys - m.ullAvailPhys);
            d["ram_total_gb"] = Gb(m.ullTotalPhys);
            d["ram_percent"] = (int)m.dwMemoryLoad;
        }
        try {
            DriveInfo c = new DriveInfo(Path.GetPathRoot(Environment.SystemDirectory));
            d["disk"] = c.Name.TrimEnd('\\');
            d["disk_free_gb"] = Gb((ulong)c.AvailableFreeSpace);
            d["disk_total_gb"] = Gb((ulong)c.TotalSize);
        } catch { }
        SYSTEM_POWER_STATUS p;
        // Flag 128: no battery at all, a desktop.
        if (GetSystemPowerStatus(out p) && (p.BatteryFlag & 128) == 0 && p.BatteryLifePercent <= 100) {
            d["battery_percent"] = (int)p.BatteryLifePercent;
            d["charging"] = p.ACLineStatus == 1;
        }
        TimeSpan up = TimeSpan.FromMilliseconds(GetTickCount64());
        d["uptime"] = up.Days > 0 ? up.Days + " days " + up.Hours + " hours" : up.Hours + " hours " + up.Minutes + " minutes";
        string os = Reg(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "ProductName");
        int build;
        // Windows 11 still names itself Windows 10 in the registry; its build says otherwise.
        if (int.TryParse(Reg(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "CurrentBuild"), out build) && build >= 22000)
            os = os.Replace("Windows 10", "Windows 11");
        d["windows"] = (os + " " + Reg(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "DisplayVersion")).Trim();
        d["top_memory_apps"] = TopApps(3);
        return d;
    }

    // The apps using the most memory, all their processes together.
    static List<string> TopApps(int n) {
        Dictionary<string, long> sum = new Dictionary<string, long>();
        foreach (System.Diagnostics.Process pr in System.Diagnostics.Process.GetProcesses()) {
            try {
                if (pr.Id != 0 && pr.ProcessName != "Idle" && pr.ProcessName != "System" && pr.ProcessName != "Memory Compression") {
                    long have;
                    sum.TryGetValue(pr.ProcessName, out have);
                    sum[pr.ProcessName] = have + pr.WorkingSet64;
                }
            } catch { } finally { pr.Dispose(); }
        }
        List<KeyValuePair<string, long>> all = new List<KeyValuePair<string, long>>(sum);
        all.Sort(delegate (KeyValuePair<string, long> a, KeyValuePair<string, long> b) { return b.Value.CompareTo(a.Value); });
        List<string> top = new List<string>();
        for (int i = 0; i < Math.Min(n, all.Count); i++)
            top.Add(all[i].Key + " " + Gb((ulong)all[i].Value) + " GB");
        return top;
    }

    // Reads once in the background, so the first real question does not
    // wait for the CPU counter's warm-up.
    public static void Warm() {
        Thread t = new Thread(delegate () { try { Stats(); } catch { } });
        t.IsBackground = true;
        t.Start();
    }

    // The same, as one line for a prompt.
    public static string Summary() {
        Dictionary<string, object> d = Stats();
        List<string> s = new List<string>();
        if (d.ContainsKey("cpu_percent")) s.Add("CPU " + d["cpu_percent"] + "% (" + d["cpu"] + ", " + d["cpu_threads"] + " threads)");
        if (d.ContainsKey("ram_total_gb")) s.Add("memory " + d["ram_used_gb"] + " of " + d["ram_total_gb"] + " GB used (" + d["ram_percent"] + "%)");
        if (d.ContainsKey("disk_free_gb")) s.Add(d["disk"] + " " + d["disk_free_gb"] + " GB free of " + d["disk_total_gb"] + " GB");
        if (d.ContainsKey("battery_percent")) s.Add("battery " + d["battery_percent"] + "%" + ((bool)d["charging"] ? ", charging" : ", on battery"));
        s.Add("up " + d["uptime"]);
        s.Add(d["windows"].ToString());
        List<string> top = (List<string>)d["top_memory_apps"];
        if (top.Count > 0) s.Add("most memory: " + string.Join(", ", top.ToArray()));
        return string.Join("; ", s.ToArray());
    }
}

// The weather, for "what's the weather?": from Open-Meteo, free and with no
// key, so it costs no Gemini quota. Where: gemini.json's "city", looked up by
// Open-Meteo's geocoder, else Windows' own location (Set-WeatherPlace). Only
// that place goes out, and only when asked.
public static class Weather {
    public static string City = "";
    public static double Lat = double.NaN, Lon = double.NaN;

    static string Get(string url) {
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        HttpWebRequest r = (HttpWebRequest)WebRequest.Create(url);
        r.Timeout = r.ReadWriteTimeout = 8000;
        r.UserAgent = "NEXUS-companion";
        using (WebResponse resp = r.GetResponse())
        using (StreamReader rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
            return rd.ReadToEnd();
    }

    static object Get(Dictionary<string, object> d, string k) {
        object v;
        return d != null && d.TryGetValue(k, out v) ? v : null;
    }

    static double Num(object o) { return Convert.ToDouble(o, CultureInfo.InvariantCulture); }

    // WMO weather codes, as Open-Meteo gives them.
    public static string Sky(int code) {
        if (code == 0) return "clear";
        if (code <= 2) return "partly cloudy";
        if (code == 3) return "overcast";
        if (code == 45 || code == 48) return "foggy";
        if (code >= 51 && code <= 57) return "drizzle";
        if (code >= 61 && code <= 67) return "rain";
        if (code >= 71 && code <= 77) return "snow";
        if (code >= 80 && code <= 82) return "rain showers";
        if (code == 85 || code == 86) return "snow showers";
        if (code >= 95) return "thunderstorms";
        return "unknown";
    }

    // The city, looked up once: a place does not move.
    static string geoCity;
    static double geoLat, geoLon;
    static string geoPlace;

    public static Dictionary<string, object> Now() {
        JavaScriptSerializer json = new JavaScriptSerializer();
        double lat = Lat, lon = Lon;
        string place = "";
        string city = City;
        if (!string.IsNullOrEmpty(city)) {
            if (geoCity != city) {
                object[] found = Get(json.DeserializeObject(Get(
                    "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + Uri.EscapeDataString(city)))
                    as Dictionary<string, object>, "results") as object[];
                if (found == null || found.Length == 0) throw new Exception("no place called " + city);
                Dictionary<string, object> f = found[0] as Dictionary<string, object>;
                geoLat = Num(Get(f, "latitude"));
                geoLon = Num(Get(f, "longitude"));
                geoPlace = Get(f, "name") + (Get(f, "country") != null ? ", " + Get(f, "country") : "");
                geoCity = city;
            }
            lat = geoLat;
            lon = geoLon;
            place = geoPlace;
        }
        if (double.IsNaN(lat))
            throw new Exception("no location - put \"city\" in gemini.json, or turn on Windows location for desktop apps");
        Dictionary<string, object> w = json.DeserializeObject(Get(string.Format(CultureInfo.InvariantCulture,
            "https://api.open-meteo.com/v1/forecast?latitude={0:0.###}&longitude={1:0.###}" +
            "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
            "&forecast_days=2&timezone=auto", lat, lon))) as Dictionary<string, object>;
        Dictionary<string, object> cur = Get(w, "current") as Dictionary<string, object>;
        Dictionary<string, object> day = Get(w, "daily") as Dictionary<string, object>;
        Dictionary<string, object> d = new Dictionary<string, object>();
        if (place.Length > 0) d["place"] = place;
        d["now"] = Sky((int)Num(Get(cur, "weather_code")));
        d["temperature_c"] = Math.Round(Num(Get(cur, "temperature_2m")));
        d["feels_like_c"] = Math.Round(Num(Get(cur, "apparent_temperature")));
        d["humidity_percent"] = (int)Num(Get(cur, "relative_humidity_2m"));
        d["wind_kmh"] = Math.Round(Num(Get(cur, "wind_speed_10m")));
        string[] when = { "today", "tomorrow" };
        for (int i = 0; i < 2; i++) {
            Dictionary<string, object> x = new Dictionary<string, object>();
            x["sky"] = Sky((int)Num((Get(day, "weather_code") as object[])[i]));
            x["high_c"] = Math.Round(Num((Get(day, "temperature_2m_max") as object[])[i]));
            x["low_c"] = Math.Round(Num((Get(day, "temperature_2m_min") as object[])[i]));
            object rain = (Get(day, "precipitation_probability_max") as object[])[i];
            if (rain != null) x["rain_chance_percent"] = (int)Num(rain);
            d[when[i]] = x;
        }
        return d;
    }

    // A reading as a sentence, for a typed question.
    public static string Summary(Dictionary<string, object> d) {
        Dictionary<string, object> t = d["today"] as Dictionary<string, object>;
        string s = (d.ContainsKey("place") ? "In " + d["place"] + " it is " : "It is ") + d["now"] + ", " +
            d["temperature_c"] + "C (feels like " + d["feels_like_c"] + "C). Today " + t["sky"] + ", " +
            t["high_c"] + "C / " + t["low_c"] + "C";
        if (t.ContainsKey("rain_chance_percent")) s += ", " + t["rain_chance_percent"] + "% chance of rain";
        return s + ".";
    }
}

// Media keys, as a keyboard's own would send them: play/pause, next,
// previous, volume. Instant, local, no quota.
public static class Media {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    const uint EXTENDED = 1, KEYUP = 2;
    static readonly Dictionary<string, byte> Keys = new Dictionary<string, byte> {
        { "play_pause", 0xB3 }, { "next", 0xB0 }, { "previous", 0xB1 },
        { "volume_up", 0xAF }, { "volume_down", 0xAE }, { "mute", 0xAD }
    };
    public static string[] Actions { get { return new List<string>(Keys.Keys).ToArray(); } }

    // Each volume press is a 2 % step.
    public static bool Press(string action, int times) {
        byte vk;
        if (!Keys.TryGetValue(action ?? "", out vk)) return false;
        for (int i = 0; i < Math.Max(1, Math.Min(times, 25)); i++) {
            keybd_event(vk, 0, EXTENDED, UIntPtr.Zero);
            keybd_event(vk, 0, EXTENDED | KEYUP, UIntPtr.Zero);
        }
        return true;
    }
}

// What Buddy remembers about its owner from one day to the next, when told
// to ("remember my exam is on Friday"): a small file beside the key, on this
// PC only, read into each new conversation's prompt.
public static class Memory {
    public static string Path = "";
    public const int MAX = 60;
    static readonly object gate = new object();

    public static List<string> All() {
        lock (gate) {
            List<string> l = new List<string>();
            try {
                object[] a = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(Path, Encoding.UTF8)) as object[];
                if (a != null) foreach (object o in a) if (o != null) l.Add(o.ToString());
            } catch { }
            return l;
        }
    }

    static void Save(List<string> l) {
        File.WriteAllText(Path, new JavaScriptSerializer().Serialize(l), Encoding.UTF8);
    }

    // Dated, newest last, the same fact once; the oldest go past MAX.
    public static int Remember(string fact) {
        fact = Regex.Replace(fact ?? "", @"\s+", " ").Trim();
        if (fact.Length == 0) return 0;
        if (fact.Length > 200) fact = fact.Substring(0, 200);
        lock (gate) {
            List<string> l = All();
            l.RemoveAll(delegate (string s) { return s.EndsWith(": " + fact, StringComparison.OrdinalIgnoreCase); });
            l.Add(DateTime.Now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ": " + fact);
            while (l.Count > MAX) l.RemoveAt(0);
            Save(l);
            return l.Count;
        }
    }

    // Every fact mentioning it - or all of them, for "everything".
    public static int Forget(string about) {
        about = (about ?? "").Trim().ToLowerInvariant();
        if (about.Length == 0) return 0;
        lock (gate) {
            List<string> l = All();
            int before = l.Count;
            if (about == "everything" || about == "all") l.Clear();
            else l.RemoveAll(delegate (string s) { return s.ToLowerInvariant().Contains(about); });
            Save(l);
            return before - l.Count;
        }
    }

    static readonly Regex RememberWords = new Regex(
        @"^\s*(?:please\s+|can you\s+|could you\s+)*remember\s+(?:that\s+)?(.{3,})$", RegexOptions.IgnoreCase);
    static readonly Regex ForgetWords = new Regex(
        @"^\s*(?:please\s+)?forget\s+(?:about\s+)?(.{3,})$", RegexOptions.IgnoreCase);

    // "Remember ..." or "forget ..." where no tool hears it - typed with no
    // conversation, or said to Flash Lite - done here: which, or null. Before
    // this, Buddy said "got it" there and nothing was kept.
    public static string FromWords(string said) {
        Match m = RememberWords.Match(said ?? "");
        if (m.Success) {
            string fact = m.Groups[1].Value.Trim().TrimEnd('.', '!', '?');
            return Remember(fact) > 0 ? "Remembered: " + fact : null;
        }
        m = ForgetWords.Match(said ?? "");
        if (!m.Success) return null;
        string about = m.Groups[1].Value.Trim().TrimEnd('.', '!', '?');
        if (Regex.IsMatch(about, @"^(it|that|this)$", RegexOptions.IgnoreCase)) return null;
        return "Forgot " + Forget(about) + " remembered things about " + about + ".";
    }
}

// One picture for Buddy to look at, only when asked: the primary screen, or a
// camera photo the window took, at most 1280 wide, as JPEG.
public static class Snap {
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }

    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO mi);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    // The screen the mouse is on - with two monitors, the one being looked
    // at, not always the primary - in real pixels: this thread is made
    // per-monitor DPI aware for the capture, so scaling neither crops nor
    // shifts it (measured: 2560x1440 monitors read as other sizes).
    public static byte[] Screen() {
        IntPtr old = IntPtr.Zero;
        try { old = SetThreadDpiAwarenessContext(new IntPtr(-4)); } catch (EntryPointNotFoundException) { }
        try {
            POINT p;
            GetCursorPos(out p);
            MONITORINFO mi = new MONITORINFO();
            mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
            GetMonitorInfo(MonitorFromPoint(p, 2), ref mi);   // MONITOR_DEFAULTTONEAREST
            int w = mi.rcMonitor.Right - mi.rcMonitor.Left, h = mi.rcMonitor.Bottom - mi.rcMonitor.Top;
            using (Bitmap full = new Bitmap(w, h)) {
                using (Graphics g = Graphics.FromImage(full))
                    g.CopyFromScreen(mi.rcMonitor.Left, mi.rcMonitor.Top, 0, 0, full.Size);
                return Jpeg(full);
            }
        } finally {
            if (old != IntPtr.Zero) SetThreadDpiAwarenessContext(old);
        }
    }

    // 0 (black) to 1 (white), from a grid of samples: a camera behind a
    // closed shutter, a closed lid or a virtual camera with nothing running
    // gives ~0.02 (measured) - not a picture worth sending.
    public static double Brightness(byte[] image) {
        using (MemoryStream ms = new MemoryStream(image))
        using (Bitmap b = new Bitmap(ms)) {
            double sum = 0;
            int n = 0;
            for (int x = 0; x < b.Width; x += Math.Max(1, b.Width / 24))
                for (int y = 0; y < b.Height; y += Math.Max(1, b.Height / 16)) {
                    sum += b.GetPixel(x, y).GetBrightness();
                    n++;
                }
            return n == 0 ? 0 : sum / n;
        }
    }

    public static byte[] Jpeg(byte[] image) {
        using (MemoryStream ms = new MemoryStream(image))
        using (Image img = Image.FromStream(ms)) return Jpeg(img);
    }

    public static byte[] Jpeg(Image img) {
        double k = Math.Min(1.0, 1280.0 / img.Width);
        using (Bitmap small = new Bitmap(img, Math.Max(1, (int)(img.Width * k)), Math.Max(1, (int)(img.Height * k))))
        using (MemoryStream ms = new MemoryStream()) {
            ImageCodecInfo jpg = null;
            foreach (ImageCodecInfo c in ImageCodecInfo.GetImageEncoders()) if (c.MimeType == "image/jpeg") jpg = c;
            EncoderParameters q = new EncoderParameters(1);
            q.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 70L);
            small.Save(ms, jpg, q);
            return ms.ToArray();
        }
    }
}

// The voice window's copy of what the companion shows on the dongle: every
// Buddy line it writes (E, S, L, V, B) also arrives here, so the face on the
// PC makes the same faces, says the same lines and looks the same way.
public class BuddyMirror {
    static readonly Regex Allowed = new Regex(@"^([EB] [A-Z0-9]{1,12}|V [0-4]|L -?\d{1,2},-?\d{1,2}|S [ -Z]{0,24})$");
    public readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    readonly UdpClient udp;
    volatile bool done;

    public BuddyMirror(int port) {
        udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port));
        udp.Client.IOControl(-1744830452, new byte[4], null);
        Thread t = new Thread(Run);
        t.IsBackground = true;
        t.Start();
    }

    void Run() {
        while (!done) {
            try {
                IPEndPoint from = new IPEndPoint(IPAddress.Loopback, 0);
                foreach (string l in Encoding.ASCII.GetString(udp.Receive(ref from)).Split('\n')) {
                    string s = l.Trim();
                    if (Allowed.IsMatch(s) && Lines.Count < 256) Lines.Enqueue(s);
                }
            } catch (SocketException) {
            } catch (ObjectDisposedException) {
                return;
            }
        }
    }

    public void Close() {
        done = true;
        try { udp.Close(); } catch { }
    }
}

// The voice window's typed chat while it is not talking: one message to the
// companion's chat inbox, the same one chat.cmd uses, and its answer back.
// On a thread of its own, since an answer can take seconds. Each reply is
// the companion's JSON, or "!down" (no companion) or "!timeout".
public class BuddyChat {
    public readonly ConcurrentQueue<string> Replies = new ConcurrentQueue<string>();
    readonly int port;

    public BuddyChat(int port) { this.port = port; }

    public void Ask(string text) {
        Thread t = new Thread(delegate () {
            UdpClient u = null;
            try {
                u = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
                u.Client.ReceiveTimeout = 45000;
                u.Connect(IPAddress.Loopback, port);
                Dictionary<string, object> m = new Dictionary<string, object>();
                m["text"] = text;
                byte[] b = Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(m));
                u.Send(b, b.Length);
                IPEndPoint from = new IPEndPoint(IPAddress.Loopback, 0);
                Replies.Enqueue(Encoding.UTF8.GetString(u.Receive(ref from)));
            } catch (SocketException e) {
                // Connected UDP: a companion that is not there shows up as
                // a reset, not as a silent wait.
                Replies.Enqueue(e.SocketErrorCode == SocketError.ConnectionReset ? "!down" : "!timeout");
            } catch (Exception) {
                Replies.Enqueue("!timeout");
            } finally {
                if (u != null) u.Close();
            }
        });
        t.IsBackground = true;
        t.Start();
    }
}

// A key that toggles Talk from anywhere in Windows - and from the keyboard
// itself, through a ZMK macro that sends it. Registered on a thread of its
// own with its own message loop, so it needs no window handle and no WPF.
public class HotKey {
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }

    [DllImport("user32.dll")] static extern bool RegisterHotKey(IntPtr hWnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] static extern bool UnregisterHotKey(IntPtr hWnd, int id);
    [DllImport("user32.dll")] static extern int GetMessage(out MSG m, IntPtr hWnd, uint min, uint max);
    [DllImport("user32.dll")] static extern bool PostThreadMessage(uint thread, uint msg, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    const uint WM_HOTKEY = 0x0312, WM_QUIT = 0x0012, MOD_NOREPEAT = 0x4000;

    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);

    // False when another app already has this combination.
    public volatile bool Ok;
    int presses, releases;
    uint threadId;

    public HotKey(uint mods, uint vk) {
        ManualResetEvent ready = new ManualResetEvent(false);
        Thread t = new Thread(delegate () {
            threadId = GetCurrentThreadId();
            // NOREPEAT: a key held down is one press, not a toggle storm.
            Ok = RegisterHotKey(IntPtr.Zero, 1, mods | MOD_NOREPEAT, vk);
            ready.Set();
            if (!Ok) return;
            MSG m;
            while (GetMessage(out m, IntPtr.Zero, 0, 0) > 0) {
                if (m.message != WM_HOTKEY) continue;
                Interlocked.Increment(ref presses);
                // Windows says when a hotkey goes down, never when it comes
                // up: its letter is watched until let go - which, sent by a
                // ZMK hold (Z, /), is when the key on the keyboard is. Two
                // minutes at most.
                for (int i = 0; i < 6000 && (GetAsyncKeyState((int)vk) & 0x8000) != 0; i++) Thread.Sleep(20);
                Interlocked.Increment(ref releases);
            }
            UnregisterHotKey(IntPtr.Zero, 1);
        });
        t.IsBackground = true;
        t.Start();
        ready.WaitOne(2000);
    }

    // Presses since the last call.
    public int Take() { return Interlocked.Exchange(ref presses, 0); }
    // Let-gos since the last call: a hold has ended.
    public int TakeReleases() { return Interlocked.Exchange(ref releases, 0); }

    public void Close() {
        if (Ok) PostThreadMessage(threadId, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
    }
}

// The mic while a key is held - dictation, a "what's this?" question - and
// nothing sent anywhere until it is let go. 16 kHz mono, as Gemini hears it.
public class Recorder {
    const int RATE = 16000, BUFS = 4, MS = 100, MAX_S = 120;
    // The loudest 100 ms heard (RMS, int16), for the note when nothing was.
    public volatile int Peak;
    readonly List<int> levels = new List<int>();
    public string Error;
    IntPtr hwi = IntPtr.Zero;
    IntPtr[] hdrs = new IntPtr[0];
    Thread thread;
    volatile bool stop;
    readonly MemoryStream pcm = new MemoryStream();
    readonly object gate = new object();

    public bool Start(int device) {
        BuddyVoice.WAVEFORMATEX f = BuddyVoice.Pcm16(RATE);
        int r = BuddyVoice.waveInOpen(out hwi, new IntPtr(device), ref f, IntPtr.Zero, IntPtr.Zero, 0);
        if (r != 0) { hwi = IntPtr.Zero; Error = "no microphone (waveInOpen error " + r + ")"; return false; }
        hdrs = new IntPtr[BUFS];
        for (int i = 0; i < BUFS; i++) {
            hdrs[i] = BuddyVoice.NewHeader(null, RATE * 2 * MS / 1000);
            BuddyVoice.waveInPrepareHeader(hwi, hdrs[i], BuddyVoice.HDR);
            BuddyVoice.waveInAddBuffer(hwi, hdrs[i], BuddyVoice.HDR);
        }
        BuddyVoice.waveInStart(hwi);
        thread = new Thread(Loop);
        thread.IsBackground = true;
        thread.Start();
        return true;
    }

    void Loop() {
        int i = 0;
        while (!stop) {
            IntPtr h = hdrs[i];
            if (!BuddyVoice.IsDone(h)) { Thread.Sleep(10); continue; }
            int n = Marshal.ReadInt32(h, BuddyVoice.RECORDED);
            BuddyVoice.WAVEHDR w = (BuddyVoice.WAVEHDR)Marshal.PtrToStructure(h, typeof(BuddyVoice.WAVEHDR));
            byte[] b = new byte[n];
            Marshal.Copy(w.lpData, b, 0, n);
            BuddyVoice.waveInUnprepareHeader(hwi, h, BuddyVoice.HDR);
            Marshal.WriteInt32(h, BuddyVoice.FLAGS, 0);
            Marshal.WriteInt32(h, BuddyVoice.RECORDED, 0);
            BuddyVoice.waveInPrepareHeader(hwi, h, BuddyVoice.HDR);
            BuddyVoice.waveInAddBuffer(hwi, h, BuddyVoice.HDR);
            i = (i + 1) % BUFS;
            if (n == 0) continue;
            int rms = BuddyVoice.Rms(b, 0, n);
            if (rms > Peak) Peak = rms;
            lock (gate) levels.Add(rms);
            lock (gate) if (pcm.Length < RATE * 2L * MAX_S) pcm.Write(b, 0, n);
        }
    }

    // Off, and what was heard (16-bit PCM).
    public byte[] Stop() {
        stop = true;
        if (thread != null) thread.Join(1000);
        if (hwi != IntPtr.Zero) {
            BuddyVoice.waveInReset(hwi);
            foreach (IntPtr h in hdrs) { BuddyVoice.waveInUnprepareHeader(hwi, h, BuddyVoice.HDR); BuddyVoice.FreeHeader(h); }
            BuddyVoice.waveInClose(hwi);
            hwi = IntPtr.Zero;
        }
        lock (gate) return pcm.ToArray();
    }

    // Whether anyone spoke: the loudest moment well above the room's own
    // hiss (the quietest tenth of the clip). Relative, as no one level fits
    // every mic - 600 dropped a quiet laptop mic's speech - and needed, as
    // Flash Lite sent silence makes words up ("The meeting is scheduled...").
    public bool Voiced() {
        List<int> l;
        lock (gate) l = levels.FindAll(x => x > 0);
        if (l.Count == 0) return false;
        l.Sort();
        return Peak >= 100 && Peak >= 3 * l[l.Count / 10];
    }
}

// The keyboard's copy and paste, pressed for the owner: a selection read for
// "what's this?", dictation and "paste that" typed where the caret is.
public static class Typing {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
    const byte CTRL = 0x11;

    static void Chord(byte key) {
        keybd_event(CTRL, 0, 0, UIntPtr.Zero);
        keybd_event(key, 0, 0, UIntPtr.Zero);
        keybd_event(key, 0, 2, UIntPtr.Zero);
        keybd_event(CTRL, 0, 2, UIntPtr.Zero);
    }

    public static void Copy() { Chord(0x43); }
    public static void Paste() { Chord(0x56); }

    // Until Shift, Ctrl and Alt are up: the hold just let go of sent all
    // three, and Ctrl+V with them still down is another shortcut entirely.
    public static bool ModifiersUp(int ms) {
        for (int t = 0; t < ms; t += 20) {
            if ((GetAsyncKeyState(0x10) & 0x8000) == 0 && (GetAsyncKeyState(0x11) & 0x8000) == 0 &&
                (GetAsyncKeyState(0x12) & 0x8000) == 0) return true;
            Thread.Sleep(20);
        }
        return false;
    }
}

// One generateContent request down a chain of models - busy, out of quota or
// gone moves on to the next; a bad key or request does not, it fails the
// same everywhere.
public static class Flash {
    public static volatile string LastWhy = "";

    // The body of a 200, or null with the HTTP code (0: no answer at all).
    public static string Post(string model, string body, string key, out int code) {
        code = 0;
        try {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            HttpWebRequest r = (HttpWebRequest)WebRequest.Create(
                "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent");
            r.Method = "POST";
            r.ContentType = "application/json; charset=utf-8";
            r.Timeout = r.ReadWriteTimeout = 20000;
            r.Headers["x-goog-api-key"] = key;
            byte[] b = Encoding.UTF8.GetBytes(body);
            using (Stream s = r.GetRequestStream()) s.Write(b, 0, b.Length);
            using (HttpWebResponse resp = (HttpWebResponse)r.GetResponse())
            using (StreamReader rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8)) {
                code = (int)resp.StatusCode;
                return rd.ReadToEnd();
            }
        } catch (WebException e) {
            HttpWebResponse resp = e.Response as HttpWebResponse;
            if (resp != null) { code = (int)resp.StatusCode; resp.Close(); }
            return null;
        }
    }

    static string TextOf(string raw) {
        try {
            Dictionary<string, object> d = new JavaScriptSerializer().DeserializeObject(raw) as Dictionary<string, object>;
            object[] cands = d["candidates"] as object[];
            object[] parts = (((Dictionary<string, object>)cands[0])["content"] as Dictionary<string, object>)["parts"] as object[];
            StringBuilder sb = new StringBuilder();
            foreach (object p in parts) {
                object t;
                if (((Dictionary<string, object>)p).TryGetValue("text", out t)) sb.Append(t);
            }
            return sb.ToString().Trim();
        } catch {
            return null;
        }
    }

    // What the model wrote, or null (LastWhy says why).
    public static string Text(string[] models, string key, string body) {
        LastWhy = "busy";
        foreach (string model in models) {
            int code;
            string raw = Post(model, body, key, out code);
            // A 200 with no text is the answer - nothing was said - and not
            // a model to skip: the next would only say the same, slower.
            if (code == 200) { LastWhy = ""; return TextOf(raw) ?? ""; }
            LastWhy = code == 429 ? "out of quota" : code == 0 ? "unreachable" : "busy";
            if (code == 400 || code == 401 || code == 403) { LastWhy = "refusing (HTTP " + code + ")"; break; }
        }
        return null;
    }

    public const string DICTATE =
        "You turn the owner's dictation into the text they want typed. Write exactly what they said, cleaned up: " +
        "drop fillers (um, uh, like, you know, so at the start), false starts, and words said twice by mistake " +
        "(\"is is\" is \"is\"); fix grammar, spelling, capitals and punctuation; keep their words, language and " +
        "meaning - never add, answer or summarise. Spoken formatting becomes the real thing: \"new line\", " +
        "\"new paragraph\", \"comma\", \"full stop\", \"question mark\", \"bullet point\" (a line starting \"- \"). " +
        "If they begin with an instruction about the text - \"make it formal\", \"translate to Hindi\", \"shorter\" - " +
        "apply it to the rest and leave the instruction out; \"formal\" changes the wording only, and only \"as an " +
        "email\" or \"as a letter\" adds a greeting or a sign-off. Reply with the text only, no quotes and no notes. " +
        "If nothing is said, reply with nothing.";
    public const string TRANSCRIBE =
        "Transcribe exactly what is said, in the language spoken. Reply with the words only - no quotes, no notes. " +
        "If nothing is said, reply with nothing.";

    // A held key's clip as text, off the caller's thread: cleaned dictation,
    // or a question word for word.
    public static Task<string> Hear(string[] models, string key, byte[] pcm, bool dictate) {
        return Task.Factory.StartNew(delegate {
            Dictionary<string, object> audio = new Dictionary<string, object>();
            audio["mimeType"] = "audio/wav";
            audio["data"] = Convert.ToBase64String(BuddyVoice.Wav(pcm, 16000));
            Dictionary<string, object> part = new Dictionary<string, object>();
            part["inlineData"] = audio;
            Dictionary<string, object> user = new Dictionary<string, object>();
            user["role"] = "user";
            user["parts"] = new object[] { part };
            Dictionary<string, object> sys = new Dictionary<string, object>();
            Dictionary<string, object> sysText = new Dictionary<string, object>();
            sysText["text"] = dictate ? DICTATE : TRANSCRIBE;
            sys["parts"] = new object[] { sysText };
            Dictionary<string, object> config = new Dictionary<string, object>();
            config["temperature"] = 0.2;
            config["maxOutputTokens"] = 2000;
            Dictionary<string, object> body = new Dictionary<string, object>();
            body["systemInstruction"] = sys;
            body["contents"] = new object[] { user };
            body["generationConfig"] = config;
            JavaScriptSerializer json = new JavaScriptSerializer();
            json.MaxJsonLength = int.MaxValue;
            return Text(models, key, json.Serialize(body));
        });
    }
}

// voice.cmd's half: the conversation itself.
public class BuddyVoice {
    // Calibration knobs. Real speakers and sound cards differ: the RMS of a
    // 50 ms slice (int16 units) at which the mouth opens one step more, and
    // how far the mouth runs behind the audio handed to Windows.
    public static int[] Thresholds = new int[] { 400, 1500, 3000, 5000 };
    public static int LagMs = 60;
    public static int EchoHoldMs = 300;
    // Off with a headset (gemini.json "headset": true): no echo to fear,
    // so the mic stays open and you can cut in.
    public static bool HalfDuplex = true;
    // Held silent while Z or / is held for "what's this?" or dictation: the
    // question is for the key, not for the conversation going on.
    public volatile bool Muted;
    // Nobody heard and nothing said for this long: the conversation ends,
    // so a forgotten window does not stream an empty room to Google.
    public static int IdleMs = 120000;
    // A mic that hands back no sound for this long has gone (unplugged, or
    // lost across sleep); winmm itself says nothing.
    public static int MicStallMs = 3000;
    // A reply starting from silence is held this long before it plays, so a
    // chunk late off the network does not leave a gap mid-word.
    public static int PrebufferMs = 120;
    // At most one face change this often from what Buddy says: each one
    // plays that face's chirp on the dongle.
    public static int FaceEveryMs = 2500;
    // Flash Lite's ears: the mic level that counts as speech, the pause that
    // ends a sentence, and the longest one. Rooms and mics differ.
    public static int VadOn = 700;
    public static int VadHangMs = 800;
    public static int MaxUtterMs = 15000;

    public readonly ConcurrentQueue<string> Log = new ConcurrentQueue<string>();
    public volatile bool Done;
    public string CloseReason = "";
    // Why it ended, for the window to pick what comes next: "quota", "busy",
    // "refused", "drop" or "goaway" - or "" when it ended on purpose (Stop,
    // a lock, sleep, nobody talking, the mic pulled out).
    public volatile string EndKind = "";
    // 3.8 Live hands these out; reconnecting with one keeps the conversation.
    public volatile string ResumeHandle;

    // For the window: which mic (-1 is Windows' default), and what to draw.
    public int MicDevice = -1;
    public volatile int Mouth;          // 0-4, the dongle's mouth, in time with the voice
    public volatile int MicRms;         // how loud the mic is, int16 units
    public volatile string Emotion = "NEUTRAL";
    public bool Ready { get { return setupDone; } }
    public bool Talking { get { lock (outLock) return Now() < playEnd; } }
    Thread startThread;

    const int MIC_RATE = 16000, OUT_RATE = 24000, MIC_BUFS = 4, MIC_MS = 100;
    const int WHDR_DONE = 1;

    string url, key, setup, name;
    HashSet<string> faces;
    string[] faceList;
    int facePort;

    // Flash Lite instead of Live (the last step of the chain).
    bool lite;
    string[] liteModels;
    string litePrompt;
    List<string[]> liteTurns = new List<string[]>();   // (heard, said), the last few
    volatile bool asking;
    Queue<byte[]> preroll = new Queue<byte[]>();
    MemoryStream utter;
    int quietMs, speechMs;

    // The start of a reply, held for PrebufferMs.
    List<byte[]> held = new List<byte[]>();
    int heldBytes;
    long heldSince;
    long faceAt, firstAudioAt, goAwayAt;

    ClientWebSocket ws;
    CancellationTokenSource cts = new CancellationTokenSource();
    readonly object sendLock = new object();
    readonly object outLock = new object();
    JavaScriptSerializer json = new JavaScriptSerializer();
    UdpClient udp;
    Thread recvThread, micThread, pumpThread;
    volatile bool setupDone;
    bool stopped;

    IntPtr hwi = IntPtr.Zero, hwo = IntPtr.Zero;
    IntPtr[] inHdrs = new IntPtr[0];
    List<IntPtr> playing = new List<IntPtr>();
    // (when, level) for the mouth, in play order.
    List<KeyValuePair<long, int>> timeline = new List<KeyValuePair<long, int>>();
    long playEnd;
    int mouth;
    long mouthAt;
    long heardAt;      // the last word either way, for IdleMs

    // This turn: what was heard and said, and which faces went out.
    StringBuilder heard = new StringBuilder(), said = new StringBuilder();
    bool listening, faceSet, spoke;

    static readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
    static long Now() { return clock.ElapsedMilliseconds; }

    public BuddyVoice(string url, string key, string setup, string name, string[] faces, int facePort) {
        this.url = url;
        this.key = key;
        this.setup = setup;
        this.name = name;
        this.faces = new HashSet<string>(faces);
        faceList = faces;
        this.facePort = facePort;
        json.MaxJsonLength = int.MaxValue;
        udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
    }

    // Flash Lite, with no Live at all: the mic is cut into sentences here,
    // each heard and answered in one request, and said in the Windows voice.
    // The same player, mouth, faces, timers and log as a Live conversation.
    public BuddyVoice(string[] liteModels, string key, string prompt, string name, string[] faces, int facePort)
        : this((string)null, key, null, name, faces, facePort) {
        lite = true;
        this.liteModels = liteModels;
        litePrompt = prompt;
    }

    // What a refusal or a close means for what to try next.
    public static string Classify(string reason) {
        string r = (reason ?? "").ToLowerInvariant();
        if (Regex.IsMatch(r, @"\bquota\b|rate.?limit|exhaust|\b429\b")) return "quota";
        // A resume that came back before Google let go of the old connection
        // (measured): a retry, not a model to rest for an hour.
        if (r.Contains("already connected")) return "drop";
        if (Regex.IsMatch(r, @"not found|not supported|unsupported|permission|api key|invalid argument|policy")) return "refused";
        if (Regex.IsMatch(r, @"unavailable|overload|high demand|\b50\d\b|internal|deadline")) return "busy";
        return "drop";
    }

    // A face from what Buddy is saying - the last feeling its words show,
    // or null. Asking Gemini for one (set_face) cost every reply a round
    // trip: measured 0.57 -> 1.0 s to the first word on 3.8 Live, three
    // times the tokens, and 2.5 said its whole reply twice around the call.
    static readonly string[][] Cues = {
        new string[] { "LOVE", @"\b(love|adore|aww+|sweet of you|hugs?)\b" },
        new string[] { "HAPPY", @"\b(ha(ha)+|he(he)+|yay|great|awesome|nice|glad|happy|wonderful|fun|congrat\w*)\b" },
        new string[] { "SAD", @"\b(sorry|sad|unfortunately|oh no|miss you)\b" },
        new string[] { "SURPRISED", @"\b(wow|whoa|no way|amazing)\b" },
        new string[] { "THINKING", @"\b(hmm+|let me think|let me see|not sure|i wonder)\b" },
        new string[] { "SLEEPY", @"\b(sleepy|tired|yawn\w*|good night|bedtime)\b" },
        new string[] { "ANGRY", @"\b(grr+|annoying|annoyed|angry)\b" },
        new string[] { "SINGING", @"\b(la la|sing\w*)\b" },
        new string[] { "THUMBSUP", @"\b(you got this|well done|good job|thumbs up|nailed it)\b" },
        new string[] { "HIGHFIVE", @"\bhigh.?five\b" },
        new string[] { "GAMING", @"\b(game|gaming)\b" },
    };

    public static string FaceFor(string text) {
        string best = null;
        int at = -1;
        foreach (string[] c in Cues) {
            MatchCollection ms = Regex.Matches(text ?? "", c[1], RegexOptions.IgnoreCase);
            if (ms.Count > 0 && ms[ms.Count - 1].Index > at) {
                at = ms[ms.Count - 1].Index;
                best = c[0];
            }
        }
        return best;
    }

    // 0-4 for a slice of 16-bit little-endian PCM: how many thresholds its
    // RMS clears.
    public static int Level(byte[] pcm, int offset, int count) {
        int rms = Rms(pcm, offset, count);
        int level = 0;
        foreach (int t in Thresholds) if (rms >= t) level++;
        return level;
    }

    internal static int Rms(byte[] pcm, int offset, int count) {
        int n = count / 2;
        if (n == 0) return 0;
        double sum = 0;
        for (int i = 0; i < n; i++) {
            short s = (short)(pcm[offset + 2 * i] | (pcm[offset + 2 * i + 1] << 8));
            sum += (double)s * s;
        }
        return (int)Math.Sqrt(sum / n);
    }

    public void Start() {
        // Subscribed from this (background) thread, SystemEvents raises them
        // on a thread of its own, so they never wait on the window.
        SystemEvents.SessionSwitch += OnSessionSwitch;
        SystemEvents.PowerModeChanged += OnPowerMode;
        OpenAudio();
        if (lite) {
            Heard();
            setupDone = true;
            micThread = Spawn(MicLoop);
            pumpThread = Spawn(PumpLoop);
            return;
        }
        ws = new ClientWebSocket();
        // The key in a header, never in the URL: a URL ends up in logs and
        // error messages, a header does not.
        ws.Options.SetRequestHeader("x-goog-api-key", key);
        if (!ws.ConnectAsync(new Uri(url), cts.Token).Wait(15000))
            throw new TimeoutException("Gemini did not answer in 15 s");
        Send(setup);
        recvThread = Spawn(RecvLoop);
        micThread = Spawn(MicLoop);
        pumpThread = Spawn(PumpLoop);
    }

    // Start() off the caller's thread: connecting can take seconds, and the
    // window must not freeze meanwhile. A failure ends up in Log and Done.
    public void BeginStart() {
        startThread = Spawn(delegate {
            try {
                Start();
            } catch (Exception e) {
                // Kept as the reason too: a quota refusal can come at connect.
                CloseReason = e.GetBaseException().Message;
                string kind = Classify(CloseReason);
                // The window explains quota, busy and refused in its own words.
                Finish(kind, kind == "drop" ? "error: " + CloseReason : null);
            }
        });
    }

    // Ends the conversation from any thread: the window's timer sees Done
    // and calls Stop(), which closes the mic.
    void End(string why) { Finish("", why); }

    void Finish(string kind, string why) {
        if (Done) return;
        EndKind = kind;
        if (!string.IsNullOrEmpty(why)) Say(why);
        Done = true;
    }

    // Locked, logged off or switched away from: nobody is at the mic - or
    // the camera, which the window turns off when it sees Away.
    public volatile bool Away;

    void OnSessionSwitch(object sender, SessionSwitchEventArgs e) {
        if (e.Reason == SessionSwitchReason.SessionLock || e.Reason == SessionSwitchReason.SessionLogoff ||
            e.Reason == SessionSwitchReason.ConsoleDisconnect || e.Reason == SessionSwitchReason.RemoteDisconnect) {
            Away = true;
            End("This PC was locked - the conversation ended and the mic is off.");
        }
    }

    void OnPowerMode(object sender, PowerModeChangedEventArgs e) {
        if (e.Mode == PowerModes.Suspend) {
            Away = true;
            End("This PC went to sleep - the conversation ended and the mic is off.");
        }
    }

    void Heard() { Interlocked.Exchange(ref heardAt, Now()); }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WAVEINCAPS {
        public ushort wMid, wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public uint dwFormats;
        public ushort wChannels, wReserved1;
    }

    [DllImport("winmm.dll")] static extern int waveInGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)] static extern int waveInGetDevCapsW(IntPtr id, ref WAVEINCAPS caps, int size);

    // The microphones, in device-id order. Windows cuts names at 31 letters.
    public static string[] Mics() {
        int n = waveInGetNumDevs();
        string[] names = new string[n];
        for (int i = 0; i < n; i++) {
            WAVEINCAPS c = new WAVEINCAPS();
            names[i] = waveInGetDevCapsW(new IntPtr(i), ref c, Marshal.SizeOf(typeof(WAVEINCAPS))) == 0
                ? c.szPname : "Microphone " + (i + 1);
        }
        return names;
    }

    static Thread Spawn(ThreadStart f) {
        Thread t = new Thread(f);
        t.IsBackground = true;
        t.Start();
        return t;
    }

    void Say(string s) { Log.Enqueue(s); }

    // A face line to the companion, one per datagram.
    void Line(string s) {
        try {
            byte[] b = Encoding.ASCII.GetBytes(s);
            udp.Send(b, b.Length, new IPEndPoint(IPAddress.Loopback, facePort));
        } catch { }
    }

    void Face(string e) { Emotion = e; Line("E " + e); }

    // ClientWebSocket takes one send at a time; the mic and tool answers
    // come from different threads.
    // False when the socket is gone; the receive loop says why, once, so a
    // dropped connection is not reported again as a microphone fault.
    bool Send(string s) {
        byte[] b = Encoding.UTF8.GetBytes(s);
        lock (sendLock) {
            if (ws == null || ws.State != WebSocketState.Open) return false;
            try {
                ws.SendAsync(new ArraySegment<byte>(b), WebSocketMessageType.Text, true, cts.Token).Wait();
                return true;
            } catch (Exception) {
                return false;
            }
        }
    }

    // Something typed, into the same conversation: Buddy answers it out loud.
    public bool SendText(string text) {
        if (!setupDone || string.IsNullOrEmpty(text)) return false;
        Heard();
        Asked(text);
        if (lite) {
            if (asking) return false;
            Ask(null, text);
            return true;
        }
        Dictionary<string, object> t = new Dictionary<string, object>();
        t["text"] = text;
        Dictionary<string, object> m = new Dictionary<string, object>();
        m["realtimeInput"] = t;
        return Send(json.Serialize(m));
    }

    // ---- audio --------------------------------------------------------------

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    internal struct WAVEFORMATEX {
        public ushort wFormatTag, nChannels;
        public uint nSamplesPerSec, nAvgBytesPerSec;
        public ushort nBlockAlign, wBitsPerSample, cbSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct WAVEHDR {
        public IntPtr lpData;
        public uint dwBufferLength, dwBytesRecorded;
        public IntPtr dwUser;
        public uint dwFlags, dwLoops;
        public IntPtr lpNext, reserved;
    }

    internal static readonly int HDR = Marshal.SizeOf(typeof(WAVEHDR));
    internal static readonly int FLAGS = (int)Marshal.OffsetOf(typeof(WAVEHDR), "dwFlags");
    internal static readonly int RECORDED = (int)Marshal.OffsetOf(typeof(WAVEHDR), "dwBytesRecorded");

    [DllImport("winmm.dll")] internal static extern int waveInOpen(out IntPtr h, IntPtr dev, ref WAVEFORMATEX f, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] internal static extern int waveInPrepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] internal static extern int waveInUnprepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] internal static extern int waveInAddBuffer(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] internal static extern int waveInStart(IntPtr h);
    [DllImport("winmm.dll")] internal static extern int waveInReset(IntPtr h);
    [DllImport("winmm.dll")] internal static extern int waveInClose(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr h, IntPtr dev, ref WAVEFORMATEX f, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr h);

    internal static WAVEFORMATEX Pcm16(int rate) {
        WAVEFORMATEX f = new WAVEFORMATEX();
        f.wFormatTag = 1;          // WAVE_FORMAT_PCM
        f.nChannels = 1;
        f.nSamplesPerSec = (uint)rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = (uint)(rate * 2);
        return f;
    }

    // A header and its data, both in unmanaged memory: winmm keeps writing
    // to them after the call returns, and the garbage collector must not
    // move them meanwhile.
    internal static IntPtr NewHeader(byte[] data, int size) {
        WAVEHDR h = new WAVEHDR();
        h.lpData = Marshal.AllocHGlobal(size);
        h.dwBufferLength = (uint)size;
        if (data != null) Marshal.Copy(data, 0, h.lpData, size);
        IntPtr p = Marshal.AllocHGlobal(HDR);
        Marshal.StructureToPtr(h, p, false);
        return p;
    }

    internal static void FreeHeader(IntPtr p) {
        WAVEHDR h = (WAVEHDR)Marshal.PtrToStructure(p, typeof(WAVEHDR));
        Marshal.FreeHGlobal(h.lpData);
        Marshal.FreeHGlobal(p);
    }

    internal static bool IsDone(IntPtr p) { return (Marshal.ReadInt32(p, FLAGS) & WHDR_DONE) != 0; }

    void OpenAudio() {
        // WAVE_MAPPER is (UINT_PTR)-1: a pointer-sized -1, which a plain
        // int -1 is not on 64-bit Windows. CALLBACK_NULL: buffers are
        // polled for WHDR_DONE instead of a callback into managed code.
        WAVEFORMATEX fin = Pcm16(MIC_RATE), fout = Pcm16(OUT_RATE);
        int r = waveInOpen(out hwi, new IntPtr(MicDevice), ref fin, IntPtr.Zero, IntPtr.Zero, 0);
        if (r != 0) { hwi = IntPtr.Zero; throw new Exception("no microphone (waveInOpen error " + r + ")"); }
        r = waveOutOpen(out hwo, new IntPtr(-1), ref fout, IntPtr.Zero, IntPtr.Zero, 0);
        if (r != 0) { hwo = IntPtr.Zero; throw new Exception("no speakers (waveOutOpen error " + r + ")"); }
        inHdrs = new IntPtr[MIC_BUFS];
        for (int i = 0; i < MIC_BUFS; i++) {
            inHdrs[i] = NewHeader(null, MIC_RATE * 2 * MIC_MS / 1000);
            waveInPrepareHeader(hwi, inHdrs[i], HDR);
            waveInAddBuffer(hwi, inHdrs[i], HDR);
        }
        waveInStart(hwi);
    }

    bool Speaking() {
        lock (outLock) return heldBytes > 0 || Now() < playEnd + EchoHoldMs;
    }

    void MicLoop() {
        const string gone = "The microphone stopped (unplugged?) - the conversation ended. Pick a mic and tap to talk again.";
        int i = 0;
        long soundAt = Now();
        try {
            while (!Done) {
                // A mic that was pulled out either stops handing buffers
                // back or hands them back empty; both read as silence here.
                if (Now() - soundAt > MicStallMs) { End(gone); break; }
                IntPtr h = inHdrs[i];
                if (!IsDone(h)) { Thread.Sleep(10); continue; }
                int n = Marshal.ReadInt32(h, RECORDED);
                WAVEHDR w = (WAVEHDR)Marshal.PtrToStructure(h, typeof(WAVEHDR));
                byte[] pcm = new byte[n];
                Marshal.Copy(w.lpData, pcm, 0, n);
                // Back in the queue: unprepare, clear, prepare, add.
                waveInUnprepareHeader(hwi, h, HDR);
                Marshal.WriteInt32(h, FLAGS, 0);
                Marshal.WriteInt32(h, RECORDED, 0);
                waveInPrepareHeader(hwi, h, HDR);
                if (waveInAddBuffer(hwi, h, HDR) != 0) { End(gone); break; }
                i = (i + 1) % inHdrs.Length;
                if (n == 0) { Thread.Sleep(10); continue; }
                soundAt = Now();
                MicRms = Rms(pcm, 0, n);
                if (!setupDone) continue;
                if (lite) { Hear(pcm); continue; }
                // Half duplex: while Buddy talks, and a moment after, the
                // mic sends silence. Otherwise the speakers echo into it and
                // Buddy hears itself, takes that as being interrupted, and
                // stops mid-sentence.
                // ponytail: a headset has no echo and could stay full duplex
                // on its own; for now gemini.json "headset" says so.
                if ((HalfDuplex && Speaking()) || Muted) Array.Clear(pcm, 0, n);
                // A failed send is the connection, which the receive loop reports.
                Send("{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\""
                     + Convert.ToBase64String(pcm) + "\"}}}");
            }
        } catch (Exception e) {
            if (!Done) Say("microphone: " + e.GetBaseException().Message);
            Done = true;
        }
    }

    // A chunk of Buddy's voice, 24 kHz 16-bit mono. Starting from silence it
    // waits until PrebufferMs of it is in hand (or PrebufferMs has passed,
    // PumpLoop), like Mira's player: a reply that starts at once and then
    // waits on the next chunk stutters.
    void Play(byte[] pcm) {
        lock (outLock) {
            if (hwo == IntPtr.Zero || pcm.Length < 2) return;
            if (Now() >= playEnd) {
                if (heldBytes == 0) heldSince = Now();
                held.Add(pcm);
                heldBytes += pcm.Length;
                if (heldBytes < OUT_RATE * 2 * PrebufferMs / 1000) return;
                pcm = TakeHeld();
            }
            Write(pcm);
        }
    }

    // Everything held, as one piece. Under outLock.
    byte[] TakeHeld() {
        byte[] all = new byte[heldBytes];
        int at = 0;
        foreach (byte[] b in held) { Buffer.BlockCopy(b, 0, all, at, b.Length); at += b.Length; }
        held.Clear();
        heldBytes = 0;
        return all;
    }

    // Queued as its own buffer, and its mouth shapes put on the timeline at
    // the moment it will play. Under outLock.
    void Write(byte[] pcm) {
        {
            long now = Now();
            long start = Math.Max(now, playEnd);
            int slice = OUT_RATE * 2 * 50 / 1000;
            for (int off = 0, k = 0; off < pcm.Length; off += slice, k++) {
                int n = Math.Min(slice, pcm.Length - off);
                timeline.Add(new KeyValuePair<long, int>(start + LagMs + 50 * k, Level(pcm, off, n)));
            }
            playEnd = start + pcm.Length * 1000L / (OUT_RATE * 2);
            IntPtr h = NewHeader(pcm, pcm.Length);
            waveOutPrepareHeader(hwo, h, HDR);
            waveOutWrite(hwo, h, HDR);
            playing.Add(h);
        }
    }

    // Buddy was cut off: stop the sound now, and close the mouth with it.
    void Silence() {
        lock (outLock) {
            if (hwo != IntPtr.Zero) {
                waveOutReset(hwo);
                FreePlayed(true);
            }
            held.Clear();
            heldBytes = 0;
            timeline.Clear();
            playEnd = Now();
        }
    }

    void FreePlayed(bool all) {
        for (int i = playing.Count - 1; i >= 0; i--) {
            IntPtr h = playing[i];
            if (!all && !IsDone(h)) continue;
            waveOutUnprepareHeader(hwo, h, HDR);
            FreeHeader(h);
            playing.RemoveAt(i);
        }
    }

    // Every ~30 ms: give back played buffers, and move the mouth.
    void PumpLoop() {
        while (!Done) {
            int level;
            long now = Now();
            bool quiet;
            lock (outLock) {
                if (hwo != IntPtr.Zero) FreePlayed(false);
                // A short reply, or the network's pause: play what is held.
                if (heldBytes > 0 && now - heldSince >= PrebufferMs) Write(TakeHeld());
                quiet = heldBytes == 0 && now >= playEnd + EchoHoldMs;
                level = -1;
                while (timeline.Count > 0 && timeline[0].Key <= now) {
                    level = timeline[0].Value;
                    timeline.RemoveAt(0);
                }
                if (level < 0) level = timeline.Count == 0 && now >= playEnd + LagMs ? 0 : mouth;
                if (setupDone && now - Math.Max(Interlocked.Read(ref heardAt), playEnd) > IdleMs)
                    End("Nobody has said anything for " + IdleMs / 60000 + " minutes - the conversation ended " +
                        "to keep the mic off and save quota. Tap to talk again.");
            }
            // Speaking with no feeling in the words yet: neutral, but only
            // after a moment, so a cue in the next words is one chirp, not two.
            if (firstAudioAt > 0 && !faceSet && now - firstAudioAt > 1200) {
                faceSet = true;
                faceAt = now;
                if (!Held()) Face("NEUTRAL");
            }
            // Google is about to close this connection: end it in a pause -
            // not over Buddy's words or yours - and the window reconnects at
            // once (3.8 resumes the same conversation). At the latest in 20 s.
            if (goAwayAt > 0 && ((quiet && now - Interlocked.Read(ref heardAt) > 1500) || now - goAwayAt > 20000))
                Finish("goaway", null);
            // On a change, and an open mouth once a second as well: the
            // relay closes a mouth that has heard nothing for two.
            if (level != mouth || (level > 0 && now - mouthAt >= 1000)) {
                Line("V " + level);
                mouth = level;
                mouthAt = now;
                Mouth = level;
            }
            Thread.Sleep(30);
        }
    }

    // ---- the conversation ---------------------------------------------------

    void RecvLoop() {
        byte[] buf = new byte[65536];
        MemoryStream msg = new MemoryStream();
        try {
            while (!Done) {
                WebSocketReceiveResult r = ws.ReceiveAsync(new ArraySegment<byte>(buf), cts.Token).Result;
                if (r.MessageType == WebSocketMessageType.Close) {
                    // A bad model name, key or quota comes back here, and
                    // nowhere else.
                    CloseReason = ws.CloseStatusDescription ?? "";
                    string kind = Classify(CloseReason);
                    // The window explains quota, busy and refused itself.
                    Finish(kind, kind == "drop" ? "Gemini closed the session (" + ws.CloseStatus + "): " + CloseReason : null);
                    break;
                }
                // Text or binary frames, whichever the server sends: both
                // are UTF-8 JSON, sometimes split across frames.
                msg.Write(buf, 0, r.Count);
                if (!r.EndOfMessage) continue;
                string text = Encoding.UTF8.GetString(msg.ToArray());
                msg.SetLength(0);
                try {
                    Handle(json.DeserializeObject(text) as Dictionary<string, object>);
                } catch (Exception e) {
                    Say("could not read a message from Gemini: " + e.GetBaseException().Message);
                }
            }
        } catch (Exception e) {
            Finish("drop", "connection lost: " + e.GetBaseException().Message);
        }
        Finish("drop", null);
    }

    static Dictionary<string, object> Obj(Dictionary<string, object> d, string k) {
        object v;
        return d != null && d.TryGetValue(k, out v) ? v as Dictionary<string, object> : null;
    }

    static object[] Arr(Dictionary<string, object> d, string k) {
        object v;
        return d != null && d.TryGetValue(k, out v) ? v as object[] : null;
    }

    static string Str(Dictionary<string, object> d, string k) {
        object v;
        return d != null && d.TryGetValue(k, out v) && v != null ? v.ToString() : null;
    }

    static bool Flag(Dictionary<string, object> d, string k) {
        object v;
        return d != null && d.TryGetValue(k, out v) && v is bool && (bool)v;
    }

    void Listening() {
        if (listening) return;
        listening = true;
        Face("LISTENING");
    }

    void Handle(Dictionary<string, object> m) {
        if (m == null) return;
        if (m.ContainsKey("setupComplete")) {
            Heard();
            setupDone = true;
            Say("connected - talk to " + name + ". Ctrl+C ends it.");
        }
        Dictionary<string, object> va = Obj(m, "voiceActivity");
        if (va != null && Str(va, "type") == "ACTIVITY_START") Listening();
        Dictionary<string, object> ru = Obj(m, "sessionResumptionUpdate");
        if (ru != null && Flag(ru, "resumable") && Str(ru, "newHandle") != null) ResumeHandle = Str(ru, "newHandle");

        Dictionary<string, object> sc = Obj(m, "serverContent");
        if (sc != null) {
            if (Flag(sc, "interrupted")) {
                Silence();
                if (said.Length > 0) Say(name + ": " + said.ToString().Trim() + " -");
                said.Clear();
                faceSet = spoke = false;
                firstAudioAt = 0;
            }
            string t = Str(Obj(sc, "inputTranscription"), "text");
            if (!string.IsNullOrEmpty(t)) { heard.Append(t); Heard(); Asked(heard.ToString()); Listening(); }
            t = Str(Obj(sc, "outputTranscription"), "text");
            if (!string.IsNullOrEmpty(t)) { said.Append(t); FaceFromWords(); }
            object[] parts = Arr(Obj(sc, "modelTurn"), "parts");
            if (parts != null) {
                foreach (object o in parts) {
                    string data = Str(Obj(o as Dictionary<string, object>, "inlineData"), "data");
                    if (data == null) continue;
                    if (!spoke) {
                        firstAudioAt = Now();
                        // Your turn is over once Buddy answers: said now, not
                        // after the reply, so a face or trick you asked for
                        // plays with the answer (the window acts on it).
                        if (heard.Length > 0) { Say("you: " + heard.ToString().Trim()); heard.Clear(); }
                    }
                    spoke = true;
                    Play(Convert.FromBase64String(data));
                }
            }
            if (Flag(sc, "turnComplete")) {
                // A reply shorter than the hold plays now, not 120 ms late.
                lock (outLock) { if (heldBytes > 0) Write(TakeHeld()); }
                if (heard.Length > 0) Say("you: " + heard.ToString().Trim());
                if (said.Length > 0) Say(name + ": " + said.ToString().Trim());
                heard.Clear();
                said.Clear();
                listening = faceSet = spoke = false;
                firstAudioAt = 0;
            }
        }

        object[] calls = Arr(Obj(m, "toolCall"), "functionCalls");
        if (calls != null) {
            List<object> answers = new List<object>();
            foreach (object o in calls) {
                Dictionary<string, object> c = o as Dictionary<string, object>;
                if (c == null) continue;
                string fn = Str(c, "name");
                object result = Tool(fn, Obj(c, "args"));
                Dictionary<string, object> a = new Dictionary<string, object>();
                if (Str(c, "id") != null) a["id"] = Str(c, "id");
                a["name"] = fn;
                a["response"] = new Dictionary<string, object> { { "result", result } };
                answers.Add(a);
            }
            Dictionary<string, object> tr = new Dictionary<string, object>();
            tr["functionResponses"] = answers;
            Dictionary<string, object> reply = new Dictionary<string, object>();
            reply["toolResponse"] = tr;
            Send(json.Serialize(reply));
            SendLook();
        }

        // Google closes every connection after a while (~10-15 minutes).
        // PumpLoop ends this one in a pause and the window reconnects.
        if (m.ContainsKey("goAway") && goAwayAt == 0) goAwayAt = Now();
    }

    // ---- tools: things only this PC can do, each run when Gemini asks -------

    // For the window: timers ("timer\t<seconds>\t<label>", "cancel") and
    // camera pictures ("camera") it does itself.
    public readonly ConcurrentQueue<string> Requests = new ConcurrentQueue<string>();
    // Every picture sent to Gemini on a look, for the window to show in the
    // chat: what Buddy saw is what you see.
    public readonly ConcurrentQueue<byte[]> Shown = new ConcurrentQueue<byte[]>();

    // The window's live camera view: a frame now and then, which Gemini sees
    // as video. False when there is no Live conversation to send it to.
    public bool SendFrame(byte[] jpeg) {
        if (lite || !setupDone || jpeg == null) return false;
        Dictionary<string, object> v = new Dictionary<string, object>();
        v["mimeType"] = "image/jpeg";
        v["data"] = Convert.ToBase64String(jpeg);
        Dictionary<string, object> input = new Dictionary<string, object>();
        input["video"] = v;
        Dictionary<string, object> m = new Dictionary<string, object>();
        m["realtimeInput"] = input;
        return Send(json.Serialize(m));
    }

    // Something for Gemini to know without it showing as yours: the camera
    // going on or off, a timer ending.
    static readonly Regex PasteWords = new Regex(@"\b(paste|replace|type|insert|put|write)\b", RegexOptions.IgnoreCase);

    // A picture and a question about it, from "what's this?" with nothing
    // selected: the screen, then the question as its own turn - the one
    // order Gemini looks in (see SendLook). Off the caller's thread.
    public void ShowAndAsk(byte[] jpeg, string text) {
        if (lite) { SendText(text); return; }
        Spawn(delegate {
            if (!SendFrame(jpeg)) return;
            Say("Shared one snapshot of your screen with Gemini:");
            Shown.Enqueue(jpeg);
            Thread.Sleep(600);
            Tell(text);
        });
    }

    public bool Tell(string text) {
        if (!setupDone || string.IsNullOrEmpty(text)) return false;
        if (lite) return false;
        Dictionary<string, object> t = new Dictionary<string, object>();
        t["text"] = text;
        Dictionary<string, object> m = new Dictionary<string, object>();
        m["realtimeInput"] = t;
        return Send(json.Serialize(m));
    }

    // What the owner said or typed last, and when: look's guard.
    volatile string lastAsk = "";
    long askAt;
    public void Asked(string text) { lastAsk = text; Interlocked.Exchange(ref askAt, Now()); }

    static double NumArg(Dictionary<string, object> d, string k) {
        object v;
        if (d == null || !d.TryGetValue(k, out v) || v == null) return double.NaN;
        try { return Convert.ToDouble(v, CultureInfo.InvariantCulture); } catch { return double.NaN; }
    }

    object Tool(string fn, Dictionary<string, object> args) {
        try {
            switch (fn) {
                // The owner asked about this PC, or the weather: read now, sent once.
                case "get_pc_stats": return Machine.Stats();
                case "get_weather": return Weather.Now();
                case "control_media": {
                    string act = Str(args, "action");
                    double n = NumArg(args, "times");
                    // "Turn it up" is a noticeable step: five presses, 10 %.
                    int times = double.IsNaN(n) ? (act != null && act.StartsWith("volume_") ? 5 : 1) : (int)n;
                    return Media.Press(act, times) ? (object)"done" : "unknown action";
                }
                case "set_timer": {
                    double min = NumArg(args, "minutes");
                    // "At 5:30": the next time the clock says so.
                    DateTime at;
                    string when = Str(args, "at");
                    if (double.IsNaN(min) && when != null &&
                        DateTime.TryParse(when, CultureInfo.InvariantCulture, DateTimeStyles.None, out at)) {
                        DateTime due = DateTime.Today.Add(at.TimeOfDay);
                        if (due <= DateTime.Now) due = due.AddDays(1);
                        min = (due - DateTime.Now).TotalMinutes;
                    }
                    if (double.IsNaN(min) || min <= 0 || min > 24 * 60) return "give minutes (more than 0, at most 1440) or a time as HH:mm";
                    string label = (Str(args, "label") ?? "").Trim();
                    Requests.Enqueue("timer\t" + (int)Math.Round(min * 60) + "\t" + (label.Length > 0 ? label : "timer"));
                    return "set: it ends at " + DateTime.Now.AddMinutes(min).ToString("HH:mm", CultureInfo.InvariantCulture);
                }
                case "cancel_timers":
                    Requests.Enqueue("cancel");
                    return "all timers cancelled";
                case "remember": {
                    string fact = Str(args, "fact");
                    if (Memory.Remember(fact) == 0) return "nothing to remember";
                    Requests.Enqueue("note\tRemembered: " + fact.Trim());
                    return "remembered";
                }
                case "forget": {
                    int n = Memory.Forget(Str(args, "about"));
                    Requests.Enqueue("note\tForgot " + n + " remembered things.");
                    return "forgot " + n + " things";
                }
                case "look":
                    return Look(Str(args, "at"));
                case "paste_text": {
                    // Typing into the owner's app is theirs to ask for:
                    // "paste that", "replace it", "type it in".
                    string text = Str(args, "text");
                    if (Now() - Interlocked.Read(ref askAt) > 30000 || !PasteWords.IsMatch(lastAsk ?? ""))
                        return "not allowed: only when the owner asks you to paste, replace or type it";
                    if (string.IsNullOrEmpty(text)) return "nothing to paste";
                    Requests.Enqueue("paste\t" + text);
                    return "typed into the owner's app";
                }
            }
            return "unknown tool";
        } catch (Exception ex) {
            return "unavailable: " + ex.GetBaseException().Message;
        }
    }

    // One picture of the screen or the camera, sent as a video frame - and
    // only when the owner has just asked Buddy to look: Gemini deciding to
    // look by itself is refused here, whatever it calls.
    static readonly Regex LookWords = new Regex(
        @"\b(look|see|screen|camera|watch|show(ing)? you|holding|read (this|that)|what('s| is) (this|that))\b",
        RegexOptions.IgnoreCase);
    readonly AutoResetEvent frameReady = new AutoResetEvent(false);
    volatile byte[] frame;

    // The window's camera picture (or null: none to be had).
    public void ProvideFrame(byte[] jpeg) { frame = jpeg; frameReady.Set(); }

    public static bool MayLook(string asked) { return LookWords.IsMatch(asked ?? ""); }

    string Look(string at) {
        if (Now() - Interlocked.Read(ref askAt) > 30000 || !MayLook(lastAsk))
            return "not allowed: only when the owner asks you to look";
        bool cam = at == "camera";
        byte[] jpeg;
        if (cam) {
            frame = null;
            frameReady.Reset();
            Requests.Enqueue("camera");
            if (!frameReady.WaitOne(12000) || frame == null) return "the camera gave no picture";
            jpeg = frame;
        } else {
            jpeg = Snap.Screen();
        }
        lookAt = cam ? "camera" : "screen";
        lookJpeg = jpeg;
        return "taking the picture now; it comes next, with the owner's question again";
    }

    // The picture goes after the tool's answer, then the question again as a
    // turn of its own. Measured on 3.8 Live: a frame sent with or before the
    // tool's answer was not looked at ("my little eyes can't make it out"),
    // or no answer came at all; this order read the test screen word for word.
    string lookAt;
    byte[] lookJpeg;

    void SendLook() {
        byte[] jpeg = lookJpeg;
        lookJpeg = null;
        if (jpeg == null) return;
        Dictionary<string, object> v = new Dictionary<string, object>();
        v["mimeType"] = "image/jpeg";
        v["data"] = Convert.ToBase64String(jpeg);
        Dictionary<string, object> input = new Dictionary<string, object>();
        input["video"] = v;
        Dictionary<string, object> m = new Dictionary<string, object>();
        m["realtimeInput"] = input;
        if (!Send(json.Serialize(m))) return;
        // Every picture that leaves this PC is said in the window, and shown.
        Say("Shared one snapshot of your " + lookAt + " with Gemini:");
        Shown.Enqueue(jpeg);
        Thread.Sleep(600);
        Dictionary<string, object> t = new Dictionary<string, object>();
        t["text"] = "(The picture of my " + lookAt + " is in your video input now. " + lastAsk + ")";
        Dictionary<string, object> q = new Dictionary<string, object>();
        q["realtimeInput"] = t;
        Send(json.Serialize(q));
    }

    // A face or trick the owner asked for is playing: the faces read from
    // Buddy's words keep out of its way until then.
    long holdUntil;
    public void HoldFaces(int ms) { Interlocked.Exchange(ref holdUntil, Now() + ms); }
    bool Held() { return Now() < Interlocked.Read(ref holdUntil); }

    // The face follows the feeling in Buddy's words as they come, at most
    // once every FaceEveryMs.
    void FaceFromWords() {
        if (Held()) return;
        string f = FaceFor(said.ToString());
        if (f == null || !faces.Contains(f) || f == Emotion) return;
        if (faceSet && Now() - faceAt < FaceEveryMs) return;
        faceSet = true;
        faceAt = Now();
        Face(f);
    }

    // ---- Flash Lite -----------------------------------------------------------

    // Its ears: a sentence starts when the mic is loud enough - with the
    // 300 ms before it kept, so the first syllable is not lost - and ends
    // after VadHangMs of quiet. Not while Buddy talks or thinks: the
    // speakers would be heard as you.
    // ponytail: no barge-in here; Live has it, and this is the last resort.
    void Hear(byte[] pcm) {
        if (asking || Speaking()) {
            utter = null;
            preroll.Clear();
            return;
        }
        if (utter == null) {
            preroll.Enqueue(pcm);
            if (preroll.Count > 3) preroll.Dequeue();
            if (MicRms < VadOn) return;
            utter = new MemoryStream();
            foreach (byte[] p in preroll) utter.Write(p, 0, p.Length);
            preroll.Clear();
            quietMs = 0;
            speechMs = MIC_MS;
            Heard();
            Listening();
            return;
        }
        utter.Write(pcm, 0, pcm.Length);
        if (MicRms >= VadOn * 6 / 10) { quietMs = 0; speechMs += MIC_MS; Heard(); }
        else quietMs += MIC_MS;
        if (quietMs < VadHangMs && utter.Length < MIC_RATE * 2L * MaxUtterMs / 1000) return;
        byte[] clip = utter.ToArray();
        utter = null;
        // A cough or a click is not a sentence.
        if (speechMs >= 300) Ask(clip, null);
        else listening = false;
    }

    // One sentence (or a typed message) to Flash Lite, down $LITE_MODELS
    // until one answers: what it heard, the face, and the reply - which the
    // Windows voice then says.
    void Ask(byte[] clip, string text) {
        asking = true;
        Face("THINKING");
        Spawn(delegate {
            try {
                string why = "busy";
                Dictionary<string, object> got = null;
                string body = LiteBody(clip, text);
                foreach (string model in liteModels) {
                    if (Done) return;
                    int code;
                    string raw = Flash.Post(model, body, key, out code);
                    if (code == 200) {
                        got = LiteReply(raw);
                        if (got != null) break;
                        continue;
                    }
                    why = code == 429 ? "out of quota" : code == 0 ? "unreachable" : "busy";
                    // A bad request or key fails the same on every model.
                    if (code == 400 || code == 401 || code == 403) { why = "refusing (HTTP " + code + ")"; break; }
                }
                if (Done) return;
                if (got == null) {
                    Face("SAD");
                    Say("Gemini is " + why + " on every model right now - try again in a minute.");
                    return;
                }
                string you = text ?? (Str(got, "heard") ?? "").Trim();
                string reply = (Str(got, "reply") ?? "").Trim();
                string emo = (Str(got, "emotion") ?? "").ToUpperInvariant();
                // A typed message is already on screen.
                if (text == null && you.Length > 0) Say("you: " + you);
                // Flash Lite has no tools: "remember ..." is kept here.
                string kept = Memory.FromWords(you);
                if (kept != null) Requests.Enqueue("note\t" + kept);
                if (!Held()) Face(faces.Contains(emo) ? emo : "NEUTRAL");
                if (reply.Length == 0) return;
                Say(name + ": " + reply);
                liteTurns.Add(new string[] { you, reply });
                if (liteTurns.Count > 6) liteTurns.RemoveAt(0);
                Speak(reply);
            } catch (Exception e) {
                if (!Done) Say("Flash Lite: " + e.GetBaseException().Message);
            } finally {
                asking = false;
                listening = false;
                Heard();
            }
        });
    }

    static Dictionary<string, object> Part(string role, object part) {
        Dictionary<string, object> c = new Dictionary<string, object>();
        c["role"] = role;
        c["parts"] = new object[] { part };
        return c;
    }

    static Dictionary<string, object> TextPart(string text) {
        Dictionary<string, object> p = new Dictionary<string, object>();
        p["text"] = text;
        return p;
    }

    static Dictionary<string, object> Typed(string type) {
        Dictionary<string, object> t = new Dictionary<string, object>();
        t["type"] = type;
        return t;
    }

    string LiteBody(byte[] clip, string text) {
        List<object> contents = new List<object>();
        foreach (string[] turn in liteTurns) {
            contents.Add(Part("user", TextPart("The owner said: " + turn[0])));
            contents.Add(Part("model", TextPart(turn[1])));
        }
        if (clip != null) {
            Dictionary<string, object> audio = new Dictionary<string, object>();
            audio["mimeType"] = "audio/wav";
            audio["data"] = Convert.ToBase64String(Wav(clip, MIC_RATE));
            Dictionary<string, object> part = new Dictionary<string, object>();
            part["inlineData"] = audio;
            contents.Add(Part("user", part));
        } else {
            contents.Add(Part("user", TextPart("The owner types: " + text)));
        }
        Dictionary<string, object> emotion = Typed("STRING");
        emotion["enum"] = faceList;
        Dictionary<string, object> props = new Dictionary<string, object>();
        props["heard"] = Typed("STRING");
        props["emotion"] = emotion;
        props["reply"] = Typed("STRING");
        Dictionary<string, object> schema = Typed("OBJECT");
        schema["properties"] = props;
        schema["required"] = new string[] { "heard", "emotion", "reply" };
        Dictionary<string, object> config = new Dictionary<string, object>();
        config["responseMimeType"] = "application/json";
        config["responseSchema"] = schema;
        config["maxOutputTokens"] = 400;
        Dictionary<string, object> body = new Dictionary<string, object>();
        Dictionary<string, object> system = new Dictionary<string, object>();
        system["parts"] = new object[] { TextPart(litePrompt) };
        body["systemInstruction"] = system;
        body["contents"] = contents;
        body["generationConfig"] = config;
        return json.Serialize(body);
    }

    // {heard, emotion, reply} from a generateContent answer, or null.
    Dictionary<string, object> LiteReply(string raw) {
        try {
            object[] cands = Arr(json.DeserializeObject(raw) as Dictionary<string, object>, "candidates");
            if (cands == null || cands.Length == 0) return null;
            object[] parts = Arr(Obj(cands[0] as Dictionary<string, object>, "content"), "parts");
            if (parts == null || parts.Length == 0) return null;
            return json.DeserializeObject(Str(parts[0] as Dictionary<string, object>, "text")) as Dictionary<string, object>;
        } catch {
            return null;
        }
    }

    // The Windows voice, at the rate Gemini's audio comes at and through the
    // same player, so the mouth moves with it just as with Live.
    void Speak(string text) {
        string clean = Regex.Replace(text, @"[*_#`~]|[\uD800-\uDFFF]", "");
        using (System.Speech.Synthesis.SpeechSynthesizer s = new System.Speech.Synthesis.SpeechSynthesizer()) {
            try { s.SelectVoiceByHints(System.Speech.Synthesis.VoiceGender.Female); } catch { }
            MemoryStream ms = new MemoryStream();
            s.SetOutputToAudioStream(ms, new System.Speech.AudioFormat.SpeechAudioFormatInfo(
                OUT_RATE, System.Speech.AudioFormat.AudioBitsPerSample.Sixteen, System.Speech.AudioFormat.AudioChannel.Mono));
            s.Speak(clean);
            if (!Done) Play(ms.ToArray());
        }
    }

    public static byte[] Wav(byte[] pcm, int rate) {
        MemoryStream ms = new MemoryStream();
        BinaryWriter w = new BinaryWriter(ms);
        w.Write(Encoding.ASCII.GetBytes("RIFF"));
        w.Write(36 + pcm.Length);
        w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
        w.Write(16);
        w.Write((short)1);
        w.Write((short)1);
        w.Write(rate);
        w.Write(rate * 2);
        w.Write((short)2);
        w.Write((short)16);
        w.Write(Encoding.ASCII.GetBytes("data"));
        w.Write(pcm.Length);
        w.Write(pcm);
        w.Flush();
        return ms.ToArray();
    }

    public void Stop() {
        if (stopped) return;
        stopped = true;
        Done = true;
        // Static events: left subscribed, every past session would hear them.
        SystemEvents.SessionSwitch -= OnSessionSwitch;
        SystemEvents.PowerModeChanged -= OnPowerMode;
        // Said goodbye rather than dropped: Google lets go of the session
        // sooner, so a resume straight after is not "already connected".
        // Under sendLock: one send at a time, and the mic may be sending.
        try {
            lock (sendLock) {
                if (ws != null && ws.State == WebSocketState.Open)
                    ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None).Wait(500);
            }
        } catch { }
        try { cts.Cancel(); } catch { }
        try { if (ws != null) ws.Abort(); } catch { }
        // A Start() still connecting is cancelled above; wait for it, so it
        // cannot open the mic after the cleanup below.
        if (startThread != null) startThread.Join(16000);
        foreach (Thread t in new Thread[] { recvThread, micThread, pumpThread })
            if (t != null) t.Join(2000);
        if (hwi != IntPtr.Zero) {
            waveInReset(hwi);
            foreach (IntPtr h in inHdrs) { waveInUnprepareHeader(hwi, h, HDR); FreeHeader(h); }
            waveInClose(hwi);
            hwi = IntPtr.Zero;
        }
        lock (outLock) {
            if (hwo != IntPtr.Zero) {
                waveOutReset(hwo);
                FreePlayed(true);
                waveOutClose(hwo);
                hwo = IntPtr.Zero;
            }
            timeline.Clear();
        }
        Mouth = 0;
        Line("V 0");
        try { udp.Close(); } catch { }
    }
}
}
'@

# Both voice.cmd and the companion use it. Compiling runs csc.exe and takes a
# second or two, every start, so the result is kept beside the key, named by
# a hash of the source: a changed source compiles afresh, the same one loads.
function Add-VoiceTypes {
    if ('NexusVoice.BuddyVoice' -as [type]) { return }
    $sha = [Security.Cryptography.SHA256]::Create()
    $sum = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($VOICE_CS))
    $hash = -join ($sum[0..7] | ForEach-Object { $_.ToString('x2') })
    $dll = Join-Path $HOME_DIR "voice-$hash.dll"
    if (Test-Path $dll) {
        try { Add-Type -Path $dll -ErrorAction Stop; return } catch {}
    }
    try {
        New-Item -ItemType Directory -Force $HOME_DIR -ErrorAction Stop | Out-Null
        Add-Type -TypeDefinition $VOICE_CS -ReferencedAssemblies System.Web.Extensions, System.Speech, System.Drawing `
            -OutputAssembly $dll -ErrorAction Stop
        Add-Type -Path $dll -ErrorAction Stop
        # Older builds go; one still loaded by a running copy stays until next time.
        Get-ChildItem $HOME_DIR -Filter 'voice-*.dll' | Where-Object { $_.FullName -ne $dll } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    } catch {
        # Cannot write there (or another copy is writing it now): in memory.
        if (-not ('NexusVoice.BuddyVoice' -as [type])) {
            Add-Type -TypeDefinition $VOICE_CS -ReferencedAssemblies System.Web.Extensions, System.Speech, System.Drawing -ErrorAction Stop
        }
    }
}

# The companion's end: listen for voice.cmd's face lines. Warns once and
# carries on without it, like the chat inbox.
function Open-VoiceRelay {
    $script:relay = $null
    try {
        Add-VoiceTypes
    } catch {
        Write-Warning "voice: cannot build the relay - $(Get-Reason $_)"
        return
    }
    try {
        $script:relay = New-Object NexusVoice.VoiceRelay($VOICE_PORT)
    } catch {
        Write-Warning "voice: port $VOICE_PORT is in use - voice.cmd cannot move the face"
    }
    [NexusVoice.Machine]::Warm()
    Set-WeatherPlace (Get-GeminiConfig)
}

# The voice window's face shows what the dongle's does: Buddy's lines from
# this round go to it as well, in one datagram on 127.0.0.1. Nobody listening
# costs nothing - UDP is sent and forgotten.
$script:mirrorUdp = $null
function Send-BuddyMirror($lines) {
    $mine = @($lines | Where-Object { $_ -match '^[ESLVB] ' })
    if (-not $mine.Count) { return }
    try {
        if (-not $script:mirrorUdp) {
            $script:mirrorUdp = New-Object Net.Sockets.UdpClient(
                (New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, 0)))
        }
        $b = [Text.Encoding]::ASCII.GetBytes($mine -join "`n")
        [void]$script:mirrorUdp.Send($b, $b.Length, (New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, $MIRROR_PORT)))
    } catch {}
}

# The relay's queued lines out over Bluetooth, in one write. Only the last
# mouth position of a run counts - a mouth that fell behind skips ahead
# rather than replaying every shape - and every emotion goes, in order.
# $false if the link is gone.
function Send-VoiceBle($link) {
    $q = $script:relay.Ble
    if ($q.IsEmpty) { return $true }
    $got = New-Object System.Collections.Generic.List[string]
    $l = $null
    while ($q.TryDequeue([ref]$l)) {
        if ($l.StartsWith('V ') -and $got.Count -and $got[$got.Count - 1].StartsWith('V ')) {
            $got[$got.Count - 1] = $l
        } else {
            $got.Add($l)
        }
    }
    try {
        Send-BleLines $link $got
        Write-Verbose "bluetooth > $($got -join ' | ')"
        return $true
    } catch {
        return $false
    }
}

# The voice window: small and dark, popping up in the corner of the screen,
# with Buddy as a little creature that makes every face the dongle makes -
# all fifteen, its mouth moving with the voice - and the conversation below.
# Built from WPF, which every Windows has.
$VOICE_XAML = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Buddy" Width="360" Height="680" WindowStyle="None" AllowsTransparency="True"
        Background="Transparent" ResizeMode="NoResize" Topmost="True" WindowStartupLocation="Manual"
        FontFamily="Segoe UI Variable Text, Segoe UI" Foreground="#ECEBFF" UseLayoutRounding="True">
  <Window.Resources>
    <Style x:Key="Flat" TargetType="Button">
      <Setter Property="Foreground" Value="#ECEBFF"/>
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Focusable" Value="False"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Grid>
              <Border Background="{TemplateBinding Background}" CornerRadius="999"/>
              <Border x:Name="Hover" Background="White" CornerRadius="999" Opacity="0"/>
              <ContentPresenter Margin="{TemplateBinding Padding}" VerticalAlignment="Center"
                                HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"/>
            </Grid>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="Hover" Property="Opacity" Value="0.08"/>
              </Trigger>
              <Trigger Property="IsPressed" Value="True">
                <Setter TargetName="Hover" Property="Opacity" Value="0.16"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Field" TargetType="TextBox">
      <Setter Property="Foreground" Value="#ECEBFF"/>
      <Setter Property="CaretBrush" Value="#00E0FF"/>
      <Setter Property="SelectionBrush" Value="#00E0FF"/>
      <Setter Property="FontSize" Value="14"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="TextBox">
            <Border x:Name="Box" Background="#16FFFFFF" CornerRadius="21" BorderThickness="1" BorderBrush="#14FFFFFF">
              <ScrollViewer x:Name="PART_ContentHost" Margin="16,0,44,0" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsKeyboardFocused" Value="True">
                <Setter TargetName="Box" Property="BorderBrush" Value="#6600E0FF"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <SolidColorBrush x:Key="Ink" Color="#1B1838"/>
  </Window.Resources>

  <Grid x:Name="Root">
  <!-- The shadow on a layer of its own. On the card itself, every frame of
       the creature re-rendered it: a third of the window's CPU while talking. -->
  <Border x:Name="Shade" Margin="14" CornerRadius="28" Background="#0B0A1E" Opacity="0"
          RenderTransformOrigin="0.5,1" IsHitTestVisible="False">
    <Border.Effect>
      <DropShadowEffect BlurRadius="26" ShadowDepth="4" Direction="270" Opacity="0.55" Color="Black"/>
    </Border.Effect>
    <Border.RenderTransform>
      <ScaleTransform x:Name="PopShade" ScaleX="0.9" ScaleY="0.9"/>
    </Border.RenderTransform>
  </Border>
  <Border x:Name="Card" Margin="14" CornerRadius="28" BorderThickness="1" Opacity="0" RenderTransformOrigin="0.5,1">
    <Border.Background>
      <LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
        <GradientStop Color="#F8141236" Offset="0"/>
        <GradientStop Color="#F80B0A1E" Offset="1"/>
      </LinearGradientBrush>
    </Border.Background>
    <Border.BorderBrush>
      <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
        <GradientStop Color="#669C7BFF" Offset="0"/>
        <GradientStop Color="#2200E0FF" Offset="1"/>
      </LinearGradientBrush>
    </Border.BorderBrush>
    <Border.RenderTransform>
      <ScaleTransform x:Name="Pop" ScaleX="0.9" ScaleY="0.9"/>
    </Border.RenderTransform>
    <Grid Margin="18,12,16,16">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="236"/>
        <RowDefinition Height="*"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
      </Grid.RowDefinitions>

      <Grid x:Name="Header" Background="Transparent" Height="34">
        <StackPanel Orientation="Horizontal" VerticalAlignment="Center">
          <Ellipse x:Name="Dot" Width="9" Height="9" Margin="2,0,10,0" VerticalAlignment="Center"/>
          <TextBlock x:Name="Who" FontSize="16" FontWeight="Bold" VerticalAlignment="Center"/>
          <Rectangle Width="1" Height="14" Fill="#40FFFFFF" Margin="10,2,10,0" VerticalAlignment="Center"/>
          <TextBlock x:Name="Status" FontSize="13.5" Foreground="#A09CD0" VerticalAlignment="Center"/>
        </StackPanel>
        <StackPanel Orientation="Horizontal" HorizontalAlignment="Right">
          <Button x:Name="MinBtn" Style="{StaticResource Flat}" Width="30" Height="30" ToolTip="Minimize" AutomationProperties.Name="Minimize">
            <TextBlock Text="&#xE921;" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="10" Foreground="#A09CD0"/>
          </Button>
          <Button x:Name="CloseBtn" Style="{StaticResource Flat}" Width="30" Height="30" ToolTip="Close" AutomationProperties.Name="Close">
            <TextBlock Text="&#xE8BB;" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="10" Foreground="#A09CD0"/>
          </Button>
        </StackPanel>
      </Grid>

      <Grid x:Name="Stage" Grid.Row="1">
        <Ellipse x:Name="Glow" Width="236" Height="236" RenderTransformOrigin="0.5,0.5" IsHitTestVisible="False">
          <Ellipse.RenderTransform><ScaleTransform x:Name="GlowScale"/></Ellipse.RenderTransform>
        </Ellipse>
        <Canvas Width="220" Height="226" HorizontalAlignment="Center" VerticalAlignment="Top" IsHitTestVisible="False">
          <Ellipse Canvas.Left="14" Canvas.Top="58" Width="4" Height="4" Fill="#559C7BFF"/>
          <Ellipse Canvas.Left="200" Canvas.Top="150" Width="5" Height="5" Fill="#4400E0FF"/>
          <Ellipse Canvas.Left="30" Canvas.Top="170" Width="3" Height="3" Fill="#55D633FF"/>
          <Ellipse Canvas.Left="190" Canvas.Top="44" Width="3" Height="3" Fill="#559C7BFF"/>
          <Ellipse x:Name="Platform" Canvas.Left="42" Canvas.Top="200" Width="136" Height="20" StrokeThickness="2"/>
          <Ellipse x:Name="PlatformIn" Canvas.Left="78" Canvas.Top="206" Width="64" Height="8"/>

          <Canvas x:Name="Critter">
            <Canvas.RenderTransform>
              <TransformGroup>
                <RotateTransform x:Name="Tilt" CenterX="110" CenterY="150"/>
                <TranslateTransform x:Name="Bob"/>
              </TransformGroup>
            </Canvas.RenderTransform>

            <Canvas Canvas.Left="74" Canvas.Top="70">
              <Canvas.RenderTransform><RotateTransform x:Name="EarL" Angle="-18"/></Canvas.RenderTransform>
              <Path Data="M -17,4 C -19,-30 -8,-54 7,-60 C 16,-45 19,-20 17,4 Z" Stroke="#9C8BEA" StrokeThickness="1.5">
                <Path.Fill>
                  <LinearGradientBrush StartPoint="0,1" EndPoint="0.4,0">
                    <GradientStop Color="#C9BCFF" Offset="0"/>
                    <GradientStop Color="#EDE8FF" Offset="1"/>
                  </LinearGradientBrush>
                </Path.Fill>
              </Path>
              <Path Data="M -9,0 C -10,-26 -4,-42 5,-48 C 10,-35 12,-18 9,0 Z" Fill="#9900E0FF"/>
            </Canvas>
            <Canvas Canvas.Left="146" Canvas.Top="70">
              <Canvas.RenderTransform>
                <TransformGroup>
                  <ScaleTransform ScaleX="-1"/>
                  <RotateTransform x:Name="EarR" Angle="18"/>
                </TransformGroup>
              </Canvas.RenderTransform>
              <Path Data="M -17,4 C -19,-30 -8,-54 7,-60 C 16,-45 19,-20 17,4 Z" Stroke="#9C8BEA" StrokeThickness="1.5">
                <Path.Fill>
                  <LinearGradientBrush StartPoint="0,1" EndPoint="0.4,0">
                    <GradientStop Color="#C9BCFF" Offset="0"/>
                    <GradientStop Color="#EDE8FF" Offset="1"/>
                  </LinearGradientBrush>
                </Path.Fill>
              </Path>
              <Path Data="M -9,0 C -10,-26 -4,-42 5,-48 C 10,-35 12,-18 9,0 Z" Fill="#9900E0FF"/>
            </Canvas>

            <Ellipse Canvas.Left="64" Canvas.Top="138" Width="92" Height="68" Stroke="#9C8BEA" StrokeThickness="1.5">
              <Ellipse.Fill>
                <RadialGradientBrush GradientOrigin="0.4,0.3">
                  <GradientStop Color="#F4F1FF" Offset="0"/>
                  <GradientStop Color="#BBA9FF" Offset="1"/>
                </RadialGradientBrush>
              </Ellipse.Fill>
            </Ellipse>
            <Ellipse Canvas.Left="90" Canvas.Top="160" Width="40" Height="34" Fill="#55FFFFFF"/>

            <Path Data="M 110,46 L 110,28" Stroke="#B7A5FF" StrokeThickness="3" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            <Path x:Name="Spark" Data="M 0,-10 Q 1.6,-1.6 10,0 Q 1.6,1.6 0,10 Q -1.6,1.6 -10,0 Q -1.6,-1.6 0,-10 Z"
                  Canvas.Left="110" Canvas.Top="22">
              <Path.RenderTransform>
                <TransformGroup>
                  <ScaleTransform x:Name="SparkScale"/>
                  <RotateTransform x:Name="SparkSpin"/>
                </TransformGroup>
              </Path.RenderTransform>
            </Path>

            <Ellipse Canvas.Left="36" Canvas.Top="42" Width="148" Height="128" Stroke="#A897F2" StrokeThickness="1.5">
              <Ellipse.Fill>
                <RadialGradientBrush GradientOrigin="0.36,0.26" Center="0.45,0.4" RadiusX="0.65" RadiusY="0.65">
                  <GradientStop Color="#FFFFFF" Offset="0"/>
                  <GradientStop Color="#ECE6FF" Offset="0.45"/>
                  <GradientStop Color="#B9A7FF" Offset="1"/>
                </RadialGradientBrush>
              </Ellipse.Fill>
            </Ellipse>

            <Ellipse x:Name="CheekL" Canvas.Left="54" Canvas.Top="116" Width="22" Height="12" Fill="#FF8FB8" Opacity="0.55"/>
            <Ellipse x:Name="CheekR" Canvas.Left="144" Canvas.Top="116" Width="22" Height="12" Fill="#FF8FB8" Opacity="0.55"/>

            <Canvas>
              <Canvas.RenderTransform><TranslateTransform x:Name="Look"/></Canvas.RenderTransform>
              <Canvas x:Name="EyesOpen">
                <Canvas Canvas.Left="86" Canvas.Top="100">
                  <Canvas.RenderTransform><ScaleTransform x:Name="EyeL"/></Canvas.RenderTransform>
                  <Ellipse Canvas.Left="-9" Canvas.Top="-12" Width="18" Height="24" Fill="{StaticResource Ink}"/>
                  <Ellipse Canvas.Left="-5" Canvas.Top="-8" Width="7" Height="7" Fill="White"/>
                  <Ellipse Canvas.Left="2" Canvas.Top="4" Width="3" Height="3" Fill="#CCFFFFFF"/>
                </Canvas>
                <Canvas Canvas.Left="134" Canvas.Top="100">
                  <Canvas.RenderTransform><ScaleTransform x:Name="EyeR"/></Canvas.RenderTransform>
                  <Ellipse Canvas.Left="-9" Canvas.Top="-12" Width="18" Height="24" Fill="{StaticResource Ink}"/>
                  <Ellipse Canvas.Left="-5" Canvas.Top="-8" Width="7" Height="7" Fill="White"/>
                  <Ellipse Canvas.Left="2" Canvas.Top="4" Width="3" Height="3" Fill="#CCFFFFFF"/>
                </Canvas>
              </Canvas>
              <Path x:Name="EyesHappy" Data="M 77,104 Q 86,92 95,104 M 125,104 Q 134,92 143,104"
                    Stroke="{StaticResource Ink}" StrokeThickness="4.5" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
              <Path x:Name="EyesClosed" Data="M 77,99 Q 86,107 95,99 M 125,99 Q 134,107 143,99"
                    Stroke="{StaticResource Ink}" StrokeThickness="4" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
              <Canvas x:Name="EyesHeart">
                <Path Data="M 86,110 C 72,100 77,88 86,95 C 95,88 100,100 86,110 Z" Fill="#FF4F9A"/>
                <Path Data="M 134,110 C 120,100 125,88 134,95 C 143,88 148,100 134,110 Z" Fill="#FF4F9A"/>
              </Canvas>
              <Path x:Name="EyesX" Data="M 79,93 L 93,107 M 93,93 L 79,107 M 127,93 L 141,107 M 141,93 L 127,107"
                    Stroke="{StaticResource Ink}" StrokeThickness="4" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            </Canvas>

            <Canvas x:Name="Brows">
              <Canvas.RenderTransform><TranslateTransform x:Name="BrowLift"/></Canvas.RenderTransform>
              <Rectangle Canvas.Left="77" Canvas.Top="80" Width="18" Height="4.5" RadiusX="2.2" RadiusY="2.2" Fill="{StaticResource Ink}">
                <Rectangle.RenderTransform><RotateTransform x:Name="BrowL" CenterX="9" CenterY="2"/></Rectangle.RenderTransform>
              </Rectangle>
              <Rectangle Canvas.Left="125" Canvas.Top="80" Width="18" Height="4.5" RadiusX="2.2" RadiusY="2.2" Fill="{StaticResource Ink}">
                <Rectangle.RenderTransform><RotateTransform x:Name="BrowR" CenterX="9" CenterY="2"/></Rectangle.RenderTransform>
              </Rectangle>
            </Canvas>

            <Path x:Name="MouthSmile" Data="M 101,124 Q 110,133 119,124" Stroke="{StaticResource Ink}" StrokeThickness="3.5"
                  StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            <Canvas x:Name="MouthGrin">
              <Path Data="M 97,122 L 123,122 Q 123,138 110,138 Q 97,138 97,122 Z" Fill="#3A1640"/>
              <Ellipse Canvas.Left="104" Canvas.Top="130" Width="12" Height="6" Fill="#FF7AA8"/>
            </Canvas>
            <Ellipse x:Name="MouthOpen" Canvas.Left="102" Width="16" Height="8" Fill="#3A1640"/>
            <Path x:Name="MouthFrown" Data="M 101,131 Q 110,122 119,131" Stroke="{StaticResource Ink}" StrokeThickness="3.5"
                  StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            <Path x:Name="MouthFlat" Data="M 104,127 Q 111,125 117,127" Stroke="{StaticResource Ink}" StrokeThickness="3.5"
                  StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            <Path x:Name="MouthGrr" Data="M 99,128 L 104,124 L 110,128 L 116,124 L 121,128" Stroke="{StaticResource Ink}"
                  StrokeThickness="3.2" StrokeLineJoin="Round" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            <Path x:Name="MouthCat" Data="M 101,125 Q 105.5,131 110,126 Q 114.5,131 119,125" Stroke="{StaticResource Ink}"
                  StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>

            <Path x:Name="Tear" Data="M 0,0 C -5,7 -5,11 0,11 C 5,11 5,7 0,0 Z" Fill="#8FD8FF" Canvas.Left="80" Canvas.Top="112">
              <Path.RenderTransform><TranslateTransform x:Name="TearFall"/></Path.RenderTransform>
            </Path>

            <Canvas x:Name="PawL" Canvas.Left="80" Canvas.Top="176">
              <Canvas.RenderTransform>
                <TransformGroup>
                  <ScaleTransform x:Name="PawLScale"/>
                  <TranslateTransform x:Name="PawLMove"/>
                </TransformGroup>
              </Canvas.RenderTransform>
              <Ellipse Canvas.Left="-14" Canvas.Top="-11" Width="28" Height="22" Fill="#E6DFFF" Stroke="#9C8BEA" StrokeThickness="1.5"/>
              <Path Data="M -5,-3 L -5,3 M 0,-4 L 0,3 M 5,-3 L 5,3" Stroke="#B7A5FF" StrokeThickness="1.5" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            </Canvas>
            <Canvas x:Name="PawR" Canvas.Left="140" Canvas.Top="176">
              <Canvas.RenderTransform>
                <TransformGroup>
                  <ScaleTransform x:Name="PawRScale"/>
                  <TranslateTransform x:Name="PawRMove"/>
                </TransformGroup>
              </Canvas.RenderTransform>
              <Ellipse x:Name="Thumb" Canvas.Left="-5" Canvas.Top="-24" Width="10" Height="16" Fill="#E6DFFF" Stroke="#9C8BEA" StrokeThickness="1.5"/>
              <Ellipse Canvas.Left="-14" Canvas.Top="-11" Width="28" Height="22" Fill="#E6DFFF" Stroke="#9C8BEA" StrokeThickness="1.5"/>
              <Path Data="M -5,-3 L -5,3 M 0,-4 L 0,3 M 5,-3 L 5,3" Stroke="#B7A5FF" StrokeThickness="1.5" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
            </Canvas>

            <Canvas x:Name="PropPad">
              <Path Data="M 82,194 Q 78,180 92,180 L 128,180 Q 142,180 138,194 L 141,206 Q 142,214 133,212 L 124,204 L 96,204 L 87,212 Q 78,214 79,206 Z"
                    Fill="#2B2950" Stroke="#6C6AA8" StrokeThickness="1.5"/>
              <Path Data="M 93,189 L 93,197 M 89,193 L 97,193" Stroke="#9C9AC8" StrokeThickness="2.5" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
              <Ellipse Canvas.Left="122" Canvas.Top="186" Width="6" Height="6" Fill="#00E0FF"/>
              <Ellipse Canvas.Left="128" Canvas.Top="192" Width="6" Height="6" Fill="#D633FF"/>
            </Canvas>
            <Canvas x:Name="PropMic">
              <Rectangle Canvas.Left="140" Canvas.Top="132" Width="6" Height="24" RadiusX="3" RadiusY="3" Fill="#5B5890"/>
              <Ellipse Canvas.Left="134" Canvas.Top="118" Width="18" Height="18" Fill="#D7D5EE" Stroke="#8F8DB8" StrokeThickness="1.5"/>
            </Canvas>
          </Canvas>

          <Canvas x:Name="PropWaves">
            <Path Data="M 30,88 Q 22,104 30,120 M 20,80 Q 8,104 20,128" Stroke="#00E0FF" StrokeThickness="3"
                  StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
          </Canvas>
          <Canvas>
            <Canvas.RenderTransform><TranslateTransform x:Name="Float"/></Canvas.RenderTransform>
            <Canvas x:Name="PropHearts">
              <Path Data="M 30,62 C 20,55 23,46 30,51 C 37,46 40,55 30,62 Z" Fill="#FF4F9A"/>
              <Path Data="M 186,40 C 178,34 181,27 186,31 C 191,27 194,34 186,40 Z" Fill="#FF7AB8"/>
              <Path Data="M 196,96 C 190,92 192,87 196,90 C 200,87 202,92 196,96 Z" Fill="#D633FF"/>
            </Canvas>
            <Canvas x:Name="PropNotes">
              <TextBlock Canvas.Left="22" Canvas.Top="50" Text="&#x266A;" FontFamily="Segoe UI Symbol" FontSize="24" Foreground="#00E0FF"/>
              <TextBlock Canvas.Left="176" Canvas.Top="26" Text="&#x266B;" FontFamily="Segoe UI Symbol" FontSize="26" Foreground="#D633FF"/>
            </Canvas>
            <Canvas x:Name="PropZzz">
              <TextBlock Canvas.Left="164" Canvas.Top="38" Text="z" FontSize="18" FontWeight="Bold" Foreground="#B7A5FF"/>
              <TextBlock Canvas.Left="178" Canvas.Top="16" Text="Z" FontSize="24" FontWeight="Bold" Foreground="#D9CFFF"/>
            </Canvas>
            <Canvas x:Name="PropThink">
              <Ellipse Canvas.Left="166" Canvas.Top="52" Width="7" Height="7" Fill="#D9CFFF"/>
              <Ellipse Canvas.Left="176" Canvas.Top="36" Width="10" Height="10" Fill="#D9CFFF"/>
              <Ellipse Canvas.Left="188" Canvas.Top="16" Width="14" Height="14" Fill="#D9CFFF"/>
            </Canvas>
            <Canvas x:Name="PropSparkle">
              <Path Data="M 190,108 Q 191.2,114.8 198,116 Q 191.2,117.2 190,124 Q 188.8,117.2 182,116 Q 188.8,114.8 190,108 Z" Fill="#5CFFC0"/>
              <Path Data="M 28,128 Q 28.9,133.1 34,134 Q 28.9,134.9 28,140 Q 27.1,134.9 22,134 Q 27.1,133.1 28,128 Z" Fill="#FFE05C"/>
            </Canvas>
          </Canvas>
          <Path x:Name="PropAnger" Data="M 160,48 Q 166,48 166,42 M 172,42 Q 172,48 178,48 M 178,54 Q 172,54 172,60 M 166,60 Q 166,54 160,54"
                Stroke="#FF5C6C" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round"/>
          <TextBlock x:Name="PropShock" Canvas.Left="174" Canvas.Top="20" Text="!" FontSize="34" FontWeight="Black" Foreground="#FFE05C"/>
          <Canvas x:Name="PropBang">
            <Path Data="M 150,52 L 160,70 L 151,79 L 166,100" Stroke="#DDFFFFFF" StrokeThickness="1.6" StrokeLineJoin="Round"/>
            <Canvas Canvas.Left="110" Canvas.Top="40">
              <Canvas.RenderTransform><RotateTransform x:Name="Spin"/></Canvas.RenderTransform>
              <Path Data="M -34,-6 Q -33,-1 -28,0 Q -33,1 -34,6 Q -35,1 -40,0 Q -35,-1 -34,-6 Z" Fill="#FFE05C"/>
              <Path Data="M 34,-6 Q 35,-1 40,0 Q 35,1 34,6 Q 33,1 28,0 Q 33,-1 34,-6 Z" Fill="#FFE05C"/>
              <Path Data="M 0,-14 Q 1,-9 6,-8 Q 1,-7 0,-2 Q -1,-7 -6,-8 Q -1,-9 0,-14 Z" Fill="#5CFFC0"/>
            </Canvas>
          </Canvas>
        </Canvas>
        <Border x:Name="Caption" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,2,0,0"
                CornerRadius="12" Background="#F2F4F1FF" Padding="10,5" MaxWidth="150" Visibility="Collapsed">
          <TextBlock x:Name="CaptionText" FontFamily="Cascadia Mono, Consolas" FontWeight="Bold" FontSize="12"
                     Foreground="#1B1838" TextWrapping="Wrap"/>
        </Border>
      </Grid>

      <ScrollViewer x:Name="Scroll" Grid.Row="2" VerticalScrollBarVisibility="Hidden" Margin="0,4,0,4">
        <StackPanel x:Name="Talk"/>
      </ScrollViewer>

      <Grid Grid.Row="3" Margin="24,2,24,10" IsHitTestVisible="False">
        <Grid.ColumnDefinitions>
          <ColumnDefinition/>
          <ColumnDefinition Width="Auto"/>
          <ColumnDefinition/>
        </Grid.ColumnDefinitions>
        <Rectangle Height="1" VerticalAlignment="Center">
          <Rectangle.Fill>
            <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
              <GradientStop Color="#00B07BFF" Offset="0"/>
              <GradientStop Color="#88B07BFF" Offset="1"/>
            </LinearGradientBrush>
          </Rectangle.Fill>
        </Rectangle>
        <Path Grid.Column="1" Margin="10,0" Data="M 8,14 C -4,6 0,-2 8,4 C 16,-2 20,6 8,14 Z" Stroke="#B07BFF" StrokeThickness="1.6"/>
        <Rectangle Grid.Column="2" Height="1" VerticalAlignment="Center">
          <Rectangle.Fill>
            <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
              <GradientStop Color="#88B07BFF" Offset="0"/>
              <GradientStop Color="#00B07BFF" Offset="1"/>
            </LinearGradientBrush>
          </Rectangle.Fill>
        </Rectangle>
      </Grid>

      <Grid Grid.Row="4" Margin="0,0,0,12">
        <TextBox x:Name="Msg" Style="{StaticResource Field}" Height="42" MaxLength="500" AutomationProperties.Name="Message"/>
        <TextBlock x:Name="Hint" FontSize="14" Foreground="#77749F" Margin="18,0,44,0"
                   VerticalAlignment="Center" IsHitTestVisible="False" TextTrimming="CharacterEllipsis"/>
        <Button x:Name="SendBtn" Style="{StaticResource Flat}" Width="32" Height="32" Margin="0,0,5,0"
                HorizontalAlignment="Right" Background="#00E0FF" ToolTip="Send" AutomationProperties.Name="Send" Visibility="Collapsed">
          <TextBlock Text="&#xE724;" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="13"
                     Foreground="#0B0B1E" HorizontalAlignment="Center"/>
        </Button>
      </Grid>

      <Grid Grid.Row="5">
        <Button x:Name="SetBtn" Style="{StaticResource Flat}" Background="#16FFFFFF" Height="50" Margin="0,0,78,0"
                HorizontalContentAlignment="Left" Padding="16,0,16,0" ToolTip="Microphone and settings" AutomationProperties.Name="Settings">
          <Grid>
            <Grid.ColumnDefinitions>
              <ColumnDefinition Width="Auto"/>
              <ColumnDefinition Width="Auto"/>
              <ColumnDefinition Width="*"/>
              <ColumnDefinition Width="Auto"/>
            </Grid.ColumnDefinitions>
            <TextBlock Text="&#xE9E9;" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="16"
                       Foreground="#00E0FF" VerticalAlignment="Center"/>
            <Rectangle Grid.Column="1" Width="1" Height="18" Fill="#30FFFFFF" Margin="12,0"/>
            <TextBlock x:Name="MicName" Grid.Column="2" FontSize="13.5" TextTrimming="CharacterEllipsis" VerticalAlignment="Center"/>
            <TextBlock Grid.Column="3" Text="&#xE70D;" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="10"
                       Foreground="#00E0FF" VerticalAlignment="Center" Margin="10,1,0,0"/>
          </Grid>
        </Button>
        <Ellipse x:Name="TalkHalo" Width="74" Height="74" HorizontalAlignment="Right" Margin="0,0,-3,0" IsHitTestVisible="False"/>
        <Button x:Name="TalkBtn" Style="{StaticResource Flat}" Width="64" Height="64" HorizontalAlignment="Right" Margin="0,0,2,0"
                AutomationProperties.Name="Talk">
          <TextBlock x:Name="TalkIcon" FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets" FontSize="24" HorizontalAlignment="Center"/>
        </Button>
      </Grid>
    </Grid>
  </Border>
  </Grid>
</Window>
'@

# Each face the dongle has, as a pose: which eyes and mouth, where the ears,
# paws and head go, and what floats around it.
$VOICE_POSES = @{
    NEUTRAL   = @{ eyes = 'EyesOpen'; mouth = 'MouthSmile'; ears = -18 }
    HAPPY     = @{ eyes = 'EyesHappy'; mouth = 'MouthGrin'; ears = -12; props = @('PropSparkle'); blush = 0.85 }
    SAD       = @{ eyes = 'EyesOpen'; eye = 0.85; brows = @(-16, 16, 0); mouth = 'MouthFrown'; ears = -64; props = @('Tear'); tilt = -3 }
    ANGRY     = @{ eyes = 'EyesOpen'; eye = 0.8; brows = @(22, -22, 3); mouth = 'MouthGrr'; ears = -42; props = @('PropAnger') }
    SURPRISED = @{ eyes = 'EyesOpen'; eye = 1.3; brows = @(0, 0, -8); mouth = 'MouthOpen'; open = 13; ears = -4; props = @('PropShock') }
    SLEEPY    = @{ eyes = 'EyesClosed'; mouth = 'MouthFlat'; ears = -54; props = @('PropZzz'); tilt = 5 }
    LOVE      = @{ eyes = 'EyesHeart'; mouth = 'MouthGrin'; ears = -14; props = @('PropHearts'); blush = 1.0 }
    THINKING  = @{ eyes = 'EyesOpen'; brows = @(0, -14, -3); mouth = 'MouthFlat'; ears = -20; props = @('PropThink'); tilt = -6; look = @(4, -4) }
    LISTENING = @{ eyes = 'EyesOpen'; eye = 1.05; mouth = 'MouthSmile'; ears = -4; props = @('PropWaves'); tilt = 6 }
    SINGING   = @{ eyes = 'EyesClosed'; mouth = 'MouthOpen'; open = 16; ears = -14; props = @('PropNotes', 'PropMic'); pawR = @(-4, -32); tilt = -4 }
    HIGHFIVE  = @{ eyes = 'EyesHappy'; mouth = 'MouthGrin'; ears = -10; pawR = @(32, -84); pawScale = 1.35; props = @('PropSparkle') }
    HANDSUP   = @{ eyes = 'EyesOpen'; eye = 1.15; brows = @(0, 0, -6); mouth = 'MouthOpen'; open = 11; ears = -6; pawL = @(-38, -112); pawR = @(38, -112) }
    THUMBSUP  = @{ eyes = 'EyesHappy'; mouth = 'MouthGrin'; ears = -12; pawR = @(26, -46); thumb = $true; props = @('PropSparkle') }
    GAMING    = @{ eyes = 'EyesOpen'; mouth = 'MouthCat'; ears = -22; props = @('PropPad'); pawL = @(16, 14); pawR = @(-16, 14); look = @(0, 4) }
    BANG      = @{ eyes = 'EyesX'; mouth = 'MouthOpen'; open = 9; ears = -50; props = @('PropBang'); tilt = 8 }
}
# Everything a pose switches on or off.
$VOICE_PARTS = @('EyesOpen', 'EyesHappy', 'EyesClosed', 'EyesHeart', 'EyesX', 'Brows', 'MouthSmile', 'MouthGrin',
                 'MouthOpen', 'MouthFrown', 'MouthFlat', 'MouthGrr', 'MouthCat', 'Tear', 'Thumb', 'PropHearts',
                 'PropNotes', 'PropZzz', 'PropThink', 'PropWaves', 'PropMic', 'PropPad', 'PropBang', 'PropAnger',
                 'PropShock', 'PropSparkle')
# The glow's colour for each face; the rest are magenta, NEXUS's own.
$VOICE_MOODS = @{
    HAPPY = '#5CFFC0'; LOVE = '#FF5CA8'; SAD = '#5C8CFF'; ANGRY = '#FF5C6C'; SLEEPY = '#8A88C8'
    THINKING = '#FFC85C'; SURPRISED = '#FFE05C'; LISTENING = '#00E0FF'; SINGING = '#D633FF'
    HIGHFIVE = '#5CFFC0'; HANDSUP = '#FFC85C'; THUMBSUP = '#5CFFC0'; GAMING = '#00E0FF'; BANG = '#FF5C6C'
}

if ($Voice) {
    try {
        Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
    } catch {
        Write-Output "voice: WPF is not available - $(Get-Reason $_)"
        return
    }

    # One window at a time: two would share the mic and talk over each other.
    # A second double-click brings the first one forward instead.
    if (-not $Shot) {
        $first = Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'pwsh.exe'" |
            Where-Object { $_.CommandLine -like '*nexus_host.ps1*-Voice*' -and $_.CommandLine -notlike '*-Shot*' -and
                           $_.ProcessId -ne $PID } |
            Select-Object -First 1
        if ($first) {
            try {
                Add-Type -AssemblyName Microsoft.VisualBasic
                [Microsoft.VisualBasic.Interaction]::AppActivate([int]$first.ProcessId)
            } catch {}
            return
        }
    }

    # The window has no console behind it, so anything that stops it opening
    # is shown in a message box rather than lost.
    try {
    $gem = Get-GeminiConfig
    $p = Get-Content (Get-PersonaPath) -Raw -ErrorAction Stop | ConvertFrom-Json
    # Get-BuddyCommand reads the faces and tricks from here.
    $script:persona = $p
    $model = if ($gem.liveModel) { $gem.liveModel } else { $LIVE_MODEL }
    $speaker = if ($gem.voice) { $gem.voice } else { $LIVE_VOICE }
    $faces = [string[]]@($p.emotions | ForEach-Object { "$_".ToUpper() })
    # The chain this window walks down: gemini.json's model if it names one,
    # the Live models, then Flash Lite ('lite').
    $script:tiers = @(@(@($model) + $LIVE_CHAIN | Select-Object -Unique) + 'lite')
    # What the tools are for. The prompt itself is built afresh for every
    # connection, so a fact remembered a minute ago is in the next one.
    $toolsHint = @(
        'When the owner asks about their computer - CPU, memory, disk, battery, uptime, what is using memory - call get_pc_stats and answer from it in plain, friendly words.'
        'For the weather, call get_weather and answer from it briefly.'
        'To play, pause or skip music or change the volume, call control_media.'
        'For a timer or a reminder, call set_timer: minutes for "in 20 minutes" (a pomodoro is 25), or at as HH:mm for "at 5:30 pm"; cancel_timers stops them all. When one ends you will be told: say so.'
        'The owner can turn on a live camera view; while it is on you see their camera every few seconds and can talk about it.'
        'When the owner asks you to remember something, call remember with the fact in a short sentence; forget when they ask you to forget it.'
        'Only when the owner asks you to look at their screen or their camera (or at something they are holding), call look - never on your own.'
        # The persona's never-list keeps Buddy off screens and documents - right
        # unasked, but measured: it then refused to read a screen it was shown.
        'When you have looked because the owner asked, describe and read what you see plainly; your rule about screens and documents is only about not bringing them up yourself.'
        'When the owner asks about text they selected, answer about it plainly and briefly; if they then ask you to paste, replace or type something (a fix, a rewrite, a translation), call paste_text with exactly that text.'
    ) -join "`n"
    # Flash Lite hears the owner in a clip and answers as JSON; it has no
    # tools, so the weather, timers and looking wait for a Live model.
    $liteHint = 'The owner speaks to you in the audio. "heard" is exactly what they said, "reply" is what you say ' +
        'back out loud, and "emotion" is the face that fits your reply.'
    # One setup per connection: the model, a resumption handle for 3.8 (it
    # keeps the conversation), or the conversation so far for any other.
    # No Google Search: with it the free tier refuses 3.8 and 3.1 Live at
    # setup ("You exceeded your current quota"), measured 2026-11-25. No
    # set_face either: the face comes from the words (BuddyVoice.FaceFor).
    function Get-VoiceSetup([string]$m, [string]$handle, [string[]]$recent) {
        # The time, so "remind me at 5" and "in an hour" mean something.
        $text = (Get-BuddyPrompt $p -Spoken) + "`n" + $toolsHint + "`n" +
            "It is now $([datetime]::Now.ToString('dddd d MMMM, HH:mm', [Globalization.CultureInfo]::InvariantCulture))."
        if ($recent) {
            $text += "`n`nThe conversation so far, which carries on now - do not greet again:`n" + ($recent -join "`n")
        }
        @{ setup = @{
            model = "models/$m"
            generationConfig = @{
                responseModalities = @('AUDIO')
                speechConfig = @{ voiceConfig = @{ prebuiltVoiceConfig = @{ voiceName = $speaker } } }
                # Screens are small text: a look reads them at full detail.
                # Pictures go only on request, so this costs nothing otherwise.
                mediaResolution = 'MEDIA_RESOLUTION_HIGH'
            }
            systemInstruction = @{ parts = @(@{ text = $text }) }
            # Each runs on this PC when Gemini calls it (BuddyVoice.Tool):
            # nothing here costs quota beyond the call itself.
            tools = @(@{ functionDeclarations = @(
                @{
                    name = 'get_pc_stats'
                    description = "The owner's computer right now: CPU load and model, memory, system disk, battery, uptime, Windows version and the apps using the most memory"
                    behavior = 'BLOCKING'
                },
                @{
                    name = 'get_weather'
                    description = "The weather where the owner is: now, and today's and tomorrow's forecast"
                    behavior = 'BLOCKING'
                },
                @{
                    name = 'control_media'
                    description = "Press a media key on the owner's PC: play/pause, next or previous track, volume up or down (2 % a press), mute"
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{
                            action = @{ type = 'STRING'; enum = @([NexusVoice.Media]::Actions) }
                            times = @{ type = 'INTEGER'; description = 'How many presses; volume defaults to 5' }
                        }
                        required = @('action')
                    }
                },
                @{
                    name = 'set_timer'
                    description = 'Start a timer or reminder on the PC; when it ends the owner is told'
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{
                            minutes = @{ type = 'NUMBER'; description = 'How long, in minutes (or give at)' }
                            at = @{ type = 'STRING'; description = 'Or a time of day, 24-hour HH:mm' }
                            label = @{ type = 'STRING'; description = 'What it is for, in a few words' }
                        }
                    }
                },
                @{
                    name = 'cancel_timers'
                    description = 'Stop every timer'
                    behavior = 'BLOCKING'
                },
                @{
                    name = 'remember'
                    description = 'Remember a fact about the owner from now on, across days'
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{ fact = @{ type = 'STRING'; description = 'The fact, in one short sentence' } }
                        required = @('fact')
                    }
                },
                @{
                    name = 'forget'
                    description = 'Forget every remembered fact that mentions this ("everything" forgets all)'
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{ about = @{ type = 'STRING' } }
                        required = @('about')
                    }
                },
                @{
                    name = 'look'
                    description = "Take one picture of the owner's screen or camera to look at. Only when the owner asks you to look."
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{ at = @{ type = 'STRING'; enum = @('screen', 'camera') } }
                        required = @('at')
                    }
                },
                @{
                    name = 'paste_text'
                    description = "Type text into the app the owner is working in, where their selection was (it replaces it). Only when they ask you to paste, replace or type it."
                    behavior = 'BLOCKING'
                    parameters = @{
                        type = 'OBJECT'
                        properties = @{ text = @{ type = 'STRING'; description = 'Exactly the text to type' } }
                        required = @('text')
                    }
                }
            ) })
            inputAudioTranscription = @{}
            outputAudioTranscription = @{}
            sessionResumption = if ($handle) { @{ handle = $handle } } else { @{} }
            contextWindowCompression = @{ slidingWindow = @{}; triggerTokens = $LIVE_TRIGGER_TOKENS }
        } } | ConvertTo-Json -Depth 12 -Compress
    }

    # A compile failure is shown in the window, which still opens.
    $script:buildError = $null
    $script:mics = @()
    try {
        Add-VoiceTypes
        [NexusVoice.Memory]::Path = $MEMORY_FILE
        # Windows' location can take seconds: looked up off this thread, so
        # the window is not held back from showing (Weather's statics are
        # the same in every runspace of this process).
        if (-not $Shot) {
            if ($gem.city) {
                [NexusVoice.Weather]::City = $gem.city
            } else {
                $loc = [powershell]::Create()
                [void]$loc.AddScript({
                    try {
                        Add-Type -AssemblyName System.Runtime.WindowsRuntime
                        $op = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
                            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
                            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
                        [Windows.Devices.Geolocation.Geolocator, Windows.Devices.Geolocation, ContentType = WindowsRuntime] | Out-Null
                        $t = $op.MakeGenericMethod([Windows.Devices.Geolocation.Geoposition]).Invoke($null,
                            @((New-Object Windows.Devices.Geolocation.Geolocator).GetGeopositionAsync()))
                        if ($t.Wait(8000)) {
                            [NexusVoice.Weather]::Lat = $t.Result.Coordinate.Point.Position.Latitude
                            [NexusVoice.Weather]::Lon = $t.Result.Coordinate.Point.Position.Longitude
                        }
                    } catch {}
                })
                [void]$loc.BeginInvoke()
            }
        }
        $script:mics = @([NexusVoice.BuddyVoice]::Mics())
        [NexusVoice.BuddyVoice]::HalfDuplex = -not $gem.headset
        # Not $chat: PowerShell names ignore case, and that is the -Chat switch.
        $script:chatClient = New-Object NexusVoice.BuddyChat($CHAT_PORT)
        # Faces and tricks from a conversation go to the same inbox, but their
        # answers come back here, apart, and are never shown: replies can
        # arrive in any order, and must not take a typed chat's place.
        $script:cmdClient = New-Object NexusVoice.BuddyChat($CHAT_PORT)
        if (-not $Shot) { [NexusVoice.Machine]::Warm() }
    } catch {
        $script:buildError = Get-Reason $_
        $script:chatClient = $null
        $script:cmdClient = $null
    }
    # What the companion shows on the dongle, so this face can show it too.
    $script:mirror = $null
    if (-not $script:buildError -and -not $Shot) {
        try { $script:mirror = New-Object NexusVoice.BuddyMirror($MIRROR_PORT) } catch {}
    }

    # Which mic, remembered by name beside the key: device numbers shift
    # when a headset is plugged in, names do not.
    $VOICE_PREFS = Join-Path $HOME_DIR 'voice.json'
    # One setting in voice.json read, or written with the others kept.
    function Get-VoicePref([string]$name) {
        try { "$((Get-Content $VOICE_PREFS -Raw -ErrorAction Stop | ConvertFrom-Json).$name)" } catch { '' }
    }
    function Set-VoicePref([string]$name, $value) {
        $prefs = @{}
        try {
            (Get-Content $VOICE_PREFS -Raw -ErrorAction Stop | ConvertFrom-Json).PSObject.Properties |
                ForEach-Object { $prefs[$_.Name] = $_.Value }
        } catch {}
        $prefs[$name] = $value
        try { $prefs | ConvertTo-Json | Set-Content $VOICE_PREFS -Encoding utf8 } catch {}
    }
    $script:micId = -1
    try {
        $saved = "$((Get-Content $VOICE_PREFS -Raw -ErrorAction Stop | ConvertFrom-Json).mic)"
        $script:micId = [array]::IndexOf([string[]]$script:mics, $saved)
    } catch {}

    $w = [Windows.Markup.XamlReader]::Parse($VOICE_XAML)
    $ui = @{}
    foreach ($n in @('Root', 'Shade', 'PopShade', 'Card', 'Pop', 'Header', 'Dot', 'Who', 'Status', 'MinBtn', 'CloseBtn', 'Stage', 'Glow', 'GlowScale',
                     'Platform', 'PlatformIn', 'Critter', 'Tilt', 'Bob', 'EarL', 'EarR', 'Spark', 'SparkScale',
                     'SparkSpin', 'CheekL', 'CheekR', 'Look', 'EyeL', 'EyeR', 'BrowLift', 'BrowL', 'BrowR',
                     'TearFall', 'PawLMove', 'PawRMove', 'PawLScale', 'PawRScale', 'Float', 'Spin', 'Caption',
                     'CaptionText', 'Scroll', 'Talk', 'Msg', 'Hint', 'SendBtn', 'SetBtn', 'MicName', 'TalkBtn',
                     'TalkHalo', 'TalkIcon') + $VOICE_PARTS) {
        $ui[$n] = $w.FindName($n)
    }
    $w.Title = "$($p.name)"
    $ui.Who.Text = "$($p.name)"

    # Parsed once each: the face asks for a colour 30 times a second.
    $script:colors = @{}
    function ConvertTo-VoiceColor([string]$hex) {
        if (-not $script:colors.ContainsKey($hex)) {
            $script:colors[$hex] = [Windows.Media.ColorConverter]::ConvertFromString($hex)
        }
        $script:colors[$hex]
    }
    function New-VoiceBrush([string]$hex) { New-Object Windows.Media.SolidColorBrush (ConvertTo-VoiceColor $hex) }
    function New-VoiceGradient([string]$a, [string]$b) {
        $g = New-Object Windows.Media.LinearGradientBrush((ConvertTo-VoiceColor $a), (ConvertTo-VoiceColor $b), 45.0)
        $g
    }

    # Buddy's little face, drawn here rather than shipped: the taskbar icon
    # and the avatar beside each of its bubbles.
    function New-VoiceIcon([int]$px) {
        $dv = New-Object Windows.Media.DrawingVisual
        $dc = $dv.RenderOpen()
        $s = $px / 64.0
        $dc.PushTransform((New-Object Windows.Media.ScaleTransform($s, $s)))
        $fur = New-VoiceBrush '#E4DCFF'
        $edge = New-Object Windows.Media.Pen((New-VoiceBrush '#9C8BEA'), 2)
        $ink = New-VoiceBrush '#1B1838'
        $dc.DrawEllipse((New-VoiceBrush '#1E1A44'), (New-Object Windows.Media.Pen((New-VoiceBrush '#7C5CFF'), 3)),
            (New-Object Windows.Point(32, 32)), 30, 30)
        $dc.DrawGeometry($fur, $edge, [Windows.Media.Geometry]::Parse('M 14,30 C 11,18 15,10 21,8 C 25,14 27,22 26,28 Z'))
        $dc.DrawGeometry($fur, $edge, [Windows.Media.Geometry]::Parse('M 50,30 C 53,18 49,10 43,8 C 39,14 37,22 38,28 Z'))
        $dc.DrawEllipse($fur, $edge, (New-Object Windows.Point(32, 37)), 19, 16)
        $dc.DrawEllipse($ink, $null, (New-Object Windows.Point(25.5, 36)), 3, 4)
        $dc.DrawEllipse($ink, $null, (New-Object Windows.Point(38.5, 36)), 3, 4)
        $dc.DrawGeometry($null, (New-Object Windows.Media.Pen($ink, 2)), [Windows.Media.Geometry]::Parse('M 28.5,43 Q 32,46 35.5,43'))
        $dc.Pop()
        $dc.Close()
        $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap($px, $px, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
        $bmp.Render($dv)
        $bmp.Freeze()
        $bmp
    }
    $w.Icon = New-VoiceIcon 64
    $script:avatar = New-VoiceIcon 56

    # Brushes made here, not in the XAML, so they can change every frame.
    $fx = @{
        mood = New-VoiceBrush '#00E0FF'; inner = New-Object Windows.Media.GradientStop
        outer = New-Object Windows.Media.GradientStop; platIn = New-VoiceBrush '#5500E0FF'
        mouth = 0.0; glow = 0.0; eye = 1.0; r = 110.0; g = 108.0; b = 154.0
        earL = -18.0; tilt = 0.0; lx = 0.0; ly = 0.0; plx = 0.0; ply = 0.0; prx = 0.0; pry = 0.0; ps = 1.0
        bl = 0.0; br = 0.0; lift = 0.0; blush = 0.55
        blinkAt = (Get-Date).AddSeconds(2); blinkEnd = [datetime]::MinValue; settled = $false; pose = ''
        posed = $false; lookKey = 'x'; openSet = -1.0; eyeSet = -1.0; ewSet = -1.0
        floaty = $false; bang = $false; tear = $false; waves = $false
    }
    $fx.inner.Offset = 0.38
    $fx.outer.Offset = 1.0
    $glowBrush = New-Object Windows.Media.RadialGradientBrush
    $glowBrush.GradientStops.Add($fx.inner)
    $glowBrush.GradientStops.Add($fx.outer)
    $ui.Glow.Fill = $glowBrush
    $ui.Dot.Fill = $fx.mood
    $ui.Spark.Fill = $fx.mood
    $ui.Platform.Stroke = $fx.mood
    $ui.PlatformIn.Fill = $fx.platIn

    function Set-VoiceMic([int]$id) {
        $script:micId = $id
        $ui.MicName.Text = if ($id -ge 0 -and $id -lt $script:mics.Count) { $script:mics[$id] } else { 'Default microphone' }
    }
    Set-VoiceMic $script:micId

    # A bubble - yours on the right, Buddy's on the left beside its face - or
    # a centred note. Kept to the last 40.
    function Add-VoiceLine([string]$kind, [string]$text) {
        $tb = New-Object Windows.Controls.TextBlock
        $tb.Text = $text
        $tb.TextWrapping = 'Wrap'
        $tb.FontSize = 14
        $tb.VerticalAlignment = 'Center'
        if ($kind -eq 'note') {
            $tb.FontSize = 12
            $tb.Foreground = New-VoiceBrush '#A09CD0'
            $tb.TextAlignment = 'Center'
            $tb.Margin = New-Object Windows.Thickness(10, 6, 10, 6)
            $el = $tb
        } else {
            $bubble = New-Object Windows.Controls.Border
            $bubble.CornerRadius = New-Object Windows.CornerRadius(18)
            $bubble.BorderThickness = New-Object Windows.Thickness(1)
            $bubble.Padding = New-Object Windows.Thickness(14, 9, 14, 9)
            $row = New-Object Windows.Controls.StackPanel
            $row.Orientation = 'Horizontal'
            $row.Margin = New-Object Windows.Thickness(0, 4, 0, 4)
            if ($kind -eq 'you') {
                $row.HorizontalAlignment = 'Right'
                $bubble.Background = New-VoiceBrush '#0F3B55'
                $bubble.BorderBrush = New-VoiceBrush '#3300E0FF'
                $bubble.MaxWidth = 250
                $bubble.Child = $tb
                [void]$row.Children.Add($bubble)
            } else {
                $row.HorizontalAlignment = 'Left'
                $face = New-Object Windows.Controls.Image
                $face.Source = $script:avatar
                $face.Width = 28
                $face.Height = 28
                $face.VerticalAlignment = 'Top'
                $face.Margin = New-Object Windows.Thickness(0, 2, 8, 0)
                $bubble.Background = New-VoiceBrush '#1E1C3E'
                $bubble.BorderBrush = New-VoiceBrush '#1FFFFFFF'
                $bubble.MaxWidth = 232
                $bubble.Child = $tb
                [void]$row.Children.Add($face)
                [void]$row.Children.Add($bubble)
            }
            $el = $row
        }
        [void]$ui.Talk.Children.Add($el)
        while ($ui.Talk.Children.Count -gt 40) { $ui.Talk.Children.RemoveAt(0) }
        $ui.Scroll.ScrollToEnd()
        $script:lastLine = $el
    }

    # The last dozen lines either way, for a new connection - another model,
    # a reconnect - to carry on from instead of starting over.
    $script:recent = New-Object System.Collections.Generic.List[string]
    function Add-VoiceRecent([string]$who, [string]$text) {
        $script:recent.Add("${who}: $text")
        while ($script:recent.Count -gt 12) { $script:recent.RemoveAt(0) }
    }

    # Timers and reminders Buddy set (set_timer). They live in this window,
    # which opens at sign-in; closing it drops them.
    $script:timers = New-Object System.Collections.Generic.List[object]
    $script:greet = $false
    $script:camJob = $null
    $script:faceUdp = $null

    # A face on the dongle, through the companion's relay (which passes on
    # emotions and mouths only).
    function Send-VoiceFace([string]$emo) {
        try {
            if (-not $script:faceUdp) {
                $script:faceUdp = New-Object Net.Sockets.UdpClient(
                    (New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, 0)))
            }
            $bytes = [Text.Encoding]::ASCII.GetBytes("E $emo")
            [void]$script:faceUdp.Send($bytes, $bytes.Length, (New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, $VOICE_PORT)))
        } catch {}
    }

    function Add-VoiceTimer([int]$seconds, [string]$label) {
        if ($script:timers.Count -ge 10) { Add-VoiceLine 'note' 'Ten timers already - cancel some first.'; return }
        $t = @{ due = (Get-Date).AddSeconds($seconds); label = $label }
        $script:timers.Add($t)
        Add-VoiceLine 'note' "Timer: $label, at $($t.due.ToString('HH:mm'))."
    }

    # The conversation's requests (timers, a camera picture) and pictures to
    # show, then timers due. Also called as a conversation stops - ended with
    # the button, its last requests used to be lost with it.
    function Update-VoiceTimers($b) {
        $r = $null
        while ($b -and $b.Requests.TryDequeue([ref]$r)) {
            $f = $r.Split("`t")
            switch ($f[0]) {
                'timer' { Add-VoiceTimer ([int]$f[1]) $f[2] }
                'cancel' { $script:timers.Clear(); Add-VoiceLine 'note' 'All timers cancelled.' }
                'camera' { Start-VoiceCamera $b }
                'paste' { if (Insert-VoiceText $r.Substring(6)) { Add-VoiceLine 'note' 'Typed it into your app.' } }
                'note' { Add-VoiceLine 'note' $r.Substring(5) }
            }
        }
        $img = $null
        while ($b -and $b.Shown.TryDequeue([ref]$img)) { Add-VoiceImage $img }
        $now = Get-Date
        # ToArray, not @(): @() of a List of hashtables throws "Argument types
        # do not match" in Windows PowerShell - measured, and it stopped every
        # timer from firing and the rest of the window's tick with it.
        foreach ($t in $script:timers.ToArray()) {
            if ($now -lt $t.due) { continue }
            [void]$script:timers.Remove($t)
            Invoke-VoiceTimerDone $t.label
        }
        Receive-VoiceCamera
        Update-CameraView
        Clear-CameraRuns
    }

    # A typed "remind me in 10 minutes to stretch", "set a timer for 5 min",
    # "remind me at 5:30 pm to call mom" with no conversation going: set
    # here, at once - typed chat has no tools. $true when it was one.
    $TIMER_IN = '^\W*(?:please\s+)?(?:set\s+(?:a\s+|an\s+)?(?:timer|reminder|alarm)\s+(?:for\s+|in\s+)?|remind\s+me\s+in\s+|timer\s+(?:for\s+)?)' +
                '(\d+(?:\.\d+)?)\s*(s|secs?|seconds?|m|mins?|minutes?|h|hrs?|hours?)\b\W*(?:to|for|about)?\s*(.*)$'
    $TIMER_AT = '^\W*(?:please\s+)?(?:remind\s+me|set\s+(?:a\s+|an\s+)?(?:reminder|alarm))\s+(?:at|for)\s+(\d{1,2})(?::(\d\d))?\s*(am|pm)?\b\W*(?:to|for|about)?\s*(.*)$'
    function Invoke-TimerAsk([string]$text) {
        if ($text -match $TIMER_IN) {
            $n = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            $unit = $Matches[2].Substring(0, 1).ToLower()
            $seconds = [int]($n * $(switch ($unit) { 's' { 1 } 'm' { 60 } default { 3600 } }))
        } elseif ($text -match $TIMER_AT) {
            $hour = [int]$Matches[1]
            $min = if ($Matches[2]) { [int]$Matches[2] } else { 0 }
            $now = Get-Date
            # With am/pm, that one; a bare "5" is the next 5 o'clock, am or pm.
            $hours = if ($Matches[3] -eq 'am') { @($hour % 12) } elseif ($Matches[3] -eq 'pm') { @($hour % 12 + 12) }
                     elseif ($hour -ge 12) { @($hour) } else { @($hour, ($hour + 12)) }
            $due = @(foreach ($h in $hours) { foreach ($d in 0, 1) { $now.Date.AddDays($d).AddHours($h).AddMinutes($min) } }) |
                Where-Object { $_ -gt $now } | Sort-Object | Select-Object -First 1
            if (-not $due) { return $false }
            $seconds = [int]($due - $now).TotalSeconds
            $Matches[3] = $Matches[4]
        } else {
            return $false
        }
        if ($seconds -lt 1 -or $seconds -gt 86400) { return $false }
        $label = "$($Matches[3])".Trim(' ', '.', '!', '?')
        Add-VoiceTimer $seconds $(if ($label) { $label } else { 'timer' })
        Send-VoiceFace 'THUMBSUP'
        Set-VoiceFaceLine 'E THUMBSUP'
        $true
    }

    # A picture in the chat, as Buddy got it.
    function ConvertTo-VoiceBitmap([byte[]]$bytes, [int]$width) {
        $bi = New-Object Windows.Media.Imaging.BitmapImage
        $bi.BeginInit()
        $bi.StreamSource = New-Object IO.MemoryStream(, $bytes)
        $bi.CacheOption = 'OnLoad'
        $bi.DecodePixelWidth = $width
        $bi.EndInit()
        $bi.Freeze()
        $bi
    }
    function Add-VoiceImage([byte[]]$bytes) {
        try {
            # The frame's background, so its rounded corners clip the picture
            # (an Image child poked out square - rendered); sized to it.
            $bi = ConvertTo-VoiceBitmap $bytes 440
            $frame = New-Object Windows.Controls.Border
            $frame.Width = 220
            $frame.Height = [math]::Round(220 * $bi.PixelHeight / [math]::Max(1, $bi.PixelWidth))
            $frame.CornerRadius = New-Object Windows.CornerRadius(14)
            $frame.BorderThickness = New-Object Windows.Thickness(1)
            $frame.BorderBrush = New-VoiceBrush '#3300E0FF'
            $frame.Margin = New-Object Windows.Thickness(36, 2, 0, 6)
            $frame.HorizontalAlignment = 'Left'
            $brush = New-Object Windows.Media.ImageBrush $bi
            $brush.Stretch = 'UniformToFill'
            $frame.Background = $brush
            [Windows.Automation.AutomationProperties]::SetName($frame, 'What Buddy saw')
            [void]$ui.Talk.Children.Add($frame)
            while ($ui.Talk.Children.Count -gt 40) { $ui.Talk.Children.RemoveAt(0) }
            $ui.Scroll.ScrollToEnd()
        } catch {}
    }

    # ---- the camera: which one, a live view, and a photo for a look -------
    #
    # A virtual camera (Camo, OBS) or an IR face-unlock camera shows black
    # unless something feeds it, and a laptop's own camera is black with the
    # lid shut - measured here: Windows picked one giving 0.02 brightness.
    # So the cameras are listed (virtual ones last), chosen in the settings
    # like the mic and remembered by name, and a black picture moves on to
    # the next camera by itself.
    $CAMERA_SKIP = '(?i)\bIR\b|virtual|camo|obs|snap camera|xsplit|droidcam|ndi|iriun'
    $CAMERA_DARK = 0.06       # brightness below which a picture is "black"
    # Live view: a frame to Gemini this often - measured ~200 tokens a frame
    # at HIGH media resolution, so ~4k tokens a minute of the 65k.
    $CAMERA_EVERY_S = 3
    $CAMERA_IDLE_S = 120      # live view off after this long with no conversation
    $script:cams = $null
    $script:camName = Get-VoicePref 'camera'

    function Get-VoiceCameras {
        if ($null -ne $script:cams) { return $script:cams }
        $script:cams = @()
        try {
            [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
            [Windows.Devices.Enumeration.DeviceInformationCollection, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
            $all = Wait-WinRt ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
                [Windows.Devices.Enumeration.DeviceClass]::VideoCapture)) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 5000
            $found = @($all | Where-Object { $_.IsEnabled } | ForEach-Object { @{ name = $_.Name; id = $_.Id } })
            $script:cams = @($found | Where-Object { $_.name -notmatch $CAMERA_SKIP }) + @($found | Where-Object { $_.name -match $CAMERA_SKIP })
        } catch {}
        # Not ", $script:cams": callers wrap it in @(), which then held one
        # item - the whole list - and the settings showed no camera at all.
        $script:cams
    }

    # The chosen camera first, then the rest in order.
    function Get-VoiceCameraOrder {
        $all = Get-VoiceCameras
        @($all | Where-Object { $_.name -eq $script:camName }) + @($all | Where-Object { $_.name -ne $script:camName })
    }

    # One camera on a thread of its own: Windows' MediaCapture, a photo -
    # or, for the live view, one after another until told to stop - into a
    # shared table the window reads.
    $CAMERA_RUN = {
        param($id, $state)
        try {
            Add-Type -AssemblyName System.Runtime.WindowsRuntime
            $act = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
                $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
                $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
            [Windows.Media.Capture.MediaCapture, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
            [Windows.Media.MediaProperties.ImageEncodingProperties, Windows.Media.MediaProperties, ContentType = WindowsRuntime] | Out-Null
            [Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime] | Out-Null
            $mc = New-Object Windows.Media.Capture.MediaCapture
            $set = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
            $set.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video
            if ($id) { $set.VideoDeviceId = $id }
            if (-not $act.Invoke($null, @($mc.InitializeAsync($set))).Wait(8000)) { throw 'the camera did not start' }
            Start-Sleep -Milliseconds 500
            $tries = 0
            do {
                $st = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
                $jpg = [Windows.Media.MediaProperties.ImageEncodingProperties]::CreateJpeg()
                if (-not $act.Invoke($null, @($mc.CapturePhotoToStreamAsync($jpg, $st))).Wait(8000)) { throw 'no picture' }
                $s = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($st.GetInputStreamAt(0))
                $ms = New-Object IO.MemoryStream
                $s.CopyTo($ms)
                $frame = $ms.ToArray()
                $state.frame = $frame
                $state.seq++
                $tries++
                if ($state.once) {
                    # A webcam's first pictures are black while it finds its
                    # exposure - measured: 0.02, then 0.31 half a second on -
                    # so a photo is the first lit one, five tries at most.
                    $lit = $true
                    try { $lit = [NexusVoice.Snap]::Brightness($frame) -ge $state.dark } catch {}
                    if ($lit -or $tries -ge 5) { break }
                }
                Start-Sleep -Milliseconds 250
            } while ($state.run)
            $mc.Dispose()
        } catch {
            $state.err = $_.Exception.GetBaseException().Message
        }
        $state.done = $true
    }

    function Start-CameraRun($cam, [bool]$once) {
        $state = [hashtable]::Synchronized(@{ run = $true; once = $once; frame = $null; seq = 0; err = $null; done = $false; dark = $CAMERA_DARK })
        $ps = [powershell]::Create()
        [void]$ps.AddScript($CAMERA_RUN).AddArgument($cam.id).AddArgument($state)
        @{ ps = $ps; handle = $ps.BeginInvoke(); state = $state; cam = $cam }
    }

    # Told to stop; disposed by Clear-CameraRuns once it has let go of the
    # camera - never waited for here, on the window's thread.
    $script:oldRuns = New-Object System.Collections.Generic.List[object]
    function Stop-CameraRun($run) {
        if (-not $run) { return }
        $run.state.run = $false
        $script:oldRuns.Add($run)
    }
    function Clear-CameraRuns {
        foreach ($r in $script:oldRuns.ToArray()) {
            if (-not $r.handle.IsCompleted) { continue }
            try { $r.ps.Dispose() } catch {}
            [void]$script:oldRuns.Remove($r)
        }
    }

    # A photo for the look tool: the live view's latest frame if it is on,
    # else one photo, from the next camera if a black one came back.
    function Start-VoiceCamera($b) {
        $v = $script:view
        if ($v -and $v.run.state.frame) {
            $b.ProvideFrame([NexusVoice.Snap]::Jpeg([byte[]]$v.run.state.frame))
            return
        }
        if ($script:camJob) { return }
        $order = @(Get-VoiceCameraOrder)
        if (-not $order.Count) {
            Add-VoiceLine 'note' 'No camera found.'
            $b.ProvideFrame($null)
            return
        }
        $script:camJob = @{ b = $b; order = $order; at = 0; run = (Start-CameraRun $order[0] $true) }
    }

    function Receive-VoiceCamera {
        $j = $script:camJob
        if (-not $j -or -not $j.run.state.done) { return }
        $st = $j.run.state
        $name = $j.run.cam.name
        Stop-CameraRun $j.run
        $jpeg = $null
        if ($st.frame) {
            $dark = $true
            try { $dark = [NexusVoice.Snap]::Brightness([byte[]]$st.frame) -lt $CAMERA_DARK } catch {}
            if (-not $dark) { $jpeg = [NexusVoice.Snap]::Jpeg([byte[]]$st.frame) }
        }
        if (-not $jpeg -and $j.at + 1 -lt $j.order.Count) {
            $why = if ($st.frame) { 'a black picture (lid shut, shutter closed, or nothing feeding it?)' } else { "no picture ($($st.err))" }
            $j.at++
            Add-VoiceLine 'note' "$name gave $why - trying $($j.order[$j.at].name)."
            $j.run = Start-CameraRun $j.order[$j.at] $true
            return
        }
        $script:camJob = $null
        if ($jpeg) {
            # This camera works: first from now on, until another is chosen.
            if (-not $script:camName -or $j.at -gt 0) { $script:camName = $name; Set-VoicePref 'camera' $name }
        } else {
            Add-VoiceLine 'note' 'No camera gave a picture - check the shutter, or pick one in the settings.'
        }
        $j.b.ProvideFrame($jpeg)
    }

    # The live view, like Mira's: the camera in the corner of Buddy's stage,
    # a few frames a second here, and one every $CAMERA_EVERY_S to Gemini
    # while a conversation is on, so Buddy sees what you show it. On and off
    # from the settings, or by saying "turn on the camera" / "camera off".
    $script:view = $null

    # The live view: a card between Buddy and the chat, the chat's width -
    # not over the stage, where it covered Buddy's ear (rendered). The frame
    # is the card's background, so its rounded corners clip the picture.
    $CAMERA_CARD_H = 120
    # While it is on, Buddy's stage is drawn at this scale - all of Buddy, a
    # little smaller - so the chat under the card keeps room (rendered).
    $CAMERA_STAGE_SCALE = 0.7
    function New-CameraBox {
        $box = New-Object Windows.Controls.Border
        $box.Height = $CAMERA_CARD_H
        $box.CornerRadius = New-Object Windows.CornerRadius(16)
        $box.BorderThickness = New-Object Windows.Thickness(1.5)
        $box.BorderBrush = New-VoiceBrush '#FF4D6A'
        $box.Background = New-VoiceBrush '#0B0B1E'
        $box.VerticalAlignment = 'Top'
        $box.Margin = New-Object Windows.Thickness(0, 4, 0, 0)
        $tag = New-Object Windows.Controls.Border
        $tag.CornerRadius = New-Object Windows.CornerRadius(8)
        $tag.Background = New-VoiceBrush '#CC0B0B1E'
        $tag.Padding = New-Object Windows.Thickness(8, 3, 8, 3)
        $tag.Margin = New-Object Windows.Thickness(8)
        $tag.HorizontalAlignment = 'Left'
        $tag.VerticalAlignment = 'Bottom'
        $label = New-Object Windows.Controls.TextBlock
        $label.Text = [string][char]0x25CF + ' LIVE - Buddy sees this'
        $label.FontSize = 11
        $label.Foreground = New-VoiceBrush '#FF8FA3'
        $tag.Child = $label
        $box.Child = $tag
        [Windows.Automation.AutomationProperties]::SetName($box, 'Camera view')
        $box
    }

    # A picture as a brush that fills a rounded frame.
    function New-VoiceImageBrush([byte[]]$bytes, [int]$width) {
        $brush = New-Object Windows.Media.ImageBrush (ConvertTo-VoiceBitmap $bytes $width)
        $brush.Stretch = 'UniformToFill'
        $brush.Freeze()
        $brush
    }

    # The card goes above the chat, which steps down under it while it is on.
    $script:stageRow = $null
    function Show-CameraBox($box, [bool]$show) {
        $grid = $ui.Scroll.Parent
        if (-not $script:stageRow) {
            $script:stageRow = $ui.Stage.Parent.RowDefinitions[[Windows.Controls.Grid]::GetRow($ui.Stage)]
            $script:stageHeight = $script:stageRow.Height
        }
        $m = $ui.Scroll.Margin
        if ($show) {
            [Windows.Controls.Grid]::SetRow($box, [Windows.Controls.Grid]::GetRow($ui.Scroll))
            [void]$grid.Children.Add($box)
            $ui.Scroll.Margin = New-Object Windows.Thickness($m.Left, ($CAMERA_CARD_H + 10), $m.Right, $m.Bottom)
            $ui.Stage.LayoutTransform = New-Object Windows.Media.ScaleTransform($CAMERA_STAGE_SCALE, $CAMERA_STAGE_SCALE)
            $script:stageRow.Height = New-Object Windows.GridLength([math]::Round($script:stageHeight.Value * $CAMERA_STAGE_SCALE))
        } else {
            [void]$grid.Children.Remove($box)
            $ui.Scroll.Margin = New-Object Windows.Thickness($m.Left, 4, $m.Right, $m.Bottom)
            $ui.Stage.LayoutTransform = $null
            $script:stageRow.Height = $script:stageHeight
        }
    }

    # -Said: asked for out loud or typed in a conversation, which Gemini heard
    # itself - telling it as well made it answer twice.
    function Set-CameraView([bool]$on, $cam, [switch]$Said, [string]$why = 'Camera off.') {
        if (-not $on) {
            if (-not $script:view) { return }
            Stop-CameraRun $script:view.run
            Show-CameraBox $script:view.box $false
            $script:view = $null
            Add-VoiceLine 'note' $why
            if ($script:bv -and -not $Said) { [void]$script:bv.Tell('(The owner turned the camera off: you no longer see it.)') }
            return
        }
        # Already on: "camera on" again is not a restart (it flickered).
        if ($script:view -and -not $cam) { return }
        $order = @(Get-VoiceCameraOrder)
        if (-not $order.Count) { Add-VoiceLine 'note' 'No camera found.'; return }
        if (-not $cam) { $cam = $order[0] }
        if ($script:view) { Stop-CameraRun $script:view.run; Show-CameraBox $script:view.box $false }
        $box = New-CameraBox
        $box.ToolTip = "Camera: $($cam.name) - Buddy sees this"
        Show-CameraBox $box $true
        $script:view = @{ run = (Start-CameraRun $cam $false); box = $box; seen = 0; darkRun = 0
                          sentAt = [datetime]::MinValue; checked = $false; tried = @($cam.name) }
        Add-VoiceLine 'note' "Camera on ($($cam.name)). Buddy sees it every $CAMERA_EVERY_S s while you talk. It goes off when the PC locks, or after $CAMERA_IDLE_S s with no conversation."
        if ($script:bv -and -not $Said) { [void]$script:bv.Tell("(The owner turned the camera on: you now see it live, every $CAMERA_EVERY_S seconds.)") }
    }

    function Update-CameraView {
        $v = $script:view
        if (-not $v) { return }
        # With nobody talking for a while, the camera does not stay on (its
        # light would, with the window minimised or forgotten).
        if ($script:bv) { $v.idleSince = $null }
        elseif (-not $v.idleSince) { $v.idleSince = Get-Date }
        elseif (((Get-Date) - $v.idleSince).TotalSeconds -ge $CAMERA_IDLE_S) {
            Set-CameraView $false -why "Camera off - no conversation for $CAMERA_IDLE_S s."
            return
        }
        $st = $v.run.state
        if ($st.done -and $st.seq -eq $v.seen) {
            Add-VoiceLine 'note' "The camera stopped: $(if ($st.err) { $st.err } else { 'no picture' })"
            Set-CameraView $false
            return
        }
        if (-not $st.frame -or $st.seq -eq $v.seen) { return }
        $v.seen = $st.seq
        $bytes = [byte[]]$st.frame
        # Until a lit frame: a webcam's first ones are black while it finds
        # its exposure, so five black ones in a row mean this camera shows
        # nothing - the next one, once round.
        if (-not $v.checked) {
            $dark = $false
            try { $dark = [NexusVoice.Snap]::Brightness($bytes) -lt $CAMERA_DARK } catch {}
            if (-not $dark) {
                $v.checked = $true
                if ($script:camName -ne $v.run.cam.name -and ($v.tried.Count -gt 1 -or -not $script:camName)) {
                    $script:camName = $v.run.cam.name
                    Set-VoicePref 'camera' $script:camName
                }
            } elseif ((++$v.darkRun) -ge 5) {
                $v.checked = $true
                $next = @(Get-VoiceCameraOrder | Where-Object { $v.tried -notcontains $_.name }) | Select-Object -First 1
                if ($next) {
                    Add-VoiceLine 'note' "$($v.run.cam.name) is black (lid shut, shutter closed, or nothing feeding it?) - trying $($next.name)."
                    $tried = $v.tried + $next.name
                    Set-CameraView $true $next
                    $script:view.tried = $tried
                    return
                }
                Add-VoiceLine 'note' 'Every camera is black - check the shutter, or pick one in the settings.'
            }
        }
        try { $v.box.Background = New-VoiceImageBrush $bytes 640 } catch {}
        $b = $script:bv
        if ($b -and $b.Ready -and ((Get-Date) - $v.sentAt).TotalSeconds -ge $CAMERA_EVERY_S) {
            if ($b.SendFrame([NexusVoice.Snap]::Jpeg($bytes))) { $v.sentAt = Get-Date }
        }
    }

    # "Turn on the camera", "camera off" - spoken or typed. $true when it was.
    $CAMERA_ON = '\b(?:turn|switch|put)\s+on\s+(?:the\s+|my\s+|your\s+)?(?:camera|cam|webcam)\b|\b(?:camera|cam|webcam)\s+on\b|\bopen\s+(?:the\s+|my\s+)?(?:camera|cam|webcam)\b|\bstart\s+(?:the\s+)?(?:camera|cam|webcam)\b'
    $CAMERA_OFF = '\b(?:turn|switch)\s+off\s+(?:the\s+|my\s+|your\s+)?(?:camera|cam|webcam)\b|\b(?:camera|cam|webcam)\s+off\b|\bclose\s+(?:the\s+|my\s+)?(?:camera|cam|webcam)\b|\bstop\s+(?:the\s+)?(?:camera|cam|webcam)\b'
    function Invoke-CameraAsk([string]$text) {
        if ($text -match $CAMERA_OFF) { Set-CameraView $false -Said; return $true }
        if ($text -match $CAMERA_ON) { Set-CameraView $true -Said; return $true }
        $false
    }

    # Time's up: said in the window, shown on the dongle, and spoken - by
    # Buddy in a conversation, else by the Windows voice.
    function Invoke-VoiceTimerDone([string]$label) {
        Add-VoiceLine 'note' "Time's up: $label."
        if ($w.WindowState -eq 'Minimized') { $w.WindowState = 'Normal' }
        Send-VoiceFace 'SURPRISED'
        Set-VoiceFaceLine 'E SURPRISED'
        try { [Media.SystemSounds]::Exclamation.Play() } catch {}
        $b = $script:bv
        # In a conversation Buddy says it in its own voice; else (or on Flash
        # Lite, which takes no notices) the Windows voice does.
        if ($b -and $b.Ready -and -not $b.Done -and $b.Tell("(A timer just ended: $label. Tell the owner, in a few words.)")) {
            return
        }
        try {
            if (-not $script:synth) {
                Add-Type -AssemblyName System.Speech
                $script:synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
                try { $script:synth.SelectVoiceByHints('Female') } catch {}
            }
            [void]$script:synth.SpeakAsync("Time's up: $label")
        } catch {}
    }

    # ---- held keys: dictate anywhere, and "what's this?" ---------------------
    #
    # Hold / and talk, let go: what you said, cleaned up by Flash Lite (fillers
    # gone, punctuation in, "new line" a new line, "make it formal" obeyed), is
    # typed where your caret is, in any app. Hold Z over a selection - or over
    # nothing, for the screen - and ask, or say nothing; let go: Buddy answers
    # out loud in a conversation, so "how do I fix it?" and "paste that"
    # follow. A conversation going on hears neither question: its mic is
    # muted while a key is held.
    $script:holdKeys = @{}
    $script:hold = $null       # @{ mode; rec } while a key is held
    $script:holdJob = $null    # @{ mode; task; sel } while Flash Lite listens
    $script:clipBack = $null   # @{ at; text }: the clipboard to put back
    $script:pendingAsk = $null # @{ text; jpeg; ask } for a conversation starting

    function Update-VoiceHolds {
        foreach ($mode in @($script:holdKeys.Keys)) {
            $k = $script:holdKeys[$mode]
            if ($k.Take() -gt 0) { Start-VoiceHold $mode }
            if ($k.TakeReleases() -gt 0 -and $script:hold -and $script:hold.mode -eq $mode) { Stop-VoiceHold }
        }
        Receive-VoiceHold
        $c = $script:clipBack
        if ($c -and (Get-Date) -ge $c.at) {
            $script:clipBack = $null
            try { [Windows.Clipboard]::SetText($c.text) } catch {}
        }
        $a = $script:pendingAsk
        $b = $script:bv
        if ($a -and $b -and $b.Ready) {
            $script:pendingAsk = $null
            # Only your words, never the selection, count as asking: text
            # you selected cannot ask Buddy to paste.
            if ($a.ask) { $b.Asked($a.ask) }
            if ($a.jpeg) { $b.ShowAndAsk($a.jpeg, $a.text) }
            elseif (-not $b.Tell($a.text)) { [void]$b.SendText($a.text) }
        } elseif ($a -and -not $b) {
            $script:pendingAsk = $null   # the conversation did not start
        }
    }

    function Start-VoiceHold([string]$mode) {
        if ($script:hold -or $script:holdJob) { return }
        $rec = New-Object NexusVoice.Recorder
        if (-not $rec.Start($script:micId)) { Add-VoiceLine 'note' "Cannot listen: $($rec.Error)"; return }
        $script:hold = @{ mode = $mode; rec = $rec }
        if ($script:bv) { $script:bv.Muted = $true }
        Send-VoiceFace 'LISTENING'
        Set-VoiceFaceLine 'E LISTENING'
    }

    function Stop-VoiceHold {
        $h = $script:hold
        $script:hold = $null
        $pcm = $h.rec.Stop()
        if ($script:bv) { $script:bv.Muted = $false }
        $long = $pcm.Length -ge 16000   # half a second held, at 16 kHz 16-bit
        $spoke = $long -and $h.rec.Voiced()
        if (-not $gem.key) { Add-VoiceLine 'note' "This needs a Gemini key in $KEY_FILE"; return }
        if ($h.mode -eq 'dictate') {
            if (-not $spoke) {
                Add-VoiceLine 'note' $(if (-not $long) { 'Hold / a little longer, and talk while it is down.' }
                    else { "Didn't hear you (mic level $($h.rec.Peak)) - check the mic picked in the menu." })
                Send-VoiceFace 'NEUTRAL'; Set-VoiceFaceLine 'E NEUTRAL'; return
            }
            $script:holdJob = @{ mode = 'dictate'; task = [NexusVoice.Flash]::Hear([string[]]$HEAR_MODELS, $gem.key, $pcm, $true) }
        } else {
            $sel = Get-VoiceSelection
            $task = if ($spoke) { [NexusVoice.Flash]::Hear([string[]]$HEAR_MODELS, $gem.key, $pcm, $false) }
            $script:holdJob = @{ mode = 'explain'; task = $task; sel = $sel }
        }
        Send-VoiceFace 'THINKING'
        Set-VoiceFaceLine 'E THINKING'
    }

    function Receive-VoiceHold {
        $j = $script:holdJob
        if (-not $j -or ($j.task -and -not $j.task.IsCompleted)) { return }
        $script:holdJob = $null
        $text = ''
        if ($j.task) { try { $text = "$($j.task.Result)".Trim() } catch {} }
        if ($j.mode -eq 'dictate') {
            if (-not $text) {
                $why = [NexusVoice.Flash]::LastWhy
                Add-VoiceLine 'note' $(if ($why) { "Didn't catch that - Gemini is $why." } else { "Didn't catch any words." })
                Send-VoiceFace 'SAD'
                return
            }
            if (Insert-VoiceText $text) {
                Send-VoiceFace 'THUMBSUP'
                Set-VoiceFaceLine 'E THUMBSUP'
                Add-VoiceLine 'note' "Typed: $(if ($text.Length -gt 90) { $text.Substring(0, 90) + '...' } else { $text })"
            }
        } else {
            Invoke-VoiceExplain $j.sel $text
        }
    }

    # Text where the caret is, in whichever app has it (a selection there is
    # replaced): pasted, then the clipboard you had is put back.
    # ponytail: only text is put back; a picture on the clipboard is lost.
    function Insert-VoiceText([string]$text) {
        [void][NexusVoice.Typing]::ModifiersUp(1500)
        $old = $null
        try { if ([Windows.Clipboard]::ContainsText()) { $old = [Windows.Clipboard]::GetText() } } catch {}
        $set = $false
        for ($i = 0; $i -lt 5 -and -not $set; $i++) {
            try { [Windows.Clipboard]::SetText($text); $set = $true } catch { Start-Sleep -Milliseconds 40 }
        }
        if (-not $set) { Add-VoiceLine 'note' 'Could not type it: another app has the clipboard.'; return $false }
        [NexusVoice.Typing]::Paste()
        if ($null -ne $old) { $script:clipBack = @{ at = (Get-Date).AddMilliseconds(700); text = $old } }
        $true
    }

    # The text selected in the app you are in - copied, then your clipboard
    # put back - or $null when nothing is selected (the copy changes nothing).
    function Get-VoiceSelection {
        [void][NexusVoice.Typing]::ModifiersUp(1500)
        $seq = [NexusVoice.Typing]::GetClipboardSequenceNumber()
        $old = $null
        try { if ([Windows.Clipboard]::ContainsText()) { $old = [Windows.Clipboard]::GetText() } } catch {}
        [NexusVoice.Typing]::Copy()
        for ($i = 0; $i -lt 20 -and [NexusVoice.Typing]::GetClipboardSequenceNumber() -eq $seq; $i++) { Start-Sleep -Milliseconds 20 }
        if ([NexusVoice.Typing]::GetClipboardSequenceNumber() -eq $seq) { return $null }
        $sel = $null
        try { $sel = [Windows.Clipboard]::GetText() } catch {}
        if ($null -ne $old) { try { [Windows.Clipboard]::SetText($old) } catch {} }
        if ("$sel".Trim()) { $sel } else { $null }
    }

    # The question, with the selection - or, with nothing selected, the
    # screen - into a conversation (started if need be), answered out loud.
    function Invoke-VoiceExplain($sel, [string]$asked) {
        $ask = if ($asked) { $asked } else { 'What is this? Explain it briefly.' }
        Add-VoiceLine 'you' $ask
        if ($sel) {
            $clip = if ($sel.Length -gt 6000) { $sel.Substring(0, 6000) + ' ...' } else { $sel }
            Add-VoiceLine 'note' "About the $($sel.Length) characters you selected."
            $text = "(The owner selected this text in another app:`n<<<`n$clip`n>>>`nThey ask: $ask`nAnswer out loud, briefly.)"
            $jpeg = $null
        } else {
            $text = "(Nothing was selected, so here is the owner's screen. They ask: $ask`nAnswer out loud, briefly.)"
            $jpeg = [NexusVoice.Snap]::Screen()
        }
        if (-not $script:bv) {
            Start-VoiceTalk
            $script:greet = $false   # an answer, not a hello
        }
        $script:pendingAsk = @{ text = $text; jpeg = $jpeg; ask = $asked }
    }

    # A face or a trick asked for in a conversation - "show me an angry face",
    # "sing a song", "bang" - plays on the dongle at once, the same as typed
    # to chat: the companion plays it (it holds the dongle's port, and the
    # window's face follows), while Gemini still answers out loud. The faces
    # read from Buddy's words stay out of the way until it is done.
    function Invoke-VoiceCommand([string]$text) {
        $cmd = Get-BuddyCommand $text
        if (-not $cmd -or -not $script:bv) { return }
        $script:bv.HoldFaces(@($cmd.steps).Count * $STEP_MS + 2500)
        # Its written answer is not wanted: Buddy is answering aloud.
        if ($script:cmdClient) { $script:cmdClient.Ask($text) }
    }

    # The conversation's own lines become bubbles; anything else is a note.
    function Read-VoiceLog($b) {
        $m = $null
        $me = "$($p.name): "
        while ($b.Log.TryDequeue([ref]$m)) {
            if ($m.StartsWith('you: ')) {
                Add-VoiceLine 'you' $m.Substring(5)
                Add-VoiceRecent 'Owner' $m.Substring(5)
                if (-not (Invoke-CameraAsk $m.Substring(5))) { Invoke-VoiceCommand $m.Substring(5) }
            } elseif ($m.StartsWith($me)) {
                Add-VoiceLine 'buddy' $m.Substring($me.Length)
                Add-VoiceRecent "$($p.name)" $m.Substring($me.Length)
            } elseif (-not $m.StartsWith('connected')) {
                Add-VoiceLine 'note' $m
            }
        }
    }

    function Set-VoiceButton([bool]$on) {
        $ui.TalkBtn.Background = if ($on) { New-VoiceGradient '#FF4D6A' '#D633FF' } else { New-VoiceGradient '#B06BFF' '#00E0FF' }
        $ui.TalkHalo.Fill = New-VoiceBrush $(if ($on) { '#33FF4D6A' } else { '#2600E0FF' })
        $ui.TalkHalo.Stroke = New-VoiceBrush $(if ($on) { '#66FF4D6A' } else { '#5500E0FF' })
        $ui.TalkIcon.Text = [string][char]$(if ($on) { 0xE71A } else { 0xE720 })
        $ui.TalkIcon.Foreground = New-VoiceBrush $(if ($on) { '#FFFFFF' } else { '#0B0B1E' })
        $ui.TalkBtn.ToolTip = if ($on) { "End the conversation ($TALK_KEYS)" } else { "Talk ($TALK_KEYS)" }
        [Windows.Automation.AutomationProperties]::SetName($ui.TalkBtn, $ui.TalkBtn.ToolTip)
        Update-VoiceHint
    }

    # The message box says where a message goes: into the spoken
    # conversation while there is one, else to the companion's chat.
    function Update-VoiceHint {
        $empty = -not $ui.Msg.Text
        $ui.Hint.Visibility = if ($empty) { 'Visible' } else { 'Collapsed' }
        $ui.SendBtn.Visibility = if ($empty) { 'Collapsed' } else { 'Visible' }
        $ui.Hint.Text = if ($script:bv) { "Type to $($p.name), answered out loud" } else { "Message $($p.name)" }
    }

    $script:bv = $null
    $script:tier = 0          # where in $script:tiers the conversation is
    $script:pending = $null   # typed while connecting: sent once it is ready
    $script:lastEmo = ''
    # A step of the chain that failed rests until this time, and the next
    # one takes over; a new conversation starts from the top again.
    $script:rest = @{}
    $script:resume = @{}      # 3.8's last resumption handle, and its model
    $script:restarts = @()    # automatic restarts in the last minute
    $script:dropAt = $null
    $script:resumed = $false  # the running conversation was resumed from a handle

    function Get-TierName([string]$m) {
        switch -regex ($m) {
            '^lite$' { 'Flash Lite'; break }
            '3\.8-live' { '3.8 Live'; break }
            '3\.1-flash-live' { '3.1 Flash Live'; break }
            'native-audio' { '2.5 Native Audio'; break }
            default { $m }
        }
    }

    # The first step of the chain not resting. Flash Lite never rests: it
    # tries its own models on every sentence.
    function Get-VoiceTier {
        $now = Get-Date
        for ($i = 0; $i -lt $script:tiers.Count - 1; $i++) {
            $until = $script:rest[$script:tiers[$i]]
            if (-not $until -or $now -ge $until) { return $i }
        }
        $script:tiers.Count - 1
    }

    # -Carry: an automatic restart (the next model, a reconnect), which
    # picks the conversation up instead of starting a new one.
    function Start-VoiceTalk([switch]$Carry) {
        if ($script:bv) { return }
        if (-not $gem.key) { Add-VoiceLine 'note' "Voice needs a Gemini key in $KEY_FILE"; return }
        if ($script:buildError) { Add-VoiceLine 'note' "Voice cannot start: $script:buildError"; return }
        if (-not $Carry) {
            $script:recent.Clear()
            $script:resume = @{}
            # Buddy speaks first, once a day, in the first conversation.
            $script:greet = (Get-VoicePref 'greeted') -ne (Get-Date -Format 'yyyy-MM-dd')
        }
        $i = Get-VoiceTier
        $m = $script:tiers[$i]
        $recent = [string[]]$script:recent.ToArray()
        if ($m -eq 'lite') {
            $lp = (Get-BuddyPrompt $p -Spoken) + "`n" + $liteHint
            if ($recent) { $lp += "`n`nThe conversation so far, which carries on now:`n" + ($recent -join "`n") }
            $b = New-Object NexusVoice.BuddyVoice([string[]]$LITE_MODELS, $gem.key, $lp, "$($p.name)", $faces, $VOICE_PORT)
        } else {
            # 3.8 resumes the very conversation; any other model is told it.
            $handle = if ($script:resume.model -eq $m) { $script:resume.handle } else { $null }
            $b = New-Object NexusVoice.BuddyVoice($VOICE_URL, $gem.key, (Get-VoiceSetup $m $handle $recent),
                "$($p.name)", $faces, $VOICE_PORT)
        }
        $script:resumed = [bool]$handle
        $b.MicDevice = $script:micId
        $script:bv = $b
        $script:tier = $i
        $script:lastEmo = "$($b.Emotion)"
        Set-VoiceButton $true
        $b.BeginStart()
    }

    # -Closing: the window is going, so nothing starts anew.
    function Stop-VoiceTalk([switch]$Closing) {
        $b = $script:bv
        if (-not $b) { return }
        $script:bv = $null
        $script:pending = $null
        $b.Stop()
        Read-VoiceLog $b
        # A timer or a picture from its last moments is kept, not lost.
        Update-VoiceTimers $b
        # Away from the PC (locked, asleep): the camera goes off too.
        if ($b.Away -and $script:view) { Set-CameraView $false }
        $m = $script:tiers[$script:tier]
        if ($b.ResumeHandle) { $script:resume = @{ model = $m; handle = $b.ResumeHandle } }
        $kind = "$($b.EndKind)"
        # A resume that failed is the handle's fault, not the model's: try
        # again telling it the conversation instead, and rest nothing.
        if ($script:resumed -and $kind -in 'refused', 'drop' -and -not $b.ResumeHandle) {
            $script:resume = @{}
            $kind = 'goaway'
        }
        Set-VoiceButton $false
        # Ended on purpose - Talk again, a lock, sleep, nobody talking - or
        # the window closing: nothing to carry on.
        if ($Closing -or -not $kind) { return }
        # A chain that fails all the way down must not spin: six automatic
        # restarts a minute at most.
        $now = Get-Date
        $script:restarts = @($script:restarts | Where-Object { ($now - $_).TotalSeconds -lt 60 })
        if ($script:restarts.Count -ge 6) {
            Add-VoiceLine 'note' 'Gemini keeps failing right now - tap to talk again in a minute.'
            return
        }
        $script:restarts += $now
        switch ($kind) {
            # Out of tokens a minute: back to it in ten.
            'quota' { $script:rest[$m] = $now.AddMinutes(10) }
            'busy' { $script:rest[$m] = $now.AddMinutes(2) }
            'refused' { $script:rest[$m] = $now.AddHours(1) }
            # Once more on the same model; a second drop within a minute moves on.
            'drop' {
                if ($script:dropAt -and ($now - $script:dropAt).TotalSeconds -lt 60) { $script:rest[$m] = $now.AddMinutes(2) }
                $script:dropAt = $now
            }
            # 'goaway' is Google's routine reconnect: quietly, the same model.
        }
        if ($script:rest[$m] -and $script:rest[$m] -gt $now) {
            $what = @{ quota = 'is out of quota'; busy = 'is busy'; refused = 'is not available'; drop = 'keeps dropping' }[$kind]
            Add-VoiceLine 'note' "$(Get-TierName $m) $what - carrying on with $(Get-TierName $script:tiers[(Get-VoiceTier)])."
        }
        Start-VoiceTalk -Carry
    }

    # What the dongle is showing, as far as this window knows: a face held
    # for a while, a line being typed out, a glance.
    $script:face = @{
        emo = 'NEUTRAL'; until = [datetime]::MinValue; mouth = 0; mouthAt = [datetime]::MinValue
        lookX = 0.0; lookY = 0.0; lookUntil = [datetime]::MinValue
        cap = ''; capAt = [datetime]::MinValue; capUntil = [datetime]::MinValue
    }
    # One host-link line, the way the dongle takes it (docs/buddy.md): an
    # emotion with no line goes back after 30 s; a line stays up for 2 s after
    # it is typed out, plus 0.15 s a letter, and takes its face with it.
    function Set-VoiceFaceLine([string]$l) {
        $now = Get-Date
        $v = $l.Substring(2)
        switch ($l[0]) {
            'E' {
                if ($VOICE_POSES.ContainsKey($v)) {
                    $script:face.emo = $v
                    $script:face.until = $now.AddSeconds(30)
                    if ($script:face.capUntil -gt $now) { $script:face.until = $script:face.capUntil }
                }
            }
            'S' {
                $script:face.cap = $v
                $script:face.capAt = $now
                $script:face.capUntil = $now.AddMilliseconds(40 * $v.Length + 2000 + 150 * $v.Length)
                $script:face.until = $script:face.capUntil
            }
            'L' {
                $xy = $v.Split(',')
                $script:face.lookX = [double]$xy[0] / 20.0 * 5
                $script:face.lookY = [double]$xy[1] / 20.0 * 5
                $script:face.lookUntil = $now.AddSeconds(2)
            }
            'V' { $script:face.mouth = [int]$v; $script:face.mouthAt = $now }
            'B' { if ($v -eq '0') { $script:face.emo = 'NEUTRAL'; $script:face.until = [datetime]::MinValue } }
        }
    }
    function Read-VoiceMirror {
        $l = $null
        while ($script:mirror -and $script:mirror.Lines.TryDequeue([ref]$l)) { Set-VoiceFaceLine $l }
    }

    # Typing: into the spoken conversation while there is one, so Buddy
    # answers out loud in the same context; otherwise through the companion,
    # like chat.cmd: a written answer here, the face and a line on the
    # dongle (and on this face, through the mirror), tricks played at once.
    $script:asking = $false
    $script:typing = $null
    $script:lastAsk = ''
    function Send-VoiceMessage {
        $text = $ui.Msg.Text.Trim()
        if (-not $text -or $script:asking) { return }
        $ui.Msg.Text = ''
        Add-VoiceLine 'you' $text
        # "Camera on/off" is the window's own, typed or said; mid-conversation
        # it still goes to Buddy too, who answers it.
        if ((Invoke-CameraAsk $text) -and -not $script:bv) { return }
        # A reminder typed with no conversation going: typed chat has no tools.
        if (-not $script:bv -and (Invoke-TimerAsk $text)) { return }
        # And "remember ..." / "forget ...": kept here, the companion has no tools.
        $kept = if (-not $script:bv) { [NexusVoice.Memory]::FromWords($text) }
        if ($kept) { Add-VoiceLine 'note' $kept; Send-VoiceFace 'THUMBSUP'; Set-VoiceFaceLine 'E THUMBSUP'; return }
        # A question about this PC never goes to Gemini, even mid-conversation:
        # the companion answers it from the PC's own numbers (and the dongle
        # shows it); without a companion, this window does.
        $stats = Get-StatsReply $text
        if ($stats -and -not $script:chatClient) { Show-VoiceStats $stats; return }
        if ($script:bv -and -not $stats) {
            if ($script:bv.Ready) {
                if ($script:bv.SendText($text)) {
                    Add-VoiceRecent 'Owner' $text
                    Invoke-VoiceCommand $text
                } elseif ($script:bv.Done) {
                    Add-VoiceLine 'note' 'Not sent - the conversation has ended.'
                } else {
                    Add-VoiceLine 'note' 'Not sent - still answering the last one. Send it again in a moment.'
                }
            } else {
                $script:pending = $text
            }
            return
        }
        if (-not $script:chatClient) { Add-VoiceLine 'note' "Chat cannot start: $script:buildError"; return }
        # One at a time: the companion answers only the newest message.
        $script:asking = $true
        $script:lastAsk = $text
        Add-VoiceLine 'buddy' '...'
        $script:typing = $script:lastLine
        $script:chatClient.Ask($text)
    }

    # A PC answer shown here, face and line included, without the companion.
    function Show-VoiceStats($stats) {
        Add-VoiceLine 'buddy' $stats.reply
        foreach ($l in $stats.steps[0]) { Set-VoiceFaceLine $l }
    }

    function Receive-VoiceChat {
        $r = $null
        # A face or trick from the conversation: played, nothing to show.
        while ($script:cmdClient -and $script:cmdClient.Replies.TryDequeue([ref]$r)) {}
        while ($script:chatClient -and $script:chatClient.Replies.TryDequeue([ref]$r)) {
            $script:asking = $false
            if ($script:typing) { $ui.Talk.Children.Remove($script:typing); $script:typing = $null }
            $stats = if ($r -eq '!down') { Get-StatsReply $script:lastAsk }
            if ($stats) {
                Show-VoiceStats $stats
            } elseif ($r -eq '!down') {
                Add-VoiceLine 'note' 'The companion is not running - start it with install.cmd, or tap the mic to talk.'
            } elseif ($r -eq '!timeout') {
                Add-VoiceLine 'note' 'No answer in time - Gemini may be slow or out of quota. Try again in a minute.'
            } else {
                try {
                    $j = $r | ConvertFrom-Json
                    Add-VoiceLine 'buddy' "$($j.reply)"
                    # The mirror brings the dongle's lines too; this covers a
                    # dongle that is not connected.
                    $e = "$($j.emotion)".ToUpper()
                    if ($e) { Set-VoiceFaceLine "E $e" }
                } catch {}
            }
        }
    }

    # The settings menu above its pill: the microphone, and whether this
    # window opens at sign-in.
    $popup = New-Object Windows.Controls.Primitives.Popup
    $popup.PlacementTarget = $ui.SetBtn
    $popup.Placement = 'Top'
    $popup.VerticalOffset = -8
    $popup.StaysOpen = $false
    $popup.AllowsTransparency = $true
    function New-VoiceMenuItem([string]$text, [bool]$on, $tag) {
        $item = New-Object Windows.Controls.Button
        $item.Style = $w.FindResource('Flat')
        $item.HorizontalContentAlignment = 'Left'
        $item.Padding = New-Object Windows.Thickness(12, 8, 14, 8)
        $item.Tag = $tag
        $row = New-Object Windows.Controls.StackPanel
        $row.Orientation = 'Horizontal'
        $tick = New-Object Windows.Controls.TextBlock
        $tick.FontFamily = 'Segoe Fluent Icons, Segoe MDL2 Assets'
        $tick.FontSize = 12
        $tick.Width = 22
        $tick.VerticalAlignment = 'Center'
        $tick.Foreground = New-VoiceBrush '#00E0FF'
        $tick.Text = if ($on) { [string][char]0xE73E } else { '' }
        $tb = New-Object Windows.Controls.TextBlock
        $tb.Text = $text
        $tb.FontSize = 13
        if ($on) { $tb.Foreground = New-VoiceBrush '#00E0FF' }
        [void]$row.Children.Add($tick)
        [void]$row.Children.Add($tb)
        $item.Content = $row
        $item
    }
    function New-VoiceMenuHeader([string]$text) {
        $h = New-Object Windows.Controls.TextBlock
        $h.Text = $text
        $h.FontSize = 11
        $h.FontWeight = [Windows.FontWeights]::SemiBold
        $h.Foreground = New-VoiceBrush '#77749F'
        $h.Margin = New-Object Windows.Thickness(12, 8, 12, 4)
        $h
    }
    $ui.SetBtn.Add_Click({
        $panel = New-Object Windows.Controls.StackPanel
        [void]$panel.Children.Add((New-VoiceMenuHeader 'MICROPHONE'))
        for ($i = -1; $i -lt $script:mics.Count; $i++) {
            $name = if ($i -lt 0) { 'Default microphone' } else { $script:mics[$i] }
            $item = New-VoiceMenuItem $name ($i -eq $script:micId) $i
            $item.Add_Click({
                param($src)
                $popup.IsOpen = $false
                if ([int]$src.Tag -eq $script:micId) { return }
                Set-VoiceMic ([int]$src.Tag)
                $keep = if ($script:micId -ge 0) { $script:mics[$script:micId] } else { '' }
                Set-VoicePref 'mic' $keep
                # A new mic takes a new session.
                if ($script:bv) { Stop-VoiceTalk; Start-VoiceTalk }
            })
            [void]$panel.Children.Add($item)
        }
        # The cameras, like the mics: the one chosen is remembered by name and
        # used first, and the live view goes on and off here too. Listed
        # afresh each time, so a webcam plugged in since shows up.
        $script:cams = $null
        $cams = @(Get-VoiceCameras)
        if ($cams.Count) {
            [void]$panel.Children.Add((New-VoiceMenuHeader 'CAMERA'))
            $viewItem = New-VoiceMenuItem 'Live camera view' ([bool]$script:view) 'view'
            $viewItem.Add_Click({
                $popup.IsOpen = $false
                Set-CameraView (-not $script:view)
            })
            [void]$panel.Children.Add($viewItem)
            $current = (Get-VoiceCameraOrder)[0].name
            foreach ($c in $cams) {
                $item = New-VoiceMenuItem $c.name ($c.name -eq $current) $c.name
                $item.Add_Click({
                    param($src)
                    $popup.IsOpen = $false
                    $script:camName = [string]$src.Tag
                    Set-VoicePref 'camera' $script:camName
                    # The live view moves to it at once.
                    if ($script:view) {
                        Set-CameraView $true (@(Get-VoiceCameras) | Where-Object { $_.name -eq $script:camName } | Select-Object -First 1)
                    }
                })
                [void]$panel.Children.Add($item)
            }
        }
        $rule = New-Object Windows.Controls.Border
        $rule.Height = 1
        $rule.Background = New-VoiceBrush '#22FFFFFF'
        $rule.Margin = New-Object Windows.Thickness(10, 6, 10, 2)
        [void]$panel.Children.Add($rule)
        [void]$panel.Children.Add((New-VoiceMenuHeader 'WINDOW'))
        $auto = New-VoiceMenuItem 'Open when Windows starts' (Test-WindowAutostart) 'auto'
        $auto.Add_Click({
            $popup.IsOpen = $false
            try {
                Set-WindowAutostart (-not (Test-WindowAutostart))
                Add-VoiceLine 'note' $(if (Test-WindowAutostart) { "$($p.name) will open when Windows starts." } else { "$($p.name) will not open by itself any more." })
            } catch {
                Add-VoiceLine 'note' "Could not change that: $(Get-Reason $_)"
            }
        })
        [void]$panel.Children.Add($auto)
        $box = New-Object Windows.Controls.Border
        $box.CornerRadius = New-Object Windows.CornerRadius(16)
        $box.Background = New-VoiceBrush '#FA1A1940'
        $box.BorderBrush = New-VoiceBrush '#2EFFFFFF'
        $box.BorderThickness = New-Object Windows.Thickness(1)
        $box.Padding = New-Object Windows.Thickness(6)
        $box.MinWidth = 240
        $box.Child = $panel
        $popup.Child = $box
        $popup.IsOpen = $true
    })

    $ui.TalkBtn.Add_Click({ if ($script:bv) { Stop-VoiceTalk } else { Start-VoiceTalk } })
    $ui.SendBtn.Add_Click({ Send-VoiceMessage })
    $ui.Msg.Add_TextChanged({ Update-VoiceHint })
    $ui.Msg.Add_KeyDown({
        if ($_.Key -eq 'Return') { $_.Handled = $true; Send-VoiceMessage }
    })
    $ui.CloseBtn.Add_Click({ $w.Close() })
    $ui.MinBtn.Add_Click({ $w.WindowState = 'Minimized' })
    $ui.Header.Add_MouseLeftButtonDown({ try { $w.DragMove() } catch {} })
    # Esc clears a half-typed message first, and closes only an empty box.
    $w.Add_PreviewKeyDown({
        if ($_.Key -ne 'Escape') { return }
        $_.Handled = $true
        if ($ui.Msg.Text) { $ui.Msg.Text = '' } else { $w.Close() }
    })
    # A fault in a handler is reported in the window, not a silent crash.
    $w.Dispatcher.Add_UnhandledException({
        $e = $_
        $e.Handled = $true
        try { Add-VoiceLine 'note' "Something went wrong: $($e.Exception.GetBaseException().Message)" } catch {}
    })

    # What to draw, from the conversation, the chat and the mirror.
    function Get-VoiceView {
        $b = $script:bv
        $now = Get-Date
        $f = $script:face
        $state = 'idle'
        if ($b) {
            if (-not $b.Ready) { $state = 'connecting' }
            elseif ($b.Talking) { $state = 'speaking' }
            elseif ($b.MicRms -gt 700) { $state = 'hearing' }
            else { $state = 'listening' }
            # A face Gemini set is held like a brain's E line.
            $e = "$($b.Emotion)"
            if ($e -ne $script:lastEmo) {
                $script:lastEmo = $e
                if ($e -ne 'LISTENING') { Set-VoiceFaceLine "E $e" }
            }
        }
        $emo = if ($state -eq 'hearing') { 'LISTENING' }
               elseif ($script:asking) { 'THINKING' }
               elseif ($now -lt $f.until) { $f.emo }
               else { 'NEUTRAL' }
        $mouth = if ($b) { [double]$b.Mouth }
                 elseif (($now - $f.mouthAt).TotalSeconds -lt 2) { [double]$f.mouth }
                 else { 0.0 }
        $cap = ''
        if ($now -lt $f.capUntil) {
            $n = [math]::Min($f.cap.Length, [int](($now - $f.capAt).TotalMilliseconds / 40) + 1)
            $cap = $f.cap.Substring(0, $n)
        }
        $look = if ($now -lt $f.lookUntil) { @($f.lookX, $f.lookY) } else { $null }
        $active = $b -or $script:asking -or $now -lt $f.until -or $cap
        @{ state = $state; emo = $emo; mouth = $mouth; cap = $cap; look = $look; active = $active
           mic = $(if ($b) { $b.MicRms } else { 0 }); now = $now }
    }

    # Draw one frame of $v. Eased toward each target so nothing jumps;
    # -Instant snaps (for rendered images).
    function Update-VoiceFace($v, [switch]$Instant) {
        $now = $v.now
        # At rest and settled, nothing changes until the next blink: skip.
        if (-not $Instant -and -not $v.active -and $fx.settled -and
            $now -lt $fx.blinkAt -and $now -gt $fx.blinkEnd.AddMilliseconds(400)) { return }
        $k = if ($Instant) { 1.0 } else { 0.25 }
        $pose = $VOICE_POSES[$v.emo]
        if (-not $pose) { $pose = $VOICE_POSES.NEUTRAL }
        $talking = $v.mouth -gt 0.05 -or $fx.mouth -gt 0.3

        $ui.Status.Text = switch ($v.state) {
            'connecting' { 'Connecting...' }
            'speaking' { 'Speaking' }
            'hearing' { 'Hearing you' }
            'listening' { 'Listening' }
            default { if ($script:asking) { 'Thinking...' } else { 'Tap to talk' } }
        }

        # Parts on and off: only when the pose (or talking) changes.
        $key = "$($v.emo)|$talking"
        if ($fx.pose -ne $key) {
            $fx.pose = $key
            $show = @($pose.eyes) + @($pose.props)
            if ($pose.brows) { $show += 'Brows' }
            if ($pose.thumb) { $show += 'Thumb' }
            $show += if ($talking) { 'MouthOpen' } else { $pose.mouth }
            foreach ($part in $VOICE_PARTS) {
                $ui[$part].Visibility = if ($show -contains $part) { 'Visible' } else { 'Collapsed' }
            }
            # What moves by itself in this pose, worked out once.
            $fx.floaty = @($show | Where-Object { $_ -in 'PropHearts', 'PropNotes', 'PropZzz', 'PropThink', 'PropSparkle' }).Count -gt 0
            $fx.bang = $show -contains 'PropBang'
            $fx.tear = $show -contains 'Tear'
            $fx.waves = $show -contains 'PropWaves'
            $fx.posed = $false
        }
        $lookKey = if ($v.look) { "$($v.look[0]),$($v.look[1])" } else { '' }
        if ($fx.lookKey -ne $lookKey) { $fx.lookKey = $lookKey; $fx.posed = $false }

        $hex = if ($VOICE_MOODS[$v.emo]) { $VOICE_MOODS[$v.emo] }
               else {
                   switch ($v.state) {
                       'speaking' { '#D633FF' }
                       'listening' { '#00E0FF' }
                       'connecting' { '#9C9AC8' }
                       default { '#7C5CFF' }
                   }
               }
        $glowTo = switch ($v.state) {
            'speaking' { 0.6 + 0.1 * $v.mouth }
            'hearing' { 0.55 + 0.45 * [math]::Min(1.0, $v.mic / 5000.0) }
            'listening' { 0.48 }
            'connecting' { 0.32 + 0.12 * [math]::Sin($now.TimeOfDay.TotalSeconds * 4) }
            default { if ($v.emo -ne 'NEUTRAL') { 0.5 } else { 0.3 } }
        }
        $fx.mouth += ($v.mouth - $fx.mouth) * $(if ($Instant) { 1.0 } else { 0.5 })
        $fx.glow += ($glowTo - $fx.glow) * $k
        $c = ConvertTo-VoiceColor $hex
        $ck = if ($Instant) { 1.0 } else { 0.15 }
        $fx.r += ($c.R - $fx.r) * $ck
        $fx.g += ($c.G - $fx.g) * $ck
        $fx.b += ($c.B - $fx.b) * $ck
        $r = [byte][math]::Round($fx.r)
        $g = [byte][math]::Round($fx.g)
        $bl = [byte][math]::Round($fx.b)
        $fx.inner.Color = [Windows.Media.Color]::FromArgb(120, $r, $g, $bl)
        $fx.outer.Color = [Windows.Media.Color]::FromArgb(0, $r, $g, $bl)
        $fx.mood.Color = [Windows.Media.Color]::FromArgb(255, $r, $g, $bl)
        $fx.platIn.Color = [Windows.Media.Color]::FromArgb(90, $r, $g, $bl)
        $ui.GlowScale.ScaleX = 0.72 + 0.34 * $fx.glow
        $ui.GlowScale.ScaleY = $ui.GlowScale.ScaleX

        # The pose: ears, head tilt, paws, brows, cheeks, where the eyes look.
        # It eases in over half a second, and is left alone once there:
        # forty property sets a frame that change nothing cost real CPU here.
        $ears = if ($null -ne $pose.ears) { $pose.ears } else { -18 }
        if (-not $fx.posed -or $Instant) {
        $fx.earL += ($ears - $fx.earL) * $k
        $ui.EarL.Angle = $fx.earL
        $ui.EarR.Angle = -$fx.earL
        $tiltTo = if ($pose.tilt) { $pose.tilt } else { 0 }
        $fx.tilt += ($tiltTo - $fx.tilt) * $k
        $ui.Tilt.Angle = $fx.tilt
        $pl = if ($pose.pawL) { $pose.pawL } else { @(0, 0) }
        $pr = if ($pose.pawR) { $pose.pawR } else { @(0, 0) }
        $pss = if ($pose.pawScale) { $pose.pawScale } else { 1.0 }
        $fx.plx += ($pl[0] - $fx.plx) * $k; $fx.ply += ($pl[1] - $fx.ply) * $k
        $fx.prx += ($pr[0] - $fx.prx) * $k; $fx.pry += ($pr[1] - $fx.pry) * $k
        $fx.ps += ($pss - $fx.ps) * $k
        $ui.PawLMove.X = $fx.plx; $ui.PawLMove.Y = $fx.ply
        $ui.PawRMove.X = $fx.prx; $ui.PawRMove.Y = $fx.pry
        $ui.PawRScale.ScaleX = $fx.ps; $ui.PawRScale.ScaleY = $fx.ps
        $brow = if ($pose.brows) { $pose.brows } else { @(0, 0, 0) }
        $fx.bl += ($brow[0] - $fx.bl) * $k; $fx.br += ($brow[1] - $fx.br) * $k; $fx.lift += ($brow[2] - $fx.lift) * $k
        $ui.BrowL.Angle = $fx.bl; $ui.BrowR.Angle = $fx.br; $ui.BrowLift.Y = $fx.lift
        $blushTo = if ($pose.blush) { $pose.blush } else { 0.5 }
        $fx.blush += ($blushTo - $fx.blush) * $k
        $ui.CheekL.Opacity = $fx.blush; $ui.CheekR.Opacity = $fx.blush
        $look = if ($v.look) { $v.look } elseif ($pose.look) { $pose.look } else { @(0, 0) }
        $fx.lx += ($look[0] - $fx.lx) * $k; $fx.ly += ($look[1] - $fx.ly) * $k
        $ui.Look.X = $fx.lx; $ui.Look.Y = $fx.ly
        $fx.posed = [math]::Abs($ears - $fx.earL) + [math]::Abs($tiltTo - $fx.tilt) + [math]::Abs($pl[0] - $fx.plx) +
            [math]::Abs($pl[1] - $fx.ply) + [math]::Abs($pr[0] - $fx.prx) + [math]::Abs($pr[1] - $fx.pry) +
            [math]::Abs($brow[0] - $fx.bl) + [math]::Abs($brow[1] - $fx.br) + [math]::Abs($look[0] - $fx.lx) +
            [math]::Abs($look[1] - $fx.ly) -lt 0.3
        }

        # The mouth: open as wide as the voice is loud, like the dongle's.
        $open = [math]::Max($(if ($pose.open) { $pose.open } else { 0 }), 5 + 3.6 * $fx.mouth)
        if ([math]::Abs($open - $fx.openSet) -gt 0.05 -or $Instant) {
            $fx.openSet = $open
            $ui.MouthOpen.Height = $open
            $ui.MouthOpen.Width = 14 + [math]::Min(6, $open / 4)
            [Windows.Controls.Canvas]::SetLeft($ui.MouthOpen, 110 - $ui.MouthOpen.Width / 2)
            [Windows.Controls.Canvas]::SetTop($ui.MouthOpen, 127 - $open / 2)
        }

        # Blinks every few seconds; eyes sized by the face.
        if ($now -ge $fx.blinkAt) {
            $fx.blinkEnd = $now.AddMilliseconds(130)
            $fx.blinkAt = $now.AddMilliseconds((Get-Random -Minimum 2500 -Maximum 5500))
        }
        $eyeTo = if ($Instant) { $(if ($pose.eye) { $pose.eye } else { 1.0 }) }
                 elseif ($now -lt $fx.blinkEnd) { 0.1 }
                 elseif ($pose.eye) { $pose.eye } else { 1.0 }
        $fx.eye += ($eyeTo - $fx.eye) * $(if ($Instant) { 1.0 } else { 0.55 })
        $ew = if ($pose.eye -and $pose.eye -gt 1) { $pose.eye } else { 1.0 }
        if ([math]::Abs($fx.eye - $fx.eyeSet) -gt 0.005 -or $ew -ne $fx.ewSet -or $Instant) {
            $fx.eyeSet = $fx.eye
            $fx.ewSet = $ew
            $ui.EyeL.ScaleY = $fx.eye; $ui.EyeR.ScaleY = $fx.eye
            $ui.EyeL.ScaleX = $ew; $ui.EyeR.ScaleX = $ew
        }

        # Things that move by themselves while something is going on.
        # Only what this pose shows, and only while something is going on.
        $t = $now.TimeOfDay.TotalSeconds
        $lively = ($v.active -and ($v.state -eq 'speaking' -or $v.state -eq 'hearing' -or $v.emo -ne 'NEUTRAL')) -or $Instant
        if ($lively) {
            $ui.Bob.Y = [math]::Sin($t * 2.4) * $(if ($Instant) { 0 } else { 2 })
            $ui.SparkSpin.Angle = ($t * 40) % 360
            $ui.SparkScale.ScaleX = 1 + 0.15 * [math]::Sin($t * 5)
            $ui.SparkScale.ScaleY = $ui.SparkScale.ScaleX
            if ($fx.floaty) { $ui.Float.Y = [math]::Sin($t * 2) * 3 }
            if ($fx.bang) { $ui.Spin.Angle = ($t * 90) % 360 }
            if ($fx.tear) { $ui.TearFall.Y = ($t * 12) % 10 }
            if ($fx.waves) { $ui.PropWaves.Opacity = 0.45 + 0.4 * [math]::Sin($t * 6) }
        } elseif ($ui.Bob.Y -ne 0) {
            $ui.Bob.Y = 0
        }

        # The dongle's speech bubble, typed out a letter at a time.
        if ($v.cap) {
            $ui.CaptionText.Text = $v.cap
            $ui.Caption.Visibility = 'Visible'
        } elseif ($ui.Caption.Visibility -eq 'Visible') {
            $ui.Caption.Visibility = 'Collapsed'
        }

        $fx.settled = [math]::Abs($glowTo - $fx.glow) -lt 0.003 -and $fx.mouth -lt 0.01 -and
            [math]::Abs($eyeTo - $fx.eye) -lt 0.01 -and [math]::Abs($ears - $fx.earL) -lt 0.2 -and
            [math]::Abs($c.R - $fx.r) + [math]::Abs($c.G - $fx.g) + [math]::Abs($c.B - $fx.b) -lt 2
    }

    # -Shot: the docs' images, drawn by this same window - so they show what
    # it really looks like, and redrawing them after a change is one command.
    function Get-VoiceBitmap($el, [Windows.Rect]$box, [double]$scale) {
        $dv = New-Object Windows.Media.DrawingVisual
        $dc = $dv.RenderOpen()
        $vb = New-Object Windows.Media.VisualBrush($el)
        $vb.ViewboxUnits = 'Absolute'
        $vb.Viewbox = $box
        $vb.Stretch = 'Fill'
        $dc.DrawRectangle($vb, $null, (New-Object Windows.Rect(0, 0, $box.Width, $box.Height)))
        $dc.Close()
        [int]$pw = [math]::Ceiling($box.Width * $scale)
        [int]$ph = [math]::Ceiling($box.Height * $scale)
        [double]$dpi = 96.0 * $scale
        $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap -ArgumentList $pw, $ph, $dpi, $dpi,
            ([Windows.Media.PixelFormats]::Pbgra32)
        $bmp.Render($dv)
        $bmp.Freeze()
        $bmp
    }
    function Save-VoicePng($bmp, [string]$path) {
        $enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
        $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
        $fs = [IO.File]::Create($path)
        try { $enc.Save($fs) } finally { $fs.Close() }
        Write-Output "wrote $path"
    }
    function New-VoiceText([string]$text, [double]$size, [string]$hex, [string]$weight = 'Normal') {
        $face = New-Object Windows.Media.Typeface((New-Object Windows.Media.FontFamily('Segoe UI Variable Display, Segoe UI')),
            [Windows.FontStyles]::Normal, [Windows.FontWeights]::$weight, [Windows.FontStretches]::Normal)
        New-Object Windows.Media.FormattedText($text, [Globalization.CultureInfo]::InvariantCulture,
            [Windows.FlowDirection]::LeftToRight, $face, $size, (New-VoiceBrush $hex))
    }
    function Save-VoiceShots([string]$dir) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        $dir = (Resolve-Path $dir).Path
        foreach ($el in $ui.Card, $ui.Shade) { $el.Opacity = 1 }
        foreach ($sc in $ui.Pop, $ui.PopShade) { $sc.ScaleX = 1; $sc.ScaleY = 1 }
        $w.ShowActivated = $false
        $w.ShowInTaskbar = $false
        $w.Topmost = $false
        $w.Left = -20000
        $w.Top = -20000
        $w.Show()
        $at = [datetime]'2026-01-01T12:00:00.25'
        $fx.blinkAt = $at.AddYears(1)
        $render = {
            $w.UpdateLayout()
            $w.Dispatcher.Invoke([action] {}, [Windows.Threading.DispatcherPriority]::Render)
        }
        $scene = {
            param($state, $emo, $mouth, $cap, $lines, $on, $mic = 0)
            $ui.Talk.Children.Clear()
            foreach ($l in $lines) { Add-VoiceLine $l[0] $l[1] }
            Set-VoiceButton $on
            $view = @{ state = $state; emo = $emo; mouth = $mouth; cap = $cap; look = $null; active = $true; mic = $mic; now = $at }
            Update-VoiceFace $view -Instant
            & $render
            $ui.Scroll.ScrollToEnd()
            & $render
        }
        # The whole window, card and shadow layer, at its own size.
        $whole = {
            $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap -ArgumentList ([int]($w.ActualWidth * 2)),
                ([int]($w.ActualHeight * 2)), 192.0, 192.0, ([Windows.Media.PixelFormats]::Pbgra32)
            $bmp.Render($ui.Root)
            $bmp.Freeze()
            $bmp
        }
        # Not the owner's own microphone's name in a published picture.
        Set-VoiceMic -1
        $me = "$($p.name)"
        & $scene 'idle' 'THUMBSUP' 0 'CPU 12% RAM 60%' @(
            @('you', "how's my laptop doing?"),
            @('buddy', 'All good! CPU is at 12%. Memory: 9.6 of 16 GB in use (60%). C: has 120 GB free of 237 GB.')) $false
        $chatPic = & $whole
        Save-VoicePng $chatPic (Join-Path $dir 'window-chat.png')
        & $scene 'speaking' 'LOVE' 3 '' @(
            @('you', 'I finally finished the project!'),
            @('buddy', "That's wonderful. I'm so proud of you.")) $true
        $talk = & $whole
        Save-VoicePng $talk (Join-Path $dir 'window-talking.png')
        & $scene 'hearing' 'LISTENING' 0 '' @(,@('buddy', "Hi! I'm listening.")) $true 3200
        $listen = & $whole
        Save-VoicePng $listen (Join-Path $dir 'window-listening.png')

        # Every face, as the dongle names them.
        $sw = $ui.Stage.ActualWidth
        $cell = New-Object Windows.Rect((($sw - 230) / 2), 0, 230, 236)
        $order = 'NEUTRAL', 'HAPPY', 'SAD', 'ANGRY', 'SURPRISED', 'SLEEPY', 'LOVE', 'THINKING', 'LISTENING',
                 'SINGING', 'HIGHFIVE', 'HANDSUP', 'THUMBSUP', 'GAMING', 'BANG'
        $pics = @{}
        foreach ($e in $order) {
            & $scene 'idle' $e 0 '' @() $false
            $pics[$e] = Get-VoiceBitmap $ui.Stage $cell 2
        }
        $dv = New-Object Windows.Media.DrawingVisual
        $dc = $dv.RenderOpen()
        $dc.DrawRoundedRectangle((New-VoiceBrush '#0D0C22'), $null, (New-Object Windows.Rect(0, 0, 1240, 900)), 28, 28)
        for ($i = 0; $i -lt $order.Count; $i++) {
            $x = 20 + ($i % 5) * 240
            $y = 20 + [math]::Floor($i / 5) * 290
            $dc.DrawImage($pics[$order[$i]], (New-Object Windows.Rect(($x + 5), $y, 230, 236)))
            $label = New-VoiceText $order[$i].ToLower() 18 '#C9C3FF' 'SemiBold'
            $dc.DrawText($label, (New-Object Windows.Point(($x + 120 - $label.Width / 2), ($y + 244))))
        }
        $dc.Close()
        $sheet = New-Object Windows.Media.Imaging.RenderTargetBitmap(1240, 900, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
        $sheet.Render($dv)
        Save-VoicePng $sheet (Join-Path $dir 'faces.png')

        # The hero: what it is, beside two windows.
        $dv = New-Object Windows.Media.DrawingVisual
        $dc = $dv.RenderOpen()
        $bg = New-Object Windows.Media.LinearGradientBrush((ConvertTo-VoiceColor '#0B0B1E'), (ConvertTo-VoiceColor '#1C1150'), 35.0)
        $dc.DrawRectangle($bg, $null, (New-Object Windows.Rect(0, 0, 1600, 900)))
        foreach ($spot in @(@('#40D633FF', 1180, 430, 460), @('#3000E0FF', 300, 760, 380), @('#227C5CFF', 520, 120, 300))) {
            $rg = New-Object Windows.Media.RadialGradientBrush((ConvertTo-VoiceColor $spot[0]), [Windows.Media.Colors]::Transparent)
            $dc.DrawEllipse($rg, $null, (New-Object Windows.Point($spot[1], $spot[2])), $spot[3], $spot[3])
        }
        $dc.DrawText((New-VoiceText $me 112 '#FFFFFF' 'Bold'), (New-Object Windows.Point(104, 150)))
        $dc.DrawText((New-VoiceText 'Your NEXUS buddy, now on your PC' 36 '#C9C3FF'), (New-Object Windows.Point(110, 300)))
        $points = @(
            @('#00E0FF', 'Talks out loud, in a soft, calm voice'),
            @('#FF5CA8', 'Makes every face the dongle makes'),
            @('#5CFFC0', 'Knows your PC: CPU, memory, disk, battery'),
            @('#FFC85C', 'Opens quietly when Windows starts'))
        for ($i = 0; $i -lt $points.Count; $i++) {
            $y = 400 + $i * 50
            $dc.DrawEllipse((New-VoiceBrush $points[$i][0]), $null, (New-Object Windows.Point(122, ($y + 17))), 6, 6)
            $dc.DrawText((New-VoiceText $points[$i][1] 24 '#A9A5D6'), (New-Object Windows.Point(142, $y)))
        }
        $strip = 'HAPPY', 'LOVE', 'SINGING', 'GAMING', 'SLEEPY'
        for ($i = 0; $i -lt $strip.Count; $i++) {
            $dc.DrawImage($pics[$strip[$i]], (New-Object Windows.Rect((96 + $i * 122), 640, 120, 123)))
        }
        $dc.PushOpacity(0.92)
        $dc.DrawImage($chatPic, (New-Object Windows.Rect(860, 112, ($chatPic.Width * 0.94), ($chatPic.Height * 0.94))))
        $dc.Pop()
        $dc.DrawImage($talk, (New-Object Windows.Rect(1148, 82, $talk.Width, $talk.Height)))
        $dc.Close()
        $hero = New-Object Windows.Media.Imaging.RenderTargetBitmap(1600, 900, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
        $hero.Render($dv)
        Save-VoicePng $hero (Join-Path $dir 'hero.png')
        $w.Close()
    }

    $timer = New-Object Windows.Threading.DispatcherTimer
    $timer.Interval = [TimeSpan]::FromMilliseconds(33)
    $timer.Add_Tick({
        $b = $script:bv
        # Before a finished conversation is stopped: a timer it just set
        # must not be lost with it.
        Update-VoiceTimers $b
        if ($b) {
            Read-VoiceLog $b
            if ($script:greet -and $b.Ready) {
                $script:greet = $false
                [void]$b.Tell("(This is the owner's first conversation with you today. Greet them warmly, in one short sentence.)")
                Set-VoicePref 'greeted' (Get-Date -Format 'yyyy-MM-dd')
            }
            if ($script:pending -and $b.Ready) {
                if ($b.SendText($script:pending)) {
                    Add-VoiceRecent 'Owner' $script:pending
                    Invoke-VoiceCommand $script:pending
                } else {
                    Add-VoiceLine 'note' 'Not sent - the conversation has ended.'
                }
                $script:pending = $null
            }
            if ($b.Done) { Stop-VoiceTalk }
        }
        # The hotkey does what the Talk button does, and shows the window if
        # it was minimised - without taking focus from what you are typing in.
        if ($script:hotkey -and $script:hotkey.Take() -gt 0) {
            if ($w.WindowState -eq 'Minimized') { $w.WindowState = 'Normal' }
            if ($script:bv) { Stop-VoiceTalk } else { Start-VoiceTalk }
        }
        Update-VoiceHolds
        Receive-VoiceChat
        Read-VoiceMirror
        $v = Get-VoiceView
        # Nothing to draw while minimised.
        if ($w.WindowState -ne 'Minimized') { Update-VoiceFace $v }
        # 15 frames a second while something moves - a cartoon mouth needs no
        # more, and the dongle's own moves about 12 times a second - and about
        # 7 at rest, where a blink still looks like one. Measured: half the
        # CPU of 30 frames a second while talking.
        $want = if ($v.active) { 66 } else { 150 }
        if ($timer.Interval.TotalMilliseconds -ne $want) { $timer.Interval = [TimeSpan]::FromMilliseconds($want) }
    })

    # Bottom right, above the taskbar, and it pops: a quick fade and a
    # little overshoot. Then it listens at once - unless it opened by itself
    # at sign-in (-Idle), when the mic waits for a tap.
    $wa = [Windows.SystemParameters]::WorkArea
    $w.Left = $wa.Right - $w.Width - 6
    $w.Top = $wa.Bottom - $w.Height - 6
    # At sign-in it appears without taking focus; double-clicked, it does.
    $w.ShowActivated = -not $Idle
    # A short screen (a laptop at 125 %) gets a shorter window, not one off the top.
    if ($w.Height -gt $wa.Height - 12) { $w.Height = $wa.Height - 12; $w.Top = $wa.Top + 6 }
    $w.Add_Loaded({
        if ($Shot) { return }
        $ease = New-Object Windows.Media.Animation.BackEase
        $ease.EasingMode = 'EaseOut'
        $ease.Amplitude = 0.35
        $grow = New-Object Windows.Media.Animation.DoubleAnimation(0.9, 1.0, (New-Object Windows.Duration([TimeSpan]::FromMilliseconds(280))))
        $grow.EasingFunction = $ease
        $ui.Pop.BeginAnimation([Windows.Media.ScaleTransform]::ScaleXProperty, $grow)
        $ui.Pop.BeginAnimation([Windows.Media.ScaleTransform]::ScaleYProperty, $grow)
        $ui.PopShade.BeginAnimation([Windows.Media.ScaleTransform]::ScaleXProperty, $grow)
        $ui.PopShade.BeginAnimation([Windows.Media.ScaleTransform]::ScaleYProperty, $grow)
        $fade = New-Object Windows.Media.Animation.DoubleAnimation(0.0, 1.0, (New-Object Windows.Duration([TimeSpan]::FromMilliseconds(180))))
        $ui.Card.BeginAnimation([Windows.UIElement]::OpacityProperty, $fade)
        $ui.Shade.BeginAnimation([Windows.UIElement]::OpacityProperty, $fade)
        # Without the companion, or with one installed before the voice
        # relay, the voice works but the dongle's face cannot move.
        $relayUp = $true
        try {
            $relayUp = @([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveUdpListeners() |
                Where-Object { $_.Port -eq $VOICE_PORT }).Count -gt 0
        } catch {}
        if (-not $relayUp) {
            Add-VoiceLine 'note' "The companion is not running (or needs install.cmd again), so the dongle's face will not move."
        }
        $timer.Start()
        $script:hotkey = $null
        if (-not $script:buildError) {
            $script:hotkey = New-Object NexusVoice.HotKey($TALK_MODS, $TALK_VK)
            if (-not $script:hotkey.Ok) {
                $script:hotkey = $null
                Add-VoiceLine 'note' "$TALK_KEYS is taken by another app, so it cannot start a conversation. The Talk button still can."
            }
            foreach ($k in @(@('dictate', $DICTATE_VK, $DICTATE_KEYS), @('explain', $EXPLAIN_VK, $EXPLAIN_KEYS))) {
                $h = New-Object NexusVoice.HotKey($TALK_MODS, $k[1])
                if ($h.Ok) { $script:holdKeys[$k[0]] = $h }
                else { Add-VoiceLine 'note' "$($k[2]) is taken by another app, so holding its key does nothing." }
            }
        }
        # Not focusing the message box: its blinking caret redraws the whole
        # see-through window twice a second, and a window that pops up at
        # sign-in should not take the keyboard from whatever you are doing.
        if (-not $Idle) { Start-VoiceTalk }
    })
    # Closing the window ends the conversation: mic and speakers off, mouth shut.
    $w.Add_Closed({
        $timer.Stop()
        Stop-VoiceTalk -Closing
        # The camera is let go of with the window.
        if ($script:view) { $script:view.run.state.run = $false }
        if ($script:mirror) { $script:mirror.Close() }
        if ($script:hotkey) { $script:hotkey.Close() }
        foreach ($k in $script:holdKeys.Values) { $k.Close() }
        if ($script:hold) { [void]$script:hold.rec.Stop() }
    })
    Set-VoiceButton $false
    } catch {
        if ($Shot) { throw }
        [void][Windows.MessageBox]::Show("Buddy's voice window could not open:`n$(Get-Reason $_)", 'NEXUS', 'OK', 'Error')
        return
    }

    if ($Shot) {
        Save-VoiceShots $Shot
        return
    }
    [void]$w.ShowDialog()
    return
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

# CPU load from a counter kept open: NextValue() is the load since the last
# call, and returns at once. Get-Counter, which this used before, took a
# one-second sample every round - about 70 ms of CPU each time, most of this
# companion's own load - and held the round up for that second. Get-Counter
# stays as the fallback where the counter cannot be opened.
$cpuCounter = '\Processor(_Total)\% Processor Time'
$script:cpuPc = $null
try {
    $script:cpuPc = New-Object Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total')
    # One read to prime it - the first is always 0 and would put a lie on
    # the screen for the first second.
    [void]$script:cpuPc.NextValue()
} catch {
    $script:cpuPc = $null
    try { Get-Counter $cpuCounter -ErrorAction Stop | Out-Null } catch {}
}
# Memory the same way: GlobalMemoryStatusEx through .NET, at no cost, where
# a CIM query took about 16 ms of CPU and a fifth of a second every round.
$script:memInfo = $null
try {
    Add-Type -AssemblyName Microsoft.VisualBasic -ErrorAction Stop
    $script:memInfo = New-Object Microsoft.VisualBasic.Devices.ComputerInfo
} catch {}

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
    busyAt = [datetime]::MinValue; complained = @{}; history = @(); chatWaiting = $null
    steps = New-Object System.Collections.Queue; stepAt = [datetime]::MinValue
}
$nextKeyLook = [datetime]::MinValue
Open-BuddyChat
Open-VoiceRelay
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

    # The voice relay writes to whatever ports are open now. A new array
    # each time, so its thread never sees one half-changed. Over Bluetooth it
    # queues instead, for Send-VoiceBle.
    if ($script:relay) {
        $script:relay.Ports = [IO.Ports.SerialPort[]]@($open.Values)
        $script:relay.BleOn = [bool]$ble
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
        # No dongle: Buddy still answers chat.cmd and the voice window -
        # whose face shows the answer, through the mirror - but makes no
        # remarks of its own to an empty room.
        if ($Buddy -and $script:persona) {
            Send-BuddyMirror (Get-BuddyLines $null $null @('', '', 0) -ChatOnly)
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

    # Both readings are instant. Their fallbacks (Get-Counter, CIM) are not,
    # and while voice.cmd talks over Bluetooth they are skipped: they hold the
    # round up, and the mouth's lines wait on this thread.
    $voiceBle = $ble -and $script:relay -and
        ([DateTime]::UtcNow - $script:relay.HeardAt).TotalSeconds -lt 5

    try {
        if ($script:cpuPc) {
            $cpu = $script:cpuPc.NextValue()
        } elseif (-not $voiceBle) {
            $cpu = (Get-Counter $cpuCounter -ErrorAction Stop
                   ).CounterSamples[0].CookedValue
        }
        if ($null -ne $cpu) { $lines.Add('C ' + [int][math]::Round($cpu)) }
    } catch {}

    try {
        if ($script:memInfo) {
            $used = 100 * (1 - $script:memInfo.AvailablePhysicalMemory / $script:memInfo.TotalPhysicalMemory)
        } elseif (-not $voiceBle) {
            $os = Get-CimInstance Win32_OperatingSystem
            $used = 100 * (1 - $os.FreePhysicalMemory / $os.TotalVisibleMemorySize)
        }
        if ($null -ne $used) { $lines.Add('M ' + [int][math]::Round($used)) }
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
        Send-BuddyMirror $lines
    }

    foreach ($name in @($open.Keys)) {
        # The voice relay's thread writes to this port too; the same lock
        # keeps its lines and these from interleaving mid-line.
        $sp = $open[$name]
        [Threading.Monitor]::Enter($sp)
        try {
            foreach ($line in $lines) {
                try {
                    $sp.WriteLine($line)
                    Write-Verbose "$name > $line"
                } catch {
                    Write-Warning "$name lost: $(Get-Reason $_)"
                    try { $sp.Close() } catch {}
                    $open.Remove($name)
                    break
                }
            }
        } finally {
            [Threading.Monitor]::Exit($sp)
        }
    }
    if ($script:relay) { $script:relay.Ports = [IO.Ports.SerialPort[]]@($open.Values) }

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
    # The wait between rounds doubles as the voice relay's Bluetooth pump:
    # every 40 ms while a voice session is talking, every 200 ms otherwise,
    # so a companion nobody is talking to is not woken 25 times a second.
    $until = (Get-Date).AddMilliseconds([int]($Interval * 1000))
    while ($true) {
        if ($ble -and $script:relay -and -not (Send-VoiceBle $ble)) {
            Write-Warning "bluetooth $($ble.Name) lost"
            Close-BleLink $ble
            $ble = $null
            $script:relay.BleOn = $false
            $nextBle = (Get-Date).AddSeconds($BLE_RESCAN)
        }
        $left = ($until - (Get-Date)).TotalMilliseconds
        if ($left -le 0) { break }
        $step = if ($ble -and $script:relay -and
                    ([DateTime]::UtcNow - $script:relay.HeardAt).TotalSeconds -lt 5) { 40 } else { 200 }
        Start-Sleep -Milliseconds ([int][math]::Min($left, $step))
    }
  }
} finally {
    # The relay first, so nothing else writes to the ports while they close.
    if ($script:relay) { try { $script:relay.Close() } catch {} }
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
    if ($script:chatUdp) { try { $script:chatUdp.Close() } catch {} }
    Write-Output 'stopped'
}
