// Tiny logger: ring of recent lines for the on-screen UI, plus sdmc:/switch/loopback/log.txt.
#pragma once
#include <stddef.h>

#define LOG_RING_LINES 10
#define LOG_LINE_LEN   76

void        lgInit(void);
void        lgExit(void);
void        lg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const char* lgLine(int i);  // 0 = oldest of the ring; "" if empty
void        lgCopyLine(int i, char* out, size_t cap);  // thread-safe copy of line i; use this from another thread
unsigned    lgCount(void);  // total lines logged, for change detection
