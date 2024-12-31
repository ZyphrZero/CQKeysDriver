import os
import sys
import pefile
import binascii
import struct
import hashlib
from math import log2
from typing import List, Dict, Tuple

class DllProtectionChecker:
    def __init__(self, dll_path: str):
        """初始化DLL保护检查器"""
        self.dll_path = dll_path
        self.pe = pefile.PE(dll_path)
        self.protection_score = 0
        self.max_score = 0
        self.findings = []
        
    def check_all(self) -> Tuple[float, List[str]]:
        """运行所有保护检查"""
        # 检查基本PE保护
        self._check_aslr()
        self._check_dep()
        self._check_seh()
        self._check_cfg()
        
        # 检查导入表保护
        self._check_imports_protection()
        
        # 检查字符串加密
        self._check_string_encryption()
        
        # 检查代码混淆
        self._check_code_obfuscation()
        
        # 检查反调试保护
        self._check_anti_debug()
        
        # 计算总分
        final_score = (self.protection_score / self.max_score * 100) if self.max_score > 0 else 0
        return final_score, self.findings
    
    def _add_finding(self, message: str, score: int, max_score: int):
        """添加发现结果"""
        self.findings.append(message)
        self.protection_score += score
        self.max_score += max_score
    
    def _check_aslr(self):
        """检查ASLR保护"""
        if self.pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040:
            self._add_finding("✓ ASLR保护已启用", 1, 1)
        else:
            self._add_finding("✗ ASLR保护未启用", 0, 1)
    
    def _check_dep(self):
        """检查DEP保护"""
        if self.pe.OPTIONAL_HEADER.DllCharacteristics & 0x0100:
            self._add_finding("✓ DEP保护已启用", 1, 1)
        else:
            self._add_finding("✗ DEP保护未启用", 0, 1)
    
    def _check_seh(self):
        """检查SEH保护"""
        if self.pe.OPTIONAL_HEADER.DllCharacteristics & 0x0400:
            self._add_finding("✓ SafeSEH已启用", 1, 1)
        else:
            self._add_finding("✗ SafeSEH未启用", 0, 1)
    
    def _check_cfg(self):
        """检查CFG保护"""
        if self.pe.OPTIONAL_HEADER.DllCharacteristics & 0x4000:
            self._add_finding("✓ CFG保护已启用", 1, 1)
        else:
            self._add_finding("✗ CFG保护未启用", 0, 1)
    
    def _check_imports_protection(self):
        """检查导入表保护"""
        suspicious_imports = [
            "IsDebuggerPresent",
            "CheckRemoteDebuggerPresent",
            "OutputDebugString",
            "NtQueryInformationProcess"
        ]
        
        found_anti_debug = False
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                if imp.name and any(s.encode() in imp.name for s in suspicious_imports):
                    found_anti_debug = True
                    break
        
        if found_anti_debug:
            self._add_finding("✓ 检测到反调试导入", 1, 1)
        else:
            self._add_finding("✗ 未检测到反调试导入", 0, 1)
    
    def _check_string_encryption(self):
        """检查字符串加密"""
        encrypted_strings = 0
        total_strings = 0
        
        # 检查所有节
        for section in self.pe.sections:
            if section.Name.startswith(b'.rdata') or section.Name.startswith(b'.data'):
                data = section.get_data()
                strings = self._extract_strings(data)
                total_strings += len(strings)
                
                # 检查每个字符串是否可能被加密
                for s in strings:
                    if self._looks_encrypted(s):
                        encrypted_strings += 1
        
        if total_strings > 0:
            encryption_ratio = encrypted_strings / total_strings
            if encryption_ratio > 0.5:
                self._add_finding(f"✓ 检测到字符串加密 ({encrypted_strings}/{total_strings})", 2, 2)
            else:
                self._add_finding(f"✗ 字符串加密不足 ({encrypted_strings}/{total_strings})", 0, 2)
        else:
            self._add_finding("- 未找到可分析的字符串", 0, 2)
    
    def _check_code_obfuscation(self):
        """检查代码混淆"""
        code_section = None
        for section in self.pe.sections:
            if section.Name.startswith(b'.text'):
                code_section = section
                break
        
        if code_section:
            code_data = code_section.get_data()
            entropy = self._calculate_entropy(code_data)
            
            if entropy > 7.0:
                self._add_finding(f"✓ 检测到代码混淆 (熵值: {entropy:.2f})", 2, 2)
            elif entropy > 6.0:
                self._add_finding(f"~ 可能存在部分代码混淆 (熵值: {entropy:.2f})", 1, 2)
            else:
                self._add_finding(f"✗ 未检测到代码混淆 (熵值: {entropy:.2f})", 0, 2)
        else:
            self._add_finding("- 未找到代码段", 0, 2)
    
    def _check_anti_debug(self):
        """检查反调试保护"""
        anti_debug_score = 0
        max_anti_debug_score = 4
        
        # 检查PEB访问
        if self._has_peb_access():
            anti_debug_score += 1
        
        # 检查时间检测
        if self._has_timing_check():
            anti_debug_score += 1
        
        # 检查异常处理
        if self._has_exception_handlers():
            anti_debug_score += 1
        
        # 检查调试API使用
        if self._has_debug_apis():
            anti_debug_score += 1
        
        self._add_finding(
            f"反调试保护评分: {anti_debug_score}/{max_anti_debug_score}",
            anti_debug_score,
            max_anti_debug_score
        )
    
    def _extract_strings(self, data: bytes, min_length: int = 4) -> List[bytes]:
        """提取数据中的字符串"""
        strings = []
        current_string = []
        
        for byte in data:
            if 32 <= byte <= 126:  # 可打印ASCII字符
                current_string.append(byte)
            else:
                if len(current_string) >= min_length:
                    strings.append(bytes(current_string))
                current_string = []
        
        if len(current_string) >= min_length:
            strings.append(bytes(current_string))
        
        return strings
    
    def _looks_encrypted(self, data: bytes) -> bool:
        """判断数据是否看起来像加密的"""
        # 计算数据的熵
        entropy = self._calculate_entropy(data)
        # 熵值高于6.5通常表示数据被加密或压缩
        return entropy > 6.5
    
    def _calculate_entropy(self, data: bytes) -> float:
        """计算Shannon熵"""
        if not data:
            return 0.0
            
        occurences = [0] * 256
        for byte in data:
            occurences[byte] += 1
            
        entropy = 0
        for count in occurences:
            if count:
                probability = count / len(data)
                entropy -= probability * (log2(probability) if probability > 0 else 0)
                
        return entropy
    
    def _has_peb_access(self) -> bool:
        """检查是否存在PEB访问"""
        # 简单检查是否有fs:[30h]访问模式
        code_section = next((s for s in self.pe.sections if s.Name.startswith(b'.text')), None)
        if code_section:
            code_data = code_section.get_data()
            return b'\x64\xA1\x30\x00\x00\x00' in code_data  # 32位PEB访问特征
        return False
    
    def _has_timing_check(self) -> bool:
        """检查是否存在时间检测"""
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                if imp.name and any(api in imp.name for api in [
                    b"QueryPerformanceCounter",
                    b"GetTickCount",
                    b"GetSystemTime"
                ]):
                    return True
        return False
    
    def _has_exception_handlers(self) -> bool:
        """检查是否存在异常处理"""
        return any(section.Name.startswith(b'.pdata') for section in self.pe.sections)
    
    def _has_debug_apis(self) -> bool:
        """检查是否使用调试相关API"""
        debug_apis = [
            b"IsDebuggerPresent",
            b"CheckRemoteDebuggerPresent",
            b"OutputDebugString",
            b"NtQueryInformationProcess"
        ]
        
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                if imp.name and any(api in imp.name for api in debug_apis):
                    return True
        return False

def main():
    if len(sys.argv) != 2:
        print("使用方法: python dll_protection_checker.py <dll_path>")
        sys.exit(1)
    
    dll_path = sys.argv[1]
    if not os.path.exists(dll_path):
        print(f"错误: 文件 {dll_path} 不存在")
        sys.exit(1)
    
    try:
        checker = DllProtectionChecker(dll_path)
        score, findings = checker.check_all()
        
        print("\n=== DLL保护检查报告 ===")
        print(f"文件: {dll_path}")
        print("\n发现:")
        for finding in findings:
            print(f"  {finding}")
        print(f"\n总体保护评分: {score:.1f}%")
        
        # 提供建议
        if score < 50:
            print("\n建议:")
            print("  - 启用ASLR、DEP和SafeSEH保护")
            print("  - 实现字符串加密")
            print("  - 添加代码混淆")
            print("  - 增强反调试保护")
        elif score < 80:
            print("\n建议:")
            print("  - 增强现有保护措施")
            print("  - 考虑添加更多的反调试技术")
            print("  - 改进代码混淆")
        else:
            print("\n✓ DLL保护措施较为完善")
        
    except Exception as e:
        print(f"错误: {str(e)}")
        sys.exit(1)

if __name__ == "__main__":
    main() 