// SmashTV.exe: the one file a player downloads and runs.
//
// First run: unpack the private build kit (embedded, no game data) to
// %LOCALAPPDATA%\Programs\SmashTV\<kit hash>, add a Start menu shortcut, ask
// for the player's own retail L8 ROM ZIP and build the native game from it
// (progress window, no console). Every later run verifies the built game and
// starts it directly: no Python, no hashing of the kit, no prompts.
// A new SmashTV.exe rebuilds from the ROM already imported, without asking.
// Hold Shift while starting (or pass --select-rom) to choose a different ROM.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

internal static class PlayerSetup {
    const string Title = "Smash T.V.";
    // STV_INSTALL_ROOT is a test override: everything (no shortcut) under one folder.
    static readonly string TestRoot = Environment.GetEnvironmentVariable("STV_INSTALL_ROOT");
    static readonly string LocalData = string.IsNullOrEmpty(TestRoot)
        ? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) : TestRoot;
    static readonly string InstallParent = Path.Combine(LocalData, "Programs", "SmashTV");
    static readonly string State = Path.Combine(LocalData, "SmashTV", "native-player");

    static string Sha256(string path) {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    [STAThread]
    static int Main(string[] args) {
        Application.EnableVisualStyles();
        try {
            using (var mutex = new Mutex(false, "Local\\SmashTV-Launcher")) {
                try { mutex.WaitOne(); } catch (AbandonedMutexException) {}
                try { return Run(args); } finally { mutex.ReleaseMutex(); }
            }
        } catch (Exception e) {
            MessageBox.Show(e.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    static int Run(string[] args) {
        string kit = ExtractKit();
        InstallLauncher(kit);
        Directory.CreateDirectory(State);
        string kitId = Sha256(Path.Combine(kit, "manifest.json"));
        bool choose = Array.IndexOf(args, "--select-rom") >= 0 || (Control.ModifierKeys & Keys.Shift) != 0;
        // A ROM ZIP dropped onto SmashTV.exe (or passed on the command line).
        string dropped = null;
        foreach (var arg in args)
            if (arg.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && File.Exists(arg)) dropped = Path.GetFullPath(arg);
        if (dropped != null) {
            if (Build(kit, dropped, "Building Smash T.V. from your ROM...")) {
                var built = Player.Load(State);
                if (built != null && built.Verify()) return Launch(built);
            }
            MessageBox.Show("That ZIP could not be used. Choose a complete retail Smash T.V. L8 set.\n\nDetails: " +
                            Path.Combine(State, "setup.log"), Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            choose = true;
        }

        Player player = Player.Load(State);
        if (!choose && player != null && player.Kit == kitId && player.Verify()) return Launch(player);

        // Same ROM, new kit (or a damaged build): rebuild without asking.
        if (!choose && player != null && player.RomIntact()) {
            string previous = Path.Combine(Path.GetTempPath(), "SmashTV-" + Guid.NewGuid().ToString("N") + ".zip");
            File.Copy(player.Rom, previous);
            try {
                if (Build(kit, previous, "Updating Smash T.V. for this version...")) {
                    player = Player.Load(State);
                    if (player != null && player.Verify()) return Launch(player);
                }
            } finally { try { File.Delete(previous); } catch (IOException) {} }
        }

        using (var picker = new OpenFileDialog()) {
            picker.Title = "Smash T.V. - choose your retail L8 ROM ZIP";
            picker.Filter = "ROM archives (*.zip)|*.zip";
            while (picker.ShowDialog() == DialogResult.OK) {
                if (Build(kit, picker.FileName, "Building Smash T.V. from your ROM...")) {
                    player = Player.Load(State);
                    if (player != null && player.Verify()) return Launch(player);
                }
                MessageBox.Show("That ZIP could not be used. Choose a complete retail Smash T.V. L8 set " +
                                "(for a split set, keep the parent smashtv.zip beside it).\n\nDetails: " +
                                Path.Combine(State, "setup.log"), Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
        return 0;
    }

    static int Launch(Player player) {
        var start = new ProcessStartInfo(player.Exe) {
            WorkingDirectory = Path.GetDirectoryName(player.Exe),
            UseShellExecute = false
        };
        start.EnvironmentVariables["STV_HOST_ROM_ZIP"] = player.Rom;
        Process.Start(start).Dispose();
        return 0;
    }

    // ---- kit ---------------------------------------------------------------
    static string ExtractKit() {
        using (Stream zip = Assembly.GetExecutingAssembly().GetManifestResourceStream("player.zip")) {
            if (zip == null) throw new InvalidDataException("This SmashTV.exe has no player kit.");
            string id;
            using (var sha = SHA256.Create()) id = BitConverter.ToString(sha.ComputeHash(zip)).Replace("-", "").Substring(0, 24);
            string dest = Path.Combine(InstallParent, id);
            if (File.Exists(Path.Combine(dest, "manifest.json"))) return dest;
            Directory.CreateDirectory(InstallParent);
            zip.Position = 0;
            string tmp = Path.Combine(InstallParent, "tmp-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tmp);
            using (var progress = new ProgressWindow("Installing Smash T.V....")) {
                progress.Show();
                try {
                    using (var archive = new ZipArchive(zip, ZipArchiveMode.Read)) {
                        string root = Path.GetFullPath(tmp) + Path.DirectorySeparatorChar;
                        foreach (var entry in archive.Entries) {
                            string path = Path.GetFullPath(Path.Combine(tmp, entry.FullName));
                            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                                throw new InvalidDataException("Invalid kit path " + entry.FullName);
                            if (entry.FullName.EndsWith("/")) { Directory.CreateDirectory(path); continue; }
                            Directory.CreateDirectory(Path.GetDirectoryName(path));
                            entry.ExtractToFile(path);
                            Application.DoEvents();
                        }
                    }
                    try { Directory.Move(tmp, dest); }
                    catch (IOException) { if (!Directory.Exists(dest)) throw; }
                } finally { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); }
            }
            // Earlier versions are superseded; anything still in use stays.
            foreach (var old in Directory.GetDirectories(InstallParent))
                if (!string.Equals(Path.GetFileName(old), id, StringComparison.OrdinalIgnoreCase))
                    try { Directory.Delete(old, true); } catch (Exception) {}
            return dest;
        }
    }

    // A stable copy of this launcher plus a Start menu shortcut to it.
    static void InstallLauncher(string kit) {
        string self = Assembly.GetExecutingAssembly().Location;
        string installed = Path.Combine(InstallParent, "SmashTV.exe");
        try {
            if (!string.Equals(Path.GetFullPath(self), Path.GetFullPath(installed), StringComparison.OrdinalIgnoreCase) &&
                (!File.Exists(installed) || Sha256(installed) != Sha256(self)))
                File.Copy(self, installed, true);
        } catch (IOException) { return; }    // another copy is running; keep it
        catch (UnauthorizedAccessException) { return; }
        if (!string.IsNullOrEmpty(TestRoot)) return;
        try {
            string programs = Environment.GetFolderPath(Environment.SpecialFolder.Programs);
            Directory.CreateDirectory(programs);
            string link = Path.Combine(programs, "Smash T.V..lnk");
            string legacy = Path.Combine(programs, "Play Smash TV.lnk");
            if (File.Exists(legacy)) File.Delete(legacy);
            dynamic shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
            dynamic shortcut = shell.CreateShortcut(link);
            shortcut.TargetPath = installed;
            shortcut.WorkingDirectory = InstallParent;
            shortcut.IconLocation = installed + ",0";
            shortcut.Description = "Smash T.V. native player (your own L8 ROM)";
            shortcut.Save();
        } catch (Exception) {}                // shortcuts are a convenience only
    }

    // ---- build ---------------------------------------------------------------
    static bool Build(string kit, string rom, string message) {
        var start = new ProcessStartInfo(Path.Combine(kit, "python", "python.exe"),
            "-I -B \"" + Path.Combine(kit, "player_setup.py") + "\" --state \"" + State + "\" --rom \"" + rom + "\" --prepare-only") {
            WorkingDirectory = kit,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        using (var progress = new ProgressWindow(message))
        using (var process = new Process { StartInfo = start }) {
            var log = new System.Text.StringBuilder();
            DataReceivedEventHandler collect = (s, e) => { if (e.Data != null) lock (log) log.AppendLine(e.Data); };
            process.OutputDataReceived += collect;
            process.ErrorDataReceived += collect;
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            progress.Show();
            var watch = Stopwatch.StartNew();
            while (!process.WaitForExit(50)) {
                progress.Elapsed(watch.Elapsed);
                Application.DoEvents();
            }
            process.WaitForExit();
            lock (log) File.WriteAllText(Path.Combine(State, "setup.log"), log.ToString());
            return process.ExitCode == 0;
        }
    }

    sealed class ProgressWindow : Form {
        readonly Label status = new Label();
        readonly string text;
        public ProgressWindow(string message) {
            text = message;
            Text = Title;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(420, 96);
            try { Icon = Icon.ExtractAssociatedIcon(Assembly.GetExecutingAssembly().Location); } catch (Exception) {}
            status.SetBounds(16, 14, 388, 36);
            status.Text = message;
            var bar = new ProgressBar { Style = ProgressBarStyle.Marquee, MarqueeAnimationSpeed = 30 };
            bar.SetBounds(16, 56, 388, 20);
            Controls.Add(status);
            Controls.Add(bar);
        }
        public void Elapsed(TimeSpan t) { status.Text = text + "\n" + (int)t.TotalSeconds + " s (first setup takes under a minute)"; }
    }

    // ---- built player record (written by player_setup.py) ---------------------
    sealed class Player {
        public string Kit, Exe, Rom, ExeHash, RomHash;
        public static Player Load(string state) {
            string marker = Path.Combine(state, "player.json");
            if (!File.Exists(marker)) return null;
            string json = File.ReadAllText(marker);
            Func<string, string> field = key => {
                var m = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"([0-9a-fA-F-]+)\"");
                return m.Success ? m.Groups[1].Value.ToLowerInvariant() : null;
            };
            string generation = field("generation");
            if (generation == null || field("kit") == null) return null;
            Guid parsed;
            if (!Guid.TryParse(generation, out parsed)) return null;
            string folder = Path.Combine(state, parsed.ToString());
            return new Player {
                Kit = field("kit"), ExeHash = field("exe"), RomHash = field("rom"),
                Exe = Path.Combine(folder, "SmashTV.exe"), Rom = Path.Combine(folder, "selected-l8.zip")
            };
        }
        public bool RomIntact() { return File.Exists(Rom) && Sha256(Rom) == RomHash; }
        public bool Verify() { return File.Exists(Exe) && Sha256(Exe) == ExeHash && RomIntact(); }
    }
}
