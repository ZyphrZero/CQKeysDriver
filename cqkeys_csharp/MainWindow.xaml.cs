using System;
using System.Windows;
using System.ComponentModel;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;

namespace CqKeys
{
    public partial class MainWindow : Window
    {
        private readonly DriverService _driverService;
        private bool operationPending;
        private bool closeAllowed;
        private CancellationTokenSource testCancellation;
        private bool observingInput;
        private int observedKeyDown, observedKeyUp, observedMouseMoves;
        private const int MaxLogCharacters = 65536;
        private const string LogTrimmedNotice = "[较早日志已裁剪，仅保留最近内容]\n";

        public MainWindow(DriverService driverService)
        {
            InitializeComponent();
            LogTextBox.IsUndoEnabled = false;
            _driverService = driverService ?? throw new ArgumentNullException(nameof(driverService));
            _driverService.StatusChanged += DriverService_StatusChanged;
            
            // 显示驱动路径
            DriverPathText.Text = $"驱动路径: {_driverService.DriverPath}";
            
            UpdateButtonStates();
            _driverService.CheckDeploymentFiles();
        }

        private void DriverService_StatusChanged(object sender, string message)
        {
            // 确保在UI线程上更新界面
            Dispatcher.Invoke(() =>
            {
                if (message != null && message.Length > MaxLogCharacters / 2)
                    message = "[单条日志过长，已截断]" + message.Substring(message.Length - MaxLogCharacters / 2);
                string entry = $"[{DateTime.Now:HH:mm:ss}] {message}\n";
                string text = LogTextBox.Text + entry;
                if (text.Length > MaxLogCharacters)
                    LogTextBox.Text = LogTrimmedNotice + text.Substring(text.Length - MaxLogCharacters + LogTrimmedNotice.Length);
                else
                    LogTextBox.AppendText(entry);
                LogTextBox.ScrollToEnd();
                UpdateButtonStates();
            });
        }

        private void UpdateButtonStates()
        {
            InstallButton.IsEnabled = !operationPending && !_driverService.NeedsCleanup;
            UninstallButton.IsEnabled = !operationPending && _driverService.NeedsCleanup;
            RemovePackageCheckBox.IsEnabled = !operationPending;
            KeyboardTestButton.IsEnabled = MouseTestButton.IsEnabled = !operationPending && _driverService.CanTestInput;
            CancelTestButton.IsEnabled = testCancellation != null && !testCancellation.IsCancellationRequested;
        }

        private async void KeyboardTestButton_Click(object sender, RoutedEventArgs e)
        {
            await RunInputTestAsync(InputTestKind.Keyboard);
        }

        private async void MouseTestButton_Click(object sender, RoutedEventArgs e)
        {
            await RunInputTestAsync(InputTestKind.Mouse);
        }

        private async Task RunInputTestAsync(InputTestKind kind)
        {
            if (operationPending || !_driverService.CanTestInput) return;
            operationPending = true;
            testCancellation = new CancellationTokenSource();
            testCancellation.CancelAfter(5000);
            UpdateButtonStates();
            try
            {
                if (!IsActive || Keyboard.Modifiers != ModifierKeys.None || !InputTestSurface.Focus())
                    throw new InvalidOperationException("请将本窗口置于前台并松开 Ctrl/Alt/Shift/Win 后重试。");
                observedKeyDown = observedKeyUp = observedMouseMoves = 0;
                InputObservationText.Text = "准备测试，请勿操作实体键鼠...";
                await Task.Delay(200, testCancellation.Token);
                if (!IsActive) throw new OperationCanceledException("窗口已失去焦点。");
                observingInput = true;
                await _driverService.TestInputAsync(kind, testCancellation.Token);
                await Task.Delay(200);
                bool observed = kind == InputTestKind.Keyboard ? observedKeyDown > 0 && observedKeyUp > 0 : observedMouseMoves > 0;
                InputObservationText.Text = (observed ? "本窗口观察到相应事件（来源未经鉴定）。" : "未观察到完整的预期事件，不能确认端到端输入生效。") + ObservationCounts();
            }
            catch (OperationCanceledException error)
            {
                InputObservationText.Text = "测试已取消；已提交输入不能撤回。" + ObservationCounts();
                DriverService_StatusChanged(this, error.Message);
            }
            catch (Exception error)
            {
                InputObservationText.Text = "测试失败，未判定输入有效。" + ObservationCounts();
                DriverService_StatusChanged(this, "输入测试失败：" + error.Message);
                MessageBox.Show(error.Message, "输入测试失败");
            }
            finally
            {
                observingInput = false;
                testCancellation.Dispose();
                testCancellation = null;
                operationPending = false;
                UpdateButtonStates();
            }
        }

