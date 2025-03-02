#!/bin/bash

set -e
set -o pipefail

KERNEL_DIR="src/core/kernel/linux-6.13.2"
BUILD_DIR="$(pwd)/build"
ISO_DIR="$BUILD_DIR/iso"

# Compiler settings
ARCH=x86_64
CROSS_COMPILE=

clean() {
    echo "🧹 Cleaning build artifacts..."
    rm -rf "$BUILD_DIR"
}

build_kernel() {
    echo "🛠️ Building kernel..."
    cd "$KERNEL_DIR"
    make ARCH=$ARCH defconfig
    make ARCH=$ARCH -j$(nproc)
    cd -
}

build_services() {
    echo "🚀 Building services..."
    make -C src/core/services
}

build_programs() {
    echo "📦 Building user programs..."
    make -C src/programs
}

create_iso() {
    echo "💿 Creating bootable ISO..."
    mkdir -p "$ISO_DIR/boot/grub"
    mkdir -p "$ISO_DIR/boot"

    cp "$KERNEL_DIR/arch/x86_64/boot/bzImage" "$ISO_DIR/boot/kernel"

    # Generate GRUB config
    cat > "$ISO_DIR/boot/grub/grub.cfg" <<EOF
set timeout=5
set default=0

menuentry "Hood OS" {
linux /boot/kernel
boot
}
EOF
    grub-mkrescue -o "$BUILD_DIR/os.iso" "$ISO_DIR"
}


build_all() {
    mkdir -p "$BUILD_DIR"
    build_kernel
    build_services
    build_programs
    create_iso
    echo "✅ Build complete! ISO is located at $BUILD_DIR/os.iso"
}

case "$1" in
    clean) clean ;;
    kernel) build_kernel ;;
    services) build_services ;;
    programs) build_programs ;;
    iso) create_iso ;;
    all | "") build_all ;;
    *) echo "Usage: $0 {clean|kernel|services|programs|iso|all}" ;;
esac
