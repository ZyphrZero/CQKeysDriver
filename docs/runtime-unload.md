# KMDF / VHF 输入与生命周期契约

## 分层与设备

SYS 只维护固定虚拟 HID、请求校验、权威输入状态、提交顺序、会话停止与设备生命周期。
DLL 实现 VK 映射、坐标/单位转换、有限动作任务、组合输入、可取消计时、线程仲裁及错误传播。
Python/C# 调用端负责 UI、配置、热键监听、任务管理、轨迹/业务内容及结果展示。
内核没有脚本、宏解释器、业务定时器、共享内存、任意访问或用户代码执行接口。

- 硬件 ID：`Root\cqkeys_vhf`；固定实例：`ROOT\CQKEYS_VHF\0000`；服务：`cqkeysVhf`。
- INF 将 `vhf` 注册为设备下层过滤驱动，KMDF 1.15 使用系统框架。
- 四个固定 HID 顶层集合独立于实体键鼠：键盘、相对鼠标、绝对指针、Consumer Control。
- ACL 为 `D:P(A;;GA;;;SY)(A;;GA;;;BA)`，设备独占打开；命名事件租约协调 DLL 的设备管理。
  这些机制不构成对抗恶意管理员的边界，也不能阻止管理员强制移除设备。
- 打开文件不产生输入。只读查询不会隐式复位输入；开始/提交/停止需要写权限。
- 普通 QueryRemove 在文件打开期间返回忙。Stop/Unload 仅管理该固定实例，保留 Driver Store 包。

## 版本与兼容

`GetLifecycleVersion()` 返回 DLL 生命周期 ABI 常量 `2`，调用约定为 `cdecl`。
线协议为 `3.0`；驱动产品版本由 `QUERY_INFO` 独立返回。能力位只能声明实际提供的能力。

协议主版本不兼容、未实现的次版本、未知操作/请求标志明确拒绝。本实现只接受 3.0。
调用端不能用 `>= 2` 代替生命周期 ABI 的明确检查。DLL 更新业务时，只要 ABI、协议和所需能力满足，
不要求重新构建 SYS；INF/SYS/CAT 必须来自匹配的完整驱动包。

VK 映射、组合键、任务执行、用户态定时、语言绑定、UI 变化通常只需更新 DLL/调用端。
描述符、键槽/按钮/轴/usage 范围、设备种类、内核协议/权限及内核缺陷修复需要更新相应 SYS。
SYS 改动后仍须重新生成并签署驱动包；只修改 INF 也可能需要重新签署目录。稳定职责不等于永不更新驱动。

## 固定报告与线协议

定义见 `shared/cqkeys_protocol.h` 和 `shared/cqkeys_reports.h`。

| Report ID | 含 ID 字节数 | 含义 |
|---|---:|---|
| 1 | 9 | 8 个修饰位，保留字节，6 个普通 Keyboard usage，0 为未使用 |
| 2 | 7 | 五按钮位图，16 位相对 X/Y，8 位滚轮格数 |
| 3 | 5 | X/Y 各 0–32767 的绝对指针，不携带按钮 |
| 4 | 3 | 一个 16 位 Consumer usage，0 释放，最大 0x03ff |

普通键范围 4–0x73，不接受重复项；相对轴 -32767–32767，滚轮 -127–127。
鼠标按钮高位、报告内部保留位、请求保留字段及报告槽未使用尾部必须为零。
绝对指针不是持键状态，停止不会把指针移回原点。相对位移和滚轮是一次性动作，不能在恢复或重试时重放。
键盘不提供 LED 输出或 Unicode 文本语义，不承诺绕过应用的输入策略。

共同头 `CqRequestHeader` 为 24 字节：`size:u32 / major:u16 / minor:u16 / deviceEpoch:u64 / sessionId:u64`。
结构有明确对齐、零填充和大小/关键偏移断言，无用户指针或变长动作数组。
请求及输出长度必须与该操作的结构完全一致，所有 IOCTL 均为 `METHOD_BUFFERED`。

