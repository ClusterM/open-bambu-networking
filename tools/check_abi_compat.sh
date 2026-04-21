#!/bin/sh
# ABI-compatibility checker for open-bambu-networking.
#
# Compares the Bambu Studio ABI surface we target against a baseline
# snapshot committed to tools/abi_snapshot/. The snapshot is just the
# upstream files at the tag recorded in tools/abi_snapshot/SOURCE_TAG.
# Refreshing the snapshot is an explicit, reviewed step (--refresh); CI
# runs the read-only check and fails if anything drifted.
#
# The checker runs five tests:
#
#   1. upstream vs snapshot: src/slic3r/Utils/bambu_networking.hpp
#      (Studio-side struct / enum / callback typedefs layout.)
#
#   2. upstream vs snapshot: src/slic3r/Utils/NetworkAgent.hpp
#      (`func_*` typedefs Studio casts `dlsym` results into. Catches
#       silent signature drift on existing ABI slots.)
#
#   3. upstream vs snapshot: sorted list of every bambu_network_*
#      symbol Studio's NetworkAgent::initialize_network_module()
#      resolves via dlsym. Catches new / removed ABI slots.
#
#   4. repo self-consistency: include/obn/bambu_networking.hpp against
#      the snapshot header (we vendor a verbatim copy; any drift here
#      means our plugin is compiled against a stale layout).
#
#   5. repo self-consistency: the hard-coded symbol list in
#      tests/probe_plugin.cpp against the snapshot (so the smoke test
#      keeps checking every symbol Studio will ever ask for).
#
# Optional: if --so=PATH is given, runs `nm` on that shared object and
# verifies every snapshot symbol is exported with type 'T' (defined,
# global, text). CI uses this to check the built libbambu_networking.so
# does not regress.
#
# Usage:
#   tools/check_abi_compat.sh                 # compare snapshot to newest
#                                             # tag found on github.com.
#   tools/check_abi_compat.sh --tag=v02.06.00.51
#   tools/check_abi_compat.sh --offline       # use 3rd_party/BambuStudio
#   tools/check_abi_compat.sh --so=build/libbambu_networking.so
#   tools/check_abi_compat.sh --refresh [--tag=...]
#                                             # overwrite the snapshot
#                                             # with upstream contents
#                                             # (then git diff / commit).

set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SNAP="$REPO/tools/abi_snapshot"
UPSTREAM_REPO="bambulab/BambuStudio"

MODE="compare"
TAG=""
OFFLINE=0
SO_PATH=""

usage() {
    sed -n '3,55p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
    case "$arg" in
        --tag=*)       TAG="${arg#*=}" ;;
        --offline)     OFFLINE=1 ;;
        --refresh)     MODE="refresh" ;;
        --so=*)        SO_PATH="${arg#*=}" ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "check_abi_compat: unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

# --- obtain upstream files at $TAG (or latest) ------------------------------

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

upstream_base=""
resolve_tag() {
    if [ -n "$TAG" ]; then
        echo "$TAG"
        return
    fi
    git ls-remote --tags --refs "https://github.com/$UPSTREAM_REPO.git" 'refs/tags/v*' 2>/dev/null \
        | awk -F/ '{print $NF}' \
        | sort -V \
        | tail -n1
}

fetch_upstream_file() {
    path="$1"; dst="$2"
    if [ "$OFFLINE" -eq 1 ]; then
        src="$REPO/3rd_party/BambuStudio/$path"
        if [ ! -f "$src" ]; then
            echo "check_abi_compat: --offline but $src does not exist" >&2
            exit 1
        fi
        cp "$src" "$dst"
    else
        url="https://raw.githubusercontent.com/$UPSTREAM_REPO/$TAG/$path"
        if ! curl -fsSL "$url" -o "$dst"; then
            echo "check_abi_compat: cannot fetch $url" >&2
            exit 1
        fi
    fi
}

