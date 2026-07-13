using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Threading;

namespace Selfvps.BuildStudio;

public partial class MainWindow : Window
{
    private static readonly IBrush Ok = new SolidColorBrush(Color.FromRgb(0x3F, 0xB9, 0x50));
    private static readonly IBrush Bad = new SolidColorBrush(Color.FromRgb(0xD8, 0xA9, 0x6F));
    private static readonly IBrush Sel = new SolidColorBrush(Color.FromRgb(0x3B, 0x82, 0xF6));
    private static readonly IBrush StrokeDim = new SolidColorBrush(Color.FromRgb(0x27, 0x35, 0x48));
    private static readonly IBrush CardSelBg = new SolidColorBrush(Color.FromRgb(0x16, 0x24, 0x38));
    private static readonly IBrush CardBg = new SolidColorBrush(Color.FromRgb(0x18, 0x23, 0x30));

    private readonly ObservableCollection<ComponentVm> _components = new();
    private readonly string? _projectRoot;
    private readonly bool _isWin = OperatingSystem.IsWindows();
    private readonly bool _isMac = OperatingSystem.IsMacOS();
    private readonly HashSet<string> _available;
    private string _target = "";
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        DepsList.ItemsSource = _components;
        _projectRoot = FindProjectRoot();

        // Which targets can be BUILT from this host OS (macOS/iOS need a real Mac).
        _available = _isWin ? new HashSet<string> { "windows", "android", "linux" }   // linux via WSL
                   : _isMac ? new HashSet<string> { "macos" }                          // Xcode/codesign
                   : new HashSet<string> { "linux", "android" };                       // native Linux

        LinuxSub.Text = _isWin ? "через WSL, .bin" : "нативно, .bin";
        DimUnavailableCards();

