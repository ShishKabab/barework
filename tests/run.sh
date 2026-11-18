#!/usr/bin/env bash
set -euo pipefail
infra=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$infra/build"
scratch=$(mktemp -d "$infra/build/.regression-XXXXXX")
Cleanup() {
    find "$scratch" -type l -exec unlink {} \;
    find "$scratch" -type f -exec unlink {} \;
    find "$scratch" -depth -type d -exec rmdir {} \;
}
trap Cleanup EXIT
fixture=${1:-$infra/tests/fixtures/news-platform}
mkdir -p "$scratch/src"
cp "$fixture/app.cfg" "$scratch/app.cfg"
cp -a "$fixture/src/business" "$scratch/src/"
mkdir -p "$scratch/build" "$scratch/src/generated"
ln -s "$infra/external" "$scratch/external"
ln -s "$infra/tests" "$scratch/tests"
for path in "$infra"/src/*; do ln -s "$path" "$scratch/src/$(basename "$path")"; done
for cache in liburing wasmtime wasi-toolchain; do ln -s "$infra/build/$cache" "$scratch/build/$cache"; done
cat > "$scratch/build.sh" <<'BUILD'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
"$BAREWORK_ROOT/build.sh" "$PWD"
ln -sfn server build/news-platform
BUILD
chmod +x "$scratch/build.sh"
export BAREWORK_ROOT="$infra"
cd "$scratch"
./tests/suite.sh
