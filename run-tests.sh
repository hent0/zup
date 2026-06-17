#!/usr/bin/env sh

# Test file format (tests/*.zupt):
#   --TEST--
#   short description
#   --FILE--
#   <zup source written to a temp file and fed to ./build/zup>
#   --EXPECT--          (optional)
#   <literal expected stdout>
#   --EXPECTERR--       (optional)
#   <literal expected stderr; if the section is absent, stderr must be empty>
#   --EXIT--            (optional, default 0)
#   <expected exit code>
#   --ARGS--            (optional)
#   <flags passed to zup, e.g. "--ast", "--tokenize -o -", "--ll -o -">
#   --SKIPIF--          (optional)
#   <zup flags; if `zup <flags> <file>` exits 0, the test is skipped.
#    First line of stdout becomes the skip reason.>
#   --PREPARE--         (optional)
#   <script piped to /bin/sh before the test runs; used to set up fixtures.>
#   --CLEANUP--         (optional)
#   <script piped to /bin/sh after the test runs, regardless of outcome.>
#
# Two modes:
#   * If --ARGS-- is present, zup is run as `zup <args> <file>` and its
#     own stdout/stderr/exit are checked (lexer, ast and ll dumps).
#   * Otherwise the source is compiled (`zup <file> -o <bin>`) and the
#     resulting binary is executed; its stdout/exit are checked.
#
# On FAIL, writes <test>.out and <test>.diff next to the test for inspection.
# On PASS, removes any stale artifacts from previous runs.

set -u

ROOT=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)

ZUP_BIN=${ZUP_BIN:-$ROOT/build/zup}
TESTS_DIR=${TESTS_DIR:-$ROOT/tests}

BOLD=
ITALIC=
BLUE=
YELLOW=
GREEN=
RED=
NC=

if test -t 1; then
    ncolors=$(tput colors 2>/dev/null || echo 0)
    if test -n "$ncolors" && test "$ncolors" -ge 8; then
        BOLD=$(tput bold)
        ITALIC=$(tput sitm)
        BLUE=$(tput setaf 4)
        YELLOW=$(tput setaf 3)
        GREEN=$(tput setaf 2)
        RED=$(tput setaf 1)
        NC=$(tput sgr0)
    fi
fi

if [ ! -x "$ZUP_BIN" ]; then
    echo "error: zup binary not found at $ZUP_BIN (build it first)" >&2
    exit 2
fi

FAIL_FIRST=false
ONLY_FAILURES=false
NO_DIFF=false

remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    remaining=$((remaining - 1))
    case "$arg" in
        --only-failures) ONLY_FAILURES=true ;;
        --ff)            FAIL_FIRST=true ;;
        --no-diff)       NO_DIFF=true ;;
        *)               set -- "$@" "$arg" ;;
    esac
done

extract_section() {
    awk -v sec="--$1--" '
        $0 == sec { flag = 1; next }
        /^--[A-Z_]+--$/ { flag = 0 }
        flag { print }
    ' "$2"
}

has_section() {
    grep -q "^--$1--\$" "$2"
}

pass=0
fail=0
skip=0

tmp_src=$(mktemp)
tmp_bin=$(mktemp)
tmp_expect=$(mktemp)
tmp_list=$(mktemp)
tmp_stdout=$(mktemp)
tmp_stderr=$(mktemp)
trap 'rm -f "$tmp_src" "$tmp_bin" "$tmp_bin.ll" "$tmp_expect" "$tmp_list" "$tmp_stdout" "$tmp_stderr"' EXIT INT TERM

