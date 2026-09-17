# Building

Current environment: Android/Termux. Persistent Meson build directory:

    build-bionic

## Build
```sh
ninja -C build-bionic -j2 src/panfrost/vulkan/libvulkan_panfrost.so
```

## Install
```sh
cp -f build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so \
      "$PREFIX/lib/libvulkan_panfrost.so"
```

Do **not** replace `$PREFIX/lib/libvulkan.so`.

Development ICD:

    $PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json

## X11 configuration
```sh
meson configure build-bionic -Dplatforms=x11
meson configure build-bionic \
  -Dc_link_args="-landroid-shmem" \
  -Dcpp_link_args="-landroid-shmem"
```

## Run
```sh
export DISPLAY=:0
export VK_ICD_FILENAMES="$PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json"
```

This remains an experimental development build.
