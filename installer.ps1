# Dual Input mod for Warhammer 40,000: Space Marine 2 - installer / uninstaller.
# Run through install.bat or uninstall.bat.
param(
    [switch]$Uninstall,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$AppId      = '2183900'
$GameFolder = 'Space Marine 2'
$TargetSub  = 'client_pc\root\bin\pc'
$ExeName    = 'Warhammer 40000 Space Marine 2 - Retail.exe'
$ModFiles   = @('dinput8.dll')
$IniName    = 'xinput_mouse.ini'
$LogNames   = @('mixed_input.log', 'mixed_input.old.log')
$BackupDir  = 'DualInput_backup'
# SHA-256 of the Steam build the mod was made and tested on (Steam build 25799387).
$KnownExe   = '8E33F3DC6E0B4475C0F55AD180F35D45ECCD64F43FCCD45F81F9375BD3870718'
$ModDir     = Join-Path $PSScriptRoot 'mod'

# Returns the bin\pc folder (the one with the Retail exe), or $null.
function Resolve-Target($dir) {
    if (-not $dir) { return $null }
    foreach ($d in $dir, (Join-Path $dir $TargetSub)) {
        if (Test-Path -LiteralPath (Join-Path $d $ExeName)) { return $d }
    }
    return $null
}

function Find-GameDir {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($steam) { break }
        } catch {}
    }
    if (-not $steam) { return $null }

    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libs | Select-Object -Unique) {
        $apps = Join-Path $lib 'steamapps'
        # Prefer the folder named in the game's Steam manifest, fall back to the default name.
        $names = @()
        $acf = Join-Path $apps "appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $names += $m.Groups[1].Value }
        }
        $names += $GameFolder
        foreach ($n in $names | Select-Object -Unique) {
            $t = Resolve-Target (Join-Path $apps "common\$n")
            if ($t) { return $t }
        }
    }
    return $null
}

Write-Host ''
Write-Host '=== Dual Input mod for Warhammer 40,000: Space Marine 2 ===' -ForegroundColor Cyan
Write-Host 'OFFLINE ONLY: the game must run with Easy Anti-Cheat OFF (campaign / offline PvE).' -ForegroundColor Yellow
Write-Host ''

$GameDir = Resolve-Target $GameDir
if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not $GameDir) {
    Write-Host 'The game was not found automatically (or the folder is wrong).' -ForegroundColor Yellow
    $in = (Read-Host "Paste the game folder (the one with $GameFolder, or $TargetSub), or leave empty to cancel").Trim('" ')
    if (-not $in) { Write-Host 'Cancelled.'; exit 1 }
    $GameDir = Resolve-Target $in
    if (-not $GameDir) { Write-Host "$ExeName not found in: $in" -ForegroundColor Yellow }
}
Write-Host "Install folder: $GameDir"

$backup = Join-Path $GameDir $BackupDir
$launch = '"' + (Join-Path $GameDir $ExeName) + '" %command%'

if (Get-Process | Where-Object { $_.Path -and $_.Path -like "*$ExeName" }) {
    Write-Host ''
    Write-Host 'The game is running. Close it first, then run this again.' -ForegroundColor Yellow
    exit 1
}

if ($Uninstall) {
    foreach ($f in $ModFiles + $LogNames) {
        $path = Join-Path $GameDir $f
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "  removed  $f" }
    }
    if (Test-Path $backup) {
        Get-ChildItem $backup | ForEach-Object {
            Move-Item $_.FullName (Join-Path $GameDir $_.Name) -Force
            Write-Host "  restored $($_.Name) (from before the mod)"
        }
        Remove-Item $backup -Force -Recurse
    }
    $ini = Join-Path $GameDir $IniName
    if (Test-Path $ini) {
        $a = Read-Host "Delete your settings file $IniName too? (y/N)"
        if ($a -match '^[yY]') { Remove-Item $ini -Force; Write-Host "  removed  $IniName" }
        else { Write-Host "  kept     $IniName" }
    }
    Write-Host ''
    Write-Host 'Mod uninstalled.' -ForegroundColor Green
    Write-Host 'To play online with Easy Anti-Cheat again, clear the Launch Options in Steam'
    Write-Host '(right-click the game > Properties > General > Launch Options).'
    exit 0
}

$hash = (Get-FileHash -LiteralPath (Join-Path $GameDir $ExeName) -Algorithm SHA256).Hash
if ($hash -ne $KnownExe) {
    Write-Host ''
    Write-Host "WARNING: your game exe is a different version than the one the mod" -ForegroundColor Yellow
    Write-Host 'was tested on (Steam build 25799387). The mod finds the game code by pattern' -ForegroundColor Yellow
    Write-Host 'and checks it before hooking; if it does not match, it turns itself off and' -ForegroundColor Yellow
    Write-Host 'the game behaves as without the mod (see mixed_input.log).' -ForegroundColor Yellow
    $a = Read-Host 'Install anyway? (y/N)'
    if ($a -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }
}

foreach ($f in $ModFiles) {
    $src = Join-Path $ModDir $f
    $dst = Join-Path $GameDir $f
    if ((Test-Path $dst) -and (Get-FileHash $dst).Hash -ne (Get-FileHash $src).Hash) {
        # Another mod (or an older copy) uses the same file name: keep it, unless it is
        # an earlier version of this mod (it then reads xinput_mouse.ini).
        $isOurs = Select-String -Path $dst -Pattern $IniName -SimpleMatch -Quiet
        if (-not $isOurs -and -not (Test-Path (Join-Path $backup $f))) {
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
            Copy-Item $dst (Join-Path $backup $f)
            Write-Host "  backed up existing $f to $BackupDir\" -ForegroundColor Yellow
        }
    }
    Copy-Item $src $dst -Force
    Write-Host "  installed $f"
}

$ini = Join-Path $GameDir $IniName
if (Test-Path $ini) {
    Write-Host "  kept your existing $IniName (settings unchanged)"
} else {
    Copy-Item (Join-Path $ModDir $IniName) $ini
    Write-Host "  installed $IniName"
}

try { Set-Clipboard -Value $launch; $clip = $true } catch { $clip = $false }

Write-Host ''
Write-Host 'Done. Next steps:' -ForegroundColor Green
Write-Host '  1. The game must start WITHOUT Easy Anti-Cheat. In Steam: right-click the game >'
Write-Host '     Properties > General > Launch Options, and paste this line:'
Write-Host ''
Write-Host "     $launch" -ForegroundColor White
Write-Host ''
if ($clip) { Write-Host '     (it is already copied to your clipboard)' }
Write-Host '     Then start the game with Play as usual. Online/PvP needs EAC: clear the line to go back.'
Write-Host '  2. Steam Input: gyro -> "As Mouse", right trackpad -> "Joystick" (see README).'
Write-Host "  3. Settings: $ini"
Write-Host '  4. To remove the mod, run uninstall.bat.'
