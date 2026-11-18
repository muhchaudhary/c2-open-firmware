/* Host build: the SDK's typedef.h, as far as the firmware uses it. */
#ifndef TYPEDEF_H
#define TYPEDEF_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

#define BIT(n)              (1u << (n))
#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif
#define LOBYTE(x)           ((u8)((x) & 0xff))
#define HIBYTE(x)           ((u8)(((x) >> 8) & 0xff))
#endif
