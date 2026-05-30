#!/bin/bash

LROOT=$PWD                                                                                                                                                                                                         
ROOTFS_ARM64=rootfs
NULL_DEV_NODE=dev/null                                                                                                                                                                                             
CONSOLE_DEV_NODE=dev/console                                                                                                                                                                                       
DEV_DIR_NODE=$PWD/$ROOTFS_ARM64/dev
export KCFLAGS="-Wno-return-type -Wno-error=return-type"


if [ $# -ne 2 ]; then
    echo "Usage: $0 [arch] [compile/compiled/run/debug/clang/clang-compile]"
    exit 1
fi 

if [ $1 == "arm64" ] && [ $2 == "compile" ]; then
    echo "start to build the kernel for $1"
    cd $LROOT
    
    make ARCH=arm64 distclean -j20
    make ARCH=arm64 ybzhang_defconfig -j20
    make ARCH=arm64  CONFIG_INITRAMFS_SOURCE="rootfs extra_nodes.txt" -j20
    make ARCH=arm64 compile_commands.json -j20

elif [ $1 == "arm64" ] && [ $2 == "compiled" ]; then
    echo "start to build the kernel for $1 (direct compile)"

    # 检查console设备节点是否存在，如果不存在则创建
    if [ ! -c $LROOT/$ROOTFS_ARM64/$CONSOLE_DEV_NODE ]; then
        echo "please create console device node first"
        if [ ! -d "$DEV_DIR_NODE" ]; then
            mkdir -p $DEV_DIR_NODE
        fi
        cd $DEV_DIR_NODE && sudo mknod console c 5 1
    fi

    # 检查null设备节点是否存在，如果不存在则创建
    if [ ! -c $LROOT/$ROOTFS_ARM64/$NULL_DEV_NODE ]; then
        echo "please create null device node first"
        if [ ! -d "$DEV_DIR_NODE" ]; then
            mkdir -p $DEV_DIR_NODE
        fi
        cd $DEV_DIR_NODE && sudo mknod null c 1 3
    fi

    cd $LROOT
    #make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- ybzhang_defconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8

elif [ $1 == "arm64" ] && [ $2 == "debug" ]; then
    echo "running kernel on QEMU for $1 (debug mode)"

    qemu-system-aarch64 -s -S -machine virt -cpu cortex-a57 -machine type=virt \
                        -m 1024 -smp 4 -kernel arch/arm64/boot/Image \
                        --append "nokaslr rdinit=/linuxrc console=ttyAMA0" -nographic \
                        --fsdev local,id=kmod_dev,path=$PWD/kmodules,security_model=none \
                        -device virtio-9p-device,fsdev=kmod_dev,mount_tag=kmod_mount &
    gdb
    killall qemu-system-aarch64
elif [ $1 == "arm64" ] && [ $2 == "run" ]; then
    echo "running kernel on QEMU for $1"

    qemu-system-aarch64 -machine virt -cpu cortex-a57 -machine type=virt \
              -m 1024 -smp 4 -kernel arch/arm64/boot/Image \
              --append "nokaslr rdinit=/linuxrc console=ttyAMA0" -nographic \
              --fsdev local,id=kmod_dev,path=$PWD/kmodules,security_model=none \
              -device virtio-9p-device,fsdev=kmod_dev,mount_tag=kmod_mount

elif [ $1 == "arm64" ] && [ $2 == "clanged" ]; then
    echo "start to build the kernel for $1 with LLVM (incremental)"
    cd $LROOT
    make ARCH=arm64 LLVM=1 CONFIG_INITRAMFS_SOURCE="rootfs extra_nodes.txt" -j20
    make ARCH=arm64 LLVM=1 compile_commands.json -j20

elif [ $1 == "arm64" ] && [ $2 == "clang" ]; then
    echo "start to build the kernel for $1 with LLVM (full)"
    cd $LROOT
    make ARCH=arm64 distclean -j20
    make ARCH=arm64 LLVM=1 ybzhang_defconfig -j20
    make ARCH=arm64 LLVM=1 CONFIG_INITRAMFS_SOURCE="rootfs extra_nodes.txt" -j20
    make ARCH=arm64 LLVM=1 compile_commands.json -j20

fi

###################################
#{
#  "clangd.path": "/usr/bin/clangd-17",
#  "clangd.arguments": [
#    "--background-index",
#    "--compile-commands-dir=${workspaceFolder}",
#    "--header-insertion=never",
#    "--log=error"
#  ],
#  "C_Cpp.intelliSenseEngine": "disabled"
#}


##################gdbinit script########################
#target remote localhost:1234
#./fix_vmlinux_head_entry.sh
#set disassemble-next-line on
#b _text
#b start_kernel
#layout src
#layout regs
#c
