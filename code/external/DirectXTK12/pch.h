// NOTE(yigit): Not part of DirectXTK12.  SimpleMath.cpp starts with
// #include "pch.h" - DirectXTK's precompiled header - and this stands in for
// it, so the file can be built exactly as Microsoft ships it.  It only needs
// RECT/LONG and std::min/max.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
