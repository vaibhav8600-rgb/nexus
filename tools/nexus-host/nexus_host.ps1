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

function Get-NowPlaying {
    # Windows has no scriptable now-playing without extra packages. Left
    # empty rather than guessed at: an empty field is honest, a wrong one
    # is not. Use the Python companion with winsdk if you want this.
    return ''
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

# Which port? If not told, try every NEXUS interface at once: the one that is
# the host link shows up on the dongle, and the other is Studio's and ignores
# us. Writing a line it cannot parse costs Studio nothing.
$targets = if ($Port) { @($Port) } else { (Get-NexusPorts).Port }
if (-not $targets) {
    Write-Error 'No NEXUS dongle found. Plug in USB, or pass -Port.'
    return
}

$open = @{}
foreach ($name in $targets) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort($name, 115200)
        $sp.WriteTimeout = 2000
        $sp.DtrEnable = $true
        $sp.Open()
        $open[$name] = $sp
        Write-Output "open $name"
    } catch {
        Write-Warning "$name : $($_.Exception.Message)"
    }
}
if ($open.Count -eq 0) { Write-Error 'No port could be opened.'; return }

# One sample to prime the counter - the first read of % Processor Time is
# always 0 and would put a lie on the screen for the first second.
$cpuCounter = '\Processor(_Total)\% Processor Time'
try { Get-Counter $cpuCounter -ErrorAction Stop | Out-Null } catch {}

Write-Output 'NEXUS companion running. Ctrl-C to stop.'
$lastClock = [datetime]::MinValue
$lastNp = $null

try {
    while ($true) {
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
        if ($open.Count -eq 0) {
            Write-Error 'All ports gone. Reflashed? Run it again.'
            return
        }
        Start-Sleep -Milliseconds ([int]($Interval * 1000))
    }
} finally {
    # "I am going away", so the dongle shows dashes rather than numbers that
    # stopped being true the moment this stopped.
    foreach ($name in @($open.Keys)) {
        try { $open[$name].WriteLine('X'); $open[$name].Close() } catch {}
    }
    Write-Output 'stopped'
}
