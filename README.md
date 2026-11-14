# cqkeydriver

基于 **KMDF + Virtual HID Framework (VHF)** 的纯软件虚拟输入设备，通过 Windows 标准
HID 输入栈提供键盘、鼠标和媒体键输入，无需实体键盘、鼠标。

```text
Python / C# / C++
       ↓
cqkeysdll.dll
       ↓ 映射、可取消任务、输入仲裁、错误传播
       ↓ 受 ACL 保护的设备接口 + 固定 IOCTL
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

SYS 维护固定报告、权威输入状态、提交顺序和会话清理，不执行宏、脚本或业务定时。
DLL 负责 VK 映射、组合输入、连按与可取消的有限动作任务；调用端负责 UI、配置、快捷键及业务内容。
生命周期 ABI 为 **2**，线协议为 **3.0**，驱动产品版本和能力独立查询。
“稳定底座”指职责和协议稳定：内核修复或能力变化修改 SYS 后仍须重新签名。

## 构建

DLL 和用户态原生测试使用 Visual Studio、MSVC v145 及 Windows SDK，测试与生产 DLL 使用同一工具集。
SYS 另需 MSVC v143、配套 Windows SDK / WDK 28000 和 WDK 平台工具集；v145 不能替代驱动构建前提。
构建缺失时不会自动安装工具、关闭检查或临时覆盖项目工具集。
使用配套 SDK/WDK 和 **64 位 MSBuild**：

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild .\cqkeysdll\cqkeysdll.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild .\cqkeysdll\cqkeysdll.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
& $msbuild .\cqkeys\cqkeys.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

DLL 输出：`cqkeysdll/x64/Debug/cqkeysdll.dll`。
Release DLL 位于 `cqkeysdll/x64/Release/cqkeysdll.dll`，是 Python 命令行演示的默认路径。
完整驱动包：`cqkeys/x64/Debug/cqkeys/`，包含同次构建的 INF、SYS、CAT。
**构建/生成目录文件不代表签名受信任、安装成功或输入可用。**
没有签名证书时，可用 `/p:SignMode=Off` 做未签名编译与 INF/目录生成检查；
这种产物不作为可安装发布包，不改变系统签名策略。生产部署须使用符合目标系统要求的签名。

驱动包日期和版本以 `cqkeys/cqkeys.inf` 中的 `DriverVer` 为唯一来源；Debug/Release 构建均保留该值，
不按构建时间自动改写。StampInf 仍执行其余处理，INF 校验和 CAT 生成不跳过。
发布新包时显式维护日期和四段版本，保证相对上一发布包的升级排序，再重新生成并签名完整 INF/SYS/CAT。
此策略只固定包版本，不承诺二进制逐字节可复现；包版本与线协议、DLL 生命周期 ABI 独立管理。

## 使用

1. 在目标 Windows 系统上准备正确签名且受信任的完整驱动包。内核故障可能导致蓝屏和业务中断，建议先在隔离环境验证。
2. `GetLifecycleVersion()` 必须为 `2`；DLL 与 SYS 必须支持线协议 3.0 及所需能力，
   不要求以后仅更新业务功能的 DLL 必须与 SYS 同次构建。INF/SYS/CAT 必须是匹配且正确签名的完整包。
3. 管理员进程调用 `LoadNTDriver("cqkeys", infPath)`，再调用 `SetHandle()`。
   DLL 经 SetupAPI 创建固定的根枚举 PnP 节点并安装所选 INF。
4. 调用 `KeyDown/KeyUp`、鼠标接口、`ConsumerKeyDown/ConsumerKeyUp`，或提交 DLL 输入任务。
   primitive 输入接口返回 `void`，立即读取线程局部 `GetLastError()` 判断本次操作结果；
   用 `GetLastInputResult` 查询部分完成，异步任务必须等待并查询任务结果。
5. 停止时先禁止调用端新任务并请求 `StopInputProcessing()` 屏障，等待真实完成；
   需要设备管理时调用 `StopNTDriver` 禁用节点，或 `UnloadNTDriver` 移除节点。
   两者均保留 Driver Store 中的包，仅管理本项目的固定设备实例。
6. 测试时可在 C# / Python GUI 勾选“卸载时删除 Driver Store 包”，点击卸载并确认。
   新接口 `UnloadNTDriverAndPackage("cqkeys")` 在移除设备后删除本次暂存及设备绑定的 OEM 驱动包；
   不强制删除其他设备使用的包，不扫描全部历史版本。失败保留清理状态供重试，要求重启时明确报错。
   该选项默认关闭；仅关闭窗口不会发起新的包删除请求，但会继续先前已确认且未完成的清理。

C++ 调用端需要同时提供 `cqkeysdll/cqkeys.h` 和 `shared/cqkeys_protocol.h`，并保持二者的相对目录；
只复制前者无法解析其共享协议头。

Python 测试工具位于 `cqkeyspy_test/`，C# 管理器为 `cqkeys_csharp/CqKeys.csproj`。
Python GUI 需要带 Tkinter 的 64 位 Python 3.9+ 及 `pywin32`；单元测试使用隔离的边界，不要求运行真实 GUI。
Python GUI 选择 DLL 和完整包内的 `cqkeys.inf`，与 C# 一样在加载前检查 DLL、INF 及同目录 SYS/CAT，
缺失时列出路径并阻止调用原生安装接口。旧 `sys_path` 配置会提示迁移到同目录 INF，保存后改用 `inf_path`。
删除 Driver Store 包需要提供新导出接口的 DLL；旧 DLL 不会静默降级为保留包的卸载。
PnP 服务名为 `cqkeysVhf`。源码仓库不分发编译后的 DLL、SYS 或 CAT。

Python 命令行演示位于 [`example/python/demo.py`](example/python/demo.py)，默认检查连接状态并在退出时禁用设备，
可显式选择键盘、鼠标或媒体键测试，运行方法见 [演示说明](example/python/README.md)。

### C# 部署

C# 的 Debug/Release 配置名为 `AnyCPU`，实际输出固定为 x64。构建 C# 本身不会生成 DLL 或驱动包。
先构建同配置的 x64 原生 DLL，将正确签名、匹配的 INF/SYS/CAT 放入 `cqkeys_csharp/dll/` 后再构建：

```powershell
& $msbuild .\cqkeys_csharp/CqKeys.csproj /t:Build /p:Configuration=Release /p:Platform=AnyCPU
```

项目默认从 `cqkeysdll/x64/$(Configuration)/cqkeysdll.dll` 复制 DLL 到 EXE 同级；
可用 `/p:CqKeysNativeDll=完整路径` 显式指定其他来源。源文件缺失会阻止构建，不沿用输出目录的旧 DLL；
每次构建均复制所选来源，其余驱动文件保留在输出目录的 `dll/` 子目录：

```text
CqKeys.exe
cqkeysdll.dll
dll/
  cqkeys.inf
  cqkeys.sys
  cqkeys.cat
