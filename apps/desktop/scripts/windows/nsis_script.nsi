; Set search path
!addincludedir "${__FILEDIR__}"

; Installer initial constants
!define PRODUCT_NAME "Farlink"
!define PRODUCT_VERSION "${VERSION}"
!define PRODUCT_VERSION_NUMERIC "${VERSION_NUMERIC}"
!define PRODUCT_PUBLISHER "Farlink contributors"
!define PRODUCT_WEB_SITE "https://github.com/R0smontis/farlink"
!define APP_NAME "Farlink"
!define UNINSTALL_REG_KEY "Farlink"
!define PRODUCT_SERVICE_NAME "FarlinkService"

; Installer icon path
!define MUI_ICON "${__FILEDIR__}\..\..\resources\windows\farlink.ico"

; Compression settings
SetCompressor /FINAL lzma

; Request admin privileges (needed to write HKLM)
RequestExecutionLevel admin

; ------ MUI Modern UI Definition ------
!include "MUI.nsh"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Add run-after-install option
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp

!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS
; ------ End of MUI Definition ------

; Include LogicLib for process handling
!include "LogicLib.nsh"
!include "FileFunc.nsh"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "farlink-win-x64-${PRODUCT_VERSION}.exe"
VIProductVersion "${PRODUCT_VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "CompanyName" "Farlink contributors"
VIAddVersionKey /LANG=2052 "FileDescription" "Farlink Installer"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (C) Farlink contributors"
VIAddVersionKey /LANG=2052 "OriginalFilename" "farlink-win-x64-${PRODUCT_VERSION}.exe"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"
InstallDir "$PROGRAMFILES\Farlink"
InstallDirRegKey HKCU "Software\${PRODUCT_NAME}" "InstallDir"
ShowInstDetails show

Section "MainSection"
    ; Check if Farlink is running
    StrCpy $1 "Farlink.exe"
    
    nsProcess::_FindProcess "$1"
    Pop $R0
    ${If} $R0 = 0  ;
        MessageBox MB_ICONQUESTION|MB_YESNO "Farlink is running. Do you want to close it and continue the installation?" IDYES closeApp IDNO cancelInstall
    ${Else}
        Goto installApp
    ${EndIf}

closeApp:
    nsProcess::_KillProcess "$1"
    Pop $R0
    Sleep 500
    Goto installApp

cancelInstall:
    SetDetailsPrint both
    MessageBox MB_ICONEXCLAMATION|MB_OK "Installation has been aborted."
    Abort

installApp:
    Call StopInstalledService

    SetOutPath "$INSTDIR"
    ; The signed release determines file versions, not filesystem timestamps.
    SetOverwrite on

    ; Main application executable path
    File /oname=Farlink.exe "..\..\..\..\build\windows\x64\release\farlink.exe"
    ; Bundle service-side binaries required by the Windows service flow
    File "..\..\..\..\build\windows\x64\release\farlink_service.exe"
    File "..\..\..\..\build\windows\x64\release\farlink_session_helper.exe"
    File "..\..\..\..\build\windows\x64\release\farlink_privacy_cursor_helper.exe"
    File "..\..\..\..\build\windows\x64\release\farlink_privacy_window.dll"
    ; Require the Slint runtime so an incomplete build cannot be packaged.
    File "..\..\..\..\build\windows\x64\release\slint_cpp.dll"
    ; Bundle the remaining runtime DLLs from the release output directory
    File /x farlink_privacy_window.dll /x slint_cpp.dll "..\..\..\..\build\windows\x64\release\*.dll"
    ; Amyuni usbmmidd_v2 virtual display driver for headless hosts. xmake
    ; copies apps\desktop\resources\windows\usbmmidd_v2 into the release
    ; output directory after building farlink; its License.txt ships with it.
    File /r "..\..\..\..\build\windows\x64\release\usbmmidd_v2"

    Call RegisterInstalledService

    ; Import the previous user's startup preference after installing service
    ; binaries. The migration CLI also runs during a silent updater install.
    ClearErrors
    ExecWait '"$INSTDIR\Farlink.exe" --autostart-migrate' $0
    ${If} ${Errors}
    ${OrIf} $0 != 0
        DetailPrint "Farlink will retry auto-start migration on the next launch."
        IfSilent +2
        MessageBox MB_ICONEXCLAMATION|MB_OK "Farlink will retry auto-start migration when you next open it."
    ${EndIf}

    ; Write uninstall information
    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayIcon" "$INSTDIR\Farlink.exe"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "NoRepair" 1
    WriteRegStr HKCU "Software\${PRODUCT_NAME}" "InstallDir" "$INSTDIR"
