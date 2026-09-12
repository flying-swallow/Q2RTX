#pragma once

/* The staged SDK's host sources assume the Windows compiler compatibility
 * layer. Keep these definitions local to the Q2RTX FSR3 target. */
#ifndef _WIN32
#include <stddef.h>
#include <wchar.h>

#ifndef __declspec
#define __declspec(x)
#endif
#ifndef _countof
#define _countof(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef wcscpy_s
#define wcscpy_s(destination, source) wcscpy((destination), (source))
#endif
#endif
