# -*- coding: utf-8 -*-
from ctypes import CDLL
import time

dll_path = 'E:\\GitHub\\KMDLL\\dll\\kmclassdll.dll'
driver_path = b'E:\\GitHub\\KMDLL\\dll\\kmclass.sys'


driver = None
VK_CODE = {
    'backspace': 0x08,
    'tab': 0x09,
    'clear': 0x0C,
    'enter': 0x0D,
    'shift': 0x10,
    'ctrl': 0x11,
    'alt': 0x12,
    'pause': 0x13,
    'caps_lock': 0x14,
    'esc': 0x1B,
    'spacebar': 0x20,
    'page_up': 0x21,
    'page_down': 0x22,
    'end': 0x23,
    'home': 0x24,
    'left_arrow': 0x25,
    'up_arrow': 0x26,
    'right_arrow': 0x27,
    'down_arrow': 0x28,
    'select': 0x29,
    'print': 0x2A,
    'execute': 0x2B,
    'print_screen': 0x2C,
    'ins': 0x2D,
    'del': 0x2E,
    'help': 0x2F,
    '0': 0x30,
    '1': 0x31,
    '2': 0x32,
    '3': 0x33,
    '4': 0x34,
    '5': 0x35,
    '6': 0x36,
    '7': 0x37,
    '8': 0x38,
    '9': 0x39,
    'a': 0x41,
    'b': 0x42,
    'c': 0x43,
    'd': 0x44,
    'e': 0x45,
    'f': 0x46,
    'g': 0x47,
    'h': 0x48,
    'i': 0x49,
    'j': 0x4A,
    'k': 0x4B,
    'l': 0x4C,
    'm': 0x4D,
    'n': 0x4E,
    'o': 0x4F,
    'p': 0x50,
    'q': 0x51,
    'r': 0x52,
    's': 0x53,
    't': 0x54,
    'u': 0x55,
    'v': 0x56,
    'w': 0x57,
    'x': 0x58,
    'y': 0x59,
    'z': 0x5A,
    'numpad_0': 0x60,
    'numpad_1': 0x61,
    'numpad_2': 0x62,
    'numpad_3': 0x63,
    'numpad_4': 0x64,
    'numpad_5': 0x65,
    'numpad_6': 0x66,
    'numpad_7': 0x67,
    'numpad_8': 0x68,
    'numpad_9': 0x69,
    'multiply_key': 0x6A,
    'add_key': 0x6B,
    'separator_key': 0x6C,
    'subtract_key': 0x6D,
    'decimal_key': 0x6E,
    'divide_key': 0x6F,
    'F1': 0x70,
    'F2': 0x71,
    'F3': 0x72,
    'F4': 0x73,
    'F5': 0x74,
    'F6': 0x75,
    'F7': 0x76,
    'F8': 0x77,
    'F9': 0x78,
    'F10': 0x79,
    'F11': 0x7A,
    'F12': 0x7B,
    'F13': 0x7C,
    'F14': 0x7D,
    'F15': 0x7E,
    'F16': 0x7F,
    'F17': 0x80,
    'F18': 0x81,
    'F19': 0x82,
    'F20': 0x83,
    'F21': 0x84,
    'F22': 0x85,
    'F23': 0x86,
    'F24': 0x87,
    'num_lock': 0x90,
    'scroll_lock': 0x91,
    'left_shift': 0xA0,
    'right_shift ': 0xA1,
    'left_control': 0xA2,
    'right_control': 0xA3,
    'left_menu': 0xA4,
    'right_menu': 0xA5,
    'browser_back': 0xA6,
    'browser_forward': 0xA7,
    'browser_refresh': 0xA8,
    'browser_stop': 0xA9,
    'browser_search': 0xAA,
    'browser_favorites': 0xAB,
    'browser_start_and_home': 0xAC,
    'volume_mute': 0xAD,
    'volume_Down': 0xAE,
    'volume_up': 0xAF,
    'next_track': 0xB0,
    'previous_track': 0xB1,
    'stop_media': 0xB2,
    'play/pause_media': 0xB3,
    'start_mail': 0xB4,
    'select_media': 0xB5,
    'start_application_1': 0xB6,
    'start_application_2': 0xB7,
    'attn_key': 0xF6,
    'crsel_key': 0xF7,
    'exsel_key': 0xF8,
    'play_key': 0xFA,
    'zoom_key': 0xFB,
    'clear_key': 0xFE,
    '+': 0xBB,
    ',': 0xBC,
    '-': 0xBD,
    '.': 0xBE,
    '/': 0xBF,
    '`': 0xC0,
    ';': 0xBA,
    '[': 0xDB,
    '\\': 0xDC,
    ']': 0xDD,
    "'": 0xDE,
    '`': 0xC0
}


