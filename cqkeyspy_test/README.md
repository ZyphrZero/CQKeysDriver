# CqKeys DLL 与 Python 调用

使用 x64 DLL，生命周期 ABI 为 2，线协议为 3.0。需要支持所需能力且正确签名的完整 INF/SYS/CAT 包。
部署、权限、内核协议和隔离验收见 [项目说明](../README.md) 与 [生命周期契约](../docs/runtime-unload.md)。
正式 C ABI 的字段和签名以 `cqkeysdll/cqkeys.h` 为准。

## 路径与驱动包

GUI 分别选择 `cqkeysdll.dll` 和完整包中的 `cqkeys.inf`，不再选择裸 SYS。
安装前检查 DLL、INF 及 INF 同目录的 `cqkeys.sys`、`cqkeys.cat`，缺少任一文件或路径实际为目录时阻止加载，
并列出所有缺失路径；检查通过不代表签名受信任，安装和连接仍以 DLL / Windows 的结果为准。
配置使用 `dll_path` / `inf_path`；读取旧 `sys_path` 时明确提示并转换为同目录 INF，保存配置才移除旧字段。

## 可选删除 Driver Store 包

默认卸载只移除设备、保留包。测试时勾选“卸载时删除 Driver Store 包”，点击卸载并确认，
GUI 调用 `DriverManager.cleanup(remove_package=True)`，通过新版 DLL 的 `UnloadNTDriverAndPackage` 执行清理。
先完成输入停止、任务回收和设备移除，再删除本次暂存及设备绑定的 OEM 包；不会强制删除其他设备使用的包，
也不会按名称扫描、删除全部历史版本。包删除失败仍是清理失败，管理对象和已确认的选择会保留以便重试。
仅勾选后关闭窗口不会新发起包删除；已经确认但失败的删除请求在后续卸载/关闭中继续，不能静默退回普通卸载。
需要重新构建并部署支持该导出接口的 DLL，不需要为本功能修改 SYS 或关闭签名检查。
加载日志分别记录请求路径、Windows 模块句柄对应的实际 DLL 路径和删除接口是否可用。
选择删除时先检查当前会话接口；不支持时不会开始停止/卸载或保留删除请求，可明确取消勾选后普通卸载。
路径框和已保存配置只影响下一次加载，不会替换当前会话的 DLL；更新 DLL 后应在完成清理后重启程序。

## 生命周期

| API | 语义 |
|---|---|
| GetLifecycleVersion | 返回 2，不是 SYS 产品版本或线协议版本 |
| LoadNTDriver(name,path) | 显式安装/启用固定节点；name 仅支持 cqkeys，调用端传入 INF；原生接口保留 SYS 路径兼容 |
| SetHandle | 连接已启用设备、验证能力、建立全新中性会话 |
| StopInputProcessing | 内核会话停止屏障，保留句柄；成功后同一会话不能重新输入 |
| StopNTDriver(name) | 停止/释放/关闭，然后禁用节点，保留包 |
| UnloadNTDriver(name) | 停止/释放/关闭，然后移除节点，保留包 |
| UnloadNTDriverAndPackage(name) | 停止/释放/关闭、移除节点，再非强制删除本次相关 OEM 包；失败保留重试状态 |
| CleanupInputRuntime | 显式收束 DLL 输入运行时和连接，不执行 PnP 禁用/移除 |
| GetDriverHandle | 仅借用；禁止关闭、直接发报告或跨进程转移 |

失败必须保留管理对象供重试；初始化失败也可能已经拥有需要收尾的资源。
停止 GUI 时同时取消本地工作并在后台立即请求原生屏障，收到真实结果后再等待工作退出、执行设备管理。
只有明确的设备断连/不存在错误（1167/433）允许在保留原错误的情况下继续等待工作退出并执行管理清理；
其他停止错误仍阻止管理清理。GUI 分别展示“输入释放未确认”和管理清理结果，不将移除节点视为输入释放成功。
管理清理失败保留对象及两种错误供重试；管理清理完成后，关闭窗口前仍显示未确认释放警告。
不要用 GUI 线程等待驱动或用固定延时猜测完成。

`FreeLibrary` 前必须完成所有并发 API 调用，停止/等待任务、释放任务对象并显式清理运行时。
`DllMain` 不负责等待、设备管理或卸载。未完成清理就卸载 DLL 不受支持。

## Primitive 输入兼容接口

- `KeyDown/KeyUp(USHORT virtualKey)`：Windows VK；普通键最多 6 个，左右修饰键独立。
- `ConsumerKeyDown/ConsumerKeyUp(USHORT usage)`：一个 Consumer usage，0x0001–0x03ff。
- `MouseMoveRELATIVE(LONG dx,LONG dy)`：原始 HID 计数，每轴 ±8388352、每次最多 256 份报告；超限拒绝，失败不自动重放。
- `MouseMoveABSOLUTE(LONG x,LONG y)`：主显示器像素坐标，不是虚拟多屏或 Unicode 输入接口。
- Left/Right/Middle/XButton1/XButton2 的 Down/Up：保留其他已按鼠标按钮。
- `MouseWheelUp/Down(USHORT delta)`：Windows 120 单位的整数倍，每次最多 127 格。

这些旧输入接口返回 void，必须立即读取线程 LastError。单次调用有序不代表多个调用自动组成原子组合键。
多个旧调用线程共享 legacy 输入所有者，需要完整组合/取消语义时使用任务 API。

## 有限动作任务

任务在 DLL 中执行，SYS 不接收宏或动作数组。保留一个有界任务对象；前一个任务结束并 Release 后才提交下一个。
任务运行时 primitive 冲突返回 `ERROR_BUSY`；legacy 仍持键时不能启动任务，防止互相释放输入。

