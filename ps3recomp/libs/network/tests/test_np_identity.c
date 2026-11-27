/*
 * test_np_identity - the player name np_psnr_setup settles on.
 *
 *   clang-cl /I include /Fe:test_np_identity.exe libs/network/tests/test_np_identity.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_identity libs/network/tests/test_np_identity.c
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"

#include <assert.h>

const char* np_fake_username(void) { return "PS3Player"; }

#ifdef _WIN32
#  define OS_USER "USERNAME"
#else
#  define OS_USER "USER"
#endif

static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* Resolve afresh, as a new process would. */
static const char* resolve(const char* username, const char* server)
{
    s_id_state = 0;
    np_psnr_setup(username, server);
    return np_psnr_identity();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    set_env("PS3_NP_ONLINE_ID", "");
    const char* idf = "test_np_identity_id.txt";
    set_env("PS3_NP_ID_FILE", idf);
    remove(idf);

    /* Offline with no name: no identity, so the old fixed PSID/MAC and
     * "PS3Player" stand and offline saves don't change. */
    assert(resolve(NULL, NULL) == NULL);
    assert(strcmp(np_psnr_online_id(), "PS3Player") == 0);

    /* A name applies offline too, and the flag beats the variable. */
    set_env("PS3_NP_ONLINE_ID", "marge");
    assert(strcmp(resolve(NULL, NULL), "marge") == 0);
    assert(strcmp(resolve("homer", NULL), "homer") == 0);
    assert(strcmp(np_psnr_online_id(), "homer") == 0);

    /* PSN's rules: disallowed characters dropped, 16 at most, and a name that
     * is still not an ID (too short, or not starting with a letter) is
     * ignored -- here falling back to the variable. */
    assert(strcmp(resolve("Bart Simpson!", NULL), "BartSimpson") == 0);
    assert(strcmp(resolve("abcdefghijklmnopqrstuvwxyz", NULL), "abcdefghijklmnop") == 0);
    assert(strcmp(resolve("x!", NULL), "marge") == 0);
    assert(strcmp(resolve("1abc", NULL), "marge") == 0);

    /* Offline never touches the saved-name file. */
    set_env("PS3_NP_ONLINE_ID", "");
    assert(resolve(NULL, NULL) == NULL);
    assert(fopen(idf, "r") == NULL);

    /* Online with no psnr server (a title with its own backend, such as
     * Medius, given only PS3_NET_HOSTS): a random valid ID too -- never the
     * OS login name, and never the shared "PS3Player". */
    set_env(OS_USER, "LisaS");
    set_env("PS3_NET_ONLINE", "1");
    char first[17];
    {
        const char* id = resolve(NULL, NULL);
        char clean[17];
        assert(id && strcmp(id, "LisaS") != 0 && strcmp(id, "PS3Player") != 0);
        assert(clean_online_id(id, clean) && strcmp(clean, id) == 0);
        assert(!np_psnr_enabled());
        snprintf(first, sizeof first, "%s", id);
    }

    /* It is saved, so the next run is the same player. */
    assert(strcmp(resolve(NULL, NULL), first) == 0);
    {
        char line[64] = {0};
        FILE* f = fopen(idf, "r");
        assert(f && fgets(line, sizeof line, f));
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        assert(strcmp(line, first) == 0);
    }

    /* The player can edit the file; a name given outright still wins. */
    {
        FILE* f = fopen(idf, "w");
        fputs("Maggie\n", f);
        fclose(f);
    }
    assert(strcmp(resolve(NULL, NULL), "Maggie") == 0);
    assert(strcmp(resolve("homer", NULL), "homer") == 0);

    /* An edited file keeps working: a BOM, spaces and CRLF are trimmed. A
     * file with no salt line gets one, and keeps it. */
    write_file(idf, "\xEF\xBB\xBF  Maggie \t\r\n");
    assert(strcmp(resolve(NULL, NULL), "Maggie") == 0);
    char key1[64];
    {
        uint64_t salt; int has_salt; char nm[17];
        assert(load_saved_id(nm, &salt, &has_salt) == 1 && has_salt && !strcmp(nm, "Maggie"));
        snprintf(key1, sizeof key1, "Maggie#%016llx", (unsigned long long)salt);
        assert(strcmp(np_psnr_identity_key(), key1) == 0);
    }
    assert(strcmp(resolve(NULL, NULL), "Maggie") == 0 && strcmp(np_psnr_identity_key(), key1) == 0);

    /* The same name with another salt is another identity; renaming keeps the
     * salt line, so only the name changes. */
    write_file(idf, "Maggie\n00000000000000aa\n");
    assert(strcmp(resolve(NULL, NULL), "Maggie") == 0);
    assert(strcmp(np_psnr_identity_key(), "Maggie#00000000000000aa") == 0);
    assert(strcmp(np_psnr_identity_key(), key1) != 0);
    write_file(idf, "Lisa\n00000000000000aa\n");
    assert(strcmp(resolve(NULL, NULL), "Lisa") == 0);
    assert(strcmp(np_psnr_identity_key(), "Lisa#00000000000000aa") == 0);

    /* A name set outright has no salt: the same name is the same player. */
    assert(strcmp(resolve("homer", NULL), "homer") == 0);
    assert(strcmp(np_psnr_identity_key(), "homer") == 0);

    /* A name that is not a valid ID is reported, a random name is used for
     * this run, and the file is left alone for the player to fix. */
    write_file(idf, "1bad name\n00000000000000aa\n");
    {
        const char* id = resolve(NULL, NULL);
        char clean[17];
        assert(id && clean_online_id(id, clean) && strcmp(clean, id) == 0);
        char line[64] = {0};
        FILE* f = fopen(idf, "r");
        assert(f && fgets(line, sizeof line, f));
        fclose(f);
        assert(strcmp(line, "1bad name\n") == 0);
    }

    /* --psnr goes online the same way, and uses the saved name. */
    write_file(idf, "Bart\n");
    assert(strcmp(resolve(NULL, "127.0.0.1:36100"), "Bart") == 0);
    assert(np_psnr_enabled());
    assert(strcmp(getenv("PSNR_SERVER"), "127.0.0.1:36100") == 0);

    /* Every name the lists can make is a valid ID, and no word repeats. */
    {
        const size_t na = sizeof k_adj / sizeof *k_adj, nb = sizeof k_animal / sizeof *k_animal;
        assert(na >= 100 && nb >= 100);
        for (size_t i = 0; i < na; i++)
            for (size_t j = 0; j < nb; j++) {
                char id[40], clean[17];
                snprintf(id, sizeof id, "%s%s", k_adj[i], k_animal[j]);
                assert(clean_online_id(id, clean) && strcmp(clean, id) == 0);
            }
        for (size_t i = 0; i < na; i++)
            for (size_t j = i + 1; j < na; j++) assert(strcmp(k_adj[i], k_adj[j]) != 0);
        for (size_t i = 0; i < nb; i++)
            for (size_t j = i + 1; j < nb; j++) assert(strcmp(k_animal[i], k_animal[j]) != 0);
    }

    /* Every generated name is a valid ID. */
    for (int i = 0; i < 1000; i++) {
        char id[17], clean[17];
        np_psnr_random_id(id);
        assert(clean_online_id(id, clean) && strcmp(clean, id) == 0);
    }

    remove(idf);
    printf("test_np_identity: all passed\n");
    return 0;
}
