#! /bin/sh

# Expand anything in the UAPI path meson did not (e.g. ~).
UAPI=$(eval echo $UAPI)

link_dir() {
    if test \! -d $1; then
	echo "meson: path given to -Duapi does not look right ($1 is missing)"
	exit 1
    fi
    mkdir -p $OUTPUT_DIR/$2
    ln -sf $1 $OUTPUT_DIR/$2
}

if test -r $UAPI/Kbuild; then
    link_dir $UAPI/include/uapi/evl .
    link_dir $UAPI/arch/$ARCH/include/uapi/asm/evl asm
    link_dir $UAPI/include/uapi/asm-generic .
else
    link_dir $UAPI/evl .
    link_dir $UAPI/asm/evl asm
    link_dir $UAPI/asm-generic .
fi
