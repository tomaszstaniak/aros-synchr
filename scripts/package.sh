#!/bin/bash
# Packs Synchr for one AROS target from existing builds
# (scripts/build.sh aros and scripts/build.sh aros gui, same target):
#   scripts/package.sh                (target from env.sh / local.env)
#   (. ../docs/vm-runs/pi-build.local.env; scripts/package.sh)
# Writes dist/Synchr-<version>[-r<revision>]-<archive name>.zip and adds
# its checksum to dist/SHA256SUMS. An archive that exists is never
# replaced: a change to the package alone (ReadMe, icons) raises the
# revision in packaging/REVISION, a change to the programs the version.
# SYNCHR_DIST=DIR writes a trial package to DIR instead.
set -euo pipefail
. "$(dirname "$0")/env.sh"
R=$ST_ROOT
version=$(sed -n 's/^#define GUI_VERSION "\([0-9.]*\)"/\1/p' "$R/gui/stgui.c")
[ -n "$version" ] || { echo "No version in gui/stgui.c" >&2; exit 1; }
case "$ST_TARGET" in
  x86_64-aros-v1) arch=aros-x86_64-abiv1-pc; requires="AROS x86_64 ABIv1 (mainline), a nightly from 2026-11-24 or later. On earlier nightlies use Synchr 0.2 revision 1." ;;
  x86_64-aros-v11) arch=aros-x86_64-abiv11-pc; requires="AROS x86_64 ABIv11 (an ABIv11 distribution)." ;;
  aarch64-aros-v1) arch=aros-aarch64-abiv1-raspi; requires="AROS Raspberry Pi AArch64 ABIv1 (mainline), a nightly from 2026-11-24 or later. On earlier nightlies use Synchr 0.2 revision 1." ;;
  *) echo "unknown target $ST_TARGET" >&2; exit 2 ;;
esac
b=$R/build/$ST_TARGET
for f in gui stpoc; do
  [ -x "$b/$f" ] || { echo "Run scripts/build.sh aros and scripts/build.sh aros gui for $ST_TARGET first" >&2; exit 1; }
done
# Both programs must come from the checked-in sources.
[ -z "$(git -C "$R" status --porcelain -- src gui)" ] || { echo "src/ or gui/ has uncommitted changes" >&2; exit 1; }
[ "$b/gui" -nt "$R/gui/stgui.c" ] && [ "$b/stpoc" -nt "$R/src/engine.c" ] || { echo "the builds are older than the sources" >&2; exit 1; }
rev=$(cat "$R/packaging/REVISION" 2>/dev/null || true)
case "$rev" in ""|0) rev= ;; *[!0-9]*) echo "packaging/REVISION is not a number" >&2; exit 1 ;; esac
name=Synchr-$version${rev:+-r$rev}-$arch
dist=${SYNCHR_DIST:-$R/dist}
out=$dist/$name.zip
[ ! -e "$out" ] || { echo "$out exists; delivered archives are not replaced (raise the version)" >&2; exit 1; }
work=$b/package
rm -rf "$work"
mkdir -p "$work/Synchr" "$dist"
cp "$b/gui" "$work/Synchr/Synchr"
cp "$b/stpoc" "$work/Synchr/SynchrEngine"
python3 "$R/scripts/make-icons.py" "$work/icons" > /dev/null
cp "$work/icons/Synchr.info" "$work/icons/ReadMe.info" "$work/Synchr/"
cp "$work/icons/drawer.info" "$work/Synchr.info"
rm -rf "$work/icons"
sed -e "s/@VERSION@/$version/" -e "s/@TARGET@/$arch/" -e "s|@REQUIRES@|$requires|" "$R/packaging/ReadMe.txt" > "$work/Synchr/ReadMe.txt"
cp "$R/LICENSE" "$work/Synchr/LICENSE.txt"
cp "$R/NOTICE" "$work/Synchr/NOTICE.txt"
# Nothing in the package names the machine it was built on. /home/vsts is
# the AROS project's own build service (inside the official Pi SDK).
found=$(LC_ALL=C grep -r -a -o -h -E "(/Users/|/Volumes/|/private/|/home/)[A-Za-z0-9_.-]*" "$work" | grep -v -x "/home/vsts" | sort -u || true)
if [ -n "$found" ]; then
  echo "host paths in the package:" $found >&2; exit 1
fi
# Every file carries the release date (GUI_DATE, dd.mm.yyyy), so that the
# same sources give the same archive.
stamp=$(sed -n 's/^#define GUI_DATE "\([0-9]*\)\.\([0-9]*\)\.\([0-9]*\)"/\3\2\10000/p' "$R/gui/stgui.c")
[ ${#stamp} = 12 ] || { echo "No release date in gui/stgui.c" >&2; exit 1; }
export TZ=UTC
find "$work" -exec touch -t "$stamp" {} +
(cd "$work" && zip -X -q -r "$out" Synchr.info Synchr)
(cd "$dist" && shasum -a 256 "$name.zip" >> SHA256SUMS)
unzip -l "$out"
echo "$out"
