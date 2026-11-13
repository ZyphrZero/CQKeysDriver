# 按需加载与运行时卸载

本分支保留 **non-PnP + SCM**，不创建 PnP 设备节点，不改变系统签名策略。
目标是让正常停止、卸载和重试具有明确结果，而不是强制移除仍在使用中的驱动。

## 生命周期契约

所有输入调用与设备句柄的关闭在 `cqkeysdll` 中串行化。Python 与 C# 共用这个实现。
同一进程只允许一个生命周期管理者；不要同时使用多份 DLL 副本管理同一个设备。

1. `LoadNTDriver("cqkeys", sysPath)`：创建或复用服务，查询到 `SERVICE_RUNNING` 后才成功。
   复用服务前校验其类型及二进制路径；路径冲突返回错误，不删除、覆盖或重启已有服务。
2. `SetHandle()`：打开设备并检查键盘、鼠标回调是否可用。
3. 输入调用：同步发送；成功按下的键和鼠标按钮会被记录。原有 `void` 输入导出不改 ABI，
   可通过本次调用的线程局部 `GetLastError()` 检查错误。Python 使用 `use_last_error=True`。
4. 停止产生新输入，并等待调用方的输入线程退出。
5. 二选一：
   - `StopNTDriver("cqkeys")`：释放记录的按键，关闭句柄，等待 `SERVICE_STOPPED`，保留服务注册。
   - `UnloadNTDriver("cqkeys")`：执行上述停止流程，再删除服务注册并确认服务条目消失。
6. 以后可再次 `LoadNTDriver` + `SetHandle`。重复停止或卸载不存在的服务成功返回。

`StopInputProcessing()` 可单独阻止新输入并释放按键，但不会关闭句柄或停止服务。
调用后应继续执行 Stop/Unload，再重新加载；不能用 `SetHandle` 绕过未完成的清理。
`GetLifecycleVersion()` 返回 `1`。客户端会拒绝没有对应生命周期接口的旧 DLL。

### 失败与重试

- 原生生命周期函数返回 `FALSE` 时，立即读取 `GetLastError()`。查询失败、权限拒绝、
  服务仍在停止、按键释放失败、服务条目等待删除等情况，都不会被报告为成功。
- 按键释放失败时保留句柄和未释放记录，供下次停止/卸载重试；不直接关闭后丢弃状态。
- 每轮 SCM 状态等待最多 5 秒；这是**轮询超时**，不是整个卸载过程的硬性上限。
  `ControlService` 或同步设备 I/O 自身仍可能阻塞，GUI 因此在后台执行卸载。
- 其他进程或手动取得的设备/服务句柄可能阻止卸载或注册信息删除。先让持有者正常关闭，
  再重试。不要强行删除驱动文件，也不要把“标记删除”当作已经卸载。
- `GetDriverHandle()` 只返回借用句柄：调用方不得关闭它或绕过 DLL 并发提交原始 I/O。
- 不在 `DllMain` 或终结器内等待、卸载驱动。调用方必须在 `FreeLibrary` 或退出前显式清理。
  进程被强杀/系统崩溃不在正常退出保证范围内。

## 客户端行为

### Python

`CQKeysGUI` 统一跟踪输入线程。卸载时取消等待、拒绝新任务、等待旧任务退出，然后调用原生卸载。
Tk 主线程保持响应；卸载失败保留窗口和管理对象，允许再次点击卸载。旧任务未退出时不允许重载。

程序调用示例：

```python
manager.cleanup(uninstall=False)  # 停止驱动，保留服务注册
manager.cleanup()                 # 停止并删除服务注册
```

若自己编写客户端，仍需先停止自己的输入任务。`InputTester.stop()` 可使旧测试器拒绝后续输入。

### C#

管理器改为 P/Invoke 调用同一个 DLL，不再另开一个独占设备句柄，也不再维护另一套 SCM 逻辑。
应用编译为 x64。窗口关闭时等待卸载，失败不关闭；操作进行中禁止重复点击。

发布目录：

```text
CqKeys.exe
cqkeysdll.dll       # 本分支构建的 x64 DLL，放在 EXE 旁
 dll/
   cqkeys.sys      # 本分支构建并正确签名的驱动
```

可将 DLL 和签名 SYS 放入 `cqkeys_csharp/dll/` 后重新构建；项目会把 DLL 复制到 EXE 旁，
SYS 保留在输出目录的 `dll/` 下。不从当前工作目录搜索 DLL。

## 构建与测试

沿用根目录 README 的 64 位 MSBuild/WDK 构建方法。
**仓库 `cqkeyspy_test/cqkeysdll/` 中的历史二进制不会被自动更新，不能代表本次重构。**
Python 应选择新构建的 `cqkeysdll/x64/Debug/cqkeysdll.dll` 及同版本、正确签名的 SYS。

在仓库根目录，PowerShell：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe -m compileall -q cqkeyspy_test

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild tests/native_lifecycle_test.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
& .\tests\bin\native_lifecycle_test.exe
```

Python 测试覆盖失败传播、重试、任务取消和 GUI 状态转换，不创建真实窗口。
原生测试编译实际 DLL 实现，替换 Win32 I/O 与 SCM 调用，验证状态机、按键释放及并发关闭顺序。
**这些测试不安装驱动、不发送真实键鼠输入，也不能替代内核集成验证。**

## 必须在隔离测试机补做的验证

使用已授权、配置好签名信任的测试机；不要通过关闭安全机制掩盖加载失败。

- 连续执行至少 20 次加载、输入、停止、重载、卸载；确认 SCM 实际状态及设备是否可再次打开。
- 高频按键、鼠标长按和延迟输入任务运行时点击卸载；确认无卡键、无旧任务在重载后继续发送。
- 关闭窗口时执行相同检查；失败时窗口应保留并展示错误。
- 保留其他进程的设备句柄，验证卸载明确失败；释放句柄后重试。
- 保留服务查询句柄，验证注册信息删除超时而不是假成功；关闭该句柄后重试。
- 检查 Driver Verifier/内核调试器日志；注意最终验证可能需要重启测试机。

## 保留的架构限制

当前输入仍依赖扫描键鼠驱动内部设备扩展寻找回调，未改成受支持的过滤驱动或 VHF。
本次增加输入长度检查、回调就绪检查、消费结果检查及正确调用 IRQL，并移除了卸载中的固定延时，
但**不承诺实体设备热插拔、休眠、系统升级或任意时刻的强制卸载安全**。
当前驱动没有自建工作线程、定时器或异步 IRP 队列；卸载依靠 I/O 管理器的引用和 IRP 生命周期规则。
以后增加异步工作时，必须同步更新清理协议与测试，不能用 Sleep 替代等待完成。
