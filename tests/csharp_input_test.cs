using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;
using CqKeys;

internal static partial class CSharpLifecycleTest
{
    private sealed class InputApi : IInputTaskApi
    {
        public int LastError { get; private set; }
        public int Submits, Waits, Cancels, Releases;
        public int SubmitError, WaitError, QueryError, ReleaseError, CancelError;
        public Exception CancelFailure;
        public Action Waiting;
        public InputTaskResult Result = new InputTaskResult {
            Size = 96, Version = 1, State = 2, TaskId = 7, CompletedActions = 2, CompletedRepeats = 1,
            LastAcceptedSequence = 12, LastSubmittedSequence = 12, DeviceEpoch = 1, SessionId = 1, SessionPhase = 2
        };
        private bool Outcome(int error) { LastError = error; return error == 0; }
        public bool Submit(InputAction[] actions, out ulong taskId)
        {
            Submits++;
            Check(actions.Length == 2, "Test must submit only its bounded two-action sequence");
            taskId = 7;
            return Outcome(SubmitError);
        }
        public bool Wait(ulong taskId) { Waits++; Waiting?.Invoke(); return Outcome(WaitError); }
        public bool Query(ulong taskId, ref InputTaskResult result)
        {
            Check(result.Size == 96 && result.Version == 1, "Caller did not initialize task result ABI");
            result = Result;
            return Outcome(QueryError);
        }
        public bool Cancel(ulong taskId)
        {
            Cancels++;
            if (CancelFailure != null) throw CancelFailure;
            Result.State = 3; Result.Error = 1223; Result.ResultFlags = 4;
            return Outcome(CancelError);
        }
        public bool Release(ulong taskId) { Releases++; return Outcome(ReleaseError); }
    }

    private static void RunInputTests()
    {
        Console.WriteLine("Input tests use simulated native/UI boundaries; they do not verify HID delivery.");
        Run("input task ABI and safe test sequences", InputLayoutsAndActions);
        Run("completed native task is not claimed as Windows receipt", InputCompletionIsNotReceipt);
        Run("failed submit cannot become a completed task", InputSubmitFailure);
        Run("wait failure retains task for lifecycle cleanup", InputWaitFailure);
        Run("query and release errors are both retained", InputQueryAndReleaseFailure);
        Run("partial completion and cleanup failure reject success", InputResultFailures);
        Run("cancellation requests native cleanup before release", InputCancellation);
        Run("cancellation callback failures retain their real cause", InputCancellationFailure);
        Run("cancel before submit sends no input", InputCancelledBeforeSubmit);
        Run("input buttons require a successful connected session", InputButtonsRequireConnection);
        Run("UI without observed events never claims end-to-end success", InputUiWithoutReceipt);
        Run("UI records preview events separately from DLL completion", InputUiWithReceipt);
        Run("closing during test preparation cancels before native input", InputCloseCancels);
        Run("focus loss during preparation cancels before native input", InputFocusLossCancels);
        Run("modifier keys prevent test submission", InputModifiersBlock);
    }

    private static Exception InputFailure(Action operation, string expected)
    {
        Exception failure = null;
        try { operation(); }
        catch (Exception error) { failure = error; }
        Check(failure != null && failure.ToString().Contains(expected), "Missing expected failure: " + expected);
        return failure;
    }

    private static void InputLayoutsAndActions()
    {
        Check(Marshal.SizeOf(typeof(InputAction)) == 16 && Marshal.SizeOf(typeof(InputTaskResult)) == 96,
            "Input ABI sizes differ from the DLL");
        Check(Marshal.OffsetOf(typeof(InputTaskResult), "TaskId").ToInt32() == 16 &&
            Marshal.OffsetOf(typeof(InputTaskResult), "ResultFlags").ToInt32() == 88, "Input ABI offsets differ");
        InputAction[] keys = DriverInputTester.CreateActions(InputTestKind.Keyboard);
        Check(keys.Length == 2 && keys[0].Type == 1 && keys[1].Type == 2 && keys[0].Value1 == 0x77 &&
            keys[1].Value1 == 0x77 && keys[0].DelayMs == 80, "Keyboard test must release its F8 key");
        InputAction[] mouse = DriverInputTester.CreateActions(InputTestKind.Mouse);
        Check(mouse.Length == 2 && mouse[0].Type == 5 && mouse[1].Type == 5 && mouse[0].Value1 == 24 &&
            mouse[1].Value1 == -24 && mouse[0].Value2 == 0 && mouse[1].Value2 == 0, "Mouse test must be bounded and click-free");
        InputFailure(() => DriverInputTester.CreateActions((InputTestKind)99), "kind");
    }