SectionEnd

Section -AdditionalIcons
    ; Desktop shortcut
    CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\Farlink.exe" "" "$INSTDIR\Farlink.exe"

    ; Start menu shortcut
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}.lnk" "$INSTDIR\Farlink.exe" "" "$INSTDIR\Farlink.exe"
SectionEnd

Section "Uninstall"
    ; Check if Farlink is running
    StrCpy $1 "Farlink.exe"
    
    nsProcess::_FindProcess "$1"
    Pop $R0
    ${If} $R0 = 0
        MessageBox MB_ICONQUESTION|MB_YESNO "Farlink is running. Do you want to close it and uninstall?" IDYES closeApp IDNO cancelUninstall
    ${Else}
        Goto uninstallApp
    ${EndIf}

closeApp:
    nsProcess::_KillProcess "$1"
    Pop $R0
    Sleep 500
    Goto uninstallApp

cancelUninstall:
    SetDetailsPrint both
    MessageBox MB_ICONEXCLAMATION|MB_OK "Uninstallation has been aborted."
    Abort

uninstallApp:
    ClearErrors
    ExecWait '"$INSTDIR\Farlink.exe" --autostart-uninstall' $0
    ${If} ${Errors}
    ${OrIf} $0 != 0
        MessageBox MB_ICONEXCLAMATION|MB_OK "Could not remove Farlink auto-start. Remove any remaining Farlink Autostart tasks in Task Scheduler."
    ${EndIf}
    Call un.UnregisterInstalledService
    Call un.RemoveUsbmmiddDriver

    ; Delete main executable and uninstaller
    Delete "$INSTDIR\Farlink.exe"
    Delete "$INSTDIR\farlink_service.exe"
    Delete "$INSTDIR\farlink_session_helper.exe"
    Delete "$INSTDIR\farlink_privacy_cursor_helper.exe"
    Delete "$INSTDIR\farlink_privacy_window.dll"
    Delete "$INSTDIR\uninstall.exe"

    ; Leave the install directory before removing it
    SetOutPath "$TEMP"

    ; Recursively delete installation directory
    RMDir /r "$INSTDIR"

    ; Delete desktop and start menu shortcuts
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    Delete "$SMPROGRAMS\${PRODUCT_NAME}.lnk"

    ; Delete registry uninstall entry
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}"

    ; Delete remembered install dir
    DeleteRegKey HKCU "Software\${PRODUCT_NAME}"

    ; Recursively delete Farlink folder in user AppData
    RMDir /r "$APPDATA\Farlink"
    RMDir /r "$LOCALAPPDATA\Farlink"
SectionEnd

; ------ Functions ------
Function LaunchApp
    Exec "$INSTDIR\Farlink.exe"
FunctionEnd

Function .onInstFailed
    SetErrorLevel 1
FunctionEnd

Function StopInstalledService
    IfFileExists "$INSTDIR\Farlink.exe" 0 stop_with_sc
    IfFileExists "$INSTDIR\farlink_service.exe" 0 stop_with_sc

    DetailPrint "Stopping existing Farlink service"
    ExecWait '"$INSTDIR\Farlink.exe" --service-stop' $0
    ${If} $0 = 0
        Return
    ${EndIf}

stop_with_sc:
    DetailPrint "Stopping existing Farlink service via Service Control Manager"
    ExecWait '"$SYSDIR\sc.exe" stop ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
    ${AndIf} $0 != 1062
        MessageBox MB_ICONSTOP|MB_OK "Failed to stop the existing Farlink service. The installation will be aborted."
        Abort
    ${EndIf}
    Sleep 1500
FunctionEnd

