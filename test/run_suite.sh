#!/bin/bash
###############################################################################
# run_suite.sh — Run the whole test suite and report pass or fail
###############################################################################
#
# One command runs every test: each top-level test/tests/*.lua file goes
# through test/run_test.sh, 4 at a time, except the tests listed in
# test/suite_exclude.txt. A summary at the end gives each test's result and
# time, and the script exits 0 only if every test it ran passed. A change
# must pass this suite before it is merged.
#
# USAGE:
#   bash test/run_suite.sh                     # the whole suite
#   bash test/run_suite.sh test/tests/test_a.lua test/tests/test_b.lua
#                                              # only these tests (the
#                                              # exclusion list still applies)
#
# OPTIONS:
#   --timeout S     each test's time limit in seconds (default 1500). It is
#                   passed to run_test.sh as MGBA_TEST_TIMEOUT.
#   --exclude FILE  the exclusion list to use (default test/suite_exclude.txt)
#
# THE EXCLUSION LIST (test/suite_exclude.txt):
#   One test per line: its file name without ".lua", then, after spaces, a
#   one-line reason saying why the suite leaves it out. Blank lines and
#   lines starting with "#" are ignored. Every exclusion needs a reason, and
#   must name a test that exists (a top-level test/tests/*.lua file, or one
#   of the files named on the command line); a line that breaks either rule
#   stops the suite before it runs anything. Excluded tests are not run, and
#   the summary lists each one as SKIPPED with its reason.
#
# RUN FOLDERS:
#   run_test.sh gives every test run its own folder, mgba-test.XXXXXX in
#   $TMPDIR (default /tmp). The suite deletes a passing run's folder and
#   keeps a failing or timed-out run's folder, with everything run_test.sh
#   printed for that run saved in it as run_test.log; the summary gives its
#   path. Folders the suite's own runs didn't make are never touched.
#
# EXIT CODES:
#   0 = every test that ran passed
#   1 = a test failed or timed out, or no test ran at all
#   2 = the suite could not start (a bad option, a missing ROM or test file,
#       a bad line in the exclusion list)
#
###############################################################################

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# How many tests run at once. 4 keeps an 8-core machine busy with room to
# spare: some tests start emulators of their own.
JOBS=4
TIMEOUT_SECONDS=1500
EXCLUDE_FILE="$SCRIPT_DIR/suite_exclude.txt"
TESTS=()

# Stop before running anything, with exit code 2 (see EXIT CODES).
die() {
    echo "ERROR: $*" >&2
    exit 2
}

# --- Options ---

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout|--exclude)
            [ $# -ge 2 ] || die "$1 needs a value"
            case "$1" in
                --timeout) TIMEOUT_SECONDS="$2" ;;
                --exclude) EXCLUDE_FILE="$2" ;;
            esac
            shift 2
            ;;
        -h|--help)
            # The header comment above, from USAGE on, is the help text.
            sed -n '/^# USAGE:/,/^####/p' "$0" | sed '$d; s/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            die "unknown option: $1 (see --help)"
            ;;
        *)
            TESTS+=("$1")
            shift
            ;;
    esac
done

