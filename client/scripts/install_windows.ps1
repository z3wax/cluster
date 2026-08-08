# SDrive Cluster o'rnatish skripti (Windows, PowerShell)
# Foydalanish: powershell -ExecutionPolicy Bypass -File install_windows.ps1
param([string]$ServerUrl = "http://127.0.0.1:8080", [string]$ExePath = "")

$ErrorActionPreference = "Stop"

if (-not $ExePath) { $ExePath = "$PSScriptRoot\sdrive.exe" }
if (-not (Test-Path $ExePath)) { throw "sdrive.exe topilmadi: $ExePath" }

# 1. Dastur papkasiga nusxalash
$installDir = "$env:LOCALAPPDATA\SDriveCluster"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item $ExePath "$installDir\sdrive.exe" -Force

# 2. Avtostart (Run key) — kompyuter yoqilishi bilan ishga tushadi
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Set-ItemProperty -Path $runKey -Name "SDriveCluster" -Value "`"$installDir\sdrive.exe`" --background"

# 3. Server URL ni config orqali berish (birinchi ishga tushishda)
Write-Host "[OK] SDrive Cluster o'rnatildi"
Write-Host "Server: $ServerUrl"
Write-Host "Boshqaruv: http://127.0.0.1:48673"

# 4. Ishga tushirish
Start-Process "$installDir\sdrive.exe"
Write-Host "[OK] Dastur ishga tushirildi — brauzerda sozlash oynasi ochiladi"
