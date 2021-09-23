#!/bin/sh

export STAGING_DIR=~/Sources2/openwrt/x-wrt/staging_dir

build_compile()
{
	make CROSS_COMPILE=~/Sources2/openwrt/x-wrt/staging_dir/toolchain-mipsel_24kc_gcc-11.2.0_musl/bin/mipsel-openwrt-linux-
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

