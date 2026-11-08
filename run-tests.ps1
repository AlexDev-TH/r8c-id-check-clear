$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'A Visual Studio C++ x64 toolchain was not found.' }
$vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
$setupCommand = 'call "' + $vcvars + '" >nul && set'
$environmentLines = & $env:ComSpec /d /s /c $setupCommand
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio build environment could not be initialized.' }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $variableName = $line.Substring(0, $separator)
        if ($variableName -in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
            [Environment]::SetEnvironmentVariable($variableName, $line.Substring($separator + 1), 'Process')
        }
    }
}
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/MT', '/utf-8', '/permissive-', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00')
Push-Location -LiteralPath $projectRoot
try {
    & cl.exe @common /Febuild/protocol_tests.exe tests/protocol_tests.cpp src/protocol.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Protocol test build failed.' }
    & ./build/protocol_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Protocol tests failed.' }
    & cl.exe @common /Febuild/session_tests.exe tests/session_tests.cpp src/serial_job.cpp src/protocol.cpp /link comctl32.lib shell32.lib comdlg32.lib advapi32.lib user32.lib gdi32.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Session test build failed.' }
    & ./build/session_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Session tests failed.' }
    Write-Host 'All tests passed.'
}
finally {
    Pop-Location
}
