# SDrive Cluster o'rnatish skripti (Windows, PowerShell)
# Foydalanish: powershell -ExecutionPolicy Bypass -File install_windows.ps1 -ServerUrl http://server:8080
param([string]$ServerUrl = "", [string]$ExePath = "")

$ErrorActionPreference = "Stop"

if (-not $ExePath) {
    $ExePath = Join-Path $PSScriptRoot "sdrive-windows.exe"
    if (-not (Test-Path $ExePath)) { $ExePath = Join-Path $PSScriptRoot "..\dist\sdrive-windows.exe" }
}
if (-not (Test-Path $ExePath)) { throw "sdrive-windows.exe topilmadi: $ExePath" }

# 1. Dastur papkasiga nusxalash (oldingi nusxa ishlayotgan bo'lsa to'xtatiladi)
$installDir = Join-Path $env:LOCALAPPDATA "SDriveCluster"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Get-Process sdrive -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$installDir*" } | Stop-Process -Force
$target = Join-Path $installDir "sdrive.exe"
Copy-Item $ExePath $target -Force
Write-Host "[OK] O'rnatildi: $target"

# 2. Ishga tushirish. Avtostart (HKCU\...\Run) shartnoma qabul qilingandan keyin
#    dasturning o'zi tomonidan yoziladi (shartnoma 9.3: roziliksiz fon ishi boshlanmaydi).
$procArgs = @()
if ($ServerUrl) { $procArgs += @("--server", $ServerUrl) }
if ($procArgs.Count -gt 0) { Start-Process $target -ArgumentList $procArgs } else { Start-Process $target }
Write-Host "[OK] Dastur ishga tushirildi — brauzerda sozlash oynasi ochiladi: http://127.0.0.1:48673"
