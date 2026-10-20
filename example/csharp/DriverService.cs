using System;
using System.Threading.Tasks;

namespace CqKeys
{
    public class DriverService : IDisposable
    {
        private readonly DriverManager _driverManager;
        private bool _isConnected;
        private bool _disposed;

        public event EventHandler<string> StatusChanged;

        public DriverService()
        {
            _driverManager = new DriverManager();
            _isConnected = false;
        }

        public bool IsConnected => _isConnected;

        public string DriverPath => @"\\.\cqkeys";

        public async Task ConnectAsync()
        {
            try
            {
                OnStatusChanged("正在连接已安装的 VHF 设备...");
                _isConnected = await _driverManager.ConnectAsync();

                if (_isConnected)
                {
                    OnStatusChanged("设备已连接");
                }
            }
            catch (Exception ex)
            {
                OnStatusChanged($"设备连接失败: {ex.Message}");
                throw;
            }
        }

        public async Task DisconnectAsync()
        {
            try
            {
                if (!_isConnected)
                {
                    OnStatusChanged("设备未连接");
                    return;
                }

                OnStatusChanged("正在断开设备...");
                bool result = await _driverManager.DisconnectAsync();

                if (result)
                {
                    _isConnected = false;
                    OnStatusChanged("设备已断开");
                }
            }
            catch (Exception ex)
            {
                OnStatusChanged($"设备断开失败: {ex.Message}");
                throw;
            }
        }

        private void OnStatusChanged(string message)
        {
            StatusChanged?.Invoke(this, message);
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (!_disposed)
            {
                if (disposing)
                {
                    _driverManager?.Dispose();
                }
                _disposed = true;
            }
        }

        ~DriverService()
        {
            Dispose(false);
        }
    }
}
