using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;

namespace AmneziaVPN.Installer;

public partial class MainWindow : Window
{
    private const string AppName = "AmneziaVPN";
    private const string AppPublisher = "AmneziaVPN";
    private const string ClientExeName = "AmneziaVPN.exe";
    private const string ServiceName = "AmneziaVPN-service";
    private const string ServiceExeName = "AmneziaVPN-service.exe";
    private const string WireGuardServiceName = "AmneziaWGTunnel$AmneziaVPN";
    private const string DriverServiceName = "AmneziaVPNSplitTunnel";
    private const string DriverFileName = "mullvad-split-tunnel.sys";
    private const string UninstallExeName = "uninstall.exe";
    private const string PayloadResourceSuffix = "payload.zip";
    private const string LaunchBypassMenuText = "Amnezia: Запустить в обход VPN";
    private const string UninstallRegPath = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\AmneziaVPN";
    private const string AppRegistryPath = @"Software\AmneziaVPN.ORG\AmneziaVPN";
    private const string AutoStartRegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Run";

    private static readonly string InstallDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        AppName);

    private static readonly string ClientExePath = Path.Combine(InstallDir, ClientExeName);
    private static readonly string ServiceExePath = Path.Combine(InstallDir, ServiceExeName);
    private static readonly string UninstallerPath = Path.Combine(InstallDir, UninstallExeName);

    private enum Step
    {
        Ready,
        ReadyUninstall,
        Installing,
        Done,
        Uninstalling,
        UninstallDone,
        Error
    }

    private Step _step;
    private readonly bool _isInstalled;
    private readonly bool _hasTraces;

    public MainWindow()
    {
        InitializeComponent();

        InstallPathText.Text = InstallDir;
        VersionTag.Text = "v" + GetDisplayVersion();

        _isInstalled = IsInstalled();
        _hasTraces = HasAmneziaTraces();

        ChkCleanInstall.Visibility = _hasTraces ? Visibility.Visible : Visibility.Collapsed;
        CleanInstallDescription.Visibility = _hasTraces ? Visibility.Visible : Visibility.Collapsed;

        bool forceUninstall = Environment.GetCommandLineArgs().Any(argument =>
            string.Equals(argument, "/uninstall", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(argument, "--uninstall", StringComparison.OrdinalIgnoreCase));

        if (forceUninstall && !_isInstalled)
        {
            ShowError("Установленная копия AmneziaVPN не найдена.");
            return;
        }

        if (_isInstalled || forceUninstall)
        {
            ShowReadyUninstall();
            return;
        }

        ShowReady();
    }

    private void ShowReady()
    {
        _step = Step.Ready;
        SwapVisible(ScreenReady);
        HeaderTitle.Text = "Готов к установке";
        HeaderSubtitle.Text = "Будут установлены клиент, Windows-сервис и сетевые компоненты AmneziaVPN. Для выполнения потребуется подтверждение UAC.";
        InfoComponentsText.Text = "AmneziaVPN.exe, Windows service, split-tunnel driver, протокольные helper-файлы и Qt runtime.";
        InfoRightsText.Text = "Установка в Program Files, регистрация службы и системных интеграций. Требуются права администратора.";
        BtnPrimary.Content = "Установить";
        BtnPrimary.IsEnabled = true;
        BtnSecondary.Content = "Отмена";
        BtnSecondary.Visibility = Visibility.Visible;
        BtnSecondary.IsEnabled = true;
    }

    private void ShowReadyUninstall()
    {
        _step = Step.ReadyUninstall;
        SwapVisible(ScreenReady);
        HeaderTitle.Text = "AmneziaVPN уже установлен";
        HeaderSubtitle.Text = "Текущую установку можно удалить или переустановить поверх существующей без возврата к старому Inno Setup.";
        InfoComponentsText.Text = "Будет использована уже установленная Windows-копия в Program Files. При переустановке staging payload заменит клиент и сервис поверх текущих файлов.";
        InfoRightsText.Text = "Удаление и переустановка выполняются с правами администратора, включая службу и системные ярлыки.";
        BtnPrimary.Content = "Удалить";
        BtnPrimary.IsEnabled = true;
        BtnSecondary.Content = "Переустановить";
        BtnSecondary.Visibility = Visibility.Visible;
        BtnSecondary.IsEnabled = true;
    }

    private void ShowProgress(string status)
    {
        SwapVisible(ScreenProgress);
        ProgressStatus.Text = status;
        ProgressBar.Value = 0;
        BtnPrimary.IsEnabled = false;
        BtnSecondary.IsEnabled = false;
    }

    private void ShowDone(bool uninstalled)
    {
        _step = uninstalled ? Step.UninstallDone : Step.Done;
        SwapVisible(ScreenDone);
        if (uninstalled)
        {
            DoneTitle.Text = "Удалено";
            DoneSubtitle.Text = "AmneziaVPN удалён. Служба, ярлыки и системные интеграции очищены.";
            ChkLaunchAfterInstall.Visibility = Visibility.Collapsed;
        }
        else
        {
            DoneTitle.Text = "Готово";
            DoneSubtitle.Text = $"AmneziaVPN установлен в {InstallDir}. Служба и ярлыки зарегистрированы.";
            ChkLaunchAfterInstall.Visibility = Visibility.Visible;
            ChkLaunchAfterInstall.IsChecked = true;
        }

        BtnPrimary.Content = "Закрыть";
        BtnPrimary.IsEnabled = true;
        BtnSecondary.Visibility = Visibility.Collapsed;
    }

    private void ShowError(string message)
    {
        _step = Step.Error;
        SwapVisible(ScreenError);
        ErrorMessage.Text = message;
        BtnPrimary.Content = "Закрыть";
        BtnPrimary.IsEnabled = true;
        BtnSecondary.Visibility = Visibility.Collapsed;
    }

    private void SwapVisible(FrameworkElement target)
    {
        ScreenReady.Visibility = ReferenceEquals(target, ScreenReady) ? Visibility.Visible : Visibility.Collapsed;
        ScreenProgress.Visibility = ReferenceEquals(target, ScreenProgress) ? Visibility.Visible : Visibility.Collapsed;
        ScreenDone.Visibility = ReferenceEquals(target, ScreenDone) ? Visibility.Visible : Visibility.Collapsed;
        ScreenError.Visibility = ReferenceEquals(target, ScreenError) ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Caption_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
        {
            DragMove();
        }
    }

    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private async void Primary_Click(object sender, RoutedEventArgs e)
    {
        switch (_step)
        {
            case Step.Ready:
                await RunInstallAsync();
                break;
            case Step.ReadyUninstall:
                await RunUninstallAsync();
                break;
            case Step.Done:
                if (ChkLaunchAfterInstall.IsChecked == true)
                {
                    LaunchClient();
                }
                Close();
                break;
            case Step.UninstallDone:
            case Step.Error:
                Close();
                break;
        }
    }

    private async void Secondary_Click(object sender, RoutedEventArgs e)
    {
        switch (_step)
        {
            case Step.Ready:
                Close();
                break;
            case Step.ReadyUninstall:
                await RunInstallAsync();
                break;
            default:
                Close();
                break;
        }
    }

    private async Task RunInstallAsync()
    {
        ShowProgress("Подготовка...");
        _step = Step.Installing;

        bool createStartMenuShortcut = ChkStartMenu.IsChecked == true;
        bool createDesktopShortcut = ChkDesktop.IsChecked == true;
        bool cleanInstall = ChkCleanInstall.IsChecked == true;

        try
        {
            await Task.Run(() => Install(createStartMenuShortcut, createDesktopShortcut, cleanInstall));
            ShowDone(false);
        }
        catch (Exception ex)
        {
            ShowError(FormatExceptionMessage(ex));
        }
    }

    private async Task RunUninstallAsync()
    {
        ShowProgress("Удаление...");
        _step = Step.Uninstalling;

        try
        {
            await Task.Run(Uninstall);
            ShowDone(true);
        }
        catch (Exception ex)
        {
            ShowError(FormatExceptionMessage(ex));
        }
    }

    private void Install(bool createStartMenuShortcut, bool createDesktopShortcut, bool cleanInstall)
    {
        string extractDir = Path.Combine(Path.GetTempPath(), "amnezia-payload-" + Guid.NewGuid().ToString("N"));

        try
        {
            Report(6, "Останавливаем процессы и службы...");
            StopProcessesAndServices();

            if (cleanInstall)
            {
                Report(14, "Очищаем локальные данные...");
                PerformCleanInstallCleanup();
            }

            Report(22, "Распаковываем полезную нагрузку...");
            Directory.CreateDirectory(extractDir);
            ExtractPayload(extractDir);

            Report(38, "Копируем файлы установки...");
            Directory.CreateDirectory(InstallDir);
            CopyPayload(extractDir, InstallDir);

            Report(56, "Готовим удаление и скрипты...");
            CopySelfTo(UninstallerPath);
            RunBundledScript(Path.Combine(InstallDir, "post_install.cmd"), true);

            Report(68, "Обновляем службы и redistributable...");
            InstallVcRedistIfPresent();
            CreateOrUpdateService();
            StartAndConfigureService();

            Report(84, "Создаём ярлыки...");
            if (createStartMenuShortcut)
            {
                CreateStartMenuShortcut();
            }
            else
            {
                RemoveStartMenuShortcut();
            }

            if (createDesktopShortcut)
            {
                CreateDesktopShortcut();
            }
            else
            {
                RemoveDesktopShortcut();
            }

            Report(93, "Регистрируем системные интеграции...");
            WriteLaunchBypassRegistry();
            WriteUninstallEntry();

            Report(100, "Готово.");
        }
        finally
        {
            TryDeleteDirectory(extractDir);
        }
    }

    private void Uninstall()
    {
        Report(10, "Останавливаем процессы и службы...");
        StopProcessesAndServices();

        Report(35, "Удаляем служебные регистрации...");
        RunBundledScript(Path.Combine(InstallDir, "post_uninstall.cmd"), false);
        DeleteLaunchBypassRegistry();
        DeleteUninstallEntry();

        Report(58, "Удаляем ярлыки...");
        RemoveStartMenuShortcut();
        RemoveDesktopShortcut();

        Report(76, "Планируем удаление файлов...");
        ScheduleSelfCleanup();

        Report(100, "Готово.");
    }

    private void Report(int percent, string status)
    {
        Dispatcher.Invoke(() =>
        {
            ProgressBar.Value = percent;
            ProgressStatus.Text = status;
        });
    }

    private static string GetDisplayVersion()
    {
        string? version = typeof(MainWindow).Assembly.GetName().Version?.ToString();
        if (string.IsNullOrWhiteSpace(version))
        {
            return "0.0.0";
        }

        string[] parts = version.Split('.');
        return string.Join('.', parts.Take(Math.Min(parts.Length, 3)));
    }

    private static bool IsInstalled()
    {
        using RegistryKey? uninstallKey = OpenLocalMachineKey(readOnly: true)?.OpenSubKey(UninstallRegPath, false);
        return uninstallKey != null || File.Exists(ClientExePath) || Directory.Exists(InstallDir);
    }

    private static bool HasAmneziaTraces()
    {
        string tempAmneziaDir = Path.Combine(Path.GetTempPath(), AppName);

        return Directory.Exists(InstallDir)
            || Directory.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AmneziaVPN.ORG"))
            || Directory.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AmneziaVPN.ORG"))
            || Directory.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), AppName))
            || Directory.Exists(tempAmneziaDir)
            || Registry.CurrentUser.OpenSubKey(AppRegistryPath, false) != null
            || Registry.CurrentUser.OpenSubKey(AutoStartRegistryPath, false)?.GetValue(AppName) != null;
    }

    private static void PerformCleanInstallCleanup()
    {
        TryDeleteDirectory(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AmneziaVPN.ORG"));
        TryDeleteDirectory(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AmneziaVPN.ORG"));
        TryDeleteDirectory(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), AppName));
        TryDeleteDirectory(Path.Combine(Path.GetTempPath(), AppName));

        Registry.CurrentUser.DeleteSubKeyTree(AppRegistryPath, false);

        using RegistryKey? runKey = Registry.CurrentUser.OpenSubKey(AutoStartRegistryPath, writable: true);
        runKey?.DeleteValue(AppName, false);
    }

    private static void StopProcessesAndServices()
    {
        TryRun("taskkill.exe", $"/F /IM {ClientExeName}");
        Thread.Sleep(400);

        foreach (string helper in new[]
                 {
                     ServiceExeName,
                     "wireguard.exe",
                     "openvpn.exe",
                     "tun2socks.exe",
                     "hysteria.exe",
                     "ck-client.exe",
                     "ss-local.exe"
                 })
        {
            TryRun("taskkill.exe", $"/F /IM {helper}");
        }

        Thread.Sleep(600);

        TryRun("sc.exe", $"failure {ServiceName} reset= 0 actions= ////");
        TryRun("net.exe", $"stop {WireGuardServiceName}");
        TryRun("sc.exe", $"delete {WireGuardServiceName}");
        TryRun("net.exe", $"stop {ServiceName}");
        Thread.Sleep(1500);
        TryRun("taskkill.exe", $"/F /IM {ServiceExeName}");

        WaitForFilesUnlocked();
    }

    private static void WaitForFilesUnlocked()
    {
        string[] watchedFiles =
        {
            ServiceExePath,
            Path.Combine(InstallDir, "tunnel.dll"),
            Path.Combine(InstallDir, "wintun.dll")
        };

        for (int attempt = 0; attempt < 40; attempt++)
        {
            bool allUnlocked = watchedFiles.All(IsFileUnlocked);
            if (allUnlocked)
            {
                return;
            }

            if (attempt == 20)
            {
                TryRun("taskkill.exe", $"/F /IM {ServiceExeName}");
            }

            Thread.Sleep(500);
        }

        throw new InvalidOperationException("Некоторые файлы всё ещё заняты системой. Перезагрузите компьютер и повторите установку.");
    }

    private static bool IsFileUnlocked(string filePath)
    {
        if (!File.Exists(filePath))
        {
            return true;
        }

        string tempPath = filePath + ".unlock_check";

        try
        {
            File.Move(filePath, tempPath, true);
            File.Move(tempPath, filePath, true);
            return true;
        }
        catch
        {
            try
            {
                if (File.Exists(tempPath) && !File.Exists(filePath))
                {
                    File.Move(tempPath, filePath, true);
                }
            }
            catch
            {
            }

            return false;
        }
    }

    private static void ExtractPayload(string destinationDirectory)
    {
        Assembly assembly = Assembly.GetExecutingAssembly();
        string? resourceName = assembly.GetManifestResourceNames()
            .FirstOrDefault(name => name.EndsWith(PayloadResourceSuffix, StringComparison.OrdinalIgnoreCase));

        if (resourceName == null)
        {
            throw new InvalidOperationException("payload.zip не встроен в установщик. Сначала соберите staging и выполните build_installer.ps1.");
        }

        using Stream payloadStream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidOperationException("Не удалось открыть встроенный payload.zip.");
        using ZipArchive archive = new(payloadStream, ZipArchiveMode.Read);
        archive.ExtractToDirectory(destinationDirectory, true);
    }

    private static void CopyPayload(string sourceRoot, string destinationRoot)
    {
        foreach (string sourcePath in Directory.EnumerateFiles(sourceRoot, "*", SearchOption.AllDirectories))
        {
            string relativePath = Path.GetRelativePath(sourceRoot, sourcePath);
            string destinationPath = Path.Combine(destinationRoot, relativePath);
            string? destinationDirectory = Path.GetDirectoryName(destinationPath);
            if (!string.IsNullOrEmpty(destinationDirectory))
            {
                Directory.CreateDirectory(destinationDirectory);
            }

            if (string.Equals(Path.GetFileName(sourcePath), DriverFileName, StringComparison.OrdinalIgnoreCase) && IsDriverRunning())
            {
                continue;
            }

            File.Copy(sourcePath, destinationPath, true);
        }
    }

    private static bool IsDriverRunning()
    {
        CommandResult result = RunCommand("cmd.exe", $"/c sc query {DriverServiceName} | findstr /I \"RUNNING\"");
        return result.ExitCode == 0;
    }

    private static void LaunchClient()
    {
        if (!File.Exists(ClientExePath))
        {
            return;
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = ClientExePath,
            WorkingDirectory = InstallDir,
            UseShellExecute = true
        });
    }

    private static void RunBundledScript(string scriptPath, bool throwOnError)
    {
        if (!File.Exists(scriptPath))
        {
            return;
        }

        CommandResult result = RunCommand("cmd.exe", $"/c \"{scriptPath}\"", InstallDir);
        if (throwOnError && result.ExitCode != 0)
        {
            throw new InvalidOperationException($"Скрипт {Path.GetFileName(scriptPath)} завершился с кодом {result.ExitCode}. {FirstNonEmpty(result.StdErr, result.StdOut)}".Trim());
        }
    }

    private static void InstallVcRedistIfPresent()
    {
        string vcRedistPath = Path.Combine(InstallDir, "vc_redist.x64.exe");
        if (!File.Exists(vcRedistPath))
        {
            return;
        }

        CommandResult result = RunCommand(vcRedistPath, "/install /quiet /norestart", InstallDir);
        // Exit codes that are NOT failures:
        //   0    = installed successfully
        //   3010 = success, reboot required
        //   1641 = success, reboot initiated
        //   1638 = a newer/equal version of this VC++ runtime is already
        //          installed (ERROR_PRODUCT_VERSION) — nothing to do, this is
        //          the most common case on a dev/up-to-date machine and must
        //          not abort the whole installation. by vovankrot
        if (result.ExitCode != 0 && result.ExitCode != 3010 &&
            result.ExitCode != 1641 && result.ExitCode != 1638)
        {
            throw new InvalidOperationException($"Не удалось установить VC++ Redistributable. Код {result.ExitCode}. {FirstNonEmpty(result.StdErr, result.StdOut)}".Trim());
        }
    }

    private static void CreateOrUpdateService()
    {
        CommandResult configResult = RunCommand("sc.exe", $"config {ServiceName} binpath= \"{ServiceExePath}\" start= auto", InstallDir);
        if (configResult.ExitCode == 0)
        {
            RunCommand("sc.exe", $"config {ServiceName} depend= BFE/nsi", InstallDir);
            return;
        }

        CommandResult createResult = RunCommand("sc.exe", $"create {ServiceName} binpath= \"{ServiceExePath}\" start= auto depend= BFE/nsi", InstallDir);
        if (createResult.ExitCode != 0)
        {
            Thread.Sleep(3000);
            createResult = RunCommand("sc.exe", $"create {ServiceName} binpath= \"{ServiceExePath}\" start= auto depend= BFE/nsi", InstallDir);
        }

        if (createResult.ExitCode != 0)
        {
            throw new InvalidOperationException($"Не удалось создать службу {ServiceName}. {FirstNonEmpty(createResult.StdErr, createResult.StdOut)}".Trim());
        }

        RunCommand("sc.exe", $"config {ServiceName} depend= BFE/nsi", InstallDir);
    }

    private static void StartAndConfigureService()
    {
        RunCommand("sc.exe", $"start {ServiceName}", InstallDir);
        RunCommand("sc.exe", $"failure {ServiceName} reset= 100 actions= restart/2000/restart/2000/restart/2000", InstallDir);
    }

    private static void CopySelfTo(string targetPath)
    {
        string selfPath = Process.GetCurrentProcess().MainModule?.FileName
            ?? throw new InvalidOperationException("Не удалось определить путь к текущему установщику.");

        if (string.Equals(Path.GetFullPath(selfPath), Path.GetFullPath(targetPath), StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
        File.Copy(selfPath, targetPath, true);
    }

    private static void WriteLaunchBypassRegistry()
    {
        using RegistryKey? commandKey = Registry.ClassesRoot.CreateSubKey(@"*\shell\AmneziaLaunchBypass\command");
        using RegistryKey? verbKey = Registry.ClassesRoot.CreateSubKey(@"*\shell\AmneziaLaunchBypass");

        if (commandKey == null || verbKey == null)
        {
            throw new InvalidOperationException("Не удалось записать контекстное меню проводника.");
        }

        verbKey.SetValue(string.Empty, LaunchBypassMenuText, RegistryValueKind.String);
        verbKey.SetValue("Icon", ClientExePath + ",0", RegistryValueKind.String);
        commandKey.SetValue(string.Empty, $"\"{ClientExePath}\" --launch-bypassed \"%1\"", RegistryValueKind.String);
    }

    private static void DeleteLaunchBypassRegistry()
    {
        Registry.ClassesRoot.DeleteSubKeyTree(@"*\shell\AmneziaLaunchBypass", false);
    }

    private static void WriteUninstallEntry()
    {
        using RegistryKey? uninstallKey = OpenLocalMachineKey(readOnly: false)?.CreateSubKey(UninstallRegPath, true);
        if (uninstallKey == null)
        {
            throw new InvalidOperationException("Не удалось создать запись удаления программы.");
        }

        uninstallKey.SetValue("DisplayName", AppName);
        uninstallKey.SetValue("DisplayVersion", GetDisplayVersion());
        uninstallKey.SetValue("Publisher", AppPublisher);
        uninstallKey.SetValue("InstallLocation", InstallDir);
        uninstallKey.SetValue("DisplayIcon", ClientExePath);
        uninstallKey.SetValue("UninstallString", $"\"{UninstallerPath}\" /uninstall");
        uninstallKey.SetValue("NoModify", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        uninstallKey.SetValue("EstimatedSize", GetDirectorySizeKb(InstallDir), RegistryValueKind.DWord);
    }

    private static void DeleteUninstallEntry()
    {
        OpenLocalMachineKey(readOnly: false)?.DeleteSubKeyTree(UninstallRegPath, false);
    }

    private static RegistryKey? OpenLocalMachineKey(bool readOnly)
    {
        return RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
    }

    private static int GetDirectorySizeKb(string directoryPath)
    {
        if (!Directory.Exists(directoryPath))
        {
            return 0;
        }

        long totalSize = Directory.EnumerateFiles(directoryPath, "*", SearchOption.AllDirectories)
            .Select(path => new FileInfo(path).Length)
            .Sum();

        return (int)(totalSize / 1024);
    }

    private static string StartMenuDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
        AppName);

    private static string DesktopShortcutPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory),
        AppName + ".lnk");

    private static void CreateStartMenuShortcut()
    {
        Directory.CreateDirectory(StartMenuDirectory);
        string applicationLink = Path.Combine(StartMenuDirectory, AppName + ".lnk");
        string uninstallLink = Path.Combine(StartMenuDirectory, "Удалить AmneziaVPN.lnk");

        CreateShortcut(applicationLink, ClientExePath, string.Empty, InstallDir, "Запустить AmneziaVPN", ClientExePath);
        CreateShortcut(uninstallLink, UninstallerPath, "/uninstall", InstallDir, "Удалить AmneziaVPN", ClientExePath);
    }

    private static void RemoveStartMenuShortcut()
    {
        TryDeleteDirectory(StartMenuDirectory);
    }

    private static void CreateDesktopShortcut()
    {
        CreateShortcut(DesktopShortcutPath, ClientExePath, string.Empty, InstallDir, "Запустить AmneziaVPN", ClientExePath);
    }

    private static void RemoveDesktopShortcut()
    {
        try
        {
            if (File.Exists(DesktopShortcutPath))
            {
                File.Delete(DesktopShortcutPath);
            }
        }
        catch
        {
        }
    }

    private static void CreateShortcut(string linkPath, string targetPath, string arguments, string workingDirectory, string description, string iconPath)
    {
        Type? shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType == null)
        {
            return;
        }

        dynamic shell = Activator.CreateInstance(shellType)!;
        dynamic shortcut = shell.CreateShortcut(linkPath);
        shortcut.TargetPath = targetPath;
        shortcut.Arguments = arguments;
        shortcut.WorkingDirectory = workingDirectory;
        shortcut.Description = description;
        shortcut.IconLocation = iconPath + ",0";
        shortcut.Save();
    }

    private static void ScheduleSelfCleanup()
    {
        string cleanupScript = Path.Combine(Path.GetTempPath(), "amnezia-cleanup-" + Guid.NewGuid().ToString("N") + ".cmd");
        int currentProcessId = Process.GetCurrentProcess().Id;

        string scriptContent =
            "@echo off\r\n" +
            "setlocal\r\n" +
            ":waitloop\r\n" +
            $"tasklist /FI \"PID eq {currentProcessId}\" 2>nul | find \"{currentProcessId}\" >nul\r\n" +
            "if not errorlevel 1 (\r\n" +
            "  ping -n 2 127.0.0.1 >nul\r\n" +
            "  goto waitloop\r\n" +
            ")\r\n" +
            $"for /l %%i in (1,1,10) do (rmdir /s /q \"{InstallDir}\" >nul 2>&1 & if not exist \"{InstallDir}\" goto done & ping -n 2 127.0.0.1 >nul)\r\n" +
            ":done\r\n" +
            "del /f /q \"%~f0\" >nul 2>&1\r\n";

        File.WriteAllText(cleanupScript, scriptContent);

        Process.Start(new ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = $"/c \"{cleanupScript}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        });
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, true);
            }
        }
        catch
        {
        }
    }

    private static void TryRun(string fileName, string arguments)
    {
        RunCommand(fileName, arguments, InstallDir);
    }

    private static CommandResult RunCommand(string fileName, string arguments, string? workingDirectory = null)
    {
        using Process process = new();
        process.StartInfo = new ProcessStartInfo
        {
            FileName = fileName,
            Arguments = arguments,
            WorkingDirectory = workingDirectory ?? Environment.CurrentDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        };

        process.Start();
        string stdOut = process.StandardOutput.ReadToEnd();
        string stdErr = process.StandardError.ReadToEnd();
        process.WaitForExit();

        return new CommandResult(process.ExitCode, stdOut.Trim(), stdErr.Trim());
    }

    private static string FormatExceptionMessage(Exception ex)
    {
        string message = ex.Message.Trim();
        if (ex.InnerException == null)
        {
            return message;
        }

        string innerMessage = ex.InnerException.Message.Trim();
        if (string.IsNullOrEmpty(innerMessage) || string.Equals(message, innerMessage, StringComparison.Ordinal))
        {
            return message;
        }

        return message + Environment.NewLine + innerMessage;
    }

    private static string FirstNonEmpty(params string[] values)
    {
        foreach (string value in values)
        {
            if (!string.IsNullOrWhiteSpace(value))
            {
                return value;
            }
        }

        return string.Empty;
    }

    private sealed record CommandResult(int ExitCode, string StdOut, string StdErr);
}