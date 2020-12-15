#pragma once

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#ifdef DXCW_BUILD_DLL

#ifdef DXCW_DLL
#define DXCW_API __declspec(dllexport)
#else
#define DXCW_API __declspec(dllimport)
#endif

#else
#define DXCW_API
#endif

#else
#define DXCW_API
#endif