| IOCTL | 输入 | 输出 | 访问 |
|---|---|---|---|
| QUERY_INFO | 24 字节查询头，token 为零 | 56 字节 CqDeviceInfo | READ |
| BEGIN_SESSION | 24 字节头，当前 epoch，sessionId=0 | 24 字节绑定头 | WRITE |
| SUBMIT_REPORT | 56 字节 CqReportRequest | 32 字节 CqSubmitResult | WRITE |
| STOP_SESSION | 24 字节绑定头 | 88 字节 CqDeviceState | WRITE |
| QUERY_STATE | 24 字节查询头，token 为零 | 88 字节 CqDeviceState | READ |

REPORT 包含非零递增序号、报告长度和固定的 16 字节报告槽。序号允许取消产生的缺号，
但重复或倒退必须拒绝，不提供按序号自动重试/结果重放。内核最多接纳一份尚未终结的报告，超限返回忙。
DLL 每会话只发一份在途报告，先分配序号再提交，形成单一提交通道。

查询结果中的 epoch 是设备当前代际；它不修改文件已绑定的会话代际。
错误状态的 IOCTL 不保证返回输出缓冲，详细诊断应单独查询 STATE，不能把查询成功当作原请求成功。
sessionId/epoch 不是权限凭据；权限、文件绑定和独占所有权共同限制请求。

## 状态与完成语义

文件会话：`Connected → Starting → Active → Stopping → Stopped → Closed`。
初始中性化失败或停止清理失败为 `CleanupFailed`；电源/设备变化导致 `Invalidated`。

- 文件建立时固定连接代际，BEGIN 成功后固定会话编号。初始中性化失败可以重试；
  已停止或失效的文件不能重新 BEGIN，哪怕它尚未成功激活过。
- 只有 Active、文件/代际/会话一致、字段合法的报告可执行。提交前再次检查状态和取消标志。
- SYS 记录最后成功提交的键盘、按钮、Consumer 状态。失败不把候选状态提交为权威状态。
- 相对移动不存成可恢复状态，清理只使用中性报告。
- `lastAcceptedSequence` 表示通过准入的请求，`lastSubmittedSequence` 表示成功提交的报告；
  二者不能当作目标应用已接收的计数。

成功分为以下阶段：

1. 入队不表示提交成功，请求仍可取消或因会话失效而失败。
2. REPORT 成功表示 `VhfReadReportSubmit` 已接受报告；只有此时产生对应序号的提交凭据。
3. 多报告 DLL 操作可能部分完成，已确认部分与失败/不确定部分必须区分，不自动重发位移、滚轮或点按。
4. STOP 成功表示本会话请求已收束、驱动资源同步满足条件，并成功提交了所需中性报告。
5. 以上都不表示 HID/Windows/目标应用已观察、处理或接受输入。不同 HID 集合不承诺应用层原子总序。

## 停止、取消与错误重试

`StopInputProcessing()` 仅停止会话输入，保留设备句柄，不禁用或移除 PnP 节点。
调用端禁止新业务任务的同时立即请求该屏障，不先等本地输入线程退出才首次请求停止。

执行顺序：

1. DLL 关闭原子输入准入、取消本地任务等待；停止通路不受长输入序列的锁阻挡。
2. SYS 先将会话改为 Stopping，拒绝新报告。
3. 独立控制工作项对输入队列执行取消/排空。路由锁覆盖接纳后到入队的窗口，
   防止 STOP 在一份已接纳请求尚未入队时错误完成。
4. 尚未执行的请求取消；已进入同步提交阶段的请求按真实结果终结，不能声称已撤回。
5. 按 Keyboard → Mouse(buttons=0, dx=dy=wheel=0) → Consumer(usage=0) 尝试中性化。
6. 只有全部待清理项成功才返回成功；DLL 收齐实际 OVERLAPPED 完成和任务结束后才关闭句柄。

控制队列不受电源管理，负责查询、建会话和停止；输入队列顺序执行并受电源管理。
同步队列 Purge 只在工作项/文件或设备生命周期的收尾路径执行，不在 IOCTL 回调中同步清空自身队列。
状态锁不跨队列等待、取消完成或同步 VHF 删除。输入回调不转发硬件请求、不保留异步驱动请求，
在短同步 VHF 提交后完成；WDF 在 D0 退出前等待已派发请求终结，不将其重新排队恢复。

