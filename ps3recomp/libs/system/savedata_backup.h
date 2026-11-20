/*
 * ps3recomp - per-slot save backups (PC ports, shared by R1 / R2 / R3: keep the copies identical).
 *
 * Right before cellSaveData writes a save directory (savedata_execute with
 * is_save, cellSaveData.c), the directory's current files are copied to
 *     <game folder>\saves_backup\<slot>\<YYYYMMDD_HHMMSS>\   (UTC)
 * and only the newest PORT_SAVE_BACKUP_KEEP (default 5) are kept per slot.
 * A copy identical to the newest backup (same files, sizes and times, or same
 * bytes) is skipped, so loading and re-saving
 * an unchanged slot does not push older versions out.
 *
 *   PORT_SAVE_BACKUP=0         off
 *   PORT_SAVE_BACKUP_KEEP=N    backups per slot (1..100, default 5)
 *
 * It never blocks the game for long (save folders are a few hundred KB; a slot
 * over 64 MB is skipped) and never fails the save: any error is one
 * "[savebackup]" log line and the save goes ahead. Copies go to a "<stamp>.tmp"
 * folder that is renamed once complete, so a half-written backup is never
 * mistaken for a good one (leftover .tmp folders are removed by the next prune).
 *
 * Header-only (static functions) so libs/system/tests can test it without the
 * runtime; the sdbk_plan_* / sdbk_is_stamp / sdbk_backup_root helpers are pure.
 */
#ifndef PS3_SAVEDATA_BACKUP_H
#define PS3_SAVEDATA_BACKUP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define SDBK_KEEP_DEFAULT 5
#define SDBK_MAX_BYTES    (64ull << 20)
#define SDBK_MAX_FILES    256
#define SDBK_NAME         96

enum { SDBK_DONE = 0, SDBK_SAME, SDBK_EMPTY, SDBK_TOOBIG, SDBK_FAIL };

