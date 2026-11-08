#!/bin/sh
# Build the CD-i disc, start the proxy for one call, run cdiPC headless against it.
#   tools/run_headless.sh OUTDIR SECONDS [cdipc options...]
# Screenshots every 5 s in OUTDIR; the proxy's log in OUTDIR/proxy.log.
# CDIPC, BIOS and CDISDK may be set in the environment.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=$1; secs=$2; shift 2
CDIPC=${CDIPC:-/d/Homebrew6/cdiPC/build/cdipc.exe}
BIOS=${BIOS:-/d/Homebrew4/CD-i/CeDImu/cdi220b.rom}
CDISDK=${CDISDK:-/d/Homebrew4/CD-i/cdi-sdk}
PORT=${PORT:-6420}
mkdir -p "$out"
rm -f "$out"/*.png
if [ -z "$NOBUILD" ]; then
	(cd "$here/cdi" && docker run --rm -v "$(cd "$CDISDK" && pwd -W):/opt/cdi-sdk:ro" -v "$(pwd -W):/work" \
		cdi-toolchain:latest make clean all 2>&1 | grep -i "error\|warning\|stripped")
fi
[ -f "$here/cdi/disk/CDINET.CUE" ] || { echo "no disc: the build failed"; exit 1; }
python "$here/proxy/cdinet_proxy.py" --port "$PORT" --once > "$out/proxy.log" 2>&1 &
proxy=$!
sleep 1
"$CDIPC" --boot "$BIOS" "$here/cdi/disk/CDINET.CUE" --seconds "$secs" --click 575,330,4 \
	--serial "127.0.0.1:$PORT" --screenshot "$out/s.png" --shot-every 5 "$@" > "$out/cdipc.txt" 2>&1 || true
sleep 1
kill $proxy 2>/dev/null || true
cat "$out/proxy.log"
grep "fault" "$out/cdipc.txt" | awk -F'[][]' '$2+0 > 5.0' | head -5
grep "^clock" "$out/cdipc.txt"
