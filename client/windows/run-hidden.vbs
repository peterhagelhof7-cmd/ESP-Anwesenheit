' ============================================================================
' ESP-Anwesenheit - fensterloser Starter fuer den PowerShell-Agenten.
'
' Warum diese Datei ueberhaupt existiert:
'   Der Scheduled Task hat den Agenten frueher direkt per
'     powershell.exe -WindowStyle Hidden -File AnwesenheitAgent.ps1
'   gestartet. powershell.exe ist eine KONSOLEN-Anwendung - wird sie vom Task
'   in der interaktiven Sitzung gestartet, legt Windows/conhost das
'   Konsolenfenster an, BEVOR PowerShell "-WindowStyle Hidden" anwenden kann.
'   Ergebnis: ein leeres schwarzes Fenster, das die ganze Sitzung offen bleibt
'   (der Agent laeuft in einer Endlosschleife) - schliesst man es, endet das
'   Monitoring. wscript.exe dagegen ist eine GUI-Subsystem-Anwendung und
'   erzeugt gar kein Konsolenfenster; der mit Fensterstil 0 gestartete
'   PowerShell-Prozess bleibt komplett unsichtbar.
'
'   bWaitOnReturn = True: wscript WARTET, bis PowerShell endet. Dadurch bleibt
'   der Scheduled-Task-Prozess (dieses wscript) so lange am Leben, wie der Agent
'   laeuft - der Task gilt als "wird ausgefuehrt" (RestartCount-Selbstheilung
'   greift) und beim Abmelden/Herunterfahren wird der ganze Prozessbaum sauber
'   mitbeendet.
'
' Der Agent liegt im selben Verzeichnis wie dieses Skript (der Installer kopiert
' beide zusammen), damit der Pfad unabhaengig vom Installationsort stimmt.
' ============================================================================
Option Explicit

Dim shell, fso, scriptDir, agentPath, cmd
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
agentPath = fso.BuildPath(scriptDir, "AnwesenheitAgent.ps1")

cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File """ & agentPath & """"

' 0 = Fenster verbergen; True = auf das Ende des Agenten warten (haelt den Task am Leben)
shell.Run cmd, 0, True