    private static void InputCompletionIsNotReceipt()
    {
        var api = new InputApi();
        string result = DriverInputTester.Run(api, InputTestKind.Keyboard, CancellationToken.None);
        Check(api.Submits == 1 && api.Waits == 1 && api.Releases == 1 && api.Cancels == 0, "Task lifecycle mismatch");
        Check(result.Contains("2/2") && result.Contains("序号 12") && result.Contains("不等于 Windows"), "Completion evidence was overstated");
    }

    private static void InputSubmitFailure()
    {
        var api = new InputApi { SubmitError = 5 };
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Keyboard, CancellationToken.None), "SubmitInputTask");
        Check(api.Waits == 0 && api.Releases == 0 && api.Cancels == 0, "Failed submit created a fictitious task");
    }

    private static void InputWaitFailure()
    {
        var api = new InputApi { WaitError = 6 };
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, CancellationToken.None), "WaitInputTask");
        Check(api.Cancels == 1 && api.Releases == 0, "Unconfirmed task was released instead of retained for cleanup");
    }

    private static void InputQueryAndReleaseFailure()
    {
        var api = new InputApi { QueryError = 13, ReleaseError = 5 };
        Exception error = InputFailure(() => DriverInputTester.Run(api, InputTestKind.Keyboard, CancellationToken.None), "QueryInputTask");
        Check(error.ToString().Contains("ReleaseInputTask") && api.Releases == 1, "Cleanup failure masked the original query failure");
    }

    private static void InputResultFailures()
    {
        var api = new InputApi(); api.Result.CompletedActions = 1;
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Keyboard, CancellationToken.None), "未完整成功");
        api = new InputApi(); api.Result.CleanupError = 5;
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, CancellationToken.None), "释放未确认");
        api = new InputApi(); api.Result.ResultFlags = 2;
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, CancellationToken.None), "未完整成功");
        api = new InputApi(); api.Result.TaskId = 999;
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, CancellationToken.None), "格式不匹配");
        api = new InputApi(); api.Result.State = 4; api.Result.Error = 5;
        InputFailure(() => DriverInputTester.Run(api, InputTestKind.Keyboard, CancellationToken.None), "0x00000005");
    }

    private static void InputCancellation()
    {
        using (var cancellation = new CancellationTokenSource())
        {
            var api = new InputApi { Waiting = cancellation.Cancel };
            Exception error = InputFailure(() => DriverInputTester.Run(api, InputTestKind.Keyboard, cancellation.Token), "不能撤回");
            Check(error is OperationCanceledException && api.Cancels == 1 && api.Releases == 1, "Cancellation lost task cleanup");
        }
    }

    private static void InputCancellationFailure()
    {
        using (var cancellation = new CancellationTokenSource())
        {
            var api = new InputApi { Waiting = cancellation.Cancel, CancelFailure = new InvalidOperationException("cancel sentinel") };
            InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, cancellation.Token), "cancel sentinel");
            Check(api.Releases == 1, "Callback exception skipped completed task release");
        }
        using (var cancellation = new CancellationTokenSource())
        {
            var api = new InputApi { Waiting = cancellation.Cancel, CancelError = 5 };
            InputFailure(() => DriverInputTester.Run(api, InputTestKind.Mouse, cancellation.Token), "0x00000005");
        }
    }

    private static void InputCancelledBeforeSubmit()
    {
        using (var cancellation = new CancellationTokenSource())
        {
            cancellation.Cancel();
            var api = new InputApi();
            try { DriverInputTester.Run(api, InputTestKind.Keyboard, cancellation.Token); throw new Exception("Cancellation ignored"); }
            catch (OperationCanceledException) { }
            Check(api.Submits == 0, "Already-cancelled test submitted input");
        }
    }

    private static MainWindow ConnectedInputWindow()
    {
        var service = new DriverService();
        typeof(DriverService).GetField("_isDriverInstalled", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(service, true);
        DriverManager.LastCreated.NeedsCleanup = true;
        DriverManager.LastCreated.IsInputReady = true;
        return new MainWindow(service);
    }

    private static Task StartUiInput(MainWindow window)
    {
        return (Task)typeof(MainWindow).GetMethod("RunInputTestAsync", BindingFlags.Instance | BindingFlags.NonPublic)
            .Invoke(window, new object[] { InputTestKind.Keyboard });
    }

    private static void InputButtonsRequireConnection()
    {
        var window = new MainWindow(new DriverService());
        Check(!window.KeyboardTestButton.IsEnabled && !window.MouseTestButton.IsEnabled && !window.CancelTestButton.IsEnabled,
            "Input enabled without connection");
        DriverManager.LastCreated.NeedsCleanup = true;
        StartUiInput(window).GetAwaiter().GetResult();
        Check(DriverManager.LastCreated.TestCalls == 0, "Failed installation was treated as connected");
    }

    private static void InputUiWithoutReceipt()
    {
        MainWindow window = ConnectedInputWindow();
        DriverManager.LastCreated.InputTestOperation = (kind, token) => {
            Check(!window.KeyboardTestButton.IsEnabled && !window.UninstallButton.IsEnabled && window.CancelTestButton.IsEnabled,
                "Concurrent lifecycle operations were not blocked");
            return Task.FromResult("SIMULATED DLL completion; no actual HID input");
        };
        StartUiInput(window).GetAwaiter().GetResult();
        Check(window.InputObservationText.Text.Contains("未观察到") && window.KeyboardTestButton.IsEnabled,
            "UI claimed receipt without any preview events or failed to restore buttons");
    }

    private static void InputUiWithReceipt()
    {
        MainWindow window = ConnectedInputWindow();
        DriverManager.LastCreated.InputTestOperation = (kind, token) => {
            typeof(MainWindow).GetMethod("InputPreviewKeyDown", BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(window, new object[] { window, new KeyEventArgs { Key = Key.F8 } });
            typeof(MainWindow).GetMethod("InputPreviewKeyUp", BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(window, new object[] { window, new KeyEventArgs { Key = Key.F8 } });
            return Task.FromResult("SIMULATED completion and UI events; no real device");
        };
        StartUiInput(window).GetAwaiter().GetResult();
        Check(window.InputObservationText.Text.Contains("来源未经鉴定") && window.InputObservationText.Text.Contains("按下 1 次") &&
            window.InputObservationText.Text.Contains("释放 1 次"), "UI receipt evidence was missing or overstated");
    }

    private static void InputCloseCancels()
    {
        MainWindow window = ConnectedInputWindow();
        Task test = StartUiInput(window);
        window.Close();
        test.GetAwaiter().GetResult();
        Check(!window.IsClosed && DriverManager.LastCreated.TestCalls == 0 && window.UninstallButton.IsEnabled,
            "Close did not cancel preparation and leave a cleanup path");
    }

    private static void InputFocusLossCancels()
    {
        MainWindow window = ConnectedInputWindow();
        Task test = StartUiInput(window);
        window.Deactivate();
        test.GetAwaiter().GetResult();
        Check(DriverManager.LastCreated.TestCalls == 0 && window.InputObservationText.Text.Contains("取消"), "Focus loss allowed new input");
    }

    private static void InputModifiersBlock()
    {
        MainWindow window = ConnectedInputWindow();
        Keyboard.Modifiers = ModifierKeys.Control;
        try { StartUiInput(window).GetAwaiter().GetResult(); }
        finally { Keyboard.Modifiers = ModifierKeys.None; }
        Check(DriverManager.LastCreated.TestCalls == 0, "Test sent a modified shortcut instead of plain F8");
    }
}
