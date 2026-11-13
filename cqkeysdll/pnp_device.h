#pragma once
#include <windows.h>

struct CqDriverStartResult;

HANDLE CqOpenInterface();
BOOL CqInstallDevice(const char* path, CqDriverStartResult* diagnostics = nullptr);
BOOL CqChangeDevice(bool remove);
BOOL CqAcquireSession();
BOOL CqReleaseSession();
