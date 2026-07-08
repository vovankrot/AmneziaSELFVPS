; AmneziaVPN Installer Script (auto-generated)
; Inno Setup 6.0
; SAFETY: NO restartreplace anywhere - kernel driver files + WDF = BSOD on boot replace.
; PrepareToInstall guarantees all files are unlocked before copy begins.

#define MyAppName "AmneziaVPN"
#define MyAppVersion "4.8.15.0"
#define MyAppPublisher "AmneziaVPN"
#define MyAppURL "https://amnezia.org/"
#define MyAppExeName "AmneziaVPN.exe"
#define MyServiceName "AmneziaVPN-service"
#define MyServiceExeName "AmneziaVPN-service.exe"

[Setup]
AppId={{2D55AC62-96D6-4692-8C05-0D85BBF95485}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
SetupMutex=AmneziaVPNSetup
LicenseFile=E:\AmneziaSELFVPS\LICENSE
OutputDir=E:\AmneziaSELFVPS
OutputBaseFilename=AmneziaVPN_4.8.15_x64_setup
SetupIconFile=E:\AmneziaSELFVPS\client\images\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardImageBackColor=#13001F
WizardSmallImageBackColor=#13001F
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
PrivilegesRequired=admin
MinVersion=6.1sp1
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.AmneziaLaunchBypassMenu=Amnezia: Run outside VPN
russian.AmneziaLaunchBypassMenu=Amnezia: Запустить в обход VPN

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Kernel driver: separate entry with Check function вЂ” silently skip if loaded/locked.
; NEVER force-replace a loaded minifilter .sys вЂ” any interaction with a loaded driver = BSOD risk.
Source: "E:\AmneziaSELFVPS\build-installer\installer_stage\mullvad-split-tunnel.sys"; DestDir: "{app}"; Flags: ignoreversion; Check: CanInstallDriver
; Everything else (excludes the .sys handled above)
Source: "E:\AmneziaSELFVPS\build-installer\installer_stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "mullvad-split-tunnel.sys"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCR; Subkey: "*\shell\AmneziaLaunchBypass"; ValueType: string; ValueName: ""; ValueData: "{cm:AmneziaLaunchBypassMenu}"; Flags: uninsdeletekey
Root: HKCR; Subkey: "*\shell\AmneziaLaunchBypass"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "*\shell\AmneziaLaunchBypass\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" --launch-bypassed ""%1"""; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\post_uninstall.cmd"; Parameters: ""; Flags: runhidden waituntilterminated; RunOnceId: "PostUninstall"
; Do not stop/delete AmneziaVPNSplitTunnel here. It is a kernel minifilter driver;
; unloading it from installer/uninstaller has caused BSODs on this fork.
Filename: "sc.exe"; Parameters: "stop AmneziaWGTunnel$AmneziaVPN"; Flags: runhidden; RunOnceId: "StopWGTunnel"
Filename: "sc.exe"; Parameters: "delete AmneziaWGTunnel$AmneziaVPN"; Flags: runhidden; RunOnceId: "DeleteWGTunnel"
Filename: "sc.exe"; Parameters: "stop {#MyServiceName}"; Flags: runhidden; RunOnceId: "StopService"
Filename: "sc.exe"; Parameters: "delete {#MyServiceName}"; Flags: runhidden; RunOnceId: "DeleteService"
Filename: "taskkill.exe"; Parameters: "/IM {#MyAppExeName} /F"; Flags: runhidden; RunOnceId: "KillApp"
Filename: "taskkill.exe"; Parameters: "/IM {#MyServiceExeName} /F"; Flags: runhidden; RunOnceId: "KillService"
Filename: "taskkill.exe"; Parameters: "/IM wireguard.exe /F"; Flags: runhidden; RunOnceId: "KillWG"
Filename: "taskkill.exe"; Parameters: "/IM openvpn.exe /F"; Flags: runhidden; RunOnceId: "KillOpenVpn"
Filename: "taskkill.exe"; Parameters: "/IM tun2socks.exe /F"; Flags: runhidden; RunOnceId: "KillTun2Socks"
Filename: "taskkill.exe"; Parameters: "/IM hysteria.exe /F"; Flags: runhidden; RunOnceId: "KillHysteria"
Filename: "taskkill.exe"; Parameters: "/IM ck-client.exe /F"; Flags: runhidden; RunOnceId: "KillCloak"
Filename: "taskkill.exe"; Parameters: "/IM ss-local.exe /F"; Flags: runhidden; RunOnceId: "KillShadowsocks"

[Code]

var
    CleanInstallPage: TInputOptionWizardPage;
    CleanInstallConfirmed: Boolean;

function LocalizedText(EnglishText, RussianText: String): String;
begin
    if ActiveLanguage = 'russian' then
        Result := RussianText
    else
        Result := EnglishText;
end;

function ExpectedInstallDir(): String;
begin
    Result := ExpandConstant('{autopf}\{#MyAppName}');
end;

function HasAmneziaTraces(): Boolean;
var
    TempAmneziaDir: String;
begin
    TempAmneziaDir := AddBackslash(GetEnv('TEMP')) + '{#MyAppName}';
    Result :=
        DirExists(ExpectedInstallDir()) or
        DirExists(ExpandConstant('{userappdata}\AmneziaVPN.ORG')) or
        DirExists(ExpandConstant('{localappdata}\AmneziaVPN.ORG')) or
        DirExists(ExpandConstant('{commonappdata}\{#MyAppName}')) or
        DirExists(TempAmneziaDir) or
        RegKeyExists(HKCU, 'Software\AmneziaVPN.ORG\AmneziaVPN') or
        RegValueExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#MyAppName}');
end;

function IsCleanInstallSelected(): Boolean;
begin
    Result := (CleanInstallPage <> nil) and CleanInstallPage.Values[1];
end;

procedure RemoveDirTreeIfExists(DirPath: String);
begin
    if DirExists(DirPath) then
    begin
        Log('Clean install: removing directory ' + DirPath);
        DelTree(DirPath, True, True, True);
    end;
end;

procedure RemoveRegistryTreeIfExists(RootKey: Integer; SubkeyName: String);
begin
    if RegKeyExists(RootKey, SubkeyName) then
    begin
        Log('Clean install: removing registry key ' + SubkeyName);
        RegDeleteKeyIncludingSubkeys(RootKey, SubkeyName);
    end;
end;

procedure PerformCleanInstallCleanup();
var
    TempAmneziaDir: String;
begin
    Log('Clean install mode selected');

    RemoveDirTreeIfExists(ExpandConstant('{userappdata}\AmneziaVPN.ORG'));
    RemoveDirTreeIfExists(ExpandConstant('{localappdata}\AmneziaVPN.ORG'));
    RemoveDirTreeIfExists(ExpandConstant('{commonappdata}\{#MyAppName}'));

    TempAmneziaDir := AddBackslash(GetEnv('TEMP')) + '{#MyAppName}';
    RemoveDirTreeIfExists(TempAmneziaDir);

    RemoveRegistryTreeIfExists(HKCU, 'Software\AmneziaVPN.ORG\AmneziaVPN');

    if RegValueExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#MyAppName}') then
    begin
        Log('Clean install: removing autostart entry');
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#MyAppName}');
    end;
end;

procedure InitializeWizard();
begin
    if not HasAmneziaTraces() then
        Exit;

    CleanInstallPage := CreateInputOptionPage(
        wpWelcome,
        LocalizedText('Installation mode', 'Режим установки'),
        LocalizedText('Choose how setup should handle existing Amnezia data', 'Выберите, как установщик должен обработать существующие данные Amnezia'),
        LocalizedText(
            'Normal installation keeps local servers, settings and logs. Clean installation removes local Amnezia state for the current Windows user before new files are copied. This includes servers, settings, snapshots, logs and startup integration.',
            'Обычная установка сохраняет локальные серверы, настройки и логи. Чистая установка удалит локальное состояние Amnezia для текущего пользователя Windows до копирования новых файлов. Это включает серверы, настройки, snapshots, логи и автозапуск.'
        ),
        True,
        False
    );

    CleanInstallPage.Add(LocalizedText(
        'Normal installation (keep existing local state)',
        'Обычная установка (сохранить текущие локальные данные)'));
    CleanInstallPage.Add(LocalizedText(
        'Clean installation (remove local state before install)',
        'Чистая установка (удалить локальные данные перед установкой)'));
    CleanInstallPage.Values[0] := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
    Result := True;

    if (CleanInstallPage <> nil) and (CurPageID = CleanInstallPage.ID) and IsCleanInstallSelected() and not CleanInstallConfirmed then
    begin
        CleanInstallConfirmed :=
            MsgBox(
                LocalizedText(
                    'Clean installation will remove local Amnezia servers, settings, snapshots, logs and startup entries for the current Windows user. This cannot be undone. Continue?',
                    'Чистая установка удалит локальные серверы Amnezia, настройки, snapshots, логи и записи автозапуска для текущего пользователя Windows. Это действие нельзя отменить. Продолжить?'
                ),
                mbConfirmation,
                MB_YESNO
            ) = IDYES;

        Result := CleanInstallConfirmed;
    end;
end;

// Simple file unlock test using RenameFile (safe, no kernel minifilter interaction issues).
function IsFileUnlocked(FilePath: String): Boolean;
var
  TempPath: String;
begin
  if not FileExists(FilePath) then
  begin
    Result := True;
    Exit;
  end;
  TempPath := FilePath + '.tmp_unlock_test';
  Result := RenameFile(FilePath, TempPath);
  if Result then
    RenameFile(TempPath, FilePath);
end;

// Check function for [Files] entry: skip driver .sys if loaded.
// CRITICAL: do NOT use any file system operations (RenameFile, DeleteFile,
// CreateFile etc.) on a loaded minifilter .sys вЂ” the minifilter intercepts
// its own FS operations and crashes with BSOD 0x3B.
// Instead, query the Service Control Manager вЂ” this is a pure SCM API call
// that does NOT touch the file system at all.
function CanInstallDriver(): Boolean;
var
  RC: Integer;
begin
  // sc query + findstr RUNNING: findstr returns 0 if match found, 1 if not.
  // cmd /c returns the exit code of the last command in the pipe (findstr).
  Exec('cmd.exe', '/c sc query AmneziaVPNSplitTunnel | findstr /I "RUNNING"',
       '', SW_HIDE, ewWaitUntilTerminated, RC);
  if RC = 0 then
  begin
    // Driver service is RUNNING вЂ” do NOT touch the .sys file.
    Log('CanInstallDriver: driver is RUNNING, skipping .sys');
    Result := False;
    Exit;
  end;
  // Driver is stopped or not registered вЂ” safe to install .sys.
  Log('CanInstallDriver: driver not running (rc=' + IntToStr(RC) + '), installing .sys');
  Result := True;
end;

// ---- PrepareToInstall: stop ALL services/processes, VERIFY files unlocked ----
// CRITICAL: NEVER stop/unload kernel drivers (AmneziaVPNSplitTunnel).
//   net stop / sc stop / fltmc unload on minifilter drivers = BSOD 0x3B.
//   Only stop the main service (which releases its handle to the driver).
//   The driver will idle on its own once no clients hold device handles.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  RC: Integer;
  I: Integer;
  AppDir: String;
  AllUnlocked: Boolean;
begin
  Result := '';
  AppDir := ExpandConstant('{app}');

  // ============ PHASE 1: Kill all usermode processes ============

  // Kill GUI app
  Exec('taskkill.exe', '/F /IM AmneziaVPN.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Sleep(500);

    // Kill protocol helpers that can keep old binaries/configs alive
    Exec('taskkill.exe', '/F /IM openvpn.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM tun2socks.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM hysteria.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM ck-client.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM ss-local.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM anytls-client.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM anytls-server.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exec('taskkill.exe', '/F /IM wstunnel.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Sleep(500);

  // Stop and kill WireGuard tunnel
  Exec('net.exe', 'stop AmneziaWGTunnel$AmneziaVPN', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Exec('taskkill.exe', '/F /IM wireguard.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Exec('sc.exe', 'delete AmneziaWGTunnel$AmneziaVPN', '', SW_HIDE, ewWaitUntilTerminated, RC);

  // Disable recovery on main service (prevent auto-restart after kill)
  Exec('sc.exe', 'failure AmneziaVPN-service reset= 0 actions= ////', '', SW_HIDE, ewWaitUntilTerminated, RC);

  // Stop main service SYNCHRONOUSLY (net stop waits for full stop).
  // This releases the service's handle to \\.\MULLVADSPLITTUNNEL, allowing
  // the driver to go idle. DO NOT stop the driver service itself вЂ” BSOD risk.
  Exec('net.exe', 'stop AmneziaVPN-service', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Sleep(2000);

  // Force kill service process if still alive
  Exec('taskkill.exe', '/F /IM AmneziaVPN-service.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Sleep(1000);

  // NO PHASE 2: do NOT touch kernel driver (AmneziaVPNSplitTunnel).
  // Stopping minifilter drivers = BSOD. The .sys file is handled via
  // CanInstallDriver() Check function вЂ” silently skipped if locked.

    if IsCleanInstallSelected() then
        PerformCleanInstallCleanup();

  // ============ PHASE 3: Verify usermode files are unlocked ============
  // Try up to 20 seconds (40 x 500ms). Skip .sys вЂ” handled by Check function.

  for I := 1 to 40 do
  begin
    AllUnlocked := True;

    if not IsFileUnlocked(AppDir + '\AmneziaVPN-service.exe') then
      AllUnlocked := False;
    if not IsFileUnlocked(AppDir + '\tunnel.dll') then
      AllUnlocked := False;
    if not IsFileUnlocked(AppDir + '\wintun.dll') then
      AllUnlocked := False;

    if AllUnlocked then
      Break;

    // Retry kill at halfway
    if I = 20 then
    begin
      Exec('taskkill.exe', '/F /IM AmneziaVPN-service.exe', '', SW_HIDE, ewWaitUntilTerminated, RC);
      Sleep(1000);
    end;

    Sleep(500);
  end;

  if not AllUnlocked then
  begin
    Result := 'Some files are still locked by system processes. '
            + 'Please reboot your computer and run the installer again.';
  end;
end;

// ---- CurStepChanged: after files installed, set up service ----
procedure CurStepChanged(CurStep: TSetupStep);
var
  RC: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    // Run post_install.cmd
    if FileExists(ExpandConstant('{app}\post_install.cmd')) then
      Exec(ExpandConstant('{cmd}'), '/C "' + ExpandConstant('{app}\post_install.cmd') + '"',
           ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, RC);
    Sleep(500);

    // Install VC++ Redist if bundled
    if FileExists(ExpandConstant('{app}\vc_redist.x64.exe')) then
      Exec(ExpandConstant('{app}\vc_redist.x64.exe'), '/install /quiet /norestart',
           '', SW_HIDE, ewWaitUntilTerminated, RC);

    // Try to update existing service (avoids sc delete + sc create race)
    Exec('sc.exe', ExpandConstant('config AmneziaVPN-service binpath= "{app}\AmneziaVPN-service.exe" start= auto'),
         '', SW_HIDE, ewWaitUntilTerminated, RC);

    if RC <> 0 then
    begin
      // Service doesn't exist - create it
      Exec('sc.exe', ExpandConstant('create AmneziaVPN-service binpath= "{app}\AmneziaVPN-service.exe" start= auto depend= BFE/nsi'),
           '', SW_HIDE, ewWaitUntilTerminated, RC);

      if RC <> 0 then
      begin
        // DELETE_PENDING race - wait and retry once
        Sleep(3000);
        Exec('sc.exe', ExpandConstant('create AmneziaVPN-service binpath= "{app}\AmneziaVPN-service.exe" start= auto depend= BFE/nsi'),
             '', SW_HIDE, ewWaitUntilTerminated, RC);
      end;
    end;

    // Set dependencies
    Exec('sc.exe', 'config AmneziaVPN-service depend= BFE/nsi', '', SW_HIDE, ewWaitUntilTerminated, RC);

    // Start service
    Exec('sc.exe', 'start AmneziaVPN-service', '', SW_HIDE, ewWaitUntilTerminated, RC);

    // Configure recovery
    Exec('sc.exe', 'failure AmneziaVPN-service reset= 100 actions= restart/2000/restart/2000/restart/2000',
         '', SW_HIDE, ewWaitUntilTerminated, RC);
  end;
end;