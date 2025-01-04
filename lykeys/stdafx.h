#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 避免PEPROCESS和PETHREAD重定义
#define _PO_INCLUDED_
#define _NTIFS_INCLUDED_

#include <ntddk.h>
#include <wdm.h>

#ifdef __cplusplus
}
#endif

#pragma warning(disable:4100)  // 未引用的参数
#pragma warning(disable:4201)  // 使用了非标准扩展
#pragma warning(disable:4057)  // 不同基类型的指针比较
#pragma warning(disable:4214)  // 使用了非标准扩展
