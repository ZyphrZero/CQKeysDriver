# -*- coding: utf-8 -*-

import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import threading
from driver_manager import DriverManager
from input_tester import InputTester
from virtuakeys_mapping import VirtualKeys
import logging
import os
import json
import sys
import ctypes
import argparse

# 添加命令行参数解析
# python gui.py --debug
parser = argparse.ArgumentParser()
parser.add_argument('--debug', action='store_true', help='启用调试模式')
args = parser.parse_args()

# 使用命令行参数设置调试模式
DEBUG_MODE = args.debug

# 在类外部或文件开头定义常量
SW_HIDE = 0
SW_SHOWNORMAL = 1
SW_MINIMIZE = 6
SW_MAXIMIZE = 3

class LYKeysGUI:
    def __init__(self):
        # 在创建窗口前检查权限
        if not self.check_privileges():
            sys.exit(0)  # 干净地退出程序
        
        # 初始化DPI感知
        self._init_dpi_awareness()
        
        self.root = tk.Tk()
        self.root.title("键鼠驱动测试工具")
        self.root.geometry("570x750")
        
        # 获取DPI缩放比例
        self.dpi_scale = self._get_dpi_scale()
        
        # 初始化组件
        self.driver_mgr = None  # 驱动管理器
        self.input_tester = None  # 输入测试器
        self.is_driver_loaded = False  # 驱动是否已加载
        self.is_closing = False  # 是否正在关闭程序
        
        self._setup_logging()   # 设置日志处理器
        self._create_widgets()  # 创建GUI组件
        
        # 绑定窗口关闭事件
        self.root.protocol("WM_DELETE_WINDOW", self._on_closing)
        
        # 配置文件路径
        self.config_file = "driver_config.json"
        
        # 创建路径配置frame
        self.create_path_frame()
        
        # 加载保存的路径配置
        self.load_path_config()
    
    def _on_closing(self):
        """处理窗口关闭事件"""
        self.is_closing = True  # 设置关闭标志
        if self.driver_mgr:
            try:
                self.driver_mgr.cleanup()
            except Exception as e:
                print(f"清理驱动时出错: {str(e)}")  # 使用print而不是logging
        self.root.destroy()
    
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
                    self.text_widget.insert(tk.END, msg)
                    self.text_widget.see(tk.END)
        
        # 创建日志区域
        self.log_frame = ttk.LabelFrame(self.root, text="日志输出", padding="5")
        self.log_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        self.log_text = tk.Text(self.log_frame, height=10, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        
        # 配置日志处理
        handler = TextHandler(self.log_text, self)
        handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))
        logging.getLogger().addHandler(handler)
        logging.getLogger().setLevel(logging.INFO)
    
    def _create_widgets(self):
        """创建GUI组件"""
        # 驱动控制区域
        driver_frame = ttk.LabelFrame(self.root, text="驱动控制", padding="5")
        driver_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.load_btn = ttk.Button(driver_frame, text="安装驱动", command=self._load_driver)
        self.load_btn.pack(side=tk.LEFT, padx=5)
        
        self.unload_btn = ttk.Button(driver_frame, text="卸载驱动", command=self._unload_driver, state=tk.DISABLED)
        self.unload_btn.pack(side=tk.LEFT, padx=5)
        
        # 状态检查区域
        status_frame = ttk.LabelFrame(self.root, text="驱动状态", padding="5")
        status_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 状态显示标签
        self.driver_status = ttk.Label(status_frame, text="驱动状态: 未知")
        self.driver_status.pack(side=tk.LEFT, padx=5, pady=2)
        
        self.last_check = ttk.Label(status_frame, text="上次检查: 未检查")
        self.last_check.pack(side=tk.LEFT, padx=5, pady=2)
        
        # 状态检查按钮
        self.check_status_btn = ttk.Button(status_frame, text="检查驱动状态", 
                                       command=self._check_driver_status, state=tk.DISABLED)
        self.check_status_btn.pack(side=tk.RIGHT, padx=5)
        
        # 输入/输出区域
        io_frame = ttk.LabelFrame(self.root, text="输入/输出", padding="5")
        io_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 左侧输入区域
        input_frame = ttk.Frame(io_frame)
        input_frame.pack(side=tk.LEFT, fill=tk.X, expand=True)
        
        ttk.Label(input_frame, text="输入文本:").pack(side=tk.TOP, anchor=tk.W)
        self.input_entry = ttk.Entry(input_frame)
        self.input_entry.pack(side=tk.TOP, fill=tk.X, padx=5, pady=2)
        self.input_entry.bind('<Return>', self._on_input_submit)
        
        # 右侧输出区域
        output_frame = ttk.Frame(io_frame)
        output_frame.pack(side=tk.LEFT, fill=tk.X, expand=True)
        
        ttk.Label(output_frame, text="模拟输入结果:").pack(side=tk.TOP, anchor=tk.W)
        self.output_entry = ttk.Entry(output_frame, state='readonly')
        self.output_entry.pack(side=tk.TOP, fill=tk.X, padx=5, pady=2)
        
        # 按钮区域
        button_frame = ttk.Frame(io_frame)
        button_frame.pack(side=tk.LEFT, padx=5)
        
        self.submit_btn = ttk.Button(button_frame, text="发送", command=self._on_submit_click)
        self.submit_btn.pack(side=tk.TOP, pady=2)
        
        self.clear_btn = ttk.Button(button_frame, text="清除", command=self._clear_text)
        self.clear_btn.pack(side=tk.TOP, pady=2)
        
        # 测试功能区域
        test_frame = ttk.LabelFrame(self.root, text="测试功能", padding="5")
        test_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.test_key_btn = ttk.Button(test_frame, text="按键测试", command=self._test_keyboard, state=tk.DISABLED)
        self.test_key_btn.pack(side=tk.LEFT, padx=5)
        
        self.test_input_btn = ttk.Button(test_frame, text="输入测试", command=self._test_input, state=tk.DISABLED)
        self.test_input_btn.pack(side=tk.LEFT, padx=5)
        
        self.test_mouse_btn = ttk.Button(test_frame, text="鼠标测试", command=self._test_mouse, state=tk.DISABLED)
        self.test_mouse_btn.pack(side=tk.LEFT, padx=5)
        
        # 高频按键测试区域
        rapid_test_frame = ttk.LabelFrame(self.root, text="高频按键测试", padding="5")
        rapid_test_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 第一行：按键和时间设置
        # 按键选择
        ttk.Label(rapid_test_frame, text="测试按键:").grid(row=0, column=0, sticky="w", padx=5)
        self.test_key_var = tk.StringVar(value="a")  # 默认测试a键
        self.test_key_entry = ttk.Entry(rapid_test_frame, textvariable=self.test_key_var, width=10)
        self.test_key_entry.grid(row=0, column=1, sticky="w", padx=5)
        
        # 按下时间设置
        ttk.Label(rapid_test_frame, text="按下时间(ms):").grid(row=0, column=2, sticky="w", padx=5)
        self.press_time_var = tk.StringVar(value="50")  # 默认50ms
        self.press_time_entry = ttk.Entry(rapid_test_frame, textvariable=self.press_time_var, width=10)
        self.press_time_entry.grid(row=0, column=3, sticky="w", padx=5)
        
        # 间隔时间设置
        ttk.Label(rapid_test_frame, text="间隔时间(ms):").grid(row=0, column=4, sticky="w", padx=5)
        self.interval_time_var = tk.StringVar(value="50")  # 默认50ms
        self.interval_time_entry = ttk.Entry(rapid_test_frame, textvariable=self.interval_time_var, width=10)
        self.interval_time_entry.grid(row=0, column=5, sticky="w", padx=5)
        
        # 第二行：运行时长设置
        ttk.Label(rapid_test_frame, text="运行时长(秒):").grid(row=1, column=0, sticky="w", padx=5)
        self.duration_var = tk.StringVar(value="0")  # 默认0秒，表示无限运行
        self.duration_entry = ttk.Entry(rapid_test_frame, textvariable=self.duration_var, width=10)
        self.duration_entry.grid(row=1, column=1, sticky="w", padx=5)
        ttk.Label(rapid_test_frame, text="(0表示无限运行)").grid(row=1, column=2, sticky="w", padx=5)
        
        # 控制按钮
        self.rapid_test_btn = ttk.Button(rapid_test_frame, text="开始测试", command=self._toggle_rapid_test)
        self.rapid_test_btn.grid(row=1, column=5, columnspan=2, padx=5)
        
        # 第三行：状态显示
        status_frame = ttk.Frame(rapid_test_frame)
        status_frame.grid(row=2, column=0, columnspan=7, sticky="ew", padx=5, pady=5)
        
        # 测试状态
        self.rapid_test_status = ttk.Label(status_frame, text="状态: 未开始")
        self.rapid_test_status.pack(side=tk.LEFT)
        
        # 运行时间
        self.run_time_label = ttk.Label(status_frame, text="运行时间: 0秒")
        self.run_time_label.pack(side=tk.LEFT, padx=20)
        
        # 按键次数
        self.press_count = 0
        self.press_count_label = ttk.Label(status_frame, text="按键次数: 0")
        self.press_count_label.pack(side=tk.LEFT, padx=20)
        
        # 点击频率
        self.click_rate_label = ttk.Label(status_frame, text="平均频率: 0次/秒")
        self.click_rate_label.pack(side=tk.LEFT, padx=20)
        
        # 添加鼠标绝对移动测试区域
        mouse_abs_frame = ttk.LabelFrame(self.root, text="鼠标绝对移动测试", padding="5")
        mouse_abs_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 坐标输入区域
        coord_frame = ttk.Frame(mouse_abs_frame)
        coord_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # X坐标输入
        ttk.Label(coord_frame, text="X坐标:").grid(row=0, column=0, padx=5)
        self.x_coord_var = tk.StringVar(value="0")
        self.x_coord_entry = ttk.Entry(coord_frame, textvariable=self.x_coord_var, width=10)
        self.x_coord_entry.grid(row=0, column=1, padx=5)
        
        # Y坐标输入
        ttk.Label(coord_frame, text="Y坐标:").grid(row=0, column=2, padx=5)
        self.y_coord_var = tk.StringVar(value="0")
        self.y_coord_entry = ttk.Entry(coord_frame, textvariable=self.y_coord_var, width=10)
        self.y_coord_entry.grid(row=0, column=3, padx=5)
        
        # 移动按钮
        self.move_abs_btn = ttk.Button(coord_frame, text="移动到指定位置", 
                                      command=self._move_to_position, state=tk.DISABLED)
        self.move_abs_btn.grid(row=0, column=4, padx=5)
        
        # 预设位置测试区域
        preset_frame = ttk.Frame(mouse_abs_frame)
        preset_frame.pack(fill=tk.X, padx=5, pady=5)
        
        # 预设位置按钮
        positions = [
            ("左上角", self._move_top_left, "btn_top_left"),
            ("右上角", self._move_top_right, "btn_top_right"),
            ("左下角", self._move_bottom_left, "btn_bottom_left"),
            ("右下角", self._move_bottom_right, "btn_bottom_right"),
            ("屏幕中心", self._move_center, "btn_center"),
            ("自动测试", self._auto_test_absolute_move, "btn_auto_test")
        ]
        
        for i, (text, command, btn_name) in enumerate(positions):
            btn = ttk.Button(preset_frame, text=text, command=command, state=tk.DISABLED)
            btn.grid(row=0, column=i, padx=5)
            # 使用英文属性名保存按钮引用
            setattr(self, btn_name, btn)
        
        # 状态显示区域
        status_frame = ttk.Frame(mouse_abs_frame)
        status_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.mouse_pos_label = ttk.Label(status_frame, text="当前位置: (0, 0)")
        self.mouse_pos_label.pack(side=tk.LEFT, padx=5)
        
        self.move_status_label = ttk.Label(status_frame, text="移动状态: 就绪")
        self.move_status_label.pack(side=tk.LEFT, padx=5)
    
    def _update_button_states(self, driver_loaded):
        """更新按钮状态"""
        state = tk.NORMAL if driver_loaded else tk.DISABLED
        self.unload_btn.config(state=state)
        self.test_key_btn.config(state=state)
        self.test_input_btn.config(state=state)
        self.test_mouse_btn.config(state=state)
        self.submit_btn.config(state=state)
        self.clear_btn.config(state=state)
        self.input_entry.config(state=state)
        self.check_status_btn.config(state=state)
        self.load_btn.config(state=tk.DISABLED if driver_loaded else tk.NORMAL)
        self.rapid_test_btn.config(state=state)
        
        # 更新鼠标绝对移动测试按钮状态
        self.move_abs_btn.config(state=state)
        self.btn_top_left.config(state=state)
        self.btn_top_right.config(state=state)
        self.btn_bottom_left.config(state=state)
        self.btn_bottom_right.config(state=state)
        self.btn_center.config(state=state)
        self.btn_auto_test.config(state=state)
    
    def _load_driver(self):
        """加载驱动"""
        try:
            self.driver_mgr = DriverManager()
            
            # 获取当前设置的路径
            dll_path = self.dll_path_var.get()
            sys_path = self.sys_path_var.get()
            
            # 如果GUI中设置了路径，则使用GUI中的路径
            if dll_path and sys_path:
                self.driver_mgr._setup_paths(dll_path=dll_path, sys_path=sys_path)
            else:
                # 否则使用默认路径或配置文件中的路径
                self.driver_mgr._setup_paths()
                # 更新GUI显示
                self.dll_path_var.set(self.driver_mgr.dll_path)
                self.sys_path_var.set(self.driver_mgr.sys_path)
            
            if self.driver_mgr.initialize():
                self.input_tester = InputTester(self.driver_mgr.get_driver())
                self.is_driver_loaded = True
                self._update_button_states(True)
                self._check_driver_status()  # 初始检查驱动状态
                messagebox.showinfo("成功", "驱动加载成功")
            else:
                messagebox.showerror("错误", "驱动加载失败")
        except Exception as e:
            messagebox.showerror("错误", f"驱动加载出错: {str(e)}")
            logging.error(f"驱动加载出错: {str(e)}")
    
    def _unload_driver(self):
        """卸载驱动"""
        try:
            if self.driver_mgr:
                self.driver_mgr.cleanup()
                self.driver_mgr = None
                self.input_tester = None
                self.is_driver_loaded = False
                self._update_button_states(False)
                self.driver_status.config(text="驱动状态: 未加载")
                self.last_check.config(text="上次检查: 未检查")
                messagebox.showinfo("成功", "驱动卸载成功")
        except Exception as e:
            messagebox.showerror("错误", f"驱动卸载出错: {str(e)}")
    
    def _test_keyboard(self):
        """测试键盘"""
        def run_test():
            try:
                # 测试一些基本按键
                logging.info(f"键盘测试3s后开始!")
                time.sleep(3)
                test_keys = ['w', 'a', 's', 'd', 'enter']
                for key in test_keys:
                    logging.info(f"测试按键: {key}")
                    self.input_tester.press_key(key)
                    time.sleep(0.5)
                logging.info("键盘测试完成")
            except Exception as e:
                logging.error(f"键盘测试出错: {str(e)}")
        
        threading.Thread(target=run_test, daemon=True).start()
    
    def _test_input(self):
        """测试输入固定字符串"""
        def run_test():
            try:
                test_string = "Hello, World!"
                logging.info(f"测试输入: {test_string}")
                
                for char in test_string:
                    if char.isupper():
                        self.input_tester.key_down('shift')
                        self.input_tester.press_key(char.lower())
                        self.input_tester.key_up('shift')
                    else:
                        self.input_tester.press_key(char)
                    time.sleep(0.1)
                
                logging.info("输入测试完成")
            except Exception as e:
                logging.error(f"输入测试出错: {str(e)}")
        
        threading.Thread(target=run_test, daemon=True).start()
    
    def _test_mouse(self):
        """测试鼠标移动"""
        def run_test():
            try:
                # 移动测试
                moves = [
                    (100, 0),   # 右
                    (0, 100),   # 下
                    (-100, 0),  # 左
                    (0, -100)   # 上
                ]
                
                for dx, dy in moves:
                    logging.info(f"鼠标移动: ({dx}, {dy})")
                    self.input_tester.mouse_move_rel(dx, dy)
                    time.sleep(0.5)
                
                # 点击测试
                logging.info("测试鼠标点击")
                self.input_tester.mouse_click()
                
                logging.info("鼠标测试完成")
            except Exception as e:
                logging.error(f"鼠标测试出错: {str(e)}")
        
        threading.Thread(target=run_test, daemon=True).start()
    
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
                result = ""
                
                for char in text:
                    if char.isupper():
                        self.input_tester.key_down('shift')
                        self.input_tester.press_key(char.lower())
                        self.input_tester.key_up('shift')
                    else:
                        self.input_tester.press_key(char)
                    result += char
                    # 更新输出框
                    self.root.after(0, self._update_output, result)
                    time.sleep(0.05)  # 适当的延迟
                
                logging.info("文本发送完成")
                # 清空输入框
                self.root.after(0, self.input_entry.delete, 0, tk.END)
                
            except Exception as e:
                logging.error(f"发送文本出错: {str(e)}")
        
        # 在新线程中执行发送操作
        threading.Thread(target=send_text, daemon=True).start()

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
                self.root.after(0, lambda: self.driver_status.config(
                    text=f"驱动状态: {self._get_status_text(driver_status)}"))
                
                # 格式化时间（DLL返回的是Unix时间戳，毫秒为单位）
                if last_check > 0:
                    time_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(last_check/1000))
                    self.root.after(0, lambda: self.last_check.config(
                        text=f"上次检查: {time_str}"))
                
                logging.info(f"驱动状态: {self._get_status_text(driver_status)}")
            except Exception as e:
                logging.error(f"检查驱动状态出错: {str(e)}")
        
        threading.Thread(target=check, daemon=True).start()
    
    def create_path_frame(self):
        """创建路径配置区域"""
        path_frame = ttk.LabelFrame(self.root, text="驱动路径配置", padding="5")
        path_frame.pack(fill="x", padx=5, pady=5)
        
        # DLL路径配置
        ttk.Label(path_frame, text="DLL路径:").grid(row=0, column=0, sticky="w")
        self.dll_path_var = tk.StringVar()
        self.dll_path_entry = ttk.Entry(path_frame, textvariable=self.dll_path_var, width=50)
        self.dll_path_entry.grid(row=0, column=1, padx=5)
        ttk.Button(path_frame, text="浏览", command=lambda: self.browse_file("dll")).grid(row=0, column=2)
        
        # SYS路径配置
        ttk.Label(path_frame, text="SYS路径:").grid(row=1, column=0, sticky="w")
        self.sys_path_var = tk.StringVar()
        self.sys_path_entry = ttk.Entry(path_frame, textvariable=self.sys_path_var, width=50)
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
        config = {
            # 驱动路径配置
            "dll_path": self.dll_path_var.get(),
            "sys_path": self.sys_path_var.get(),
            
            # 高频按键测试配置
            "rapid_test": {
                "test_key": self.test_key_var.get(),
                "press_time": self.press_time_var.get(),
                "interval_time": self.interval_time_var.get(),
                "duration": self.duration_var.get()
            }
        }
        try:
            with open(self.config_file, "w") as f:
                json.dump(config, f, indent=4)
            messagebox.showinfo("成功", "配置保存成功！")
        except Exception as e:
            messagebox.showerror("错误", f"保存配置失败: {str(e)}")
    
    def load_path_config(self):
        """加载保存的路径配置"""
        try:
            if os.path.exists(self.config_file):
                with open(self.config_file, "r") as f:
                    config = json.load(f)
                    
                # 加载驱动路径配置
                self.dll_path_var.set(config.get("dll_path", ""))
                self.sys_path_var.set(config.get("sys_path", ""))
                
                # 加载高频按键测试配置
                rapid_test_config = config.get("rapid_test", {})
                if rapid_test_config:
                    self.test_key_var.set(rapid_test_config.get("test_key", "a"))
                    self.press_time_var.set(rapid_test_config.get("press_time", "50"))
                    self.interval_time_var.set(rapid_test_config.get("interval_time", "50"))
                    self.duration_var.set(rapid_test_config.get("duration", "0"))
                    logging.info("已加载高频按键测试配置")
                    
        except Exception as e:
            messagebox.showerror("错误", f"加载配置失败: {str(e)}")
            logging.error(f"加载配置失败: {str(e)}")
    
    def initialize_driver(self):
        """初始化驱动"""
        try:
            dll_path = self.dll_path_var.get()
            sys_path = self.sys_path_var.get()
            
            if not dll_path or not sys_path:
                messagebox.showerror("错误", "请先设置DLL和SYS文件路径！")
                return False
            
            self.driver_mgr = DriverManager()
            self.driver_mgr._setup_paths(dll_path=dll_path, sys_path=sys_path)
            
            if self.driver_mgr.initialize():
                self.input_tester = InputTester(self.driver_mgr.get_driver())
                messagebox.showinfo("成功", "驱动初始化成功！")
                return True
            else:
                messagebox.showerror("错误", "驱动初始化失败！")
                return False
                
        except Exception as e:
            messagebox.showerror("错误", f"驱动初始化失败: {str(e)}")
            return False
    
    def run(self):
        """运行GUI"""
        self.root.mainloop()
        
        # 确保程序退出时清理资源
        if self.driver_mgr:
            self.driver_mgr.cleanup()
    
    def _toggle_rapid_test(self):
        """切换高频按键测试状态"""
        if not hasattr(self, 'rapid_test_running'):
            self.rapid_test_running = False
            
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
                
                # if press_time <= 0 or interval_time <= 0:
                #     messagebox.showerror("错误", "时间间隔必须大于0！")
                #     return
                
                # 检查按键是否有效
                if not VirtualKeys.is_valid_key(test_key):
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
                
                # 启动测试线程
                self.rapid_test_thread = threading.Thread(
                    target=self._run_rapid_test,
                    args=(test_key, press_time/1000.0, interval_time/1000.0),
                    daemon=True
                )
                self.rapid_test_thread.start()
                
            except ValueError:
                messagebox.showerror("错误", "请输入有效的时间间隔！")
                return
        else:
            # 停止测试
            self.rapid_test_running = False
            self.rapid_test_btn.config(text="开始测试")
            self.rapid_test_status.config(text="状态: 已停止")
    
    def _run_rapid_test(self, key, press_time, interval_time):
        """运行高频按键测试
        
        Args:
            key (str): 要测试的按键
            press_time (float): 按键按下时间（秒）
            interval_time (float): 按键间隔时间（秒）
        """
        try:
            if not self.input_tester:
                raise RuntimeError("输入测试器未初始化")
            
            # 获取运行时长设置
            try:
                duration = float(self.duration_var.get())
            except ValueError:
                duration = 0  # 默认无限运行
            
            # 初始化计时和计数
            start_time = time.time()
            last_update_time = start_time
            
            while self.rapid_test_running and self.is_driver_loaded:
                current_time = time.time()
                elapsed_time = current_time - start_time
                
                # 检查是否达到运行时长
                if duration > 0 and elapsed_time >= duration:
                    self.root.after(0, self._stop_test)
                    break
                
                # 每秒更新一次状态
                if current_time - last_update_time >= 1.0:
                    # 计算并更新频率
                    rate = self.press_count / elapsed_time if elapsed_time > 0 else 0
                    self.root.after(0, self._update_status, elapsed_time, rate)
                    last_update_time = current_time
                
                # 检查驱动状态
                if not self.input_tester._check_device_status():
                    raise RuntimeError("驱动状态异常")
                
                # 按下按键
                if not self.input_tester.key_down(key):
                    raise RuntimeError("按键按下失败")
                
                # 等待按下时间
                time.sleep(press_time)
                
                # 释放按键
                if not self.input_tester.key_up(key):
                    raise RuntimeError("按键释放失败")
                
                # 更新计数
                self.press_count += 1
                self.root.after(0, self._update_press_count)
                
                # 等待间隔时间
                time.sleep(interval_time)
                
        except Exception as e:
            self.rapid_test_running = False
            error_msg = f"高频按键测试出错: {str(e)}"
            logging.error(error_msg)
            self.root.after(0, lambda: messagebox.showerror("错误", error_msg))
            self.root.after(0, lambda: self.rapid_test_btn.config(text="开始测试"))
            self.root.after(0, lambda: self.rapid_test_status.config(text="状态: 出错"))
    
    def _update_status(self, elapsed_time, rate):
        """更新状态显示
        
        Args:
            elapsed_time (float): 运行时间（秒）
            rate (float): 点击频率（次/秒）
        """
        self.run_time_label.config(text=f"运行时间: {int(elapsed_time)}秒")
        self.click_rate_label.config(text=f"平均频率: {rate:.1f}次/秒")
    
    def _stop_test(self):
        """停止测试"""
        self.rapid_test_running = False
        self.rapid_test_btn.config(text="开始测试")
        self.rapid_test_status.config(text="状态: 已完成")
    
    def _update_press_count(self):
        """更新按键计数显示"""
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

    def _init_dpi_awareness(self):
        """初始化DPI感知设置"""
        try:
            # 尝试使用Per-Monitor DPI感知
            awareness = ctypes.c_int()
            errorCode = ctypes.windll.shcore.GetProcessDpiAwareness(0, ctypes.byref(awareness))
            
            # 如果当前不是Per-Monitor DPI感知，则设置
            if errorCode == 0 and awareness.value != 2:  # 2 = PROCESS_PER_MONITOR_DPI_AWARE
                ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception as e:
            logging.warning(f"设置DPI感知失败: {str(e)}")

    def _get_dpi_scale(self):
        """获取当前DPI缩放比例"""
        try:
            user32 = ctypes.windll.user32
            user32.SetProcessDPIAware()
            dpi = user32.GetDpiForSystem()  # 获取系统DPI
            return dpi / 96.0  # 96 DPI是基准值
        except Exception as e:
            logging.warning(f"获取DPI缩放失败: {str(e)}")
            return 1.0  # 默认返回1.0表示100%缩放

    def _get_screen_metrics(self):
        """获取屏幕信息（物理像素）"""
        import ctypes
        user32 = ctypes.windll.user32
        
        # 获取虚拟屏幕的物理尺寸
        x = user32.GetSystemMetrics(76)  # SM_XVIRTUALSCREEN
        y = user32.GetSystemMetrics(77)  # SM_YVIRTUALSCREEN
        width = user32.GetSystemMetrics(78)  # SM_CXVIRTUALSCREEN
        height = user32.GetSystemMetrics(79)  # SM_CYVIRTUALSCREEN
        
        # 添加DPI信息到日志
        logging.info(f"当前DPI缩放: {self.dpi_scale * 100}%")
        logging.info(f"虚拟屏幕范围（物理像素）: x={x}, y={y}, width={width}, height={height}")
        
        return x, y, width, height

    def _get_logical_coordinates(self, physical_x, physical_y):
        """将物理坐标转换为逻辑坐标（用于显示）"""
        return int(physical_x / self.dpi_scale), int(physical_y / self.dpi_scale)

    def _move_to_position(self):
        """移动到指定坐标（使用物理像素）"""
        try:
            # 输入的是逻辑坐标
            logical_x = int(self.x_coord_var.get())
            logical_y = int(self.y_coord_var.get())
            
            # 转换为物理坐标
            physical_x = int(logical_x * self.dpi_scale)
            physical_y = int(logical_y * self.dpi_scale)
            
            def move():
                try:
                    self.move_status_label.config(text="移动状态: 移动中")
                    success = self.input_tester.mouse_move_abs(physical_x, physical_y)
                    status = "成功" if success else "失败"
                    self.move_status_label.config(text=f"移动状态: 移动{status}")
                    self.mouse_pos_label.config(text=f"当前位置: ({logical_x}, {logical_y}) [物理: ({physical_x}, {physical_y})]")
                    logging.info(f"鼠标移动 - 逻辑坐标: ({logical_x}, {logical_y}), 物理坐标: ({physical_x}, {physical_y})")
                except Exception as e:
                    self.move_status_label.config(text=f"移动状态: 错误 - {str(e)}")
                    logging.error(f"鼠标移动出错: {str(e)}")
            
            threading.Thread(target=move, daemon=True).start()
            
        except ValueError:
            messagebox.showerror("错误", "请输入有效的坐标值")

    def _move_top_left(self):
        """移动到左上角"""
        x, y, _, _ = self._get_screen_metrics()
        logical_x, logical_y = self._get_logical_coordinates(x, y)
        self.x_coord_var.set(str(logical_x))
        self.y_coord_var.set(str(logical_y))
        self._move_to_position()

    def _move_top_right(self):
        """移动到右上角"""
        x, y, width, _ = self._get_screen_metrics()
        logical_x, logical_y = self._get_logical_coordinates(x + width - 1, y)
        self.x_coord_var.set(str(logical_x))
        self.y_coord_var.set(str(logical_y))
        self._move_to_position()

    def _move_bottom_left(self):
        """移动到左下角"""
        x, y, _, height = self._get_screen_metrics()
        logical_x, logical_y = self._get_logical_coordinates(x, y + height - 1)
        self.x_coord_var.set(str(logical_x))
        self.y_coord_var.set(str(logical_y))
        self._move_to_position()

    def _move_bottom_right(self):
        """移动到右下角"""
        x, y, width, height = self._get_screen_metrics()
        logical_x, logical_y = self._get_logical_coordinates(x + width - 1, y + height - 1)
        self.x_coord_var.set(str(logical_x))
        self.y_coord_var.set(str(logical_y))
        self._move_to_position()

    def _move_center(self):
        """移动到屏幕中心"""
        x, y, width, height = self._get_screen_metrics()
        logical_x, logical_y = self._get_logical_coordinates(x + width // 2, y + height // 2)
        self.x_coord_var.set(str(logical_x))
        self.y_coord_var.set(str(logical_y))
        self._move_to_position()

    def _auto_test_absolute_move(self):
        """自动测试鼠标绝对移动"""
        def run_test():
            try:
                x, y, width, height = self._get_screen_metrics()
                test_points = [
                    (x, y, "左上角"),
                    (x + width - 1, y, "右上角"),
                    (x, y + height - 1, "左下角"),
                    (x + width - 1, y + height - 1, "右下角"),
                    (x + width // 2, y + height // 2, "中心"),
                    # 添加边界测试点
                    (x + 1, y + 1, "接近左上角"),
                    (x + width - 2, y + 1, "接近右上角"),
                    (x + 1, y + height - 2, "接近左下角"),
                    (x + width - 2, y + height - 2, "接近右下角"),
                    # 添加四等分点
                    (x + width // 4, y + height // 4, "左上四分点"),
                    (x + width * 3 // 4, y + height // 4, "右上四分点"),
                    (x + width // 4, y + height * 3 // 4, "左下四分点"),
                    (x + width * 3 // 4, y + height * 3 // 4, "右下四分点")
                ]
                
                for physical_x, physical_y, position_name in test_points:
                    if not self.is_driver_loaded:
                        break
                        
                    logical_x, logical_y = self._get_logical_coordinates(physical_x, physical_y)
                    logging.info(f"测试移动到{position_name}: 逻辑({logical_x}, {logical_y}), 物理({physical_x}, {physical_y})")
                    self.x_coord_var.set(str(logical_x))
                    self.y_coord_var.set(str(logical_y))
                    self._move_to_position()
                    time.sleep(1)  # 等待移动完成
                
                logging.info("自动测试完成")
                self.move_status_label.config(text="移动状态: 测试完成")
                
            except Exception as e:
                logging.error(f"自动测试出错: {str(e)}")
                self.move_status_label.config(text=f"移动状态: 测试出错")
        
        threading.Thread(target=run_test, daemon=True).start()

if __name__ == '__main__':
    try:
        app = LYKeysGUI()
        app.run()
    except PermissionError:
        # 权限相关的错误已经在check_privileges中处理
        pass
    except Exception as e:
        # 其他未预期的错误
        messagebox.showerror("错误", f"程序运行出错: {str(e)}") 