#!/bin/sh
# Prints "<version> <archive name>" for the current checkout.
#
# A release tag vX.Yq on HEAD (X and Y one digit each, q an optional
# lowercase qualifier such as a = alpha, b = beta) gives version X.Yq and
# DAGNTXYQ.ZIP. Untagged builds give dev-<commit> and DAGNT.ZIP.
# Pass a tag as $1 to preview a release build without tagging.
tag=${1:-$(git describe --tags --exact-match --match 'v*' 2>/dev/null)}
if [ -z "$tag" ]; then
    rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    git diff --quiet HEAD 2>/dev/null || rev=$rev-dirty
    echo "dev-$rev DAGNT.ZIP"
    exit 0
fi
case $tag in
v[0-9].[0-9] | v[0-9].[0-9][a-z]) ;;
*)
    echo "version.sh: tag '$tag' is not vX.Y or vX.Yq (e.g. v0.1a)" >&2
    exit 1
    ;;
esac
ver=${tag#v}
echo "$ver DAGNT$(echo "$ver" | tr -d . | tr a-z A-Z).ZIP"
