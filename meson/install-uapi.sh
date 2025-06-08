#! /bin/sh
# $1 = O_UAPI
# $2 = $includedir

cd $1
find -L evl \! \( -name '*~' \) -type f | cpio -pdum --quiet $MESON_INSTALL_DESTDIR_PREFIX/$2/uapi