def _key_up(key): 
    """按键释放"""
    global driver
    driver.KeyUp(VK_CODE[key])

def _key_down(key):
    """按键按下"""
    global driver
    driver.KeyDown(VK_CODE[key])

def _left_button_down():
    global driver
    driver.MouseLeftButtonDown()

def _left_button_up():
    global driver
    driver.MouseLeftButtonUp()

def _right_button_down():
    global driver
    driver.MouseRightButtonDown()

def _right_button_up():
    global driver
    driver.MouseRightButtonUp()

def _middle_button_down():
    global driver
    driver.MouseMiddleButtonDown()

def _middle_button_up():
    global driver
    driver.MouseMiddleButtonUp()

def _move_rel(x, y):
    global driver
    driver.MouseMoveRELATIVE(x,y)

def _move_to(x, y):
    global driver
    driver.MouseMoveABSOLUTE(x,y)

def _mouse_xbutton1_down():
    """鼠标前侧键(X1)按下"""
    global driver
    driver.MouseXButton1Down()

def _mouse_xbutton1_up():
    """鼠标前侧键(X1)释放"""
    global driver
    driver.MouseXButton1Up()

def _mouse_xbutton2_down():
    """鼠标后侧键(X2)按下"""
    global driver
    driver.MouseXButton2Down()

def _mouse_xbutton2_up():
    """鼠标后侧键(X2)释放"""
    global driver
    driver.MouseXButton2Up()

# 加载驱动
def load_driver():
    global driver
    try:
        print("开始加载驱动...")
        # 先加载 DLL
        print(f"正在加载DLL: {dll_path}")
        driver = CDLL(dll_path)
        
        # 先尝试卸载，如果服务不存在也不报错
        print("尝试卸载已存在的驱动服务...")
        try:
            unload_driver()
        except Exception as e:
            print(f"卸载旧驱动时出现异常(可以忽略): {e}")
        
        # 重新加载驱动
        print(f"正在加载驱动文件: {driver_path}")
        result = driver.LoadNTDriver('kmclass', driver_path)
        if result == 0:
            print("驱动加载失败，错误码:", result)
            raise RuntimeError("驱动加载失败")
        else:
            print("驱动加载成功")
            
        # 等待一小段时间确保服务创建完成
        print("等待服务启动...")
        time.sleep(1)
        
        # 设置句柄
        print("正在设置驱动句柄...")
        handle_result = driver.SetHandle()
        if handle_result == 0:
            print("设置句柄失败，错误码:", handle_result)
            raise RuntimeError("设置句柄失败")
        print("驱动初始化完成")
            
    except Exception as e:
        print(f"加载驱动失败: {str(e)}")
        # 确保清理
        if driver is not None:
            print("正在清理...")
            unload_driver()
        raise

# 测试键盘功能
def test_keyboard():
    """测试键盘功能"""
    print("\n=== 开始单键测试 ===")
    
    # 测试单个按键
    print("测试按下字母'A'键和'B'键...")
    _key_down('a')
    time.sleep(1)
    _key_up('a')
    time.sleep(1)
    _key_down('b')
    time.sleep(1)
    _key_up('b')

    # 测试功能键
    print("测试F5键...")
    _key_down('F5')
    time.sleep(1)
    _key_up('F5')
    
    print("单键功能测试完成")