Function RegisterInstalledService
    IfFileExists "$INSTDIR\Farlink.exe" 0 missing_service_binary
    IfFileExists "$INSTDIR\farlink_service.exe" 0 missing_service_binary
    IfFileExists "$INSTDIR\farlink_session_helper.exe" 0 missing_service_binary

    DetailPrint "Registering Farlink service"
    ExecWait '"$INSTDIR\Farlink.exe" --service-install' $0
    ${If} $0 != 0
        MessageBox MB_ICONSTOP|MB_OK "Failed to register the Farlink service. The installation will be aborted."
        Abort
    ${EndIf}

    ; Preserve an existing unattended opt-in and resume it after upgrades.
    ; A fresh install still registers the ordinary on-demand lock-screen service.
    SetRegView 64
    ReadRegDWORD $1 HKLM "Software\Farlink\Unattended" "Enabled"
    SetRegView 32
    ${If} $1 = 1
        ExecWait '"$INSTDIR\Farlink.exe" --service-start' $0
        ${If} $0 != 0
            MessageBox MB_ICONEXCLAMATION|MB_OK "Could not restart the unattended host. Run Farlink.exe --service-start from an administrator terminal."
        ${EndIf}
    ${EndIf}
    DetailPrint "Farlink service registered"

    Return

missing_service_binary:
    MessageBox MB_ICONSTOP|MB_OK "Farlink service files are missing from the installer package. The installation will be aborted."
    Abort
FunctionEnd

Function un.UnregisterInstalledService
    ; Also disarm unattended startup when removal must fall back to sc.exe.
    SetRegView 64
    DeleteRegKey HKLM "Software\Farlink\Unattended"
    SetRegView 32
    IfFileExists "$INSTDIR\Farlink.exe" 0 unregister_with_sc

    DetailPrint "Stopping Farlink service"
    ExecWait '"$INSTDIR\Farlink.exe" --service-stop' $0
    ${If} $0 = 0
        DetailPrint "Removing Farlink service"
        ExecWait '"$INSTDIR\Farlink.exe" --service-uninstall' $0
        ${If} $0 = 0
            Return
        ${EndIf}
    ${EndIf}

unregister_with_sc:
    DetailPrint "Removing Farlink service via Service Control Manager"
    ExecWait '"$SYSDIR\sc.exe" stop ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
    ${AndIf} $0 != 1062
        MessageBox MB_ICONSTOP|MB_OK "Failed to stop the Farlink service. Uninstall will be aborted."
        Abort
    ${EndIf}
    Sleep 1500

    ExecWait '"$SYSDIR\sc.exe" delete ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
        MessageBox MB_ICONSTOP|MB_OK "Failed to remove the Farlink service. Uninstall will be aborted."
        Abort
    ${EndIf}
FunctionEnd

; The session helper (or an elevated Farlink) installs the bundled usbmmidd
; driver on first headless use and records HKLM\SOFTWARE\Farlink\
; UsbmmiddDriverInstalled in the 64-bit registry view. Only a driver Farlink
; installed is removed here; one set up by the user for other software stays.
; deviceinstaller64.exe resolves usbmmIdd.dll relative to the working
; directory, so run it from the driver folder. Failures are logged, not fatal:
; the driver can still be removed from Device Manager.
Function un.RemoveUsbmmiddDriver
    IfFileExists "$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" 0 done

    SetRegView 64
    ClearErrors
    ReadRegDWORD $0 HKLM "Software\${PRODUCT_NAME}" "UsbmmiddDriverInstalled"
    ${If} ${Errors}
    ${OrIf} $0 != 1
        SetRegView lastused
        Return
    ${EndIf}

    DetailPrint "Removing the usbmmidd virtual display driver installed by Farlink"
    SetOutPath "$INSTDIR\usbmmidd_v2"
    ; Unplug every virtual monitor first (the driver allows up to four).
    StrCpy $1 0
unplug_loop:
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" enableidd 0'
    Pop $2
    IntOp $1 $1 + 1
    ${If} $2 = 0
    ${AndIf} $1 < 4
        Goto unplug_loop
    ${EndIf}
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" stop usbmmidd'
    Pop $2
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" remove usbmmidd'
    Pop $2
    ${If} $2 != 0
        DetailPrint "usbmmidd driver removal returned $2; remove 'USB Mobile Monitor Virtual Display' from Device Manager if it remains"
    ${EndIf}
    SetOutPath "$TEMP"

    DeleteRegValue HKLM "Software\${PRODUCT_NAME}" "UsbmmiddDriverInstalled"
    DeleteRegKey /ifempty HKLM "Software\${PRODUCT_NAME}"
    SetRegView lastused

done:
FunctionEnd