        if (_projectRoot == null)
        {
            StatusText.Text = "Не найден корень репозитория (build_installer.ps1)";
            AppendLog("[error] build_installer.ps1 not found near the launcher. Put the exe inside the project tree.");
        }
    }

    private static string? FindProjectRoot()
    {
        var dir = Path.GetDirectoryName(Environment.ProcessPath ?? AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir != null; i++)
        {
            if (File.Exists(Path.Combine(dir, "build_installer.ps1")))
                return dir;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    private void DimUnavailableCards()
    {
        foreach (var (card, tag) in Cards())
            card.Opacity = _available.Contains(tag) ? 1.0 : 0.4;
    }

    private IEnumerable<(Border card, string tag)> Cards() => new[]
    {
        (CardWindows, "windows"),
        (CardMac, "macos"),
        (CardAndroid, "android"),
        (CardLinux, "linux"),
    };

    // ================= UI plumbing =================

    private void TitleBar_Pressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) BeginMoveDrag(e);
    }

    private void Minimize_Click(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Close_Click(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => Close();

    private void Card_Pressed(object? sender, PointerPressedEventArgs e)
    {
        if (_busy || sender is not Border b || b.Tag is not string tag) return;
        if (!_available.Contains(tag))
        {
            StatusText.Text = $"«{tag}» нельзя собрать на этой ОС";
            return;
        }
        _target = tag;
        HighlightCards();
        _ = DetectAsync();
    }

    private void HighlightCards()
    {
        foreach (var (card, tag) in Cards())
        {
            bool on = tag == _target;
            card.BorderBrush = on ? Sel : StrokeDim;
            card.BorderThickness = new Avalonia.Thickness(on ? 2 : 1);
            card.Background = on ? CardSelBg : CardBg;
        }
    }

    private void SetBusy(bool busy, string status)
    {
        _busy = busy;
        Busy.IsVisible = busy;
        StatusText.Text = status;
        BtnDetect.IsEnabled = !busy && _target != "" && _projectRoot != null;
        if (busy) { BtnInstall.IsEnabled = false; BtnBuild.IsEnabled = false; }
    }

    private void AppendLog(string line) => Dispatcher.UIThread.Post(() =>
    {
        LogBox.Text = (LogBox.Text ?? "") + line + "\n";
        LogBox.CaretIndex = LogBox.Text.Length;
    });

    private void ClearLog() => Dispatcher.UIThread.Post(() => LogBox.Text = "");

    // ================= actions =================

    private void Detect_Click(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => _ = DetectAsync();
    private void Install_Click(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => _ = InstallAsync();
    private void Build_Click(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => _ = BuildAsync();

    // ---- backend command builders (host-OS aware) ----

    private (string exe, string[] args) DetectCmd(string target)
    {
        if (_isWin)
            return ("powershell.exe", new[]
            {
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                Path.Combine(_projectRoot!, "build_env.ps1"), "-Detect", target, "-Json"
            });
        return ("bash", new[] { Path.Combine(_projectRoot!, "build_env.sh"), "detect", target, "--json" });
    }

    private (string exe, string[] args) InstallCmd(string target)
    {
        if (_isWin)
            return ("powershell.exe", new[]
            {
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                Path.Combine(_projectRoot!, "build_env.ps1"), "-InstallAll", target
            });
        return ("bash", new[] { Path.Combine(_projectRoot!, "build_env.sh"), "install-all", target });
    }

    private (string exe, string[] args) BuildCmd(string target)
    {
        if (_isWin)
        {
            return target switch
            {
                "windows" => ("powershell.exe", new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(_projectRoot!, "build_installer.ps1"), "-SkipAndroidApk", "-NoElevate" }),
                "android" => ("powershell.exe", new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(_projectRoot!, "build_android.ps1"), "-NoElevate" }),
                _ => ("wsl.exe", new[] { "bash", "-lc", $"cd '{WinToWslPath(_projectRoot!)}' && chmod +x deploy/build_linux.sh && ./deploy/build_linux.sh" }),
            };
        }
        // non-Windows host (Linux or macOS) — native build scripts
        return target switch
        {
            "macos" => ("bash", new[] { "-lc", "chmod +x deploy/build_macos.sh && ./deploy/build_macos.sh" }),
            "android" => ("bash", new[] { "-lc", "chmod +x deploy/build_android.sh && ./deploy/build_android.sh" }),
            _ => ("bash", new[] { "-lc", "chmod +x deploy/build_linux.sh && ./deploy/build_linux.sh" }),
        };
    }

    private async Task DetectAsync()
    {
        if (_projectRoot == null || _target == "") return;
        SetBusy(true, $"Проверяю зависимости для «{_target}»…");
        _components.Clear();
        try
        {
            var (exe, args) = DetectCmd(_target);
            var (code, stdout, stderr) = await RunCaptureAsync(exe, args, _projectRoot);
            if (code != 0 && string.IsNullOrWhiteSpace(stdout))
            {
                AppendLog("[detect error] " + stderr.Trim());
                SetBusy(false, "Ошибка проверки — см. журнал");
                return;
            }

            var comps = ParseComponents(stdout);
            int missing = 0, installable = 0;
            foreach (var c in comps)
            {
                if (c.Id != "winget" && !c.Present) missing++;
                if (!c.Present && c.CanAutoInstall) installable++;
                _components.Add(new ComponentVm(c));
            }

            SummaryText.Text = missing == 0 ? "всё готово" : $"нет {missing}";
            bool ready = missing == 0;
            SetBusy(false, ready ? "Готово к сборке" : $"Не хватает компонентов: {missing}");
            BtnInstall.IsEnabled = installable > 0;
            BtnBuild.IsEnabled = ready;
        }
        catch (Exception ex)
        {
            AppendLog("[detect exception] " + ex.Message);
            SetBusy(false, "Ошибка проверки");
        }
    }

    private async Task InstallAsync()
    {
        if (_projectRoot == null || _target == "") return;
        SetBusy(true, "Доустанавливаю недостающее (может занять время)…");
        AppendLog($"=== Установка зависимостей для {_target} ===");
        var (exe, args) = InstallCmd(_target);
        int code = await RunStreamAsync(exe, args, _projectRoot);
        AppendLog(code == 0 ? "=== Установка завершена ===" : $"=== Установщик вернул код {code} ===");
        SetBusy(false, "Перепроверяю…");
        await DetectAsync();
    }

    private async Task BuildAsync()
    {
        if (_projectRoot == null || _target == "") return;
        ClearLog();
        SetBusy(true, $"Собираю «{_target}»…");
        AppendLog($"=== Сборка {_target} ===");
        int code;
        try
        {
            var (exe, args) = BuildCmd(_target);
            code = await RunStreamAsync(exe, args, _projectRoot);
        }
        catch (Exception ex)
        {
            AppendLog("[build exception] " + ex.Message);
            code = -1;
        }
        AppendLog(code == 0 ? "=== Готово. Артефакт в корне проекта. ===" : $"=== Сборка завершилась с кодом {code} ===");
        SetBusy(false, code == 0 ? "Сборка завершена ✓" : "Сборка не удалась — см. журнал");
        BtnDetect.IsEnabled = _target != "" && _projectRoot != null;
        BtnBuild.IsEnabled = true;
    }

    // ================= process runners =================

    private static async Task<(int code, string stdout, string stderr)> RunCaptureAsync(string exe, string[] args, string cwd)
    {
        var psi = MakePsi(exe, args, cwd);
        using var p = new Process { StartInfo = psi };
        var so = new StringBuilder();
        var se = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data != null) so.AppendLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) se.AppendLine(e.Data); };
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        await p.WaitForExitAsync();
        return (p.ExitCode, so.ToString(), se.ToString());
    }

    private async Task<int> RunStreamAsync(string exe, string[] args, string cwd)
    {
        var psi = MakePsi(exe, args, cwd);
        using var p = new Process { StartInfo = psi };
        p.OutputDataReceived += (_, e) => { if (e.Data != null) AppendLog(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) AppendLog(e.Data); };
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        await p.WaitForExitAsync();
        return p.ExitCode;
    }

    private static ProcessStartInfo MakePsi(string exe, string[] args, string cwd)
    {
        var psi = new ProcessStartInfo(exe)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = cwd,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        return psi;
    }

    private static string WinToWslPath(string winPath)
    {
        var full = Path.GetFullPath(winPath);
        var drive = char.ToLowerInvariant(full[0]);
        return $"/mnt/{drive}{full.Substring(2).Replace('\\', '/')}";
    }

    // ================= JSON =================

    private static List<Comp> ParseComponents(string json)
    {
        var s = json.Trim();
        if (s.Length == 0) return new List<Comp>();
        if (s[0] == '{') s = "[" + s + "]";
        var opts = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
        return JsonSerializer.Deserialize<List<Comp>>(s, opts) ?? new List<Comp>();
    }

    internal sealed class Comp
    {
        [JsonPropertyName("id")] public string Id { get; set; } = "";
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("group")] public string Group { get; set; } = "";
        [JsonPropertyName("present")] public bool Present { get; set; }
        [JsonPropertyName("detail")] public string Detail { get; set; } = "";
        [JsonPropertyName("installHint")] public string InstallHint { get; set; } = "";
        [JsonPropertyName("canAutoInstall")] public bool CanAutoInstall { get; set; }
    }

    public sealed class ComponentVm
    {
        public string Id { get; }
        public string Name { get; }
        public string Detail { get; }
        public bool Present { get; }
        public string Glyph => Present ? "✓" : "✕"; // check / cross
        public IBrush StatusBrush => Present ? Ok : Bad;
        public bool DetailVisible => !string.IsNullOrWhiteSpace(Detail);
        public string HintLine => "нужно: " + _hint;
        public bool HintVisible => !Present && !string.IsNullOrWhiteSpace(_hint);

        private readonly string _hint;

        internal ComponentVm(Comp c)
        {
            Id = c.Id; Name = c.Name; Present = c.Present; Detail = c.Detail; _hint = c.InstallHint;
        }
    }
}
