# GeneralsVR installer + launcher.
# Run with -Install for first-time setup (GeneralsVR-Setup.cmd does this for you);
# afterwards the desktop shortcut runs this same script as the launcher.
#
# Everything lives in %LOCALAPPDATA%\GeneralsVR - NOTHING is written into the
# Zero Hour game folder. The game finds its assets because the launcher starts
# the exe with the game folder as working directory (Zero Hour data) and via
# the EA registry keys (base Generals data).
#
# What it manages:
#   - downloads the newest release from GitHub (or the version you pin with [V])
#   - the EA registry keys a Steam install never gets (one-time, asks for admin)
#   - the HKCU\Software\Wine\VR keys DXVK needs to enable the Vulkan extensions
#     OpenXR requires, written for YOUR GPU (detected automatically)
#   - Options.ini (missing = known startup crash), desktop shortcut, launch flags
#
# -Uninstall removes the install folder and shortcut (your game is untouched).

param(
    [switch]$Install,
    [switch]$RegistryOnly,
    [switch]$Uninstall,
    [string]$GamePath
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Repo       = 'Gonzorro/GeneralsVR'
$ApiBase    = "https://api.github.com/repos/$Repo"
$ExeName    = 'generalszhv.exe'
$InstallDir = Join-Path $env:LOCALAPPDATA 'GeneralsVR'
$ConfigPath = Join-Path $InstallDir 'generalsvr.json'
$PathFile   = Join-Path $InstallDir 'game-path.txt'
$Headers    = @{ 'User-Agent' = 'GeneralsVR-Launcher' }

# ---------------------------------------------------------------- game folder

function Test-GameDir([string]$dir) {
    return ($dir) -and (Test-Path (Join-Path $dir 'INIZH.big'))
}

# The player can point us at any copy of the game (Steam, GOG, retail, a weird
# path) by writing the folder into game-path.txt. This is the manual override.
function Get-GamePathFile {
    if (-not (Test-Path $PathFile)) { return $null }
    foreach ($line in (Get-Content $PathFile)) {
        $t = $line.Trim()
        if ($t -and -not $t.StartsWith('#')) { return $t.Trim('"').TrimEnd('\') }
    }
    return $null
}

function Write-GamePathTemplate {
    @'
# GeneralsVR - where is your Zero Hour game folder?
#
# The launcher normally finds the game on its own (Steam, EA app, Origin, GOG).
# If it can't - or you want to point it at a specific copy - type the FULL PATH
# to your Zero Hour folder on a line of its own below (the folder that contains
# INIZH.big), then save this file and start GeneralsVR again.
#
# Examples (delete the leading # and use YOUR real path):
#   C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour
#   D:\GOG Games\Command and Conquer Generals Zero Hour
#   C:\Program Files (x86)\EA Games\Command and Conquer Generals Zero Hour

'@ | Set-Content $PathFile -Encoding utf8
}

# Auto-detect across Steam / EA / Origin / GOG. Returns $null if nothing found.
function Find-GameDir {
    $candidates = @()

    # Steam: main install plus every extra library folder
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $steam = (Get-ItemProperty -Path $k -ErrorAction Stop).InstallPath
            if ($steam) {
                $candidates += (Join-Path $steam 'steamapps\common\Command & Conquer Generals - Zero Hour')
                $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
                if (Test-Path $vdf) {
                    foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
                        $lib = $m.Groups[1].Value -replace '\\\\', '\'
                        $candidates += (Join-Path $lib 'steamapps\common\Command & Conquer Generals - Zero Hour')
                    }
                }
            }
        } catch {}
    }

    # EA app / Origin / retail - the game's own installer writes these keys
    foreach ($k in
        'HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour',
        'HKLM:\SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour') {
        try { $candidates += (Get-ItemProperty -Path $k -ErrorAction Stop).InstallPath } catch {}
    }

    # GOG - each installed game gets a numbered key carrying a "path" value
    foreach ($base in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
        if (Test-Path $base) {
            foreach ($sub in (Get-ChildItem $base -ErrorAction SilentlyContinue)) {
                try { $candidates += (Get-ItemProperty -Path $sub.PSPath -ErrorAction Stop).path } catch {}
            }
        }
    }

    # last-resort fixed locations
    $candidates += 'C:\Program Files (x86)\Origin Games\Command and Conquer Generals Zero Hour'
    $candidates += 'C:\Program Files (x86)\EA Games\Command and Conquer Generals Zero Hour'

    foreach ($c in ($candidates | Where-Object { $_ } | Sort-Object -Unique)) {
        $c = $c.Trim().TrimEnd('\')
        if (Test-GameDir $c) { return $c }
    }
    return $null
}

# ------------------------------------------------------------ config, releases

function Get-Config {
    if (Test-Path $ConfigPath) { return (Get-Content $ConfigPath -Raw | ConvertFrom-Json) }
    return [PSCustomObject]@{
        installed  = ''
        pin        = ''
        autoUpdate = $true
        gamePath   = ''
        flags      = '-vr -win -noshellmap -vrscale 500 -vrres 1.0'
    }
}

function Save-Config($cfg) {
    $cfg | ConvertTo-Json | Set-Content $ConfigPath -Encoding utf8
}

# Returns an array of published releases (possibly empty = GitHub reached, none
# published). Returns $null when GitHub could not be reached at all (offline or
# rate-limited) - callers tell the two apart so the error message is honest.
function Get-Releases {
    for ($i = 0; $i -lt 3; $i++) {
        try {
            $rel = Invoke-RestMethod -Uri "$ApiBase/releases" -Headers $Headers -UseBasicParsing
            return ,@($rel | Where-Object { -not $_.draft })
        } catch {
            if ($i -lt 2) { Start-Sleep -Seconds ($i + 1) }
        }
    }
    return $null
}

function Install-Build($release) {
    $asset = $release.assets | Where-Object { $_.name -like 'GeneralsVR-*.zip' } | Select-Object -First 1
    if (-not $asset) { throw "Release $($release.tag_name) has no GeneralsVR-*.zip asset." }
    Write-Host ("Downloading {0} ({1:n1} MB)..." -f $release.tag_name, ($asset.size / 1MB)) -ForegroundColor Cyan
    $tmpZip = Join-Path $env:TEMP 'GeneralsVR-update.zip'
    $tmpDir = Join-Path $env:TEMP 'GeneralsVR-update'
    Invoke-WebRequest -Uri $asset.browser_download_url -Headers $Headers -OutFile $tmpZip -UseBasicParsing
    if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force }
    Expand-Archive $tmpZip -DestinationPath $tmpDir -Force
    Copy-Item (Join-Path $tmpDir '*') $InstallDir -Recurse -Force
    Remove-Item $tmpZip -Force
    Remove-Item $tmpDir -Recurse -Force
    $cfg = Get-Config
    $cfg.installed = $release.tag_name
    Save-Config $cfg
    Write-Host "Installed $($release.tag_name)." -ForegroundColor Green
}

