# CQKeys

基于 KMDF 和 Windows Virtual HID Framework 的虚拟键盘、鼠标驱动。
支持 **Windows 10 1709（16299）及更新版本的 x64 系统**。

| 项目 | 值 |
| --- | --- |
| 设备路径 | `\\.\cqkeys` |
| PnP 服务 | `cqkeys_vhf` |
| 硬件 ID | `ROOT\CQKEYSVHF` |
| 固定设备实例 | `ROOT\CQKEYSVHF\0000` |
| 客户端 DLL | `cqkeysdll.dll` |

## 目录

```text
cqkeys/       KMDF/VHF 驱动和 INF
cqkeysdll/    客户端 DLL
shared/       固定大小的 IOCTL 协议
tools/        PnP 安装工具
tests/        回归测试和设备接口检查
example/      Python、C# 调用示例
```

## 构建

安装 Visual Studio C++ 工具链（v143）、Windows SDK 和 WDK。
已验证的 WDK 版本为 28000.2526。使用 64 位 MSBuild，在仓库根目录运行：

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild .\cqkeys\cqkeys.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SignMode=Off /p:RunCodeAnalysis=true /p:EnablePREfast=true
& $msbuild .\cqkeysdll\cqkeysdll.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /p:RunCodeAnalysis=true
& $msbuild .\tools\device_setup.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

输出：

- 驱动包：`cqkeys/x64/Debug/cqkeys/`，包含 INF、SYS、CAT。
- DLL：`cqkeysdll/x64/Debug/cqkeysdll.dll`。
- 安装工具：`tools/x64/Debug/device_setup.exe`。

`SignMode=Off` 用于本地构建检查。安装前必须按目标系统要求完成驱动包签名；
修改 INF/SYS 后需要重新生成目录并签名。构建不会安装驱动或修改系统签名策略。

## 安装和移除

将同一次构建并完成签名的 INF/SYS/CAT 放在同一目录，在管理员终端运行：

```powershell
.\tools\x64\Debug\device_setup.exe install "C:\DriverPackage\cqkeys.inf"
```

返回码 `0` 表示安装 API 成功，`3010` 表示需要重启。
安装后检查设备管理器中的设备状态，再连接客户端。

安装工具通过 SetupAPI 注册固定实例，并将指定 INF 安装到该实例。
INF 在虚拟设备上配置 `vhf` 下层过滤器。安装失败时，工具尝试撤销本次新建的设备节点，
并报告撤销结果；已暂存的 Driver Store 包保留。

关闭客户端后移除设备：

```powershell
.\tools\x64\Debug\device_setup.exe remove
```

该命令只移除上述固定实例。删除 Driver Store 包前，需用 `pnputil /enum-drivers`
确认其准确发布名称。`pnputil /add-driver /install` 本身不会创建本项目需要的 root 设备节点。

## 客户端

[调用示例](example/README.md) 包括 Python 文本输入、图形操作界面和 C# 设备连接示例。
DLL 声明见 [cqkeys.h](cqkeysdll/cqkeys.h)，底层协议见
[cqkeys_protocol.h](shared/cqkeys_protocol.h)。

连接、断开、状态查询和输入操作返回 `BOOL`；失败时读取当前线程的 `GetLastError()`。
`GetDetailedErrorCode()` 提供最近一次操作的共享错误快照。
DLL 内部串行化句柄操作，调用方仍需同步完整的按下/释放序列。

## 输入范围与资源管理

- 标准 set-1 键盘扫描码、左右修饰键、方向键、Print Screen 和 Pause。
  最多同时按住六个普通键和八个修饰键。消费者键、厂商扫描码及 `ExtraInformation` 不支持。
- 相对移动每次每轴为 `-32768..32767`；屏幕上的结果受 Windows 鼠标设置影响。
- 绝对移动仅支持主显示器。DLL 接受像素坐标，底层 IOCTL 接受 `0..65535`。
- 支持五个鼠标按钮。滚轮以 120 为一格，余量累计；水平滚轮可通过底层协议提交。
- 绝对定位和按钮使用不同 HID collection，组合请求先提交位置，再提交按钮。
  桌面事件的最终顺序需要在目标系统验证。

设备仅允许 SYSTEM 和管理员打开，采用独占连接并拒绝子路径。
输入必须满足结构长度、权限和数值范围要求。未知请求及不支持字段会返回错误。

报告队列最多保存 64 个待提交报告，另有一个 VHF 持有的报告。队列满时返回忙错误，
不改变输入状态。输入请求成功表示已入队，不保证桌面已经处理。
VHF 提交失败会锁存错误并停止接收输入；状态协议中的 `LastSubmitStatus` 提供原始
NTSTATUS，可通过重新创建设备恢复。

关闭客户端、调用 `ResetInput` 或电源恢复时，驱动丢弃待提交输入并排入全释放报告。
VHF 失败、设备移除或消费端停止读报告时，不能保证释放报告被消费。
设备清理会等待在途 worker 结束，再同步删除 VHF 并释放工作项。

## 测试

无需安装驱动的测试：

```powershell
powershell -NoProfile -File .\tests\run.ps1
python .\tests\dll_contract_test.py
```

原生测试运行实际输入转换和有界队列代码，启用 AddressSanitizer。
DLL 测试只验证接口和失败路径，不打开设备。

在已安装签名驱动的隔离 VM 中运行：

```powershell
# 管理员终端，关闭其他客户端
python .\tests\device_contract_test.py
# 普通用户终端
python .\tests\device_contract_test.py --expect-denied
```

设备接口测试不发送有效的非零键鼠输入；关闭连接会生成全释放报告。
运行时还需检查 HID 枚举、输入及释放、队列背压、持续输入下的设备移除、
睡眠恢复和重复安装卸载，并使用 Driver Verifier 检查内存池、I/O、锁和 WDF 行为。
编译和静态测试不能替代这些验证。

## 许可与参考

许可证为 [AGPL-3.0](LICENSE)。

源码致谢：

- [kmclassdll](https://github.com/BestBurning/kmclassdll)
- [kmclass](https://github.com/BestBurning/kmclass)

实现参考 Microsoft 的
[VHF 源驱动文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)、
[报告缓冲契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfreadreportsubmit)及
[同步删除接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfdelete)。
