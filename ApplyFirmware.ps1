[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [string]$Image = (Join-Path $PSScriptRoot 'firmware/build/ledpanel_firmware.bin'),
    [string]$Python = 'python',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$imagePath = (Resolve-Path -LiteralPath $Image).Path
$argsToFlash = @(
    '--chip', 'esp32s3',
    '-p', $Port,
    '-b', '460800',
    '--before', 'default_reset',
    '--after', 'hard_reset',
    'write_flash', '0x20000', $imagePath
)

if ($DryRun) {
    Write-Output "Image: $imagePath"
    Write-Output "Target: $Port"
    Write-Output 'Operation: write application partition at 0x20000 only; keep saved settings.'
    exit 0
}

& $Python -m esptool @argsToFlash

if ($LASTEXITCODE -ne 0) {
    throw "Firmware write failed with exit code $LASTEXITCODE. Check the USB port and try again."
}
Write-Output 'Firmware written and verified. Saved settings were left intact.'
