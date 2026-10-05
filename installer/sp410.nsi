; sp410.nsi - NSIS installer for the SP410 Open Driver for Windows.
; SPDX-License-Identifier: Apache-2.0
; Copyright 2026 The sp410-opensource-windows contributors.
;
; Built with NSIS (open source, zlib licence):
;   makensis -DVERSION=1.0.0 -DBINDIR=..\build\windows-x86_64 -DOUTFILE=..\build\setup.exe installer\sp410.nsi
; (`make installer` passes these for you.)
;
; Installs:
;   %ProgramFiles%\SP410 Open Driver\   sp410-ippd.exe, sp410-cli.exe, PowerShell helpers, docs
;   %ProgramData%\SP410\sp410.ini       settings (kept on upgrade)
;   Service "SP410IPP"                  runs sp410-ippd.exe --service, starts automatically
;   Printer "iDPRT SP410"               Windows' in-box Microsoft IPP Class Driver -> 127.0.0.1:8631
;
; Silent install: setup.exe /S        Silent uninstall: uninstall.exe /S

Unicode true
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef BINDIR
  !define BINDIR "..\build\windows-x86_64"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\build\sp410-opensource-windows-${VERSION}-setup.exe"
!endif

!define APPNAME   "SP410 Open Driver"
!define SVCNAME   "SP410IPP"
!define PORT      "8631"
!define PRINTER   "iDPRT SP410"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SP410OpenDriver"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "${UNINSTKEY}" "InstallLocation"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
BrandingText "${APPNAME} ${VERSION} - open source (Apache-2.0)"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} setup"
VIAddVersionKey "CompanyName"     "sp410-opensource-windows contributors"
VIAddVersionKey "LegalCopyright"  "Apache License 2.0"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs an open-source driver for the iDPRT SP410 label printer.$\r$\n$\r$\nIt adds a small background service and a printer named '${PRINTER}' that uses the IPP Class Driver built into Windows. No third-party kernel or print driver is installed.$\r$\n$\r$\nConnect the printer by USB and switch it on before you continue."
!define MUI_FINISHPAGE_LINK "Open the printer status page"
!define MUI_FINISHPAGE_LINK_LOCATION "http://127.0.0.1:${PORT}/"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------

!macro StopService
  DetailPrint "Stopping the ${APPNAME} service (if running)..."
  nsExec::ExecToLog 'sc.exe stop ${SVCNAME}'
  Pop $0
  ; wait up to 15 s for the process to exit so its files can be replaced
  StrCpy $1 0
  ${Do}
    nsExec::ExecToStack 'sc.exe query ${SVCNAME}'
    Pop $0
    Pop $2
    ${If} $0 != 0
      ${Break}
    ${EndIf}
    Push $2
    Push "STOPPED"
    Call StrContains
    Pop $3
    ${If} $3 == "1"
      ${Break}
    ${EndIf}
    IntOp $1 $1 + 1
    ${If} $1 > 30
      ${Break}
    ${EndIf}
    Sleep 500
  ${Loop}
!macroend

; Push haystack, push needle, Call -> pops "1" if found else "0"
!macro StrContainsFn PREFIX
Function ${PREFIX}StrContains
  Exch $R0            ; needle
  Exch
  Exch $R1            ; haystack
  Push $R2
  Push $R3
  Push $R4
  Push $R5
  StrLen $R2 $R0
  StrCpy $R3 0
  StrCpy $R4 "0"
  ${Do}
    StrCpy $R5 $R1 $R2 $R3
    ${If} $R5 == ""
      ${Break}
    ${EndIf}
    ${If} $R5 == $R0
      StrCpy $R4 "1"
      ${Break}
    ${EndIf}
    IntOp $R3 $R3 + 1
  ${Loop}
  StrCpy $R0 $R4
  Pop $R5
  Pop $R4
  Pop $R3
  Pop $R2
  Pop $R1
  Exch $R0
FunctionEnd
!macroend
!insertmacro StrContainsFn ""
!insertmacro StrContainsFn "un."

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} needs 64-bit Windows 10 or 11." /SD IDOK
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "${APPNAME} needs Windows 10 or 11 (for the in-box IPP Class Driver)." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; ---------------------------------------------------------------------------

