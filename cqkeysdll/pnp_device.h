#pragma once
#include <windows.h>

struct CqDriverStartResult;

HANDLE CqOpenInterface();
BOOL CqInstallDevice(const char* path, CqDriverStartResult* diagnostics = nullptr);
BOOL CqChangeDevice(bool remove, bool removePackage = false);
BOOL CqAcquireSession();
BOOL CqReleaseSession();
