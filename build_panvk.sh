#!/bin/bash
# Build script for PanVK Mali-G57 (Valhall JM) on Android/Termux
set -e

BUILD_DIR="build-bionic"

echo "=== Configuring Meson for PanVK Mali-G57 ==="
meson setup "${BUILD_DIR}" \
  -Dbuildtype=release \
  -Dpanvk-use-kbase=true \
  -Dvulkan-drivers=panfrost \
  -Dgallium-drivers= \
  -Dplatforms=x11 \
  -Ddebug=false \
  -Dstrip=true \
  -Dbuild-tests=false \
  -Dc_link_args=-landroid-shmem \
  -Dcpp_link_args=-landroid-shmem \
  -Dpanfrost-kmds=kbase,panthor

echo "=== Compiling PanVK Driver ==="
ninja -C "${BUILD_DIR}" src/panfrost/vulkan/libvulkan_panfrost.so

echo "=== Build Complete ==="
echo "Driver binary located at: ${BUILD_DIR}/src/panfrost/vulkan/libvulkan_panfrost.so"
