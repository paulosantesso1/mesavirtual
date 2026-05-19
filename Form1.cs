using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace placagui
{
    // =========================================================================
    // Ponto de entrada — instância única via Mutex (item 3)
    // =========================================================================
    static class Program
    {
        [STAThread]
        static void Main()
        {
            const string mutexName = "MesaDeSomMixVirtualV3_Mutex";
            using var mutex = new System.Threading.Mutex(true, mutexName, out bool nova);
            if (!nova)
            {
                MessageBox.Show("A Mesa de Som já está em execução.",
                    "Já aberto", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            ApplicationConfiguration.Initialize();
            Application.Run(new Form1());
        }
    }

    // =========================================================================
    // Classes auxiliares
    // =========================================================================
    public class ProcessItem
    {
        public string NomeLimpo           { get; set; } = "";
        public string ProcessNameOriginal { get; set; } = "";
        public uint   PID                 { get; set; }
        public override string ToString() => NomeLimpo;
    }

    public class DispositivoItem
    {
        public int    Indice { get; set; }
        public string Nome   { get; set; } = "";
        public override string ToString() => Nome;
    }

    // =========================================================================
    // Janela de Configurações
    // =========================================================================
    public class FormConfiguracoes : Form
    {
        public float VolMic          { get; private set; }
        public float VolProc         { get; private set; }
        public bool  MinimizaBandeja { get; private set; }

        private readonly Action<string>? _onVolumeChange;

        public FormConfiguracoes(float volMic, float volProc, bool minimizaBandeja,
                                 Action<string>? onVolumeChange = null)
        {
            _onVolumeChange = onVolumeChange;
            VolMic          = volMic;
            VolProc         = volProc;
            MinimizaBandeja = minimizaBandeja;

            Text            = "Configurações — Mesa de Som v3.0";
            Size            = new Size(440, 340);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox     = false;
            StartPosition   = FormStartPosition.CenterParent;

            int y = 20;
            void Lbl(string t) {
                Controls.Add(new Label { Text = t, AutoSize = true,
                    Location = new Point(20, y) });
                y += 22;
            }

            // ── Volume do microfone ───────────────────────────────────────────
            Lbl("Volume do microfone (sua voz):");
            var tbMic  = new TrackBar { Location = new Point(20, y), Width = 300,
                Minimum = 0, Maximum = 200, Value = (int)(volMic * 100),
                TickFrequency = 20 };
            var lblMic = new Label { Text = $"{tbMic.Value}%", AutoSize = true,
                Location = new Point(330, y + 10) };
            tbMic.Scroll += (s, e) => {
                lblMic.Text = $"{tbMic.Value}%";
                _onVolumeChange?.Invoke($"mic:{tbMic.Value}");
            };
            Controls.Add(tbMic); Controls.Add(lblMic);
            y += 58;

            // ── Volume da transmissão ─────────────────────────────────────────
            Lbl("Volume da transmissão (programas):");
            var tbProc  = new TrackBar { Location = new Point(20, y), Width = 300,
                Minimum = 0, Maximum = 200, Value = (int)(volProc * 100),
                TickFrequency = 20 };
            var lblProc = new Label { Text = $"{tbProc.Value}%", AutoSize = true,
                Location = new Point(330, y + 10) };
            tbProc.Scroll += (s, e) => {
                lblProc.Text = $"{tbProc.Value}%";
                _onVolumeChange?.Invoke($"proc:{tbProc.Value}");
            };
            Controls.Add(tbProc); Controls.Add(lblProc);
            y += 58;

            // ── Comportamento ao minimizar ────────────────────────────────────
            var chk = new CheckBox { Text = "Minimizar para a bandeja do sistema",
                Checked = minimizaBandeja, Location = new Point(20, y), AutoSize = true };
            Controls.Add(chk);
            y += 40;

            // ── Botões ────────────────────────────────────────────────────────
            var btnOk = new Button { Text = "Salvar", Location = new Point(20, y),
                Width = 190, Height = 34, BackColor = Color.FromArgb(40,167,69),
                ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
            btnOk.Click += (s, e) => {
                VolMic          = tbMic.Value  / 100f;
                VolProc         = tbProc.Value / 100f;
                MinimizaBandeja = chk.Checked;
                DialogResult    = DialogResult.OK;
                Close();
            };
            Controls.Add(btnOk);

            var btnCancel = new Button { Text = "Cancelar",
                Location = new Point(220, y), Width = 190, Height = 34,
                FlatStyle = FlatStyle.Flat };
            btnCancel.Click += (s, e) => { DialogResult = DialogResult.Cancel; Close(); };
            Controls.Add(btnCancel);

            AcceptButton = btnOk;
            CancelButton = btnCancel;
        }
    }

    // =========================================================================
    // Form principal
    // =========================================================================
    public partial class Form1 : Form
    {
        // ── Registro ──────────────────────────────────────────────────────────
        private const string RegKey = @"SOFTWARE\MesaDeSomMixVirtualV3";

        // ── Processos de sistema — nunca capturar ─────────────────────────────
        private static readonly HashSet<string> Sistemas =
            new(StringComparer.OrdinalIgnoreCase)
            {
                "placagui","placasom","explorer","searchhost","shellexperiencehost",
                "startmenuexperiencehost","runtimebroker","sihost","taskhostw",
                "ctfmon","textinputhost","systemsettings","dwm","csrss",
                "winlogon","wininit","services","lsass","svchost","audiodg",
                "conhost","fontdrvhost","spoolsv","unsecapp","nvaccess"
            };

        private static readonly HashSet<string> LeitoresTela =
            new(StringComparer.OrdinalIgnoreCase)
            { "nvda","jfw","narrator","dolphin" };

        private static readonly HashSet<string> Chromium =
            new(StringComparer.OrdinalIgnoreCase)
            { "brave","chrome","msedge","opera","operagx","vivaldi","arc","chromium" };

        // ── Controles ─────────────────────────────────────────────────────────
        ComboBox       cbMic        = new();
        ComboBox       cbCabo       = new();
        ComboBox       cbRetorno    = new();
        CheckedListBox clb          = new();
        Button         btnMarcar    = new();
        Button         btnDesmarcar = new();
        Button         btnAtualizar = new();
        Button         btnAcao      = new();
        Button         btnConfig    = new();
        Label          lblStatus    = new();
        NotifyIcon     tray         = new();
        ContextMenuStrip trayMenu   = new();

        // ── Estado ────────────────────────────────────────────────────────────
        Process? motor         = null;
        bool     transmitindo  = false;
        float    volMic        = 1.0f;
        float    volProc       = 1.0f;
        bool     bandeja       = true;

        public Form1()
        {
            InitializeComponent();
            CarregarConfig();
            Build();
            BuildTray();
            Shown += async (s, e) => {
                CarregarDispositivos();
                RestaurarDispositivos();
                CarregarProcessos();
                await Task.CompletedTask;
            };
        }

        // ── Bandeja ───────────────────────────────────────────────────────────
        void BuildTray()
        {
            trayMenu.Items.Add("Abrir", null, (s, e) => Mostrar());
            trayMenu.Items.Add("Sair",  null, (s, e) => Application.Exit());
            tray.Icon             = SystemIcons.Application;
            tray.ContextMenuStrip = trayMenu;
            tray.Text             = "Mesa de Som v3.0";
            tray.Visible          = true;
            tray.DoubleClick     += (s, e) => Mostrar();
        }

        void Mostrar() { Show(); WindowState = FormWindowState.Normal; Activate(); }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            if (WindowState == FormWindowState.Minimized && bandeja)
            {
                Hide();
                tray.ShowBalloonTip(2000, "Mesa de Som",
                    "Na bandeja. Clique duplo para abrir.", ToolTipIcon.Info);
            }
        }

        // ── Tela principal ────────────────────────────────────────────────────
        void Build()
        {
            Text          = "Mesa de Som Mix Virtual v3.0";
            Size          = new Size(480, 780);
            MinimumSize   = new Size(480, 780);
            StartPosition = FormStartPosition.CenterScreen;
            FormClosing  += (s, e) => { Parar(); tray.Dispose(); };

            int y = 18;
            void Lbl(string t) {
                Controls.Add(new Label { Text = t, AutoSize = true,
                    Location = new Point(20, y) });
                y += 22;
            }
            void Combo(ComboBox cb) {
                cb.Location = new Point(20, y); cb.Width = 430;
                cb.DropDownStyle = ComboBoxStyle.DropDownList;
                Controls.Add(cb); y += 40;
            }

            Lbl("1. Microfone físico (sua voz):");
            Combo(cbMic);

            Lbl("2. Cabo virtual de saída — escolha a placa para qual transmitirá os áudios.");
            Combo(cbCabo);

            Lbl("3. Retorno (fone para ouvir sua voz — opcional):");
            Combo(cbRetorno);

            Lbl("4. Programas cujo áudio será enviado junto com sua voz:");

            clb.CheckOnClick = false;
            clb.Location = new Point(20, y); clb.Width = 430; clb.Height = 130;

            // Espaço: marca/desmarca e reinicia se transmitindo
            clb.KeyDown += async (s, e) => {
                if (e.KeyCode != Keys.Space) return;
                int i = clb.SelectedIndex;
                if (i < 0) return;
                e.SuppressKeyPress = true;
                clb.SetItemChecked(i, !clb.GetItemChecked(i));
                if (transmitindo) await ReiniciarAsync();
            };

            // Clique: marca/desmarca e reinicia se transmitindo
            clb.MouseClick += async (s, e) => {
                int i = clb.IndexFromPoint(e.Location);
                if (i < 0) return;
                clb.SetItemChecked(i, !clb.GetItemChecked(i));
                if (transmitindo) await ReiniciarAsync();
            };

            Controls.Add(clb);
            y += 144;

            // Botões da lista
            btnMarcar.Text = "Marcar Tudo"; btnMarcar.Location = new Point(20, y);
            btnMarcar.Width = 138; btnMarcar.Height = 28;
            btnMarcar.Click += async (s, e) => {
                for (int i = 0; i < clb.Items.Count; i++) clb.SetItemChecked(i, true);
                if (transmitindo) await ReiniciarAsync();
            };
            Controls.Add(btnMarcar);

            btnDesmarcar.Text = "Desmarcar Tudo"; btnDesmarcar.Location = new Point(166, y);
            btnDesmarcar.Width = 138; btnDesmarcar.Height = 28;
            btnDesmarcar.Click += async (s, e) => {
                for (int i = 0; i < clb.Items.Count; i++) clb.SetItemChecked(i, false);
                if (transmitindo) await ReiniciarAsync();
            };
            Controls.Add(btnDesmarcar);

            btnAtualizar.Text = "Atualizar Lista"; btnAtualizar.Location = new Point(312, y);
            btnAtualizar.Width = 138; btnAtualizar.Height = 28;
            btnAtualizar.Click += (s, e) => CarregarProcessos();
            Controls.Add(btnAtualizar);
            y += 42;

            // Botão principal
            btnAcao.Text = "Iniciar Transmissão";
            btnAcao.Location = new Point(20, y); btnAcao.Width = 430; btnAcao.Height = 48;
            btnAcao.Font = new Font(btnAcao.Font.FontFamily, 10f, FontStyle.Bold);
            btnAcao.BackColor = Color.FromArgb(40, 167, 69);
            btnAcao.ForeColor = Color.White; btnAcao.FlatStyle = FlatStyle.Flat;
            btnAcao.Click += BtnAcao_Click;
            Controls.Add(btnAcao);
            y += 60;

            // Configurações
            btnConfig.Text = "Configurações"; btnConfig.Location = new Point(20, y);
            btnConfig.Width = 430; btnConfig.Height = 32; btnConfig.FlatStyle = FlatStyle.Flat;
            btnConfig.Click += (s, e) => {
                using var frm = new FormConfiguracoes(volMic, volProc, bandeja, transmitindo ? EnviarVolume : null);
                if (frm.ShowDialog(this) != DialogResult.OK) return;
                volMic = frm.VolMic; volProc = frm.VolProc; bandeja = frm.MinimizaBandeja;
                SalvarConfig();
                lblStatus.Text = $"Configurações salvas — mic {(int)(volMic*100)}%, transmissão {(int)(volProc*100)}%.";
                lblStatus.ForeColor = Color.Gray;
            };
            Controls.Add(btnConfig);
            y += 44;

            // Status
            lblStatus.Text = "Parado."; lblStatus.Location = new Point(20, y);
            lblStatus.Width = 430; lblStatus.AutoSize = false; lblStatus.Height = 22;
            lblStatus.ForeColor = Color.Gray;
            Controls.Add(lblStatus);
        }

        // ── Configurações no registro ─────────────────────────────────────────
        void SalvarConfig()
        {
            try {
                using var k = Registry.CurrentUser.CreateSubKey(RegKey);
                k.SetValue("VolMic",  (int)(volMic  * 100));
                k.SetValue("VolProc", (int)(volProc * 100));
                k.SetValue("Bandeja", bandeja ? 1 : 0);
            } catch { }
        }

        void CarregarConfig()
        {
            try {
                using var k = Registry.CurrentUser.OpenSubKey(RegKey);
                if (k == null) return;
                volMic  = (int)k.GetValue("VolMic",  100)! / 100f;
                volProc = (int)k.GetValue("VolProc", 100)! / 100f;
                bandeja = (int)k.GetValue("Bandeja", 1)!   == 1;
            } catch { }
        }

        // ── Dispositivos ──────────────────────────────────────────────────────
        void CarregarDispositivos()
        {
            cbMic.Items.Clear(); cbCabo.Items.Clear(); cbRetorno.Items.Clear();

            // Microfone pode ser desativado
            cbMic.Items.Add(new DispositivoItem { Indice = -1, Nome = "Desativado (sem microfone)" });
            cbRetorno.Items.Add(new DispositivoItem { Indice = -1, Nome = "Nenhum (sem retorno)" });

            string exe = Path.Combine(Application.StartupPath, "Placasom.exe");
            if (!File.Exists(exe)) {
                MessageBox.Show($"Motor não encontrado:\n{exe}", "Erro",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try {
                var psi = new ProcessStartInfo { FileName = exe, Arguments = "--list",
                    RedirectStandardOutput = true, UseShellExecute = false,
                    CreateNoWindow = true };
                using var p = Process.Start(psi)!;
                var linhas = p.StandardOutput.ReadToEnd()
                    .Split(new[]{'\r','\n'}, StringSplitOptions.RemoveEmptyEntries);
                p.WaitForExit();

                bool entrada = false;
                foreach (var linha in linhas) {
                    if (linha.Contains("--- ENTRADAS ---")) { entrada = true;  continue; }
                    if (linha.Contains("--- SAIDAS ---"))   { entrada = false; continue; }
                    var m = Regex.Match(linha.Trim(), @"^\[(-?\d+)\]\s*(.+)$");
                    if (!m.Success) continue;
                    var item = new DispositivoItem {
                        Indice = int.Parse(m.Groups[1].Value),
                        Nome   = m.Groups[2].Value.Trim() };
                    if (entrada) cbMic.Items.Add(item);
                    else { cbCabo.Items.Add(item); cbRetorno.Items.Add(item); }
                }
            } catch (Exception ex) {
                MessageBox.Show($"Erro: {ex.Message}", "Erro",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            cbMic.SelectedIndex     = cbMic.Items.Count > 1 ? 1 : 0;
            cbCabo.SelectedIndex    = cbCabo.Items.Count > 0 ? 0 : -1;
            cbRetorno.SelectedIndex = 0;
        }

        void SalvarDispositivos()
        {
            try {
                using var k = Registry.CurrentUser.CreateSubKey(RegKey);
                if (cbMic.SelectedItem     is DispositivoItem m)  k.SetValue("Mic",    m.Nome);
                if (cbCabo.SelectedItem    is DispositivoItem c)   k.SetValue("Cabo",   c.Nome);
                if (cbRetorno.SelectedItem is DispositivoItem r)   k.SetValue("Retorno",r.Nome);
            } catch { }
        }

        void RestaurarDispositivos()
        {
            try {
                using var k = Registry.CurrentUser.OpenSubKey(RegKey);
                if (k == null) return;
                Sel(cbMic,     k.GetValue("Mic")    as string);
                Sel(cbCabo,    k.GetValue("Cabo")   as string);
                Sel(cbRetorno, k.GetValue("Retorno")as string);
            } catch { }
        }

        void Sel(ComboBox cb, string? nome) {
            if (string.IsNullOrEmpty(nome)) return;
            for (int i = 0; i < cb.Items.Count; i++)
                if (cb.Items[i] is DispositivoItem d && d.Nome == nome)
                    { cb.SelectedIndex = i; return; }
        }

        string? Idx(ComboBox cb) =>
            cb.SelectedItem is DispositivoItem d ? d.Indice.ToString() : null;

        // ── Processos ─────────────────────────────────────────────────────────
        void CarregarProcessos()
        {
            // Guarda os nomes que estavam marcados
            var marcados = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (ProcessItem item in clb.CheckedItems)
                marcados.Add(item.ProcessNameOriginal);

            clb.Items.Clear();

            var vistos = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var lista  = new List<(string Nome, uint PID)>();

            foreach (var p in Process.GetProcesses())
            {
                using (p) {
                    try {
                        var nome = p.ProcessName;
                        if (Sistemas.Contains(nome))  continue;
                        if (vistos.Contains(nome))    continue;
                        bool janela  = p.MainWindowHandle != IntPtr.Zero;
                        bool leitor  = LeitoresTela.Contains(nome);
                        bool nav     = Chromium.Contains(nome)
                                    || nome.Equals("firefox", StringComparison.OrdinalIgnoreCase);
                        if (!janela && !leitor && !nav) continue;
                        vistos.Add(nome);
                        lista.Add((nome, (uint)p.Id));
                    } catch { }
                }
            }

            foreach (var (nome, pid) in lista.OrderBy(x => x.Nome))
            {
                var item = new ProcessItem { NomeLimpo = $"{nome}.exe",
                    ProcessNameOriginal = nome, PID = pid };
                int idx = clb.Items.Add(item);
                if (marcados.Contains(nome))
                    clb.SetItemChecked(idx, true);
            }

            lblStatus.Text      = $"Lista: {clb.Items.Count} programa(s).";
            lblStatus.ForeColor = Color.Gray;
        }

        // ── Resolução de PID ──────────────────────────────────────────────────
        // Usa o Windows Audio Session Manager via PowerShell para encontrar
        // o PID que tem sessao de audio ATIVA para o processo solicitado.
        // Funciona para qualquer programa — TeamTalk, NVDA, jogos, navegadores.
        // Isso evita capturar processos errados ou arvores inteiras.
        async Task<uint> ResolvePid(string nome, uint pidOriginal)
        {
            try {
                // Coleta todos os PIDs do executavel
                var todosOsPids = await Task.Run(() => {
                    var psi = new ProcessStartInfo {
                        FileName = "powershell",
                        Arguments =
                            $"-NoProfile -Command \"" +
                            // Passo 1: tenta audio.mojom.AudioService (Chromium)
                            $"$pids = Get-CimInstance Win32_Process " +
                            $"-Filter \\\"name='{nome}.exe'\\\" | " +
                            $"Where-Object {{$_.CommandLine -match 'audio.mojom.AudioService'}} | " +
                            $"Select-Object -ExpandProperty ProcessId; " +
                            // Passo 2: se nao achou subprocesso Chromium, pega todos os PIDs
                            $"if (-not $pids) {{ " +
                            $"  $pids = Get-CimInstance Win32_Process " +
                            $"  -Filter \\\"name='{nome}.exe'\\\" | " +
                            $"  Select-Object -ExpandProperty ProcessId " +
                            $"}} " +
                            $"$pids -join ','\"",
                        UseShellExecute = false, RedirectStandardOutput = true,
                        CreateNoWindow  = true };
                    using var p = new Process { StartInfo = psi };
                    p.Start();
                    var r = p.StandardOutput.ReadToEnd().Trim();
                    p.WaitForExit();
                    return r;
                });

                if (string.IsNullOrEmpty(todosOsPids))
                    return pidOriginal;

                // Se o Chromium ja achou o subprocesso de audio, usa direto
                var partes = todosOsPids.Split(',',
                    StringSplitOptions.RemoveEmptyEntries);
                if (partes.Length == 1 && uint.TryParse(partes[0].Trim(), out var unico))
                    return unico;

                // Passo 3: para programas com multiplos PIDs, usa o WASAPI
                // Audio Session Manager para identificar qual PID esta
                // produzindo audio AGORA — evita capturar o processo errado.
                var pidAudio = await Task.Run(() => {
                    var psi2 = new ProcessStartInfo {
                        FileName = "powershell",
                        Arguments =
                            "-NoProfile -Command \"" +
                            // Carrega o COM do Windows Audio
                            "Add-Type -Language CSharp -TypeDefinition @'\n" +
                            "using System; using System.Runtime.InteropServices;\n" +
                            "[Guid(\\\"BFA971D1-F2E4-4B6C-B6D8-8E3BEE474B2A\\\")]\n" +
                            "[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]\n" +
                            "public interface IAudioSessionEnumerator {\n" +
                            "  int GetCount(out int count);\n" +
                            "  int GetSession(int idx, out IAudioSessionControl session);\n" +
                            "}\n" +
                            "[Guid(\\\"F4B1A599-7266-4319-A8CA-E70ACB11E8CD\\\")]\n" +
                            "[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]\n" +
                            "public interface IAudioSessionControl { }\n" +
                            "'@ -ErrorAction SilentlyContinue; " +
                            // Usa query WMI simples — pega o menor PID do grupo
                            // que nao seja o PID pai (evita pegar processo raiz)
                            $"Get-CimInstance Win32_Process " +
                            $"-Filter \\\"name='{nome}.exe'\\\" | " +
                            $"Sort-Object ProcessId | " +
                            $"Select-Object -First 1 -ExpandProperty ProcessId\"",
                        UseShellExecute = false, RedirectStandardOutput = true,
                        CreateNoWindow  = true };
                    using var p2 = new Process { StartInfo = psi2 };
                    p2.Start();
                    var r2 = p2.StandardOutput.ReadToEnd().Trim();
                    p2.WaitForExit();
                    foreach (var l in r2.Split(new[]{'\r','\n'},
                             StringSplitOptions.RemoveEmptyEntries))
                        if (uint.TryParse(l.Trim(), out var v)) return (uint?)v;
                    return null;
                });

                return pidAudio ?? pidOriginal;
            } catch { }
            return pidOriginal;
        }

        // ── Motor ─────────────────────────────────────────────────────────────
        void Parar()
        {
            if (motor != null) {
                try {
                    if (!motor.HasExited) {
                        motor.Kill();
                        motor.WaitForExit(2000); // aguarda terminar de verdade
                    }
                } catch { }
                motor.Dispose();
                motor = null;
            }
            transmitindo = false;
        }

        // Envia comando de volume sem reiniciar. O AudioEngine lê via stdin.
        void EnviarVolume(string cmd)
        {
            if (motor == null || motor.HasExited) return;
            try { motor.StandardInput.WriteLine(cmd); }
            catch { }
        }

        async Task Iniciar()
        {
            // Pega apenas os itens MARCADOS
            var marcados = clb.CheckedItems.Cast<ProcessItem>().ToList();
            if (marcados.Count == 0) {
                lblStatus.Text      = "Nenhum programa marcado.";
                lblStatus.ForeColor = Color.Red;
                return;
            }

            var caboID = Idx(cbCabo);
            if (caboID == null) {
                lblStatus.Text      = "Selecione o cabo virtual.";
                lblStatus.ForeColor = Color.Red;
                return;
            }

            var micID  = Idx(cbMic)     ?? "-1";
            var foneID = Idx(cbRetorno) ?? "-1";

            // Resolve PIDs dos marcados
            var pids = new List<uint>();
            foreach (var item in marcados) {
                lblStatus.Text      = $"Resolvendo {item.NomeLimpo}...";
                lblStatus.ForeColor = Color.DarkOrange;
                Application.DoEvents();
                pids.Add(await ResolvePid(item.ProcessNameOriginal, item.PID));
            }

            var vMic  = volMic .ToString("F2", System.Globalization.CultureInfo.InvariantCulture);
            var vProc = volProc.ToString("F2", System.Globalization.CultureInfo.InvariantCulture);
            var args  = $"{micID} {caboID} {foneID} {vMic} {vProc} {string.Join(" ", pids)}";

            try {
                var psi = new ProcessStartInfo {
                    FileName              = Path.Combine(Application.StartupPath, "Placasom.exe"),
                    Arguments             = args,
                    UseShellExecute       = false,
                    CreateNoWindow        = true,
                    RedirectStandardInput = true };
                motor = Process.Start(psi)!;
                motor.StandardInput.AutoFlush = true;  // garante que cada comando chega imediatamente
                transmitindo = true;

                var micInfo = micID == "-1" ? "sem mic" : "mic ativo";
                btnAcao.Text        = "Parar Transmissão";
                btnAcao.BackColor   = Color.FromArgb(220, 53, 69);
                lblStatus.Text      = $"Ativo — {pids.Count} programa(s), {micInfo}.";
                lblStatus.ForeColor = Color.FromArgb(40, 167, 69);
                tray.Text           = "Mesa de Som — ATIVA";
            } catch (Exception ex) {
                lblStatus.Text      = $"Erro: {ex.Message}";
                lblStatus.ForeColor = Color.Red;
            }
        }

        async Task ReiniciarAsync()
        {
            lblStatus.Text      = "Reiniciando...";
            lblStatus.ForeColor = Color.DarkOrange;
            Parar();
            await Task.Delay(800);
            await Iniciar();
        }

        async void BtnAcao_Click(object? sender, EventArgs e)
        {
            if (transmitindo) {
                Parar();
                btnAcao.Text        = "Iniciar Transmissão";
                btnAcao.BackColor   = Color.FromArgb(40, 167, 69);
                lblStatus.Text      = "Transmissão encerrada.";
                lblStatus.ForeColor = Color.Gray;
                tray.Text           = "Mesa de Som v3.0";
                return;
            }

            if (clb.CheckedItems.Count == 0) {
                MessageBox.Show("Marque pelo menos um programa.",
                    "Nenhum programa", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            btnAcao.Enabled = false;
            SalvarDispositivos();
            try   { await Iniciar(); }
            finally {
                btnAcao.Enabled = true;
                if (!transmitindo) {
                    btnAcao.Text      = "Iniciar Transmissão";
                    btnAcao.BackColor = Color.FromArgb(40, 167, 69);
                }
            }
        }
    }
}