每个释放项独立更新：失败时仍保留其状态和待清理位，其他项继续尝试。
重试只处理未成功的中性化，不重放业务输入。首次清理失败和最近清理结果独立保留，
查询成功或后续重试成功不会擦掉首次错误记录；记录生命周期限于当前设备对象。

停止等待请求取消不撤销已经开始的停止过程，之后可查询/重试 STOP 获得状态。
重复 STOP 对已完成的会话幂等；并发停止共享一个状态转换，进行中可明确返回忙。
电源失效后的 STOP 可以成功收尾，但会保留 Invalidated 阶段；只有本次 STOP 成功才证明屏障完成，
不能仅凭一次 STATE 的 Invalidated/零待清理位跳过停止。成功收尾不让旧文件重新具备输入资格。
超时不是完成证明，不允许释放仍被 I/O 使用的缓冲、OVERLAPPED 或句柄。
SetupAPI 和设备 I/O 没有无条件的整体硬实时完成承诺。

DLL 清理失败保留句柄和管理租约，禁止重新 SetHandle 绕过失败；PnP 失败也保留管理权供重试。
租约句柄关闭失败同样返回失败并保留所有权，不丢弃句柄或报告清理成功。
系统明确断开后可以退役失效句柄，但必须保留未确认释放的真实错误，不冒充正常中性化成功。
Python GUI 仅对 `ERROR_DEVICE_NOT_CONNECTED` / `ERROR_NO_SUCH_DEVICE` 继续收尾管理；仍先等待本地工作结束。
输入释放错误与管理清理错误分别保留，管理失败可重试，管理完成也必须明确提示输入释放未确认。
要求重启时返回失败和 `ERROR_SUCCESS_REBOOT_REQUIRED (3010)`，不自动重启。

## 异常退出、恢复与资源

- 异常退出依靠最后设备句柄关闭触发 FileCleanup，由 SYS 权威状态清理，不依赖 DLL、finally 或正常退出。
- FileCleanup 无法向已退出进程返回错误；失败状态留在设备上下文，新连接必须先成功清理才能 Active。
- DLL 不创建可继承的设备句柄。`GetDriverHandle()` 是借用句柄，禁止关闭、混发原始报告或转移到其他进程。
  复制句柄给仍存活的进程会使原进程退出不再等同最后句柄关闭；本模型不支持这种用法。
- D0 退出先使所有旧文件失效并尝试释放；D0 进入递增代际并尝试建立中性基线。
  中性化失败保留诊断和重试入口，不能允许旧会话恢复输入。STATE 的 ready 表示 D0 可用，不替代 Active/清理状态检查。
  D0 退出返回中性化的实际失败状态，可能导致 WDF/PnP 移除或重启设备；设备对象销毁后不能继续查询该对象的错误记录。
  D0 进入的中性化错误保存在状态中，不使旧会话复活；新会话必须重新成功建立中性基线。
- 意外移除不能保证有机会发送释放报告，错误必须如实保留。移除节点不等于输入释放成功。
- PrepareHardware 创建并启动 VHF；ReleaseHardware 关闭准入、收束工作项/输入、分离 VHF 句柄后同步删除，
  不持状态锁或提交锁等待 `VhfDelete(TRUE)`。
- `DllMain` 不等待、不启动输入线程、不操作设备或 SetupAPI。`FreeLibrary` 前必须显式停止、完成所有 API 调用，
  释放任务对象并执行清理；不能在 DLL 线程仍运行时卸载代码。

采用 VHF 默认报告缓冲，不提供自定义异步缓存。代际校验只能证明源驱动不重新执行旧请求，
不能证明已经提交到 VHF/HID/应用中的事件可以撤回或已经排空。
**带按键休眠恢复后没有旧持键重新断言，是必须实测的发布门槛。**不能凭单元测试或代际字段宣称通过。

## DLL 与客户端

