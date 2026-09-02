#!/bin/bash
# Resolve the pseudo-random seed shared by every test in a run.
# Usage: ./seed.sh                print the effective seed (empty if none is set)
#        ./seed.sh --write FILE   pick a seed and write it to FILE
# Sources, in order: ECONOPET_TEST_SEED, then the file named by ECONOPET_TEST_SEED_FILE.
# See docs/dev/testing.md.

# Simulators accept 1..INT_MAX.
check_seed() {
    if ! [[ "$1" =~ ^[1-9][0-9]{0,9}$ ]] || [ "$1" -gt 2147483647 ]; then
        echo "Test seed must be an integer in 1..2147483647, got '$1'" >&2
        return 1
    fi
}

case "$1" in
    --write)
        SEED_FILE="$2"
        if [ -z "$SEED_FILE" ]; then
            echo "Usage: $0 --write FILE" >&2
            exit 1
        fi

        # The ECONOPET_TEST_SEED env var overrides the randomly generated seed.
        # If set, write ECONOPET_TEST_SEED to the file so that the seed file matches
        # the value used for the run.
        SEED="${ECONOPET_TEST_SEED:-$(( ($(od -An -N4 -tu4 /dev/urandom) % 2147483647) + 1 ))}"
        check_seed "$SEED" || exit 1
        echo "$SEED" > "$SEED_FILE" || exit 1
        echo "Test seed: $SEED"
        ;;
    "")
        # Prefer the explicit override (${VAR:-} is empty when VAR is unset or empty.)
        SEED="${ECONOPET_TEST_SEED:-}"

        # With no override, read the seed shared by the current CTest run.
        if [ -z "$SEED" ] && [ -n "${ECONOPET_TEST_SEED_FILE:-}" ]; then
            if [ ! -r "$ECONOPET_TEST_SEED_FILE" ]; then
                echo "Cannot read ECONOPET_TEST_SEED_FILE: $ECONOPET_TEST_SEED_FILE" >&2
                exit 1
            fi
            # $(<FILE) reads the file contents without starting another process.
            SEED="$(<"$ECONOPET_TEST_SEED_FILE")"
        fi

        # Validate the seed (if one was supplied).  No seed is valid for direct runs.
        if [ -n "$SEED" ]; then
            check_seed "$SEED" || exit 1
        fi

        # Callers capture this output to read the seed.  Do not print anything else to stdout.
        echo "$SEED"
        ;;
    *)
        echo "Usage: $0 [--write FILE]" >&2
        exit 1
        ;;
esac
