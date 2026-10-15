; Bridger installer: one setup for Windows XP SP3 to 11. Build with build.bat (passes /DVERSION).
Unicode true
!include "MUI2.nsh"
!include "WinVer.nsh"

Name "Bridger"
OutFile "..\build\Bridger-Setup-${VERSION}.exe"
InstallDir "$PROGRAMFILES\Bridger"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "Bridger ${VERSION}"

!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Bridger"
!define MUI_ICON "..\res\bridger.ico"
!define MUI_UNICON "..\res\bridger.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bridger.exe"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; A running Bridger holds the exe open; ask it to quit (WM_APP + 2) before replacing or removing it.
!macro CloseBridger
  FindWindow $0 "BridgerWnd"
  StrCmp $0 0 +3
  SendMessage $0 0x8002 0 0
  Sleep 1000
!macroend

Section "Bridger"
  !insertmacro CloseBridger
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File "..\build\bridger.exe"
  File "..\LICENSE"
  File "..\res\fonts\HankenGrotesk-OFL.txt"
  File "..\res\fonts\B612Mono-OFL.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortCut "$SMPROGRAMS\Bridger.lnk" "$INSTDIR\bridger.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Bridger" '"$INSTDIR\bridger.exe" /tray'

  ; The controlled PC listens on port 24800: let Bridger through the firewall without a prompt.
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Bridger'
    nsExec::Exec 'netsh advfirewall firewall add rule name=Bridger dir=in action=allow program="$INSTDIR\bridger.exe" enable=yes'
  ${Else}
    nsExec::Exec 'netsh firewall add allowedprogram program="$INSTDIR\bridger.exe" name=Bridger mode=ENABLE'
  ${EndIf}

  ; The controlled PC listens on port 24800: let Bridger through the firewall without a prompt.
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Bridger'
    nsExec::Exec 'netsh advfirewall firewall add rule name=Bridger dir=in action=allow program="$INSTDIR\bridger.exe" enable=yes'
  ${Else}
    nsExec::Exec 'netsh firewall add allowedprogram program="$INSTDIR\bridger.exe" name=Bridger mode=ENABLE'
  ${EndIf}

  WriteRegStr HKLM "${UNINST}" "DisplayName" "Bridger"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "Bridger"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\bridger.exe"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  !insertmacro CloseBridger
  SetShellVarContext all
  Delete "$SMPROGRAMS\Bridger.lnk"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Bridger"
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Bridger'
  ${Else}
    nsExec::Exec 'netsh firewall delete allowedprogram program="$INSTDIR\bridger.exe"'
  ${EndIf}
  DeleteRegKey HKLM "${UNINST}"
  Delete "$INSTDIR\bridger.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\HankenGrotesk-OFL.txt"
  Delete "$INSTDIR\B612Mono-OFL.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
