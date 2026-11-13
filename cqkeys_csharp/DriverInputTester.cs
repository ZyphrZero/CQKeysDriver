using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

namespace CqKeys
{
    public enum InputTestKind { Keyboard, Mouse }

    [StructLayout(LayoutKind.Sequential)]
    internal struct InputAction
    {
        public uint Type;
        public int Value1;
        public int Value2;
        public uint DelayMs;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct InputTaskResult
    {
        public uint Size, Version, State, Error;
        public ulong TaskId, CompletedActions;
        public uint CompletedRepeats, CleanupError;
        public ulong LastAcceptedSequence, LastSubmittedSequence, DeviceEpoch, SessionId;
        public uint SessionPhase, PendingCleanup;
        public int InputStatus, CleanupStatus;
        public uint ResultFlags, Reserved;
    }

    internal interface IInputTaskApi
    {
        int LastError { get; }
        bool Submit(InputAction[] actions, out ulong taskId);
        bool Wait(ulong taskId);
        bool Query(ulong taskId, ref InputTaskResult result);
        bool Cancel(ulong taskId);
        bool Release(ulong taskId);
    }

    internal static class DriverInputTester
    {
        internal static InputAction[] CreateActions(InputTestKind kind)
        {
            switch (kind)
            {
                case InputTestKind.Keyboard:
                    return new[] {
                        new InputAction { Type = 1, Value1 = 0x77, DelayMs = 80 },
                        new InputAction { Type = 2, Value1 = 0x77 }
                    };
                case InputTestKind.Mouse:
                    return new[] {
                        new InputAction { Type = 5, Value1 = 24, DelayMs = 150 },
                        new InputAction { Type = 5, Value1 = -24 }
                    };
                default:
                    throw new ArgumentOutOfRangeException(nameof(kind));
            }
        }

        internal static string Run(IInputTaskApi api, InputTestKind kind, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            InputAction[] actions = CreateActions(kind);
            ulong taskId;
            if (!api.Submit(actions, out taskId)) throw ApiError(api, "SubmitInputTask");
            var result = new InputTaskResult { Size = (uint)Marshal.SizeOf(typeof(InputTaskResult)), Version = 1 };
            bool finished = false;
            Exception cancellationFailure = null;
            Exception failure = null;
            try
            {
                using (cancellation.Register(() =>
                {
                    try
                    {
                        if (!api.Cancel(taskId)) throw ApiError(api, "CancelInputTask");
                    }
                    catch (Exception error) { Interlocked.CompareExchange(ref cancellationFailure, error, null); }
                }))
                {
                    if (!api.Wait(taskId)) throw ApiError(api, "WaitInputTask（任务结束未确认，请卸载清理）");
                    finished = true;
                    if (!api.Query(taskId, ref result)) throw ApiError(api, "QueryInputTask");
                }
            }
            catch (Exception error) { failure = error; }
            try
            {
                if (finished)
                {
                    if (!api.Release(taskId)) throw ApiError(api, "ReleaseInputTask");
                }
                else if (!api.Cancel(taskId)) throw ApiError(api, "CancelInputTask（需卸载清理）");
            }
            catch (Exception error) { failure = Combine(failure, error); }
            if (cancellationFailure != null) failure = Combine(failure, cancellationFailure);
            if (failure != null) throw failure;

            string details = $"任务 {taskId}，已完成动作 {result.CompletedActions}/{actions.Length}，最后提交序号 {result.LastSubmittedSequence}，" +
                $"错误 0x{result.Error:X8}，清理错误 0x{result.CleanupError:X8}，输入状态 0x{result.InputStatus:X8}，清理状态 0x{result.CleanupStatus:X8}，标志 0x{result.ResultFlags:X8}";
            if (result.Size != Marshal.SizeOf(typeof(InputTaskResult)) || result.Version != 1 || result.TaskId != taskId || result.Reserved != 0)
                throw new InvalidOperationException("输入任务结果格式不匹配。" + details);
            if (result.CleanupError != 0 || result.PendingCleanup != 0 || result.CleanupStatus < 0 || result.InputStatus < 0)
                throw new InvalidOperationException("输入或释放未确认完成，请卸载清理。" + details);
            if (result.State == 3)
                throw new OperationCanceledException("输入测试已取消；已提交的输入不能撤回。" + details, cancellation);
            if (result.State != 2 || result.Error != 0 || result.ResultFlags != 0 || result.TaskId != taskId ||
                result.CompletedActions != (ulong)actions.Length || result.CompletedRepeats != 1 ||
                result.SessionPhase != 2 || result.DeviceEpoch == 0 || result.SessionId == 0 || result.LastSubmittedSequence == 0)
                throw new InvalidOperationException("输入测试未完整成功。" + details);
            return "DLL 输入任务完成；" + details + "。这不等于 Windows 或目标应用已收到，请核对接收区。";
        }

        private static Exception Combine(Exception first, Exception next)
        {
            return first == null ? next : new AggregateException(first, next);
        }

        private static Win32Exception ApiError(IInputTaskApi api, string operation)
        {
            int error = api.LastError;
            return new Win32Exception(error, $"{operation} 失败（Win32 {error}, 0x{error:X8}）：{new Win32Exception(error).Message}");
        }
    }

    internal sealed class NativeInputTaskApi : IInputTaskApi
    {
        public int LastError => Marshal.GetLastWin32Error();
        public bool Submit(InputAction[] actions, out ulong taskId) => Native.SubmitInputTask(actions, (uint)actions.Length, 1, 0, out taskId);
        public bool Wait(ulong taskId) => Native.WaitInputTask(taskId, uint.MaxValue);
        public bool Query(ulong taskId, ref InputTaskResult result) => Native.QueryInputTask(taskId, ref result);
        public bool Cancel(ulong taskId) => Native.CancelInputTask(taskId);
        public bool Release(ulong taskId) => Native.ReleaseInputTask(taskId);

        private static class Native
        {
            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool SubmitInputTask([In] InputAction[] actions, uint count, uint repeats, uint repeatDelayMs, out ulong taskId);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool WaitInputTask(ulong taskId, uint timeoutMs);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool QueryInputTask(ulong taskId, ref InputTaskResult result);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool CancelInputTask(ulong taskId);

            [DllImport("cqkeysdll.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]
            [return: MarshalAs(UnmanagedType.Bool)]
            internal static extern bool ReleaseInputTask(ulong taskId);
        }
    }
}