if [ "$OFFLINE" -eq 1 ]; then
    if [ -z "$TAG" ]; then
        if [ -d "$REPO/3rd_party/BambuStudio/.git" ]; then
            TAG=$(cd "$REPO/3rd_party/BambuStudio" && \
                  git -c advice.detachedHead=false describe --tags HEAD 2>/dev/null \
                  || echo "HEAD")
        else
            TAG="local"
        fi
    fi
    upstream_base="offline ($REPO/3rd_party/BambuStudio)"
else
    [ -n "$TAG" ] || TAG=$(resolve_tag)
    if [ -z "$TAG" ]; then
        echo "check_abi_compat: could not resolve an upstream tag" >&2
        exit 1
    fi
    upstream_base="github.com/$UPSTREAM_REPO@$TAG"
fi

mkdir -p "$WORK/live"
fetch_upstream_file src/slic3r/Utils/bambu_networking.hpp "$WORK/live/bambu_networking.hpp"
fetch_upstream_file src/slic3r/Utils/NetworkAgent.hpp     "$WORK/live/NetworkAgent.hpp"
fetch_upstream_file src/slic3r/Utils/NetworkAgent.cpp     "$WORK/live/NetworkAgent.cpp"

grep -oE 'get_network_function\("bambu_network_[a-zA-Z_0-9]+"\)' "$WORK/live/NetworkAgent.cpp" \
    | sed -E 's/^get_network_function\("//; s/"\)$//' \
    | sort -u > "$WORK/live/symbols.txt"

# --- refresh mode -----------------------------------------------------------

if [ "$MODE" = "refresh" ]; then
    mkdir -p "$SNAP"
    cp "$WORK/live/bambu_networking.hpp" "$SNAP/bambu_networking.hpp"
    cp "$WORK/live/NetworkAgent.hpp"     "$SNAP/NetworkAgent.hpp"
    cp "$WORK/live/symbols.txt"          "$SNAP/symbols.txt"
    printf '%s\n' "$TAG" > "$SNAP/SOURCE_TAG"
    cat <<EOF
check_abi_compat: refreshed $SNAP/ from $upstream_base

Next steps:
    1. Update include/obn/bambu_networking.hpp to match the new snapshot.
    2. Update tests/probe_plugin.cpp if the symbol list changed.
    3. Re-run ./configure and make test to confirm everything still builds.
    4. Commit the snapshot + code changes together.
EOF
    exit 0
fi

# --- comparisons ------------------------------------------------------------

FAIL=0

echo "Comparing against $upstream_base"
echo "Snapshot tag:   $(cat "$SNAP/SOURCE_TAG" 2>/dev/null || echo '<none>')"
echo

report_diff() {
    label="$1"; expected="$2"; actual="$3"; hint="$4"
    if cmp -s "$expected" "$actual"; then
        printf '  [PASS] %s\n' "$label"
        return 0
    fi
    printf '  [FAIL] %s\n' "$label"
    printf '         %s\n' "$hint"
    echo   '         --- diff ---'
    diff -u "$expected" "$actual" | sed 's/^/         /'
    echo   '         ------------'
    FAIL=1
    return 1
}

report_missing() {
    # $1=label  $2=expected  $3=actual  $4=hint  $5=mode  ("exact"|"superset")
    label="$1"; expected="$2"; actual="$3"; hint="$4"; mode="${5:-exact}"
    missing=$(comm -23 "$expected" "$actual" || true)
    extra=$(comm -13 "$expected" "$actual" || true)
    if [ -z "$missing" ] && [ -z "$extra" ]; then
        printf '  [PASS] %s\n' "$label"
        return 0
    fi
    # In "superset" mode we only require actual to contain every expected
    # entry; additional entries in `actual` are a notice, not an error.
    if [ "$mode" = 'superset' ] && [ -z "$missing" ]; then
        printf '  [PASS] %s (with %d unreferenced extras)\n' \
            "$label" "$(echo "$extra" | grep -c .)"
        printf  '         note: these symbols are exported but Studio does not resolve them at %s:\n' "$TAG"
        printf  '           %s\n' $extra
        return 0
    fi
    printf '  [FAIL] %s\n' "$label"
    printf '         %s\n' "$hint"
    if [ -n "$missing" ]; then
        echo   '         missing (in snapshot, not in actual):'
        printf  '           %s\n' $missing
    fi
    if [ -n "$extra" ] && [ "$mode" != 'superset' ]; then
        echo   '         extra (in actual, not in snapshot):'
        printf  '           %s\n' $extra
    fi
    FAIL=1
    return 1
}

