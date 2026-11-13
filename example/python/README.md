# Python 演示

需要 Windows、64 位 Python 3.9+、ABI v2 的 x64 `cqkeysdll.dll`。仅使用 Python 标准库。
以管理员身份打开 PowerShell，再从仓库根目录运行；脚本不限制物理机、虚拟机或生产环境。
内核驱动故障可能导致蓝屏和业务中断，建议先在隔离环境验证，并为目标机器准备恢复方式。
脚本不会更改签名策略、导入证书或自动重启。

安装完整驱动包、连接并查询状态，不发送测试输入：

```powershell
python .\example\python\demo.py --inf "C:\DriverPackage\cqkeys.inf"
```

`C:\DriverPackage` 必须包含匹配的 `cqkeys.inf`、`cqkeys.sys` 和正确签名的 `cqkeys.cat`。
文件存在检查不代表签名受信任；安装结果以 Windows 返回值为准。不能仅提供一个签名 SYS。
DLL 默认使用仓库的 `cqkeysdll/x64/Release/cqkeysdll.dll`；可以通过 `--dll` 指定路径。

发送一个 A 键、一次鼠标移动或一个媒体键，每次只选择一种操作：

```powershell
python .\example\python\demo.py --inf "C:\DriverPackage\cqkeys.inf" --tap-key 0x41 --delay 5
python .\example\python\demo.py --inf "C:\DriverPackage\cqkeys.inf" --move 100 0
python .\example\python\demo.py --inf "C:\DriverPackage\cqkeys.inf" --consumer 0xCD
```

在倒计时内将焦点切到专用测试窗口。A 键实际字符由系统键盘布局决定；`0xCD` 会触发播放/暂停。
不传 `--inf` 时仅连接已安装且已启用的设备，但连接、清理仍会发送全释放报告。

退出时默认释放输入并**禁用 PnP 节点**；再次使用可传入 `--inf` 启用。
指定 `--uninstall` 则移除 PnP 节点，两种方式均保留 Driver Store 中的包。
失败会打印操作名和 Win32 错误码；清理失败也会单独报告并返回非零退出码。

`SetHandle` 返回 `0xE000020B (ERROR_NO_SUCH_DEVINST)` 时，表示找不到指定设备实例。
当前版本使用 `ROOT\CQKEYS_VHF\0000`；没有传入 `--inf` 的运行只连接设备，不执行安装。
应先用匹配且签名有效的完整驱动包安装/启用该实例，不能将设备 ID 改为其他实例来替代安装。
失败后的“管理会话已清理”仅说明清理流程完成，不表示驱动安装、连接或加载成功。

查看参数不会访问驱动：

```powershell
python .\example\python\demo.py --help
```