function Copy-LocalFiles {
    # Running from an extracted zip (not from the install folder): copy everything
    # that came with this script into the install folder and continue from there.
    $here = Split-Path -Parent $PSCommandPath
    if ($here -eq $InstallDir) { return }
    if (-not (Test-Path (Join-Path $here $ExeName))) { return }
    $script:CopiedFromZip = $true
    Write-Host "Copying files into $InstallDir..." -ForegroundColor Cyan
    Copy-Item (Join-Path $here '*') $InstallDir -Recurse -Force -Exclude 'generalsvr.json', 'game-path.txt'
    $cfg = Get-Config
    if (-not $cfg.installed) {
        $cfg.installed = 'local-zip'
        Save-Config $cfg
    }
}

# ---------------------------------------------------------------- registry etc

function Test-IsAdmin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-MachineKeys {
    foreach ($name in 'Command and Conquer Generals Zero Hour', 'ZeroHour', 'Generals') {
        if (-not (Test-Path "HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\$name")) { return $false }
    }
    return $true
}

function Set-MachineKeys([string]$gameDir) {
    # Mirrors the game's installScript.vdf, plus the EA Games\Generals key the engine
    # hard-requires but Steam's script never writes (it points at the base Generals
    # assets inside the Zero Hour folder - that is also how the exe finds the game's
    # data without living in its folder).
    $ea = 'HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games'
    $keys = @(
        @{ Path = "$ea\Command and Conquer Generals Zero Hour"; Values = [ordered]@{
            InstallPath = "$gameDir\"; language = 'english'
            UserDataLeafName = 'Command and Conquer Generals Zero Hour Data'
            MapPackVersion = 65536; version = 65540 } }
        @{ Path = "$ea\ZeroHour"; Values = [ordered]@{
            InstallPath = "$gameDir\ZH_Generals"; language = 'english'
            MapPackVersion = 65536; version = 65544 } }
    )
    foreach ($k in $keys) {
        if (-not (Test-Path $k.Path)) { New-Item -Path $k.Path -Force | Out-Null }
        foreach ($name in $k.Values.Keys) {
            $v = $k.Values[$name]
            $type = 'String'
            if ($v -is [int]) { $type = 'DWord' }
            New-ItemProperty -Path $k.Path -Name $name -Value $v -PropertyType $type -Force | Out-Null
        }
    }

    # Base Generals assets. Zero Hour needs the original Generals .big files; on
    # the Steam "Ultimate Collection" they sit in the ZH_Generals subfolder. A
    # retail or GOG copy may keep Generals somewhere else and its own installer
    # already wrote this key - so only set it when it is missing or points at a
    # folder that no longer exists. Never clobber a valid one.
    $gen = "$ea\Generals"
    $existing = $null
    try { $existing = (Get-ItemProperty -Path $gen -ErrorAction Stop).InstallPath } catch {}
    if (-not $existing -or -not (Test-Path $existing)) {
        if (-not (Test-Path $gen)) { New-Item -Path $gen -Force | Out-Null }
        $genPath = if (Test-Path (Join-Path $gameDir 'ZH_Generals')) { "$gameDir\ZH_Generals\" } else { "$gameDir\" }
        New-ItemProperty -Path $gen -Name InstallPath -Value $genPath -PropertyType String -Force | Out-Null
    }
}

function Confirm-MachineKeys([string]$gameDir) {
    if (Test-MachineKeys) { return }
    if (Test-IsAdmin) { Set-MachineKeys $gameDir; return }
    Write-Host 'One-time setup: the game requires a few registry entries Steam never writes.' -ForegroundColor Cyan
    Write-Host 'Windows will now ask for administrator permission for that single step.' -ForegroundColor Cyan
    Start-Process powershell -Verb RunAs -Wait -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-RegistryOnly', '-GamePath', "`"$gameDir`"")
    if (-not (Test-MachineKeys)) { throw 'The registry entries are still missing (was the admin prompt cancelled?).' }
}

function Set-VulkanKeys {
    # DXVK reads extra Vulkan extension lists from HKCU\Software\Wine\VR (its OpenVR
    # provider). Without these the OpenXR runtime cannot share images with DXVK's
    # device (xrCreateSession fails). Values are REG_BINARY ASCII, no trailing NUL.
    $key = 'HKCU:\Software\Wine\VR'
    if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
    New-ItemProperty -Path $key -Name state -PropertyType DWord -Value 1 -Force | Out-Null
    $inst = 'VK_KHR_surface VK_KHR_external_memory_capabilities VK_KHR_win32_surface VK_KHR_external_fence_capabilities VK_KHR_external_semaphore_capabilities VK_KHR_get_physical_device_properties2'
    $dev  = 'VK_KHR_swapchain VK_KHR_external_memory VK_KHR_external_memory_win32 VK_KHR_external_fence VK_KHR_external_fence_win32 VK_KHR_external_semaphore VK_KHR_external_semaphore_win32 VK_KHR_get_memory_requirements2 VK_KHR_dedicated_allocation'
    New-ItemProperty -Path $key -Name 'openvr_vulkan_instance_extensions' -PropertyType Binary `
        -Value ([Text.Encoding]::ASCII.GetBytes($inst)) -Force | Out-Null
    $gpuIds = @()
    foreach ($pnp in (Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty PNPDeviceID)) {
        if ($pnp -match 'VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})') {
            $gpuIds += ('PCIID:{0}:{1}' -f $Matches[1].ToLower(), $Matches[2].ToLower())
        }
    }
    foreach ($id in ($gpuIds | Sort-Object -Unique)) {
        New-ItemProperty -Path $key -Name $id -PropertyType Binary `
            -Value ([Text.Encoding]::ASCII.GetBytes($dev)) -Force | Out-Null
    }
}

