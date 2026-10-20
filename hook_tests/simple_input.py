# -*- coding: utf-8 -*-

import os
import time
from ctypes import CDLL
import sys
import ctypes

class SimpleInputTester:
    def __init__(self, dll_path=None, sys_path=None):
        # 检查管理员权限
        if not ctypes.windll.shell32.IsUserAnAdmin():
            print("请以管理员权限运行此脚本")
            sys.exit(1)

        # 设置默认路径
        if not dll_path or not sys_path:
            base_dir = os.path.dirname(os.path.dirname(__file__))
            self.dll_path = dll_path or os.path.join(base_dir, 'cqkeysdll', 'x64', 'Debug', 'cqkeysdll.dll')
            self.sys_path = sys_path or os.path.join(base_dir, 'cqkeys', 'x64', 'Debug', 'cqkeys', 'cqkeys.sys')

        # 加载驱动
        self.driver = CDLL(self.dll_path)
        
        # 先尝试卸载已存在的驱动
        self.driver.UnloadNTDriver(b'cqkeys')
        
        # 加载驱动
        if not self.driver.LoadNTDriver(b'cqkeys', self.sys_path.encode()):
            raise RuntimeError("驱动加载失败")
        
        # 设置句柄
        self.driver.SetHandle()

    def cleanup(self):
        """清理驱动"""
        if self.driver:
            self.driver.UnloadNTDriver(b'cqkeys')

    def press_key(self, key):
        """按下并释放按键"""
        vk_code = self._get_virtual_key_code(key)
        if vk_code is None:
            print(f"无效的按键: {key}")
            return False

        try:
            if self._needs_shift(key):
                # 按下Shift
                self.driver.KeyDown(0x10)  # VK_SHIFT
                time.sleep(0.01)
                
                # 按下释放目标键
                self.driver.KeyDown(vk_code)
                time.sleep(0.05)
                self.driver.KeyUp(vk_code)
                
                # 释放Shift
                time.sleep(0.01)
                self.driver.KeyUp(0x10)
            else:
                # 直接按下释放目标键
                self.driver.KeyDown(vk_code)
                time.sleep(0.05)
                self.driver.KeyUp(vk_code)
            return True
        except Exception as e:
            print(f"按键操作失败: {str(e)}")
            return False

    def type_string(self, text, interval=0.05):
        """输入字符串"""
        for char in text:
            self.press_key(char)
            time.sleep(interval)

    def _get_virtual_key_code(self, key):
        """获取虚拟键码"""
        # 空格特殊处理
        if key == ' ':
            return 0x20  # VK_SPACE
            
        # 基本按键映射
        key_map = {
            # 字母
            'a': 0x41, 'b': 0x42, 'c': 0x43, 'd': 0x44, 'e': 0x45,
            'f': 0x46, 'g': 0x47, 'h': 0x48, 'i': 0x49, 'j': 0x4A,
            'k': 0x4B, 'l': 0x4C, 'm': 0x4D, 'n': 0x4E, 'o': 0x4F,
            'p': 0x50, 'q': 0x51, 'r': 0x52, 's': 0x53, 't': 0x54,
            'u': 0x55, 'v': 0x56, 'w': 0x57, 'x': 0x58, 'y': 0x59,
            'z': 0x5A,
            
            # 数字
            '0': 0x30, '1': 0x31, '2': 0x32, '3': 0x33, '4': 0x34,
            '5': 0x35, '6': 0x36, '7': 0x37, '8': 0x38, '9': 0x39,
            
            # 特殊字符
            ',': 0xBC, '.': 0xBE, '/': 0xBF, ';': 0xBA,
            "'": 0xDE, '[': 0xDB, ']': 0xDD, '\\': 0xDC,
            '-': 0xBD, '=': 0xBB, '`': 0xC0,
            
            # 需要Shift的特殊字符
            '!': 0x31, '@': 0x32, '#': 0x33, '$': 0x34, '%': 0x35,
            '^': 0x36, '&': 0x37, '*': 0x38, '(': 0x39, ')': 0x30,
            '_': 0xBD, '+': 0xBB, '{': 0xDB, '}': 0xDD, '|': 0xDC,
            ':': 0xBA, '"': 0xDE, '<': 0xBC, '>': 0xBE, '?': 0xBF,
            '~': 0xC0,
        }
        
        return key_map.get(key.lower())

    def _needs_shift(self, char):
        """判断字符是否需要按住Shift"""
        shift_chars = {
            '~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', 
            '_', '+', '{', '}', '|', ':', '"', '<', '>', '?'
        }
        return char.isupper() or char in shift_chars

def main():
    # 创建测试实例
    tester = SimpleInputTester()
    
    try:
        # 等待用户准备
        print("驱动已加载，3秒后开始输入...")
        time.sleep(3)
        
        # # 测试空格
        # print("测试空格输入")
        # tester.press_key(' ')
        # time.sleep(1)
        
        # 测试字符串
        test_string = "Hello, World!"
        print(f"测试字符串输入: {test_string}")
        tester.type_string(test_string)
        
    except Exception as e:
        print(f"测试出错: {str(e)}")
    finally:
        # 清理驱动
        tester.cleanup()

if __name__ == '__main__':
    main() 