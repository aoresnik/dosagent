#!/bin/sh
# Prints "<version> <archive name>" for the current checkout.
#
# A release tag vX.Y<qualifier> on HEAD (X and Y one digit each, the
# qualifier optional: lowercase letters, digits, '.' and '-', starting with
# a letter, e.g. a, alpha, beta2, rc1) gives version X.Y<qualifier> and
# DAGNTXYQ.ZIP, where Q is the qualifier's first letter, so the name fits
# DOS's 8.3 limit. Untagged builds give dev-<commit> and DAGNT.ZIP.
# Pass a tag as $1 to preview a release build without tagging.
tag=${1:-$(git describe --tags --exact-match --match 'v*' 2>/dev/null)}
if [ -z "$tag" ]; then
    rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    git diff --quiet HEAD 2>/dev/null || rev=$rev-dirty
    echo "dev-$rev DAGNT.ZIP"
    exit 0
fi
bad_tag() {
    echo "version.sh: tag '$tag' is not vX.Y or vX.Y<qualifier>" \
         "(e.g. v0.1alpha)" >&2
    exit 1
}
case $tag in
v[0-9].[0-9]*) qual=${tag#v[0-9].[0-9]} ;;
*) bad_tag ;;
esac
case $qual in
*[!a-z0-9.-]*) bad_tag ;;
"" | [a-z]*) ;;
*) bad_tag ;;
esac
ver=${tag#v}
short=$(echo "$ver" | cut -c1,3,4 | tr a-z A-Z)
echo "$ver DAGNT$short.ZIP"
