# CqKeys DLL 接口文档

接口使用 KMDF + VHF **ABI v2**，需要同版本 DLL 和包含 INF/SYS/CAT 的正确签名包。
安装、权限、输入范围和验收限制见
[项目说明](../README.md) 与 [生命周期契约](../docs/runtime-unload.md)。

## 接口列表

### 驱动管理接口

| 函数名 | 参数列表 | 返回值类型 | 功能描述 | 示例 |
|--------|----------|------------|----------|------|
| LoadNTDriver | const char* name, const char* path | BOOL | 安装/启用固定 PnP 节点；SYS 路径要求同目录 INF | LoadNTDriver("cqkeys", "cqkeys.inf") |
| StopNTDriver | const char* name | BOOL | 释放输入并禁用节点，保留包 | StopNTDriver("cqkeys") |
| UnloadNTDriver | const char* name | BOOL | 移除节点，保留 Driver Store 包 | UnloadNTDriver("cqkeys") |
| GetLifecycleVersion | void | DWORD | 必须为 2 | GetLifecycleVersion() |
| StopInputProcessing | void | BOOL | 拒绝新输入并复位，保留句柄 | StopInputProcessing() |
| SetHandle | void | BOOL | 设置驱动句柄 | SetHandle() |
| GetDriverHandle | void | HANDLE | 获取驱动句柄 | GetDriverHandle() |
| GetDriverStatus | void | DEVICE_STATUS | 获取驱动状态 | GetDriverStatus() |
| GetLastCheckTime | void | ULONGLONG | 获取上次检查时间 | GetLastCheckTime() |

### 键盘操作接口

| 函数名 | 参数列表 | 返回值类型 | 功能描述 | 示例 |
|--------|----------|------------|----------|------|
| KeyDown | USHORT VirtualKey | void | 模拟键盘按键按下 | KeyDown(0x41) // 按下A键 |
| KeyUp | USHORT VirtualKey | void | 模拟键盘按键抬起 | KeyUp(0x41) // 抬起A键 |
| ConsumerKeyDown | USHORT usage | void | 按下 Consumer Control usage | ConsumerKeyDown(0xCD) |
| ConsumerKeyUp | USHORT usage | void | 释放同一 Consumer Control usage | ConsumerKeyUp(0xCD) |

### 鼠标操作接口

| 函数名 | 参数列表 | 返回值类型 | 功能描述 | 示例 |
|--------|----------|------------|----------|------|
| MouseMoveRELATIVE | LONG dx, LONG dy | void | 鼠标相对移动 | MouseMoveRELATIVE(10, 20) |
| MouseMoveABSOLUTE | LONG dx, LONG dy | void | 主显示器像素坐标；不支持虚拟多屏绝对定位 | MouseMoveABSOLUTE(100, 200) |
| MouseLeftButtonDown | void | void | 鼠标左键按下 | MouseLeftButtonDown() |
| MouseLeftButtonUp | void | void | 鼠标左键抬起 | MouseLeftButtonUp() |
| MouseRightButtonDown | void | void | 鼠标右键按下 | MouseRightButtonDown() |
| MouseRightButtonUp | void | void | 鼠标右键抬起 | MouseRightButtonUp() |
| MouseMiddleButtonDown | void | void | 鼠标中键按下 | MouseMiddleButtonDown() |
| MouseMiddleButtonUp | void | void | 鼠标中键抬起 | MouseMiddleButtonUp() |
| MouseXButton1Down | void | void | 鼠标X1键按下 | MouseXButton1Down() |
| MouseXButton1Up | void | void | 鼠标X1键抬起 | MouseXButton1Up() |
| MouseXButton2Down | void | void | 鼠标X2键按下 | MouseXButton2Down() |
| MouseXButton2Up | void | void | 鼠标X2键抬起 | MouseXButton2Up() |
| MouseWheelUp | USHORT wheelDelta | void | 鼠标滚轮向上 | MouseWheelUp(120) |
| MouseWheelDown | USHORT wheelDelta | void | 鼠标滚轮向下 | MouseWheelDown(120) |

## 状态码说明

