#include "game_profiles.h"

#include <stdio.h>
#include <string.h>

static const char *const g_deadcells_hashes[] = {
    "696aaec83db7a21e76c828449167880f001ef056c230350dca6fddccc34cc9c7",
    "376564ab2173ddcbadf53d73baf2fc335793e4d14a637fc1829569c314f39667",
    "d5d17575f4bec6ab674a9cac56fba5fd696576f23fc1b22e32629bcafba92ad3",
    "45ebaecbedeff7c9b4b7d8b2faaf91ecd47c1f74dd7642938444ec9c83b488f5",
    NULL,
};

static const char *const g_deadcells_boot_exes[] = {
    /* platform-neutral variant; hlmod has an easier time loading it (native DirectX is still broken) */
    "deadcells_gl.exe",
    "deadcells.exe",
    NULL,
};

/* Known games. Adding another (Northgard, Wartales, Dune: Spice Wars, ...)
 * is a new entry here, nothing else. */
static const HlmodGameProfile g_hlmod_game_profiles[] = {
    {
        .name = "Dead Cells",
        .bytecode_sha256 = g_deadcells_hashes,
        .steam_appid = "588650",
        .boot_exe_names = g_deadcells_boot_exes,
    },
    { 0 },
};

const HlmodGameProfile *hlmod_game_profile_by_sha256(const char *sha256_hex) {
    for (int i = 0; g_hlmod_game_profiles[i].name != NULL; i++) {
        const char *const *hashes = g_hlmod_game_profiles[i].bytecode_sha256;
        if (!hashes) continue;
        for (int j = 0; hashes[j] != NULL; j++) {
            if (strcmp(sha256_hex, hashes[j]) == 0) return &g_hlmod_game_profiles[i];
        }
    }
    return NULL;
}

void hlmod_game_profile_apply_compat(const char *sha256_hex) {
    const HlmodGameProfile *profile = hlmod_game_profile_by_sha256(sha256_hex);
    if (!profile) return;
    printf("[hlmod] %s detected...\n", profile->name);
#ifdef HL_WIN
    if (!profile->steam_appid) return;
    pchar exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, MAX_PATH);

    pchar appid_path[MAX_PATH];
    swprintf(appid_path, MAX_PATH, L"%ssteam_appid.txt", exe_dir);

    if (_waccess(appid_path, 0) == -1) {
        printf("[hlmod] steam_appid.txt not found. Creating it for %s (%s).\n", profile->name, profile->steam_appid);
        FILE* f = _wfopen(appid_path, L"w");
        if (f) {
            fprintf(f, "%s", profile->steam_appid);
            fclose(f);
        }
    }
#endif
}

const pchar *hlmod_game_profile_find_boot_file(void) {
#ifdef HL_WIN
    static pchar wide_name[MAX_PATH];
#endif
    for (int i = 0; g_hlmod_game_profiles[i].name != NULL; i++) {
        const char *const *candidates = g_hlmod_game_profiles[i].boot_exe_names;
        if (!candidates) continue;
        for (int j = 0; candidates[j] != NULL; j++) {
            if (!fileExists(candidates[j])) continue;
#ifdef HL_WIN
            /* HL's GC/allocator isn't initialized yet at this point in
             * main(), so this can't use hl_to_utf16; MultiByteToWideChar
             * needs no HL state. */
            MultiByteToWideChar(CP_UTF8, 0, candidates[j], -1, wide_name, MAX_PATH);
            return wide_name;
#else
            return candidates[j];
#endif
        }
    }
    return NULL;
}
