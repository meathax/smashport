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
using System.Linq;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

internal static class PlayerSetup {
    const string Title = "Smash T.V.";
    const int ExitUsage = 64;
    const int ExitPrepare = 65;
    const int ExitPlayer = 66;
    const int ExitLaunch = 67;
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
            if (IsRecompLauncher()) {
                Console.Error.WriteLine(e.ToString());
                return Array.IndexOf(args, "--prepare") >= 0 ? ExitPrepare : ExitLaunch;
            }
            MessageBox.Show(e.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    static bool IsRecompLauncher() {
        return Environment.GetEnvironmentVariable("RECOMPLAUNCHER") == "1";
    }

    static int Run(string[] args) {
        if (IsRecompLauncher()) return RunRecompLauncher(args);
        string kit = ExtractKit(InstallParent);
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

    static int RunRecompLauncher(string[] args) {
        bool prepare = Array.IndexOf(args, "--prepare") >= 0;
        bool play = Array.IndexOf(args, "--play") >= 0;
        string data = Argument(args, "--data") ??
                      Environment.GetEnvironmentVariable("RECOMPLAUNCHER_DATA");
        if (string.IsNullOrEmpty(data) || (prepare == play)) return ExitUsage;
        data = Path.GetFullPath(data);
        Directory.CreateDirectory(data);
        string state = Path.Combine(data, "native-player");
        string kitRoot = Path.Combine(data, "cache", "kits");
        Directory.CreateDirectory(state);
        string kit = ExtractKit(kitRoot, false);
        if (prepare) {
            string rom = Argument(args, "--rom") ?? Argument(args, "--game");
            if (string.IsNullOrEmpty(rom) || !File.Exists(rom)) {
                Console.Error.WriteLine("The complete retail L8 ROM ZIP was not selected.");
                return ExitUsage;
            }
            return BuildHeadless(kit, state, Path.GetFullPath(rom)) ? 0 : ExitPrepare;
        }
        var player = Player.Load(state);
        if (player == null || !player.Verify()) {
            Console.Error.WriteLine("The prepared Smash T.V. player is missing or incomplete.");
            return ExitPlayer;
        }
        return LaunchAndWait(player, data, PassThrough(args));
    }

    static string Argument(string[] args, string name) {
        for (int i = 0; i < args.Length; ++i) {
            if (args[i].StartsWith(name + "=", StringComparison.Ordinal)) return args[i].Substring(name.Length + 1);
            if (args[i] == name && i + 1 < args.Length) return args[i + 1];
        }
        return null;
    }

    static string[] PassThrough(string[] args) {
        var result = new System.Collections.Generic.List<string>();
        for (int i = 0; i < args.Length; ++i) {
            string arg = args[i];
            if (arg == "--play" || arg == "--prepare") continue;
            if (arg == "--data" || arg == "--rom" || arg == "--game") { ++i; continue; }
            if (arg.StartsWith("--data=", StringComparison.Ordinal) ||
                arg.StartsWith("--rom=", StringComparison.Ordinal) ||
                arg.StartsWith("--game=", StringComparison.Ordinal)) continue;
            result.Add(arg);
        }
        return result.ToArray();
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

    static int LaunchAndWait(Player player, string data, string[] arguments) {
        var start = new ProcessStartInfo(player.Exe) {
            WorkingDirectory = Path.GetDirectoryName(player.Exe),
            UseShellExecute = false
        };
        start.Arguments = QuoteArguments(new[] { "--data=" + data }.Concat(arguments));
        start.EnvironmentVariables["STV_HOST_ROM_ZIP"] = player.Rom;
        start.EnvironmentVariables["STV_HOST_DATA"] = data;
        using (var process = Process.Start(start)) {
            if (process == null) return ExitLaunch;
            process.WaitForExit();
            return process.ExitCode;
        }
    }

    static string QuoteArguments(System.Collections.Generic.IEnumerable<string> arguments) {
        return string.Join(" ", arguments.Select(arg =>
            arg.IndexOfAny(new[] {' ', '\t', '"'}) >= 0
                ? "\"" + arg.Replace("\"", "\\\"") + "\""
                : arg));
    }

    // ---- kit ---------------------------------------------------------------
    static string LongPath(string path) {
        if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) return path;
        if (path.StartsWith(@"\\", StringComparison.Ordinal))
            return @"\\?\UNC\" + path.Substring(2);
        return @"\\?\" + path;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateDirectoryW(string path, IntPtr security);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string path);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string source, string destination, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    const uint InvalidFileAttributes = 0xffffffff;
    const uint GenericWrite = 0x40000000;
    const uint CreateAlways = 2;
    const uint FileAttributeNormal = 0x80;
    static bool WinExists(string path) {
        return GetFileAttributesW(LongPath(path)) != InvalidFileAttributes;
    }

    static string ParentPath(string path) {
        int slash = Math.Max(path.LastIndexOf('\\'), path.LastIndexOf('/'));
        return slash > 2 ? path.Substring(0, slash) : null;
    }

    static void WinCreateDirectory(string path) {
        if (WinExists(path)) return;
        string parent = ParentPath(path);
        if (!string.IsNullOrEmpty(parent) && !WinExists(parent)) WinCreateDirectory(parent);
        if (!CreateDirectoryW(LongPath(path), IntPtr.Zero) &&
            Marshal.GetLastWin32Error() != 183)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create " + path);
    }

    static Stream WinCreateFile(string path) {
        SafeFileHandle handle = CreateFileW(LongPath(path), GenericWrite, 0, IntPtr.Zero,
                                            CreateAlways, FileAttributeNormal, IntPtr.Zero);
        if (handle.IsInvalid)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create " + path);
        return new FileStream(handle, FileAccess.Write, 65536, false);
    }

    static string KitMemberPath(string root, string member) {
        if (member.Length == 0 || member.IndexOf(':') >= 0 ||
            member.StartsWith("/", StringComparison.Ordinal) ||
            member.StartsWith(@"\", StringComparison.Ordinal))
            throw new InvalidDataException("Invalid kit path " + member);
        string relative = member.Replace('/', '\\');
        foreach (string part in relative.Split('\\'))
            if (part == "." || part == "..")
                throw new InvalidDataException("Invalid kit path " + member);
        return Path.Combine(root, relative);
    }

    static string ExtractKit(string installParent, bool showProgress = true) {
        using (Stream zip = Assembly.GetExecutingAssembly().GetManifestResourceStream("player.zip")) {
            if (zip == null) throw new InvalidDataException("This SmashTV.exe has no player kit.");
            string id;
            using (var sha = SHA256.Create()) id = BitConverter.ToString(sha.ComputeHash(zip)).Replace("-", "").Substring(0, 24);
            string dest = Path.Combine(installParent, id);
            if (WinExists(Path.Combine(dest, "manifest.json"))) return dest;
            WinCreateDirectory(installParent);
            zip.Position = 0;
            string tmp = Path.Combine(installParent, "tmp-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tmp);
            using (var progress = showProgress ? new ProgressWindow("Installing Smash T.V....") : null) {
                if (progress != null) progress.Show();
                try {
                    using (var archive = new ZipArchive(zip, ZipArchiveMode.Read)) {
                        foreach (var entry in archive.Entries) {
                            string path = KitMemberPath(tmp, entry.FullName);
                            if (entry.FullName.EndsWith("/")) {
                                WinCreateDirectory(path);
                                continue;
                            }
                            WinCreateDirectory(ParentPath(path));
                            using (Stream input = entry.Open())
                            using (Stream output = WinCreateFile(path))
                                input.CopyTo(output);
                            Application.DoEvents();
                        }
                    }
                    if (!MoveFileExW(LongPath(tmp), LongPath(dest), 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not finish extracting the player kit");
                } finally {
                    try { Directory.Delete(tmp, true); } catch (Exception) {}
                }
            }
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
        start.EnvironmentVariables["PYTHONHOME"] = kit;
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

    static bool BuildHeadless(string kit, string state, string rom) {
        Console.WriteLine("progress 0.05 Verifying the selected retail L8 ROM set");
        var start = new ProcessStartInfo(Path.Combine(kit, "python", "python.exe"),
            "-I -B \"" + Path.Combine(kit, "player_setup.py") + "\" --state \"" + state +
            "\" --rom \"" + rom + "\" --prepare-only") {
            WorkingDirectory = kit,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.EnvironmentVariables["PYTHONHOME"] = kit;
        using (var process = new Process { StartInfo = start }) {
            var log = new System.Text.StringBuilder();
            DataReceivedEventHandler collect = (s, e) => {
                if (e.Data == null) return;
                lock (log) log.AppendLine(e.Data);
                Console.WriteLine(e.Data);
            };
            process.OutputDataReceived += collect;
            process.ErrorDataReceived += collect;
            if (!process.Start()) return false;
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            string logFolder = Path.Combine(Path.GetDirectoryName(state), "logs");
            Directory.CreateDirectory(logFolder);
            lock (log) File.WriteAllText(Path.Combine(logFolder, "setup.log"), log.ToString());
            if (process.ExitCode == 0) Console.WriteLine("progress 1.0 Smash T.V. is ready to play");
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
