# LingYaoDriver

## 本地构建

已验证的构建环境：Visual Studio 2026、MSVC v143（14.44）、Windows SDK / WDK 28000.2526。
在 Visual Studio Installer 中安装“使用 C++ 的桌面开发”、MSVC v143 和“Windows Driver Kit”组件，
并安装配套的 Windows SDK 与 WDK。

在仓库根目录使用 PowerShell 构建 x64 Debug 版本：

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
& $msbuild .\lykeysdll\lykeysdll.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild .\lykeys\lykeys.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

WDK 28000 的 INF 验证需要使用上面的 **64 位 MSBuild**。
输出文件分别位于 `lykeysdll/x64/Debug/lykeysdll.dll` 和 `lykeys/x64/Debug/lykeys.sys`；
驱动安装包位于 `lykeys/x64/Debug/lykeys/`。构建成功不代表驱动已安装或加载。

## Python示例

1. 开启 **测试模式** & **禁用强制驱动签名** 模式 & **重启**  
（适用于编译出来的sys驱动没有经过签名的情况）
```shell
# 使用 `管理员身份` 打开 `CMD` 输入以下命令
bcdedit /set nointegritychecks on
bcdedit /set testsigning on
shutdown  -r -t 0
```

2. 重启后**管理员身份**运行Python脚本  
  - 使用`pip`或`conda`安装win32gui库  
    `conda install win32gui`  

**分为两种模式运行：**  
  - 正常模式：`python gui.py`  
  - DeBug模式：`python gui.py --debug`  

3. 高频按键需要注意  
  - Python的 `time.sleep()` 精度问题  
    - windows下 `time.sleep(0.001)` 的最小精度约为15ms，需要使用 `time.perf_counter()` 作为高精度计时器，可以精确到纳秒  

## 声明
_本项目遵循 AGPLv3 开源协议基于以下原始项目修改:_
   - [kmclassdll.dll](https://github.com/BestBurning/kmclassdll/releases) - DLL动态库
   - [kmclass.sys](https://github.com/BestBurning/kmclass/releases) - 内核驱动

## 参考资料

- 如何编译可参考 [编译dll并在python中使用ctypes调用](https://di1shuai.com/%E7%BC%96%E8%AF%91dll%E5%B9%B6%E5%9C%A8python%E4%B8%AD%E4%BD%BF%E7%94%A8ctypes%E8%B0%83%E7%94%A8.html)
- 错误代码 [Error Codes](https://docs.microsoft.com/zh-cn/windows/win32/debug/system-error-codes)
- [KMDF Hello World](https://docs.microsoft.com/zh-cn/windows-hardware/drivers/gettingstarted/writing-a-very-small-kmdf--driver)
- [WDK 10](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) 