生命周期 ABI v2 的管理、键盘、鼠标和 Consumer primitive 保持签名与语义。
Python 使用 `CDLL(..., use_last_error=True)` 并立即读取本次错误；C# 使用 `Cdecl` 和 `SetLastError=true`。
异步任务错误必须从任务结果读取，不能读取提交线程或工作线程的其他 LastError 冒充执行结果。

任务动作在用户态逐份转换为固定报告，内核不接收任务数组。
任务间以及任务和 primitive 的输入所有权必须隔离，冲突明确返回忙，不隐式覆盖或释放他人状态。
动作按住时长和间隔是业务参数，不是“等待 HID 已接收”的同步条件；不承诺 Windows 硬实时调度。

Load/Stop/Unload 仍显式管理固定节点。只连接已启用设备可调用 SetHandle。
Stop 禁用节点，Unload 移除节点，二者均保留包；没有管理会话的清理不操作其他客户端设备。
驱动包和签名策略由部署流程管理，客户端不会自动关闭安全机制、导入信任或批量清包。

### 启动失败诊断

驱动包安装成功不等于设备已成功启动。`LoadNTDriver` 在启用固定节点后等待接口，
每次接口尚未出现时通过 `CM_Get_DevNode_Status` 检查 `DN_HAS_PROBLEM`，并读取
`DEVPKEY_Device_ProblemStatus`。已报告的失败立即返回，不再统一等待 5 秒后报 1460；
到达等待期限时也检查设备状态。未报告设备问题且接口一直不出现时仍返回 `ERROR_TIMEOUT`。

- 失败 NTSTATUS 通过系统 `RtlNtStatusToDosError` 转换，保留原有 `GetLastError` 调用约定。
  例如 `0xC0000603 (STATUS_IMAGE_CERT_REVOKED)` 映射为 `0x800B010C (CERT_E_REVOKED)`，
  C# 的有符号整数显示为 `-2146762484`，并保留完整十六进制值。
- `CM_PROB_NEED_RESTART` 返回 3010，不自动重启；没有可用失败 NTSTATUS 时按已知 Problem Code
  报告重启、未签名或禁用，否则返回 `ERROR_GEN_FAILURE`，不把缺少详细原因当作成功。
- 查询失败或属性类型/长度错误显式返回错误。可选 Problem Status 不存在时，仍报告已知的
  Problem Code，并在诊断中记录属性查询失败；未知 NTSTATUS 保留原值和系统转换结果（无映射时通常为 317，自定义状态可能直接透传）。

新增可选导出 `BOOL __cdecl GetLastDriverStartResult(CqDriverStartResult* result)`，生命周期 ABI 仍为 2。
结构为 24 字节：`size:u32 / version:u32 / error:u32 / problemCode:u32 / problemStatus:i32 / diagnosticError:u32`。
调用方先设置 `size=sizeof(CqDriverStartResult)`、`version=CQ_DRIVER_START_RESULT_ABI (1)`。
在调用 `LoadNTDriver` 的同一线程立即读取：`error` 为本次启动的系统错误，`problemCode` 为 PnP 问题码，
`problemStatus` 为原始 NTSTATUS；`diagnosticError` 非零表示诊断查询未完成，不能把零状态解释为成功。
未发现问题时 `problemCode=0`；每次 Load（包括参数或租约检查失败）都会清空上次诊断，
其他线程和清理操作不覆盖本线程快照。成功读取快照不修改原生调用线程进入该函数时的 LastError 或兼容错误码。
托管运行时可能在 P/Invoke 前重置错误缓存，因此 C# 必须先保存 `Marshal.GetLastWin32Error()`，再读取快照。

C# 在安装工作线程上读取并显示 Problem Code/Status；确认 `0xC0000603` 时提示 Windows 吊销拦截，
而不是推测等待不足。应同步部署新 C# 程序和新 DLL（DLL 放在 EXE 同目录）；旧 DLL 缺少该导出时，
C# 保留原始安装错误并明确提示更新 DLL，不伪造诊断结果。旧客户端无需调用新增导出，仍可读取改进后的系统错误。
本改动不解除签名拦截，也不要求修改 SYS；真正恢复加载仍需符合内核签名策略的完整 INF/SYS/CAT 包。

