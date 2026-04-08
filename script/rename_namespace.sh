#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  script/rename_namespace.sh [--apply] [--old <name>] [--new <name>]

Description:
  Rename namespace/token occurrences across layer1 source files.
  Default rename: async_uv -> flux
  Default mode: dry-run (only prints files that would change)

Options:
  --apply         Apply changes in place
  --old <name>    Old namespace/token (default: async_uv)
  --new <name>    New namespace/token (default: flux)
  -h, --help      Show this help

Notes:
  - This script only touches tracked files (git ls-files).
  - Excludes: layer2/, layer3/, build/, build_*/, .git/
EOF
}

apply=0
old_ns="async_uv"
new_ns="flux"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply)
            apply=1
            shift
            ;;
        --old)
            old_ns="${2:-}"
            shift 2
            ;;
        --new)
            new_ns="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$old_ns" || -z "$new_ns" ]]; then
    echo "old/new namespace must be non-empty." >&2
    exit 1
fi

if [[ "$old_ns" == "$new_ns" ]]; then
    echo "old and new namespace are identical, nothing to do."
    exit 0
fi

is_excluded() {
    local path="$1"
    case "$path" in
        layer2/*|layer3/*|build/*|build_*/*|.git/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_probably_text() {
    local file="$1"
    LC_ALL=C grep -Iq . "$file" 2>/dev/null
}

changed=0
changed_files=()

while IFS= read -r -d '' file; do
    [[ -f "$file" ]] || continue
    is_excluded "$file" && continue
    is_probably_text "$file" || continue

    tmp_file="$(mktemp)"
    OLD_NS="$old_ns" NEW_NS="$new_ns" \
        perl -0pe 's/\b\Q$ENV{OLD_NS}\E\b/$ENV{NEW_NS}/g' "$file" >"$tmp_file"

    if ! cmp -s "$file" "$tmp_file"; then
        changed=$((changed + 1))
        changed_files+=("$file")
        if [[ $apply -eq 1 ]]; then
            mv "$tmp_file" "$file"
        else
            rm -f "$tmp_file"
        fi
    else
        rm -f "$tmp_file"
    fi
done < <(git ls-files -z)

mode="DRY-RUN"
if [[ $apply -eq 1 ]]; then
    mode="APPLY"
fi

echo "[$mode] rename token: ${old_ns} -> ${new_ns}"
echo "[$mode] changed files: $changed"
if [[ $changed -gt 0 ]]; then
    printf '%s\n' "${changed_files[@]}"
fi

