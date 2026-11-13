# KMDF / VHF 生命周期与验证

## 设备与安全边界

- 驱动包：`cqkeys.inf` / `cqkeys.sys` / `cqkeys.cat`，必须来自同一构建并正确签名。
- 硬件 ID：`Root\cqkeys_vhf`；固定实例：`ROOT\CQKEYS_VHF\0000`；服务：`cqkeysVhf`。
- INF 安装 `vhf` 为设备下层过滤驱动；KMDF 1.15 使用系统内置框架，不分发 co-installer。
- INF 的设备 ACL 为 `D:P(A;;GA;;;SY)(A;;GA;;;BA)`。没有 Everyone/普通用户写权限。
- 状态查询需读权限，提交/复位需写权限，全部 IOCTL 使用 `METHOD_BUFFERED`。
- DLL 按设备接口 GUID 枚举并打开固定实例。
- 设备独占；跨进程 DLL 会话通过带 ACL 的命名事件租约互斥，不依赖线程绑定的 mutex ownership。
  未获得租约的客户端无法通过失败初始化后的 cleanup 移除别人的设备。
- 内核使用顺序、受电源管理的 WDF 队列，并用 PASSIVE_LEVEL wait lock 串行化文件清理、
  IOCTL 和电源回调；只有成功打开的文件对象可以提交输入。打开期间拒绝普通 QueryRemove。
  这些措施不阻止另一个管理员主动执行强制移除，也不承诺对抗管理员权限攻击者。

## 生命周期 ABI v2

DLL 使用 `cdecl` 调用约定；`GetLifecycleVersion()` 返回 `2`。
客户端、DLL 和驱动协议版本必须一致。

1. `LoadNTDriver("cqkeys", infPath)`：取得会话租约、校验 INF 类、创建/复用固定根节点，
   校验硬件 ID，将签名包放入 Driver Store，仅为该实例安装从指定 INF 选择的兼容驱动，
   启用节点并等待设备接口可打开。不会按硬件 ID 批量更新其他设备，也不修改其他服务。
2. SYS 路径参数用于定位同目录同名 INF；安装要求完整的 INF/SYS/CAT 驱动包。
3. `SetHandle()`：独占打开设备接口，验证状态结构长度、版本、D0 就绪状态和代际。
4. 输入同步提交完整 HID 状态。只有 IOCTL 成功，DLL 才更新自己的按键/鼠标/媒体状态。
   IOCTL 成功表示 VHF 接收，不代表应用程序已经观察到输入。
5. 调用方先停止生产输入并等待自己的工作线程退出。
6. `StopInputProcessing()`：拒绝新输入，提交全部释放报告，保留句柄，可重试。
7. `StopNTDriver("cqkeys")`：复位输入、关闭句柄、禁用 PnP 节点，并验证 `CM_PROB_DISABLED`。
8. `UnloadNTDriver("cqkeys")`：复位、关闭后通过 `DiUninstallDevice` 移除节点，确认不再存在，
   保留 Driver Store 包。完成后释放会话租约；同一 DLL 重复调用为无操作成功。
9. 无会话的 Stop/Unload 不操作系统；要管理已有节点，先 Load 或 SetHandle 获取管理会话。

安装失败可能留下已注册但未启动的根节点或已暂存的包；错误不伪装为成功，也不自动删除
已有包。保留管理对象调用 Unload 重试清理；缺失包/权限问题修复后也可重新 Load。
系统要求重启时返回 `FALSE` + `ERROR_SUCCESS_REBOOT_REQUIRED (3010)`，不会自动重启。
接口就绪轮询最多 5 秒，但同步 SetupAPI/设备 I/O 本身没有整个操作的硬超时。

### 清理与电源

- DLL 复位失败保留句柄及租约，拒绝重新 SetHandle，必须重试清理。
- 若系统明确返回设备断开/不存在，关闭失效设备句柄但仍返回该错误并保留租约；
  下一次清理重试可确认 PnP 状态后完成，避免永久保留已移除设备的句柄。
- PnP 失败也保留租约供重试，不把“已关闭句柄”当作“已卸载设备”。
- 进程退出由内核 `EvtFileCleanup` 尝试复位。该回调无法向已退出进程返回错误；
  最后一次复位状态保存在查询结构中，新会话打开时必须再次成功复位。
- D0 退出复位输入，D0 进入复位并递增代际。请求代际不符时拒绝输入；
  调用端必须重新建立生命周期，不能把休眠前的完整状态在恢复后重新提交。
