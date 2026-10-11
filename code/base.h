#if !defined(BASE_H)
/*
  NOTE(yigit): The few things every file wants - fixed-size types and a
  handful of macros.  Included first, by everything.
*/

#include <stdint.h>
#include <stddef.h>

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef float    real32;
typedef double   real64;

typedef int8 i8;
typedef int16 i16;
typedef int32 i32;
typedef int64 i64;
typedef uint8 u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;

typedef real32 r32;
typedef real64 r64;
// A 32-bit truth value.  Same size as the Win32 BOOL, so they convert freely.
typedef int32    bool32;

// A size or an offset in bytes - 64 bits on a 64-bit build.
typedef size_t   memory_index;

// "static" means three different things in C; these say which one is meant.
#define internal        static      // a function only this file can see
#define global_variable static      // a variable only this file can see
#define local_persist   static      // a local that keeps its value between calls

#define Pi32 3.14159265359f

#define Kilobytes(Value) ((Value)*1024LL)
#define Megabytes(Value) (Kilobytes(Value)*1024LL)
#define Gigabytes(Value) (Megabytes(Value)*1024LL)
#define Terabytes(Value) (Gigabytes(Value)*1024LL)

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))

// Stops in the debugger, on the failing line, when Expression is false.
// Compiled out entirely when CLASH_DEBUG is 0.
#if CLASH_DEBUG
#define Assert(Expression) do { if(!(Expression)) { __debugbreak(); } } while(0)
#else
#define Assert(Expression)
#endif

// For a branch that should be unreachable.
#define InvalidCodePath Assert(!"InvalidCodePath")
#define InvalidDefaultCase default: {InvalidCodePath;} break

inline uint32
SafeTruncateUInt64(uint64 Value)
{
    Assert(Value <= 0xFFFFFFFF);
    uint32 Result = (uint32)Value;
    return(Result);
}

#define BASE_H
#endif
