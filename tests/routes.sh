#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
Cleanup() {
    # Only this test's own tree; no dependency caches or unrelated build files.
    find "$task_tmp" -type f -exec unlink {} \;
    find "$task_tmp" -depth -type d -exec rmdir {} \;
}
trap Cleanup EXIT
mkdir "$task_tmp/project"
cat > "$task_tmp/project/app.cfg" <<'CFG'
version(1);
route('profile', '/profile/:id', (context) => {});
route('parameterFirst', '/:first/b/:last', (context) => {});
route('literalFirst', '/a/:middle/end', (context) => {});
route('newProfile', '/profile/new', (context) => {});
route('trailing', '/a/:middle/end/', (context) => {});
route('home', '/', (context) => {});
route('escaped', '/quote/"\\', (context) => {});
CFG
./build/app-compiler build "$task_tmp/project"
test -f "$task_tmp/project/src/generated/app-routes.h"
test ! -e "$task_tmp/project/build/app-routes.c"
test ! -e "$task_tmp/project/build/app-routes.h"
# Compilation owns generated files, never handwritten route implementations.
mkdir -p "$task_tmp/project/src/business"
printf '/* handwritten route implementation */\n' > "$task_tmp/project/src/business/route_profile.c"
cp "$task_tmp/project/src/business/route_profile.c" "$task_tmp/handwritten.c"
flags=(-std=c11 -Wall -Wextra -Wpedantic -Werror -Werror=enum-conversion -Werror=enum-compare -Iexternal/common -Iexternal/secure-backends -I"$task_tmp/project/src/generated")
"${CC:-cc}" "${flags[@]}" -O2 tests/routes.c "$task_tmp/project/src/generated/app-routes.c" external/common/bump.c external/common/os_memory.c -o "$task_tmp/routes"
"$task_tmp/routes"
"${CC:-cc}" "${flags[@]}" -O1 -g -fsanitize=address,undefined tests/routes.c "$task_tmp/project/src/generated/app-routes.c" external/common/bump.c external/common/os_memory.c -o "$task_tmp/routes-sanitized"
"$task_tmp/routes-sanitized"
# Reverse declaration order: precedence must remain the same.
{ head -n 1 "$task_tmp/project/app.cfg"; tail -n +2 "$task_tmp/project/app.cfg" | tac; } > "$task_tmp/reordered.cfg"
cp "$task_tmp/reordered.cfg" "$task_tmp/project/app.cfg"
./build/app-compiler build "$task_tmp/project"
"${CC:-cc}" "${flags[@]}" -O2 tests/routes.c "$task_tmp/project/src/generated/app-routes.c" external/common/bump.c external/common/os_memory.c -o "$task_tmp/routes"
"$task_tmp/routes"
cp "$task_tmp/project/src/generated/app-routes.c" "$task_tmp/saved.c"
cat >> "$task_tmp/project/app.cfg" <<'CFG'
route('duplicate', '/profile/:different', (context) => {});
CFG
if ./build/app-compiler build "$task_tmp/project" >"$task_tmp/diagnostic" 2>&1; then
    echo 'Duplicate matching pattern unexpectedly compiled' >&2
    exit 1
fi
rg -q 'duplicate route matching pattern' "$task_tmp/diagnostic"
cmp "$task_tmp/saved.c" "$task_tmp/project/src/generated/app-routes.c"
cmp "$task_tmp/handwritten.c" "$task_tmp/project/src/business/route_profile.c"
test -z "$(find "$task_tmp/project/src/generated" -mindepth 1 -type d -print -quit)"
# Empty applications must produce compilable code too.
printf 'version(1);\n' > "$task_tmp/project/app.cfg"
./build/app-compiler build "$task_tmp/project"
"${CC:-cc}" "${flags[@]}" -fsyntax-only "$task_tmp/project/src/generated/app-routes.c"
echo 'PASS: duplicate-route diagnostics, preserved outputs, empty route set'
# Version recording uses the same generated-source destination and preserves it
# across a rejected attempt to rewrite an already recorded version.
./build/app-compiler version "$task_tmp/project"
cp "$task_tmp/project/src/generated/app-routes.c" "$task_tmp/recorded.c"
printf "version(1);\nroute('newRoute', '/new', (context) => {});\n" > "$task_tmp/project/app.cfg"
if ./build/app-compiler version "$task_tmp/project" > "$task_tmp/diagnostic" 2>&1; then
    echo 'Changed source unexpectedly rewrote a recorded version' >&2
    exit 1
fi
cmp "$task_tmp/recorded.c" "$task_tmp/project/src/generated/app-routes.c"
cmp "$task_tmp/handwritten.c" "$task_tmp/project/src/business/route_profile.c"
echo 'PASS: generated-source location, handwritten source preservation, version recording'