### DEVICE_STATUS 枚举值
| 值 | 含义 |
|----|------|
| DEVICE_STATUS_UNKNOWN (0) | 设备状态未知 |
| DEVICE_STATUS_READY (1) | 设备就绪 |
| DEVICE_STATUS_ERROR (2) | 设备错误 |

## 使用注意事项
1. 使用前必须先调用 `LoadNTDriver` 加载驱动
2. 加载驱动后需要调用 `SetHandle` 获取设备句柄
3. 所有操作前建议通过 `GetDriverStatus` 检查驱动状态
4. 程序退出前应调用 `UnloadNTDriver` 卸载驱动
5. 鼠标移动坐标使用屏幕坐标系(左上角为原点)
6. 键盘操作使用Windows虚拟键码(Virtual-Key Codes)

## 错误处理
- 所有返回BOOL类型的函数，返回FALSE表示操作失败；void 输入接口也须立即读取本次 GetLastError
- 失败时可通过 `GetLastError()` 获取具体错误码
- 建议实现错误重试机制，特别是对于驱动加载和设备句柄获取操作

## 线程安全性
- 所有接口都是线程安全的
- 调用方仍需同步完整的按下/释放序列，避免多个任务交叉操作同一按键
- 加载/卸载是同步操作，GUI 应在后台执行；卸载前先取消并等待自己的输入任务


## 调试&蓝屏分析

### WinDbg  
1. 配置系统生成完整转储：
```
1. 右键"此电脑" -> 属性
2. 高级系统设置 -> 高级 -> 启动和故障恢复 -> 设置
3. 在"写入调试信息"下选择"完整内存转储"
4. 确保"自动重新启动"已勾选
```
2. 安装调试工具：
```
1. 下载并安装 WDK (Windows Driver Kit)
2. 安装 Windows SDK
3. 安装 WinDbg (Windows Debugger)
```

3. 使用 WinDbg 分析：
```
// 使用powershell管理员方式执行以下命令安装 WinDbg
winget install Microsoft.WinDbg

1. 打开 WinDbg
2. File -> Open Crash Dump
3. 选择 C:\Windows\MEMORY.DMP 或 Minidump 文件
```
4. 在命令窗口输入：
```
   !analyze -v    # 详细分析崩溃原因
   .bugcheck      # 显示蓝屏代码
   kb             # 显示调用栈
   lmvm cqkeys    # 显示我们的驱动信息
```

### 2. BlueScreenView
1. 使用 BlueScreenView（更简单的方法）：
```
1. 下载并安装 BlueScreenView
2. 运行后自动显示所有蓝屏记录
3. 查看：
   - 蓝屏代码
   - 发生时间
   - 导致崩溃的驱动
   - 调用栈信息
```
2. 在驱动代码中添加详细日志
```
KdPrint(("Driver State: %d, IRQL: %d\n", state, KeGetCurrentIrql()));
```

3. 配置本地内核调试：
```
bcdedit /debug on
bcdedit /dbgsettings local
bcdedit /set TESTSIGNING ON
```

4. 收集到的信息应该包括：
```
需要提供的信息
{
    1. 基本信息:
    - 蓝屏代码 (例如: 0x0000007E)
    - 发生时间
    - 系统版本
    
    2. 详细信息:
    - 导致崩溃的驱动名称
    - 崩溃时的调用栈
    - 相关的内存地址
    
    3. 环境信息:
    - 系统配置
    - 已安装的驱动
    - 硬件信息
}
```

### 完整验证（推荐用于开发测试）
verifier /flags 0xFF /driver cqkeys.sys

### 基本验证（推荐用于日常测试）
verifier /standard /driver cqkeys.sys

### 内存验证（针对内存问题）
verifier /flags 0x5 /driver cqkeys.sys

### IRQL验证（针对IRQL问题）
verifier /flags 0x2 /driver cqkeys.sys

### 异常退出后处理

重新启动 ABI v2 客户端取得管理会话，然后正常移除 PnP 设备；
或由管理员在设备管理器中核实 `ROOT\CQKEYS_VHF\0000` 后单独处理该节点。
设备清理仅针对该实例。详见生命周期契约中的隔离验收要求。