/* "YYYYMMDD_HHMMSS" optionally followed by "_<n>" (two backups in one second). */
static int sdbk_is_stamp(const char* s)
{
    if (!s) return 0;
    for (int i = 0; i < 15; i++) {
        if (i == 8) { if (s[i] != '_') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    if (s[15] == 0) return 1;
    if (s[15] != '_' || !s[16]) return 0;
    for (const char* p = s + 16; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Stamps sort by time; "_<n>" suffixes sort by n after the plain stamp. */
static int sdbk_stamp_cmp(const char* a, const char* b)
{
    const int c = strncmp(a, b, 15);
    if (c) return c;
    const long na = a[15] ? strtol(a + 16, NULL, 10) : 0;
    const long nb = b[15] ? strtol(b + 16, NULL, 10) : 0;
    return na < nb ? -1 : na > nb;
}

/* Which entries of names[0..n) to delete so that `keep` stamp folders remain
 * (oldest first). Non-stamp names are never touched, except "<stamp>.tmp"
 * leftovers of an interrupted backup, which are always deleted. Writes indices
 * into del[] (capacity maxdel); returns how many. */
static int sdbk_plan_prune(const char (*names)[SDBK_NAME], int n, int keep, int* del, int maxdel)
{
    int nd = 0;
    int stamps[SDBK_MAX_FILES];
    int ns = 0;
    for (int i = 0; i < n; i++) {
        const size_t L = strlen(names[i]);
        if (L > 4 && strcmp(names[i] + L - 4, ".tmp") == 0) {
            char base[SDBK_NAME];
            memcpy(base, names[i], L - 4);
            base[L - 4] = 0;
            if (sdbk_is_stamp(base) && nd < maxdel) del[nd++] = i;
            continue;
        }
        if (sdbk_is_stamp(names[i]) && ns < SDBK_MAX_FILES) stamps[ns++] = i;
    }
    if (keep < 1) keep = 1;
    /* selection of the oldest (ns - keep): repeatedly take the minimum */
    for (int k = 0; k < ns - keep && nd < maxdel; k++) {
        int best = -1;
        for (int j = 0; j < ns; j++) {
            if (stamps[j] < 0) continue;
            if (best < 0 || sdbk_stamp_cmp(names[stamps[j]], names[stamps[best]]) < 0) best = j;
        }
        del[nd++] = stamps[best];
        stamps[best] = -1;
    }
    return nd;
}

/* The folder name for a backup made at `now`: UTC, so a DST fall-back or a
 * time-zone change does not make a newer backup sort before an older one. */
static void sdbk_utc_stamp(time_t now, char* out, size_t n)
{
    struct tm t;
#ifdef _WIN32
    if (gmtime_s(&t, &now)) { snprintf(out, n, "19700101_000000"); return; }
#else
    if (!gmtime_r(&now, &t)) { snprintf(out, n, "19700101_000000"); return; }
#endif
    strftime(out, n, "%Y%m%d_%H%M%S", &t);
}

/* The newest stamp in names[0..n), or -1. */
static int sdbk_newest(const char (*names)[SDBK_NAME], int n)
{
    int best = -1;
    for (int i = 0; i < n; i++)
        if (sdbk_is_stamp(names[i]) && (best < 0 || sdbk_stamp_cmp(names[i], names[best]) > 0)) best = i;
    return best;
}

/* The name for a new backup: `stamp`, unless that does not sort after the
 * newest existing backup (same second, or the clock was set back): then the
 * newest one's stamp with the next "_n", so the new backup is always the newest
 * by name (rotation and the unchanged check go by name). */
static void sdbk_next_stamp(const char (*names)[SDBK_NAME], int n, const char* stamp, char* out, size_t out_n)
{
    snprintf(out, out_n, "%s", stamp);
    const int nw = sdbk_newest(names, n);
    if (nw < 0 || sdbk_stamp_cmp(stamp, names[nw]) > 0) return;
    const long k = names[nw][15] ? strtol(names[nw] + 16, NULL, 10) : 1;
    snprintf(out, out_n, "%.15s_%ld", names[nw], k + 1);
}

/* "<root>/gamedata/dev_hdd0/home/00000001/savedata" -> "<root>/saves_backup":
 * the folder next to gamedata (the game folder). A PS3_GAMEDATA_ROOT not named
 * "gamedata" (e.g. Saved Games\Resistance 3) gets it inside: "<that>/saves_backup". */
static void sdbk_backup_root(const char* save_root, char* out, size_t n)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", save_root ? save_root : "");
    size_t L = strlen(tmp);
    for (int up = 0; up < 5; up++) {
        while (L && (tmp[L - 1] == '/' || tmp[L - 1] == '\\')) tmp[--L] = 0;
        if (up == 4) {
            size_t b = L;
            while (b && tmp[b - 1] != '/' && tmp[b - 1] != '\\') b--;
            const char* g = "gamedata";
            int is_gd = L - b == 8;
            for (size_t i = 0; is_gd && i < 8; i++) is_gd = (tmp[b + i] | 0x20) == g[i];
            if (!is_gd && L > 0) break;
        }
        while (L && tmp[L - 1] != '/' && tmp[L - 1] != '\\') tmp[--L] = 0;
    }
    while (L > 1 && (tmp[L - 1] == '/' || tmp[L - 1] == '\\') && tmp[L - 2] != ':') tmp[--L] = 0;
    if (!L) snprintf(out, n, "saves_backup");
    else if (tmp[L - 1] == '/' || tmp[L - 1] == '\\') snprintf(out, n, "%ssaves_backup", tmp);
    else snprintf(out, n, "%s/saves_backup", tmp);
}

/* ---- host file system helpers ---------------------------------------------------- */

typedef struct { char name[SDBK_NAME]; int is_dir; uint64_t size, mtime; } sdbk_entry;

/* Entries of dir (no "." / ".."); returns count, -1 if the folder cannot be read. */
static int sdbk_list(const char* dir, sdbk_entry* out, int max)
{
    int n = 0;
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (strlen(fd.cFileName) >= SDBK_NAME) continue;
        if (n >= max) break;
        snprintf(out[n].name, SDBK_NAME, "%s", fd.cFileName);
        out[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out[n].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out[n].mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strlen(e->d_name) >= SDBK_NAME) continue;
        if (n >= max) break;
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        snprintf(out[n].name, SDBK_NAME, "%s", e->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode);
        out[n].size = (uint64_t)st.st_size;
        out[n].mtime = (uint64_t)st.st_mtime;
        n++;
    }
    closedir(d);
#endif
    return n;
}

static int sdbk_mkdir(const char* p)
{
#ifdef _WIN32
    return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(p, 0755) == 0 || access(p, F_OK) == 0 ? 0 : -1;
#endif
}

static int sdbk_mkdirs(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if ((*p == '/' || *p == '\\') && p[-1] != ':') {
            const char c = *p;
            *p = 0;
            sdbk_mkdir(tmp);
            *p = c;
        }
    }
    return sdbk_mkdir(tmp);
}

/* Remove a folder of plain files (backup folders have no subfolders). */
static void sdbk_rmtree(const char* dir)
{
    sdbk_entry* e = (sdbk_entry*)malloc(sizeof(sdbk_entry) * SDBK_MAX_FILES);
    if (!e) return;
    const int n = sdbk_list(dir, e, SDBK_MAX_FILES);
    for (int i = 0; i < n; i++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, e[i].name);
        if (e[i].is_dir) sdbk_rmtree(p);
        else remove(p);
    }
    free(e);
#ifdef _WIN32
    RemoveDirectoryA(dir);
#else
    rmdir(dir);
#endif
}

