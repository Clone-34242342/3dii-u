#pragma once
#include <cstdint>
using OSDynLoad_Module = uint32_t;
enum { OS_DYNLOAD_OK = 0, OS_DYNLOAD_EXPORT_FUNC = 0 };
int OSDynLoad_Acquire(const char *, OSDynLoad_Module *);
int OSDynLoad_FindExport(OSDynLoad_Module, int, const char *, void **);
void OSDynLoad_Release(OSDynLoad_Module);
