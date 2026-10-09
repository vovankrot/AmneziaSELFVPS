using System.Diagnostics;
using System.IO;

namespace AmneziaVPN.Installer;

internal static class InstallerCommandRunner
{
    internal sealed record Result(int ExitCode,string StdOut,string StdErr);
    internal static Result Run(ProcessStartInfo info,int timeoutMs)
    {
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.WindowStyle = ProcessWindowStyle.Hidden;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        using var process = Process.Start(info) ?? throw new IOException("Не удалось запустить " + Path.GetFileName(info.FileName));
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(timeoutMs))
        {
            try { process.Kill(entireProcessTree:true); } catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
            process.WaitForExit(2000);
            throw new TimeoutException(Path.GetFileName(info.FileName) + ": превышено время ожидания. Файлы установки не должны заменяться, пока служба не остановлена.");
        }
        if (!Task.WhenAll(stdout,stderr).Wait(2000))
            throw new TimeoutException("Не завершилось чтение результата " + Path.GetFileName(info.FileName));
        return new(process.ExitCode,stdout.Result.Trim(),stderr.Result.Trim());
    }
}