echo 'Upstream vs snapshot:'
report_diff 'bambu_networking.hpp' \
    "$SNAP/bambu_networking.hpp" "$WORK/live/bambu_networking.hpp" \
    "Upstream changed structs/enums/callback typedefs. Review the diff, port to include/obn/bambu_networking.hpp, then run tools/check_abi_compat.sh --refresh."
report_diff 'NetworkAgent.hpp (func_* typedef source)' \
    "$SNAP/NetworkAgent.hpp" "$WORK/live/NetworkAgent.hpp" \
    "Upstream changed a func_* typedef signature or added a new one. Re-check every matching plugin-side implementation, then run tools/check_abi_compat.sh --refresh."
report_missing 'bambu_network_* symbol list (NetworkAgent.cpp dlsym calls)' \
    "$SNAP/symbols.txt" "$WORK/live/symbols.txt" \
    "Upstream added / removed ABI slots. Implement missing ones in src/abi_*.cpp, add them to tests/probe_plugin.cpp, then --refresh."

echo
echo 'Repo self-consistency:'
report_diff 'include/obn/bambu_networking.hpp vs snapshot' \
    "$SNAP/bambu_networking.hpp" "$REPO/include/obn/bambu_networking.hpp" \
    "Our vendored header drifted from the snapshot. This means we'd build the plugin against a different struct layout than Studio expects. Resync include/obn/bambu_networking.hpp to tools/abi_snapshot/bambu_networking.hpp."

# Extract tests/probe_plugin.cpp's symbol list (strings inside the
# kBambuNetworkSymbols[] array).
awk '
    /kBambuNetworkSymbols\[\]/      { inside = 1; next }
    inside && /\};/                 { inside = 0; next }
    inside                          {
        while (match($0, /"bambu_network_[a-zA-Z0-9_]+"/)) {
            print substr($0, RSTART + 1, RLENGTH - 2)
            $0 = substr($0, RSTART + RLENGTH)
        }
    }
' "$REPO/tests/probe_plugin.cpp" | sort -u > "$WORK/probe_symbols.txt"

report_missing 'tests/probe_plugin.cpp symbol list' \
    "$SNAP/symbols.txt" "$WORK/probe_symbols.txt" \
    "The kBambuNetworkSymbols[] array in tests/probe_plugin.cpp is out of sync with the snapshot."

if [ -n "$SO_PATH" ]; then
    echo
    echo 'Built plugin:'
    if [ ! -f "$SO_PATH" ]; then
        printf '  [FAIL] %s does not exist; build the plugin first.\n' "$SO_PATH"
        FAIL=1
    else
        nm -D --defined-only "$SO_PATH" 2>/dev/null \
            | awk '$2=="T" || $2=="W" {print $3}' \
            | grep -E '^bambu_network_' \
            | sort -u > "$WORK/so_symbols.txt"
        report_missing "$(basename "$SO_PATH") exports every snapshot symbol" \
            "$SNAP/symbols.txt" "$WORK/so_symbols.txt" \
            "Some bambu_network_* symbols declared in the snapshot are NOT exported by the built .so. Studio would crash when dlsym returns NULL." \
            'superset'
    fi
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo 'ABI OK.'
    exit 0
fi

cat <<EOF
ABI drift detected.

To review and accept upstream changes (after porting them into the plugin):
    tools/check_abi_compat.sh --refresh $( [ -n "$TAG" ] && echo "--tag=$TAG" )

To bail out and pin the plugin to the snapshot tag instead:
    Put --tag=$(cat "$SNAP/SOURCE_TAG" 2>/dev/null || echo '<tag>') in the caller
    (in CI: .github/workflows/build.yml abi-compat job).
EOF
exit 1