## 验证层级

### 不接触真实设备的本地检查

- 协议 sizeof/offsetof、四个描述符长度、未知操作/版本、长度、保留位、范围和重复键。
- 真实生产状态核心：成功后才更新状态、全部清理失败组合、逐项重试、停止终态和旧代际不可复活。
- 真实 DLL 代码加可控 Win32 I/O/PnP 边界：挂起完成、取消竞争、部分移动、错误保留、并发停止、关闭失败重试。
- DLL 任务：仲裁、取消等待、持键释放、结果保留和显式运行时清理；Python 薄绑定和 GUI 停止顺序。
- 启动诊断：吊销/其他 NTSTATUS、缺失或畸形属性、查询失败、延迟枚举、真正超时及期限边界；快照 ABI、线程隔离、重试清空与 C# 原始错误展示。
- 配套工具齐备时编译 Debug/Release x64 DLL、SYS、C#；可用 SignMode=Off 做未签名构建检查，不安装产物。

这些检查不能证明真实 WDF 取消、ACL、HID 接收、设备枚举、电源恢复或签名信任已通过。
编译工具缺失必须如实报告，不能通过修改系统设置或伪造边界结果替代。

### 另行授权的隔离 Windows 验收

需要正确签名与信任、恢复手段，并明确允许真实设备、电源及验证器操作；本清单不表示已经执行。

| 场景 | 验收条件 |
|---|---|
| 输入顺序 | 关联 DLL 序号、SYS 提交及专用接收器；组合键正确、无重复位移，分别报告提交和实际接收 |
| 异常退出 | 持键/按钮/媒体、排队和在途时终止进程；最后句柄关闭后清理，失败可查且新会话不带脏状态 |
| 并发停止 | 持续输入、大位移与任务中同时 Stop/Unload；屏障后无新业务，队列和引用收束，无死锁/双完成 |
| 取消/超时 | 提交前后竞争、部分位移、等待取消；确认与不确定部分分开，不重放业务 |
| 休眠/恢复 | 持键、排队输入、旧句柄查询新代际后再提交；旧会话始终无效，新会话中性，无旧持键重新断言 |
| 重复生命周期 | 至少 100 次会话循环和 20 轮 Load→Stop→Load→Unload；无重复节点或持续资源增长 |
| 权限与协议 | 普通用户、只读写入、第二客户端、跨文件 token、未知码/畸形字段均不改变输入状态 |
| 清理故障 | 提交/释放失败、意外移除、关闭/PnP 失败与重启要求；保留真实错误、重试资源及未确认释放 |
| DLL 重载 | 所有任务/API 结束并显式清理后 FreeLibrary；重新加载从新会话开始，无活动线程执行已卸载代码 |

并发用事件、屏障和真实完成通知断言，不用固定 sleep 证明“应该已经完成”。
Driver Verifier、KMDF 验证、调试器和电源测试只在另行授权的隔离系统执行；不增加发布驱动故障注入入口。
不满足默认 VHF 缓冲的恢复验收时必须阻断发布并回报证据，不以未经验证的缓存优化或固定延时掩盖。

## 官方依据

- [CM_Get_DevNode_Status](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status)
- [CM_Get_DevNode_PropertyW](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_propertyw)
- [RtlNtStatusToDosError](https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-rtlntstatustodoserror)
- [VHF 提交、默认缓冲与删除](https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
- [WDF 请求取消](https://learn.microsoft.com/windows-hardware/drivers/wdf/canceling-i-o-requests)
- [电源管理队列](https://learn.microsoft.com/windows-hardware/drivers/wdf/using-power-managed-i-o-queues)
- [队列管理与排空](https://learn.microsoft.com/windows-hardware/drivers/wdf/managing-i-o-queues)
- [IO 回调中同步队列操作限制](https://learn.microsoft.com/windows-hardware/drivers/devtest/noioqueuepurgesynchronously)
- [设备回调失败与 PnP 重启](https://learn.microsoft.com/windows-hardware/drivers/wdf/reporting-device-failures)
