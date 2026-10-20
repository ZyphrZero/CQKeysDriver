$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools were not found.' }
$out = Join-Path $env:TEMP 'cqkeys-native-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
# cmd is only used to initialize the MSVC environment and compile, not for file operations.
$buildScript = Join-Path $out 'build-tests.cmd'
@"
@echo off
call "$devcmd" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /fsanitize=address /Zi "$PSScriptRoot\input_reports_test.cpp" "$root\cqkeys\input_reports.cpp" /Fe:"$out\input_reports_test.exe" /Fo:"$out\\" /Fd:"$out\tests.pdb"
if errorlevel 1 exit /b %errorlevel%
"$out\input_reports_test.exe"
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildScript -Encoding ASCII
& $env:ComSpec /d /c $buildScript
if ($LASTEXITCODE -ne 0) { throw "Native tests failed: $LASTEXITCODE" }
