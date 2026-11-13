# cqkeydriver

基于 **KMDF + Virtual HID Framework (VHF)** 的纯软件虚拟输入设备，通过 Windows 标准
HID 输入栈提供键盘、鼠标和媒体键输入，无需实体键盘、鼠标。

```text
Python / C# / C++
       ↓
cqkeysdll.dll
       ↓ 受 ACL 保护的设备接口 + 版本化 IOCTL
KMDF HID Source Driver (cqkeys.sys)
       ↓ VhfReadReportSubmit
Windows 标准 HID 输入栈
       ↓
应用程序
```

提供键盘、五键相对鼠标、绝对指针和媒体键四个 HID 顶层集合。接口仅允许 SYSTEM 和
提升权限的管理员访问；一个系统同时只允许一个协作式 DLL 管理会话，内核设备也独占打开。
本仓库安装包目标为 **Windows 10 1809+ / Windows 11 x64**。VHF 本身从 Windows 10 开始提供；
1809 是本包采用 Driver Store 安装方式的支持下限，不代表所有这些系统均已实测。

## 构建

环境：Visual Studio 2026、MSVC v143、Windows SDK / WDK 28000。
使用配套 SDK/WDK 和 **64 位 MSBuild**：

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild .\cqkeysdll\cqkeysdll.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild .\cqkeys\cqkeys.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

DLL 输出：`cqkeysdll/x64/Debug/cqkeysdll.dll`。
完整驱动包：`cqkeys/x64/Debug/cqkeys/`，包含同次构建的 INF、SYS、CAT。
**构建/生成目录文件不代表签名受信任、安装成功或输入可用。**
没有签名证书时，可用 `/p:SignMode=Off` 做未签名编译与 INF/目录生成检查；
这种产物不作为可安装发布包，不改变系统签名策略。生产部署须使用符合目标系统要求的签名。

## 使用

1. 在获授权的隔离测试机准备正确签名且受信任的完整驱动包。不要在日常主机上调试内核驱动。
2. 使用同次构建的 DLL 和驱动包；`GetLifecycleVersion()` 必须为 `2`。
3. 管理员进程调用 `LoadNTDriver("cqkeys", infPath)`，再调用 `SetHandle()`。
   DLL 经 SetupAPI 创建固定的根枚举 PnP 节点并安装所选 INF。
4. 调用 `KeyDown/KeyUp`、鼠标接口，或 `ConsumerKeyDown/ConsumerKeyUp`。
   输入接口返回 `void`，立即读取线程局部 `GetLastError()` 判断本次操作结果。
5. 停止调用端任务后，调用 `StopNTDriver` 禁用节点，或 `UnloadNTDriver` 移除节点。
   两者均保留 Driver Store 中的包，仅管理本项目的固定设备实例。

Python 测试工具位于 `cqkeyspy_test/`，C# 管理器为 `cqkeys_csharp/CqKeys.csproj`。
在 Python GUI 的 SYS 选择框中选择完整包内的 `cqkeys.sys`；DLL 使用同目录同名 INF
执行安装，缺失时报错。PnP 服务名为 `cqkeysVhf`。源码仓库不分发编译后的 DLL、SYS 或 CAT。

### 行为边界

- 键盘支持 6 个普通键及 8 个独立修饰键；第 7 个普通键明确报错，左右 Ctrl/Shift/Alt 独立处理。
- 支持常用 Windows VK、导航键、数字小键盘、F1–F24；未映射 VK 返回 `ERROR_NOT_SUPPORTED`。
  OEM 键按常见 US 物理键位映射，由系统布局解释，不是 Unicode 文本输入 API。
- 相对移动使用原始 HID 计数，大位移按 16 位范围拆分，屏幕位移由 Windows 鼠标设置决定。
- 绝对移动只映射**主显示器**；越界坐标明确报错。
- 滚轮参数使用 Windows `120` 单位，每次必须为 120 的整数倍且最多 127 格。
- 媒体键使用独立 Consumer Control Report；同一时刻一个 consumer usage，不覆盖已按下的媒体键。
- 休眠恢复时清空输入并改变代际，旧会话输入失败，需停止后重新加载/连接，避免重放旧按键。

详细协议、失败重试和隔离验收清单见 [生命周期与验证](docs/runtime-unload.md)。

## 本地测试

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe -m compileall -q cqkeyspy_test
& $msbuild .\tests\native_lifecycle_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\native_lifecycle_test.exe
& $msbuild .\tests\native_pnp_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\native_pnp_test.exe
```

原生测试编译实际 DLL 输入/生命周期、SetupAPI 安装逻辑及共享协议，使用假的 Win32 API
边界测试失败、重试和并发顺序；不安装驱动、不产生实际输入，不能代替内核运行验收。

## 参考与许可

- [Microsoft: Write a HID source driver by using VHF](https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
- [Microsoft: KMDF](https://learn.microsoft.com/windows-hardware/drivers/wdf/)
- [Microsoft: Driver package installation](https://learn.microsoft.com/windows-hardware/drivers/install/driver-installation)

本项目遵循 AGPLv3 开源协议。
