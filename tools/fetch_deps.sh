#!/usr/bin/env bash
# Fetch the pinned reference dependencies listed in deps/deps.lock into
# $RCOMP_DEPS (default: ../deps next to the checkout). Never touches an
# existing checkout that is at a different commit or dirty: it reports and
# stops instead, so shared checkouts used by other agents are left alone.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
deps="${RCOMP_DEPS:-$(dirname "$root")/deps}"
mkdir -p "$deps"
status=0
while IFS='|' read -r name url pin license role; do
    [[ -z "$name" || "$name" == \#* || "$name" == */* ]] && continue
    case "$name" in
    XenonRecomp) continue ;;  # git submodule
    ps5-payload-sdk)
        want="${pin#sha256:}"
        dir="$deps/sdk"; zip="$dir/ps5-payload-sdk.zip"
        mkdir -p "$dir"
        [[ -f "$zip" ]] || curl -fsSL -o "$zip" "$url"
        echo "$want  $zip" | sha256sum --check --strict --quiet || { echo "HASH MISMATCH $zip" >&2; status=1; continue; }
        [[ -x "$dir/ps5-payload-sdk/bin/prospero-clang" ]] || (cd "$dir" && unzip -q -o ps5-payload-sdk.zip)
        echo "ok $name sha256:$want"
        continue ;;
    esac
    dir="$deps/$name"
    if [[ ! -d "$dir/.git" ]]; then
        if [[ "$name" == xenia ]]; then
            git clone -q --filter=blob:none --sparse "$url" "$dir"
            git -C "$dir" sparse-checkout set src/xenia/cpu/ppc/testing
        else
            git clone -q "$url" "$dir"
        fi
        git -C "$dir" checkout -q "$pin"
    fi
    have="$(git -C "$dir" rev-parse HEAD)"
    if [[ "$have" != "$pin" ]]; then
        echo "MISMATCH $name: have $have want $pin (not touching it)" >&2; status=1; continue
    fi
    dirty="$(git -C "$dir" status --porcelain | wc -l)"
    echo "ok $name $have dirty_files=$dirty"
done < "$root/deps/deps.lock"
exit $status
