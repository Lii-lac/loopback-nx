// Self-update from GitHub releases. See update.h for the overview.
#include "update.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include "log.h"
#else
#define lg(...) ((void)0)
#endif

#define MAX_NRO_BYTES (64u * 1024u * 1024u)  // the app is under 1 MiB; anything near this is not it
#define MAX_SHA_BYTES 1024u

// ---------------------------------------------------------------------------------------------
// Versions and tags

static bool parseVer(const char* s, unsigned v[4], int* n) {
    if (*s == 'v' || *s == 'V') s++;
    *n = 0;
    for (;;) {
        if (!isdigit((unsigned char)*s)) return false;
        unsigned long x = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            x = x * 10 + (unsigned long)(*s - '0');
            if (++digits > 6) return false;
            s++;
        }
        if (*n >= 4) return false;
        v[(*n)++] = (unsigned)x;
        if (*s == '.') { s++; continue; }
        return *s == 0;
    }
}

int updVersionCmp(const char* a, const char* b) {
    unsigned va[4] = { 0 }, vb[4] = { 0 };
    int na, nb;
    if (!parseVer(a, va, &na) || !parseVer(b, vb, &nb)) return -2;
    for (int i = 0; i < 4; i++) {
        unsigned x = i < na ? va[i] : 0, y = i < nb ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool updTagFromUrl(const char* url, char* tag, size_t cap) {
    const char* k = strstr(url, "/releases/tag/");
    if (!k) return false;
    k += strlen("/releases/tag/");
    size_t n = strcspn(k, "/?#");
    if (n == 0 || n >= cap) return false;
    memcpy(tag, k, n);
    tag[n] = 0;
    unsigned v[4];
    int c;
    return parseVer(tag, v, &c);
}

bool updParseSha(const char* text, char hex[65]) {
    while (*text && isspace((unsigned char)*text)) text++;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)text[i])) return false;
        hex[i] = (char)tolower((unsigned char)text[i]);
    }
    if (text[64] && !isspace((unsigned char)text[64]) && text[64] != '*') return false;
    hex[64] = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// SHA-256

