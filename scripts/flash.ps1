<#
.SYNOPSIS
  Installiert alle Abhaengigkeiten und flasht die ESP-Anwesenheit-Firmware
  auf ein leeres WT32-ETH01-Board unter Windows.

.DESCRIPTION
  - Installiert Python + PlatformIO, falls nicht vorhanden (ueber winget/pip)
  - Installiert Git, falls nicht vorhanden (ueber winget)
  - Ermittelt den Projekt-Checkout: liegt dieses Skript bereits in scripts/
    innerhalb eines vollstaendigen ESP-Anwesenheit-Checkouts (Normalfall bei
    "git clone"), wird dieser verwendet. Wurde stattdessen nur diese eine
    Datei heruntergeladen (z.B. per Rechtsklick "Speichern unter" auf
    GitHub, ohne den Rest des Repos), fehlt firmware/platformio.ini dort -
    in dem Fall klont das Skript das komplette Repository selbst nach
    -RepoPath (Default: ein Ordner "ESP-Anwesenheit" neben dem Skript).
  - Legt firmware/include/config.h aus der Vorlage an, falls sie noch fehlt
    (wird nie ueberschrieben, falls bereits vorhanden)
  - Baut die Firmware (pio run) zur Kontrolle
  - Flasht sie auf das per USB-Adapter angeschlossene Board
    (pio run --target upload)

  Abhaengigkeits-Erkennung ist bewusst "funktional" (ruft z.B.
  "python --version" auf und prueft die Ausgabe), nicht nur eine
  PATH-Pruefung: Windows legt standardmaessig einen "python"-Store-Alias
  auf PATH, der vorhanden aussieht, aber kein echtes Python ist.

  Board-Verkabelung/Boot-Modus siehe docs/flash-anleitung.txt - dieses
  Skript kann den manuellen Boot-Modus-Handgriff (IO0/EN) nicht
  automatisieren, falls der verwendete USB-Adapter kein DTR/RTS hat.

.PARAMETER RepoPath
  Zielordner fuer den Checkout, falls unter scripts/../ noch keiner liegt
  (siehe .DESCRIPTION). Default: ein Ordner "ESP-Anwesenheit" neben diesem
  Skript.

.PARAMETER Port
  Serieller Port des Boards (z.B. COM7), falls die automatische Erkennung
  fehlschlaegt oder mehrere USB-Seriell-Adapter angeschlossen sind.

.PARAMETER SkipUpload
  Nur bauen, nicht flashen (z.B. um vorab zu pruefen, ob alles compiliert,
  ohne dass ein Board angeschlossen ist).

.EXAMPLE
  .\flash.ps1

.EXAMPLE
  .\flash.ps1 -Port COM7

.EXAMPLE
  .\flash.ps1 -SkipUpload

.EXAMPLE
  .\flash.ps1 -RepoPath C:\Projekte\ESP-Anwesenheit
#>

[CmdletBinding()]
param(
  [string]$RepoPath,
  [string]$Port,
  [switch]$SkipUpload
)

$ErrorActionPreference = "Stop"

$RepoUrl = "https://github.com/peterhagelhof7-cmd/ESP-Anwesenheit.git"

# Versionierung dieses Skripts (unabhaengig von der Firmware-Version) -
# Muster aus dem sensormeter-Projekt uebernommen, siehe docs/entscheidungen.md.
#
# Changelog:
#   1.1.0 (2026-07-30) - Klont das Repository selbst, falls nur flash.ps1
#                         allein heruntergeladen wurde statt des gesamten
#                         Checkouts (siehe .DESCRIPTION) - vorher brach das
#                         Skript in dem Fall mit "platformio.ini nicht
#                         gefunden" ab, real bei einem Nutzer aufgetreten.
#   1.0.0 (2026-07-29) - Erste Fassung.
$FlashScriptVersion = "1.1.0"

Write-Host "ESP-Anwesenheit Flash-Skript v$FlashScriptVersion" -ForegroundColor DarkGray

function Write-Step {
  param([string]$Text)
  Write-Host ""
  Write-Host "==> $Text" -ForegroundColor Cyan
}

function Update-SessionPath {
  $machinePath = [System.Environment]::GetEnvironmentVariable("Path", "Machine")
  $userPath = [System.Environment]::GetEnvironmentVariable("Path", "User")
  $env:Path = "$machinePath;$userPath"
}

# Funktionale Pruefung statt reiner PATH-Pruefung (siehe .DESCRIPTION oben).
function Test-ToolWorks {
  param(
    [string]$Command,
    [string]$Pattern
  )
  try {
    $output = & $Command --version 2>&1 | Out-String
  } catch {
    return $false
  }
  if ($LASTEXITCODE -ne 0) { return $false }
  return ($output -match $Pattern)
}

function Install-ToolIfMissing {
  param(
    [string]$Name,
    [string]$Command,
    [string]$Pattern,
    [scriptblock]$InstallAction
  )
  Write-Step "Pruefe $Name..."
  if (Test-ToolWorks -Command $Command -Pattern $Pattern) {
    Write-Host "OK: $Name ist bereits vorhanden und funktionsfaehig."
    return
  }

  Write-Host "$Name nicht gefunden oder nicht funktionsfaehig - installiere..."
  & $InstallAction
  Update-SessionPath

  if (-not (Test-ToolWorks -Command $Command -Pattern $Pattern)) {
    throw "$Name ist nach der Installation weiterhin nicht nutzbar. Bitte Terminal/PowerShell neu starten und Skript erneut ausfuehren (PATH-Aenderungen greifen sonst erst in einer neuen Sitzung)."
  }
  Write-Host "OK: $Name einsatzbereit."
}

