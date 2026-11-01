#!/bin/bash
# Give the kernel's build ath10k's descriptions of the messages a WCN3990's firmware speaks (the OnePlus 6T),
# for bsd/dev/hakios.wlfw10.c, as tools/mk-wlfw.sh does ath11k's for redwood's WCN6750. They are ath10k's
# qmi_wlfw_v01.h and qmi_wlfw_v01.c as they are (ISC), written as one header into the kernel's build
# directory, where the compiler finds it and git does not. Without it hakios.wlfw10.c compiles to a line
# that says so.
#
# usage: tools/mk-wlfw10.sh        then build the kernel (./xnu-build.sh); again after a clean build
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
VERSION=v6.8
SRC=$ROOT/xnu-deps/build/ath10k-$VERSION
mkdir -p "$SRC"
for f in qmi_wlfw_v01.c qmi_wlfw_v01.h; do
	[ -s "$SRC/$f" ] || curl -sfL -o "$SRC/$f" \
		"https://raw.githubusercontent.com/torvalds/linux/$VERSION/drivers/net/wireless/ath/ath10k/$f"
	# every file must say itself that it may be used so: the licence is checked, not assumed
	head -3 "$SRC/$f" | grep -q "SPDX-License-Identifier: ISC" || { echo "mk-wlfw10.sh: $f is not ISC" >&2; exit 1; }
done
OUT=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw10.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
{
	echo "/* ath10k's qmi_wlfw_v01.h and qmi_wlfw_v01.c (Linux $VERSION, ISC), put together by tools/mk-wlfw10.sh:"
	echo " * not to be edited, not to be kept. */"
	cat <<'EOF'
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
EOF
	sed -e '/^#include/d' "$SRC/qmi_wlfw_v01.h"
	sed -e '/^#include/d' "$SRC/qmi_wlfw_v01.c"
} > "$OUT/hakios.wlfw_ath10k.h.new"
mv "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h"
echo "mk-wlfw10.sh: $OUT/hakios.wlfw_ath10k.h"
