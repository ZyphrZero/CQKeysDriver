extern alias ProductionApp;

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using CqKeys;

internal static class CSharpLifecycleTest
{
    private static int failures;
    private static int executed;

    private static int Main()
    {
        Console.WriteLine("SAFE HARNESS: real App/MainWindow/DriverService source; simulated UI and DriverManager boundaries.");
        Console.WriteLine("Only the production DriverManager static error formatter is reflected; no real windows or Native entry points.");
        Run("cold close is deferred and cannot reenter", ColdCloseIsDeferred);
        Run("close after explicit uninstall is deferred", CloseAfterUninstallIsDeferred);
        Run("uninstall failure preserves retry state", UninstallFailureCanRetry);
        Run("dispatcher enqueue failure preserves retry state", EnqueueFailureCanRetry);
        Run("deferred close exception preserves retry state", DeferredCloseFailureCanRetry);
        Run("pending cleanup blocks close and supports failure retry", PendingCleanupCanRetry);
        Run("cancelled final close resets its authorization", CancelledFinalCloseCanRetry);
        Run("startup access denial is explained and exits", StartupAccessDenied);
        Run("preflight reports every missing file after subscription", PreflightReportsAllMissingFiles);
        foreach (string missingFile in new[] { "cqkeys.inf", "cqkeys.sys", "cqkeys.cat", "cqkeysdll.dll" })
            Run("preflight blocks missing " + missingFile, () => PreflightBlocksMissingFile(missingFile));
        Run("complete file set is not claimed to be a validated installation", CompleteFileSetHasNoWarning);
        Run("service dispose is idempotent and retryable", DisposeCanRetry);
        Run("Win32 errors retain operation, code and system description", ErrorMessagesPreserveDetails);
        Run("3010 explains restart without restarting", RebootErrorIsExplicit);
        Console.WriteLine($"{executed - failures}/{executed} passed; simulated boundaries are not WPF or device acceptance tests.");
        return failures == 0 ? 0 : 1;
    }

