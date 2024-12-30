# KMDLL

## 声明
_本项目遵循 AGPLv3 开源协议基于以下原始项目修改:_
- kmclass: `https://github.com/BestBurning/kmclass`
- kmclassdll: `https://github.com/BestBurning/kmclassdll`

## Demo运行
1. 运行
  - debug模式：`python gui.py --debug`
  - 正常模式：`python gui.py`

## kmclass驱动程序

### 编译
建议使用[Visual Studio 2019](https://visualstudio.microsoft.com/zh-hans/vs/)进行编译

### 资料

- 错误代码 [Error Codes](https://docs.microsoft.com/zh-cn/windows/win32/debug/system-error-codes)
- [KMDF Hello World](https://docs.microsoft.com/zh-cn/windows-hardware/drivers/gettingstarted/writing-a-very-small-kmdf--driver)
- [WDK 10](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) 

## kmclassdll动态库

### 编译
建议使用[Visual Studio 2019](https://visualstudio.microsoft.com/zh-hans/vs/)进行编译，
如何编译可参考 [编译dll并在python中使用ctypes调用](https://di1shuai.com/%E7%BC%96%E8%AF%91dll%E5%B9%B6%E5%9C%A8python%E4%B8%AD%E4%BD%BF%E7%94%A8ctypes%E8%B0%83%E7%94%A8.html)


### python 示例

示例文件为 [python_examples.py](https://github.com/BestBurning/kmclassdll/blob/master/python_examples.py)

1. 自行编译或下载
   - [kmclassdll.dll](https://github.com/BestBurning/kmclassdll/releases) - 动态库
   - [kmclass.sys](https://github.com/BestBurning/kmclass/releases) - 驱动

2. 修改变量路径为你的真实路径
   - `dll_path`
   - `driver_path`

```
dll_path = 'YourPath\\kmclassdll.dll'
driver_path = b'YourPath\\kmclass.sys'
```
3. 开启 **测试模式** & **禁用强制驱动签名** 模式 & **重启**

`管理员身份`打开`CMD`
```
bcdedit /set nointegritychecks on
bcdedit /set testsigning on
shutdown  -r -t 0
```
4. 重启后**管理员身份**运行
```
python python_examples.py
```
