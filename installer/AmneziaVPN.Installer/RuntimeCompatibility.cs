using System.Text.Json;
using System.IO;

namespace AmneziaVPN.Installer;

internal static class RuntimeCompatibility
{
    internal const string ManifestName = "runtime-compatibility.json";
    internal static void Check(string payloadDirectory)
    {
        var path = Path.Combine(payloadDirectory, ManifestName);
        using var json = JsonDocument.Parse(File.ReadAllText(path));
        int minimumBuild = json.RootElement.GetProperty("minimumWindowsBuild").GetInt32();
        Validate(Environment.OSVersion.Version, Environment.Is64BitOperatingSystem, minimumBuild);
    }

    internal static void Validate(Version windows, bool is64Bit, int minimumBuild)
    {
        if (minimumBuild < 17763) throw new IOException("Некорректные требования к Windows в пакете установки.");
        if (!is64Bit || windows.Major < 10 || windows.Build < minimumBuild)
            throw new InvalidOperationException($"Этому пакету нужна 64-битная Windows 10/11, сборка {minimumBuild} или новее. " +
                "Текущая установка и VPN-соединение не изменены.");
    }
}
