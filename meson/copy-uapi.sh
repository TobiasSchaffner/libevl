#! /bin/sh
# $1 = O_UAPI
# $2 = $prefix/includedir

find -L $1/evl \! \( -name '*~' \) -type f | cpio -pduvm $DESTDIR/$2
