# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
#
# remove-printer.ps1 - remove the Windows printer(s) that point at the SP410
# service. Leaves every other printer alone.
#
#   powershell -ExecutionPolicy Bypass -File remove-printer.ps1 [-Port 8631] [-Name "iDPRT SP410"]

param(
    [int]$Port = 8631,
    [string]$Name = "iDPRT SP410"
)

$ErrorActionPreference = "Continue"

$ours = Get-Printer -ErrorAction SilentlyContinue | Where-Object {
    $_.PortName -like "*127.0.0.1:$Port*" -or
    $_.PortName -like "*localhost:$Port*" -or
    ($_.Name -eq $Name -and $_.DriverName -like "*IPP*")
}

foreach ($p in $ours) {
    try {
        Remove-Printer -Name $p.Name -ErrorAction Stop
        Write-Host "Removed printer '$($p.Name)'"
    } catch {
        Write-Host "Could not remove '$($p.Name)': $($_.Exception.Message)"
    }
}
if (-not $ours) { Write-Host "No SP410 printer to remove." }
exit 0