    private static void Run(string name, Action test)
    {
        executed++;
        MessageBox.Messages.Clear();
        DriverManager.DenyConstruction = false;
        DriverManager.LastCreated = null;
        try
        {
            test();
            Console.WriteLine("PASS " + name);
        }
        catch (Exception error)
        {
            failures++;
            Console.WriteLine("FAIL " + name + ": " + error.Message);
        }
        finally { DriverManager.DenyConstruction = false; }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void CheckQueuedClose(MainWindow window)
    {
        Check(window.ReentrantCloseCount == 0, "Close reentered its closing callback");
        Check(!window.IsClosed && window.Dispatcher.PendingCount == 1, "Final close must be queued exactly once");
        Check(!window.InstallButton.IsEnabled && !window.UninstallButton.IsEnabled, "Buttons must stay disabled until final close");
        window.Close();
        Check(window.Dispatcher.PendingCount == 1, "Repeated close queued duplicate cleanup");
        window.Dispatcher.Drain();
        Check(window.IsClosed, "Queued close did not complete");
        Check(DriverManager.LastCreated.DisposeCalls == 1, "Cleanup should dispose once after close");
    }

    private static void ColdCloseIsDeferred()
    {
        var service = new DriverService();
        var window = new MainWindow(service);
        window.Close();
        CheckQueuedClose(window);
        Check(MessageBox.Messages.Count == 0, "Cold close was reported as an uninstall failure");
    }

    private static void CloseAfterUninstallIsDeferred()
    {
        var service = new DriverService();
        DriverManager.LastCreated.NeedsCleanup = true;
        service.UninstallDriverAsync().GetAwaiter().GetResult();
        var window = new MainWindow(service);
        window.Close();
        CheckQueuedClose(window);
        Check(DriverManager.LastCreated.UninstallCalls == 1, "A clean session was uninstalled twice");
    }

    private static void UninstallFailureCanRetry()
    {
        var service = new DriverService();
        DriverManager manager = DriverManager.LastCreated;
        manager.NeedsCleanup = true;
        manager.UninstallFailure = new InvalidOperationException("cleanup retry sentinel");
        var window = new MainWindow(service);
        window.Close();
        Check(!window.IsClosed && window.Dispatcher.PendingCount == 0, "Failed cleanup must not authorize close");
        Check(service.NeedsCleanup && manager.DisposeCalls == 0, "Failure lost cleanup ownership");
        Check(window.UninstallButton.IsEnabled && !window.InstallButton.IsEnabled, "Cleanup retry must remain available");
        Check(MessageBox.Messages.Count == 1 && MessageBox.Messages[0].Contains("cleanup retry sentinel"), "Cleanup error was hidden");
        manager.UninstallFailure = null;
        window.Close();
        CheckQueuedClose(window);
        Check(manager.UninstallCalls == 2, "Retry did not perform cleanup again");
    }

    private static void EnqueueFailureCanRetry()
    {
        var window = new MainWindow(new DriverService());
        window.Dispatcher.RejectNextEnqueue = true;
        window.Close();
        Check(!window.IsClosed && window.Dispatcher.PendingCount == 0, "Failed enqueue closed the window");
        Check(window.InstallButton.IsEnabled, "Failed enqueue left the UI permanently busy");
        Check(MessageBox.Messages.Count == 1 && MessageBox.Messages[0].Contains("enqueue failed"), "Dispatcher failure was hidden");
        window.Close();
        CheckQueuedClose(window);
    }

    private static void CancelledFinalCloseCanRetry()
    {
        var window = new MainWindow(new DriverService());
        window.Close();
        window.CancelNextClose = true;
        window.Dispatcher.Drain();
        Check(!window.IsClosed && window.InstallButton.IsEnabled, "Cancellation did not restore the UI");
        DriverManager.LastCreated.NeedsCleanup = true;
        window.Close();
        Check(DriverManager.LastCreated.UninstallCalls == 1, "Stale close authorization bypassed a new cleanup");
        CheckQueuedClose(window);
    }

    private static void DeferredCloseFailureCanRetry()
    {
        var window = new MainWindow(new DriverService());
        window.Close();
        window.CloseFailure = new InvalidOperationException("deferred close sentinel");
        window.Dispatcher.Drain();
        Check(!window.IsClosed && window.InstallButton.IsEnabled, "Deferred close failure left the UI unusable");
        Check(MessageBox.Messages.Count == 1 && MessageBox.Messages[0].Contains("deferred close sentinel"), "Deferred close error was hidden");
        window.CloseFailure = null;
        DriverManager.LastCreated.NeedsCleanup = true;
        window.Close();
        Check(DriverManager.LastCreated.UninstallCalls == 1, "Failed close left stale authorization");
        CheckQueuedClose(window);
    }

    private static void PendingCleanupCanRetry()
    {
        var window = new MainWindow(new DriverService());
        DriverManager manager = DriverManager.LastCreated;
        manager.NeedsCleanup = true;
        manager.UninstallCompletion = new TaskCompletionSource<bool>();
        SynchronizationContext previousContext = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(window.Dispatcher);
        try
        {
            window.Close();
            window.Close();
            Check(!window.IsClosed && manager.UninstallCalls == 1, "Pending cleanup was bypassed or duplicated");
            Check(manager.DisposeCalls == 0 && window.Dispatcher.PendingCount == 0, "Close or dispose was queued before cleanup completion");
            Check(!window.InstallButton.IsEnabled && !window.UninstallButton.IsEnabled, "Pending operation did not lock buttons");
            manager.UninstallCompletion.SetException(new InvalidOperationException("pending cleanup sentinel"));
            window.Dispatcher.Drain();
            Check(!window.IsClosed && manager.NeedsCleanup && window.UninstallButton.IsEnabled, "Async failure lost retry state");
            Check(MessageBox.Messages.Count == 1 && MessageBox.Messages[0].Contains("pending cleanup sentinel"), "Async error was hidden");
            manager.UninstallCompletion = new TaskCompletionSource<bool>();
            window.Close();
            manager.UninstallCompletion.SetResult(true);
            window.Dispatcher.Drain();
            Check(window.IsClosed && manager.UninstallCalls == 2 && manager.DisposeCalls == 1, "Async retry did not finish cleanly");
            Check(window.ReentrantCloseCount == 0, "Async completion reentered closing");
        }
        finally { SynchronizationContext.SetSynchronizationContext(previousContext); }
    }

    private sealed class TestApp : App
    {
        public void Start() { OnStartup(new StartupEventArgs()); }
    }

    private static void StartupAccessDenied()
    {
        DriverManager.DenyConstruction = true;
        var application = new TestApp();
        application.Start();
        Check(application.ShutdownCode.HasValue && application.ShutdownCode.Value != 0, "Permission failure must exit unsuccessfully");
        Check(application.MainWindow == null, "Permission failure must not create or show a window");
        Check(MessageBox.Messages.Count == 1 && MessageBox.Messages[0].Contains("管理员"), "Missing friendly administrator guidance");
    }

    private static string[] DeploymentPaths()
    {
        string baseDirectory = AppDomain.CurrentDomain.BaseDirectory;
        return new[]
        {
            Path.Combine(baseDirectory, "dll", "cqkeys.inf"),
            Path.Combine(baseDirectory, "dll", "cqkeys.sys"),
            Path.Combine(baseDirectory, "dll", "cqkeys.cat"),
            Path.Combine(baseDirectory, "cqkeysdll.dll")
        };
    }

    private static void PreflightReportsAllMissingFiles()
    {
        foreach (string path in DeploymentPaths()) Check(!File.Exists(path), "Unexpected deployment file in isolated harness output");
        var window = new MainWindow(new DriverService());
        foreach (string path in DeploymentPaths()) Check(window.LogTextBox.Text.Contains(path), "Preflight did not report " + Path.GetFileName(path));
        Check(DriverManager.LastCreated.InstallCalls == 0, "Preflight called the install boundary");
    }

    private sealed class DeploymentFiles : IDisposable
    {
        private readonly List<string> created = new List<string>();
        public DeploymentFiles(string missingFile)
        {
            try
            {
                foreach (string path in DeploymentPaths())
                {
                    Check(!File.Exists(path), "Refusing to overwrite an existing deployment file");
                    if (Path.GetFileName(path) == missingFile) continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write))
                    {
                        created.Add(path);
                        stream.WriteByte(0);
                    }
                }
            }
            catch { Dispose(); throw; }
        }
        public void Dispose()
        {
            foreach (string path in created) File.Delete(path);
            created.Clear();
        }
    }

    private static void PreflightBlocksMissingFile(string missingFile)
    {
        using (var files = new DeploymentFiles(missingFile))
        using (var service = new DriverService())
        {
            var window = new MainWindow(service);
            Check(window.LogTextBox.Text.Contains(missingFile), "Subscribed UI did not receive missing-file warning");
            int warnings = window.LogTextBox.Text.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries).Length;
            Check(warnings == 1, "Preflight should only report the missing file");
            try
            {
                service.InitializeDriverAsync().GetAwaiter().GetResult();
                throw new InvalidOperationException("Incomplete deployment accepted");
            }
            catch (FileNotFoundException) { }
            Check(DriverManager.LastCreated.InstallCalls == 0, "Incomplete deployment reached install boundary");
        }
    }

    private static void CompleteFileSetHasNoWarning()
    {
        using (var files = new DeploymentFiles(null))
        using (var service = new DriverService())
        {
            var window = new MainWindow(service);
            Check(window.LogTextBox.Text.Length == 0, "Presence checks must not claim installation or signature validation");
            Check(DriverManager.LastCreated.InstallCalls == 0, "Preflight installed a device");
        }
    }

    private static void DisposeCanRetry()
    {
        var service = new DriverService();
        DriverManager manager = DriverManager.LastCreated;
        manager.DisposeFailure = new InvalidOperationException("dispose retry sentinel");
        try { service.Dispose(); throw new Exception("Dispose failure was hidden"); }
        catch (InvalidOperationException error) { Check(error.Message == "dispose retry sentinel", "Unexpected dispose error"); }
        manager.DisposeFailure = null;
        service.Dispose();
        service.Dispose();
        Check(manager.DisposeCalls == 2, "Dispose must retry failures but not repeat success");
    }

    private static Win32Exception CreateProductionError(int code, string operation)
    {
        MethodInfo formatter = typeof(ProductionApp::CqKeys.DriverManager).GetMethod(
            "CreateDriverException", BindingFlags.NonPublic | BindingFlags.Static);
        Check(formatter != null, "Missing production error formatter");
        return (Win32Exception)formatter.Invoke(null, new object[] { code, operation });
    }

    private static void ErrorMessagesPreserveDetails()
    {
        foreach (int code in new[] { 5, 170, unchecked((int)0xE000020B) })
        {
            Win32Exception error = CreateProductionError(code, "LoadNTDriver");
            Check(error.NativeErrorCode == code, "Native error code changed");
            Check(error.Message.Contains("LoadNTDriver"), "Operation name missing");
            Check(error.Message.Contains(code.ToString()) && error.Message.Contains("0x" + code.ToString("X8")), "Numeric error details missing");
            Check(error.Message.Contains(new Win32Exception(code).Message), "System error description missing");
        }
    }

    private static void RebootErrorIsExplicit()
    {
        Win32Exception error = CreateProductionError(3010, "UnloadNTDriver");
        Check(error.NativeErrorCode == 3010 && error.Message.Contains("3010"), "3010 was changed or hidden");
        Check(error.Message.Contains(new Win32Exception(3010).Message), "Reboot system description missing");
        Check(error.Message.Contains("重启") && error.Message.Contains("不会自动重启"), "Missing manual restart guidance");
    }
}
