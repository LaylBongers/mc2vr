// Case-sensitivity shim for MinGW cross builds (Linux filesystems).
// safetyhook.cpp does `#include <Windows.h>`, but mingw-w64 only ships
// lowercase `windows.h`. This directory is added to the include path
// before the system includes.
#pragma once
#include <windows.h>
