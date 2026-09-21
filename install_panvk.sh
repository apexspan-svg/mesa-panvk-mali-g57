#!/bin/bash
# Install PanVK Mali-G57 ICD JSON for Vulkan Loader in Termux
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SO_PATH="${SCRIPT_DIR}/build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so"

if [ ! -f "${SO_PATH}" ]; then
  echo "Error: ${SO_PATH} not found. Please run ./build_panvk.sh first."
  exit 1
fi

ICD_DIR="${PREFIX}/share/vulkan/icd.d"
mkdir -p "${ICD_DIR}"

cat <<EOF > "${ICD_DIR}/panfrost_icd.aarch64.json"
{
    "file_format_version": "1.0.1",
    "ICD": {
        "api_version": "1.3.354",
        "library_arch": "64",
        "library_path": "${SO_PATH}"
    }
}
EOF

echo "Installed PanVK ICD to: ${ICD_DIR}/panfrost_icd.aarch64.json"
echo "Verify with: vulkaninfo --summary"
