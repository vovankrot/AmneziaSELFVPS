using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace AmneziaVPN.Installer;

internal static class InstallerServiceState
{
    internal static void Wait(string name,uint wanted,int timeout,bool allowMissing)
    {
        IntPtr manager = OpenSCManager(null,null,1);
        if (manager == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            IntPtr service = OpenService(manager,name,4);
            if (service == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                if (error == 1060 && allowMissing) return;
                throw new Win32Exception(error);
            }
            try
            {
                var timer = Stopwatch.StartNew();
                do
                {
                    if (!QueryServiceStatus(service,out var status)) throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (status.CurrentState == wanted) return;
                    Thread.Sleep(200);
                } while (timer.ElapsedMilliseconds < timeout);
                throw new IOException($"Служба {name} не перешла в ожидаемое состояние. Установка/удаление прерваны до изменения файлов. Возможен зависший запрос драйвера; перезагрузите Windows.");
            }
            finally { CloseServiceHandle(service); }
        }
        finally { CloseServiceHandle(manager); }
    }
    [StructLayout(LayoutKind.Sequential)] struct Status { public uint Type,CurrentState,Controls,Win32Exit,ServiceExit,Checkpoint,WaitHint; }
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenSCManager(string? machine,string? database,uint access);
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenService(IntPtr manager,string name,uint access);
    [DllImport("advapi32.dll",SetLastError=true)] static extern bool QueryServiceStatus(IntPtr service,out Status status);
    [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr handle);
}
