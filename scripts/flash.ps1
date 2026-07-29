<#
.SYNOPSIS
  Installiert alle Abhaengigkeiten und flasht die ESP-Anwesenheit-Firmware
  auf ein leeres WT32-ETH01-Board unter Windows.

.DESCRIPTION
  - Installiert Python + PlatformIO, falls nicht vorhanden (ueber winget/pip)
  - Installiert Git, falls nicht vorhanden (ueber winget)
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
#>

[CmdletBinding()]
param(
  [string]$Port,
  [switch]$SkipUpload
)

$ErrorActionPreference = "Stop"

# Versionierung dieses Skripts (unabhaengig von der Firmware-Version) -
# Muster aus dem sensormeter-Projekt uebernommen, siehe docs/entscheidungen.md.
#
# Changelog:
#   1.0.0 (2026-07-29) - Erste Fassung.
$FlashScriptVersion = "1.0.0"

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
$RepoRoot = Split-Path -Parent $PSScriptRoot
$firmwarePath = Join-Path $RepoRoot "firmware"

if (-not (Test-Path (Join-Path $firmwarePath "platformio.ini"))) {
  throw "firmware/platformio.ini nicht gefunden unter $firmwarePath - liegt dieses Skript noch in scripts/ innerhalb des ESP-Anwesenheit-Checkouts?"
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
