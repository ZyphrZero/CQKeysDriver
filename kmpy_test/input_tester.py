# -*- coding: utf-8 -*-

from virtuakeys_mapping import VirtualKeys
import logging
import time


class InputTester:
    def __init__(self, driver):
        self.driver = driver
        self.last_check_time = 0
        self.check_interval = 1.0  # 状态检查间隔(秒)
        # self.min_key_interval = 0.001  # 最小按键间隔(秒)
    
    def _check_device_status(self):
        """检查驱动状态"""
        current_time = time.time()
        if current_time - self.last_check_time >= self.check_interval:
            self.driver.CheckDeviceStatus()
            self.last_check_time = current_time
            
            driver_status = self.driver.GetDriverStatus()
            
            if driver_status != 1:  # 1 = DEVICE_STATUS_READY
                logging.warning(f"驱动状态异常: {driver_status}")
                return False
        return True
    
    def key_down(self, key):
        """按键按下"""
        if not self._check_device_status():
            return False
            
        vk_code = VirtualKeys.get_code(key)
        if vk_code is None:
            logging.error(f"无效的按键: {key}")
            return False
        
        try:
            self.driver.KeyDown(vk_code)
            # time.sleep(self.min_key_interval)  # 添加最小延时
            return True
        except Exception as e:
            logging.error(f"按键按下失败: {str(e)}")
            return False
    
    def key_up(self, key):
        """按键释放"""
        if not self._check_device_status():  # 添加状态检查
            return False
            
        vk_code = VirtualKeys.get_code(key)
        if vk_code is None:
            logging.error(f"无效的按键: {key}")
            return False
        
        try:
            self.driver.KeyUp(vk_code)
            # time.sleep(self.min_key_interval)  # 添加最小延时
            return True
        except Exception as e:
            logging.error(f"按键释放失败: {str(e)}")
            return False
    
    def press_key(self, key, duration=0.1):
        """按下并释放按键"""
        if self.key_down(key):
            time.sleep(duration)
            return self.key_up(key)
        return False
    
    def mouse_left_down(self):
        """鼠标左键按下"""
        if not self._check_device_status():
            return False
        self.driver.MouseLeftButtonDown()
        return True
    
    def mouse_left_up(self):
        """鼠标左键释放"""
        if not self._check_device_status():
            return False
        self.driver.MouseLeftButtonUp()
        return True
    
    def mouse_click(self, duration=0.1):
        """鼠标左键点击"""
        if self.mouse_left_down():
            time.sleep(duration)
            return self.mouse_left_up()
        return False
    
    def mouse_move_rel(self, dx, dy):
        """相对移动鼠标"""
        if not self._check_device_status():
            return False
        self.driver.MouseMoveRELATIVE(dx, dy)
        return True
    
    def mouse_move_abs(self, x, y):
        """绝对移动鼠标"""
        if not self._check_device_status():
            return False
        self.driver.MouseMoveABSOLUTE(x, y)
        return True
    
    def check_device_status(self):
        """检查驱动状态"""
        if not self.driver:
            return
        # 调用DLL中的检查设备状态函数
        self.driver.CheckDeviceStatus()
    
    def get_driver_status(self):
        """获取驱动状态"""
        if not self.driver:
            return 0
        return self.driver.GetDriverStatus()
    
    def get_last_check_time(self):
        """获取上次检查时间"""
        if not self.driver:
            return 0
        return self.driver.GetLastCheckTime() 