# ------------------------------------------------------------------
Install-ToolIfMissing -Name "Python" -Command "python" -Pattern "^Python \d" -InstallAction {
  winget install --id Python.Python.3.12 -e --source winget --accept-package-agreements --accept-source-agreements
}

# ------------------------------------------------------------------
Install-ToolIfMissing -Name "Git" -Command "git" -Pattern "^git version" -InstallAction {
  winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
}

# ------------------------------------------------------------------
Install-ToolIfMissing -Name "PlatformIO" -Command "pio" -Pattern "PlatformIO Core" -InstallAction {
  python -m pip install --upgrade pip
  python -m pip install --upgrade platformio
}

# ------------------------------------------------------------------
Write-Step "Pruefe Projekt-Checkout..."

if (-not $RepoPath) {
  $candidateRoot = Split-Path -Parent $PSScriptRoot
  if (Test-Path (Join-Path $candidateRoot "firmware\platformio.ini")) {
    # Normalfall: dieses Skript liegt in scripts/ innerhalb eines
    # vollstaendigen Checkouts (git clone) - dessen Root verwenden.
    $RepoPath = $candidateRoot
  } else {
    # Nur diese eine Datei wurde heruntergeladen (kein voller Checkout
    # daneben) - Default-Zielordner neben dem Skript.
    $RepoPath = Join-Path $PSScriptRoot "ESP-Anwesenheit"
  }
}

$firmwarePath = Join-Path $RepoPath "firmware"

if (Test-Path (Join-Path $firmwarePath "platformio.ini")) {
  Write-Host "Checkout bereits vorhanden unter $RepoPath"
  $status = git -C $RepoPath status --porcelain
  if ([string]::IsNullOrWhiteSpace($status)) {
    Write-Host "Keine lokalen Aenderungen - hole neueste Version (git pull)..."
    git -C $RepoPath pull
  } else {
    Write-Host "Lokale Aenderungen im Checkout gefunden - ueberspringe 'git pull', um nichts zu ueberschreiben." -ForegroundColor Yellow
  }
} else {
  Write-Step "Kein Checkout gefunden - klone Repository nach $RepoPath ..."
  git clone $RepoUrl $RepoPath
  if ($LASTEXITCODE -ne 0) {
    throw "git clone fehlgeschlagen (Exitcode $LASTEXITCODE)"
  }
}

if (-not (Test-Path (Join-Path $firmwarePath "platformio.ini"))) {
  throw "firmware/platformio.ini wurde auch nach dem Checkout nicht gefunden - stimmt -RepoPath ($RepoPath)?"
}

# ------------------------------------------------------------------
Write-Step "Pruefe config.h..."
$configExample = Join-Path $firmwarePath "include\config.h.example"
$configReal = Join-Path $firmwarePath "include\config.h"

if (-not (Test-Path $configReal)) {
  Copy-Item $configExample $configReal
  Write-Host "include/config.h aus der Vorlage angelegt."
} else {
  Write-Host "config.h existiert bereits - wird nicht ueberschrieben."
}

# ------------------------------------------------------------------
Set-Location $firmwarePath

Write-Step "Baue Firmware (pio run)..."
pio run
if ($LASTEXITCODE -ne 0) {
  throw "Build fehlgeschlagen (pio-Exitcode $LASTEXITCODE)"
}

if ($SkipUpload) {
  Write-Host ""
  Write-Host "SkipUpload gesetzt - Build erfolgreich, kein Flash-Vorgang durchgefuehrt." -ForegroundColor Green
  exit 0
}

# ------------------------------------------------------------------
Write-Step "Verfuegbare serielle Ports:"
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
if ($ports.Count -eq 0) {
  Write-Host "  (keine gefunden - ist der USB-Adapter angeschlossen?)" -ForegroundColor Yellow
} else {
  $ports | ForEach-Object { Write-Host "  - $_" }
}

Write-Host ""
Write-Host "Hinweis: Board muss am USB-Seriell-Adapter angeschlossen sein und sich im" -ForegroundColor Yellow
Write-Host "Boot-/Download-Modus befinden - siehe docs/flash-anleitung.txt (Pinbelegung" -ForegroundColor Yellow
Write-Host "und manueller Boot-Modus, falls der Adapter kein DTR/RTS hat)." -ForegroundColor Yellow

Write-Step "Flashe Firmware (pio run --target upload)..."
if ($Port) {
  pio run --target upload --upload-port $Port
} else {
  pio run --target upload
}

if ($LASTEXITCODE -ne 0) {
  Write-Host ""
  Write-Host "Upload fehlgeschlagen. Haeufige Ursachen:" -ForegroundColor Red
  Write-Host "  - Falscher COM-Port (siehe Liste oben, ggf. mit -Port COM<n> erneut versuchen)"
  Write-Host "  - CH340-/CP2102-USB-Treiber fehlt (Geraete-Manager pruefen)"
  Write-Host "  - Board nicht angeschlossen oder nicht im Bootloader-/Download-Modus (siehe docs/flash-anleitung.txt)"
  throw "pio run --target upload fehlgeschlagen (Exitcode $LASTEXITCODE)"
}

Write-Host ""
Write-Host "Fertig! Firmware erfolgreich geflasht." -ForegroundColor Green
Write-Host "Seriellen Monitor ansehen: pio device monitor (115200 Baud)."
Write-Host "Beim ersten Start: Netzwerk (LAN/WLAN) und ggf. SNMP-Community ueber die Weboberflaeche einrichten."