[[ "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "--timeout must be a whole number of seconds, got: $TIMEOUT_SECONDS"

# With no test files named, run every top-level test. Files in subfolders of
# test/tests/ (fixtures for the framework's own tests, Mt. Moon checks) are
# loaded by other tests, so they are not tests of their own.
if [ ${#TESTS[@]} -eq 0 ]; then
    TESTS=("$PROJECT_DIR"/test/tests/*.lua)
fi

# --- Prerequisites ---

# Checked here once, so a missing file stops the suite at once instead of
# failing every test the same way. (run_test.sh checks for mgba-headless
# before it makes a run folder, so a missing emulator fails each test
# without leaving anything behind.)
[ -f "$PROJECT_DIR/pokefirered.gba" ] \
    || die "ROM not found at $PROJECT_DIR/pokefirered.gba; build it with: make -j\$(nproc) firered"

# Each test is known by its file name without ".lua" (its "name"): in the
# exclusion list, in the summary, and in the scratch files below. So no two
# tests in one run may share a name.
#
# KNOWN[<name>] is set for every test of this run. (In bash,
# "${array[key]+set}" expands to "set" when the key exists and to nothing
# when it doesn't, which is how this script asks "is this key there?".)
declare -A KNOWN=()
for test in "${TESTS[@]}"; do
    [ -f "$test" ] || die "test script not found: $test"
    name="$(basename "$test" .lua)"
    [ -z "${KNOWN[$name]+set}" ] || die "two tests are both called $name; run them separately"
    KNOWN["$name"]=set
done

# --- The exclusion list ---

# EXCLUDED[<name>] = the reason that test is left out.
declare -A EXCLUDED=()

# True if test <name> is in the exclusion list.
is_excluded() {
    [ -n "${EXCLUDED[$1]+set}" ]
}

[ -f "$EXCLUDE_FILE" ] || die "exclusion list not found: $EXCLUDE_FILE"
line_no=0
while IFS= read -r line || [ -n "$line" ]; do
    line_no=$((line_no + 1))
    line="${line%$'\r'}"                      # tolerate Windows line endings
    line="${line#"${line%%[![:space:]]*}"}"   # drop leading spaces
    case "$line" in
        ''|'#'*) continue ;;
    esac
    name="${line%%[[:space:]]*}"              # the first word
    reason="${line#"$name"}"                  # everything after it,
    reason="${reason#"${reason%%[![:space:]]*}"}"  # without leading
    reason="${reason%"${reason##*[![:space:]]}"}"  # or trailing spaces
    where="$EXCLUDE_FILE line $line_no"
    # Every exclusion needs a reason: a test left out of the suite proves
    # nothing on any run, so the list must say why that is acceptable.
    [ -n "$reason" ] \
        || die "$where: '$name' has no reason. Every exclusion needs a one-line reason after the test name."
    # And it must name a real test, so the list never claims to leave out a
    # test that was renamed or deleted (or misspelt).
    [ -f "$PROJECT_DIR/test/tests/$name.lua" ] || [ -n "${KNOWN[$name]+set}" ] \
        || die "$where: '$name' is not a test (no test/tests/$name.lua). Give the file name without .lua, or remove the line."
    EXCLUDED["$name"]="$reason"
done < "$EXCLUDE_FILE"

# --- Running the tests ---

# The suite's own scratch folder: each test's output while it runs, and the
# result each one ends with. It is removed when the suite exits.
SUITE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/mgba-suite.XXXXXX")" || die "could not make a scratch folder"
trap 'rm -rf "$SUITE_TMP"' EXIT

# Ctrl-C (SIGINT) or a kill (SIGTERM) stops every test still running.
# Without this the emulators would keep running: a script's background jobs
# ignore Ctrl-C, and `timeout` moves itself into a process group of its own,
# out of reach of the terminal's signal. So the suite sends SIGTERM itself to
# every process under each running test: its run_test.sh, `timeout`, and
# mgba-headless (which shuts down cleanly on SIGTERM).
process_tree() {
    local child
    echo "$1"
    for child in $(pgrep -P "$1"); do process_tree "$child"; done
}
stop_all() {
    trap - INT TERM
    echo "Interrupted: stopping the tests still running." >&2
    local pids=() pid
    for pid in $(jobs -p); do pids+=($(process_tree "$pid")); done
    [ ${#pids[@]} -eq 0 ] || kill -TERM "${pids[@]}" 2>/dev/null
    exit 130
}
trap stop_all INT TERM

# One line of the progress list or the summary:
#   <RESULT> <name> <seconds> s  <note>
print_row() {
    local result="$1" name="$2" seconds="$3" note="${4:-}"
    printf '%-8s %-36s %5d s%s\n' "$result" "$name" "$seconds" "${note:+  $note}"
}

# Delete one test's run folder: the folder run_test.sh made with mktemp and
# printed as "Output: <folder>". The name is checked against mktemp's
# pattern, so nothing else can be deleted by mistake, and only folders this
# suite's own runs printed are ever passed here.
remove_run_dir() {
    case "$1" in
        */mgba-test.??????) [ -d "$1" ] && rm -rf -- "$1" ;;
    esac
}

# Run one test through run_test.sh. This runs in the background, one copy
# per test, while the loop below starts the others. It saves everything
# run_test.sh printed in $SUITE_TMP/<name>.log and its verdict in
# $SUITE_TMP/<name>.result as "<RESULT> <seconds> <run folder>", and prints
# one progress line when the test ends.
run_one() {
    local test="$1" name="$2"
    local log="$SUITE_TMP/$name.log"
    local start code seconds result dir
    start=$(date +%s)
    MGBA_TEST_TIMEOUT="$TIMEOUT_SECONDS" bash "$SCRIPT_DIR/run_test.sh" "$test" > "$log" 2>&1
    code=$?
    seconds=$(( $(date +%s) - start ))
    dir="$(sed -n 's/^Output: //p' "$log" | head -n 1)"

    # run_test.sh exits 0 for a pass and 124 for a timeout (see its EXIT
    # CODES). A timeout is told apart by that code, never by how long the
    # test took: the PC may sleep mid-run, which adds to the elapsed time but
    # not to the time `timeout` counts.
    case "$code" in
        0)   result=PASS ;;
        124) result=TIMEOUT ;;
        *)   result=FAIL ;;
    esac

    # A passing run's folder is deleted. A failing run's folder is kept, with
    # everything run_test.sh printed saved in it as run_test.log (the
    # "Scripting:" lines exist nowhere else), so it holds all there is to
    # know about the failure.
    if [ "$result" = PASS ]; then
        remove_run_dir "$dir"
    elif [ -d "$dir" ]; then
        mv -- "$log" "$dir/run_test.log"
    fi

    printf '%s %s %s\n' "$result" "$seconds" "$dir" > "$SUITE_TMP/$name.result"
    print_row "$result" "$name" "$seconds"
}

