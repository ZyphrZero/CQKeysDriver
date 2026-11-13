using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace CqKeys
{
    // All clients share the native lifecycle implementation: one device handle,
    // input drain, and PnP disable/remove ordering.
    public sealed class DriverManager : IDisposable
    {
        private readonly SemaphoreSlim operations = new SemaphoreSlim(1, 1);
        private bool needsCleanup;
        private bool disposed;
        public bool NeedsCleanup => needsCleanup;

        private static class Native
        {
            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            internal static extern uint GetLifecycleVersion();

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true, CharSet = CharSet.Ansi)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool LoadNTDriver(string name, string path);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool SetHandle();

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true, CharSet = CharSet.Ansi)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool StopNTDriver(string name);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true, CharSet = CharSet.Ansi)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool UnloadNTDriver(string name);
        }

        public DriverManager()
        {
            using (var identity = WindowsIdentity.GetCurrent())
            {
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException("需要管理员权限才能操作驱动程序。");
            }
        }

        public async Task<bool> InstallAndStartDriverAsync(string driverName, string driverPath)
        {
            if (driverName != "cqkeys") throw new ArgumentException("驱动名称必须为 cqkeys");
            if (!File.Exists(driverPath)) throw new FileNotFoundException("找不到驱动文件", driverPath);
            await operations.WaitAsync();
            try
            {
                if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
                if (needsCleanup) throw new InvalidOperationException("请先卸载当前会话");
                return await Task.Run(() =>
                {
                    if (Native.GetLifecycleVersion() != 2)
                        throw new InvalidOperationException("需要 ABI v2 DLL");
                    needsCleanup = true;
                    if (!Native.LoadNTDriver(driverName, Path.GetFullPath(driverPath)))
                    {
                        int error = Marshal.GetLastWin32Error();
                        throw CreateDriverException(error, "LoadNTDriver 驱动启动失败");
                    }
                    if (!Native.SetHandle())
                        throw CreateDriverException(Marshal.GetLastWin32Error(), "SetHandle 连接驱动设备失败");
                    return true;
                });
            }
            finally { operations.Release(); }
        }

        public async Task<bool> StopAndUninstallDriverAsync(string driverName)
        {
            if (driverName != "cqkeys") throw new ArgumentException("驱动名称必须为 cqkeys");
            await operations.WaitAsync();
            try
            {
                if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
                await Task.Run(() =>
                {
                    if (!Native.UnloadNTDriver(driverName))
                        throw CreateDriverException(Marshal.GetLastWin32Error(), "UnloadNTDriver 驱动卸载失败");
                });
                needsCleanup = false;
                return true;
            }
            finally { operations.Release(); }
        }

        public void Dispose()
        {
            // UI awaits uninstall before disposing. Do not hide an unfinished
            // lifecycle operation or perform PnP work from a finalizer.
            if (disposed) return;
            if (!operations.Wait(0)) throw new InvalidOperationException("驱动操作尚未完成");
            try
            {
                if (needsCleanup && !Native.StopNTDriver("cqkeys"))
                    throw CreateDriverException(Marshal.GetLastWin32Error(), "StopNTDriver 驱动停止失败");
                needsCleanup = false;
                disposed = true;
            }
            finally { operations.Release(); }
        }

        private static Win32Exception CreateDriverException(int error, string operation)
        {
            string message = $"{operation}（Win32 {error}, 0x{error:X8}）：{new Win32Exception(error).Message}";
            if (error == 3010)
                message += " 需要重启 Windows 后重试；程序不会自动重启。";
            return new Win32Exception(error, message);
        }
    }
}
