/* savedata_backup.h: stamp names, prune plan (keep the newest N per slot), the
 * backup folder next to gamedata, and a real backup round trip in a scratch
 * folder (copy, identical-skip, rotation to N, .tmp leftovers removed).
 * Usage: test_savedata_backup <scratch dir> */
#include "../savedata_backup.h"

static int g_fail = 0, g_n = 0;
static void check(int ok, const char* what)
{
    g_n++;
    if (!ok) { g_fail++; printf("FAIL: %s\n", what); }
    else printf("ok  : %s\n", what);
}

static void put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int count_stamps(const char* dir, char newest[SDBK_NAME], int* tmp_left)
{
    sdbk_entry e[64];
    const int n = sdbk_list(dir, e, 64);
    int c = 0;
    newest[0] = 0;
    *tmp_left = 0;
    for (int i = 0; i < n; i++) {
        if (sdbk_is_stamp(e[i].name)) {
            c++;
            if (!newest[0] || sdbk_stamp_cmp(e[i].name, newest) > 0) snprintf(newest, SDBK_NAME, "%s", e[i].name);
        } else if (strstr(e[i].name, ".tmp")) (*tmp_left)++;
    }
    return c;
}

int main(int argc, char** argv)
{
    /* stamps */
    check(sdbk_is_stamp("20250102_030405"), "plain stamp");
    check(sdbk_is_stamp("20250102_030405_2"), "stamp with _n");
    check(!sdbk_is_stamp("20250102-030405"), "wrong separator");
    check(!sdbk_is_stamp("20250102_03040"), "short stamp");
    check(!sdbk_is_stamp("20250102_030405_"), "empty suffix");
    check(!sdbk_is_stamp("20250102_030405.tmp"), ".tmp is not a stamp");
    check(sdbk_stamp_cmp("20250102_030405_10", "20250102_030405_9") > 0, "_10 after _9");
    check(sdbk_stamp_cmp("20250102_030405", "20250102_030405_2") < 0, "plain before _2");

    /* prune plan */
    {
        static const char names[][SDBK_NAME] = {
            "20250105_000000", "20250101_000000", "notes.txt", "20250103_000000",
            "20250102_000000", "20250104_000000", "20250106_000000", "20250107_000000.tmp",
            "20250106_000000_2",
        };
        int del[16];
        const int nd = sdbk_plan_prune(names, 9, 5, del, 16);
        int seen[9] = {0};
        for (int i = 0; i < nd; i++) seen[del[i]] = 1;
        check(nd == 3 && seen[7] && seen[1] && seen[4], "prune: .tmp leftover + two oldest of 7");
        check(!seen[2], "prune: foreign files untouched");
        check(sdbk_newest(names, 9) == 8, "newest: _2 suffix wins");
        const int nd0 = sdbk_plan_prune(names, 2, 5, del, 16);
        check(nd0 == 0, "prune: under the limit deletes nothing");
        const int nd1 = sdbk_plan_prune(names, 7, 0, del, 16);
        check(nd1 == 5, "prune: keep < 1 still keeps one");
    }

    /* backup root */
    {
        char out[1100];
        sdbk_backup_root("./gamedata/dev_hdd0/home/00000001/savedata", out, sizeof(out));
        check(!strcmp(out, "./saves_backup"), "root: default cwd gamedata");
        sdbk_backup_root("D:\\Games\\R2\\gamedata\\dev_hdd0\\home\\00000001\\savedata", out, sizeof(out));
        check(!strcmp(out, "D:\\Games\\R2/saves_backup"), "root: absolute");
        sdbk_backup_root("D:\\gamedata\\dev_hdd0\\home\\00000001\\savedata", out, sizeof(out));
        check(!strcmp(out, "D:\\saves_backup"), "root: drive root");
        sdbk_backup_root("gamedata/dev_hdd0/home/00000001/savedata", out, sizeof(out));
        check(!strcmp(out, "saves_backup"), "root: bare relative");
        sdbk_backup_root("C:\\Users\\x\\Saved Games\\Resistance 3/dev_hdd0/home/00000001/savedata", out, sizeof(out));
        check(!strcmp(out, "C:\\Users\\x\\Saved Games\\Resistance 3/saves_backup"), "root: PS3_GAMEDATA_ROOT folder keeps them inside");
    }

    {   /* stamps are UTC; a new backup always sorts newest by name */
        char st[SDBK_NAME];
        sdbk_utc_stamp((time_t)0, st, sizeof(st));
        check(!strcmp(st, "19700101_000000"), "utc stamp: epoch");
        sdbk_utc_stamp((time_t)1700000000, st, sizeof(st));
        check(!strcmp(st, "20231114_221320"), "utc stamp: not local time");
        const char ex[][SDBK_NAME] = { "20251102_013000", "20251102_015959", "notes", "20251102_020000.tmp" };
        sdbk_next_stamp(ex, 4, "20251102_014500", st, sizeof(st));   /* clock set back 15 min */
        check(!strcmp(st, "20251102_015959_2") && sdbk_stamp_cmp(st, ex[1]) > 0, "next stamp: clock set back -> after the newest");
        sdbk_next_stamp(ex, 4, "20251102_015959", st, sizeof(st));
        check(!strcmp(st, "20251102_015959_2"), "next stamp: same second -> _2");
        const char ex2[][SDBK_NAME] = { "20251102_015959", "20251102_015959_3" };
        sdbk_next_stamp(ex2, 2, "20251101_000000", st, sizeof(st));
        check(!strcmp(st, "20251102_015959_4"), "next stamp: after _3 -> _4");
        sdbk_next_stamp(ex, 4, "20251102_030000", st, sizeof(st));
        check(!strcmp(st, "20251102_030000"), "next stamp: later time used as is");
        sdbk_next_stamp(ex, 0, "20251102_000000", st, sizeof(st));
        check(!strcmp(st, "20251102_000000"), "next stamp: first backup");
    }

    /* safe slot names */
    {
        char s[SDBK_NAME];
        sdbk_safe_name("BCES00052-SAVE00", s, sizeof(s));
        check(!strcmp(s, "BCES00052-SAVE00"), "slot name kept");
        sdbk_safe_name("..", s, sizeof(s));
        check(!strcmp(s, "slot"), "slot name '..' replaced");
        sdbk_safe_name("a/b:c", s, sizeof(s));
        check(!strcmp(s, "a_b_c"), "slot name separators replaced");
    }

    /* round trip on disk */
    if (argc > 1) {
        char save[1100], root[1100], slot_dir[1200], out[1200], err[256], newest[SDBK_NAME], p0[1100];
        int tmp_left = 0;
        snprintf(save, sizeof(save), "%s/save/SLOT0", argv[1]);
        snprintf(root, sizeof(root), "%s/saves_backup", argv[1]);
        snprintf(slot_dir, sizeof(slot_dir), "%s/SLOT0", root);
        /* a fresh start when the scratch folder is reused (ctest) */
        snprintf(p0, sizeof(p0), "%s/save", argv[1]);
        sdbk_rmtree(p0);
        sdbk_rmtree(root);
        sdbk_mkdirs(save);

        check(sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20250101_000000", out, sizeof(out), err, sizeof(err)) == SDBK_EMPTY,
              "run: empty slot is not backed up");

        char p[1300];
        snprintf(p, sizeof(p), "%s/PARAM.SFO", save); put(p, "sfo-v1");
        snprintf(p, sizeof(p), "%s/DATA.BIN", save);  put(p, "data-v1");
        check(sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20250101_000001", out, sizeof(out), err, sizeof(err)) == SDBK_DONE,
              "run: first backup");
        snprintf(p, sizeof(p), "%s/20250101_000001/DATA.BIN", slot_dir);
        {
            char buf[32] = "";
            FILE* f = fopen(p, "rb");
            if (f) { buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0; fclose(f); }
            check(!strcmp(buf, "data-v1"), "run: file content copied");
        }
        check(sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20250101_000002", out, sizeof(out), err, sizeof(err)) == SDBK_SAME,
              "run: unchanged save is skipped");

        /* stale .tmp from an interrupted backup */
        snprintf(p, sizeof(p), "%s/20240101_000000.tmp", slot_dir);
        sdbk_mkdirs(p);

        for (int v = 2; v <= 8; v++) {
            char text[32], st[32];
            snprintf(p, sizeof(p), "%s/DATA.BIN", save);
            snprintf(text, sizeof(text), "data-v%d", v);
            put(p, text);
            snprintf(st, sizeof(st), "20250101_0000%02d", v + 1);
            if (sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, st, out, sizeof(out), err, sizeof(err)) != SDBK_DONE)
                printf("  run %d: %s\n", v, err);
        }
        const int c = count_stamps(slot_dir, newest, &tmp_left);
        check(c == 5, "rotation: five backups kept");
        check(!strcmp(newest, "20250101_000009"), "rotation: newest kept");
        check(tmp_left == 0, "rotation: .tmp leftover removed");

        /* same second twice -> _2 */
        snprintf(p, sizeof(p), "%s/DATA.BIN", save); put(p, "data-v9");
        check(sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20250101_000009", out, sizeof(out), err, sizeof(err)) == SDBK_DONE &&
              strstr(out, "20250101_000009_2") != NULL, "run: same-second stamp gets _2");

        check(sdbk_run(save, "SLOT0", root, 5, 4, "20250101_000020", out, sizeof(out), err, sizeof(err)) == SDBK_TOOBIG,
              "run: over the size cap is skipped");

        /* clock set back: the new backup still sorts newest, survives the prune, and
           the unchanged check compares against it */
        snprintf(p, sizeof(p), "%s/DATA.BIN", save); put(p, "data-v10");
        const int rb = sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20240101_000000", out, sizeof(out), err, sizeof(err));
        count_stamps(slot_dir, newest, &tmp_left);
        snprintf(p, sizeof(p), "%s/20250101_000009_3/DATA.BIN", slot_dir);
        FILE* f = fopen(p, "rb"); char nb[32] = ""; if (f) { size_t r = fread(nb, 1, sizeof(nb) - 1, f); nb[r] = 0; fclose(f); }
        check(rb == SDBK_DONE && !strcmp(newest, "20250101_000009_3") && !strcmp(nb, "data-v10"),
              "run: clock set back -> newest by name, kept");
        check(sdbk_run(save, "SLOT0", root, 5, SDBK_MAX_BYTES, "20240101_000001", out, sizeof(out), err, sizeof(err)) == SDBK_SAME,
              "run: unchanged after a clock change -> SAME");
    }

    printf("%d/%d passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
