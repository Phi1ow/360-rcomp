#!/usr/bin/env bash
# Builds the disc-image extractor used by tools/iso_extract.py from its pinned
# source (deps/deps.lock: extract-xiso/source). Host tool only, never vendored:
# source and binary stay under build/catalog-tools/.
#   tools/build_extract_xiso.sh      (needs git and a C compiler; Cygwin or POSIX)
# RCOMP_GIT selects the git executable (a Windows git when Cygwin's lacks CA roots).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
commit=3f5b62cfe68f000b0e3c8a30104973f3a297948e
license_sha=4a7802fdcbc4781a2f9f70be9ed8b65f196393188b71347df309c4d9ead03e32
git="${RCOMP_GIT:-git}"
out="$root/build/catalog-tools"
src="$out/extract-xiso-src"
# A Windows git needs Windows paths.
gsrc="$src"; [[ -n ${RCOMP_GIT:-} ]] && command -v cygpath >/dev/null && gsrc="$(cygpath -w "$src")"
mkdir -p "$out/extract-xiso"
if [[ ! -d $src/.git ]]; then
  "$git" init -q "$gsrc"
  "$git" -C "$gsrc" remote add origin https://github.com/XboxDev/extract-xiso.git
fi
[[ "$("$git" -C "$gsrc" rev-parse -q --verify HEAD 2>/dev/null || true)" == "$commit" ]] || {
  "$git" -C "$gsrc" fetch -q --depth 1 origin "$commit"
  "$git" -C "$gsrc" checkout -q --detach FETCH_HEAD
}
[[ "$("$git" -C "$gsrc" rev-parse HEAD)" == "$commit" ]] || { echo "extract-xiso: wrong commit" >&2; exit 1; }
# The pinned hash is that of LICENSE.TXT with CRLF line endings, as Git for Windows checks it out (autocrlf). Hash it in that
# form, so the check does not depend on which git fetched the source (a Cygwin git leaves LF and used to fail here).
[[ "$(sed -e 's/\r$//' -e 's/$/\r/' "$src/LICENSE.TXT" | sha256sum | cut -c1-64)" == "$license_sha" ]] || { echo "extract-xiso: LICENSE.TXT changed" >&2; exit 1; }
# The source has no Cygwin branch; its Darwin branch is the plain POSIX one
# (O_RDONLY, 64-bit off_t, '/' separators). Only the banner names macos-x.
case "$(uname -s)" in
  CYGWIN*|Darwin*) target=-D__DARWIN__ ;;
  Linux*) target=-D__LINUX__ ;;
  *) echo "extract-xiso: unsupported host $(uname -s)" >&2; exit 1 ;;
esac
${CC:-gcc} -O2 "$target" -o "$out/extract-xiso/extract-xiso.exe" "$src/extract-xiso.c"
cp "$src/LICENSE.TXT" "$out/extract-xiso/LICENSE.TXT"
sha256sum "$out/extract-xiso/extract-xiso.exe"
