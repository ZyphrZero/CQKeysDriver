# 调用示例

运行前先按[项目文档](../README.md)构建签名驱动包。Python GUI 可执行安装，
命令行及 C# 示例需要预先安装驱动。
设备采用独占连接，请一次只运行一个客户端。所有示例需管理员权限。

## Python

`python/driver_manager.py` 集中声明 ctypes 函数签名并检查 `BOOL` 返回值。
默认优先使用脚本同目录的 `cqkeysdll.dll`，其次使用仓库中的
`cqkeysdll/x64/Release/cqkeysdll.dll`，不存在时选择 Debug 构建；与当前工作目录无关。

在仓库根目录运行文本输入示例：

```powershell
python .\example\python\simple_input.py --text "Hello, World!"
# 指定 DLL
python .\example\python\simple_input.py --dll "C:\CQKeys\cqkeysdll.dll" --text "Hello"
```

脚本连接后等待三秒，然后向当前前台窗口输入文本。
使用图形界面还需安装 `pywin32`：

```powershell
python -m pip install -r .\example\python\requirements.txt
python .\example\python\gui.py
```

在图形界面的“驱动控制”页：

1. 点击“选择发布包目录”，选中已解压并包含 `cqkeysdll.dll`、`cqkeys.inf`、
   `cqkeys.sys`、`cqkeys.cat`、`device_setup.exe` 的目录。
   也可以分别选择 DLL、INF 和安装工具。选择仓库中的 DLL 时，会按其
   Release / Debug 配置查找配套 INF 和安装工具。
2. 首次使用点击“安装并连接”；已安装设备可直接点击“连接设备”。
   安装在后台执行，调用项目的 `device_setup.exe install`，连接成功后才开放输入测试。
3. 安装返回 `3010` 时，界面提示重启并保持未连接；不会自动重启。
   安装失败会保留原始错误码和工具输出。返回 `0` 后仍需通过 DLL 的设备协议检查，
   不会把“安装 API 成功”直接当作“已连接”。
4. “刷新设备状态”无需先连接，可查看设备是否安装、启动或存在设备管理器问题码。
   `ConnectDevice` 错误 2 表示设备路径不存在；请检查安装和启动状态。
   访问被拒绝时检查管理员权限，设备被占用时关闭其他客户端。

GUI 不会自动导入证书、修改 Secure Boot 或启用测试签名。
自签名驱动需要事先准备相应测试环境；“安装并连接”不能绕过系统签名要求。
安装及连接均需管理员权限。断开连接或关闭 GUI 只释放客户端句柄，驱动保持安装。

运行配置保存在示例目录的 `driver_config.json`，不纳入源码。
DLL、INF、安装工具路径及输入测试设置可一同保存；旧的仅含 DLL 路径的配置仍可读取。

无需安装驱动的 Python 安装流程单元测试：

```powershell
python .\tests\python_driver_setup_test.py
python .\tests\python_gui_driver_test.py
```

GUI 测试使用隐藏的 Tk 窗口及已构建 DLL；安装返回码通过测试替身模拟，
不会实际安装驱动，不能替代隔离测试系统中的安装及运行验证。

## C#

`csharp/` 是 .NET Framework 4.7.2 WPF 示例，通过 `CreateFile` 和专用状态 IOCTL
展示连接、协议检查和句柄释放。使用 Visual Studio 打开 `csharp/CqKeys.sln`，或在仓库根目录运行：

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild .\example\csharp\CqKeys.csproj /t:Build /p:Configuration=Debug /p:Platform=AnyCPU
.\example\csharp\bin\Debug\CqKeys.exe
```

## DLL 接口

| 接口 | 用途 |
| --- | --- |
| `ConnectDevice()` | 打开设备并验证协议及状态 |
| `DisconnectDevice()` | 关闭设备连接 |
| `ResetInput()` | 丢弃待提交输入并排入全释放报告 |
| `CheckDeviceStatus()` | 更新连接状态 |
| `GetDriverStatus()` | 读取最近状态：0 未连接、1 就绪、2 错误、5 连接失败 |
| `GetProtocolVersion()` | 读取 DLL 协议版本 |
| `KeyDown(vk)` / `KeyUp(vk)` | 按下、释放 Windows 虚拟键 |
| `MouseMoveRELATIVE(x, y)` | 相对移动，每轴每次 -32768..32767 |
| `MouseMoveABSOLUTE(x, y)` | 移动到主显示器上的像素坐标 |
| `Mouse…Down()` / `Mouse…Up()` | 五个鼠标按钮的按下和释放 |
| `MouseWheelUp(delta)` / `MouseWheelDown(delta)` | 滚轮，120 为一格 |

操作函数返回 `BOOL`，失败时读取当前线程的 `GetLastError()`。
Python 绑定使用 `use_last_error=True` 和 `restype=c_int`，将失败转换成异常。
完整导出声明见 [cqkeys.h](../cqkeysdll/cqkeys.h)。

输入成功只表示报告已入队。应用应显式配对按下/释放，并在 `finally` 或
`Dispose` 中关闭连接；队列满及设备失效时应处理错误，不应无限重试。