`CqInputAction` 为 16 字节：`type:u32 / value1:i32 / value2:i32 / delayMs:u32`。
delayMs 是该动作之后的可取消等待；它不是设备已收到输入的证明。

| type | 含义 | value1 / value2 |
|---|---|---|
| 1 / 2 | Key Down / Up | VK / 0 |
| 3 / 4 | Consumer Down / Up | usage / 0 |
| 5 | 相对移动 | dx / dy |
| 6 | 主屏绝对移动 | x / y |
| 7 | 鼠标按钮完整快照 | 低 5 位按钮 / 0 |
| 8 | 滚轮 | HID 格数 -127–127 / 0 |
| 9 | 仅等待 | 0 / 0 |

- `SubmitInputTask(actions,count,repeats,repeatDelayMs,outId)`：复制动作，启动用户态执行。
- `CancelInputTask(id)`：请求取消；仍须等待终结并查询释放结果，不能把取消请求成功当成已释放。
- `WaitInputTask(id,timeoutMs)`：只等待终结；超时返回失败及 `WAIT_TIMEOUT (258)`，任务仍可能在执行。
- `QueryInputTask(id,result)`：结果结构须先设置正确 size/version；state、error、cleanupError 才决定任务结果。
- `ReleaseInputTask(id)`：仅在结束后释放对象并收齐线程退出，运行中返回忙。

任务结构版本为 1，结果 `CqInputTaskResult` 为 96 字节。
state：1 Running、2 Completed、3 Cancelled、4 Failed。查询成功不等于任务成功。
`completedActions` 只计完整完成的动作，失败动作可能已经提交部分报告；结合提交水位和真实错误判断，不能整段重放。
`cleanupError` 单独报告释放失败，不能被取消状态覆盖。异步任务错误不会自动出现在发起线程的 LastError 中。
resultFlags 的 1/2/4 位分别表示部分完成、不确定结果、取消；未知位及非零 reserved 是不兼容结果。

资源边界：动作 1–4096 个，重复 1–65535 次，每动作等待及重复间隔 0–60000ms。零重复不是无限任务。
需要长业务时由调用端管理明确的有限任务，不在内核增加通用调度器。不承诺硬实时精度。

高频重复放在一个任务中以避免逐次创建线程。正常完整任务只释放仍持有的输入；取消或不确定提交仍完整中性化。
等待是动作完成后的相对毫秒等待，实际周期还包含 I/O 与调度耗时；GUI 时长仅按理论周期换算重复次数，不是硬截止时间。
整数毫秒经秒数往返不会因二进制浮点误差额外进位；真正的小数毫秒仍向上取整。

Python 的 `InputCancellation` 是单次提交的取消令牌。每个任务新建令牌，使用
`repeat_key(..., cancellation=token)` 或 `run_actions(..., cancellation=token)` 传入；其他线程可调用 `token.cancel()` 直接通知该任务。
提交前取消不会发送输入，提交期间的取消在绑定任务后送达；完成/释放时解除绑定，旧令牌不能取消下一任务。
取消失败仍抛出真实错误并保留任务供重试；取消请求不等于任务结束，仍需等待和检查结果。现有 `cancelled` 回调继续兼容。

## GUI 长时运行

高频进度只保存最新快照，不逐次堆积 Tk 回调；任务终态和错误通知使用独立可靠通道。
日志缓冲保留最近 256 条普通日志及 64 条错误日志，单条最多 2048 字符，溢出和截断数量显式显示。
日志窗口最多保留 1000 行、64000 字符；它不是完整审计存储，长期诊断应另行保存所需日志。
热键通知只保留一个待处理槽，每个 UI 轮次最多切换一次，过载合并次数显式记录；它不用于逐条计数热键事件。
停止或重启热键监听会使旧代际回调失效，不能由旧通知重新启动自动移动。
高频任务和自动移动各自持有取消令牌，停止按钮直接通知所属任务，不取消其他任务。

## 错误与结果

Python 使用 `ctypes.CDLL(path, use_last_error=True)`，C# 使用 `CallingConvention.Cdecl`、`SetLastError=true`。
BOOL 失败和 void 输入错误都须立即读取本次错误，不能在其他调用之后再读取。
`GetDetailedErrorCode` 是线程局部兼容接口，只表示当前线程最近一次 DLL 成功/失败，不是其他线程或异步任务的结果通道。

`QueryDriverInfo` 返回独立的协议、驱动版本、能力和报告边界；`QueryInputState` 返回驱动权威状态与清理结果。
二者需要当前连接，但查询结果不会把旧文件绑定刷新为新代际。
`GetLastInputResult` 读取调用线程最近一次 primitive 输入的结构化结果，调用前设置 size=48、version=1。
其中 confirmedReports/confirmedX/confirmedY/lastSequence 描述已确认提交部分，flags 区分部分完成、不确定和取消。
相对移动返回已确认的相对量，绝对移动成功返回所提交的主屏坐标；没有确认的部分不能靠补发猜测。
读取结果或状态不会把最近输入结构中的真实错误改写为成功。

报告成功仅表示 VHF 接受，停止成功仅表示驱动侧屏障和中性报告提交完成；不表示目标应用已经观察或处理。
超时、失联、取消竞争或部分完成不会自动重试输入。清理失败只允许明确的收尾重试，不恢复旧持键。
普通状态检查成功不得覆盖原操作失败或首次清理错误。

## 测试边界

Python 单元测试用模拟 DLL 验证绑定、结果传播、任务释放和 GUI 停止顺序，不运行真实输入。
原生边界测试与生产状态核心测试也不能代替 WDF/VHF、签名、权限及电源恢复验收。
真实设备与 Driver Verifier 等设置仅在单独授权的可恢复隔离环境操作；不通过关闭安全机制掩盖失败。