static int sdbk_name_cmp(const void* a, const void* b)
{
    return strcmp(((const sdbk_entry*)a)->name, ((const sdbk_entry*)b)->name);
}

/* Same plain files (name, size, modification time) in both lists: the save was
 * not rewritten since that backup (copies keep the time). Sorts both lists. */
static int sdbk_same_meta(sdbk_entry* a, int na, sdbk_entry* b, int nb)
{
    qsort(a, (size_t)na, sizeof(*a), sdbk_name_cmp);
    qsort(b, (size_t)nb, sizeof(*b), sdbk_name_cmp);
    int i = 0, j = 0;
    for (;;) {
        while (i < na && a[i].is_dir) i++;
        while (j < nb && b[j].is_dir) j++;
        if (i >= na || j >= nb) return i >= na && j >= nb;
        if (strcmp(a[i].name, b[j].name) || a[i].size != b[j].size || a[i].mtime != b[j].mtime) return 0;
        i++; j++;
    }
}

/* FNV-1a over (name, size, bytes) of the plain files of dir, in name order. */
static uint64_t sdbk_hash_dir(const char* dir, sdbk_entry* e, int n)
{
    uint64_t h = 1469598103934665603ull;
    unsigned char buf[16384];
    qsort(e, (size_t)n, sizeof(*e), sdbk_name_cmp);
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir) continue;
        for (const char* c = e[i].name; *c; c++) { h ^= (unsigned char)*c; h *= 1099511628211ull; }
        h ^= e[i].size; h *= 1099511628211ull;
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, e[i].name);
        FILE* f = fopen(p, "rb");
        if (!f) { h ^= 0xFF; h *= 1099511628211ull; continue; }
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            for (size_t k = 0; k < r; k++) { h ^= buf[k]; h *= 1099511628211ull; }
        fclose(f);
    }
    return h;
}

static int sdbk_copy_file(const char* src, const char* dst)
{
#ifdef _WIN32
    /* CopyFileExW keeps the time stamps and copies files the game holds open for reading */
    WCHAR ws[1200], wd[1300];
    if (!MultiByteToWideChar(CP_ACP, 0, src, -1, ws, 1200) || !MultiByteToWideChar(CP_ACP, 0, dst, -1, wd, 1300))
        return CopyFileA(src, dst, FALSE) ? 0 : -1;
    return CopyFileExW(ws, wd, NULL, NULL, NULL, 0) ? 0 : -1;
#else
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[16384];
    size_t r;
    int rc = 0;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, r, out) != r) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
#endif
}

static int sdbk_rename(const char* a, const char* b)
{
#ifdef _WIN32
    return MoveFileA(a, b) ? 0 : -1;
#else
    return rename(a, b);
#endif
}

/* Slot names come from the game ("BCES00052-SAVE00"); keep them file-name safe. */
static void sdbk_safe_name(const char* in, char* out, size_t n)
{
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++) {
        const char c = *in;
        out[k++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.') ? c : '_';
    }
    out[k] = 0;
    if (!k || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, n, "slot");
}

