param(
    [switch]$DebugBuild,
    [string]$OutputName = 'R8C ID Check Clear By AlexDev-TH.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
if ([System.IO.Path]::GetFileName($OutputName) -ne $OutputName -or -not $OutputName.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputName must be an EXE filename inside this project directory.'
}
$outputFile = Join-Path $projectRoot $OutputName
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
$sources = @((Join-Path $projectRoot 'src/main.cpp'), (Join-Path $projectRoot 'src/serial_job.cpp'), (Join-Path $projectRoot 'src/protocol.cpp'))
foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath $source)) { throw "Required source file is missing: $source" }
}
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$previousLocation = Get-Location
try {
    Set-Location -LiteralPath $projectRoot
    $resourceFile = Join-Path $buildDirectory 'app.res'
    & rc.exe /nologo /fo $resourceFile (Join-Path $projectRoot 'app.rc')
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    $compilerArguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/MT', '/utf-8', '/permissive-', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00')
    if ($DebugBuild) { $compilerArguments += @('/Od', '/Zi') } else { $compilerArguments += '/O2' }
    $compilerArguments += @(('/Fo' + $buildDirectory.Replace('\', '/') + '/'), ('/Fe' + $outputFile))
    $compilerArguments += $sources
    $compilerArguments += @($resourceFile, '/link', '/SUBSYSTEM:WINDOWS', '/MANIFEST:NO', 'comctl32.lib', 'shell32.lib', 'comdlg32.lib', 'advapi32.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib')
    & cl.exe @compilerArguments
    if ($LASTEXITCODE -ne 0) { throw 'Native application build failed.' }
    Get-Item -LiteralPath $outputFile | Select-Object FullName, Length
    Get-FileHash -LiteralPath $outputFile -Algorithm SHA256
}
finally {
    Set-Location -LiteralPath $previousLocation.Path
}
