# -*- coding: utf-8 -*-

import os
import logging
from ctypes import CDLL, c_int, c_ulonglong
import json


class DriverManager:
    def __init__(self):
        self.driver = None
        self._setup_paths()
        self._setup_logging()
    
    def _setup_paths(self, dll_path=None, sys_path=None):
        """设置驱动路径
        
        Args:
            dll_path (str, optional): DLL文件的完整路径
            sys_path (str, optional): SYS文件的完整路径
        """
        # 尝试从配置文件加载路径
        config_file = "driver_config.json"
        if os.path.exists(config_file):
            try:
                with open(config_file, "r") as f:
                    config = json.load(f)
                    dll_path = dll_path or config.get("dll_path")
                    sys_path = sys_path or config.get("sys_path")
                logging.info("从配置文件加载驱动路径")
            except Exception as e:
                logging.warning(f"加载配置文件失败: {str(e)}")
        
        # 如果配置文件不存在或加载失败，使用默认路径
        if not dll_path or not sys_path:
            logging.info("使用默认驱动路径")
            base_dir = os.path.dirname(os.path.dirname(__file__))
            self.dll_path = dll_path or os.path.join(base_dir, 'lykeysdll', 'x64', 'Debug', 'lykeysdll.dll')
            self.sys_path = sys_path or os.path.join(base_dir, 'lykeys', 'x64', 'Debug', 'lykeys.sys')
        else:
            self.dll_path = dll_path
            self.sys_path = sys_path
            
        logging.info(f"DLL路径: {self.dll_path}")
        logging.info(f"SYS路径: {self.sys_path}")
        
        # 验证路径是否存在
        if not os.path.exists(self.dll_path):
            raise FileNotFoundError(f"找不到DLL文件: {self.dll_path}")
            
        if not os.path.exists(self.sys_path):
            raise FileNotFoundError(f"找不到SYS文件: {self.sys_path}")
    
    def _setup_logging(self):
        """设置日志"""
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s'
        )
    
    def initialize(self):
        """初始化驱动"""
        try:
            logging.info("开始加载驱动...")
            self.driver = CDLL(self.dll_path)
            
            # 设置返回类型
            self.driver.GetDriverStatus.restype = c_int
            self.driver.GetLastCheckTime.restype = c_ulonglong
            
            # 先尝试卸载已存在的驱动
            self._unload_driver()
            
            # 加载驱动
            if not self.driver.LoadNTDriver(b'lykeys', self.sys_path.encode()):
                raise RuntimeError("驱动加载失败")
            
            # time.sleep(1)  # 等待服务启动
            
            # 设置句柄
            self.driver.SetHandle()
            logging.info("驱动初始化完成")
            return True
            
        except Exception as e:
            logging.error(f"初始化失败: {str(e)}")
            self.cleanup()
            return False
    
    def _unload_driver(self):
        """卸载驱动"""
        try:
            if self.driver:
                self.driver.UnloadNTDriver(b'lykeys')
                
                # time.sleep(1)
                
        except Exception as e:
            logging.warning(f"卸载驱动时出现异常(可忽略): {str(e)}")
    
    def cleanup(self):
        """清理资源"""
        try:
            if self.driver:
                self._unload_driver()
                logging.info("驱动卸载成功")
        except Exception as e:
            logging.error(f"清理失败: {str(e)}")
    
    def get_driver(self):
        """获取驱动实例"""
        return self.driver 