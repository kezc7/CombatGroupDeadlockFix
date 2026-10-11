# Builds CombatGroupDeadlockFix in release, runs the lock test, and packages it as an installable mod:
#   dist/CombatGroupDeadlockFix-<version>/SKSE/Plugins/CombatGroupDeadlockFix.dll (+ readme)
#   dist/CombatGroupDeadlockFix-<version>.zip
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Select-String -Path (Join-Path $root 'xmake.lua') -Pattern "^set_version\('([^']+)'\)").Matches[0].Groups[1].Value
if (-not $version) { throw 'no set_version in xmake.lua' }

# Always release, whatever the tree was last configured for.
xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

$started = Get-Date
xmake build -P . -y CombatGroupDeadlockFix
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
xmake build -P . -y locktest
if ($LASTEXITCODE -ne 0) { throw 'lock test build failed' }
xmake run -P . locktest
if ($LASTEXITCODE -ne 0) { throw 'lock test failed' }

$dll = Join-Path $root 'build\windows\x64\release\CombatGroupDeadlockFix.dll'
$dllInfo = Get-Item $dll
# xmake may skip an up-to-date link; a DLL older than the sources means a stale build.
$newestSource = Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'xmake.lua') -Recurse -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($dllInfo.LastWriteTime -lt $newestSource.LastWriteTime) { throw "stale DLL: $dll is older than $($newestSource.Name)" }
$fileVersion = $dllInfo.VersionInfo.FileVersion
if ($fileVersion -ne "$version.0") { throw "DLL file version $fileVersion doesn't match $version" }

$name = "CombatGroupDeadlockFix-$version"
$stage = Join-Path $root "dist\$name"
$plugins = Join-Path $stage 'SKSE\Plugins'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item -Force $dll $plugins
Copy-Item -Force (Join-Path $root 'release\CombatGroupDeadlockFix-readme.txt') $plugins

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $stage SKSE
if ($LASTEXITCODE -ne 0) { throw 'zip failed' }

$hash = (Get-FileHash -Algorithm SHA256 $dll).Hash
Write-Host "Packaged: $zip"
Write-Host "DLL $fileVersion, SHA-256 $hash (built after $started)"
