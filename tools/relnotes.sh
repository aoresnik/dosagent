#!/bin/sh
# Prints the CHANGELOG.md section for a version (as printed by version.sh),
# without its heading, for use as GitHub release notes. dev-* versions get
# the Unreleased section, which may be empty; a release version must have a
# non-empty "## [<version>]" section or this fails.
ver=$1
case $ver in
dev-*) section=Unreleased ;;
*) section=$ver ;;
esac
file=$(dirname "$0")/../CHANGELOG.md
if ! grep -q "^## \[$section\]" "$file"; then
    echo "relnotes.sh: no '## [$section]' section in CHANGELOG.md" >&2
    exit 1
fi
# The section ends at the next heading or at the link references; leading
# and trailing blank lines are dropped.
notes=$(awk -v h="## [$section]" '
    /^## \[/ { if (on) exit; on = index($0, h) == 1; next }
    /^\[[^]]*\]: / { if (on) exit }
    on' "$file" | sed '/./,$!d')
if [ -z "$notes" ] && [ "$section" != Unreleased ]; then
    echo "relnotes.sh: the [$section] section in CHANGELOG.md is empty" >&2
    exit 1
fi
printf '%s\n' "$notes"
