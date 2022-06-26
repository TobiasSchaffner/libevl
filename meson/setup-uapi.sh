#! /bin/sh

# Expand anything in the UAPI path meson did not (e.g. ~).
UAPI=$(eval echo $UAPI)

if test -r $UAPI/Kbuild; then
	set \
		$UAPI/arch/$ARCH/include/uapi/asm \
		$UAPI/include/uapi/asm-generic \
		$UAPI/include/uapi/evl
else
	set $UAPI/asm $UAPI/asm-generic $UAPI/evl
fi

for d do
	test -d $d && continue
	echo "meson: path given to -Duapi does not look right ($d is missing)"
	exit 1
done

mkdir -p $O_UAPI
for d do ln -sf $d $O_UAPI/; done
