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
            DriverPathText.Text = $"设备: {_driverService.DriverPath}。请先使用安装工具安装 VHF 驱动包。";
            
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
            ConnectButton.IsEnabled = !_driverService.IsConnected;
            DisconnectButton.IsEnabled = _driverService.IsConnected;
        }

        private async void ConnectButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                ConnectButton.IsEnabled = false;
                await _driverService.ConnectAsync();
            }
            catch (UnauthorizedAccessException)
            {
                MessageBox.Show("需要管理员权限才能连接设备。\n请以管理员身份运行程序。",
                    "权限错误", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"连接设备时发生错误：{ex.Message}",
                    "错误", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                UpdateButtonStates();
            }
        }

        private async void DisconnectButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                DisconnectButton.IsEnabled = false;
                await _driverService.DisconnectAsync();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"断开设备时发生错误：{ex.Message}",
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
