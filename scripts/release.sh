#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Release pipeline. Run from the project root on a checked-out
# tagged commit:
#
#   git tag -s -m "Release 0.2.0" v0.2.0
#   ./scripts/release.sh
#
# Steps:
#   1. Verify the working tree is clean and HEAD is on an
#      annotated `vX.Y.Z` tag.
#   2. Verify the tag's version matches V8M_VERSION_STRING in the
#      public header — the two are easy to forget to bump together.
#   3. Run a release-mode build + the full ctest suite to confirm
#      the tagged tree is shippable.
#   4. Produce a signed source tarball (git archive + sha256 +
#      optional gpg signature).
#   5. If `gh` is available and `--gh-release` is passed, create a
#      draft GitHub release with the tarball attached. The script
#      stops short of `--publish` so a maintainer always inspects
#      the draft before flipping the visibility.
#
# All output lands under build/release-<tag>/.

set -euo pipefail

usage() {
	cat <<EOF
Usage: $0 [--gh-release] [--skip-tests]

Options:
  --gh-release   Create a draft GitHub release via gh after building
                 the tarball. Requires gh to be authenticated.
  --skip-tests   Skip the build + ctest step. Don't use for actual
                 releases — handy when iterating on the script.
  -h, --help     This message.
EOF
}

GH_RELEASE=0
SKIP_TESTS=0
while [ $# -gt 0 ]; do
	case "$1" in
	--gh-release) GH_RELEASE=1 ;;
	--skip-tests) SKIP_TESTS=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		usage >&2
		exit 1
		;;
	esac
	shift
done

# --- Step 1: clean working tree on an annotated tag ----------------
if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "release: working tree has uncommitted changes; aborting" >&2
	exit 1
fi

TAG=$(git describe --exact-match --tags HEAD 2>/dev/null || true)
if [ -z "$TAG" ]; then
	echo "release: HEAD is not on an annotated tag (run 'git tag' first)" >&2
	exit 1
fi
case "$TAG" in
v[0-9]*.[0-9]*.[0-9]*) ;;
*)
	echo "release: tag '$TAG' does not match vMAJOR.MINOR.PATCH" >&2
	exit 1
	;;
esac
TAG_VERSION="${TAG#v}"

# --- Step 2: header version matches the tag ------------------------
HEADER_VERSION=$(grep -E '^#define V8M_VERSION_STRING' \
	include/v8malloc/v8malloc.h | sed -E 's/.*"([^"]+)".*/\1/')
if [ "$HEADER_VERSION" != "$TAG_VERSION" ]; then
	echo "release: header V8M_VERSION_STRING ($HEADER_VERSION) does not match tag ($TAG_VERSION)" >&2
	exit 1
fi
echo "release: tag $TAG matches header version $HEADER_VERSION"

OUT="build/release-$TAG"
rm -rf "$OUT"
mkdir -p "$OUT"

# --- Step 3: release-mode build + ctest ----------------------------
if [ "$SKIP_TESTS" -eq 0 ]; then
	echo "release: building release tree + running ctest"
	cmake -S . -B "$OUT/build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DV8MALLOC_BUILD_TESTS=ON >/dev/null
	cmake --build "$OUT/build" --parallel >/dev/null
	ctest --test-dir "$OUT/build" --output-on-failure
fi

# --- Step 4: source tarball + sha256 + gpg signature ---------------
TARBALL="v8malloc-$TAG_VERSION.tar.gz"
echo "release: producing $OUT/$TARBALL"
git archive --format=tar.gz \
	--prefix="v8malloc-$TAG_VERSION/" \
	-o "$OUT/$TARBALL" "$TAG"

(
	cd "$OUT"
	sha256sum "$TARBALL" >"$TARBALL.sha256"
	if command -v gpg >/dev/null 2>&1 && \
		gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; then
		gpg --armor --detach-sign --output "$TARBALL.asc" "$TARBALL"
		echo "release: detached signature at $OUT/$TARBALL.asc"
	else
		echo "release: skipping GPG signature (no key available)"
	fi
)

# --- Step 5: optional GitHub draft release -------------------------
if [ "$GH_RELEASE" -eq 1 ]; then
	if ! command -v gh >/dev/null 2>&1; then
		echo "release: --gh-release requested but gh is not installed" >&2
		exit 1
	fi
	echo "release: creating draft GitHub release"
	NOTES="$(awk -v tag="$TAG_VERSION" '
		/^## \[/ {
			if (in_section) exit
			if ($0 ~ "\\[" tag "\\]") in_section = 1
			next
		}
		in_section { print }
	' CHANGELOG.md)"
	gh release create "$TAG" \
		--draft \
		--title "v8malloc $TAG_VERSION" \
		--notes "${NOTES:-See CHANGELOG.md.}" \
		"$OUT/$TARBALL" \
		"$OUT/$TARBALL.sha256" \
		${OUT}/${TARBALL}.asc 2>/dev/null || true
fi

echo "release: artifacts ready in $OUT/"
