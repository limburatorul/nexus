; Nexus installer: one setup for Windows XP SP3 to 11. Build with build.bat (passes /DVERSION).
Unicode true
!include "MUI2.nsh"
!include "WinVer.nsh"

Name "Nexus"
OutFile "..\build\Nexus-Setup-${VERSION}.exe"
InstallDir "$PROGRAMFILES\Nexus"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "Nexus ${VERSION}"

!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nexus"
!define MUI_ICON "..\res\nexus.ico"
!define MUI_UNICON "..\res\nexus.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\nexus.exe"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; A running Nexus holds the exe open; ask it to quit (WM_APP + 2) before replacing or removing it.
!macro CloseNexus
  FindWindow $0 "NexusWnd"
  StrCmp $0 0 +3
  SendMessage $0 0x8002 0 0
  Sleep 1000
!macroend

Section "Nexus"
  !insertmacro CloseNexus
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File "..\build\nexus.exe"
  File "..\LICENSE"
  File "..\res\fonts\HankenGrotesk-OFL.txt"
  File "..\res\fonts\B612Mono-OFL.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortCut "$SMPROGRAMS\Nexus.lnk" "$INSTDIR\nexus.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Nexus" '"$INSTDIR\nexus.exe" /tray'

  ; The controlled PC listens on port 24800: let Nexus through the firewall without a prompt.
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Nexus'
    nsExec::Exec 'netsh advfirewall firewall add rule name=Nexus dir=in action=allow program="$INSTDIR\nexus.exe" enable=yes'
  ${Else}
    nsExec::Exec 'netsh firewall add allowedprogram program="$INSTDIR\nexus.exe" name=Nexus mode=ENABLE'
  ${EndIf}

  ; The controlled PC listens on port 24800: let Nexus through the firewall without a prompt.
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Nexus'
    nsExec::Exec 'netsh advfirewall firewall add rule name=Nexus dir=in action=allow program="$INSTDIR\nexus.exe" enable=yes'
  ${Else}
    nsExec::Exec 'netsh firewall add allowedprogram program="$INSTDIR\nexus.exe" name=Nexus mode=ENABLE'
  ${EndIf}

  WriteRegStr HKLM "${UNINST}" "DisplayName" "Nexus"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "Nexus"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\nexus.exe"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

; An update runs this installer with /S from Nexus, which it closed: start Nexus again.
Function .onInstSuccess
  IfSilent 0 +2
  Exec '"$INSTDIR\nexus.exe" /tray'
FunctionEnd

Section "Uninstall"
  !insertmacro CloseNexus
  SetShellVarContext all
  Delete "$SMPROGRAMS\Nexus.lnk"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Nexus"
  ${If} ${AtLeastWinVista}
    nsExec::Exec 'netsh advfirewall firewall delete rule name=Nexus'
  ${Else}
    nsExec::Exec 'netsh firewall delete allowedprogram program="$INSTDIR\nexus.exe"'
  ${EndIf}
  DeleteRegKey HKLM "${UNINST}"
  Delete "$INSTDIR\nexus.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\HankenGrotesk-OFL.txt"
  Delete "$INSTDIR\B612Mono-OFL.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
