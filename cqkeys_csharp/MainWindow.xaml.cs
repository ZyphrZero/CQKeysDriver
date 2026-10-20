using System;
using System.Windows;

namespace CqKeys
{
    public partial class MainWindow : Window
    {
        private readonly DriverService _driverService;

        public MainWindow(DriverService driverService)
        {
            InitializeComponent();
            _driverService = driverService ?? throw new ArgumentNullException(nameof(driverService));
            _driverService.StatusChanged += DriverService_StatusChanged;
            
            // 显示驱动路径
            DriverPathText.Text = $"驱动路径: {_driverService.DriverPath}";
            
            UpdateButtonStates();
        }

        private void DriverService_StatusChanged(object sender, string message)
        {
            // 确保在UI线程上更新界面
            Dispatcher.Invoke(() =>
            {
                LogTextBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}\n");
                LogTextBox.ScrollToEnd();
                UpdateButtonStates();
            });
        }

        private void UpdateButtonStates()
        {
            InstallButton.IsEnabled = !_driverService.IsDriverInstalled;
            UninstallButton.IsEnabled = _driverService.IsDriverInstalled;
        }

        private async void InstallButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                InstallButton.IsEnabled = false;
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
                UpdateButtonStates();
            }
        }

        private async void UninstallButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                UninstallButton.IsEnabled = false;
                await _driverService.UninstallDriverAsync();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"卸载驱动时发生错误：{ex.Message}", 
                    "错误", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
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