def test_copy_operation():
    """测试复制操作 Ctrl+A -> Ctrl+C -> Ctrl+V"""
    print("\n=== 开始测试组合键复制操作 ===")
    
    # Ctrl+A 全选
    print("执行 Ctrl+A 全选...")
    _key_down('ctrl')
    _key_down('a')
    print("按下Ctrl+A")
    time.sleep(0.1)
    _key_up('a')
    _key_up('ctrl')
    print("释放Ctrl+A")
    time.sleep(0.5)  # 等待全选完成
    
    # Ctrl+C 复制
    print("执行 Ctrl+C 复制...")
    _key_down('ctrl')
    _key_down('c')
    print("按下Ctrl+C")
    time.sleep(0.1)
    _key_up('c')
    _key_up('ctrl')
    print("释放Ctrl+C")
    time.sleep(0.5)  # 等待复制完成
    print("----复制完成----")
    
    # Ctrl+V 粘贴
    print("执行 Ctrl+V 粘贴...")
    _key_down('ctrl')
    _key_down('v')
    print("按下Ctrl+V")
    time.sleep(0.1)
    _key_up('v')
    _key_up('ctrl')
    print("释放Ctrl+V")
    print("----第一次粘贴完成----")
    
    # 再次粘贴
    print("执行 Ctrl+V 粘贴...")
    _key_down('ctrl')
    _key_down('v')
    print("按下Ctrl+V")
    time.sleep(0.1)
    _key_up('v')
    _key_up('ctrl')
    print("释放Ctrl+V")
    print("----第二次粘贴完成----")
    
def test_mouse():
    """测试鼠标功能"""
    print("\n=== 开始鼠标测试 ===")
    
    # 测试鼠标移动
    print("测试鼠标相对移动...")
    _move_rel(100, 0)  # 向右移动100像素
    time.sleep(1)
    _move_rel(0, 100)  # 向下移动100像素
    time.sleep(1)
    
    # 测试鼠标按键
    print("测试左键点击...")
    _left_button_down()
    time.sleep(1)
    _left_button_up()
    
    print("测试右键点击...")
    _right_button_down()
    time.sleep(1)
    _right_button_up()
    
    print("测试中键点击...")
    _middle_button_down()
    time.sleep(1)
    _middle_button_up()
    
    print("测试鼠标前侧键(X1)...")
    _mouse_xbutton1_down()
    time.sleep(1)
    _mouse_xbutton1_up()
    
    print("测试鼠标后侧键(X2)...")
    _mouse_xbutton2_down()
    time.sleep(1)
    _mouse_xbutton2_up()
    
    # 测试绝对位置移动
    print("测试鼠标移动到屏幕中心...")
    _move_to(500, 500)  # 移动到屏幕中心附近
    
    print("鼠标测试完成")


def run_all_tests():
    """运行所有测试"""
    try:

        print("开始加载驱动并运行测试...")
        load_driver()
        print("驱动加载完成，3s后开始测试...")
        time.sleep(3)
        # 测试单键
        test_keyboard()
        time.sleep(1)
        
        # 测试组合键复制粘贴
        test_copy_operation()
        time.sleep(1)
        
        # 测试鼠标
        test_mouse()
        time.sleep(1)
        print("\n所有测试完成!")
        
    except Exception as e:
        print(f"测试过程中出现错误: {str(e)}")
    finally:
        print("正在清理...")
        unload_driver()

def unload_driver():
    global driver
    if driver is not None:
        try:
            print("正在卸载驱动服务...")
            result = driver.UnloadNTDriver('kmclass')
            print(f"驱动卸载{'成功' if result != 0 else '失败'}")
        except Exception as e:
            print(f"卸载驱动时出现异常: {e}")


if __name__ == '__main__' : 
    run_all_tests()
