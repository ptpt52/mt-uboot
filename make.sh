#!/bin/sh

#make mt7621_nand_ax_rfb_defconfig  #(NAND flash)
make mt7621_ax_rfb_defconfig  #(NOR flash)
make CROSS_COMPILE=/opt/buildroot-gcc492_mips_glibc/usr/bin/mipsel-linux-
