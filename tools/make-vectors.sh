#!/usr/bin/env bash
# Encodes schema/samples/*.xml with the rexi CLI into testdata/vectors/*.exi:
# the independent reference both libjlproto (C) and JlServ (Julia) must match.
# Options must match jlp_ctx_create / JlServ: strict, byte-aligned,
# valueMaxLength 128, options out of band.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
# The rexi CLI: $REXI, or the one `cmake --build build --target rexi-cli` makes.
rexi=${REXI:-$root/build/rexi-target/release/rexi}
[[ -x $rexi ]] || { echo "no rexi CLI at $rexi: run cmake --build build --target rexi-cli, or set REXI" >&2; exit 1; }
out=$root/testdata/vectors
mkdir -p "$out"
for f in "$root"/schema/samples/*.xml; do
    b=$(basename "$f" .xml)
    "$rexi" encode --xsd "$root/schema/jlp2.xsd" --alignment byte-aligned --value-max-length 128 "$f" "$out/$b.exi"
done
echo "vectors: $(ls "$out" | wc -l) files in testdata/vectors"