function Confirm-OptionsIni {
    # A missing Options.ini is a known startup crash (GeneralsGameCode issue #97).
    $dir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Command and Conquer Generals Zero Hour Data'
    $ini = Join-Path $dir 'Options.ini'
    if (Test-Path $ini) { return }
    New-Item -ItemType Directory -Force $dir | Out-Null
    @'
AnisotropyLevel = 2
AntiAliasing = 0
BuildingOcclusion = yes
CampaignDifficulty = 0
DynamicLOD = no
ExtraAnimations = yes
GameTimeFontSize = 8
Gamma = 50
HeatEffects = yes
IdealStaticGameLOD = VeryHigh
MaxParticleCount = 5000
MaxRenderFPS = 60
MusicVolume = 55
Resolution = 1920 1080
Retaliation = yes
SFX3DVolume = 79
SFXVolume = 71
ScrollFactor = 50
ShowSoftWaterEdge = yes
ShowTrees = yes
StaticGameLOD = VeryHigh
TextureReduction = 0
UseCloudMap = yes
UseLightMap = yes
UseShadowDecals = yes
UseShadowVolumes = yes
VoiceVolume = 70
'@ | Set-Content $ini -Encoding ascii
}

function Confirm-Shortcut([string]$gameDir) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'GeneralsVR.lnk'
    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = 'powershell.exe'
    $s.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'GeneralsVR.ps1')`""
    $s.WorkingDirectory = $InstallDir
    $ico = Join-Path $gameDir 'GeneralsZH.ico'
    if (Test-Path $ico) { $s.IconLocation = $ico }
    $s.Save()
}

# --------------------------------------------------------------------- launch

function Start-Game([string]$gameDir) {
    $exe = Join-Path $InstallDir $ExeName
    if (-not (Test-Path $exe)) { throw "No game build installed yet ($exe is missing) - update first." }
    $cfg = Get-Config
    Confirm-MachineKeys $gameDir
    Set-VulkanKeys
    Confirm-OptionsIni
    Write-Host ''
    Write-Host '  Put the headset on (Quest Link running). The headset shows only the' -ForegroundColor Yellow
    Write-Host '  BATTLEFIELD - it stays blue/dark until you are in a battle.' -ForegroundColor Yellow
    Write-Host '  Click SKIRMISH, pick a map, hit START, then look around.' -ForegroundColor Yellow
    Write-Host ''
    Start-Process -FilePath $exe -WorkingDirectory $gameDir -ArgumentList ($cfg.flags -split ' ') -Wait
}

# ----------------------------------------------------------------------- menu

function Show-VersionPicker($releases) {
    if ($releases.Count -eq 0) { Write-Host 'No releases reachable (offline?).' -ForegroundColor Yellow; return }
    Write-Host ''
    Write-Host 'Available versions (0 = newest, auto-update):'
    for ($i = 0; $i -lt $releases.Count; $i++) {
        $r = $releases[$i]
        Write-Host ("  {0,2}. {1,-16} {2}  ({3:yyyy-MM-dd})" -f ($i + 1), $r.tag_name, $r.name, [datetime]$r.published_at)
    }
    $pick = Read-Host 'Version number'
    $cfg = Get-Config
    if ($pick -eq '0') {
        $cfg.pin = ''
        Save-Config $cfg
        Write-Host 'Unpinned - the launcher follows the newest release again.' -ForegroundColor Green
        return
    }
    $n = 0
    if (-not [int]::TryParse($pick, [ref]$n)) { return }
    if ($n -lt 1 -or $n -gt $releases.Count) { return }
    $r = $releases[$n - 1]
    $cfg.pin = $r.tag_name
    Save-Config $cfg
    if ((Get-Config).installed -ne $r.tag_name) { Install-Build $r }
    Write-Host "Pinned to $($r.tag_name). Press V again and pick 0 to go back to auto." -ForegroundColor Green
}

function Show-Menu([string]$gameDir) {
    $releases = Get-Releases
    if ($null -eq $releases) {
        Write-Host "(Couldn't reach GitHub - playing your installed version. Updates paused.)" -ForegroundColor DarkYellow
        $releases = @()
    }
    $cfg = Get-Config

    # resolve what should be installed right now
    $target = $null
    if ($cfg.pin) {
        $target = $releases | Where-Object { $_.tag_name -eq $cfg.pin } | Select-Object -First 1
    } elseif ($cfg.autoUpdate -and $releases.Count -gt 0) {
        $target = $releases[0]
    }
    if ($target -and $target.tag_name -ne $cfg.installed) {
        Install-Build $target
        $cfg = Get-Config
    }

    while ($true) {
        $latestTag = '?'
        if ($releases.Count -gt 0) { $latestTag = $releases[0].tag_name }
        $mode = 'auto-update ON'
        if ($cfg.pin) { $mode = "PINNED to $($cfg.pin)" }
        elseif (-not $cfg.autoUpdate) { $mode = 'auto-update OFF' }
        Write-Host ''
        Write-Host '===============================================' -ForegroundColor DarkGreen
        Write-Host "  GeneralsVR  $($cfg.installed)   (newest: $latestTag, $mode)" -ForegroundColor Green
        Write-Host '===============================================' -ForegroundColor DarkGreen
        Write-Host '  [Enter] Play      [V] Switch version'
        Write-Host '  [A] Toggle auto-update      [Q] Quit'
        $k = Read-Host 'Choice'
        if ($k -eq '') { Start-Game $gameDir; return }
        elseif ($k -match '^[Vv]$') { Show-VersionPicker $releases; $cfg = Get-Config }
        elseif ($k -match '^[Aa]$') {
            $cfg.autoUpdate = -not $cfg.autoUpdate
            Save-Config $cfg
        }
        elseif ($k -match '^[Qq]$') { return }
    }
}

# ----------------------------------------------------------------------- main

try {
    if ($RegistryOnly) {
        if (-not $GamePath) { throw 'RegistryOnly requires -GamePath.' }
        Set-MachineKeys $GamePath
        exit 0
    }

    if ($Uninstall) {
        $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'GeneralsVR.lnk'
        if (Test-Path $lnk) { Remove-Item $lnk -Force }
        if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
        Write-Host 'GeneralsVR removed. Your Zero Hour installation was never touched.' -ForegroundColor Green
        exit 0
    }

    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $script:CopiedFromZip = $false
    Copy-LocalFiles

    $cfg = Get-Config
    $gameDir = $GamePath
    if (-not (Test-GameDir $gameDir)) { $gameDir = Get-GamePathFile }   # manual override wins over the cache
    if (-not (Test-GameDir $gameDir)) { $gameDir = $cfg.gamePath }
    if (-not (Test-GameDir $gameDir)) { $gameDir = Find-GameDir }
    if (-not (Test-GameDir $gameDir)) {
        Write-GamePathTemplate
        Write-Host ''
        Write-Host "I couldn't find your Command & Conquer: Generals Zero Hour install." -ForegroundColor Yellow
        Write-Host "I've opened a file called game-path.txt - paste the path to your game" -ForegroundColor Yellow
        Write-Host "folder into it, save, then start GeneralsVR again." -ForegroundColor Yellow
        try { Start-Process notepad.exe $PathFile } catch {}
        throw "Waiting for your game folder in game-path.txt ($PathFile)."
    }
    if ($cfg.gamePath -ne $gameDir) {
        $cfg.gamePath = $gameDir
        Save-Config $cfg
    }
    if ($script:CopiedFromZip) { Confirm-Shortcut $gameDir }

    if ($Install) {
        Write-Host ''
        Write-Host 'GeneralsVR setup' -ForegroundColor Green
        Write-Host "Zero Hour found: $gameDir"
        Write-Host "Installing to:   $InstallDir  (your game folder is not touched)"
        $releases = Get-Releases
        $haveExe = Test-Path (Join-Path $InstallDir $ExeName)
        if ($null -eq $releases) {
            if (-not $haveExe) {
                throw "Couldn't reach GitHub to download the game - this is usually a network hiccup or GitHub's rate limit. Check your connection and run the setup again in a minute."
            }
            Write-Host "Couldn't reach GitHub - keeping the version already installed." -ForegroundColor DarkYellow
        } elseif ($releases.Count -gt 0) {
            Install-Build $releases[0]
        } elseif (-not $haveExe) {
            throw 'No published builds were found yet. Check back soon.'
        }
        Confirm-MachineKeys $gameDir
        Set-VulkanKeys
        Confirm-OptionsIni
        Confirm-Shortcut $gameDir
        Write-Host ''
        Write-Host 'Done! A "GeneralsVR" shortcut is on your desktop - always start the game' -ForegroundColor Green
        Write-Host 'from there (it keeps you updated and lets you switch versions with V).' -ForegroundColor Green
        $r = Read-Host 'Play now? (Y/n)'
        if ($r -eq '' -or $r -match '^[Yy]') { Start-Game $gameDir }
    } else {
        Show-Menu $gameDir
    }
} catch {
    Write-Host ''
    Write-Host "Something went wrong: $_" -ForegroundColor Red
    Write-Host 'Ask for help: https://github.com/Gonzorro/GeneralsVR/issues' -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
