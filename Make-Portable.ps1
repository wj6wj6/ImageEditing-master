[CmdletBinding()]
param(
    [string]$Generator = 'Visual Studio 16 2019'
)

$ErrorActionPreference = 'Stop'
$projectDirectory = $PSScriptRoot
$buildDirectory = Join-Path $projectDirectory 'build-portable'
$cmake = (Get-Command cmake -ErrorAction Stop).Source

& $cmake -S $projectDirectory -B $buildDirectory -G $Generator -A x64 -DIMAGEEDITING_BUILD_PORTABLE=ON
if ($LASTEXITCODE -ne 0) {
    throw 'Portable configuration failed.'
}

& $cmake --build $buildDirectory --config Release --target package --parallel
if ($LASTEXITCODE -ne 0) {
    throw 'Portable build or packaging failed.'
}

$archive = Join-Path $projectDirectory 'dist\P1-Demo-Windows-x64.zip'
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "The package was not created: $archive"
}
Write-Host "Portable package: $archive"
