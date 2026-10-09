using System.Security.Cryptography;
using System.IO;
namespace AmneziaVPN.Installer;

// File-only transaction. The caller stops the service before replacing files
// and restarts it only after commit/rollback. Settings live outside this tree.
internal sealed class InstallFileTransaction : IDisposable
{
    private readonly string _target, _backup;
    private readonly bool _existed;
    private bool _modified, _committed, _rollbackFailed;
    private readonly List<string> _changed = new();
    private readonly List<string> _createdDirectories = new();
    public InstallFileTransaction(string target)
    {
        _target = Path.GetFullPath(target);
        if (Path.GetPathRoot(_target)!.TrimEnd(Path.DirectorySeparatorChar) == _target.TrimEnd(Path.DirectorySeparatorChar))
            throw new InvalidOperationException("Installation target cannot be a filesystem root.");
        _backup = Path.Combine(Path.GetDirectoryName(_target)!, ".selfvps-backup-" + Guid.NewGuid().ToString("N"));
        _existed = Directory.Exists(_target);
        if (_existed) CopyTree(_target, _backup);
    }
    public void Apply(string source)
    {
        _modified = true;
        CopyTree(source, _target, path => _changed.Add(path), path => _createdDirectories.Add(path));
    }
    public void Commit() { _committed = true; }
    public void TrackWrite(string path)
    {
        path = Path.GetFullPath(path);
        if (!path.StartsWith(_target.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Tracked file must stay inside the installation directory.");
        _modified = true;
        if (!_changed.Contains(path, StringComparer.OrdinalIgnoreCase)) _changed.Add(path);
    }
    public void Rollback()
    {
        if (!_modified || _committed) return;
        try
        {
            foreach (string path in _changed.AsEnumerable().Reverse())
            {
                string previous = Path.Combine(_backup, Path.GetRelativePath(_target, path));
                if (File.Exists(previous))
                {
                    if (!EqualFiles(previous, path)) RetrySharingViolation(() => { File.Copy(previous, path, true); return true; });
                }
                else if (File.Exists(path)) File.Delete(path);
            }
            foreach (string path in _createdDirectories.OrderByDescending(path => path.Length))
                if (Directory.Exists(path) && !Directory.EnumerateFileSystemEntries(path).Any()) Directory.Delete(path);
            _modified = false;
        }
        catch (Exception ex)
        {
            _rollbackFailed = true;
            throw new IOException($"Could not restore installation. Backup retained at {_backup}.", ex);
        }
    }
    private static bool EqualFiles(string left, string right)
    {
        return RetrySharingViolation(() =>
        {
            if (!File.Exists(right) || new FileInfo(left).Length != new FileInfo(right).Length) return false;
            using var a = File.OpenRead(left); using var b = File.OpenRead(right);
            return SHA256.HashData(a).AsSpan().SequenceEqual(SHA256.HashData(b));
        });
    }
    // Files can be briefly locked by scanners immediately after extraction.
    // Retry only sharing/lock violations; permanent failures still trigger rollback.
    internal static T RetrySharingViolation<T>(Func<T> operation)
    {
        for (int attempt = 0; ; attempt++)
        {
            try { return operation(); }
            catch (IOException ex) when (attempt < 50 && ((ex.HResult & 0xffff) == 32 || (ex.HResult & 0xffff) == 33))
            {
                System.Threading.Thread.Sleep(100);
            }
        }
    }
    internal static void CopyTree(string source, string destination,
        Action<string>? beforeCopy = null, Action<string>? beforeDirectory = null)
    {
        var sourceInfo = new DirectoryInfo(source);
        if ((sourceInfo.Attributes & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Reparse points are not allowed in an installation transaction.");
        if (!Directory.Exists(destination)) { beforeDirectory?.Invoke(destination); Directory.CreateDirectory(destination); }
        foreach (var file in sourceInfo.EnumerateFiles().OrderBy(file => file.Name, StringComparer.OrdinalIgnoreCase))
        {
            if ((file.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Reparse point file refused.");
            string target = Path.Combine(destination, file.Name);
            if (EqualFiles(file.FullName, target)) continue;
            beforeCopy?.Invoke(target);
            RetrySharingViolation(() => file.CopyTo(target, true));
        }
        foreach (var folder in sourceInfo.EnumerateDirectories()) CopyTree(folder.FullName, Path.Combine(destination, folder.Name), beforeCopy, beforeDirectory);
    }
    public void Dispose()
    {
        if (!_committed && _modified) Rollback();
        if (!_rollbackFailed && Directory.Exists(_backup))
        {
            try { Directory.Delete(_backup, true); }
            catch (IOException ex) { System.Diagnostics.Trace.WriteLine($"Backup cleanup deferred: {_backup}: {ex.Message}"); }
            catch (UnauthorizedAccessException ex) { System.Diagnostics.Trace.WriteLine($"Backup cleanup deferred: {_backup}: {ex.Message}"); }
        }
    }
}
