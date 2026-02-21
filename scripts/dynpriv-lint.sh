#!/bin/bash
#
# dynpriv-lint.sh — Lint dynamic privilege annotations for consistency.
#
# Checks:
#   1) Header __PRIVILEGED_CODE  →  source definition must also be __PRIVILEGED_CODE
#   2) Source __PRIVILEGED_CODE  →  header declaration must also be __PRIVILEGED_CODE
#   3) Header __PRIVILEGED_CODE  →  must have "@note Privilege: **required**" docstring
#
# Usage:
#   scripts/dynpriv-lint.sh            # scan kernel/
#   scripts/dynpriv-lint.sh kernel/mm  # scan only kernel/mm/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SCAN_DIR="${1:-$ROOT_DIR/kernel}"
KERNEL_DIR="$ROOT_DIR/kernel"

# Resolve relative paths
[[ "$SCAN_DIR" != /* ]] && SCAN_DIR="$ROOT_DIR/$SCAN_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

ERRORS=0

rel() { echo "${1#"$ROOT_DIR"/}"; }

error() {
    printf "  ${RED}error${NC} ${BOLD}%s:%d${NC}: %s\n" "$(rel "$1")" "$2" "$3"
    ((ERRORS++)) || true
}

# ────────────────────────────────────────────────────────────────────────────
# extract_priv_funcs FILE
#
# Print one line per __PRIVILEGED_CODE function found in FILE:
#   bare_name|qualified_name|line_number|is_inline|is_static
# ────────────────────────────────────────────────────────────────────────────
extract_priv_funcs() {
    awk '
    /__PRIVILEGED_CODE/ {
        # Skip if inside a block comment (continuation line starting with *)
        if ($0 ~ /^[[:space:]]*\*/) next
        if ($0 ~ /^[[:space:]]*\/\*/) next

        sig  = $0
        lnum = NR

        # Join with next line if signature is split across two lines
        if (index(sig, "(") == 0) {
            if ((getline nextline) > 0) {
                sig = sig " " nextline
            }
        }

        # Must be a function (has parenthesis) — skip data declarations
        if (index(sig, "(") == 0) next

        # Detect inline / static
        is_inline = (sig ~ /[[:space:]]inline([[:space:]]|$)/) ? 1 : 0
        is_static = 0
        if (!is_inline && sig ~ /(^|[[:space:]])static[[:space:]]/) is_static = 1

        # Get everything before the first "("
        paren = index(sig, "(")
        prefix = substr(sig, 1, paren - 1)
        gsub(/[[:space:]]+$/, "", prefix)

        # Last whitespace-delimited token is the (possibly qualified) name
        n = split(prefix, tok, /[[:space:]]+/)
        if (n == 0) next
        qname = tok[n]
        gsub(/^[*&]+/, "", qname)           # strip pointer/ref from return type

        # Bare name: strip Class:: or ns:: qualifiers
        bare = qname
        if (index(qname, "::") > 0) {
            split(qname, parts, "::")
            bare = parts[length(parts)]
        }

        if (bare == "" || bare !~ /^[a-zA-Z_~]/) next

        printf "%s|%s|%d|%d|%d\n", bare, qname, lnum, is_inline, is_static
    }
    ' "$1"
}