- `VhfCreate/VhfStart` 随 PrepareHardware 创建虚拟设备，ReleaseHardware 在排空请求后
  同步 `VhfDelete`。采用 VHF 默认报告缓冲，不另设未经验证的自定义异步缓存。
- 不在 `DllMain` 中执行卸载、等待或 SetupAPI。必须在 `FreeLibrary` 前完成清理；
  未清理直接卸载 DLL 可能令句柄/租约保留到进程退出。
- `GetDriverHandle()` 返回借用句柄，禁止调用方关闭或绕过 DLL 直接混发原始报告。

## 线协议

定义位于 `shared/cqkeys_protocol.h`，HID descriptor 位于 `shared/cqkeys_reports.h`。
固定宽度数据、零保留字节；没有用户指针。驱动拒绝未知 IOCTL、错误长度、版本、报告 ID、
重复普通键、非法范围及非零尾部数据。

| Report ID | 数据长度（含 ID） | 内容 |
|---|---:|---|
| 1 | 9 | 修饰键位图、保留字节、6 个 Keyboard Usage |
| 2 | 7 | 五键位图、16 位相对 X/Y、8 位有符号滚轮格数 |
| 3 | 5 | 主屏绝对 X/Y，各 0–32767，不携带按钮状态 |
| 4 | 3 | 单个 16 位 Consumer Usage，0 为释放，最大 0x03ff |

`CqReportRequest` 为 28 字节，含 size/version/generation/reportLength/报告/保留字节；
`CqDeviceStatus` 为 20 字节，含 size/version/generation/ready/lastStatus。
键盘不提供 LED 输出报告；不模拟实体设备身份，不提供绕过应用输入策略的保证。

## 客户端与包部署

Python 以 `CDLL(path, use_last_error=True)` 加载，并立即用 `ctypes.get_last_error()`
读取 `void` 输入调用结果。C# P/Invoke 需要 `CallingConvention.Cdecl`、`SetLastError=true`。
GUI 通过任务取消和等待机制停止输入，清理失败时保留管理对象供用户重试。

```text
CqKeys.exe
cqkeysdll.dll          ABI v2 x64 DLL
dll/
  cqkeys.inf          签名驱动包
  cqkeys.sys
  cqkeys.cat
```

源码仓库不分发编译后的二进制。部署时使用同一次构建生成的 DLL、INF、SYS 和 CAT，
并按目标系统要求完成签名。
移除节点不等于删除 Driver Store 包。确需彻底清包时，管理员先用 `pnputil /enum-drivers`
核实提供者、版本及对应 `oemNN.inf`，再对确认的单个包操作；不要自动遍历删除同类驱动。

## 验证层级

本地验证：DLL/C# 构建、驱动编译与链接、未签名 INF/目录生成、Python 单元测试，
原生协议/descriptor 长度和 DLL 并发生命周期测试。测试中的 I/O 和 PnP 适配器是假的，
仅验证调用、状态、错误传播；**不代表实际安装、权限、HID 枚举或输入已通过**。
缺少受信任签名时不能做实际安装验收；不能通过关闭系统安全机制掩盖失败。

必须在获授权、具备签名信任和恢复手段的隔离 x64 Windows 测试机补做：

- 全新安装及失败安装重试；检查固定父节点、四个 HID 集合、`vhf` 下层过滤驱动。
- 无实体键盘/鼠标的虚拟机中验证输入；实体设备插拔不影响虚拟输入。
- 普通用户被拒绝；管理员可连接；第二客户端因租约/独占拒绝且不能误卸载首客户端。
- 畸形 IOCTL、长度、版本、权限、保留字节、报告 ID、重复键与边界值的实际内核拒绝。
- 左右修饰键、方向键与数字键盘、6 键上限、五个鼠标按钮、正负相对移动、主屏绝对位置、
  滚轮及媒体键。观察实际接收事件，而不只查看 DeviceIoControl 返回值。
- 持续输入时停止、任务取消、复位失败重试、进程被终止、持有外部句柄时移除失败。
- 至少 20 轮 Load → 输入 → Stop → 重载 → Unload，检查 PnP 状态与设备重新打开。
- 带按键休眠/唤醒、禁用/启用、意外移除与重启要求；旧代际输入不能恢复旧按键。
- Driver Verifier、KMDF 验证、内核调试器及内存/资源检查，无悬空请求或 VHF 句柄泄漏。
