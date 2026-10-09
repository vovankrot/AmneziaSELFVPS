using AmneziaVPN.Installer;

string root = Path.Combine(Path.GetTempPath(), "selfvps-installer-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
void Assert(bool result, string message) { if (!result) throw new Exception(message); }
try
{
    var command = new System.Diagnostics.ProcessStartInfo(Path.Combine(Environment.SystemDirectory,"cmd.exe"));
    command.ArgumentList.Add("/c"); command.ArgumentList.Add("echo stdout & echo stderr 1>&2 & exit /b 17");
    var commandResult = InstallerCommandRunner.Run(command,5000);
    Assert(commandResult.ExitCode==17 && commandResult.StdOut.Contains("stdout") && commandResult.StdErr.Contains("stderr"),"Command result/output lost");
    var blocked = new System.Diagnostics.ProcessStartInfo(Path.Combine(Environment.SystemDirectory,"ping.exe"));
    blocked.ArgumentList.Add("-n");blocked.ArgumentList.Add("20");blocked.ArgumentList.Add("127.0.0.1");
    var deadline = System.Diagnostics.Stopwatch.StartNew();
    bool timedOut = false;
    try { InstallerCommandRunner.Run(blocked,150); } catch (TimeoutException) { timedOut=true; }
    Assert(timedOut && deadline.ElapsedMilliseconds<4000,"Hung command did not respect deadline");
    Console.WriteLine("PASS: command captures both streams and status; stalled command stops within deadline");
    string scanSource = Path.Combine(root, "scan-source"), scanTarget = Path.Combine(root, "scan-target");
    Directory.CreateDirectory(scanSource); Directory.CreateDirectory(scanTarget);
    string scanFile = Path.Combine(scanSource, "helper.exe");
    File.WriteAllText(scanFile, "helper"); File.WriteAllText(Path.Combine(scanTarget, "helper.exe"), "helper");
    var scanLock = new FileStream(scanFile, FileMode.Open, FileAccess.Read, FileShare.None);
    var releaseScan = Task.Run(() => { Thread.Sleep(250); scanLock.Dispose(); });
    using (var scanInstall = new InstallFileTransaction(scanTarget)) { scanInstall.Apply(scanSource); scanInstall.Commit(); }
    releaseScan.GetAwaiter().GetResult();
    Assert(File.ReadAllText(Path.Combine(scanTarget, "helper.exe")) == "helper", "Transient source lock corrupted payload");
    int permanentAttempts = 0;
    try { InstallFileTransaction.RetrySharingViolation<bool>(() => { permanentAttempts++; throw new IOException("permanent"); }); }
    catch (IOException) {}
    Assert(permanentAttempts == 1, "Non-sharing I/O errors must not be retried");
    Console.WriteLine("PASS: transient source locks retry; permanent I/O errors fail immediately");
    RuntimeCompatibility.Validate(new Version(10, 0, 17763), true, 17763);
    RuntimeCompatibility.Validate(new Version(10, 0, 26100), true, 18362);
    foreach (var candidate in new[] { (new Version(10, 0, 17763), true, 18362),
        (new Version(6, 3, 9600), true, 17763), (new Version(10, 0, 26100), false, 17763) })
    {
        bool rejected = false;
        try { RuntimeCompatibility.Validate(candidate.Item1, candidate.Item2, candidate.Item3); }
        catch (InvalidOperationException) { rejected = true; }
        Assert(rejected, "Unsupported runtime was accepted");
    }
    Console.WriteLine("PASS: runtime preflight supports LTSC 1809 without ICU and rejects unsupported OS/architecture");
    int resets = 0, stops = 0; uint status = 4;
    DriverUpdateGuard.Prepare(false, () => status, () => resets++, () => stops++);
    Assert(resets == 0 && stops == 0, "Unchanged driver must remain loaded");
    bool resetFailed = false;
    try { DriverUpdateGuard.Prepare(true, () => status, () => throw new IOException("reset"), () => stops++); }
    catch (IOException) { resetFailed = true; }
    Assert(resetFailed && stops == 0, "Failed reset must prohibit legacy unload");
    DriverUpdateGuard.Prepare(true, () => status, () => resets++, () => { stops++; status = 1; });
    Assert(resets == 1 && stops == 1, "Changed driver must reset before stop");
    bool stillLoaded = false;
    try { DriverUpdateGuard.Prepare(true, () => 4, () => {}, () => {}); } catch (IOException) { stillLoaded = true; }
    Assert(stillLoaded, "Loaded driver must prohibit payload replacement");
    Console.WriteLine("PASS: driver update ordering, reset failure and loaded-state guards");
    string target = Path.Combine(root, "installed"), payload = Path.Combine(root, "payload");
    Directory.CreateDirectory(target); Directory.CreateDirectory(payload);
    File.WriteAllText(Path.Combine(target, "client.exe"), "old");
    File.WriteAllText(Path.Combine(target, "keep.txt"), "preserved");
    File.WriteAllText(Path.Combine(payload, "client.exe"), "new");
    File.WriteAllText(Path.Combine(payload, "added.dll"), "new dll");
    using (var update = new InstallFileTransaction(target)) { update.Apply(payload); update.Rollback(); }
    Assert(File.ReadAllText(Path.Combine(target, "client.exe")) == "old", "Old binary was not restored");
    Assert(!File.Exists(Path.Combine(target, "added.dll")), "New binary survived rollback");
    Assert(File.ReadAllText(Path.Combine(target, "keep.txt")) == "preserved", "Existing file was lost");
    Console.WriteLine("PASS: rollback restores old files, removes newly added files, preserves existing data");
    using (var update = new InstallFileTransaction(target)) { update.Apply(payload); update.Commit(); }
    Assert(File.ReadAllText(Path.Combine(target, "client.exe")) == "new", "New binary was not committed");
    Assert(Directory.GetDirectories(root, ".selfvps-backup-*").Length == 0, "Backup cleanup failed");
    Console.WriteLine("PASS: successful update commits files and removes temporary backup");
    using (var update = new InstallFileTransaction(target)) { update.Apply(payload); }
    Assert(File.ReadAllText(Path.Combine(target, "keep.txt")) == "preserved", "Dispose rollback lost data");
    string fresh = Path.Combine(root, "new-install");
    using (var update = new InstallFileTransaction(fresh)) { update.Apply(payload); update.Rollback(); }
    Assert(!Directory.Exists(fresh), "Failed fresh installation left a partial directory");
    Console.WriteLine("PASS: automatic rollback and failed fresh installation cleanup");
    File.WriteAllText(Path.Combine(target, "zzz-driver.sys"), "loaded driver");
    File.WriteAllText(Path.Combine(payload, "zzz-driver.sys"), "loaded driver");
    using (var update = new InstallFileTransaction(target))
    using (var loaded = new FileStream(Path.Combine(target, "zzz-driver.sys"), FileMode.Open, FileAccess.Read, FileShare.Read))
    {
        update.Apply(payload); update.Commit();
    }
    Console.WriteLine("PASS: unchanged loaded driver is not overwritten");
    File.WriteAllText(Path.Combine(payload, "client.exe"), "different new client");
    File.WriteAllText(Path.Combine(payload, "zzz-driver.sys"), "different driver");
    using (var update = new InstallFileTransaction(target))
    using (var loaded = new FileStream(Path.Combine(target, "zzz-driver.sys"), FileMode.Open, FileAccess.Read, FileShare.Read))
    {
        bool failed = false;
        try { update.Apply(payload); } catch (IOException) { failed = true; }
        Assert(failed, "Changed locked driver was overwritten");
        update.Rollback();
        Assert(File.ReadAllText(Path.Combine(target, "client.exe")) == "new", "Partial update was not rolled back");
    }
    Console.WriteLine("PASS: locked changed driver causes partial-copy rollback, old client remains intact");
    string uninstaller = Path.Combine(target, "uninstall.exe");
    File.WriteAllText(uninstaller, "old uninstaller");
    using (var update = new InstallFileTransaction(target))
    {
        update.TrackWrite(uninstaller); File.WriteAllText(uninstaller, "new uninstaller"); update.Rollback();
    }
    Assert(File.ReadAllText(uninstaller) == "old uninstaller", "Uninstaller did not roll back");
    Console.WriteLine("PASS: generated uninstaller is included in rollback journal");
    bool refused = false;
    try { using var update = new InstallFileTransaction(Path.GetPathRoot(root)!); } catch (InvalidOperationException) { refused = true; }
    Assert(refused, "Filesystem root was accepted");
    Console.WriteLine("PASS: filesystem root rejected");
}
finally { Directory.Delete(root, true); }