if [ $# -gt 0 ]; then
    arg=$1
    [ ! -e "$arg" ] && [ -e "$TESTS_DIR/$arg" ] && arg=$TESTS_DIR/$arg
    [ ! -e "$arg" ] && [ -e "$TESTS_DIR/$arg.zupt" ] && arg=$TESTS_DIR/$arg.zupt

    if [ -d "$arg" ]; then
        find "$arg" -type f -name '*.zupt' | sort > "$tmp_list"
    elif [ -f "$arg" ]; then
        echo "$arg" > "$tmp_list"
    else
        echo "error: not found: $1" >&2
        exit 2
    fi
else
    find "$TESTS_DIR" -type f -name '*.zupt' | sort > "$tmp_list"
fi

if [ ! -s "$tmp_list" ]; then
    echo "no tests found"
    exit 1
fi

clean_root=${arg:-$TESTS_DIR}
[ -f "$clean_root" ] && clean_root=${clean_root%/*}
find "$clean_root" -type f \( -name '*.out' -o -name '*.diff' -o -name '*.err' \) -delete 2>/dev/null

prev_dir=
dir_header_printed=false
first_header=true

emit_header() {
    $dir_header_printed && return
    $first_header || echo
    printf "${BOLD}${BLUE}%s:${NC}\n" "$dir"
    dir_header_printed=true
    first_header=false
}

while IFS= read -r test; do
    rel=${test#"$TESTS_DIR"/}
    dir=${rel%/*}
    [ "$dir" = "$rel" ] && dir=.
    base=${rel##*/}

    if [ "$dir" != "$prev_dir" ]; then
        prev_dir=$dir
        dir_header_printed=false
    fi

    name=$(extract_section TEST "$test" | head -n1)
    args=$(extract_section ARGS "$test")
    expect=$(extract_section EXPECT "$test")
    expecterr=$(extract_section EXPECTERR "$test")
    skipif=$(extract_section SKIPIF "$test")
    prepare=$(extract_section PREPARE "$test")
    cleanup=$(extract_section CLEANUP "$test")
    exit_expect=$(extract_section EXIT "$test")
    [ -z "$exit_expect" ] && exit_expect=0

    has_section EXPECT "$test" && has_expect=true || has_expect=false
    has_section EXPECTERR "$test" && has_expecterr=true || has_expecterr=false
    has_section ARGS "$test" && has_args=true || has_args=false

    extract_section FILE "$test" > "$tmp_src"

    if [ -n "$skipif" ]; then
        # shellcheck disable=SC2086
        skip_out=$("$ZUP_BIN" $skipif "$tmp_src" 2>/dev/null)
        if [ $? -eq 0 ]; then
            reason=$(printf '%s' "$skip_out" | head -n1)
            $ONLY_FAILURES || { emit_header; printf "  ${YELLOW}SKIP${NC}  ${ITALIC}%s${NC} — %s${reason:+ (${reason})}\n" "$base" "$name"; }
            skip=$((skip + 1))
            continue
        fi
    fi

    if [ -n "$prepare" ]; then
        printf '%s\n' "$prepare" | sh >/dev/null 2>&1
    fi

    if $has_args; then
        # Compiler-output mode: check zup's own stdout/stderr/exit.
        # shellcheck disable=SC2086
        "$ZUP_BIN" $args "$tmp_src" >"$tmp_stdout" 2>"$tmp_stderr"
        status=$?
    else
        # Run mode: compile to a binary and execute it.
        if "$ZUP_BIN" "$tmp_src" -o "$tmp_bin" >"$tmp_stderr" 2>&1; then
            "$tmp_bin" >"$tmp_stdout" 2>/dev/null
            status=$?
        else
            status=$?
            : > "$tmp_stdout"   # nothing ran; compile errors are in tmp_stderr
        fi
    fi

    actual=$(cat "$tmp_stdout")
    actual_err=$(cat "$tmp_stderr")

    ok=true
    $has_expect && [ "$actual" != "$expect" ] && ok=false
    if $has_expecterr; then
        [ "$actual_err" != "$expecterr" ] && ok=false
    else
        # no --EXPECTERR-- section: stderr must be empty
        [ -n "$actual_err" ] && ok=false
    fi
    [ "$status" != "$exit_expect" ] && ok=false

    out_file=${test%.zupt}.out
    err_file=${test%.zupt}.err
    diff_file=${test%.zupt}.diff

    should_stop=false

    if $ok; then
        $ONLY_FAILURES || { emit_header; printf "  ${GREEN}PASS${NC}  ${ITALIC}%s${NC} — %s\n" "$base" "$name"; }
        rm -f "$out_file" "$err_file" "$diff_file"
        pass=$((pass + 1))
    else
        emit_header
        printf "  ${RED}FAIL${NC}  ${ITALIC}%s${NC} — %s\n" "$base" "$name"
        printf '%s\n' "$actual" > "$out_file"
        printf '%s\n' "$actual_err" > "$err_file"
        : > "$diff_file"
        if $has_expect && [ "$actual" != "$expect" ]; then
            printf '%s\n' "$expect" > "$tmp_expect"
            {
                echo "--- stdout"
                diff --color -u "$tmp_expect" "$out_file" 2>/dev/null || true
            } >> "$diff_file"
        fi
        if $has_expecterr && [ "$actual_err" != "$expecterr" ]; then
            printf '%s\n' "$expecterr" > "$tmp_expect"
            {
                echo "--- stderr"
                diff --color -u "$tmp_expect" "$err_file" 2>/dev/null || true
            } >> "$diff_file"
        elif ! $has_expecterr && [ -n "$actual_err" ]; then
            { echo "--- unexpected stderr (no --EXPECTERR-- section)"; cat "$err_file"; } >> "$diff_file"
        fi
        if [ "$status" != "$exit_expect" ]; then
            printf -- "--- exit: expected %s, got %s\n" "$exit_expect" "$status" >> "$diff_file"
        fi
        $NO_DIFF || sed 's/^/        /' "$diff_file"
        fail=$((fail + 1))
        $FAIL_FIRST && should_stop=true
    fi

    if [ -n "$cleanup" ]; then
        printf '%s\n' "$cleanup" | sh >/dev/null 2>&1
    fi

    $should_stop && break
done < "$tmp_list"

echo
echo "-----------------------------------------------------------------"
printf 'passed: %d  failed: %d  skipped: %d\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
