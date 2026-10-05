[CmdletBinding()]
param(
    [string]$Archive = 'dist\P1-Demo-Windows-x64.zip'
)

$ErrorActionPreference = 'Stop'
$projectDirectory = $PSScriptRoot
$portableDirectory = Join-Path $projectDirectory 'portable'
$tempDirectory = Join-Path $projectDirectory 'dist\portable-unzip'

# Relative paths are relative to the project directory.
if (-not [System.IO.Path]::IsPathRooted($Archive)) {
    $Archive = Join-Path $projectDirectory $Archive
}
if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
    throw "The ZIP was not found: $Archive (run Make-Portable.ps1 first)"
}

# 1. Unzip into a temporary folder.
if (Test-Path -LiteralPath $tempDirectory) {
    Remove-Item -LiteralPath $tempDirectory -Recurse -Force
}
Expand-Archive -LiteralPath $Archive -DestinationPath $tempDirectory

# 2. The ZIP has one top-level folder (P1-Demo-Windows-x64); use its contents.
$newPortable = $tempDirectory
$items = @(Get-ChildItem -LiteralPath $tempDirectory)
if ($items.Count -eq 1 -and $items[0].PSIsContainer) {
    $newPortable = $items[0].FullName
}
if (-not (Test-Path -LiteralPath (Join-Path $newPortable 'ImageEditing.exe') -PathType Leaf)) {
    throw "ImageEditing.exe is missing from the ZIP; portable was not changed."
}

# 3. Replace the old portable folder with the unzipped one.
if (Test-Path -LiteralPath $portableDirectory) {
    Remove-Item -LiteralPath $portableDirectory -Recurse -Force
}
Move-Item -LiteralPath $newPortable -Destination $portableDirectory

if (Test-Path -LiteralPath $tempDirectory) {
    Remove-Item -LiteralPath $tempDirectory -Recurse -Force
}
Write-Host "Portable folder updated: $portableDirectory"
