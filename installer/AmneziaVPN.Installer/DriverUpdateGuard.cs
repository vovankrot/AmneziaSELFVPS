using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
namespace AmneziaVPN.Installer;

internal static class DriverUpdateGuard
{
    internal static bool RequiresReboot { get; private set; }
    const string Service = "AmneziaVPNSplitTunnel";
    internal static bool NeedsUpdate(string source, string target) => !File.Exists(target) ||
        !SHA256.HashData(File.ReadAllBytes(source)).SequenceEqual(SHA256.HashData(File.ReadAllBytes(target)));

    // Hooks keep the update ordering testable without touching a live kernel driver.
    internal static void Prepare(bool changed, Func<uint> status, Action reset, Action stop)
    {
        if (!changed || status() == 1) return;
        reset(); // Legacy unload is unsafe until its WFP callouts have been removed.
        stop();
        if (status() != 1) throw new IOException("Драйвер ещё загружен. Перезагрузите Windows и повторите установку.");
    }
    internal static void Prepare(string source, string target)
    {
        if (RequiresReboot) throw new IOException("Перезагрузите Windows перед повторной установкой: предыдущая операция драйвера не завершилась.");
        Prepare(NeedsUpdate(source, target), Status, Reset, Stop);
    }

    static uint Status()
    {
        IntPtr manager = OpenSCManager(null, null, 1);
        if (manager == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            IntPtr service = OpenService(manager, Service, 4);
            if (service == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                if (error == 1060) return 1;
                throw new Win32Exception(error);
            }
            try { if (!QueryServiceStatus(service, out var state)) throw new Win32Exception(Marshal.GetLastWin32Error()); return state.CurrentState; }
            finally { CloseServiceHandle(service); }
        }
        finally { CloseServiceHandle(manager); }
    }
    static void Reset()
    {
        // Worker owns handles/buffers even after a timeout. Never force-unload an
        // old driver with a pending synchronous IRP or release its output memory.
        var work = Task.Run(() =>
        {
            IntPtr handle = CreateFile(@"\\.\MULLVADSPLITTUNNEL", 0xc0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (handle == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr output = Marshal.AllocHGlobal(8);
            try
            {
                ulong State()
                {
                    if (!DeviceIoControl(handle, 0x80000024, IntPtr.Zero, 0, output, 8, out uint size, IntPtr.Zero) || size != 8)
                        throw new IOException("Не удалось прочитать состояние драйвера.");
                    return unchecked((ulong)Marshal.ReadInt64(output));
                }
                if (State() >= 2 && !DeviceIoControl(handle, 0x8000002f, IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                if (State() != 1) throw new IOException("Драйвер не освободил сетевые фильтры.");
            }
            finally { Marshal.FreeHGlobal(output); CloseHandle(handle); }
        });
        if (!work.Wait(TimeSpan.FromSeconds(8))) { RequiresReboot = true; throw new IOException("Драйвер не отвечает. Перезагрузите Windows и повторите установку; файлы не заменены."); }
        work.GetAwaiter().GetResult();
    }
    static void Stop()
    {
        RunSystemTool("sc.exe", ["stop", Service], 15000);
        var timer = Stopwatch.StartNew();
        while (Status() != 1 && timer.ElapsedMilliseconds < 15000) Thread.Sleep(200);
        if (Status() != 1) RequiresReboot = true;
    }
    internal static void StageCatalog(string directory) => RunSystemTool("pnputil.exe",
        ["/add-driver", Path.Combine(directory, "mullvad-split-tunnel.inf")], 60000);
    static void RunSystemTool(string name, string[] args, int timeout)
    {
        var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, name)) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string arg in args) info.ArgumentList.Add(arg);
        using var process = Process.Start(info) ?? throw new IOException("Не удалось запустить " + name);
        var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(timeout)) { if (name == "sc.exe") RequiresReboot = true; process.Kill(); throw new IOException(name + ": превышено время ожидания. Перезагрузите Windows перед повторной установкой."); }
        if (process.ExitCode != 0) throw new IOException(name + ": " + process.ExitCode + " " + stdout.GetAwaiter().GetResult() + stderr.GetAwaiter().GetResult());
    }
    [StructLayout(LayoutKind.Sequential)] struct ServiceStatus { public uint Type, CurrentState, Controls, Win32Exit, SpecificExit, Checkpoint, WaitHint; }
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr OpenSCManager(string? machine, string? database, uint access);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr OpenService(IntPtr manager, string name, uint access);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool QueryServiceStatus(IntPtr service, out ServiceStatus status);
    [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(IntPtr handle, uint code, IntPtr input, uint inputSize, IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
}
