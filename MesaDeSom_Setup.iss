; ============================================================
; Mesa de Som Mix Virtual v3.0
; Script para Inno Setup 6.x
; https://jrsoftware.org/isinfo.php
; ============================================================

#define AppName      "Mesa de Som Mix Virtual"
#define AppVersion   "3.0"
#define AppPublisher "Paulo Santesso"
#define AppExeName   "placagui.exe"

[Setup]
AppId={{B3A7C2D1-4F8E-4A2B-9C3D-1E5F6A7B8C9D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL=
AppSupportURL=
AppUpdatesURL=
; {autopf} e resolvido pelo Inno Setup para o Program Files correto
; independente do idioma do Windows (PT, EN, etc.) — nao precisa tratar manualmente.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=no
OutputDir=Instalador
OutputBaseFilename=MesaDeSom_v3.0_Setup
SetupIconFile=
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
MinVersion=10.0.19041

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english";             MessagesFile: "compiler:Default.isl"
Name: "french";              MessagesFile: "compiler:Languages\French.isl"
Name: "italian";             MessagesFile: "compiler:Languages\Italian.isl"
Name: "portuguese";          MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "spanish";             MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon";   Description: "Criar atalho na Área de Trabalho"; GroupDescription: "Atalhos:"
Name: "startmenuicon"; Description: "Criar atalho no Menu Iniciar";     GroupDescription: "Atalhos:"

[Files]
; Interface grafica publicada (.NET self-contained + arquivos auxiliares)
Source: "bin\Release\net8.0-windows\win-x64\publish\*"; DestDir: "{app}"; \
    Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

; Motor de audio (C++)
Source: "Placasom.exe"; DestDir: "{app}"; Flags: ignoreversion

; Guia de instrucoes
Source: "README.md"; DestDir: "{app}"; DestName: "LEIA-ME.txt"; Flags: ignoreversion

[Icons]
; Menu Iniciar
Name: "{group}\{#AppName}";             Filename: "{app}\{#AppExeName}"; \
    Comment: "Mesa de Som Virtual com Mix Multi-Canal"; \
    Tasks: startmenuicon
Name: "{group}\Guia de Instruções";      Filename: "{app}\LEIA-ME.txt"; \
    Tasks: startmenuicon
Name: "{group}\Desinstalar {#AppName}"; Filename: "{uninstallexe}"; \
    Tasks: startmenuicon

; Area de Trabalho
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; \
    Comment: "Mesa de Som Virtual com Mix Multi-Canal"; \
    Tasks: desktopicon

[Run]
; Abre o guia de instrucoes (desmarcado por padrao — usuario escolhe)
Filename: "notepad.exe"; Parameters: "{app}\LEIA-ME.txt"; \
    Description: "Abrir o guia de instruções"; \
    Flags: postinstall skipifsilent unchecked

; Abre o programa apos instalar (usuario pode desmarcar)
Filename: "{app}\{#AppExeName}"; \
    Description: "Iniciar {#AppName} agora"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; Encerra os processos antes de desinstalar
Filename: "taskkill.exe"; Parameters: "/F /IM placagui.exe /IM Placasom.exe"; \
    RunOnceId: "StopMesaDeSomProcesses"; Flags: runhidden skipifdoesntexist

[Code]
// ============================================================
// Verifica se o VB-Audio Virtual Cable esta instalado.
// ============================================================
function VBAudioInstalado: Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\VBAudioVACWDM') or
            RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\VBAudioVoicemeeter') or
            RegKeyExists(HKLM, 'SOFTWARE\VB-Audio\Cable');
end;

// ============================================================
// Bloqueia instalacao em Windows abaixo do build 19041.
// O Process Loopback API exige Windows 10 versao 2004+.
// ============================================================
function InitializeSetup: Boolean;
var
  BuildStr: String;
  BuildNum: Integer;
begin
  Result := True;

  // Le o CurrentBuildNumber do registro
  if not RegQueryStringValue(HKLM,
    'SOFTWARE\Microsoft\Windows NT\CurrentVersion',
    'CurrentBuildNumber', BuildStr) then
    Exit; // nao conseguiu ler — deixa instalar

  BuildNum := StrToIntDef(BuildStr, 0);

  if BuildNum < 19041 then
  begin
    MsgBox(
      'Este programa requer Windows 10 versão 2004 (build 19041) ou superior.' + #13#10 +
      'Build detectado: ' + BuildStr + #13#10 + #13#10 +
      'Atualize o Windows e tente novamente.',
      mbCriticalError, MB_OK);
    Result := False;
  end;
end;

// ============================================================
// Avisa sobre VB-Audio no inicio do assistente.
// ============================================================
procedure InitializeWizard;
begin
  if not VBAudioInstalado then
    MsgBox(
      'Bem-vindo ao instalador de {#AppName}!' + #13#10 + #13#10 +
      'ATENÇÃO: O VB-Audio Virtual Cable NÃO foi detectado.' + #13#10 + #13#10 +
      'Para que o programa funcione, instale o VB-Audio:' + #13#10 +
      '  https://vb-audio.com/Cable/' + #13#10 + #13#10 +
      'Você pode instalar antes ou depois — a instalação continuará normalmente.',
      mbInformation, MB_OK)
  else
    MsgBox(
      'Bem-vindo ao instalador de {#AppName}!' + #13#10 + #13#10 +
      'VB-Audio Virtual Cable detectado. Tudo certo!' + #13#10 + #13#10 +
      'Clique em Avançar para continuar.',
      mbInformation, MB_OK);
end;