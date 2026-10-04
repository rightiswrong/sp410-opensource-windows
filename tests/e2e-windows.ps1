# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-opensource-windows contributors.
#
# e2e-windows.ps1 - end-to-end check on a real Windows machine (used by CI).
#
#   1. silent install            -> service "SP410IPP" running
#   2. printer "iDPRT SP410"     -> created on the in-box Microsoft IPP Class Driver
#   3. print a page from Windows -> Windows renders PWG raster, the service converts
#                                   it to TSPL; output is captured to a file
#   4. silent uninstall          -> service and printer gone
#
#   powershell -ExecutionPolicy Bypass -File tests\e2e-windows.ps1 -Setup build\...-setup.exe
#
# Leaves a TSPL capture in $OutDir for tools\tspl_decode.py.

param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$OutDir = "C:\sp410-e2e",
    [string]$Printer = "iDPRT SP410"
)

$ErrorActionPreference = "Stop"
$failures = @()

function Step($msg) { Write-Host "`n=== $msg" }
function Fail($msg) {
    Write-Host "FAIL: $msg"
    $script:failures += $msg
}

function Dump-Diagnostics {
    Write-Host "`n--- diagnostics"
    Get-Service SP410IPP -ErrorAction SilentlyContinue | Format-List Name, Status, StartType | Out-String | Write-Host
    Get-Printer -ErrorAction SilentlyContinue | Format-Table Name, DriverName, PortName -AutoSize | Out-String -Width 200 | Write-Host
    Get-PrinterDriver -ErrorAction SilentlyContinue | Format-Table Name -AutoSize | Out-String | Write-Host
    $log = "$env:ProgramData\SP410\sp410-ippd.log"
    if (Test-Path $log) { Write-Host "--- sp410-ippd.log (tail)"; Get-Content $log -Tail 60 | Write-Host }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Step "Silent install"
$p = Start-Process -FilePath $Setup -ArgumentList "/S" -Wait -PassThru
if ($p.ExitCode -ne 0) { Fail "installer exit code $($p.ExitCode)" }

$svc = $null
for ($i = 0; $i -lt 30 -and -not ($svc -and $svc.Status -eq "Running"); $i++) {
    Start-Sleep -Seconds 1
    $svc = Get-Service SP410IPP -ErrorAction SilentlyContinue
}
if (-not $svc -or $svc.Status -ne "Running") { Fail "service SP410IPP is not running" }
else { Write-Host "service running" }

try {
    $page = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:8631/" -TimeoutSec 10
    if ($page.Content -notmatch "iDPRT SP410") { Fail "status page content unexpected" }
    else { Write-Host "status page OK" }
} catch { Fail "status page not reachable: $($_.Exception.Message)" }

Step "Printer queue"
$q = Get-Printer -Name $Printer -ErrorAction SilentlyContinue
if (-not $q) {
    Fail "printer '$Printer' was not created by the installer"
} else {
    Write-Host "printer: $($q.Name)  driver: $($q.DriverName)  port: $($q.PortName)"
}

if ($q) {
    Step "Print from Windows through the IPP Class Driver"
    # Send output to a file instead of USB (the CI machine has no printer).
    # [device] is re-read for every job, so no service restart is needed.
    $ini = "$env:ProgramData\SP410\sp410.ini"
    $text = Get-Content $ini -Raw
    $text = $text -replace "(?m)^uri = usb\s*$", "uri = file:$OutDir\job-{job}.prn"
    Set-Content -Path $ini -Value $text -Encoding ASCII

    "SP410 end-to-end test from Windows CI`r`nLine two of the label" | Out-Printer -Name $q.Name

    $file = $null
    for ($i = 0; $i -lt 120 -and -not $file; $i++) {
        Start-Sleep -Seconds 1
        $file = Get-ChildItem -Path $OutDir -Filter "job-*.prn" -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $file) {
        Fail "no TSPL output after printing from Windows"
        Get-PrintJob -PrinterName $q.Name -ErrorAction SilentlyContinue | Format-Table | Out-String | Write-Host
    } else {
        Write-Host "captured $($file.FullName) ($($file.Length) bytes)"
    }
}

Dump-Diagnostics

Step "Silent uninstall"
$un = Join-Path ${env:ProgramFiles} "SP410 Open Driver\uninstall.exe"
if (Test-Path $un) {
    Start-Process -FilePath $un -ArgumentList "/S" -Wait | Out-Null
    for ($i = 0; $i -lt 60 -and (Get-Service SP410IPP -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
    if (Get-Service SP410IPP -ErrorAction SilentlyContinue) { Fail "service still present after uninstall" }
    if (Get-Printer -Name $Printer -ErrorAction SilentlyContinue) { Fail "printer still present after uninstall" }
    if (Test-Path (Join-Path ${env:ProgramFiles} "SP410 Open Driver\sp410-ippd.exe")) { Fail "program files left behind" }
    if (-not $failures) { Write-Host "uninstalled cleanly" }
} else {
    Fail "uninstaller not found at $un"
}

if ($failures) {
    Write-Host "`n$($failures.Count) check(s) failed:"
    $failures | ForEach-Object { Write-Host " - $_" }
    exit 1
}
Write-Host "`nall end-to-end checks passed"
exit 0
