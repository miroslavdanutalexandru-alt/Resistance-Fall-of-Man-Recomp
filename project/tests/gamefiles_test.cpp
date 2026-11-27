// port_gamefiles.h: the launcher's "Game files folder" -- what a picked folder is taken as, the checks before it is
// saved, PARAM.SFO's title id and which folder wins (PS3_VFS_ROOT > the setting > <port>\gamefiles).
// Builds a fake game folder under the current directory (gamefiles_test\, removed again).
#include "port_gamefiles.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void put(const std::string& p, const std::vector<unsigned char>& b)
{
    for (size_t i = 3; i < p.size(); i++) if (p[i] == '\\') CreateDirectoryA(p.substr(0, i).c_str(), NULL);
    if (FILE* f = fopen(p.c_str(), "wb")) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
}
static void rm(const std::string& d)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = d + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm(p); else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(d.c_str());
}
// a PARAM.SFO with CATEGORY=DG and TITLE_ID=<id>
static std::vector<unsigned char> sfo(const char* id)
{
    std::vector<unsigned char> b(20 + 2 * 16);
    auto le16 = [&](size_t o, unsigned v) { b[o] = v & 255; b[o + 1] = v >> 8 & 255; };
    auto le32 = [&](size_t o, unsigned v) { for (int i = 0; i < 4; i++) b[o + i] = v >> (8 * i) & 255; };
    memcpy(b.data(), "\0PSF", 4); le32(4, 0x101);
    const char keys[] = "CATEGORY\0TITLE_ID\0";
    const unsigned kt = (unsigned)b.size();
    b.insert(b.end(), keys, keys + sizeof keys - 1);
    while (b.size() % 4) b.push_back(0);
    const unsigned dt = (unsigned)b.size();
    le32(8, kt); le32(12, dt); le32(16, 2);
    le16(20, 0); le16(22, 0x0204); le32(24, 3); le32(28, 4); le32(32, 0);
    le16(36, 9); le16(38, 0x0204); le32(40, 10); le32(44, 16); le32(48, 4);
    const char data[] = "DG\0\0";
    b.insert(b.end(), data, data + 4);
    std::vector<unsigned char> t(16, 0); memcpy(t.data(), id, strlen(id));
    b.insert(b.end(), t.begin(), t.end());
    return b;
}

int main()
{
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
    const std::string root = std::string(cwd) + "\\gamefiles_test";
    rm(root);
    const std::string port = root + "\\port", gf = root + "\\elsewhere\\gamefiles";
    static const char* const req[] = { "PS3_GAME/PARAM.SFO", "PS3_GAME/USRDIR/EBOOT.BIN", "PS3_GAME/USRDIR/game.psarc", nullptr };
    put(gf + "\\PS3_GAME\\PARAM.SFO", sfo("BCUS98107"));
    put(gf + "\\PS3_GAME\\USRDIR\\EBOOT.BIN", { 1 });
    CreateDirectoryA(port.c_str(), NULL);

    // the checks before it is saved
    CHECK(port_gamefiles_problem(gf, req).find("game.psarc is missing") != std::string::npos);
    put(gf + "\\PS3_GAME\\USRDIR\\game.psarc", { 1 });
    CHECK(port_gamefiles_problem(gf, req).empty());
    CHECK(port_gamefiles_problem(gf + "\\", req).empty());
    put(gf + ".copying", { 1 });                                  // Setup's copy was cut short
    CHECK(port_gamefiles_problem(gf, req).find(".copying") != std::string::npos);
    DeleteFileA((gf + ".copying").c_str());
    CHECK(port_gamefiles_problem(root + "\\nothere", req).find("does not exist") != std::string::npos);

    // what a picked folder is taken as: the folder that holds PS3_GAME
    CHECK(port_gamefiles_normalize(gf) == gf);
    CHECK(port_gamefiles_normalize(gf + "\\") == gf);
    CHECK(port_gamefiles_normalize(root + "\\elsewhere") == gf);          // the folder above (a port folder)
    CHECK(port_gamefiles_normalize(gf + "\\PS3_GAME") == gf);
    CHECK(port_gamefiles_normalize(gf + "\\PS3_GAME\\USRDIR") == gf);
    CHECK(port_gamefiles_normalize(root + "\\nothere") == root + "\\nothere");
    CHECK(port_gamefiles_normalize("E:/Games/gf/") == "E:\\Games\\gf");
    CHECK(port_gamefiles_clean("E:") == "E:\\" && port_gamefiles_clean("E:\\\\") == "E:\\");

    // paths settings.bat / run.bat can carry
    CHECK(port_gamefiles_path_problem("").empty());
    CHECK(port_gamefiles_path_problem("E:\\Games\\Resistance (PS3) & more\\gamefiles").empty());
    CHECK(port_gamefiles_path_problem("\\\\nas\\games\\gamefiles").empty());
    CHECK(!port_gamefiles_path_problem("gamefiles").empty());              // not a full path
    CHECK(!port_gamefiles_path_problem("E:\\100%\\gamefiles").empty());
    CHECK(!port_gamefiles_path_problem("E:\\a\"b").empty());
    CHECK(!port_gamefiles_path_problem("E:\\wow!\\gamefiles").empty());
    CHECK(!port_gamefiles_path_problem("E:\\Spiele\\R\xC3\xA9sistance").empty());
    CHECK(!port_gamefiles_path_problem("E:\\" + std::string(220, 'a')).empty());

    CHECK(port_gamefiles_title_id(gf) == "BCUS98107");
    CHECK(port_gamefiles_title_id(port).empty());

    CHECK(port_gamefiles_same(gf, gf + "\\") && port_gamefiles_same(gf, root + "\\ELSEWHERE\\x\\..\\GameFiles"));
    CHECK(!port_gamefiles_same(gf, port));

    // which folder wins: PS3_VFS_ROOT > the setting > <port>\gamefiles
    int src = -1;
    SetEnvironmentVariableA("PS3_VFS_ROOT", NULL);
    CHECK(port_gamefiles_effective(port, "", &src) == port + "\\gamefiles" && src == PORT_GF_DEFAULT);
    CHECK(port_gamefiles_effective(port, gf + "\\", &src) == gf && src == PORT_GF_SETTING);
    SetEnvironmentVariableA("PS3_VFS_ROOT", "F:\\manual\\gamefiles");
    CHECK(port_gamefiles_effective(port, gf, &src) == "F:\\manual\\gamefiles" && src == PORT_GF_ENV);
    SetEnvironmentVariableA("PS3_VFS_ROOT", NULL);

    rm(root);
    std::printf(g_fail ? "%d FAILED\n" : "gamefiles: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
