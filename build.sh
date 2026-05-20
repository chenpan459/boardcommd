#!/usr/bin/env bash
set -euo pipefail

print_usage() {
    cat <<EOF
Usage:
  $0 <version>
  $0 clean

Examples:
  $0 1.0.0
  $0 1.0.0-rc1
  $0 2026.05.19
  $0 clean

Version format:
  number segments plus optional suffix, such as 1.0.0 or 1.0.0-rc1
EOF
}

version="${1:-}"

if [[ -z "${version}" ]]; then
    echo "error: build version or clean command is required" >&2
    print_usage >&2
    exit 1
fi

if [[ "${version}" == "clean" ]]; then
    rm -rf build
    echo "clean finished: removed build/"
    exit 0
fi

if [[ ! "${version}" =~ ^[0-9]+(\.[0-9]+){0,3}([._-][A-Za-z0-9]+)*$ ]]; then
    echo "error: invalid version '${version}'" >&2
    print_usage >&2
    exit 1
fi

cmake -S . -B build -DBOARDCOMM_VERSION="${version}"
cmake --build build

echo "build finished: version=${version}"