SUITE_START=$(date +%s)
to_run=0
for test in "${TESTS[@]}"; do
    is_excluded "$(basename "$test" .lua)" || to_run=$((to_run + 1))
done
echo "=== Test suite: $to_run to run, $(( ${#TESTS[@]} - to_run )) excluded, $JOBS at a time, timeout $TIMEOUT_SECONDS s each ==="

# Keep $JOBS tests running at once: once that many have started, wait for
# any one of them to end (`wait -n`) before starting the next.
running=0
for test in "${TESTS[@]}"; do
    name="$(basename "$test" .lua)"
    is_excluded "$name" && continue
    if [ "$running" -ge "$JOBS" ]; then
        wait -n
        running=$((running - 1))
    fi
    run_one "$test" "$name" &
    running=$((running + 1))
done
wait

# --- Summary ---

echo ""
echo "=== Summary ==="
passed=0
failed=0
timed_out=0
skipped=0
failures=()
for test in "${TESTS[@]}"; do
    name="$(basename "$test" .lua)"
    if is_excluded "$name"; then
        printf '%-8s %-36s %7s  %s\n' SKIPPED "$name" "" "${EXCLUDED[$name]}"
        skipped=$((skipped + 1))
        continue
    fi
    result=FAIL seconds=0 dir=""
    [ -f "$SUITE_TMP/$name.result" ] && read -r result seconds dir < "$SUITE_TMP/$name.result"
    case "$result" in
        PASS)
            print_row "$result" "$name" "$seconds"
            passed=$((passed + 1))
            ;;
        *)
            # A run that failed before run_test.sh made its folder (or that
            # left no result) has only its log, in the scratch folder that is
            # about to be removed, so its last line is shown instead.
            if [ -n "$dir" ]; then
                note="kept $dir"
            else
                note="no run folder; run_test.sh said: $(tail -n 1 "$SUITE_TMP/$name.log" 2>/dev/null)"
            fi
            print_row "$result" "$name" "$seconds" "$note"
            if [ "$result" = TIMEOUT ]; then
                timed_out=$((timed_out + 1))
            else
                failed=$((failed + 1))
            fi
            failures+=("$name")
            ;;
    esac
done

elapsed=$(( $(date +%s) - SUITE_START ))
echo ""
echo "$passed passed, $failed failed, $timed_out timed out, $skipped skipped"
printf 'Wall-clock time: %dm %02ds\n' $((elapsed / 60)) $((elapsed % 60))

if [ ${#failures[@]} -gt 0 ]; then
    echo "SUITE FAILED: ${failures[*]}"
    exit 1
fi
if [ "$passed" -eq 0 ]; then
    echo "SUITE FAILED: no test ran"
    exit 1
fi
echo "SUITE PASSED"
exit 0
