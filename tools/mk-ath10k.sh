#!/bin/bash
# Build the driver of the OnePlus 6T's Wi-Fi radio for our kernel: ath10k, Qualcomm's driver for the WCN3990
# (its snoc back end: the radio on the SoC's own bus, its firmware a domain of the modem's), as it is in Linux,
# unmodified, on the layer tools/mk-ath11k.sh builds redwood's ath11k on (see there): Linux's ath10k and the
# Atheros drivers' common files (ISC), FreeBSD's LinuxKPI (that script's tree, reused), and ours, wifi/lx.
#
# Not all of the driver is built yet: DRIVER below lists the files that are. What they call in the files
# that are not becomes a function that says its name in the log and answers 0 (out/stubs.txt), unless the
# glue has one that does more.
#
# What comes out, in xnu-deps/build/ath10k:
#   src/ath10k/, src/*.[ch]   the driver's files and the common ones, untouched
#   out/ath10k.o              the object for the kernel        out/stubs.txt   what is not there yet
#
# usage: tools/mk-ath10k.sh [compile]        "compile" stops after compiling (no object: the glue is not done)
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B11=$ROOT/xnu-deps/build/ath11k
B=$ROOT/xnu-deps/build/ath10k
VERSION=7.0.14
ATH=drivers/net/wireless/ath
[ -d "$B11/freebsd/sys" ] && [ -d "$B11/gen" ] || "$ROOT/tools/mk-ath11k.sh"
mkdir -p "$B"
cd "$B"
if [ "$(cat src/.release 2>/dev/null)" != "$VERSION" ]; then
	rm -rf src unpacked
	mkdir unpacked src
	COMMON="ath.h regd.h regd.c regd_common.h main.c key.c spectral_common.h dfs_pattern_detector.h hw.c trace.h"
	tar xJf "$B11/linux-$VERSION.tar.xz" -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath10k \
		$(for f in $COMMON; do echo linux-$VERSION/$ATH/$f; done)
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in unpacked/$ATH/ath10k/*.[ch] $(for f in $COMMON; do echo unpacked/$ATH/$f; done); do
		case "$(head -6 "$f")" in
		*"SPDX-License-Identifier: ISC"*|*"Permission to use, copy, modify"*) ;;
		*) echo "mk-ath10k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mv unpacked/$ATH/ath10k src/ath10k
	for f in $COMMON; do mv unpacked/$ATH/$f src/; done
	rm -rf unpacked
	echo "$VERSION" > src/.release
fi
W=$ROOT/wifi/lx
S=$B11/freebsd/sys

# --- compile: the driver's files built so far, the common ones, and the glue
DRIVER="core mac wmi wmi-tlv htt htt_rx htt_tx htc ce hw txrx p2p qmi qmi_wlfw_v01 snoc debug"
COMMON="regd main"
GLUE=""
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -I$B11/gen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/ath10k -Isrc -include $W/include/lx_prelude.h -DCONFIG_ATH10K_SNOC -DCONFIG_ATH10K_DEBUG
	-DKBUILD_MODNAME=\"ath10k\""
source "$ROOT/xnu-env.sh"
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/hakios.$h.h" ] || { echo "mk-ath10k.sh: the kernel tree has no bsd/sys/hakios.$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\" -DLX_HAKIOS_QMI_H=\"$XNU_SRC/bsd/sys/hakios.qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile src/ath10k/$f.c obj/ath10k_$f.o $DFLAGS; done
for f in $COMMON; do compile src/$f.c obj/ath_$f.o $DFLAGS; done
for f in $GLUE; do compile "$W/$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src; done
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
[ "$1" = compile ] && { echo "mk-ath10k.sh: all compiled"; exit 0; }
echo "mk-ath10k.sh: the glue and the object are not written yet" >&2
exit 1
