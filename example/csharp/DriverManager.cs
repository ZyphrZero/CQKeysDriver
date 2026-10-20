using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace CqKeys
{
    public class DriverManager : IDisposable
    {
        private const string DevicePath = @"\\.\cqkeys";
        private const uint StatusIoctl = (0x8000u << 16) | (1u << 14) | (0x802u << 2);
        private readonly object sync = new object();
        private SafeFileHandle deviceHandle;
        private bool disposed;

        [StructLayout(LayoutKind.Sequential)]
        private struct DriverStatus
        {
            public uint Size, Version, Flags, QueuedReports, QueueCapacity;
            public int LastSubmitStatus;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DeviceIoControl(
            SafeFileHandle handle, uint code, IntPtr input, uint inputLength,
            out DriverStatus output, uint outputLength, out uint returned, IntPtr overlapped);

        public DriverManager()
        {
            using (var identity = WindowsIdentity.GetCurrent())
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException("需要管理员权限才能连接输入设备。");
        }

        public Task<bool> ConnectAsync()
        {
            lock (sync)
            {
                if (disposed) throw new ObjectDisposedException(nameof(DriverManager));
                if (deviceHandle != null && !deviceHandle.IsInvalid) return Task.FromResult(true);
                var handle = CreateFile(DevicePath, 0xc0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
                try
                {
                    if (handle.IsInvalid)
                        throw new Win32Exception(Marshal.GetLastWin32Error(),
                            "无法连接设备：请先安装 VHF 驱动包，并关闭其他客户端连接");
                    DriverStatus status;
                    uint returned;
                    uint size = (uint)Marshal.SizeOf(typeof(DriverStatus));
                    if (!DeviceIoControl(handle, StatusIoctl, IntPtr.Zero, 0, out status, size, out returned, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "VHF 状态查询失败");
                    if (returned != size || status.Size != size || status.Version != 2)
                        throw new InvalidOperationException("设备协议与客户端不匹配。");
                    if ((status.Flags & 2) == 0)
                        throw new InvalidOperationException($"设备未就绪，VHF 状态：0x{status.LastSubmitStatus:X8}");
                    deviceHandle = handle;
                    handle = null;
                    return Task.FromResult(true);
                }
                finally { handle?.Dispose(); }
            }
        }

        public Task<bool> DisconnectAsync()
        {
            lock (sync)
            {
                deviceHandle?.Dispose();
                deviceHandle = null;
                return Task.FromResult(true);
            }
        }

        public void Dispose()
        {
            lock (sync)
            {
                if (disposed) return;
                deviceHandle?.Dispose();
                deviceHandle = null;
                disposed = true;
            }
        }
    }
}