```

DLL 不能只放在输出的 `dll/` 内。文件齐备或 C# 编译成功不代表签名受信任或设备已安装。

### 行为边界

- 键盘支持 6 个普通键及 8 个独立修饰键；第 7 个普通键明确报错，左右 Ctrl/Shift/Alt 独立处理。
- 支持常用 Windows VK、导航键、数字小键盘、F1–F24；未映射 VK 返回 `ERROR_NOT_SUPPORTED`。
  OEM 键按常见 US 物理键位映射，由系统布局解释，不是 Unicode 文本输入 API。
- 相对移动使用原始 HID 计数，大位移按 16 位范围拆分；每次最多 256 份报告，每轴范围 ±8388352。
  超限在提交前拒绝，任务内超限会拒绝整个任务；屏幕位移由 Windows 鼠标设置决定。
- 绝对移动只映射**主显示器**；越界坐标明确报错。
- 滚轮参数使用 Windows `120` 单位，每次必须为 120 的整数倍且最多 127 格。
- 媒体键使用独立 Consumer Control Report；同一时刻一个 consumer usage，不覆盖已按下的媒体键。
- 休眠恢复时清空输入并改变代际，旧会话输入失败，需停止后重新加载/连接，避免重放旧按键。
- 报告成功仅表示 VHF 接受，停止成功仅表示驱动屏障完成并成功提交中性报告；均不表示目标应用已收到。
- 超时、取消和部分完成不自动重发输入；释放失败保留真实错误和可重试状态。
- `GetDriverHandle()` 是借用句柄，禁止关闭、直接混发报告、继承或跨进程复制后继续使用。
- 默认 VHF 缓冲在休眠恢复时的实际行为必须在隔离系统验收，不能用代际检查代替下游验证。

详细协议、失败重试和隔离验收清单见 [生命周期与验证](docs/runtime-unload.md)。

### 高频输入与资源边界

- 高频连按应提交一个有限重复任务，而不是每次点按新建任务；每个任务仍使用独立工作线程。
- DLL 复用一个串行报告事件，仅在真实 I/O 完成后重置复用；运行时清理释放事件，关闭失败保留重试状态。
- 正常任务只释放仍按住的输入；取消或结果不确定时保留完整中性化。已完整释放的 N 次点按为 2N 份报告及一次最终状态查询。
- 查询 I/O 使用短期借用计数，不占用生命周期锁等待；STOP 可先执行，但关闭设备仍等待在途查询真实结束。
- `GetDetailedErrorCode()` 读取本线程最近一次 DLL 成功/失败，不再被其他线程覆盖；异步结果仍须读取任务结果。
- 毫秒参数仍是动作完成后的相对等待，不承诺硬实时固定频率。GUI 时长按理论周期折算次数，显示任务完成侧统计，而非目标程序接收数。
- Python GUI 合并高频进度并限制日志缓冲/显示容量，溢出有显式计数；C# 日志限制为 65536 字符并关闭撤销历史，裁剪有提示。
- VHF 默认缓冲策略和 SYS 协议未改变。下游丢报告、休眠恢复及长时内核池稳定性仍须隔离验收，不能由用户态优化或单元测试证明。

## 本地测试

使用已安装的 64 位 Python 创建项目虚拟环境后运行；不需要激活脚本或修改 PowerShell 执行策略：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe -m compileall -q cqkeyspy_test
& $msbuild .\tests\native_lifecycle_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\Debug\native_lifecycle_test.exe
& $msbuild .\tests\native_pnp_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\Debug\native_pnp_test.exe
& $msbuild .\tests\native_kernel_state_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\Debug\native_kernel_state_test.exe
& $msbuild .\tests\native_input_tasks_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\tests\bin\Debug\native_input_tasks_test.exe
& $msbuild .\tests\csharp_lifecycle_test.csproj /t:Build /p:Configuration=Debug /p:Platform=AnyCPU
.\tests\bin\csharp-lifecycle\Debug\csharp_lifecycle_test.exe
```

需要运行 GUI 时，再在该虚拟环境提供运行依赖：`.\.venv\Scripts\python.exe -m pip install pywin32`。
这些准备命令不会安装或加载驱动；启动 GUI 后的驱动管理按钮才执行显式设备操作。

原生测试编译实际 DLL 输入/生命周期、SetupAPI 安装逻辑、共享协议及生产输入状态核心，使用假的 Win32 API
边界测试失败、重试和并发顺序；不安装驱动、不产生实际输入，不能代替内核运行验收。
C# 生命周期测试使用模拟的窗口和设备管理边界，不启动真实窗口或加载驱动。

## 参考与许可

- [Microsoft: Write a HID source driver by using VHF](https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
- [Microsoft: KMDF](https://learn.microsoft.com/windows-hardware/drivers/wdf/)
- [Microsoft: Driver package installation](https://learn.microsoft.com/windows-hardware/drivers/install/driver-installation)

本项目遵循 AGPLv3 开源协议。
