using System.Diagnostics;
using System.IO;

namespace AmneziaVPN.Installer;

internal static class InstallerJournal
{
    internal static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AmneziaSELFVPS","Installer","logs",
        DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Environment.ProcessId + ".log");
    static readonly object Gate = new();
    internal static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
                File.AppendAllText(LogPath,DateTimeOffset.Now.ToString("O") + " " + message + Environment.NewLine);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
