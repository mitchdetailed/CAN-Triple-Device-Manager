#!/bin/sh
# Build cantriple-socketcan_<version>_all.deb into build/ beside this script.
#
#   sh build-deb.sh            (here, or by its path from anywhere)
#
# Needs dpkg-deb (dpkg) and gzip; no root. The tree is staged on a Linux
# filesystem, so the modes in the package are the ones set here whatever the
# checkout's filesystem says (on NTFS everything reads as 777).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=$(sed -n 's/^VERSION = "\(.*\)"$/\1/p' "$HERE/ctriple")
[ -n "$VERSION" ] || { echo "no VERSION in ctriple" >&2; exit 1; }
OUT="$HERE/build"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/cantriple-socketcan"
DOC="$ROOT/usr/share/doc/cantriple-socketcan"

install -d -m 755 "$ROOT/DEBIAN" "$ROOT/usr/bin" "$ROOT/usr/lib/udev/rules.d" \
    "$ROOT/usr/lib/cantriple-socketcan" "$ROOT/etc/default" "$DOC" \
    "$ROOT/usr/share/man/man1" "$ROOT/usr/share/man/man7"
install -m 755 "$HERE/ctriple" "$ROOT/usr/bin/ctriple"
install -m 755 "$HERE/ctriple-ifup" "$ROOT/usr/lib/cantriple-socketcan/ctriple-ifup"
install -m 644 "$HERE/70-cantriple-gs-usb.rules" "$ROOT/usr/lib/udev/rules.d/70-cantriple-gs-usb.rules"
install -m 644 "$HERE/cantriple-socketcan.default" "$ROOT/etc/default/cantriple-socketcan"
install -m 644 "$HERE/README.md" "$DOC/README.md"
install -m 644 "$HERE/debian/copyright" "$DOC/copyright"
gzip -9n -c "$HERE/debian/changelog" > "$DOC/changelog.gz"
chmod 644 "$DOC/changelog.gz"
gzip -9n -c "$HERE/ctriple.1" > "$ROOT/usr/share/man/man1/ctriple.1.gz"
gzip -9n -c "$HERE/cantriple-socketcan.7" > "$ROOT/usr/share/man/man7/cantriple-socketcan.7.gz"
chmod 644 "$ROOT/usr/share/man/man1/ctriple.1.gz" "$ROOT/usr/share/man/man7/cantriple-socketcan.7.gz"

install -m 755 "$HERE/debian/postinst" "$ROOT/DEBIAN/postinst"
install -m 755 "$HERE/debian/postrm" "$ROOT/DEBIAN/postrm"
install -m 644 "$HERE/debian/conffiles" "$ROOT/DEBIAN/conffiles"
SIZE=$(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
sed -e "s/@VERSION@/$VERSION/" -e "s/@SIZE@/$SIZE/" "$HERE/debian/control.in" > "$ROOT/DEBIAN/control"
chmod 644 "$ROOT/DEBIAN/control"
(cd "$ROOT" && find . -type f ! -path './DEBIAN/*' ! -path './etc/*' -printf '%P\0' \
    | sort -z | xargs -0 md5sum) > "$ROOT/DEBIAN/md5sums"
chmod 644 "$ROOT/DEBIAN/md5sums"

mkdir -p "$OUT"
DEB="$OUT/cantriple-socketcan_${VERSION}_all.deb"
dpkg-deb --root-owner-group --build "$ROOT" "$DEB" > /dev/null
echo "$DEB"
