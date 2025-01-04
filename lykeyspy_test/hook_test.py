import os
import sys
import time
import ctypes
import win32api
import win32con
import win32process
import win32security
from ctypes import *
from ctypes.wintypes import *

# 定义常量
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_EXECUTE_READWRITE = 0x40
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 0x3
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_SHARE_READ = 0x1
FILE_SHARE_WRITE = 0x2

# 添加内存操作相关的常量和函数定义
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008

# 错误码
ERROR_ACCESS_DENIED = 5
ERROR_INVALID_HANDLE = 6

def is_admin():
    """检查是否具有管理员权限"""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def get_last_error_message():
    """获取最后的错误信息"""
    error_code = win32api.GetLastError()
    try:
        message = win32api.FormatMessage(error_code)
        return f"错误码: {error_code}, 信息: {message}"
    except:
        return f"错误码: {error_code}"

class HookTester:
    def __init__(self):
        # 获取当前脚本所在目录的上级目录
        current_dir = os.path.dirname(os.path.abspath(__file__))
        parent_dir = os.path.dirname(current_dir)
        
        # 构建DLL和SYS文件的完整路径
        self.dll_path = os.path.join(parent_dir, "lykeysdll", "x64", "Debug", "lykeysdll.dll")
        self.sys_path = os.path.join(parent_dir, "lykeys", "x64", "Debug", "lykeys.sys")
        
        # 验证文件是否存在
        if not os.path.exists(self.dll_path):
            print(f"[-] 错误: DLL文件不存在: {self.dll_path}")
            sys.exit(1)
            
        if not os.path.exists(self.sys_path):
            print(f"[-] 错误: SYS文件不存在: {self.sys_path}")
            sys.exit(1)
            
        print(f"[*] DLL路径: {self.dll_path}")
        print(f"[*] SYS路径: {self.sys_path}")
        
        self.dll = None
        self.driver_handle = None
        
    def load_dll(self):
        """加载DLL"""
        try:
            if not os.path.exists(self.dll_path):
                print(f"[-] DLL文件不存在: {self.dll_path}")
                return False
                
            # 设置DLL函数的参数类型和返回类型
            self.dll = ctypes.CDLL(self.dll_path)
            
            # 设置函数参数类型
            self.dll.LoadNTDriver.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            self.dll.LoadNTDriver.restype = ctypes.c_bool
            
            self.dll.UnloadNTDriver.argtypes = [ctypes.c_char_p]
            self.dll.UnloadNTDriver.restype = ctypes.c_bool
            
            self.dll.SetHandle.restype = ctypes.c_bool
            
            # 新增：获取驱动句柄函数（需要在DLL中实现）
            if hasattr(self.dll, 'GetDriverHandle'):
                self.dll.GetDriverHandle.restype = ctypes.c_void_p
            
            self.dll.KeyDown.argtypes = [ctypes.c_ushort]
            self.dll.KeyUp.argtypes = [ctypes.c_ushort]
            
            self.dll.MouseMoveRELATIVE.argtypes = [ctypes.c_long, ctypes.c_long]
            self.dll.MouseLeftButtonDown.restype = None
            self.dll.MouseLeftButtonUp.restype = None
            
            print("[+] DLL加载成功")
            return True
        except Exception as e:
            print(f"[-] DLL加载失败: {e}")
            return False
            
    def load_driver(self):
        """加载驱动"""
        try:
            if not self.dll:
                return False
                
            # 获取管理员权限
            if not self.get_admin_privileges():
                print("[-] 获取管理员权限失败")
                return False
                
            # 加载驱动
            result = self.dll.LoadNTDriver(b"lykeys", self.sys_path.encode())
            if result:
                print("[+] 驱动加载成功")
                
                # 获取驱动句柄
                result = self.dll.SetHandle()
                if result:
                    print("[+] 驱动句柄获取成功")
                    return True
                else:
                    print("[-] 驱动句柄获取失败")
            else:
                print("[-] 驱动加载失败")
            return False
        except Exception as e:
            print(f"[-] 驱动加载异常: {e}")
            return False
            
    def unload_driver(self):
        """卸载驱动"""
        try:
            if self.dll:
                result = self.dll.UnloadNTDriver(b"lykeys")
                if result:
                    print("[+] 驱动卸载成功")
                else:
                    print("[-] 驱动卸载失败")
        except Exception as e:
            print(f"[-] 驱动卸载异常: {e}")
            
    def get_admin_privileges(self):
        """获取管理员权限"""
        try:
            if is_admin():
                return True
                
            # 如果不是管理员，尝试提升权限
            if sys.argv[-1] != 'asadmin':
                script = os.path.abspath(sys.argv[0])
                params = ' '.join([script] + sys.argv[1:] + ['asadmin'])
                win32api.ShellExecute(0, 'runas', sys.executable, params, '', 1)
                sys.exit()
            return False
        except Exception as e:
            print(f"[-] 获取管理员权限失败: {e}")
            return False
            
    def test_hook_protection(self):
        """测试反hook保护"""
        try:
            # 1. 尝试修改驱动内存
            print("\n[*] 测试1: 尝试修改驱动内存")
            self.test_memory_modification()
            
            # 2. 尝试hook回调函数
            print("\n[*] 测试2: 尝试hook回调函数")
            self.test_callback_hook()
            
            # 3. 尝试修改驱动代码段
            print("\n[*] 测试3: 尝试修改驱动代码段")
            self.test_code_modification()
            
            # 4. 测试键鼠功能是否正常
            print("\n[*] 测试4: 验证键鼠功能")
            self.test_keyboard_mouse()
            
        except Exception as e:
            print(f"[-] 测试过程发生异常: {e}")
            
    def test_memory_modification(self):
        """测试修改驱动内存"""
        try:
            # 获取System进程句柄
            h_system = win32process.GetCurrentProcess()
            
            # 获取进程访问权限
            desired_access = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
            
            # 尝试修改驱动内存
            buffer = b"\x90" * 10
            
            try:
                # 使用ctypes直接调用Windows API
                kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel32.WriteProcessMemory.argtypes = [
                    ctypes.wintypes.HANDLE,
                    ctypes.wintypes.LPVOID,
                    ctypes.wintypes.LPCVOID,
                    ctypes.c_size_t,
                    ctypes.POINTER(ctypes.c_size_t)
                ]
                kernel32.WriteProcessMemory.restype = ctypes.wintypes.BOOL
                
                bytes_written = ctypes.c_size_t()
                result = kernel32.WriteProcessMemory(
                    h_system,
                    self.driver_handle,
                    buffer,
                    len(buffer),
                    ctypes.byref(bytes_written)
                )
                
                if result:
                    print("[-] 警告: 成功修改驱动内存，反hook保护可能未生效")
                else:
                    error = get_last_error_message()
                    if "拒绝访问" in error:
                        print("[+] 内存保护正常: 访问被拒绝")
                    else:
                        print(f"[*] 内存写入失败: {error}")
                        
            except Exception as e:
                if "拒绝访问" in str(e):
                    print("[+] 内存保护正常: 访问被拒绝")
                else:
                    print(f"[-] 内存写入异常: {e}")
                
        except Exception as e:
            print(f"[-] 内存修改测试异常: {e}")
            
    def test_callback_hook(self):
        """测试hook回调函数"""
        try:
            # 获取驱动设备句柄
            h_device = self.dll.GetDriverHandle() if hasattr(self.dll, 'GetDriverHandle') else None
            if not h_device:
                print("[-] 无法获取驱动句柄，跳过回调函数测试")
                return

            # 尝试获取并修改回调函数
            h_system = win32process.GetCurrentProcess()
            
            # 模拟hook代码
            hook_code = b"\x90" * 5  # NOP指令
            
            try:
                # 使用ctypes直接调用Windows API
                kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel32.WriteProcessMemory.argtypes = [
                    ctypes.wintypes.HANDLE,
                    ctypes.wintypes.LPVOID,
                    ctypes.wintypes.LPCVOID,
                    ctypes.c_size_t,
                    ctypes.POINTER(ctypes.c_size_t)
                ]
                kernel32.WriteProcessMemory.restype = ctypes.wintypes.BOOL
                
                bytes_written = ctypes.c_size_t()
                
                # 尝试修改驱动中的回调函数
                result = kernel32.WriteProcessMemory(
                    h_system,
                    h_device,  # 使用驱动句柄
                    hook_code,
                    len(hook_code),
                    ctypes.byref(bytes_written)
                )
                
                if result:
                    print("[-] 警告: 成功修改驱动回调，反hook保护可能未生效")
                else:
                    error = get_last_error_message()
                    if "拒绝访问" in error:
                        print("[+] 回调保护正常: 访问被拒绝")
                    else:
                        print(f"[*] 回调函数修改失败: {error}")
                        
            except Exception as e:
                if "拒绝访问" in str(e):
                    print("[+] 回调保护正常: 访问被拒绝")
                else:
                    print(f"[-] 回调函数修改异常: {e}")
                
        except Exception as e:
            print(f"[-] 回调函数测试异常: {e}")
            
    def test_code_modification(self):
        """测试修改驱动代码段"""
        try:
            h_system = win32process.GetCurrentProcess()
            
            # 尝试修改代码段
            code = b"\x90" * 10  # NOP指令
            
            try:
                # 使用ctypes直接调用Windows API
                kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel32.WriteProcessMemory.argtypes = [
                    ctypes.wintypes.HANDLE,
                    ctypes.wintypes.LPVOID,
                    ctypes.wintypes.LPCVOID,
                    ctypes.c_size_t,
                    ctypes.POINTER(ctypes.c_size_t)
                ]
                kernel32.WriteProcessMemory.restype = ctypes.wintypes.BOOL
                
                bytes_written = ctypes.c_size_t()
                result = kernel32.WriteProcessMemory(
                    h_system,
                    self.dll._handle,
                    code,
                    len(code),
                    ctypes.byref(bytes_written)
                )
                
                if result:
                    print("[-] 警告: 成功修改代码段，反hook保护可能未生效")
                else:
                    error = get_last_error_message()
                    if "拒绝访问" in error:
                        print("[+] 代码段保护正常: 访问被拒绝")
                    else:
                        print(f"[*] 代码段修改失败: {error}")
                        
            except Exception as e:
                if "拒绝访问" in str(e):
                    print("[+] 代码段保护正常: 访问被拒绝")
                else:
                    print(f"[-] 代码段修改异常: {e}")
                
        except Exception as e:
            print(f"[-] 代码段测试异常: {e}")
            
    def test_keyboard_mouse(self):
        """测试键鼠功能"""
        try:
            if not self.dll:
                print("[-] DLL未加载")
                return
                
            try:
                # 测试键盘
                print("[*] 测试键盘功能...")
                self.dll.KeyDown(0x41)  # 'A'键
                time.sleep(0.1)
                self.dll.KeyUp(0x41)
                print("[+] 键盘测试完成")
                
                # 测试鼠标
                print("[*] 测试鼠标功能...")
                self.dll.MouseMoveRELATIVE(100, 100)
                time.sleep(0.1)
                self.dll.MouseLeftButtonDown()
                time.sleep(0.1)
                self.dll.MouseLeftButtonUp()
                print("[+] 鼠标测试完成")
                
            except Exception as e:
                error = get_last_error_message()
                print(f"[-] 键鼠操作失败: {error}")
                
        except Exception as e:
            print(f"[-] 键鼠功能测试异常: {e}")
            
def main():
    print("[*] 开始反hook保护测试...")
    print("[*] 检查管理员权限...")
    
    if not is_admin():
        print("[-] 请以管理员权限运行此脚本")
        return
        
    tester = HookTester()
    
    # 1. 加载DLL
    if not tester.load_dll():
        return
        
    # 2. 加载驱动
    if not tester.load_driver():
        return
        
    try:
        # 3. 运行测试
        print("\n[*] 开始执行测试...")
        tester.test_hook_protection()
        print("\n[*] 测试完成")
        
    except Exception as e:
        print(f"\n[-] 测试过程发生异常: {e}")
        
    finally:
        # 4. 清理
        print("\n[*] 开始清理...")
        tester.unload_driver()
        print("[*] 清理完成")
        
if __name__ == "__main__":
    main() 