static int sdbk_run_in(const char* save_dir, const char* slot, const char* backup_root,
                       int keep, uint64_t max_bytes, const char* stamp,
                       char* out_path, size_t out_n, char* err, size_t err_n,
                       sdbk_entry* src, sdbk_entry* ex, char (*names)[SDBK_NAME])
{
    const int ns = sdbk_list(save_dir, src, SDBK_MAX_FILES);
    uint64_t total = 0;
    int files = 0;
    for (int i = 0; i < ns; i++) if (!src[i].is_dir) { total += src[i].size; files++; }
    if (ns <= 0 || !files) return SDBK_EMPTY;
    if (total > max_bytes) return SDBK_TOOBIG;

    char safe[SDBK_NAME], slot_dir[1100], dst[1200], tmp[1200];
    sdbk_safe_name(slot, safe, sizeof(safe));
    snprintf(slot_dir, sizeof(slot_dir), "%s/%s", backup_root, safe);

    /* existing backups of this slot */
    int nex = sdbk_list(slot_dir, ex, SDBK_MAX_FILES);
    if (nex < 0) nex = 0;
    for (int i = 0; i < nex; i++) snprintf(names[i], SDBK_NAME, "%s", ex[i].name);

    const int newest = sdbk_newest((const char (*)[SDBK_NAME])names, nex);
    if (newest >= 0) {
        char nd[1200];
        snprintf(nd, sizeof(nd), "%s/%s", slot_dir, names[newest]);
        sdbk_entry* ne = (sdbk_entry*)malloc(sizeof(sdbk_entry) * SDBK_MAX_FILES);
        if (ne) {
            const int nn = sdbk_list(nd, ne, SDBK_MAX_FILES);
            /* cheap check first; the content hash catches a rewrite with the same bytes */
            const int same = nn > 0 && (sdbk_same_meta(src, ns, ne, nn) || sdbk_hash_dir(nd, ne, nn) == sdbk_hash_dir(save_dir, src, ns));
            free(ne);
            if (same) { snprintf(out_path, out_n, "%s", nd); return SDBK_SAME; }
        }
    }

    /* always newest by name: a second backup within one second, or after the
     * clock was set back, gets the newest stamp + "_2", "_3", ... */
    char st[SDBK_NAME];
    sdbk_next_stamp((const char (*)[SDBK_NAME])names, nex, stamp, st, sizeof(st));
    snprintf(dst, sizeof(dst), "%s/%s", slot_dir, st);
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    if (sdbk_mkdirs(tmp) != 0) { snprintf(err, err_n, "cannot create %s", tmp); return SDBK_FAIL; }
    for (int i = 0; i < ns; i++) {
        if (src[i].is_dir) continue;
        char a[1200], b[1300];
        snprintf(a, sizeof(a), "%s/%s", save_dir, src[i].name);
        snprintf(b, sizeof(b), "%s/%s", tmp, src[i].name);
        if (sdbk_copy_file(a, b) != 0) {
            snprintf(err, err_n, "cannot copy %s", src[i].name);
            sdbk_rmtree(tmp);
            return SDBK_FAIL;
        }
    }
    if (sdbk_rename(tmp, dst) != 0) {
        snprintf(err, err_n, "cannot rename %s", tmp);
        sdbk_rmtree(tmp);
        return SDBK_FAIL;
    }
    snprintf(out_path, out_n, "%s", dst);

    /* prune this slot to `keep` */
    nex = sdbk_list(slot_dir, ex, SDBK_MAX_FILES);
    if (nex > 0) {
        int del[SDBK_MAX_FILES];
        for (int i = 0; i < nex; i++) snprintf(names[i], SDBK_NAME, "%s", ex[i].name);
        const int nd = sdbk_plan_prune((const char (*)[SDBK_NAME])names, nex, keep, del, SDBK_MAX_FILES);
        for (int i = 0; i < nd; i++) {
            char p[1200];
            snprintf(p, sizeof(p), "%s/%s", slot_dir, names[del[i]]);
            sdbk_rmtree(p);
        }
    }
    return SDBK_DONE;
}

/* Back up save_dir to <backup_root>/<slot>/<stamp> and prune to `keep`.
 * Returns SDBK_DONE / SAME (identical to the newest backup) / EMPTY (nothing to
 * back up) / TOOBIG (over max_bytes) / FAIL; err gets a reason on FAIL. */
static int sdbk_run(const char* save_dir, const char* slot, const char* backup_root,
                    int keep, uint64_t max_bytes, const char* stamp,
                    char* out_path, size_t out_n, char* err, size_t err_n)
{
    sdbk_entry* src = (sdbk_entry*)malloc(sizeof(sdbk_entry) * SDBK_MAX_FILES * 2);
    char (*names)[SDBK_NAME] = (char (*)[SDBK_NAME])malloc(SDBK_NAME * SDBK_MAX_FILES);
    int rc = SDBK_FAIL;
    if (!src || !names) snprintf(err, err_n, "out of memory");
    else rc = sdbk_run_in(save_dir, slot, backup_root, keep, max_bytes, stamp, out_path, out_n, err, err_n,
                          src, src + SDBK_MAX_FILES, names);
    free(src);
    free(names);
    return rc;
}

/* cellSaveData's hook: called before a save writes save_dir. */
static void sdbk_before_save(const char* save_dir, const char* slot, const char* save_root)
{
    const char* e = getenv("PORT_SAVE_BACKUP");
    if (e && e[0] == '0') return;
    int keep = SDBK_KEEP_DEFAULT;
    const char* k = getenv("PORT_SAVE_BACKUP_KEEP");
    if (k && *k) { keep = atoi(k); if (keep < 1) keep = 1; if (keep > 100) keep = 100; }

    char root[1100], stamp[32], out[1200] = "", err[256] = "";
    sdbk_backup_root(save_root, root, sizeof(root));
    sdbk_utc_stamp(time(NULL), stamp, sizeof(stamp));
    const int rc = sdbk_run(save_dir, slot, root, keep, SDBK_MAX_BYTES, stamp, out, sizeof(out), err, sizeof(err));
#ifdef _WIN32
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
#endif
    if (rc == SDBK_DONE) printf("[savebackup] %s -> %s\n", slot, out);
    else if (rc == SDBK_TOOBIG) printf("[savebackup] %s skipped (over 64 MB)\n", slot);
    else if (rc == SDBK_FAIL) printf("[savebackup] %s failed: %s\n", slot, err);
}

#endif /* PS3_SAVEDATA_BACKUP_H */
