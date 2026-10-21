# C# 调用示例

运行前先按[项目文档](../README.md)构建签名驱动包。示例需要预先安装驱动，
设备采用独占连接，请一次只运行一个客户端。示例需管理员权限。

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
完整导出声明见 [cqkeys.h](../cqkeysdll/cqkeys.h)。

输入成功只表示报告已入队。应用应显式配对按下/释放，并在 `finally` 或
`Dispose` 中关闭连接；队列满及设备失效时应处理错误，不应无限重试。
