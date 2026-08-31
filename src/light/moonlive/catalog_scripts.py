"""Generate the MoonLive script CATALOG: the name and role of every factory script.

Called by catalog_scripts.cmake, never by hand. It takes a file listing the script paths (one per
line) and writes a header holding their NAMES, not their contents: a name costs ~12 bytes where a
script costs ~800, so the catalog stays a few KB however large the library grows. The device fetches
a script's text the first time someone picks it (the UI fetches it from GitHub and posts it to /api/file).

Python rather than pure CMake so the role can be derived from the extension in one place and the
name collision below can be reported properly.
"""

import sys
from pathlib import Path

# The role a script plays, from its extension. This mirrors MoonLiveScriptFile.h's kEffectExt /
# kLayoutExt / kModifierExt, and it is what a picker filters on: the DEVICE keeps one flat
# directory, so the extension is the only role signal once a file lands there.
ROLE_BY_EXT = {".mle": "Effect", ".mll": "Layout", ".mlm": "Modifier"}

# Where each role lives in the repo. The device keeps one flat directory, so this is only ever part
# of the download URL.
FOLDER_BY_ROLE = {"Effect": "effects", "Layout": "layouts", "Modifier": "modifiers"}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: catalog_scripts.py <filelist> <out.h>", file=sys.stderr)
        return 2

    paths = [Path(p) for p in Path(sys.argv[1]).read_text().split("\n") if p.strip()]
    out = Path(sys.argv[2])

    # A name must be unique on the device: the repo's subfolders vanish when scripts land in one
    # flat directory, so two roles sharing a base name would collide there. Catching it here turns a
    # silent overwrite on a user's device into a build failure.
    seen: dict[str, Path] = {}
    for p in paths:
        if p.suffix not in ROLE_BY_EXT:
            print(f"catalog_scripts: {p} has no known script extension "
                  f"({', '.join(ROLE_BY_EXT)})", file=sys.stderr)
            return 1
        if p.name in seen:
            print(f"catalog_scripts: duplicate script name {p.name} "
                  f"({seen[p.name]} and {p}); the device keeps one flat directory, "
                  f"so these would collide", file=sys.stderr)
            return 1
        seen[p.name] = p

    # One array per role rather than one array of {name, folder, role}. The folder is implied by
    # the role ("effects" holds the effects) and the role by the extension, so storing either per
    # entry would be the same value repeated once per script. It also makes the picker's job a
    # range rather than a scan: it wants "every effect", which is now an array, not a filter.
    by_role: dict[str, list[str]] = {r: [] for r in ROLE_BY_EXT.values()}
    for p in paths:
        by_role[ROLE_BY_EXT[p.suffix]].append(p.name)

    parts = [
        "// Auto-generated from moonlive/ by catalog_scripts.cmake. Do not edit; rebuild to update.\n",
        "//\n",
        "// The CATALOG, not the library: names only. A device carries this list and the UI fetches a\n",
        "// script's text from GitHub the first time someone picks it, so flash scales with how many\n",
        "// scripts exist rather than how large they are, and the filesystem holds only what is used.\n",
        "//\n",
        "// One array per role: the folder a script lives in is implied by its role and the role by its\n",
        "// extension, so neither is stored per entry.\n",
        "#pragma once\n",
        "#include <cstddef>\n\n",
        "namespace mm::moonlive {\n\n",
    ]

    for role, names in by_role.items():
        lower = role.lower()
        folder = FOLDER_BY_ROLE[role]
        parts.append(f"/// Every factory {lower}, by file name. They live in `moonlive/{folder}/`\n")
        parts.append(f"/// upstream and in the factory script directory on the device.\n")
        parts.append(f"constexpr const char* k{role}Catalog[] = {{\n")
        parts.append("".join(f'    "{n}",\n' for n in names))
        parts.append("};\n")
        parts.append(f"constexpr size_t k{role}CatalogCount = {len(names)};\n")
        parts.append(f'constexpr const char* k{role}Folder = "{folder}";   ///< its directory upstream\n\n')

    total = sum(len(v) for v in by_role.values())
    parts.append(f"constexpr size_t kCatalogCount = {total};   ///< every factory script, all roles\n\n")
    parts.append("} // namespace mm::moonlive\n")

    out.write_text("".join(parts))
    print(f"catalog: {len(paths)} scripts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