typedef struct { uint32_t h[8]; uint64_t bytes; unsigned char buf[64]; size_t fill; } Sha;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void shaBlock(Sha* s, const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void shaInit(Sha* s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->bytes = 0;
    s->fill = 0;
}

static void shaUpdate(Sha* s, const void* data, size_t n) {
    const unsigned char* p = data;
    s->bytes += n;
    while (n) {
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take; p += take; n -= take;
        if (s->fill == 64) { shaBlock(s, s->buf); s->fill = 0; }
    }
}

static void shaHex(Sha* s, char hex[65]) {
    uint64_t bits = s->bytes * 8;
    unsigned char pad = 0x80;
    shaUpdate(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) shaUpdate(s, &pad, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    shaUpdate(s, len, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}

bool updSha256File(const char* path, char hex[65]) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    Sha s;
    shaInit(&s);
    static unsigned char buf[32768];  // one hash at a time (the worker is single)
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) shaUpdate(&s, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) shaHex(&s, hex);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Swapping the file in

const char* updSwapIn(const char* tmp, const char* target, const char* bak) {
    struct stat st;
    bool had_old = stat(target, &st) == 0;
    if (had_old) {
        remove(bak);
        if (rename(target, bak) != 0) return "Could not move the old version aside.";
    }
    if (rename(tmp, target) != 0) {
        if (had_old) rename(bak, target);
        return "Could not put the new version in place.";
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// State

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static UpdStatus   g_st;
static char        g_running[24], g_target[256], g_work[256], g_tag[24];
static atomic_int  g_cancel, g_busy;

static void lockSt(void) { while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) { } }
static void unlockSt(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static void setStatus(UpdState s, int pct, const char* msg) {
    lockSt();
    g_st.state = s;
    g_st.pct = pct;
    snprintf(g_st.msg, sizeof(g_st.msg), "%s", msg);
    unlockSt();
}

static void setPct(int pct) {
    lockSt();
    g_st.pct = pct;
    unlockSt();
}

void updGet(UpdStatus* out) {
    lockSt();
    *out = g_st;
    unlockSt();
}

bool updBusy(void) { return atomic_load(&g_busy) != 0; }
void updCancel(void) { atomic_store(&g_cancel, 1); }

void updInit(const char* running, const char* target, const char* work_dir) {
    snprintf(g_running, sizeof(g_running), "%s", running);
    snprintf(g_target, sizeof(g_target), "%s", target);
    snprintf(g_work, sizeof(g_work), "%s", work_dir);
    g_tag[0] = 0;
    memset(&g_st, 0, sizeof(g_st));
    g_st.state = UPD_IDLE;
    atomic_store(&g_cancel, 0);
    atomic_store(&g_busy, 0);
    mkdir(g_work, 0777);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s/update.tmp", g_work);
    remove(tmp);  // left over from a download that was cut short
}

// ---------------------------------------------------------------------------------------------
// Network

static const char ERR_CANCELLED[] = "Cancelled.";
static char       g_err[160];

static const char* fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return g_err;
}

static const char* curlFail(CURLcode rc) {
    if (rc == CURLE_ABORTED_BY_CALLBACK) return ERR_CANCELLED;
    return fail("Could not reach GitHub (%s).", curl_easy_strerror(rc));
}

static const char* netBegin(void) {
#ifdef __SWITCH__
    bool nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    if (nifm) {
        NifmInternetConnectionType type;
        u32 strength;
        NifmInternetConnectionStatus status;
        bool up = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) && status == NifmInternetConnectionStatus_Connected;
        if (!up) { nifmExit(); return "No internet connection. Connect to Wi-Fi in System Settings."; }
    }
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        if (nifm) nifmExit();
        return fail("Could not start networking (%08x).", rc);
    }
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return NULL;
}

static void netEnd(void) {
    curl_global_cleanup();
#ifdef __SWITCH__
    socketExit();
    nifmExit();
#endif
}

typedef struct { char buf[MAX_SHA_BYTES + 1]; size_t len; } Mem;
typedef struct { FILE* f; unsigned long long n; } Sink;

static size_t memWrite(void* p, size_t sz, size_t cnt, void* u) {
    Mem* m = u;
    size_t n = sz * cnt;
    if (m->len + n > MAX_SHA_BYTES) return 0;
    memcpy(m->buf + m->len, p, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

static size_t fileWrite(void* p, size_t sz, size_t cnt, void* u) {
    Sink* s = u;
    size_t n = sz * cnt;
    if (s->n + n > MAX_NRO_BYTES) return 0;
    if (fwrite(p, 1, n, s->f) != n) return 0;
    s->n += n;
    return n;
}

static int xfer(void* u, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)u; (void)ultotal; (void)ulnow;
    if (dltotal > 0) setPct((int)(dlnow * 100 / dltotal));
    return atomic_load(&g_cancel) ? 1 : 0;
}

static CURL* newEasy(const char* url) {
    CURL* h = curl_easy_init();
    if (!h) return NULL;
    char ua[64];
    snprintf(ua, sizeof(ua), "Loopback/%s", g_running);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // a download that crawls for 30 s is given up on
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
#ifndef UPD_ALLOW_HTTP
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfer);
    return h;
}

// Asks where /releases/latest points. NULL on success with the raw tag in `tag`.
static const char* latestTag(char* tag, size_t cap) {
    CURL* h = newEasy(UPD_REPO_URL "/releases/latest");
    if (!h) return "Could not start the download library.";
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    char* loc = NULL;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &loc);
    const char* err = NULL;
    if (rc != CURLE_OK) err = curlFail(rc);
    else if (code >= 300 && code < 400 && loc) {
        if (!updTagFromUrl(loc, tag, cap)) err = "No release has been published yet.";
    } else if (code == 404) err = "No release found. The repository may be private.";
    else err = fail("GitHub answered with code %ld.", code);
    curl_easy_cleanup(h);
    return err;
}

static const char* fetchMem(const char* url, Mem* m) {
    m->len = 0;
    m->buf[0] = 0;
    CURL* h = newEasy(url);
    if (!h) return "Could not start the download library.";
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, memWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, m);
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    if (rc == CURLE_HTTP_RETURNED_ERROR) return fail("GitHub answered with code %ld for the checksum.", code);
    return rc == CURLE_OK ? NULL : curlFail(rc);
}

static const char* fetchFile(const char* url, FILE* f) {
    Sink s = { f, 0 };
    CURL* h = newEasy(url);
    if (!h) return "Could not start the download library.";
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, fileWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &s);
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    if (rc == CURLE_HTTP_RETURNED_ERROR) return fail("GitHub answered with code %ld for the download.", code);
    return rc == CURLE_OK ? NULL : curlFail(rc);
}

// ---------------------------------------------------------------------------------------------
// Check and install

static bool claim(void) { return !atomic_exchange(&g_busy, 1); }
static void release(void) { atomic_store(&g_busy, 0); }

