# 调用示例

运行前先按[项目文档](../README.md)构建并安装签名驱动包。
设备采用独占连接，请一次只运行一个客户端。所有示例需管理员权限。

## Python

`python/driver_manager.py` 集中声明 ctypes 函数签名并检查 `BOOL` 返回值。
默认 DLL 路径为仓库中的 `cqkeysdll/x64/Debug/cqkeysdll.dll`，与当前工作目录无关。

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

图形界面提供键盘、鼠标和状态查询操作；运行配置保存在示例目录的
`driver_config.json`，不纳入源码。

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
