# -*- coding: utf-8 -*-

from virtuakeys_mapping import VirtualKeys
import logging
import time
from typing import Optional


class InputTester:
    """输入测试器类，用于测试键盘和鼠标输入"""
    
    def __init__(self, driver):
        """初始化输入测试器
        
        Args:
            driver: 驱动实例
        """
        self.driver = driver
        self.last_check_time = 0
        self.check_interval = 1.0  # 状态检查间隔(秒)
        self._vk_code_cache = {}  # 虚拟键码缓存
        self._last_status = True  # 上次状态检查结果
        self._status_check_count = 0  # 状态检查计数器
        self._status_check_threshold = 1000  # 状态检查阈值
        self._batch_enabled = True  # 启用批处理
    
    # ===== 状态检查相关方法 =====
    def _check_device_status(self) -> bool:
        """检查驱动状态，使用计数器控制检查频率"""
        self._status_check_count += 1
        
        # 只在计数达到阈值时进行状态检查
        if self._status_check_count >= self._status_check_threshold:
            current_time = time.time()
            if current_time - self.last_check_time >= self.check_interval:
                try:
                    self.driver.CheckDeviceStatus()
                    self.last_check_time = current_time
                    driver_status = self.driver.GetDriverStatus()
                    self._last_status = (driver_status == 1)  # 1 = DEVICE_STATUS_READY
                except:
                    self._last_status = False
                
            self._status_check_count = 0
            
        return self._last_status
    
    def check_device_status(self) -> None:
        """检查驱动状态（供GUI调用）"""
        if not self.driver:
            return
        self.driver.CheckDeviceStatus()
    
    def get_driver_status(self) -> int:
        """获取驱动状态（供GUI调用）"""
        if not self.driver:
            return 0
        return self.driver.GetDriverStatus()
    
    def get_last_check_time(self) -> int:
        """获取上次检查时间（供GUI调用）"""
        if not self.driver:
            return 0
        return self.driver.GetLastCheckTime()
    
    def _get_vk_code(self, key: str) -> Optional[int]:
        """获取虚拟键码，使用缓存优化性能"""
        if key not in self._vk_code_cache:
            self._vk_code_cache[key] = VirtualKeys.get_code(key)
        return self._vk_code_cache[key]
    
    # ===== 键盘操作相关方法 =====
    def _process_key_batch(self) -> bool:
        """处理按键批次"""
        try:
            return bool(self.driver.ProcessKeyBatch())
        except:
            return False
            
    def _add_key_to_batch(self, key: str, is_down: bool) -> bool:
        """添加按键到批处理
        
        Args:
            key: 按键名称
            is_down: 是否是按下操作
            
        Returns:
            bool: 是否成功添加到批处理
        """
        vk_code = self._get_vk_code(key)
        if vk_code is None:
            return False
            
        try:
            flags = 0 if is_down else 1  # KEY_DOWN = 0, KEY_UP = 1
            return bool(self.driver.AddKeyToBatch(vk_code, flags))
        except:
            return False
            
    def _clear_key_batch(self):
        """清空按键批处理缓冲区"""
        try:
            self.driver.ClearKeyBatch()
        except:
            pass
            
    def key_down(self, key: str) -> bool:
        """按键按下，支持批处理
        
        Args:
            key: 按键名称
            
        Returns:
            bool: 操作是否成功
        """
        if not self._batch_enabled:
            # 如果未启用批处理，使用原始方式
            vk_code = self._get_vk_code(key)
            if vk_code is None:
                return False
            try:
                self.driver.KeyDown(vk_code)
                return True
            except:
                return False
                
        # 使用批处理
        return self._add_key_to_batch(key, True)
        
    def key_up(self, key: str) -> bool:
        """按键释放，支持批处理
        
        Args:
            key: 按键名称
            
        Returns:
            bool: 操作是否成功
        """
        if not self._batch_enabled:
            # 如果未启用批处理，使用原始方式
            vk_code = self._get_vk_code(key)
            if vk_code is None:
                return False
            try:
                self.driver.KeyUp(vk_code)
                return True
            except:
                return False
                
        # 使用批处理
        return self._add_key_to_batch(key, False)
        
    def process_batch(self) -> bool:
        """处理当前的按键批次
        
        Returns:
            bool: 是否成功处理
        """
        if not self._batch_enabled:
            return True
        return self._process_key_batch()
        
    def clear_batch(self):
        """清空当前的按键批次"""
        if self._batch_enabled:
            self._clear_key_batch()
            
    def _process_special_symbol(self, symbol: str) -> bool:
        """处理特殊符号
        
        Args:
            symbol: 要处理的特殊符号
            
        Returns:
            bool: 操作是否成功
        """
        symbol_info = VirtualKeys.get_symbol_keys(symbol)
        if not symbol_info:
            return False
            
        base_key, need_shift = symbol_info
        
        if not self._batch_enabled:
            # 不使用批处理的情况
            if need_shift:
                if not self.key_down('left_shift'):
                    return False
            if not self.press_key(base_key):
                if need_shift:
                    self.key_up('left_shift')
                return False
            if need_shift:
                return self.key_up('left_shift')
            return True
            
        # 使用批处理的情况
        try:
            if need_shift:
                if not self._add_key_to_batch('left_shift', True):
                    return False
            if not self._add_key_to_batch(base_key, True):
                return False
            if not self._add_key_to_batch(base_key, False):
                return False
            if need_shift:
                if not self._add_key_to_batch('left_shift', False):
                    return False
            return self._process_key_batch()
        except:
            return False

    def press_key(self, key: str, duration: float = 0.1) -> bool:
        """按下并释放按键，支持批处理和特殊符号
        
        Args:
            key: 按键名称或特殊符号
            duration: 按下持续时间(秒)
            
        Returns:
            bool: 操作是否成功
        """
        # 检查是否是特殊符号
        if VirtualKeys.get_symbol_keys(key):
            return self._process_special_symbol(key)
            
        # 原有的按键处理逻辑
        if not self._batch_enabled:
            # 不使用批处理的情况
            if self.key_down(key):
                time.sleep(duration)
                return self.key_up(key)
            return False
            
        # 使用批处理的情况
        if self._add_key_to_batch(key, True):
            if self._process_key_batch():  # 处理按下操作
                time.sleep(duration)
                if self._add_key_to_batch(key, False):
                    return self._process_key_batch()  # 处理释放操作
        return False
        
    def press_keys(self, keys: list[str], duration: float = 0.1) -> bool:
        """批量按下并释放多个按键，支持特殊符号
        
        Args:
            keys: 按键名称或特殊符号列表
            duration: 每个按键的按下持续时间(秒)
            
        Returns:
            bool: 所有操作是否成功
        """
        if not self._batch_enabled:
            # 不使用批处理的情况
            success = True
            for key in keys:
                if not self.press_key(key, duration):
                    success = False
            return success
            
        # 使用批处理的情况
        try:
            # 处理所有按键
            for key in keys:
                # 检查是否是特殊符号
                symbol_info = VirtualKeys.get_symbol_keys(key)
                if symbol_info:
                    base_key, need_shift = symbol_info
                    if need_shift:
                        if not self._add_key_to_batch('shift', True):
                            return False
                    if not self._add_key_to_batch(base_key, True):
                        return False
                    if not self._add_key_to_batch(base_key, False):
                        return False
                    if need_shift:
                        if not self._add_key_to_batch('shift', False):
                            return False
                else:
                    # 普通按键处理
                    if not self._add_key_to_batch(key, True):
                        return False
                    if not self._add_key_to_batch(key, False):
                        return False
                        
            # 处理所有操作
            return self._process_key_batch()
        except:
            return False
    
    # ===== 鼠标操作相关方法 =====
    def mouse_move_rel(self, dx: int, dy: int) -> bool:
        """相对移动鼠标
        
        Args:
            dx: X轴相对移动距离
            dy: Y轴相对移动距离
            
        Returns:
            bool: 操作是否成功
        """
        self.driver.MouseMoveRELATIVE(dx, dy)
        return True
    
    def mouse_move_abs(self, x: int, y: int) -> bool:
        """绝对移动鼠标
        
        Args:
            x: 目标X坐标
            y: 目标Y坐标
            
        Returns:
            bool: 操作是否成功
        """
        self.driver.MouseMoveABSOLUTE(x, y)
        return True
    
    # 鼠标左键操作
    def mouse_left_down(self) -> bool:
        """鼠标左键按下"""
        self.driver.MouseLeftButtonDown()
        return True
    
    def mouse_left_up(self) -> bool:
        """鼠标左键释放"""
        self.driver.MouseLeftButtonUp()
        return True
    
    def mouse_click(self, duration: float = 0.1) -> bool:
        """鼠标左键点击"""
        if self.mouse_left_down():
            time.sleep(duration)
            return self.mouse_left_up()
        return False
    
    # 鼠标右键操作
    def mouse_right_down(self) -> bool:
        """鼠标右键按下"""
        self.driver.MouseRightButtonDown()
        return True
    
    def mouse_right_up(self) -> bool:
        """鼠标右键释放"""
        self.driver.MouseRightButtonUp()
        return True
    
    def mouse_right_click(self, duration: float = 0.1) -> bool:
        """鼠标右键点击"""
        if self.mouse_right_down():
            time.sleep(duration)
            return self.mouse_right_up()
        return False
    
    # 鼠标中键操作
    def mouse_middle_down(self) -> bool:
        """鼠标中键按下"""
        self.driver.MouseMiddleButtonDown()
        return True
    
    def mouse_middle_up(self) -> bool:
        """鼠标中键释放"""
        self.driver.MouseMiddleButtonUp()
        return True
    
    def mouse_middle_click(self, duration: float = 0.1) -> bool:
        """鼠标中键点击"""
        if self.mouse_middle_down():
            time.sleep(duration)
            return self.mouse_middle_up()
        return False
    
    # 鼠标X1键操作
    def mouse_x1_down(self) -> bool:
        """鼠标X1键按下"""
        self.driver.MouseXButton1Down()
        return True
    
    def mouse_x1_up(self) -> bool:
        """鼠标X1键释放"""
        self.driver.MouseXButton1Up()
        return True
    
    def mouse_x1_click(self, duration: float = 0.1) -> bool:
        """鼠标X1键点击"""
        if self.mouse_x1_down():
            time.sleep(duration)
            return self.mouse_x1_up()
        return False
    
    # 鼠标X2键操作
    def mouse_x2_down(self) -> bool:
        """鼠标X2键按下"""
        self.driver.MouseXButton2Down()
        return True
    
    def mouse_x2_up(self) -> bool:
        """鼠标X2键释放"""
        self.driver.MouseXButton2Up()
        return True
    
    def mouse_x2_click(self, duration: float = 0.1) -> bool:
        """鼠标X2键点击"""
        if self.mouse_x2_down():
            time.sleep(duration)
            return self.mouse_x2_up()
        return False
    
    # ===== 鼠标滚轮操作 =====
    def mouse_wheel_up(self, wheel_delta: int = 120) -> bool:
        """鼠标滚轮向上滚动
        
        Args:
            wheel_delta: 滚动量，默认为120（标准滚动单位）
            
        Returns:
            bool: 操作是否成功
        """
        try:
            self.driver.MouseWheelUp(wheel_delta)
            return True
        except Exception as e:
            logging.error(f"滚轮向上滚动失败: {str(e)}")
            return False
    
    def mouse_wheel_down(self, wheel_delta: int = 120) -> bool:
        """鼠标滚轮向下滚动
        
        Args:
            wheel_delta: 滚动量，默认为120（标准滚动单位）
            
        Returns:
            bool: 操作是否成功
        """
        try:
            self.driver.MouseWheelDown(wheel_delta)
            return True
        except Exception as e:
            logging.error(f"滚轮向下滚动失败: {str(e)}")
            return False
    
    def mouse_wheel(self, delta: int) -> bool:
        """鼠标滚轮滚动
        需要动态控制滚动方向时使用，可接受正负值，自动处理方向
        Args:
            delta: 滚动量，正值向上滚动，负值向下滚动
            
        Returns:
            bool: 操作是否成功
        """
        try:
            if delta > 0:
                return self.mouse_wheel_up(abs(delta))
            elif delta < 0:
                return self.mouse_wheel_down(abs(delta))
            return True
        except Exception as e:
            logging.error
            (f"滚轮滚动失败: {str(e)}")
            return False 