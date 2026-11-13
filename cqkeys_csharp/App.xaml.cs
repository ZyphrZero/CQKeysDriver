using System.Windows;
using System;

namespace CqKeys
{
    public partial class App : Application
    {
        private DriverService _driverService;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            try
            {
                _driverService = new DriverService();
            }
            catch (UnauthorizedAccessException)
            {
                MessageBox.Show("需要管理员权限才能操作驱动。\n请以管理员身份运行程序。",
                    "权限错误", MessageBoxButton.OK, MessageBoxImage.Warning);
                Shutdown(1);
                return;
            }
            
            MainWindow = new MainWindow(_driverService);
            MainWindow.Show();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            _driverService?.Dispose();
            base.OnExit(e);
        }
    }
}
