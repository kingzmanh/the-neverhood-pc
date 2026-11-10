/* What the installer does that is particular to The Neverhood.
 *
 * The CD is copied whole to <install>\game\cd (the game's D: drive, always
 * "in the drive").  <install>\game\hd is C:\NEVERHOOD, what the original
 * setup95.exe installed: the game exe and the Smacker DLL (their code is in
 * our exe; their data and resources are read from these files) and HD.BLB. */
#include "setup.h"
#include <string.h>
#include "install.h"

int game_is_cd(CdSrc *c)
{
    static const char *files[] = GAME_CD_FILES;
    if (!c) return 0;
    for (int i = 0; files[i]; i++)
        if (!cd_exists(c, files[i])) { setup_log("CD check: %s missing", files[i]); return 0; }
    return 1;
}

int game_install_files(const char *cd, const char *hd)
{
    static const char *map[][2] = {
        { "SETUP\\NHC.EXE", "nhc.exe" },
        { "SETUP\\SMACKW32.DLL", "smackw32.dll" },
        { "DATA\\HD.BLB", "HD.BLB" },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        char s[MAX_PATH * 2], d[MAX_PATH * 2];
        snprintf(s, sizeof s, "%s\\%s", cd, map[i][0]);
        snprintf(d, sizeof d, "%s\\%s", hd, map[i][1]);
        if (!copy_file(s, d, NULL)) {
            setup_log("cannot copy %s", s);
            MessageBoxA(NULL, "Copying the game files from the CD failed.", GAME_TITLE " setup", MB_ICONERROR);
            return 0;
        }
    }
    return 1;
}

const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
const char *game_icon_ico(void) { return GAME_ICON_ICO; }