static const char* versionOf(const char* tag) { return (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag; }

static void checkBody(void) {
    atomic_store(&g_cancel, 0);
    setStatus(UPD_CHECKING, 0, "Checking for a newer version...");
    char tag[24] = "";
    const char* err = netBegin();
    if (!err) {
        err = latestTag(tag, sizeof(tag));
        netEnd();
    }
    if (err == ERR_CANCELLED) {
        setStatus(UPD_IDLE, 0, ERR_CANCELLED);
    } else if (err) {
        lg("update check failed: %s", err);
        setStatus(UPD_FAILED, 0, err);
    } else {
        const char* ver = versionOf(tag);
        int cmp = updVersionCmp(ver, g_running);
        lockSt();
        snprintf(g_st.latest, sizeof(g_st.latest), "%s", ver);
        snprintf(g_tag, sizeof(g_tag), "%s", tag);
        unlockSt();
        lg("update check: latest %s, running %s", ver, g_running);
        char msg[120];
        if (cmp == 1 || cmp == -2) {
            snprintf(msg, sizeof(msg), "Version %s is available.", ver);
            setStatus(UPD_AVAILABLE, 0, msg);
        } else {
            setStatus(UPD_CURRENT, 0, cmp == 0 ? "You have the latest version." : "This build is newer than the latest release.");
        }
    }
}

static void installBody(void) {
    atomic_store(&g_cancel, 0);
    char tag[24], ver[24], msg[120], url[300], tmp[300], bak[300];
    lockSt();
    snprintf(tag, sizeof(tag), "%s", g_tag);
    unlockSt();
    snprintf(ver, sizeof(ver), "%s", versionOf(tag));
    snprintf(tmp, sizeof(tmp), "%s/update.tmp", g_work);
    snprintf(bak, sizeof(bak), "%s/loopback.nro.bak", g_work);
    snprintf(msg, sizeof(msg), "Downloading version %s...", ver);
    setStatus(UPD_DOWNLOADING, 0, msg);

    const char* err = netBegin();
    char want[65] = "", got[65] = "";
    if (!err) {
        static Mem m;
        snprintf(url, sizeof(url), "%s/releases/download/%s/" UPD_ASSET ".sha256", UPD_REPO_URL, tag);
        err = fetchMem(url, &m);
        if (!err && !updParseSha(m.buf, want)) err = "The release has no valid checksum file.";
        if (!err) {
            FILE* f = fopen(tmp, "wb");
            if (!f) err = "Could not write to the SD card.";
            else {
                snprintf(url, sizeof(url), "%s/releases/download/%s/" UPD_ASSET, UPD_REPO_URL, tag);
                err = fetchFile(url, f);
                if (fclose(f) != 0 && !err) err = "Could not write to the SD card.";
            }
        }
        netEnd();
    }
    if (!err) {
        // the file must be an NRO and match the published checksum before anything on the card is touched
        FILE* f = fopen(tmp, "rb");
        unsigned char hdr[0x14] = { 0 };
        bool nro = f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && !memcmp(hdr + 0x10, "NRO0", 4);
        if (f) fclose(f);
        if (!nro) err = "The download is not a Switch app. Nothing was changed.";
        else if (!updSha256File(tmp, got)) err = "Could not read the download back.";
        else if (strcmp(got, want) != 0) err = "The download did not match its checksum. Nothing was changed.";
    }
    if (!err) err = updSwapIn(tmp, g_target, bak);

    if (err == ERR_CANCELLED) {
        remove(tmp);
        setStatus(UPD_AVAILABLE, 0, ERR_CANCELLED);
    } else if (err) {
        remove(tmp);
        lg("update failed: %s", err);
        lockSt();  // the version found by the check still stands, so Install can be tried again
        g_st.state = UPD_FAILED;
        g_st.pct = 0;
        snprintf(g_st.msg, sizeof(g_st.msg), "%s", err);
        unlockSt();
    } else {
        lg("updated to %s (old version kept in %s)", ver, bak);
        snprintf(msg, sizeof(msg), "Updated to version %s. Restart Loopback to use it.", ver);
        setStatus(UPD_READY, 100, msg);
    }
}

void updCheckNow(void) {
    if (!claim()) return;
    checkBody();
    release();
}

void updInstallNow(void) {
    if (!claim()) return;
    if (!g_tag[0]) { release(); return; }
    installBody();
    release();
}

// ---------------------------------------------------------------------------------------------
// The worker thread (console only)

#ifdef __SWITCH__
static Thread g_th;
static bool   g_th_on;

static void worker(void* arg) {
    if ((intptr_t)arg == 0) checkBody();
    else installBody();
    release();
}

static void spawn(int what) {
    if (g_th_on) { threadWaitForExit(&g_th); threadClose(&g_th); g_th_on = false; }  // the previous run has finished
    if (R_SUCCEEDED(threadCreate(&g_th, worker, (void*)(intptr_t)what, NULL, 0x40000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&g_th))) {
        g_th_on = true;
    } else {
        lg("could not start the update thread");
        setStatus(UPD_FAILED, 0, "Could not start the update.");
        release();
    }
}

void updCheckAsync(void) {
    if (!claim()) return;
    setStatus(UPD_CHECKING, 0, "Checking for a newer version...");
    spawn(0);
}

void updInstallAsync(void) {
    if (!claim()) return;
    UpdStatus s;
    updGet(&s);
    if (!g_tag[0] || s.state == UPD_READY) { release(); return; }
    setStatus(UPD_DOWNLOADING, 0, "Downloading...");
    spawn(1);
}

void updShutdown(void) {
    updCancel();
    if (g_th_on) { threadWaitForExit(&g_th); threadClose(&g_th); g_th_on = false; }
}
#endif
