#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#define LOG_DIR  "sdmc:/switch/loopback"
#define LOG_PATH LOG_DIR "/log.txt"
#define LOG_MAX_FILE_LINES 4000  // stop writing after this so a chatty session can't fill the card

static char     g_ring[LOG_RING_LINES][LOG_LINE_LEN];
static unsigned g_count;
static FILE*    g_file;
static Mutex    g_mutex;  // lg() is called from both the USB thread and the UI thread

void lgInit(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir(LOG_DIR, 0777);
    g_file = fopen(LOG_PATH, "w");
}

void lgExit(void) {
    if (g_file) fclose(g_file);
    g_file = NULL;
}

void lg(const char* fmt, ...) {
    char line[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    mutexLock(&g_mutex);
    strcpy(g_ring[g_count % LOG_RING_LINES], line);
    if (g_file && g_count < LOG_MAX_FILE_LINES) {
        fprintf(g_file, "%s\n", line);
        fflush(g_file);
    }
    g_count++;
    mutexUnlock(&g_mutex);
}

const char* lgLine(int i) {
    unsigned n = g_count < LOG_RING_LINES ? g_count : LOG_RING_LINES;
    if ((unsigned)i >= n) return "";
    return g_ring[(g_count - n + i) % LOG_RING_LINES];
}

unsigned lgCount(void) { return g_count; }

void lgCopyLine(int i, char* out, size_t cap) {
    mutexLock(&g_mutex);
    const char* l = lgLine(i);
    size_t n = strlen(l);
    if (n >= cap) n = cap - 1;
    memcpy(out, l, n);
    out[n] = 0;
    mutexUnlock(&g_mutex);
}
