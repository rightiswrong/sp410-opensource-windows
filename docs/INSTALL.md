# Installing, upgrading and troubleshooting

## Install

1. Connect the SP410 by USB and switch it on. If Windows already lists an
   "iDPRT SP410" from the vendor driver, remove that printer first
   (Settings → Printers & scanners → the printer → Remove) so two drivers do
   not compete for the USB port.
2. Download the installer from the project's Releases page and run
   `sp410-opensource-windows-<version>-setup.exe`; accept the
   administrator prompt.
3. Keep all three components selected:
   - **Driver service** installs `sp410-ippd.exe` as the "SP410 Open Driver"
     service, set to start automatically.
   - **Add the printer** creates "iDPRT SP410" using the in-box
     *Microsoft IPP Class Driver* pointed at `http://127.0.0.1:8631/ipp/print`.
   - **Start menu shortcuts** adds status page, settings, test label,
     calibration and uninstall entries.
4. After loading labels, run **Start → SP410 Open Driver → Calibrate label size**
   once, then **Print a test label**.

### SmartScreen

The installer is built by CI and is not signed with a paid code-signing
certificate, so Windows SmartScreen may show "Windows protected your PC".
Choose **More info → Run anyway**. To check what you are running, compare
the file's SHA-256 (`Get-FileHash setup.exe`) with `SHA256SUMS.txt` on the
release page, which CI generates in the same run that builds the installer.
You can also build it yourself (README → Build from source).

### Silent / scripted install

```bat
sp410-opensource-windows-1.0.0-setup.exe /S
"C:\Program Files\SP410 Open Driver\uninstall.exe" /S
```

## Upgrade

Run the newer installer. It stops the service, replaces the programs, keeps
`C:\ProgramData\SP410\sp410.ini`, and restarts the service. The printer is
reused.

## Uninstall

Settings → Apps → *SP410 Open Driver* → Uninstall (or the Start menu entry).
This removes the printer, the service and the program files. You are asked
whether to keep your settings file.

## Adding the printer by hand

If the installer reports that Windows did not add the printer, as can happen
on some locked-down or older builds:

1. Settings → Bluetooth & devices → Printers & scanners → **Add device**.
2. Wait for **Add manually** and click it.
3. Choose **Add a printer using an IP address or hostname**.
4. Device type **IPP Device**; hostname or address `http://127.0.0.1:8631/ipp/print`.
5. Finish. The driver shown should be *Microsoft IPP Class Driver*.

From an administrator PowerShell, the same thing:

```powershell
Add-Printer -IppURL http://127.0.0.1:8631/ipp/print
```

## Other connections

Edit `C:\ProgramData\SP410\sp410.ini`, section `[device]`. The change applies
to the next label.

| Printer connected by | `uri =` |
|---|---|
| USB (default) | `usb`, or `usb:any` if Windows reports no vendor ID |
| Bluetooth (SP410BT) | pair it, find its **outgoing** COM port (Bluetooth settings → More Bluetooth options → COM Ports), then `serial:COM5?baud=115200` |
| Network print server / Wi-Fi bridge | `socket://192.168.1.50:9100` |
| An existing Windows queue (fallback) | `spool:Name of that printer`; RAW data is sent through it |

Run `sp410-cli list` to see the USB printers Windows can see, and
`sp410-cli --device <uri> status` to test a connection.

## Sharing with other computers

By default only this PC can print (`listen = 127.0.0.1`). To share, set
`listen = 0.0.0.0` under `[server]`, restart the service, allow TCP 8631 in
Windows Firewall, and on the other computers add an IPP printer at
`http://<this-pc>:8631/ipp/print`. macOS, Linux, iOS and Android can print
to it too, because it is a standard IPP Everywhere printer.

## Troubleshooting

| Symptom | Check |
|---|---|
| Job stays "printing", then fails with "Waiting for printer" | SP410 off, unplugged or claimed by another driver. Run `sp410-cli list`. |
| Job fails "printer not accepting data" | Out of labels, cover open, or paused. Run `sp410-cli status`. |
| Labels skip, or print across the gap | Run *Calibrate label size*; check `MediaTracking` and `GapHeight`. |
| Image shifted or cut at the tear bar | `ShiftX` / `ShiftY` / `TearOffset` in `[print]` (mm). |
| Barcodes fuzzy | `Dither = Threshold`, and print at 100 % scale (no "fit to page"). |
| Gray areas blotchy | `Dither = Atkinson`. |
| Too light or too dark | `Darkness = 0` … `15`. Slower `PrintSpeed` also prints darker. |
| Wrong label size in the dialog | Pick it in the print dialog, or set `[media] default`. |
| "Printer status" page does not open | Check that the *SP410 Open Driver* service is running in `services.msc`; see the log below. |

Log: `C:\ProgramData\SP410\sp410-ippd.log`. For more detail set
`log-level = debug` under `[server]` and restart the service:

```bat
sc stop SP410IPP && sc start SP410IPP
```

To see exactly what is sent to the printer, set `uri = file:C:\temp\job-{job}.prn`,
print, and inspect the file with `python tools\tspl_decode.py C:\temp\job-1.prn --png-dir C:\temp\render`.
