// Tests the update engine on a PC: pure helpers first, then a check and an install against the fake release server in
// tests/update_server.py (see run_update_test.sh).
//   update_test <running version> <workdir> [<check state> [<install state> [cancel]]]
// States are the UpdState names in lower case: idle current available downloading ready failed.
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../source/update.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static const char* stateName(UpdState s) {
    static const char* n[] = { "idle", "checking", "current", "available", "downloading", "ready", "failed" };
    return n[s];
}

static void* canceller(void* a) {
    (void)a;
    usleep(700 * 1000);
    updCancel();
    return NULL;
}

static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static int readAll(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    return (int)n;
}

static void pure(const char* dir) {
    CHECK(updVersionCmp("1.0.1", "1.0.0") == 1);
    CHECK(updVersionCmp("v1.0.0", "1.0.0") == 0);
    CHECK(updVersionCmp("1.0", "1.0.0") == 0);
    CHECK(updVersionCmp("1.9.0", "1.10.0") == -1);
    CHECK(updVersionCmp("v2.0.0", "1.99.99") == 1);
    CHECK(updVersionCmp("1.0.0-rc1", "1.0.0") == -2);
    CHECK(updVersionCmp("", "1.0.0") == -2);
    CHECK(updVersionCmp("1..0", "1.0.0") == -2);
    CHECK(updVersionCmp("1.2.3.4.5", "1.0.0") == -2);

    char tag[24];
    CHECK(updTagFromUrl("https://github.com/a/b/releases/tag/v1.2.3", tag, sizeof(tag)) && !strcmp(tag, "v1.2.3"));
    CHECK(updTagFromUrl("https://github.com/a/b/releases/tag/1.2.3?x=1", tag, sizeof(tag)) && !strcmp(tag, "1.2.3"));
    CHECK(!updTagFromUrl("https://github.com/a/b/releases", tag, sizeof(tag)));
    CHECK(!updTagFromUrl("https://github.com/a/b/releases/tag/nightly", tag, sizeof(tag)));
    CHECK(!updTagFromUrl("https://github.com/a/b/releases/tag/", tag, sizeof(tag)));
    CHECK(!updTagFromUrl("https://github.com/a/b/releases/tag/v1.2.3.4.5.6.7.8.9.10.11.12.13", tag, sizeof(tag)));

    char hex[65];
    const char* h = "5a2009b34bbdfc0dcdedd9d94bfe22f4cb7a17f5a1d5ec51a42cf2d9d6b35c61";
    char line[200];
    snprintf(line, sizeof(line), "%s *loopback.nro\n", h);
    CHECK(updParseSha(line, hex) && !strcmp(hex, h));
    snprintf(line, sizeof(line), "  %s\n", h);
    CHECK(updParseSha(line, hex));
    CHECK(!updParseSha("not a hash", hex));
    CHECK(!updParseSha("5a2009b34bbdfc0dcdedd9d94bfe22f4cb7a17f5a1d5ec51a42cf2d9d6b35c6", hex));   // 63 digits
    CHECK(!updParseSha("5a2009b34bbdfc0dcdedd9d94bfe22f4cb7a17f5a1d5ec51a42cf2d9d6b35c61f", hex)); // 65 digits

    char p[300];
    snprintf(p, sizeof(p), "%s/sha_abc", dir);
    put(p, "abc");
    CHECK(updSha256File(p, hex) && !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    snprintf(p, sizeof(p), "%s/sha_empty", dir);
    put(p, "");
    CHECK(updSha256File(p, hex) && !strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    // two blocks and the padding edge (56 bytes forces an extra block)
    snprintf(p, sizeof(p), "%s/sha_56", dir);
    put(p, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(updSha256File(p, hex) && !strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(!updSha256File("/nonexistent/x", hex));

    // swap: normal, then with no old file, then a failing second rename puts the old file back
    char t[300], target[300], bak[300], buf[64];
    snprintf(t, sizeof(t), "%s/sw.tmp", dir);
    snprintf(target, sizeof(target), "%s/sw.nro", dir);
    snprintf(bak, sizeof(bak), "%s/sw.bak", dir);
    put(target, "OLD"); put(t, "NEW"); put(bak, "STALE");
    CHECK(updSwapIn(t, target, bak) == NULL);
    readAll(target, buf, sizeof(buf)); CHECK(!strcmp(buf, "NEW"));
    readAll(bak, buf, sizeof(buf)); CHECK(!strcmp(buf, "OLD"));
    CHECK(access(t, F_OK) != 0);
    remove(target); remove(bak);
    put(t, "NEW");
    CHECK(updSwapIn(t, target, bak) == NULL);
    readAll(target, buf, sizeof(buf)); CHECK(!strcmp(buf, "NEW"));
    CHECK(access(bak, F_OK) != 0);
    snprintf(t, sizeof(t), "%s/missing.tmp", dir);
    remove(t);
    CHECK(updSwapIn(t, target, bak) != NULL);
    readAll(target, buf, sizeof(buf)); CHECK(!strcmp(buf, "NEW"));  // restored
    printf("  pure helpers %s\n", g_fail ? "FAILED" : "ok");
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s version workdir [check [install [cancel]]]\n", argv[0]); return 2; }
    const char* ver = argv[1];
    const char* dir = argv[2];
    mkdir(dir, 0777);
    pure(dir);
    if (argc < 4) return g_fail;

    char target[600], work[600], bak[700], tmp[700];
    snprintf(target, sizeof(target), "%s/loopback.nro", dir);
    snprintf(work, sizeof(work), "%s/app", dir);
    snprintf(bak, sizeof(bak), "%s/loopback.nro.bak", work);
    snprintf(tmp, sizeof(tmp), "%s/update.tmp", work);
    put(target, "OLD");
    updInit(ver, target, work);

    UpdStatus s;
    updCheckNow();
    updGet(&s);
    printf("  check: %s \"%s\" latest=%s\n", stateName(s.state), s.msg, s.latest);
    CHECK(!strcmp(stateName(s.state), argv[3]));
    if (argc >= 5) {
        if (argc >= 6 && !strcmp(argv[5], "cancel")) {  // the slow server takes seconds; stop it part way
            pthread_t th;
            pthread_create(&th, NULL, canceller, NULL);
            pthread_detach(th);
        }
        updInstallNow();
        updGet(&s);
        printf("  install: %s \"%s\"\n", stateName(s.state), s.msg);
        CHECK(!strcmp(stateName(s.state), argv[4]));
        char buf[64];
        if (s.state == UPD_READY) {
            struct stat st;  // replaced by the served NRO (see update_server.py), not the old text
            CHECK(stat(target, &st) == 0 && st.st_size == 0x14 + 2 * 1024 * 1024);
            FILE* f = fopen(target, "rb");
            unsigned char hdr[0x14] = { 0 };
            CHECK(f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && !memcmp(hdr + 0x10, "NRO0", 4));
            if (f) fclose(f);
            readAll(bak, buf, sizeof(buf)); CHECK(!strcmp(buf, "OLD"));
            CHECK(access(tmp, F_OK) != 0);
        } else {
            readAll(target, buf, sizeof(buf)); CHECK(!strcmp(buf, "OLD"));  // untouched
            CHECK(access(bak, F_OK) != 0);
            CHECK(access(tmp, F_OK) != 0);
        }
    }
    printf("  %s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
