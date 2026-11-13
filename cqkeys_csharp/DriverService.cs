using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace CqKeys
{
    public class DriverService : IDisposable
    {
        private readonly DriverManager _driverManager;
        private readonly string _driverName;
        private readonly string _driverPath;
        private readonly string _dllPath;
        private bool _isDriverInstalled;
        private bool _disposed;

        public event EventHandler<string> StatusChanged;

        public DriverService()
        {
            _driverManager = new DriverManager();
            _driverName = "cqkeys";
            
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            
            _driverPath = Path.Combine(
                baseDir,
                "dll",
                "cqkeys.inf"
            );
            
            _dllPath = Path.Combine(
                baseDir,
                "cqkeysdll.dll"
            );

            _isDriverInstalled = false;
        }

        public bool IsDriverInstalled => _isDriverInstalled;
        public bool NeedsCleanup => _driverManager.NeedsCleanup;
        public bool CanTestInput => _isDriverInstalled && _driverManager.IsInputReady;
        
        public string DriverPath => _driverPath;

        public bool CheckDeploymentFiles()
        {
            string packageDirectory = Path.GetDirectoryName(_driverPath);
            string[] requiredFiles =
            {
                _driverPath,
                Path.Combine(packageDirectory, "cqkeys.sys"),
                Path.Combine(packageDirectory, "cqkeys.cat"),
                _dllPath
            };
            bool complete = true;
            foreach (string path in requiredFiles)
            {
                if (!File.Exists(path))
                {
                    complete = false;
                    OnStatusChanged($"警告：部署文件不存在: {path}");
                }
            }
            return complete;
        }

        public async Task InitializeDriverAsync()
        {
            try
            {
                OnStatusChanged($"正在查找驱动文件: {_driverPath}");
                
                if (!CheckDeploymentFiles())
                {
                    throw new FileNotFoundException("驱动部署文件不完整，请查看日志中的缺失路径。");
                }

                OnStatusChanged("正在安装驱动...");
                _isDriverInstalled = await _driverManager.InstallAndStartDriverAsync(_driverName, _driverPath);
                
                if (_isDriverInstalled)
                {
                    OnStatusChanged("驱动安装并连接成功，可测试键盘/鼠标输入；连接成功不代表应用已收到输入。");
                }
            }
            catch (Exception ex)
            {
                OnStatusChanged($"驱动安装失败: {ex.Message}");
                throw;
            }
        }

        public async Task UninstallDriverAsync()
        {
            try
            {
                if (!NeedsCleanup)
                {
                    OnStatusChanged("驱动未安装");
                    return;
                }

                OnStatusChanged("正在卸载驱动...");
                bool result = await _driverManager.StopAndUninstallDriverAsync(_driverName);
                
                if (result)
                {
                    _isDriverInstalled = false;
                    OnStatusChanged("驱动管理会话已清理，Driver Store 驱动包保留。");
                }
            }
            catch (Exception ex)
            {
                OnStatusChanged($"驱动卸载失败: {ex.Message}");
                throw;
            }
        }

        public async Task TestInputAsync(InputTestKind kind, CancellationToken cancellation)
        {
            if (!CanTestInput) throw new InvalidOperationException("驱动尚未成功连接，不能测试输入。");
            try
            {
                OnStatusChanged(kind == InputTestKind.Keyboard ? "正在发送 F8 按下/释放测试..." : "正在发送鼠标右移/左移各 24 HID 计数测试...");
                OnStatusChanged(await _driverManager.TestInputAsync(kind, cancellation));
            }
            catch (Exception error)
            {
                OnStatusChanged($"输入测试未成功完成：{error.Message} 请先卸载清理后重试。");
                throw;
            }
        }

        private void OnStatusChanged(string message)
        {
            StatusChanged?.Invoke(this, message);
        }

        public void Dispose()
        {
            if (_disposed) return;
            _driverManager.Dispose();
            _disposed = true;
        }
    }
}
