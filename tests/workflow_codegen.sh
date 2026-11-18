#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d "$PWD/build/.workflow-codegen-XXXXXX")
Cleanup() {
    find "$scratch" -type f -exec unlink {} \;
    find "$scratch" -depth -type d -exec rmdir {} \;
}
trap Cleanup EXIT
mkdir "$scratch/project"
printf 'version(1);\n' > "$scratch/project/app.cfg"
./build/app-compiler build "$scratch/project" > "$scratch/output"
flags=(-std=c11 -Wall -Wextra -Wpedantic -Werror -Iexternal/common -Iexternal/secure-backends -I"$scratch/project/src/generated")
"${CC:-cc}" "${flags[@]}" -fsyntax-only "$scratch/project/src/generated/"*.c
cp app.cfg "$scratch/project/app.cfg"
./build/app-compiler build "$scratch/project" > "$scratch/output"
"${CC:-cc}" "${flags[@]}" -fsyntax-only "$scratch/project/src/generated/"*.c
cp "$scratch/project/src/generated/app-business.c" "$scratch/saved.c"
cat >> "$scratch/project/app.cfg" <<'CFG'
route('unsupported', '/unsupported', (context) => { return true; });
CFG
if ./build/app-compiler build "$scratch/project" > "$scratch/output" 2>&1; then exit 1; fi
rg -q 'workflow subset' "$scratch/output"
cmp "$scratch/saved.c" "$scratch/project/src/generated/app-business.c"
cat > "$scratch/project/app.cfg" <<'CFG'
version(1);
route('testRoute', '/one', (context) => {});
route('test_route', '/two', (context) => {});
CFG
if ./build/app-compiler build "$scratch/project" > "$scratch/output" 2>&1; then exit 1; fi
rg -q 'route names collide' "$scratch/output"
cat > "$scratch/project/app.cfg" <<'CFG'
version(1);
collection('item', () => {
  field('displayName', { type: 'string' });
  field('display_name', { type: 'string' });
});
route('item', '/item/:id', (context) => {
  let item = query('getItem', { collection: 'item', where: { id: context.route.id } });
});
CFG
if ./build/app-compiler build "$scratch/project" > "$scratch/output" 2>&1; then exit 1; fi
rg -q 'field names collide' "$scratch/output"
echo 'PASS: workflow compilation, empty application, unsupported statements, normalized names, preserved outputs'
