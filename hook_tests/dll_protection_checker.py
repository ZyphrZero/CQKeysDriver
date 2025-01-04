import os
import sys
import pefile
import binascii
import ctypes
from ctypes import wintypes

# 设置DLL的绝对路径
DLL_PATH = r"E:\GitHub\LingYaoDriver\lykeysdll\x64\Debug\lykeysdll.dll"

def check_strings(dll_path):
    """检查DLL中的字符串加密情况"""
    print("\n[1] 字符串加密检测:")
    
    # 关键字符串列表
    sensitive_strings = [
        b"\\\\.\\lykeys",
        b"lykeys",
        b"ntdll.dll",
        b"kernel32.dll"
    ]
    
    # 读取DLL文件
    with open(dll_path, 'rb') as f:
        content = f.read()
    
    found_strings = []
    for s in sensitive_strings:
        if s in content:
            found_strings.append(s)
            
    if found_strings:
        print("[-] 警告: 发现未加密的敏感字符串:")
        for s in found_strings:
            print(f"    - {s}")
    else:
        print("[+] 未发现明文敏感字符串，字符串加密有效")

def check_pe_security(dll_path):
    """检查PE文件的安全特性"""
    print("\n[2] PE安全特性检测:")
    
    pe = pefile.PE(dll_path)
    
    # 检查ASLR
    if pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040:
        print("[+] ASLR: 已启用")
    else:
        print("[-] ASLR: 未启用")
    
    # 检查DEP/NX
    if pe.OPTIONAL_HEADER.DllCharacteristics & 0x0100:
        print("[+] DEP/NX: 已启用")
    else:
        print("[-] DEP/NX: 未启用")
    
    # 检查CFG
    if pe.OPTIONAL_HEADER.DllCharacteristics & 0x4000:
        print("[+] Control Flow Guard: 已启用")
    else:
        print("[-] Control Flow Guard: 未启用")
    
    # 检查调试信息
    if hasattr(pe, 'DIRECTORY_ENTRY_DEBUG'):
        print("[-] 警告: 包含调试信息")
    else:
        print("[+] 调试信息已移除")

def test_anti_debug():
    """测试反调试保护"""
    print("\n[3] 反调试保护测试:")
    
    try:
        # 加载DLL
        dll = ctypes.CDLL(DLL_PATH)
        
        # 尝试调用SetHandle函数
        try:
            result = dll.SetHandle()
            print("[-] 警告: 在正常环境下可以加载驱动")
        except Exception as e:
            print("[+] 反调试保护正常工作:", str(e))
            
    except Exception as e:
        print("[!] DLL加载失败:", str(e))

def main():
    """主函数"""
    print("=== DLL保护检查工具 ===")
    
    # 检查DLL路径
    if not os.path.exists(DLL_PATH):
        print(f"错误: 找不到DLL文件: {DLL_PATH}")
        return
    
    print(f"正在分析: {DLL_PATH}")
    
    # 运行所有检查
    check_strings(DLL_PATH)
    check_pe_security(DLL_PATH)
    test_anti_debug()

if __name__ == "__main__":
    main() 