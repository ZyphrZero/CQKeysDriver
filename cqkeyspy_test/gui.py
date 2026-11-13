# -*- coding: utf-8 -*-

import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import threading
import queue
from input_workers import InputWorkers, WorkCancelled
from driver_manager import DriverManager
from input_tester import InputTester
from input_tasks import MOVE_RELATIVE, MOVE_ABSOLUTE, MAX_ACTIONS, MAX_REPEATS, action
from virtuakeys_mapping import VirtualKeys
import logging
import os
import json
import tempfile
import sys
import ctypes
import argparse
import win32gui
import math
from keyboard_hook import KeyboardHook

# python gui.py --debug
parser = argparse.ArgumentParser()
parser.add_argument('--debug', action='store_true', help='启用调试模式')
args = parser.parse_args() if __name__ == "__main__" else parser.parse_args([])

# 使用命令行参数设置调试模式
DEBUG_MODE = args.debug

# 在类外部或文件开头定义常量
SW_HIDE = 0
SW_SHOWNORMAL = 1
SW_MINIMIZE = 6
SW_MAXIMIZE = 3

class CQKeysGUI:
    def __init__(self):
        """初始化GUI"""
        # 权限检查
        if not self.check_privileges():
            sys.exit(0)
            
        # 创建主窗口（必须最先创建）
        self.root = tk.Tk()
        self.root.title("cqkeydriver测试工具")
        self.root.geometry("1000x1000")
        
        # 初始化基本变量
        # 驱动相关
        self.driver_mgr = None
        self.input_tester = None
        self.is_driver_loaded = False
        self.is_closing = False
        self.is_unloading = False
        self.input_workers = InputWorkers()
        self._rapid_test_pending = False
        self._auto_move_pending = False
        self.rapid_test_running = False
        self._stop_results = queue.Queue()
        self._cleanup_results = queue.Queue()
        self._input_release_error = None
        self._management_cleanup_error = None
        self._ui_events = queue.Queue()
        self._ui_generation = 0
        self._ui_closed = False
        self._log_handler = None
        self.config_file = "driver_config.json"
        
        # 路径配置变量（在创建根窗口后创建）
        self.dll_path_var = tk.StringVar()
        self.sys_path_var = tk.StringVar()
        
        # 测试相关变量
        self.test_key_var = tk.StringVar(value="a")
        self.press_time_var = tk.StringVar(value="1")
        self.interval_time_var = tk.StringVar(value="1")
        self.duration_var = tk.StringVar(value="1")
        
        # 自动移动相关变量
        self.keyboard_hook = None
        self.auto_move_running = False
        self.auto_move_config = {
            'hotkey': 'F8',
            'speed': 10,
            'range': 100
        }
        
        # 配置界面样式
        # 设置全局字体
        self.default_font = ('Microsoft YaHei UI', 10)
        self.style = ttk.Style()
        self.style.configure('.', font=self.default_font)
        
        # 配置各种部件字体
        for widget in ['TLabel', 'TButton', 'TEntry', 'TCheckbutton', 'TRadiobutton']:
            self.style.configure(widget, font=self.default_font)
        self.style.configure('TLabelframe.Label', font=self.default_font)
        self.style.configure('TNotebook.Tab', font=self.default_font)
        
        # 创建主界面布局
        # 配置根窗口网格
        self.root.grid_rowconfigure(0, weight=1)
        self.root.grid_columnconfigure(0, weight=1)
        
        # 创建分隔窗口
        self.paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        self.paned.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)
        
        # 创建左右面板
        self.left_panel = ttk.Frame(self.paned)
        self.right_panel = ttk.Frame(self.paned)
        self.paned.add(self.left_panel, weight=2)
        self.paned.add(self.right_panel, weight=3)
        
        # 创建标签页容器
        self.notebook = ttk.Notebook(self.left_panel)
        self.notebook.pack(fill=tk.BOTH, expand=True)
        
        # 初始化功能组件
        # 设置日志处理器
        self._setup_logging()
        
        # 加载配置
        self.load_path_config()
        self.load_auto_move_config()
        
        # 如果没有配置路径，显示提示消息
        if not self.dll_path_var.get() or not self.sys_path_var.get():
            logging.warning("请先在驱动路径配置中设置DLL和SYS文件路径")
        
        # 创建功能标签页
        self.create_driver_tab()
        self.create_input_test_tab()
        self.create_rapid_test_tab()
        self.create_mouse_test_tab()
        
        # 绑定事件
        self.root.protocol("WM_DELETE_WINDOW", self._on_closing)
        self.root.after(25, self._drain_ui_events)
        
    def create_driver_tab(self):
        """创建驱动控制标签页"""
        driver_tab = ttk.Frame(self.notebook)
        self.notebook.add(driver_tab, text="驱动控制")

        # 驱动控制区域
        driver_frame = ttk.LabelFrame(driver_tab, text="驱动控制", padding="5")
        driver_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.load_btn = ttk.Button(driver_frame, text="安装驱动", command=self._load_driver)
        self.load_btn.pack(side=tk.LEFT, padx=5)
        
        self.unload_btn = ttk.Button(driver_frame, text="卸载驱动", command=self._unload_driver, state=tk.DISABLED)
        self.unload_btn.pack(side=tk.LEFT, padx=5)
        
        # 状态检查区域
        status_frame = ttk.LabelFrame(driver_tab, text="驱动状态", padding="5")
        status_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.driver_status = ttk.Label(status_frame, text="驱动状态: 未知")
        self.driver_status.pack(fill=tk.X, padx=5, pady=2)
        
        self.last_check = ttk.Label(status_frame, text="上次检查: 未检查")
        self.last_check.pack(fill=tk.X, padx=5, pady=2)
        
        self.check_status_btn = ttk.Button(status_frame, text="检查驱动状态", 
                                       command=self._check_driver_status, 
                                       state=tk.DISABLED,
                                       width=15)
        self.check_status_btn.pack(side=tk.LEFT, padx=5, pady=2)

        # 路径配置区域
        self.create_path_frame(driver_tab)

    def create_input_test_tab(self):
        """创建输入测试标签页"""
        input_tab = ttk.Frame(self.notebook)
        self.notebook.add(input_tab, text="输入测试")

        # 输入/输出区域
        io_frame = ttk.LabelFrame(input_tab, text="输入/输出", padding="5")
        io_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 输入区域
        input_frame = ttk.LabelFrame(io_frame, text="输入区域", padding="5")
        input_frame.pack(fill=tk.X, padx=5, pady=(5,2))
        
        # 创建固定宽度的输入框
        self.input_entry = ttk.Entry(input_frame, width=25, font=self.default_font)  # 设置字体
        self.input_entry.pack(side=tk.LEFT, padx=5, pady=5)
        self.input_entry.bind('<Return>', self._on_input_submit)
        
        # 输入区域按钮
        button_frame = ttk.Frame(input_frame)
        button_frame.pack(side=tk.LEFT, padx=5)
        
        self.submit_btn = ttk.Button(button_frame, text="发送", command=self._on_submit_click, width=8)
        self.submit_btn.pack(side=tk.LEFT)
        
        self.clear_btn = ttk.Button(button_frame, text="清除", command=self._clear_text, width=8)
        self.clear_btn.pack(side=tk.LEFT, padx=5)
        
        # 输出区域
        output_frame = ttk.LabelFrame(io_frame, text="输出区域", padding="5")
        output_frame.pack(fill=tk.X, padx=5, pady=(2,5))
        
        # 创建固定宽度的输出框
        self.output_entry = ttk.Entry(output_frame, state='readonly', width=25, font=self.default_font)  # 设置字体
        self.output_entry.pack(side=tk.LEFT, padx=5, pady=5)

        # 测试功能区域
        test_frame = ttk.LabelFrame(input_tab, text="测试功能", padding="5")
        test_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 创建按钮容器
        test_button_frame = ttk.Frame(test_frame)
        test_button_frame.pack(side=tk.LEFT, padx=5, pady=5)
        
        self.test_key_btn = ttk.Button(test_button_frame, text="按键测试", 
                                     command=self._test_keyboard, 
                                     state=tk.DISABLED, 
                                     width=15)
        self.test_key_btn.pack(side=tk.LEFT, padx=5)
        
        self.test_input_btn = ttk.Button(test_button_frame, text="输入测试", 
                                       command=self._test_input, 
                                       state=tk.DISABLED, 
                                       width=15)
        self.test_input_btn.pack(side=tk.LEFT, padx=5)
        
        self.test_mouse_btn = ttk.Button(test_button_frame, text="鼠标测试", 
                                       command=self._test_mouse, 
                                       state=tk.DISABLED, 
                                       width=15)
        self.test_mouse_btn.pack(side=tk.LEFT, padx=5)

    def create_rapid_test_tab(self):
        """创建高频测试标签页"""
        rapid_tab = ttk.Frame(self.notebook)
        self.notebook.add(rapid_tab, text="高频测试")

        # 创建主容器
        main_container = ttk.Frame(rapid_tab)
        main_container.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # 创建左右分栏
        settings_container = ttk.Frame(main_container)
        settings_container.pack(fill=tk.BOTH, expand=True)
        
        left_settings = ttk.Frame(settings_container)
        left_settings.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0,5))
        
        right_settings = ttk.Frame(settings_container)
        right_settings.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(5,0))

        # 左侧 - 按键和时间设置
        # 按键选择
        key_frame = ttk.LabelFrame(left_settings, text="按键设置", padding="5")
        key_frame.pack(fill=tk.X, pady=(0,5))
        
        ttk.Label(key_frame, text="测试按键:").pack(side=tk.LEFT, padx=5)
        self.test_key_entry = ttk.Entry(key_frame, textvariable=self.test_key_var, width=8, font=self.default_font)
        self.test_key_entry.pack(side=tk.LEFT, padx=5)

        # 时间设置框架
        time_frame = ttk.LabelFrame(left_settings, text="时间设置(毫秒)", padding="5")
        time_frame.pack(fill=tk.X)

        # 按下时间设置
        press_frame = ttk.Frame(time_frame)
        press_frame.pack(fill=tk.X, pady=2)
        ttk.Label(press_frame, text="按下抬起间隔:").pack(side=tk.LEFT, padx=5)
        self.press_time_entry = ttk.Entry(press_frame, textvariable=self.press_time_var, width=8, font=self.default_font)
        self.press_time_entry.pack(side=tk.LEFT, padx=5)

        # 间隔时间设置
        interval_frame = ttk.Frame(time_frame)
        interval_frame.pack(fill=tk.X, pady=2)
        ttk.Label(interval_frame, text="等待间隔:").pack(side=tk.LEFT, padx=5)
        self.interval_time_entry = ttk.Entry(interval_frame, textvariable=self.interval_time_var, width=8, font=self.default_font)
        self.interval_time_entry.pack(side=tk.LEFT, padx=5)

        # 右侧 - 运行时长和状态显示
        # 运行时长设置
        duration_frame = ttk.LabelFrame(right_settings, text="运行设置", padding="5")
        duration_frame.pack(fill=tk.X, pady=(0,5))
        
        ttk.Label(duration_frame, text="运行时长(秒):").pack(side=tk.LEFT, padx=5)
        self.duration_entry = ttk.Entry(duration_frame, textvariable=self.duration_var, width=8, font=self.default_font)
        self.duration_entry.pack(side=tk.LEFT, padx=5)

        # 状态显示框架
        status_frame = ttk.LabelFrame(right_settings, text="运行状态", padding="5")
        status_frame.pack(fill=tk.X)

        # 创建左右两列的容器
        status_left = ttk.Frame(status_frame)
        status_left.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5)
        
        status_right = ttk.Frame(status_frame)
        status_right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5)

        # 左列 - 状态和运行时间
        self.rapid_test_status = ttk.Label(status_left, text="状态: 未开始")
        self.rapid_test_status.pack(fill=tk.X, pady=2)
        
        self.run_time_label = ttk.Label(status_left, text="运行时间: 0秒")
        self.run_time_label.pack(fill=tk.X, pady=2)
        
        # 右列 - 按键次数和频率
        self.press_count = 0
        self.press_count_label = ttk.Label(status_right, text="按键次数: 0")
        self.press_count_label.pack(fill=tk.X, pady=2)
        
        self.click_rate_label = ttk.Label(status_right, text="平均频率: 0次/秒")
        self.click_rate_label.pack(fill=tk.X, pady=2)

        # 控制按钮 - 垂直布局
        control_frame = ttk.Frame(main_container)
        control_frame.pack(fill=tk.X, pady=(10,0))
        
        # 创建一个容器来居中放置按钮
        button_container = ttk.Frame(control_frame)
        button_container.pack(anchor=tk.CENTER)
        
        self.rapid_test_btn = ttk.Button(button_container, 
                                       text="开始测试", 
                                       command=self._toggle_rapid_test, 
                                       width=15)
        self.rapid_test_btn.pack(pady=5)

    def create_mouse_test_tab(self):
        """创建鼠标测试标签页"""
        mouse_tab = ttk.Frame(self.notebook)
        self.notebook.add(mouse_tab, text="鼠标测试")

        # 创建主容器
        main_container = ttk.Frame(mouse_tab)
        main_container.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # 鼠标点击测试区域
        click_frame = ttk.LabelFrame(main_container, text="鼠标点击测试")
        click_frame.pack(fill=tk.X, padx=5, pady=5)

        # 基本按键测试
        basic_click_frame = ttk.Frame(click_frame)
        basic_click_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 创建按钮容器并居中
        button_container = ttk.Frame(basic_click_frame)
        button_container.pack(anchor=tk.CENTER)
        
        # 鼠标按键测试按钮 - 2x2布局
        click_buttons = [
            ("左键点击", "left"),
            ("右键点击", "right"),
            ("中键点击", "middle"),
            ("X1键点击", "x1")
        ]
        
        for i, (btn_text, btn_cmd) in enumerate(click_buttons):
            row = i // 2  # 整除得到行号
            col = i % 2   # 取余得到列号
            btn = ttk.Button(button_container, 
                           text=btn_text, 
                           command=lambda cmd=btn_cmd: self._test_mouse_click(cmd),
                           width=15)
            btn.grid(row=row, column=col, padx=5, pady=2)
            
        # X2键点击按钮单独放在下面居中
        ttk.Button(button_container, 
                  text="X2键点击", 
                  command=lambda: self._test_mouse_click("x2"),
                  width=15).grid(row=2, column=0, columnspan=2, padx=5, pady=2)

        # 鼠标移动测试区域
        move_frame = ttk.LabelFrame(main_container, text="鼠标移动测试")
        move_frame.pack(fill=tk.X, padx=5, pady=5)

        # 移动到四角的按钮
        corner_frame = ttk.LabelFrame(move_frame, text="移动到屏幕角落")
        corner_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 创建按钮容器并居中
        corner_button_container = ttk.Frame(corner_frame)
        corner_button_container.pack(anchor=tk.CENTER)
        
        # 四角移动按钮 - 2x2布局
        corner_buttons = [
            ("左上角", "top_left"),
            ("右上角", "top_right"),
            ("左下角", "bottom_left"),
            ("右下角", "bottom_right")
        ]
        
        for i, (btn_text, btn_cmd) in enumerate(corner_buttons):
            row = i // 2  # 整除得到行号
            col = i % 2   # 取余得到列号
            btn = ttk.Button(corner_button_container, 
                           text=btn_text, 
                           command=lambda cmd=btn_cmd: self._move_to_corner(cmd),
                           width=15)
            btn.grid(row=row, column=col, padx=5, pady=2)

        # 相对移动测试
        rel_move_frame = ttk.LabelFrame(move_frame, text="相对平滑移动测试")
        rel_move_frame.pack(fill=tk.X, padx=5, pady=5)

        # 相对移动距离设置
        distance_frame = ttk.Frame(rel_move_frame)
        distance_frame.pack(anchor=tk.CENTER, pady=5)
        
        ttk.Label(distance_frame, text="移动距离:").pack(side=tk.LEFT, padx=5)
        self.rel_distance_var = tk.StringVar(value="100")
        ttk.Entry(distance_frame, textvariable=self.rel_distance_var, width=8, font=self.default_font).pack(side=tk.LEFT, padx=5)

        # 相对移动方向按钮
        direction_frame = ttk.Frame(rel_move_frame)
        direction_frame.pack(anchor=tk.CENTER, pady=5)
        
        # 上方向按钮
        ttk.Button(direction_frame, text="↑", 
                  command=lambda: self._test_smooth_move_rel("up"),
                  width=8).pack(pady=2)
        
        # 左右方向按钮
        lr_frame = ttk.Frame(direction_frame)
        lr_frame.pack(pady=2)
        ttk.Button(lr_frame, text="←", 
                  command=lambda: self._test_smooth_move_rel("left"),
                  width=8).pack(side=tk.LEFT, padx=2)
        ttk.Button(lr_frame, text="→", 
                  command=lambda: self._test_smooth_move_rel("right"),
                  width=8).pack(side=tk.LEFT, padx=2)
        
        # 下方向按钮
        ttk.Button(direction_frame, text="↓", 
                  command=lambda: self._test_smooth_move_rel("down"),
                  width=8).pack(pady=2)

        # 绝对移动测试
        abs_move_frame = ttk.LabelFrame(move_frame, text="绝对平滑移动测试")
        abs_move_frame.pack(fill=tk.X, padx=5, pady=5)

        # 目标坐标设置
        coords_frame = ttk.Frame(abs_move_frame)
        coords_frame.pack(anchor=tk.CENTER, pady=5)
        
        ttk.Label(coords_frame, text="X:").pack(side=tk.LEFT, padx=5)
        self.abs_x_var = tk.StringVar(value="500")
        ttk.Entry(coords_frame, textvariable=self.abs_x_var, width=8, font=self.default_font).pack(side=tk.LEFT, padx=5)

        ttk.Label(coords_frame, text="Y:").pack(side=tk.LEFT, padx=5)
        self.abs_y_var = tk.StringVar(value="500")
        ttk.Entry(coords_frame, textvariable=self.abs_y_var, width=8, font=self.default_font).pack(side=tk.LEFT, padx=5)

        # 移动按钮
        move_button_frame = ttk.Frame(abs_move_frame)
        move_button_frame.pack(anchor=tk.CENTER, pady=5)
        
        ttk.Button(move_button_frame, 
                  text="移动到指定位置", 
                  command=self._test_smooth_move_abs,
                  width=20).pack()

        # 鼠标滚轮测试区域
        wheel_frame = ttk.LabelFrame(main_container, text="鼠标滚轮测试")
        wheel_frame.pack(fill=tk.X, padx=5, pady=5)

        # 滚动量设置
        wheel_settings_frame = ttk.Frame(wheel_frame)
        wheel_settings_frame.pack(anchor=tk.CENTER, pady=5)
        
        ttk.Label(wheel_settings_frame, text="滚动量:").pack(side=tk.LEFT, padx=5)
        self.wheel_delta_var = tk.StringVar(value="120")
        ttk.Entry(wheel_settings_frame, textvariable=self.wheel_delta_var, width=8, font=self.default_font).pack(side=tk.LEFT, padx=5)

        # 滚轮测试按钮
        wheel_button_frame = ttk.Frame(wheel_frame)
        wheel_button_frame.pack(anchor=tk.CENTER, pady=5)
        
        ttk.Button(wheel_button_frame, 
                  text="向上滚动", 
                  command=lambda: self._test_mouse_wheel("up"),
                  width=15).pack(pady=2)
        ttk.Button(wheel_button_frame, 
                  text="向下滚动", 
                  command=lambda: self._test_mouse_wheel("down"),
                  width=15).pack(pady=2)

        auto_move_frame = ttk.LabelFrame(main_container, text="自动移动设置")
        auto_move_frame.pack(fill=tk.X, padx=5, pady=5)

        # 热键设置
        hotkey_frame = ttk.Frame(auto_move_frame)
        hotkey_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Label(hotkey_frame, text="触发热键:").pack(side=tk.LEFT, padx=5)
        self.hotkey_var = tk.StringVar(value=self.auto_move_config['hotkey'])
        self.hotkey_entry = ttk.Entry(hotkey_frame, textvariable=self.hotkey_var, width=8, font=self.default_font)
        self.hotkey_entry.pack(side=tk.LEFT, padx=5)
        
        # 移动速度设置
        speed_frame = ttk.Frame(auto_move_frame)
        speed_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Label(speed_frame, text="移动速度:").pack(side=tk.LEFT, padx=5)
        self.speed_var = tk.StringVar(value=str(self.auto_move_config['speed']))
        self.speed_entry = ttk.Entry(speed_frame, textvariable=self.speed_var, width=8, font=self.default_font)
        self.speed_entry.pack(side=tk.LEFT, padx=5)
        
        # 移动范围设置
        range_frame = ttk.Frame(auto_move_frame)
        range_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Label(range_frame, text="移动范围:").pack(side=tk.LEFT, padx=5)
        self.range_var = tk.StringVar(value=str(self.auto_move_config['range']))
        self.range_entry = ttk.Entry(range_frame, textvariable=self.range_var, width=8, font=self.default_font)
        self.range_entry.pack(side=tk.LEFT, padx=5)
        
        # 状态显示
        status_frame = ttk.Frame(auto_move_frame)
        status_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.auto_move_status = ttk.Label(status_frame, text="状态: 未启动")
        self.auto_move_status.pack(side=tk.LEFT, padx=5)
        
        # 控制按钮
        button_frame = ttk.Frame(auto_move_frame)
        button_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.start_hook_btn = ttk.Button(button_frame, 
                                       text="启动热键监听", 
                                       command=self._toggle_keyboard_hook,
                                       width=15)
        self.start_hook_btn.pack(side=tk.LEFT, padx=5)
        
        ttk.Button(button_frame, 
                  text="保存设置", 
                  command=self.save_auto_move_config,
                  width=15).pack(side=tk.LEFT, padx=5)

    def _on_closing(self):
        # Keep Tk alive while workers finish; they may still have queued UI work.
        self._begin_unload(close_after=True)

    def _start_worker(self, target, args=()):
        if self.is_unloading or self.is_closing or not self.is_driver_loaded:
            return None
        return self.input_workers.start(target, args)

    def _post_ui(self, callback, *args, **kwargs):
        if not self._ui_closed:
            self._ui_events.put((self._ui_generation, callback, args, kwargs))

    def _drain_ui_events(self):
        for _ in range(100):
            if self._ui_closed:
                return
            try:
                generation, callback, args, kwargs = self._ui_events.get_nowait()
            except queue.Empty:
                break
            if generation == self._ui_generation:
                try:
                    callback(*args, **kwargs)
                except Exception:
                    self.root.report_callback_exception(*sys.exc_info())
        if not self._ui_closed:
            self.root.after(25, self._drain_ui_events)

    def _setup_logging(self):
        """设置日志处理"""
        class TextHandler(logging.Handler):
            def __init__(self, text_widget, gui):
                logging.Handler.__init__(self)
                self.text_widget = text_widget
                self.gui = gui
                
            def emit(self, record):
                if not self.gui.is_closing:  # 只在程序未关闭时输出日志
                    msg = self.format(record) + '\n'
                    self.gui._post_ui(self._insert_log, msg)
            
            def _insert_log(self, msg):
                """在主线程中插入日志并滚动"""
                self.text_widget.insert(tk.END, msg)
                self.text_widget.yview_moveto(1.0)  # 确保滚动到底部
        
        # 创建日志区域（放在右侧面板）
        self.log_frame = ttk.LabelFrame(self.right_panel, text="日志输出")
        self.log_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # 配置日志框架的grid权重
        self.log_frame.grid_rowconfigure(0, weight=1)
        self.log_frame.grid_columnconfigure(0, weight=1)
        
        # 创建文本框和滚动条
        self.log_text = tk.Text(self.log_frame, wrap=tk.WORD, width=50)  # 设置初始大小
        self.log_scrollbar = ttk.Scrollbar(self.log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=self.log_scrollbar.set)
        
        # 设置文本框样式
        self.log_text.configure(font=self.default_font)  # 使用全局字体
        
        # 放置文本框和滚动条
        self.log_text.grid(row=0, column=0, sticky="nsew", padx=(5,0), pady=5)
        self.log_scrollbar.grid(row=0, column=1, sticky="ns", pady=5)
        
        # 配置日志处理
        handler = TextHandler(self.log_text, self)
        self._log_handler = handler
        formatter = logging.Formatter('%(asctime)s - %(levelname)s - %(message)s')
        handler.setFormatter(formatter)
        logging.getLogger().addHandler(handler)
        logging.getLogger().setLevel(logging.INFO)
    
    def _update_button_states(self, driver_loaded):
        """更新按钮状态"""
        state = tk.NORMAL if driver_loaded and not self.is_unloading else tk.DISABLED
        self.unload_btn.config(state=tk.NORMAL if self.driver_mgr and not self.is_unloading else tk.DISABLED)
        self.test_key_btn.config(state=state)
        self.test_input_btn.config(state=state)
        self.test_mouse_btn.config(state=state)
        self.submit_btn.config(state=state)
        self.clear_btn.config(state=state)
        self.input_entry.config(state=state)
        self.check_status_btn.config(state=state)
        self.load_btn.config(state=tk.DISABLED if self.driver_mgr or self.is_unloading else tk.NORMAL)
        self.rapid_test_btn.config(state=state)
    
    def _load_driver(self):
        if self.is_unloading or self.driver_mgr is not None:
            return False
        try:
            self.input_workers.reset()
            manager = DriverManager()
            manager._setup_paths(dll_path=self.dll_path_var.get(), sys_path=self.sys_path_var.get())
            self.driver_mgr = manager
            self._input_release_error = None
            self._management_cleanup_error = None
            if not manager.initialize():
                if manager.get_driver() is None:
                    self.driver_mgr = None
                self._update_button_states(False)
                messagebox.showerror("加载失败", "请检查日志；如仍有会话，可先点击卸载后重试")
                return False
            self.input_tester = InputTester(manager.get_driver(), tasks=manager.tasks)
            self.is_driver_loaded = True
            self._update_button_states(True)
            self._check_driver_status()
            return True
        except Exception as error:
            self._update_button_states(False)
            messagebox.showerror("加载失败", str(error))
            logging.exception("驱动加载失败")
            return False

    def _unload_driver(self):
        self._begin_unload()

    def _begin_unload(self, close_after=False):
        if self.is_unloading:
            return
        self.is_unloading = True
        self.is_closing = close_after
        self.auto_move_running = False
        self.rapid_test_running = False
        self.input_workers.cancel()
        if self.input_tester:
            self.input_tester.stop()
        manager = self.driver_mgr

        def stop_input():
            try:
                if manager:
                    manager.stop_input()
                self._stop_results.put(None)
            except Exception as error:
                self._stop_results.put(error)

        try:
            threading.Thread(target=stop_input, daemon=True).start()
        except Exception as error:
            self._finish_unload(error, close_after)
            return
        self._update_button_states(False)
        self.driver_status.config(text="驱动状态: 正在停止输入并卸载")
        self.root.after(25, self._poll_stop, close_after)

    def _poll_stop(self, close_after):
        try:
            error = self._stop_results.get_nowait()
        except queue.Empty:
            self.root.after(25, self._poll_stop, close_after)
            return
        if error is not None:
            if isinstance(error, OSError) and getattr(error, "winerror", None) in (1167, 433):
                self._input_release_error = error
            else:
                self._finish_unload(error, close_after)
                return
        self._wait_for_workers(time.monotonic() + 5, close_after)

    def _wait_for_workers(self, deadline, close_after):
        if not self.input_workers.is_idle():
            if time.monotonic() >= deadline:
                self._finish_unload(RuntimeError("输入线程尚未退出，未强制卸载；请稍后重试"), close_after)
            else:
                self.root.after(25, self._wait_for_workers, deadline, close_after)
            return

        def cleanup():
            try:
                if self.keyboard_hook:
                    self.keyboard_hook.stop()
                    self.keyboard_hook = None
                if self.driver_mgr:
                    self.driver_mgr.cleanup()
                self._cleanup_results.put(None)
            except Exception as error:
                self._cleanup_results.put(error)

        try:
            threading.Thread(target=cleanup, daemon=True).start()
        except Exception as error:
            self._finish_unload(error, close_after)
            return
        self.root.after(25, self._poll_cleanup, close_after)

    def _poll_cleanup(self, close_after):
        try:
            result = self._cleanup_results.get_nowait()
        except queue.Empty:
            self.root.after(25, self._poll_cleanup, close_after)
            return
        self._finish_unload(result, close_after)

    def _finish_unload(self, error, close_after):
        self.is_unloading = False
        self.is_driver_loaded = False
        self._management_cleanup_error = error
        if error is not None:
            # Preserve the manager and window for retry; stopped input must not resume.
            self.is_closing = False
            self._update_button_states(False)
            self.driver_status.config(text="驱动状态: 卸载失败，可重试")
            details = str(error)
            if self._input_release_error is not None:
                details = f"输入释放未确认: {self._input_release_error}\n管理清理未完成: {error}"
            messagebox.showerror("卸载失败", details)
            return
        self.driver_mgr = None
        self.input_tester = None
        self._rapid_test_pending = False
        self._auto_move_pending = False
        self._ui_generation += 1
        self._update_button_states(False)
        self.driver_status.config(text="驱动状态: 未加载")
        self.last_check.config(text="上次检查: 未检查")
        self.start_hook_btn.config(text="启动热键监听")
        self.auto_move_status.config(text="状态: 未启动")
        self.rapid_test_btn.config(text="开始测试")
        if self._input_release_error is not None:
            self.driver_status.config(text="驱动状态: 管理清理完成，输入释放未确认")
            messagebox.showwarning("输入释放未确认",
                                   f"PnP 管理清理已完成；输入释放未确认: {self._input_release_error}\n"
                                   "设备已断连，不能据此认定原输入已成功释放。")
        if close_after:
            self._ui_closed = True
            if self._log_handler is not None:
                logging.getLogger().removeHandler(self._log_handler)
                self._log_handler.close()
                self._log_handler = None
            self.root.destroy()

    def _test_keyboard(self):
        """测试键盘"""
        def run_test():
            try:
                # 测试一些基本按键
                logging.info(f"键盘测试3s后开始!")
                self.input_workers.wait(3)
                test_keys = ['w', 'a', 's', 'd', 'enter']
                actions = []
                for key in test_keys:
                    logging.info(f"测试按键: {key}")
                    actions.extend(InputTester.key_actions(key, interval=0.5))
                self.input_tester.run_actions(actions)
                logging.info("键盘测试任务完成")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"键盘测试出错: {str(e)}")
        
        self._start_worker(run_test)
    
    def _test_input(self):
        """测试输入固定字符串"""
        def run_test():
            try:
                logging.info("输入测试3s后开始!")
                self.input_workers.wait(3)
                test_string = "Hello, World!"
                logging.info(f"测试输入: {test_string}")
                
                self.input_tester.type_text(test_string, interval=0.1)
                
                logging.info("文本任务完成；目标应用接收情况未验证")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"输入测试出错: {str(e)}")
        
        self._start_worker(run_test)
    
    def _test_mouse(self):
        """测试鼠标移动"""
        def run_test():
            try:
                logging.info("鼠标测试3s后开始!")
                self.input_workers.wait(3)  # 给用户准备时间
                actions = []
                
                # 1. 相对移动测试
                logging.info("=== 开始相对移动测试 ===")
                
                # 基本方向测试
                moves = [
                    (100, 0, "右"),
                    (0, 100, "下"),
                    (-100, 0, "左"),
                    (0, -100, "上")
                ]
                
                for dx, dy, direction in moves:
                    logging.info(f"测试{direction}移动: ({dx}, {dy})")
                    actions.append(action(MOVE_RELATIVE, dx, dy, 500))
                
                # 对角线移动测试
                diag_moves = [
                    (50, 50, "右下"),
                    (-50, 50, "左下"),
                    (-50, -50, "左上"),
                    (50, -50, "右上")
                ]
                
                for dx, dy, direction in diag_moves:
                    logging.info(f"测试{direction}对角移动: ({dx}, {dy})")
                    actions.append(action(MOVE_RELATIVE, dx, dy, 500))
                
                # 小距离精确移动测试
                small_moves = [(5, 0), (0, 5), (-5, 0), (0, -5)]
                logging.info("测试小距离精确移动")
                for dx, dy in small_moves:
                    actions.append(action(MOVE_RELATIVE, dx, dy, 300))
                
                # 2. 绝对移动测试
                logging.info("\n=== 开始主显示器绝对移动测试 ===")
                
                screen_width = ctypes.windll.user32.GetSystemMetrics(0)
                screen_height = ctypes.windll.user32.GetSystemMetrics(1)
                if screen_width <= 1 or screen_height <= 1:
                    raise RuntimeError("主显示器尺寸无效，未提交鼠标测试任务")
                
                # 屏幕边界点测试
                border_points = [
                    (0, 0, "左上角"),
                    (screen_width - 1, 0, "右上角"),
                    (0, screen_height - 1, "左下角"),
                    (screen_width - 1, screen_height - 1, "右下角")
                ]
                
                for x, y, position in border_points:
                    logging.info(f"测试移动到主显示器{position}: ({x}, {y})")
                    actions.append(action(MOVE_ABSOLUTE, x, y, 800))
                
                # 屏幕中心点测试
                center_x = screen_width // 2
                center_y = screen_height // 2
                logging.info(f"测试移动到主显示器中心: ({center_x}, {center_y})")
                actions.append(action(MOVE_ABSOLUTE, center_x, center_y, 800))
                
                # 3. 鼠标按键测试
                logging.info("\n=== 开始鼠标按键测试 ===")
                
                # 左键测试
                logging.info("测试左键点击")
                actions.extend(InputTester.click_actions("left", interval=0.5))
                
                # 右键测试
                logging.info("测试右键点击")
                actions.extend(InputTester.click_actions("right", interval=0.5))
                
                # 中键测试
                logging.info("测试中键点击")
                actions.extend(InputTester.click_actions("middle", interval=0.5))
                
                # 额外按键测试
                logging.info("测试X1按键点击")
                actions.extend(InputTester.click_actions("x1", interval=0.5))
                
                logging.info("测试X2按键点击")
                actions.extend(InputTester.click_actions("x2", interval=0.5))
                self.input_tester.run_actions(actions)
                
                logging.info("鼠标测试任务完成")
                
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"鼠标测试出错: {str(e)}")
        
        self._start_worker(run_test)
    
    def _on_input_submit(self, event):
        """处理Enter键提交事件"""
        if self.is_driver_loaded:
            self._send_input_text()

    def _on_submit_click(self):
        """处理提交按钮点击事件"""
        if self.is_driver_loaded:
            self._send_input_text()

    def _clear_text(self):
        """清除输入和输出文本"""
        self.input_entry.delete(0, tk.END)
        self.output_entry.config(state='normal')
        self.output_entry.delete(0, tk.END)
        self.output_entry.config(state='readonly')

    def _send_input_text(self):
        """发送输入框中的文本"""
        text = self.input_entry.get()
        if not text:
            return
            
        def send_text():
            try:
                logging.info(f"发送文本: {text}")
                self.input_tester.type_text(text, interval=0.05)
                self._post_ui(self._update_output, text)
                
                logging.info("文本任务完成；目标应用接收情况未验证")
                # 清空输入框
                self._post_ui(self.input_entry.delete, 0, tk.END)
                
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"发送文本出错: {str(e)}")
        
        # 在新线程中执行发送操作
        self._start_worker(send_text)

    def _update_output(self, text):
        """更新输出框的文本"""
        self.output_entry.config(state='normal')
        self.output_entry.delete(0, tk.END)
        self.output_entry.insert(0, text)
        self.output_entry.config(state='readonly')
    
    def _get_status_text(self, status):
        """获取状态文本"""
        status_map = {
            0: "未知",
            1: "正常",
            2: "错误"
        }
        return status_map.get(status, "未知")
    
    def _check_driver_status(self):
        """检查驱动状态"""
        if not self.input_tester:
            return
            
        def check():
            try:
                logging.info("开始检查驱动状态")
                self.input_tester.check_device_status()
                
                # 获取状态
                driver_status = self.input_tester.get_driver_status()
                last_check = self.input_tester.get_last_check_time()
                
                # 更新显示
                self._post_ui(self.driver_status.config,
                              text=f"驱动状态: {self._get_status_text(driver_status)}")
                
                # 格式化时间（DLL返回的是Unix时间戳，毫秒为单位）
                if last_check > 0:
                    time_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(last_check/1000))
                    self._post_ui(self.last_check.config, text=f"上次检查: {time_str}")
                
                logging.info(f"驱动状态: {self._get_status_text(driver_status)}")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"检查驱动状态出错: {str(e)}")
        
        self._start_worker(check)
    
    def create_path_frame(self, parent=None):
        """创建路径配置区域
        
        Args:
            parent: 父级容器，如果为None则使用self.left_panel
        """
        # 如果没有指定父级容器，则使用left_panel
        if parent is None:
            parent = self.left_panel
            
        path_frame = ttk.LabelFrame(parent, text="驱动路径配置", padding="5")
        path_frame.pack(fill="x", padx=5, pady=5)
        
        # DLL路径配置
        ttk.Label(path_frame, text="DLL路径:").grid(row=0, column=0, sticky="w")
        # 使用已经在__init__中创建的StringVar
        self.dll_path_entry = ttk.Entry(path_frame, textvariable=self.dll_path_var, width=30, font=self.default_font)
        self.dll_path_entry.grid(row=0, column=1, padx=5)
        ttk.Button(path_frame, text="浏览", command=lambda: self.browse_file("dll")).grid(row=0, column=2)
        
        # SYS路径配置
        ttk.Label(path_frame, text="SYS路径:").grid(row=1, column=0, sticky="w")
        # 使用已经在__init__中创建的StringVar
        self.sys_path_entry = ttk.Entry(path_frame, textvariable=self.sys_path_var, width=30, font=self.default_font)
        self.sys_path_entry.grid(row=1, column=1, padx=5)
        ttk.Button(path_frame, text="浏览", command=lambda: self.browse_file("sys")).grid(row=1, column=2)
        
        # 保存配置按钮
        ttk.Button(path_frame, text="保存配置", command=self.save_path_config).grid(row=2, column=1, pady=5)
    
    def browse_file(self, file_type):
        """浏览文件对话框
        
        Args:
            file_type (str): 文件类型，"dll" 或 "sys"
        """
        filetypes = [("DLL files", "*.dll")] if file_type == "dll" else [("SYS files", "*.sys")]
        filename = filedialog.askopenfilename(filetypes=filetypes)
        if filename:
            if file_type == "dll":
                self.dll_path_var.set(filename)
            else:
                self.sys_path_var.set(filename)
    
    def save_path_config(self):
        """保存路径配置到文件"""
        # 获取当前路径设置
        dll_path = self.dll_path_var.get().strip()
        sys_path = self.sys_path_var.get().strip()
        
        # 验证路径
        if not dll_path or not sys_path:
            logging.error("DLL路径和SYS路径不能为空")
            return
            
        config = {
            # 驱动路径配置
            "dll_path": dll_path,
            "sys_path": sys_path,
            
            # 高频按键测试配置
            "rapid_test": {
                "test_key": self.test_key_var.get(),
                "press_time": self.press_time_var.get(),
                "interval_time": self.interval_time_var.get(),
                "duration": self.duration_var.get()
            }
        }
        
        try:
            self._save_config_updates(config)
            logging.info("配置保存成功")
        except Exception as e:
            error_msg = f"保存配置失败: {str(e)}"
            logging.error(error_msg)
            messagebox.showerror("错误", error_msg)

    def _save_config_updates(self, updates):
        try:
            with open(self.config_file, "r", encoding="utf-8") as source:
                config = json.load(source)
        except FileNotFoundError:
            config = {}
        if not isinstance(config, dict):
            raise ValueError("配置文件必须包含 JSON 对象，未覆盖原文件")
        for key, value in updates.items():
            if isinstance(value, dict):
                section = config.get(key, {})
                if not isinstance(section, dict):
                    raise ValueError(f"配置项 {key} 必须是 JSON 对象，未覆盖原文件")
                config[key] = {**section, **value}
            else:
                config[key] = value

        temporary_path = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False,
                                             dir=os.path.dirname(os.path.abspath(self.config_file))) as target:
                temporary_path = target.name
                json.dump(config, target, indent=4, allow_nan=False)
            os.replace(temporary_path, self.config_file)
            temporary_path = None
        finally:
            if temporary_path is not None:
                os.unlink(temporary_path)
        return config
    
    def load_path_config(self):
        """加载保存的路径配置"""
        try:
            if os.path.exists(self.config_file):
                with open(self.config_file, "r", encoding="utf-8") as f:
                    config = json.load(f)
                    
                # 加载驱动路径配置
                dll_path = config.get("dll_path", "")
                sys_path = config.get("sys_path", "")
                
                # 设置路径变量
                if dll_path:
                    self.dll_path_var.set(dll_path)
                    
                if sys_path:
                    self.sys_path_var.set(sys_path)
                
                # 加载高频按键测试配置
                rapid_test_config = config.get("rapid_test", {})
                if rapid_test_config:
                    self.test_key_var.set(rapid_test_config.get("test_key", "a"))
                    self.press_time_var.set(rapid_test_config.get("press_time", "1"))
                    self.interval_time_var.set(rapid_test_config.get("interval_time", "1"))
                    self.duration_var.set(rapid_test_config.get("duration", "1"))
                    logging.info("已加载高频按键测试配置")
                    
        except Exception as e:
            logging.error(f"加载配置失败: {str(e)}")
    
    def initialize_driver(self):
        return self._load_driver()

    def run(self):
        """运行GUI"""
        self.root.mainloop()
        
        # 确保程序退出时清理资源
        if self.driver_mgr:
            self.driver_mgr.cleanup()
    
    def _toggle_rapid_test(self):
        """切换高频按键测试状态"""
        if self.is_unloading or self.is_closing:
            return
        if self._rapid_test_pending:
            self.rapid_test_running = False
            self.rapid_test_btn.config(state=tk.DISABLED)
            self.rapid_test_status.config(text="状态: 正在取消，等待原生结果")
            return
        if not self.rapid_test_running:
            # 开始测试
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    messagebox.showerror("错误", "请先加载驱动！")
                    return
                
                # 获取参数
                test_key = self.test_key_var.get()
                if not test_key:
                    messagebox.showerror("错误", "请输入测试按键！")
                    return
                    
                press_time = int(self.press_time_var.get())
                interval_time = int(self.interval_time_var.get())
                InputTester.repeat_count(float(self.duration_var.get()), press_time / 1000,
                                         interval_time / 1000)
                
                # 获取运行时长并验证
                try:
                    duration = float(self.duration_var.get())
                    if duration < 1:
                        messagebox.showerror("错误", "运行时长必须大于等于1秒！")
                        return
                except ValueError:
                    messagebox.showerror("错误", "请输入有效的运行时长！")
                    return
                
                # 检查按键是否有效
                if VirtualKeys.get_vk_code(test_key) is None:
                    messagebox.showerror("错误", f"无效的按键: {test_key}")
                    return
                
                # 重置所有计数和统计数据
                self.press_count = 0
                self._update_press_count()
                self.run_time_label.config(text="运行时间: 0秒")
                self.click_rate_label.config(text="平均频率: 0次/秒")
                
                # 更新UI状态
                self.rapid_test_btn.config(text="停止测试")
                self.rapid_test_status.config(text="状态: 运行中")
                self.rapid_test_running = True
                self._rapid_test_pending = True
                
                # 启动测试线程
                self.rapid_test_thread = self._start_worker(
                    self._run_rapid_test,
                    (test_key, press_time/1000.0, interval_time/1000.0, duration))
                
            except Exception as error:
                self._stop_test("状态: 未启动")
                messagebox.showerror("错误", str(error))
                return
    
    def _run_rapid_test(self, key, press_time, interval_time, duration):
        """Submit one finite repeat task; polling only updates the UI."""
        start_time = time.monotonic()

        def progress(result):
            elapsed = time.monotonic() - start_time
            self._post_ui(self._update_press_count, result.completedRepeats)
            self._post_ui(self._update_status, elapsed,
                          result.completedRepeats / elapsed if elapsed else 0)

        status = "状态: 任务完成"
        try:
            if not self.input_tester:
                raise RuntimeError("输入测试器未初始化")
            self.input_tester.repeat_key(key, press_time, interval_time, duration,
                                         cancelled=lambda: not self.rapid_test_running,
                                         progress=progress)
        except WorkCancelled:
            status = "状态: 已取消"
        except Exception as error:
            status = "状态: 出错，请卸载重试清理"
            error_msg = f"高频按键测试出错: {error}"
            logging.error(error_msg)
            self._post_ui(messagebox.showerror, "错误", error_msg)
        finally:
            self._post_ui(self._stop_test, status)
    
    def _update_status(self, elapsed_time, rate):
        """更新状态显示
        
        Args:
            elapsed_time (float): 运行时间（秒）
            rate (float): 点击频率（次/秒）
        """
        self.run_time_label.config(text=f"运行时间: {round(elapsed_time, 1)}秒")
        self.click_rate_label.config(text=f"平均频率: {rate:.1f}次/秒")
    
    def _stop_test(self, status="状态: 任务完成"):
        """停止测试"""
        self.rapid_test_running = False
        self._rapid_test_pending = False
        self.rapid_test_btn.config(text="开始测试", state=tk.NORMAL if self.is_driver_loaded
                                   and not self.is_unloading else tk.DISABLED)
        self.rapid_test_status.config(text=status)
    
    def _update_press_count(self, count=None):
        """更新按键计数显示"""
        if count is not None:
            self.press_count = count
        self.press_count_label.config(text=f"按键次数: {self.press_count}")
    
    def check_privileges(self):
        """检查权限并在需要时请求提升
        
        Returns:
            bool: 如果已经有管理员权限返回True，否则返回False
        """
        try:
            if ctypes.windll.shell32.IsUserAnAdmin():
                return True
                
            # 需要提权
            response = messagebox.askyesno(
                "权限提示",
                "此程序需要管理员权限才能正常使用驱动功能。\n是否以管理员身份重新启动？\n\n" +
                "点击'否'将以普通权限运行，但驱动相关功能可能无法使用。"
            )
            
            if response:
                # 准备以管理员身份重启
                script = os.path.abspath(sys.argv[0])
                
                # 根据调试模式选择python解释器
                if DEBUG_MODE:
                    python_exe = sys.executable  # 使用python.exe
                    window_mode = SW_SHOWNORMAL  # 显示窗口
                else:
                    # 使用pythonw.exe
                    pythonw_path = os.path.join(os.path.dirname(sys.executable), 'pythonw.exe')
                    python_exe = pythonw_path if os.path.exists(pythonw_path) else sys.executable
                    window_mode = SW_HIDE  # 隐藏窗口
                    
                # 构建参数字符串
                params = ' '.join([f'"{arg}"' for arg in sys.argv[1:]])
                
                ret = ctypes.windll.shell32.ShellExecuteW(
                    None, 
                    "runas",
                    python_exe,
                    f'"{script}" {params}',
                    None,
                    window_mode
                )
                
                # 返回值大于32表示成功
                if ret > 32:
                    return False  # 返回False以关闭当前实例
                else:
                    messagebox.showerror(
                        "错误",
                        f"无法获取管理员权限 (错误码: {ret})"
                    )
                    return True  # 继续以普通权限运行
            else:
                # 用户选择不提升权限
                messagebox.showwarning(
                    "权限提示",
                    "您选择了以普通权限运行程序。\n" +
                    "请注意：驱动相关功能将无法使用，但其他功能不受影响。\n" +
                    "如需使用完整功能，请重新启动程序并选择以管理员身份运行。"
                )
                return True  # 继续以普通权限运行
            
        except Exception as e:
            messagebox.showerror("错误", f"权限检查失败: {str(e)}")
            return True  # 出错时也继续运行

    def _test_mouse_click(self, button: str) -> None:
        """测试鼠标点击
        
        Args:
            button: 鼠标按钮，"left", "right", "middle", "x1", "x2"
        """
        def run_test():
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    logging.error("驱动未加载或测试器未初始化")
                    self._post_ui(messagebox.showerror, "错误", "请先加载驱动")
                    return

                # 获取屏幕信息
                screen_width = ctypes.windll.user32.GetSystemMetrics(0)  # SM_CXSCREEN
                screen_height = ctypes.windll.user32.GetSystemMetrics(1)  # SM_CYSCREEN
                
                # 计算测试位置（屏幕中心偏右）
                test_x = screen_width * 3 // 4
                test_y = screen_height // 2

                # 移动到测试位置
                logging.info(f"移动鼠标到测试位置: ({test_x}, {test_y})")
                actions = [action(MOVE_ABSOLUTE, test_x, test_y, 1000)]

                # 根据按钮类型执行不同的测试
                if button == "left":
                    logging.info("测试左键点击 - 请观察鼠标位置的选择效果")
                    # 双击测试
                    actions.extend(InputTester.click_actions("left", interval=0.1))
                    actions.extend(InputTester.click_actions("left"))
                    
                elif button == "right":
                    logging.info("测试右键点击 - 请观察是否出现右键菜单")
                    actions.extend(InputTester.click_actions("right"))
                    
                elif button == "middle":
                    # 先移动到浏览器标签区域位置
                    test_y = screen_height // 8
                    logging.info(f"移动鼠标到标签栏位置: ({test_x}, {test_y})")
                    actions.append(action(MOVE_ABSOLUTE, test_x, test_y, 1000))
                    
                    logging.info("测试中键点击 - 如果在浏览器中，应该会打开新标签页")
                    actions.extend(InputTester.click_actions("middle"))
                    
                elif button == "x1":
                    logging.info("测试X1键点击 - 如果在浏览器中，应该会后退")
                    actions.extend(InputTester.click_actions("x1"))
                    
                elif button == "x2":
                    logging.info("测试X2键点击 - 如果在浏览器中，应该会前进")
                    actions.extend(InputTester.click_actions("x2"))

                self.input_tester.run_actions(actions)
                logging.info(f"{button}键测试任务完成")
                    
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"鼠标点击测试出错: {str(e)}")
                self._post_ui(messagebox.showerror, "错误", f"鼠标点击测试出错: {str(e)}")

        # 在新线程中运行测试
        self._start_worker(run_test)

    def _test_mouse_wheel(self, direction: str) -> None:
        """测试鼠标滚轮
        
        Args:
            direction: 滚动方向，"up" 或 "down"
        """
        try:
            wheel_delta = int(self.wheel_delta_var.get())
            if not 1 <= wheel_delta <= 65535:
                raise ValueError("滚动量必须为 1..65535")
        except ValueError as error:
            messagebox.showerror("错误", str(error))
            return

        def run_test():
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    logging.error("驱动未加载或测试器未初始化")
                    self._post_ui(messagebox.showerror, "错误", "请先加载驱动")
                    return

                logging.info(f"滚轮测试 - 方向: {direction}, 滚动量: {wheel_delta}")
                    
                # 检查驱动状态
                self.input_tester.check_device_status()
                driver_status = self.input_tester.get_driver_status()
                if driver_status != 1:  # DEVICE_STATUS_READY
                    logging.error(f"驱动状态异常: {driver_status}")
                    self._post_ui(messagebox.showerror, "错误", "驱动状态异常")
                    return

                logging.info("滚轮测试将在3秒后开始...")
                self.input_workers.wait(3)
                    
                # 根据方向调用相应函数
                if direction == "up":
                    logging.info("执行向上滚动")
                    if not self.input_tester.mouse_wheel_up(wheel_delta):
                        logging.error("鼠标滚轮向上滚动失败")
                        self._post_ui(messagebox.showerror, "错误", "鼠标滚轮向上滚动失败")
                    else:
                        logging.info("向上滚动成功")
                else:
                    logging.info("执行向下滚动")
                    if not self.input_tester.mouse_wheel_down(wheel_delta):
                        logging.error("鼠标滚轮向下滚动失败")
                        self._post_ui(messagebox.showerror, "错误", "鼠标滚轮向下滚动失败")
                    else:
                        logging.info("向下滚动成功")
                        
            except ValueError as ve:
                logging.error(f"数值转换错误: {str(ve)}")
                self._post_ui(messagebox.showerror, "错误", "请输入有效的滚动量数值")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"鼠标滚轮测试出错: {str(e)}")
                self._post_ui(messagebox.showerror, "错误", f"鼠标滚轮测试出错: {str(e)}")

        # 使用新线程运行滚轮测试，避免阻塞GUI
        self._start_worker(run_test)

    def _move_to_corner(self, corner: str) -> None:
        """移动鼠标到主显示器的四角
        
        Args:
            corner: 目标角落，可选值："top_left", "top_right", "bottom_left", "bottom_right"
        """
        def run_move():
            corner_name = corner
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    logging.error("驱动未加载或测试器未初始化")
                    self._post_ui(messagebox.showerror, "错误", "请先加载驱动")
                    return

                # 获取主显示器信息
                screen_width = ctypes.windll.user32.GetSystemMetrics(0)   # SM_CXSCREEN
                screen_height = ctypes.windll.user32.GetSystemMetrics(1)  # SM_CYSCREEN

                # 设置边缘偏移量（避免完全处于边缘）
                EDGE_OFFSET = 1

                # 根据选择的角落确定目标坐标
                if corner == "top_left":
                    target_x, target_y = EDGE_OFFSET, EDGE_OFFSET
                    corner_name = "左上角"
                elif corner == "top_right":
                    target_x, target_y = screen_width - EDGE_OFFSET, EDGE_OFFSET
                    corner_name = "右上角"
                elif corner == "bottom_left":
                    target_x, target_y = EDGE_OFFSET, screen_height - EDGE_OFFSET
                    corner_name = "左下角"
                else:  # bottom_right
                    target_x, target_y = screen_width - EDGE_OFFSET, screen_height - EDGE_OFFSET
                    corner_name = "右下角"

                logging.info(f"移动鼠标到主显示器{corner_name}: ({target_x}, {target_y})")
                self.input_tester.mouse_move_abs(target_x, target_y)

            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"移动到{corner_name}失败: {str(e)}")
                self._post_ui(messagebox.showerror, "错误", f"移动到{corner_name}失败: {str(e)}")

        # 在新线程中运行移动操作
        self._start_worker(run_move)

    def _test_smooth_move_rel(self, direction: str) -> None:
        """测试相对平滑移动
        
        Args:
            direction: 移动方向，可选值："left", "right", "up", "down"
        """
        try:
            distance = int(self.rel_distance_var.get())
            if distance <= 0:
                raise ValueError("移动距离必须大于0")
        except ValueError as error:
            messagebox.showerror("错误", str(error))
            return

        def run_test():
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    logging.error("驱动未加载或测试器未初始化")
                    self._post_ui(messagebox.showerror, "错误", "请先加载驱动")
                    return

                # 根据方向设置移动参数
                dx, dy = 0, 0
                if direction == "left":
                    dx = -distance
                elif direction == "right":
                    dx = distance
                elif direction == "up":
                    dy = -distance
                else:  # down
                    dy = distance

                logging.info(f"开始相对平滑移动测试 - 方向: {direction}, 距离: {distance}")
                
                # 执行平滑移动
                STEPS = 10  # 将移动分成10步
                
                self.input_workers.wait(3)
                logging.info("3秒后开始移动")
                
                actions = []
                previous_x, previous_y = 0, 0
                for step in range(1, STEPS + 1):
                    current_x, current_y = round(dx * step / STEPS), round(dy * step / STEPS)
                    actions.append(action(MOVE_RELATIVE, current_x - previous_x,
                                          current_y - previous_y, 20))
                    previous_x, previous_y = current_x, current_y
                self.input_tester.run_actions(actions)
                    
                logging.info("相对平滑移动任务完成")

            except ValueError as ve:
                logging.error(f"数值转换错误: {str(ve)}")
                self._post_ui(messagebox.showerror, "错误", "请输入有效的移动距离")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"相对平滑移动测试出错: {str(e)}")
                self._post_ui(messagebox.showerror, "错误", f"相对平滑移动测试出错: {str(e)}")

        # 在新线程中运行测试
        self._start_worker(run_test)

    def _test_smooth_move_abs(self) -> None:
        """测试绝对平滑移动"""
        try:
            target_x = int(self.abs_x_var.get())
            target_y = int(self.abs_y_var.get())
        except ValueError:
            messagebox.showerror("错误", "请输入有效的坐标值")
            return

        def run_test():
            try:
                # 检查驱动状态
                if not self.is_driver_loaded or not self.input_tester:
                    logging.error("驱动未加载或测试器未初始化")
                    self._post_ui(messagebox.showerror, "错误", "请先加载驱动")
                    return

                # 获取当前鼠标位置
                cursor = win32gui.GetCursorPos()
                start_x, start_y = cursor[0], cursor[1]

                # 计算移动距离
                dx = target_x - start_x
                dy = target_y - start_y

                logging.info(f"开始绝对平滑移动测试 - 目标位置: ({target_x}, {target_y})")
                
                # 执行平滑移动
                STEPS = 20  # 将移动分成20步
                actions = []
                for step in range(STEPS + 1):
                    # 使用缓动函数使移动更自然
                    progress = step / STEPS
                    eased_progress = progress * (2 - progress)  # 二次缓动
                    
                    current_x = int(start_x + dx * eased_progress)
                    current_y = int(start_y + dy * eased_progress)
                    
                    actions.append(action(MOVE_ABSOLUTE, current_x, current_y, 10))
                self.input_tester.run_actions(actions)
                    
                logging.info("绝对平滑移动任务完成")

            except ValueError as ve:
                logging.error(f"数值转换错误: {str(ve)}")
                self._post_ui(messagebox.showerror, "错误", "请输入有效的坐标值")
            except WorkCancelled:
                return
            except Exception as e:
                logging.error(f"绝对平滑移动测试出错: {str(e)}")
                self._post_ui(messagebox.showerror, "错误", f"绝对平滑移动测试出错: {str(e)}")

        # 在新线程中运行测试
        self._start_worker(run_test)

    def _toggle_keyboard_hook(self):
        """切换键盘钩子状态"""
        if self.is_unloading or self.is_closing:
            return
        if not self.keyboard_hook:
            try:
                # 注册热键回调
                hotkey = self.hotkey_var.get().upper()
                vk_code = VirtualKeys.get_vk_code(hotkey)
                if not vk_code:
                    messagebox.showerror("错误", f"无效的热键: {hotkey}")
                    return
                    
                self.keyboard_hook = KeyboardHook()
                self.keyboard_hook.register_hotkey(vk_code, lambda: self._post_ui(self._toggle_auto_move))
                self.keyboard_hook.start()
                
                # 更新UI
                self.start_hook_btn.config(text="停止热键监听")
                self.auto_move_status.config(text="状态: 等待热键触发")
                
            except Exception as e:
                messagebox.showerror("错误", f"启动键盘钩子失败: {str(e)}")
                # Retain the object until stop() confirms its native thread exited.
                self.start_hook_btn.config(text="停止热键监听" if self.keyboard_hook else "启动热键监听")
                
        else:
            try:
                # 停止键盘钩子
                self.keyboard_hook.stop()
                self.keyboard_hook = None
                
                # 确保自动移动也停止
                self.auto_move_running = False
                
                # 更新UI
                self.start_hook_btn.config(text="启动热键监听")
                self.auto_move_status.config(text="状态: 正在取消" if self._auto_move_pending else "状态: 未启动")
                
            except Exception as e:
                messagebox.showerror("错误", f"停止键盘钩子失败: {str(e)}")

    def _toggle_auto_move(self):
        """切换自动移动状态"""
        if self.is_unloading or self.is_closing:
            return
        if not self.is_driver_loaded or not self.input_tester:
            messagebox.showerror("错误", "请先加载驱动")
            return
        if self._auto_move_pending:
            self.auto_move_running = False
            self.auto_move_status.config(text="状态: 正在取消，等待原生结果")
            return
            
        if not self.auto_move_running:
            try:
                # 获取设置
                speed = float(self.speed_var.get())
                move_range = float(self.range_var.get())
                
                self._auto_move_actions(speed, move_range)
                
                # 启动自动移动
                self.auto_move_running = True
                self._auto_move_pending = True
                self.auto_move_status.config(text=f"状态: 自动移动中（最多{MAX_REPEATS}圈）")
                
                # 在新线程中运行自动移动
                self._start_worker(self._run_auto_move, (speed, move_range))
                
            except Exception as error:
                self._finish_auto_move("状态: 未启动")
                messagebox.showerror("错误", str(error))
                return

    @staticmethod
    def _auto_move_actions(speed, move_range):
        if not all(math.isfinite(value) and value > 0 for value in (speed, move_range)):
            raise ValueError("速度和范围必须为正的有限数值")
        radius = move_range * 0.5
        if speed < 2 * math.pi / (MAX_ACTIONS * 0.01) or radius > MAX_ACTIONS * 5 / (2 * math.pi):
            raise ValueError(f"轨迹最多 {MAX_ACTIONS} 点；请增大速度或减小范围")
        count = math.ceil(max(2 * math.pi / (speed * 0.01), 2 * math.pi * radius / 5))
        if not 1 <= count <= MAX_ACTIONS:
            raise ValueError(f"轨迹最多 {MAX_ACTIONS} 点；请增大速度或减小范围")
        steps = []
        previous_x, previous_y = round(radius), 0
        for index in range(1, count + 1):
            angle = index * 2 * math.pi / count
            current_x, current_y = round(radius * math.cos(angle)), round(radius * math.sin(angle))
            steps.append(action(MOVE_RELATIVE, current_x - previous_x, current_y - previous_y, 16))
            previous_x, previous_y = current_x, current_y
        if not any(step.value1 or step.value2 for step in steps):
            raise ValueError("轨迹没有有效位移；请调整移动范围或速度")
        return steps

    def _run_auto_move(self, speed, move_range):
        status = "状态: 任务完成"
        try:
            self.input_tester.run_actions(self._auto_move_actions(speed, move_range),
                                          repeats=MAX_REPEATS,
                                          cancelled=lambda: not self.auto_move_running)
        except WorkCancelled:
            status = "状态: 已取消，等待热键触发"
        except Exception as error:
            status = "状态: 出错，请卸载重试清理"
            logging.error("自动移动失败: %s", error)
        finally:
            self._post_ui(self._finish_auto_move, status)

    def _finish_auto_move(self, status):
        self.auto_move_running = False
        self._auto_move_pending = False
        self.auto_move_status.config(text=status)

    def save_auto_move_config(self):
        """保存自动移动配置"""
        try:
            auto_move = {
                'hotkey': self.hotkey_var.get(),
                'speed': float(self.speed_var.get()),
                'range': float(self.range_var.get())
            }
            config = self._save_config_updates({'auto_move': auto_move})
            self.auto_move_config = config['auto_move']
                
            messagebox.showinfo("成功", "配置保存成功！")
            
        except Exception as e:
            messagebox.showerror("错误", f"保存配置失败: {str(e)}")

    def load_auto_move_config(self):
        """加载自动移动配置"""
        try:
            if os.path.exists(self.config_file):
                with open(self.config_file, 'r', encoding="utf-8") as f:
                    config = json.load(f)
                    
                if 'auto_move' in config:
                    self.auto_move_config.update(config['auto_move'])
                    
        except Exception as e:
            logging.error(f"加载自动移动配置失败: {str(e)}")


if __name__ == '__main__':
    try:
        app = CQKeysGUI()
        app.run()
        
    except Exception as e:
        # 其他未预期的错误
        messagebox.showerror("错误", f"程序运行出错: {str(e)}")
