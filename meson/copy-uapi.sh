#! /bin/sh
# $1 = O_UAPI
# $2 = $prefix/includedir

cd $1
find -L evl \! \( -name '*~' \) -type f | cpio -pduvm $DESTDIR/$2/uapi/