# ────────────────────────────────────────────────────────────────────────────
# check_docstring FILE LINENO
#
# Return 0 if the __PRIVILEGED_CODE at LINENO is preceded by a doc-comment
# containing "@note Privilege: **required**".
# ────────────────────────────────────────────────────────────────────────────
check_docstring() {
    awk -v target="$2" '
    { lines[NR] = $0 }
    END {
        i = target - 1

        # Skip blank lines and template<...> lines between comment and marker
        while (i >= 1 && (lines[i] ~ /^[[:space:]]*$/ || lines[i] ~ /^[[:space:]]*template[[:space:]]*</)) i--

        # Expect closing "*/"
        if (i < 1 || lines[i] !~ /\*\//) exit 1

        found = 0
        while (i >= 1) {
            if (lines[i] ~ /@note[[:space:]]+Privilege:.*\*\*required\*\*/)
                found = 1
            if (lines[i] ~ /\/\*/)
                break
            i--
        }
        exit (found ? 0 : 1)
    }
    ' "$1"
}

# ────────────────────────────────────────────────────────────────────────────
# find_unpriv_definition FILE FUNC_NAME MAX_INDENT
#
# Print "lineno|content" for every definition/declaration of FUNC_NAME in
# FILE that is NOT marked __PRIVILEGED_CODE (checking the preceding line too,
# for the split-line pattern).
#
# MAX_INDENT controls how deep to look:
#   0  = only top-level (function definitions at namespace scope in .cpp)
#   4  = include class members (declarations in .h)
# ────────────────────────────────────────────────────────────────────────────
find_unpriv_definition() {
    local file="$1" fname="$2" max_indent="${3:-0}"

    awk -v fname="$fname" -v maxind="$max_indent" '
    BEGIN {
        pat = "(^|[^a-zA-Z0-9_])" fname "[[:space:]]*\\("
    }
    {
        prev = cur
        cur  = $0
    }
    $0 ~ pat {
        # Skip comment lines
        if ($0 ~ /^[[:space:]]*\/\//) next
        if ($0 ~ /^[[:space:]]*\*/)  next

        # Check indentation — definitions are at top level
        match($0, /^[[:space:]]*/); indent = RLENGTH
        if (indent > maxind) next

        # Skip if this line or the preceding line has __PRIVILEGED_CODE
        if ($0   ~ /__PRIVILEGED_CODE/) next
        if (prev ~ /__PRIVILEGED_CODE/) next

        printf "%d|%s\n", NR, $0
    }
    ' "$file"
}

# ────────────────────────────────────────────────────────────────────────────
# find_sources_for_header HEADER
#
# Print .cpp files that correspond to the given header:
#   1) Same basename in the same directory
#   2) Same basename in architecture subdirectories
#   3) .cpp files that #include this header (by basename)
# Deduplicated.
# ────────────────────────────────────────────────────────────────────────────
find_sources_for_header() {
    local header="$1"
    local base
    base=$(basename "$header" .h)

    {
        # Same directory sibling
        local sibling="${header%.h}.cpp"
        [[ -f "$sibling" ]] && echo "$sibling"

        # Same basename under arch directories
        find "$KERNEL_DIR" -name "${base}.cpp" -type f 2>/dev/null
    } | sort -u
}

# ────────────────────────────────────────────────────────────────────────────
# find_primary_header SOURCE
#
# Find the header that matches this source by basename (same base, .h).
# Returns up to one result (the most specific match).
# ────────────────────────────────────────────────────────────────────────────
find_primary_header() {
    local source="$1"
    local base
    base=$(basename "$source" .cpp)

    # Check same directory first
    local sibling="${source%.cpp}.h"
    if [[ -f "$sibling" ]]; then
        echo "$sibling"
        return
    fi

    # Search upward and in common directories
    find "$KERNEL_DIR" -name "${base}.h" -type f 2>/dev/null | head -5
}

# ════════════════════════════════════════════════════════════════════════════
# Main
# ════════════════════════════════════════════════════════════════════════════

printf "${BOLD}dynpriv-lint${NC}: scanning ${DIM}%s${NC}\n\n" "$(rel "$SCAN_DIR")"

# Collect files containing __PRIVILEGED_CODE
mapfile -t HEADERS < <(grep -rl --include='*.h'   '__PRIVILEGED_CODE' "$SCAN_DIR" 2>/dev/null || true)
mapfile -t SOURCES < <(grep -rl --include='*.cpp'  '__PRIVILEGED_CODE' "$SCAN_DIR" 2>/dev/null || true)

printf "  Found ${BOLD}%d${NC} header(s) and ${BOLD}%d${NC} source(s) with privilege annotations.\n\n" \
    "${#HEADERS[@]}" "${#SOURCES[@]}"

# ── Check 3: Docstring ────────────────────────────────────────────────────
printf "${BOLD}[3] Docstring: @note Privilege: **required**${NC}\n"
check3_hits=0

for hdr in "${HEADERS[@]}"; do
    while IFS='|' read -r fname _ lineno _ _; do
        [[ -z "$fname" ]] && continue
        if ! check_docstring "$hdr" "$lineno"; then
            error "$hdr" "$lineno" \
                "'${fname}' is __PRIVILEGED_CODE but missing '@note Privilege: **required**' docstring"
            ((check3_hits++)) || true
        fi
    done < <(extract_priv_funcs "$hdr")
done

(( check3_hits == 0 )) && printf "  ${GREEN}OK${NC}\n"
echo

# ── Check 1: Header privileged → source privileged ────────────────────────
printf "${BOLD}[1] Header privileged → source must be privileged${NC}\n"
check1_hits=0

for hdr in "${HEADERS[@]}"; do
    mapfile -t srcs < <(find_sources_for_header "$hdr")
    [[ ${#srcs[@]} -eq 0 ]] && continue

    while IFS='|' read -r fname qname lineno is_inline _; do
        [[ -z "$fname" ]] && continue
        # Inline functions live entirely in the header — skip
        [[ "$is_inline" == "1" ]] && continue

        for src in "${srcs[@]}"; do
            # maxind=0: only top-level definitions (not calls inside function bodies)
            while IFS='|' read -r sline _; do
                [[ -z "$sline" ]] && continue
                error "$src" "$sline" \
                    "'${fname}' is __PRIVILEGED_CODE in $(rel "$hdr"):${lineno} but NOT in source definition"
                ((check1_hits++)) || true
            done < <(find_unpriv_definition "$src" "$fname" 0)
        done
    done < <(extract_priv_funcs "$hdr")
done

(( check1_hits == 0 )) && printf "  ${GREEN}OK${NC}\n"
echo

# ── Check 2: Source privileged → header privileged ────────────────────────
printf "${BOLD}[2] Source privileged → header must be privileged${NC}\n"
check2_hits=0

for src in "${SOURCES[@]}"; do
    # Use basename-matched headers to avoid cross-namespace false positives
    mapfile -t hdrs < <(find_primary_header "$src")
    [[ ${#hdrs[@]} -eq 0 ]] && continue

    while IFS='|' read -r fname qname lineno _ is_static; do
        [[ -z "$fname" ]] && continue
        # File-local static functions have no header declaration — skip
        [[ "$is_static" == "1" ]] && continue

        for hdr in "${hdrs[@]}"; do
            # maxind=4: declarations can be inside class bodies
            while IFS='|' read -r hline _; do
                [[ -z "$hline" ]] && continue
                error "$hdr" "$hline" \
                    "'${fname}' is __PRIVILEGED_CODE in $(rel "$src"):${lineno} but NOT in header declaration"
                ((check2_hits++)) || true
            done < <(find_unpriv_definition "$hdr" "$fname" 4)
        done
    done < <(extract_priv_funcs "$src")
done

(( check2_hits == 0 )) && printf "  ${GREEN}OK${NC}\n"
echo

# ── Summary ───────────────────────────────────────────────────────────────
if [[ $ERRORS -eq 0 ]]; then
    printf "${GREEN}${BOLD}✓ All checks passed.${NC}\n"
else
    printf "${RED}${BOLD}✗ ${ERRORS} error(s) found.${NC}\n"
fi

exit $(( ERRORS > 0 ? 1 : 0 ))
