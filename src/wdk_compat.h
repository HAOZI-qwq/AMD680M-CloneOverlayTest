#pragma once

// d3dkmthk.h is a WDK-facing header. On some desktop Windows SDK / MSVC
// combinations, including it after WIN32_LEAN_AND_MEAN leaves NT native
// types unavailable. Force the normal Win32 + NT declarations in first.
#include <windows.h>
#include <winternl.h>