Section "Driver service (required)" SecCore
  SectionIn RO
  SetShellVarContext all

  !insertmacro StopService

  SetOutPath "$INSTDIR"
  File "${BINDIR}\sp410-ippd.exe"
  File "${BINDIR}\sp410-cli.exe"
  File "add-printer.ps1"
  File "remove-printer.ps1"
  File /oname=README.txt "..\README.md"
  File /oname=LICENSE.txt "..\LICENSE"

  ; Settings live in %ProgramData%\SP410 and survive upgrades.
  CreateDirectory "$APPDATA\SP410"
  ${IfNot} ${FileExists} "$APPDATA\SP410\sp410.ini"
    SetOutPath "$APPDATA\SP410"
    File "sp410.ini"
  ${EndIf}
  SetOutPath "$INSTDIR"

  DetailPrint "Registering the ${APPNAME} service..."
  nsExec::ExecToLog 'sc.exe query ${SVCNAME}'
  Pop $0
  ${If} $0 == 0
    nsExec::ExecToLog 'sc.exe config ${SVCNAME} binPath= "\"$INSTDIR\sp410-ippd.exe\" --service" start= auto'
  ${Else}
    nsExec::ExecToLog 'sc.exe create ${SVCNAME} binPath= "\"$INSTDIR\sp410-ippd.exe\" --service" start= auto DisplayName= "${APPNAME}"'
  ${EndIf}
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "Could not register the ${APPNAME} service (sc.exe returned $0)." /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog 'sc.exe description ${SVCNAME} "Lets Windows print to the iDPRT SP410 label printer (IPP Everywhere to TSPL)."'
  Pop $0
  nsExec::ExecToLog 'sc.exe failure ${SVCNAME} reset= 86400 actions= restart/5000/restart/5000/restart/30000'
  Pop $0
  nsExec::ExecToLog 'sc.exe start ${SVCNAME}'
  Pop $0

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher"       "sp410-opensource-windows contributors"
  WriteRegStr HKLM "${UNINSTKEY}" "URLInfoAbout"    "https://github.com/rightiswrong/sp410-opensource-windows"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\sp410-ippd.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" 1200
SectionEnd

Section "Add the '${PRINTER}' printer to Windows" SecPrinter
  DetailPrint "Adding the printer (Microsoft IPP Class Driver -> 127.0.0.1:${PORT})..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\add-printer.ps1" -Port ${PORT} -Name "${PRINTER}"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "The service is installed, but Windows did not add the printer automatically.$\r$\n$\r$\nAdd it in Settings > Printers & scanners > Add device > Add manually > 'Add a printer using an IP address or hostname' > IPP Device, address:$\r$\n$\r$\nhttp://127.0.0.1:${PORT}/ipp/print" /SD IDOK
  ${EndIf}
SectionEnd

Section "Start menu shortcuts" SecShortcuts
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  WriteINIStr "$SMPROGRAMS\${APPNAME}\Printer status.url" "InternetShortcut" "URL" "http://127.0.0.1:${PORT}/"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Edit printer settings.lnk" "$WINDIR\notepad.exe" '"$APPDATA\SP410\sp410.ini"'
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Print a test label.lnk" "$INSTDIR\sp410-cli.exe" "test-label"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Calibrate label size.lnk" "$INSTDIR\sp410-cli.exe" "calibrate"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\README.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "The background service that converts print jobs for the SP410 (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPrinter} "Create the '${PRINTER}' printer using Windows' built-in IPP Class Driver."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Start menu entries for status, settings, test label and calibration."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------

Section "Uninstall"
  SetShellVarContext all

  DetailPrint "Removing the printer..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\remove-printer.ps1" -Port ${PORT} -Name "${PRINTER}"'
  Pop $0

  DetailPrint "Stopping the ${APPNAME} service..."
  nsExec::ExecToLog 'sc.exe stop ${SVCNAME}'
  Pop $0
  StrCpy $1 0
  ${Do}
    nsExec::ExecToStack 'sc.exe query ${SVCNAME}'
    Pop $0
    Pop $2
    ${If} $0 != 0
      ${Break}
    ${EndIf}
    Push $2
    Push "STOPPED"
    Call un.StrContains
    Pop $3
    ${If} $3 == "1"
    ${OrIf} $1 > 30
      ${Break}
    ${EndIf}
    IntOp $1 $1 + 1
    Sleep 500
  ${Loop}
  nsExec::ExecToLog 'sc.exe delete ${SVCNAME}'
  Pop $0

  Delete "$INSTDIR\sp410-ippd.exe"
  Delete "$INSTDIR\sp410-cli.exe"
  Delete "$INSTDIR\add-printer.ps1"
  Delete "$INSTDIR\remove-printer.ps1"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  RMDir /r "$SMPROGRAMS\${APPNAME}"

  MessageBox MB_YESNO "Keep your printer settings (sp410.ini) for a later reinstall?" /SD IDYES IDYES keep
    Delete "$APPDATA\SP410\sp410.ini"
  keep:
  Delete "$APPDATA\SP410\sp410-ippd.log"
  RMDir "$APPDATA\SP410"

  DeleteRegKey HKLM "${UNINSTKEY}"
SectionEnd
