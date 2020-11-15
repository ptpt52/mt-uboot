#!/bin/sh

build_compile()
{
	make CROSS_COMPILE=/opt/buildroot-gcc492_mips_glibc/usr/bin/mipsel-linux-
}

build_nand()
{
	make mt7621_nand_ax_rfb_defconfig  #(NAND flash)
	build_compile
}

build_nor()
{
	make mt7621_ax_rfb_defconfig  #(NOR flash)
	build_compile
}

eval "build_$1" && {
	mv u-boot-mt7621.bin u-boot-mt7621-$1.bin
}

