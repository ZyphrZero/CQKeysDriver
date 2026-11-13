using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Threading;
using System.Threading.Tasks;

namespace System.Windows
{
    public class StartupEventArgs : EventArgs { }
    public class ExitEventArgs : EventArgs { }
    public class RoutedEventArgs : EventArgs { }
    public enum MessageBoxButton { OK }
    public enum MessageBoxImage { Warning, Error }

    public static class MessageBox
    {
        public static readonly List<string> Messages = new List<string>();
        public static void Show(string message, string caption,
            MessageBoxButton button = MessageBoxButton.OK, MessageBoxImage image = MessageBoxImage.Error)
        {
            Messages.Add(caption + ": " + message);
        }
    }

    public class Application
    {
        public Window MainWindow { get; set; }
        public int? ShutdownCode { get; private set; }
        protected virtual void OnStartup(StartupEventArgs args) { }
        protected virtual void OnExit(ExitEventArgs args) { }
        public void Shutdown(int exitCode)
        {
            ShutdownCode = exitCode;
            OnExit(new ExitEventArgs());
        }
    }

    public sealed class TestDispatcher : SynchronizationContext
    {
        private readonly Queue<Action> callbacks = new Queue<Action>();
        public int PendingCount => callbacks.Count;
        public bool RejectNextEnqueue { get; set; }
        public void Invoke(Action callback) { callback(); }
        public override void Post(SendOrPostCallback callback, object state)
        {
            BeginInvoke(new Action(() => callback(state)));
        }
        public Delegate BeginInvoke(Delegate callback)
        {
            if (RejectNextEnqueue)
            {
                RejectNextEnqueue = false;
                throw new InvalidOperationException("Dispatcher enqueue failed");
            }
            callbacks.Enqueue((Action)callback);
            return callback;
        }
        public void Drain()
        {
            while (callbacks.Count != 0) callbacks.Dequeue()();
        }
    }

    public class Window
    {
        private bool closing;
        public TestDispatcher Dispatcher { get; } = new TestDispatcher();
        public bool IsClosed { get; private set; }
        public bool IsShown { get; private set; }
        public bool IsActive { get; private set; } = true;
        public bool CancelNextClose { get; set; }
        public Exception CloseFailure { get; set; }
        public int ReentrantCloseCount { get; private set; }
        public void Show() { IsShown = true; }
        public void Deactivate() { IsActive = false; OnDeactivated(EventArgs.Empty); }
        public void Close()
        {
            if (closing)
            {
                ReentrantCloseCount++;
                throw new InvalidOperationException("Close cannot reenter OnClosing");
            }
            if (IsClosed) throw new InvalidOperationException("Window already closed");
            if (CloseFailure != null) throw CloseFailure;
            closing = true;
            try
            {
                var args = new CancelEventArgs();
                OnClosing(args);
                if (!args.Cancel)
                {
                    IsClosed = true;
                    OnClosed(EventArgs.Empty);
                }
            }
            finally { closing = false; }
        }
        protected virtual void OnClosing(CancelEventArgs args)
        {
            if (CancelNextClose)
            {
                CancelNextClose = false;
                args.Cancel = true;
            }
        }
        protected virtual void OnClosed(EventArgs args) { }
        protected virtual void OnDeactivated(EventArgs args) { }
    }

    public sealed class TestControl
    {
        public bool IsEnabled { get; set; }
        public string Text { get; set; } = "";
        public void AppendText(string text) { Text += text; }
        public void ScrollToEnd() { }
        public bool Focus() { return true; }
    }
}

namespace System.Windows.Input
{
    public enum ModifierKeys { None, Control }
    public enum Key { F8, A }
    public static class Keyboard { public static ModifierKeys Modifiers { get; set; } }
    public class KeyEventArgs : EventArgs
    {
        public Key Key { get; set; }
        public bool IsRepeat { get; set; }
        public bool Handled { get; set; }
    }
    public class MouseEventArgs : EventArgs { }
}

namespace CqKeys
{
    public sealed class DriverManager : IDisposable
    {
        public static bool DenyConstruction;
        public static DriverManager LastCreated;
        public bool NeedsCleanup { get; set; }
        public bool IsInputReady { get; set; }
        public int TestCalls { get; private set; }
        public Func<InputTestKind, CancellationToken, Task<string>> InputTestOperation { get; set; }
        public int InstallCalls { get; private set; }
        public int UninstallCalls { get; private set; }
        public int DisposeCalls { get; private set; }
        public Exception UninstallFailure { get; set; }
        public TaskCompletionSource<bool> UninstallCompletion { get; set; }
        public Exception DisposeFailure { get; set; }
        public DriverManager()
        {
            if (DenyConstruction) throw new UnauthorizedAccessException("Test administrator denial");
            LastCreated = this;
        }
        public Task<bool> InstallAndStartDriverAsync(string name, string path)
        {
            InstallCalls++;
            throw new InvalidOperationException("Unexpected install boundary call; no native implementation is linked here");
        }
        public async Task<bool> StopAndUninstallDriverAsync(string name)
        {
            UninstallCalls++;
            if (UninstallFailure != null) throw UninstallFailure;
            if (UninstallCompletion != null) await UninstallCompletion.Task;
            NeedsCleanup = false;
            IsInputReady = false;
            return true;
        }
        public async Task<string> TestInputAsync(InputTestKind kind, CancellationToken cancellation)
        {
            TestCalls++;
            if (InputTestOperation == null) throw new InvalidOperationException("No input boundary configured; real input is forbidden in this harness");
            try { return await InputTestOperation(kind, cancellation); }
            catch { IsInputReady = false; throw; }
        }
        public void Dispose()
        {
            DisposeCalls++;
            if (DisposeFailure != null) throw DisposeFailure;
            if (NeedsCleanup) throw new InvalidOperationException("Dispose before successful cleanup");
        }
    }

    public partial class MainWindow
    {
        public System.Windows.TestControl DriverPathText = new System.Windows.TestControl();
        public System.Windows.TestControl LogTextBox = new System.Windows.TestControl();
        public System.Windows.TestControl InstallButton = new System.Windows.TestControl();
        public System.Windows.TestControl UninstallButton = new System.Windows.TestControl();
        public System.Windows.TestControl KeyboardTestButton = new System.Windows.TestControl();
        public System.Windows.TestControl MouseTestButton = new System.Windows.TestControl();
        public System.Windows.TestControl CancelTestButton = new System.Windows.TestControl();
        public System.Windows.TestControl InputTestSurface = new System.Windows.TestControl();
        public System.Windows.TestControl InputObservationText = new System.Windows.TestControl();
        private void InitializeComponent() { }
    }
}
