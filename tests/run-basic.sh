#!/usr/bin/env sh
set -eu

assert_true() {
    if [ "$1" != "0" ]; then
        echo "$2" >&2
        exit 1
    fi
}

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exe_path=""
if [ -x "$repo_root/renamer" ]; then
    exe_path="$repo_root/renamer"
elif [ -x "$repo_root/renamer.exe" ]; then
    exe_path="$repo_root/renamer.exe"
else
    echo "renamer executable not found. Build first with make." >&2
    exit 1
fi

work_root="$repo_root/.tmp_test_run"
trap 'rm -rf "$work_root"' EXIT
rm -rf "$work_root"
mkdir -p "$work_root/regex" "$work_root/chain"
printf 'a\n' > "$work_root/regex/photo_001.jpg"
printf 'b\n' > "$work_root/regex/photo_002.jpg"
printf 'c\n' > "$work_root/regex/notes.txt"
printf 'a\n' > "$work_root/chain/A.jpg"
printf 'b\n' > "$work_root/chain/photo_001.jpg"

cd "$work_root"

regex_output=$($exe_path --dryrun --yes --regex '^photo_[0-9]{3}\.jpg$' regex batch_ 2)
printf '%s\n' "$regex_output" | grep -q 'Dry run: 2 file(s)'
assert_true "$?" "anchored regex matched an unexpected number of files"

set +e
invalid_output=$($exe_path --dryrun --yes --regex '*photo' regex batch_ 2 2>&1)
invalid_rc=$?
set -e
if [ "$invalid_rc" -eq 0 ]; then
    echo "invalid regex quantifier should fail" >&2
    exit 1
fi
printf '%s\n' "$invalid_output" | grep -q 'Invalid regex pattern'
assert_true "$?" "invalid regex error message missing"

$exe_path --yes chain .jpg photo_ 3 >/dev/null

ls chain | grep -q '^photo_001\.jpg$'
assert_true "$?" "chain rename did not produce photo_001.jpg"
ls chain | grep -q '^photo_002\.jpg$'
assert_true "$?" "chain rename did not produce photo_002.jpg"

$exe_path undo --yes >/dev/null

ls chain | grep -q '^A\.jpg$'
assert_true "$?" "undo did not restore A.jpg"
ls chain | grep -q '^photo_001\.jpg$'
assert_true "$?" "undo did not restore photo_001.jpg"

echo "All basic regression checks passed."
