# Builds CombatGroupDeadlockFix and packages it as an installable mod:
#   dist/CombatGroupDeadlockFix-1.0.0/SKSE/Plugins/CombatGroupDeadlockFix.dll (+ readme)
#   dist/CombatGroupDeadlockFix-1.0.0.zip
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

xmake build -P . -y CombatGroupDeadlockFix
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$dll = Join-Path $root 'build\windows\x64\release\CombatGroupDeadlockFix.dll'
$stage = Join-Path $root 'dist\CombatGroupDeadlockFix-1.0.0'
$plugins = Join-Path $stage 'SKSE\Plugins'
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item -Force $dll $plugins
Copy-Item -Force (Join-Path $root 'release\CombatGroupDeadlockFix-readme.txt') $plugins

$zip = Join-Path $root 'dist\CombatGroupDeadlockFix-1.0.0.zip'
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $stage .
if ($LASTEXITCODE -ne 0) { throw 'zip failed' }
Write-Host "Packaged: $zip"