        private string ObservationCounts()
        {
            return $" F8 按下 {observedKeyDown} 次、释放 {observedKeyUp} 次；鼠标移动事件 {observedMouseMoves} 次。";
        }

        private void InputPreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (!observingInput || e.Key != Key.F8 || e.IsRepeat) return;
            observedKeyDown++;
            InputObservationText.Text = "正在接收：" + ObservationCounts();
            e.Handled = true;
        }

        private void InputPreviewKeyUp(object sender, KeyEventArgs e)
        {
            if (!observingInput || e.Key != Key.F8) return;
            observedKeyUp++;
            InputObservationText.Text = "正在接收：" + ObservationCounts();
            e.Handled = true;
        }

        private void InputPreviewMouseMove(object sender, MouseEventArgs e)
        {
            if (!observingInput) return;
            observedMouseMoves++;
            InputObservationText.Text = "正在接收：" + ObservationCounts();
        }

        private void CancelTestButton_Click(object sender, RoutedEventArgs e)
        {
            testCancellation?.Cancel();
            InputObservationText.Text = "已请求取消，等待实际任务结束和释放；请勿强制关闭。";
            UpdateButtonStates();
        }

        protected override void OnDeactivated(EventArgs e)
        {
            testCancellation?.Cancel();
            base.OnDeactivated(e);
        }

        private async void InstallButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                operationPending = true;
                UpdateButtonStates();
                await _driverService.InitializeDriverAsync();
            }
            catch (UnauthorizedAccessException)
            {
                MessageBox.Show("需要管理员权限才能安装驱动。\n请以管理员身份运行程序。", 
                    "权限错误", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"安装驱动时发生错误：{ex.Message}", 
                    "错误", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                operationPending = false;
                UpdateButtonStates();
            }
        }

        private async void UninstallButton_Click(object sender, RoutedEventArgs e)
        {
            if (operationPending || !_driverService.NeedsCleanup) return;
            bool removePackage = RemovePackageCheckBox.IsChecked == true;
            if (removePackage && MessageBox.Show(
                    "将移除设备，并删除本次暂存及设备绑定的驱动包。\n不强制删除其他设备使用的包，不清扫所有历史版本。\n下次安装需要完整且正确签名的 INF/SYS/CAT。继续？",
                    "删除 Driver Store 驱动包", MessageBoxButton.YesNo, MessageBoxImage.Warning,
                    MessageBoxResult.No) != MessageBoxResult.Yes) return;
            try
            {
                operationPending = true;
                UpdateButtonStates();
                await _driverService.UninstallDriverAsync(removePackage);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"卸载驱动时发生错误：{ex.Message}", 
                    "错误", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                operationPending = false;
                UpdateButtonStates();
            }
        }

        protected override async void OnClosing(CancelEventArgs e)
        {
            if (closeAllowed) { base.OnClosing(e); return; }
            e.Cancel = true;
            base.OnClosing(e);
            if (operationPending)
            {
                testCancellation?.Cancel();
                return;
            }
            operationPending = true;
            UpdateButtonStates();
            try
            {
                await _driverService.UninstallDriverAsync();
                _ = Dispatcher.BeginInvoke(new Action(CompleteClose));
            }
            catch (Exception error)
            {
                operationPending = false;
                UpdateButtonStates();
                MessageBox.Show($"关闭失败，窗口保留以便重试：{error.Message}", "关闭失败");
            }
        }

        private void CompleteClose()
        {
            closeAllowed = true;
            try
            {
                Close();
            }
            catch (Exception error)
            {
                MessageBox.Show($"关闭窗口时发生错误：{error.Message}", "关闭失败");
            }
            finally
            {
                closeAllowed = false;
                operationPending = false;
                UpdateButtonStates();
            }
        }

        protected override void OnClosed(EventArgs e)
        {
            _driverService.Dispose();
            base.OnClosed(e);
        }
    }
}
