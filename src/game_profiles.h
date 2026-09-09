#ifndef HLMOD_GAME_PROFILES_H
#define HLMOD_GAME_PROFILES_H

#include "platform.h"

/* A known game's bytecode fingerprints and launch quirks. Add a new entry to
 * `g_hlmod_game_profiles` in game_profiles.c to support another game -
 * nothing else in hlmod needs to change. All fields except `name` are
 * optional (NULL-terminated arrays, or NULL to opt out of that fixup). */
typedef struct {
    const char *name;                      /* human-readable, for logging */
    const char *const *bytecode_sha256;    /* known-good hashes for this game's main bytecode, or NULL */
    const char *steam_appid;               /* writes steam_appid.txt next to the exe if missing, or NULL */
    const char *const *boot_exe_names;     /* alternate boot executables to try when hlboot.dat is absent, or NULL */
} HlmodGameProfile;

/* Looks up the profile whose `bytecode_sha256` contains the given hex
 * digest, or NULL if unrecognized. */
const HlmodGameProfile *hlmod_game_profile_by_sha256(const char *sha256_hex);

/* Runs the matching profile's post-load launch fixups (currently: writing a
 * missing steam_appid.txt). No-op if no profile matches. */
void hlmod_game_profile_apply_compat(const char *sha256_hex);

/* Returns the first existing file, among every profile's `boot_exe_names`,
 * found in the current working directory - for games that don't ship a bare
 * `hlboot.dat`. Returns NULL if none exist. The returned pointer is valid
 * for the rest of the process; do not free it. */
const pchar *hlmod_game_profile_find_boot_file(void);

#endif // HLMOD_GAME_PROFILES_H
