using System;
using System.ComponentModel;
using System.Diagnostics;
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
        private bool removePackagePending;
        private bool disposed;
        public bool IsInputReady { get; private set; }
        public bool NeedsCleanup => needsCleanup;

        public sealed class NativeLibraryInfo
        {
            public string Path { get; }
            public bool SupportsPackageRemoval { get; }

            internal NativeLibraryInfo(string path, bool supportsPackageRemoval)
            {
                Path = path;
                SupportsPackageRemoval = supportsPackageRemoval;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct DriverStartResult
        {
            public uint Size;
            public uint Version;
            public uint Error;
            public uint ProblemCode;
            public int ProblemStatus;
            public uint DiagnosticError;
        }

        private static class Native
        {
            [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
            internal static extern IntPtr GetProcAddress(IntPtr module, string name);

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
            internal static extern bool GetLastDriverStartResult(ref DriverStartResult result);

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

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true, CharSet = CharSet.Ansi)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool UnloadNTDriverAndPackage(string name);
        }

        public DriverManager()
        {
            using (var identity = WindowsIdentity.GetCurrent())
            {
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException("需要管理员权限才能操作驱动程序。");
            }
        }

        public static NativeLibraryInfo InspectNativeLibrary()
        {
            uint version = Native.GetLifecycleVersion();
            string expectedPath = Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "cqkeysdll.dll"));
            using (var process = Process.GetCurrentProcess())
            {
                ProcessModule loaded = null;
                foreach (ProcessModule module in process.Modules)
                {
                    if (!string.Equals(module.ModuleName, "cqkeysdll.dll", StringComparison.OrdinalIgnoreCase)) continue;
                    if (loaded != null)
                        throw new InvalidOperationException($"检测到多个同名 DLL，不能确定原生接口来源，请完成清理后重启：{loaded.FileName}；{module.FileName}");
                    loaded = module;
                }
                if (loaded == null) throw new InvalidOperationException("无法查询实际加载的 cqkeysdll.dll，未执行驱动操作。");
                string actualPath = Path.GetFullPath(loaded.FileName);
                if (!string.Equals(actualPath, expectedPath, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException($"DLL 加载路径不匹配。预期: {expectedPath}；实际: {actualPath}。请完成清理后重启。");
                if (version != 2)
                    throw new InvalidOperationException($"需要 ABI v2 DLL，实际版本为 {version}。实际加载 DLL: {actualPath}");
                IntPtr removal = Native.GetProcAddress(loaded.BaseAddress, "UnloadNTDriverAndPackage");
                if (removal == IntPtr.Zero)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != 127)
                        throw CreateDriverException(error, $"检查 Driver Store 删除接口失败，实际加载 DLL: {actualPath}");
                }
                return new NativeLibraryInfo(actualPath, removal != IntPtr.Zero);
            }
        }

        public void RequirePackageRemoval()
        {
            if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
            var library = InspectNativeLibrary();
            if (!library.SupportsPackageRemoval)
                throw new InvalidOperationException("当前 DLL 缺少 UnloadNTDriverAndPackage，未开始卸载或删除包。\n"
                    + $"实际加载 DLL: {library.Path}\n"
                    + "可取消勾选后普通卸载（保留驱动包），完成清理后退出并更新 DLL，再重启程序。不会自动降级卸载。");
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
                    InspectNativeLibrary();
                    needsCleanup = true;
                    if (!Native.LoadNTDriver(driverName, Path.GetFullPath(driverPath)))
                    {
                        int error = Marshal.GetLastWin32Error();
                        throw ReadDriverStartException(error);
                    }
                    if (!Native.SetHandle())
                        throw CreateDriverException(Marshal.GetLastWin32Error(), "SetHandle 连接驱动设备失败");
                    IsInputReady = true;
                    return true;
                });
            }
            finally { operations.Release(); }
        }

        public async Task<bool> StopAndUninstallDriverAsync(string driverName, bool removePackage = false)
        {
            if (driverName != "cqkeys") throw new ArgumentException("驱动名称必须为 cqkeys");
            await operations.WaitAsync();
            try
            {
                if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
                if (removePackage || removePackagePending) RequirePackageRemoval();
                IsInputReady = false;
                removePackagePending |= removePackage;
                await Task.Run(() =>
                {
                    try
                    {
                        bool success = removePackagePending ? Native.UnloadNTDriverAndPackage(driverName)
                                                            : Native.UnloadNTDriver(driverName);
                        if (!success)
                            throw CreateDriverException(Marshal.GetLastWin32Error(), removePackagePending
                                ? "UnloadNTDriverAndPackage 设备或 Driver Store 包清理失败"
                                : "UnloadNTDriver 驱动卸载失败");
                    }
                    catch (EntryPointNotFoundException error)
                    {
                        throw new InvalidOperationException("DLL 缺少所选卸载接口，请更新 cqkeysdll.dll；未退回普通卸载。", error);
                    }
                });
                needsCleanup = false;
                removePackagePending = false;
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
                IsInputReady = false;
                disposed = true;
            }
            finally { operations.Release(); }
        }

        public async Task<string> TestInputAsync(InputTestKind kind, CancellationToken cancellation)
        {
            await operations.WaitAsync(cancellation);
            try
            {
                if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
                if (!IsInputReady) throw new InvalidOperationException("请先成功安装并连接驱动；失败后须先卸载清理。");
                return await Task.Run(() => DriverInputTester.Run(new NativeInputTaskApi(), kind, cancellation));
            }
            catch { IsInputReady = false; throw; }
            finally { operations.Release(); }
        }

        private static Win32Exception ReadDriverStartException(int error)
        {
            var result = new DriverStartResult { Size = (uint)Marshal.SizeOf(typeof(DriverStartResult)), Version = 1 };
            try
            {
                if (!Native.GetLastDriverStartResult(ref result))
                    result.DiagnosticError = unchecked((uint)Marshal.GetLastWin32Error());
            }
            catch (EntryPointNotFoundException)
            {
                string message = CreateDriverException(error, "LoadNTDriver 驱动启动失败").Message;
                return new Win32Exception(error, message + " 当前 DLL 缺少启动诊断接口，请同步更新 cqkeysdll.dll。");
            }
            return CreateDriverStartException(error, result);
        }

        private static Win32Exception CreateDriverStartException(int error, DriverStartResult result)
        {
            string message = CreateDriverException(error, "LoadNTDriver 驱动启动失败").Message;
            if (result.ProblemCode != 0)
            {
                message += $" PnP Problem Code {result.ProblemCode}（0x{result.ProblemCode:X8}）。";
                if (result.DiagnosticError == 0)
                    message += $" Problem Status 0x{result.ProblemStatus:X8}。";
            }
            if (result.DiagnosticError != 0)
            {
                int queryError = unchecked((int)result.DiagnosticError);
                message += $" 设备诊断查询失败（Win32 {queryError}, 0x{queryError:X8}）：{new Win32Exception(queryError).Message}";
            }
            else if (result.ProblemCode != 0 && result.ProblemStatus == unchecked((int)0xC0000603))
            {
                message += " STATUS_IMAGE_CERT_REVOKED：Windows 按吊销策略阻止加载该驱动。请使用有效且符合内核签名策略的完整驱动包；延长等待时间不能解决。";
            }
            return new Win32Exception(error, message);
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
