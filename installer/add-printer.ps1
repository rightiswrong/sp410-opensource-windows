# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
#
# add-printer.ps1 - add the SP410 to Windows as an IPP printer that uses the
# in-box "Microsoft IPP Class Driver" (no third-party driver is installed).
#
#   powershell -ExecutionPolicy Bypass -File add-printer.ps1 [-Port 8631] [-Name "iDPRT SP410"]
#
# Exit codes: 0 added or already present, 1 Windows refused to add it,
#             2 the SP410 service is not answering.

param(
    [int]$Port = 8631,
    [string]$Name = "iDPRT SP410",
    [int]$TimeoutSec = 45
)

$ErrorActionPreference = "Stop"
$base = "http://127.0.0.1:$Port"
$url  = "$base/ipp/print"

function Find-Sp410Printer {
    Get-Printer -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -eq $Name -or
        $_.PortName -like "*127.0.0.1:$Port*" -or
        $_.PortName -like "*localhost:$Port*"
    } | Select-Object -First 1
}

# 1. Wait for the service to answer (it may still be starting).
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$up = $false
while (-not $up -and (Get-Date) -lt $deadline) {
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$base/" -TimeoutSec 3 | Out-Null
        $up = $true
    } catch {
        Start-Sleep -Seconds 1
    }
}
if (-not $up) {
    Write-Host "The SP410 Open Driver service is not answering on $base."
    Write-Host "Check that the 'SP410 Open Driver' service is running (services.msc)."
    exit 2
}

# 2. Already there?
$existing = Find-Sp410Printer
if ($existing) {
    Write-Host "Printer already installed: $($existing.Name)"
    exit 0
}

$before = @(Get-Printer -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })

# 3. Preferred: directed IPP discovery (Windows 10 21H2+/11, works with
#    Windows Protected Print Mode). Windows picks the IPP Class Driver itself.
$added = $false
try {
    Add-Printer -IppURL $url -ErrorAction Stop
    $added = $true
} catch {
    Write-Host "Add-Printer -IppURL did not work here: $($_.Exception.Message)"
}

# 4. Fallback for builds without -IppURL: name the in-box driver explicitly.
if (-not $added) {
    try {
        Add-Printer -Name $Name -DriverName "Microsoft IPP Class Driver" -PortName $url -ErrorAction Stop
        $added = $true
    } catch {
        Write-Host "Adding with the Microsoft IPP Class Driver failed: $($_.Exception.Message)"
    }
}

# 5. Directed discovery can finish in the background; wait for the queue.
$printer = $null
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $printer -and (Get-Date) -lt $deadline) {
    $printer = Find-Sp410Printer
    if (-not $printer) {
        $printer = Get-Printer -ErrorAction SilentlyContinue |
            Where-Object { $before -notcontains $_.Name -and $_.DriverName -like "*IPP*" } |
            Select-Object -First 1
    }
    if (-not $printer) { Start-Sleep -Seconds 1 }
}

if (-not $printer) {
    Write-Host ""
    Write-Host "Windows did not create the printer automatically. Add it by hand:"
    Write-Host "  Settings > Bluetooth & devices > Printers & scanners > Add device >"
    Write-Host "  'Add manually' > 'Add a printer using an IP address or hostname' >"
    Write-Host "  Device type 'IPP Device' > $url"
    exit 1
}

if ($printer.Name -ne $Name) {
    try {
        Rename-Printer -Name $printer.Name -NewName $Name -ErrorAction Stop
        Write-Host "Added printer '$Name' (was '$($printer.Name)')"
    } catch {
        Write-Host "Added printer '$($printer.Name)' (rename failed: $($_.Exception.Message))"
    }
} else {
    Write-Host "Added printer '$Name'"
}
Write-Host "Driver: $($printer.DriverName)  Port: $($printer.PortName)"
exit 0
