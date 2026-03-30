#!/usr/bin/env bash

set -euo pipefail

IMAGE="${IMAGE:-alpine:latest}"
CONTAINER_NAME="${CONTAINER_NAME:-async_uv_alpine_builder}"
BUILD_DIR="${BUILD_DIR:-build_alpine_asan}"
TEST_TIMEOUT="${TEST_TIMEOUT:-300}"
JOBS="${JOBS:-$(nproc)}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

retry() {
  local max_attempts="$1"
  shift
  local attempt=1
  until "$@"; do
    if (( attempt >= max_attempts )); then
      echo "[alpine-asan] 命令失败，已达到最大重试次数: $*" >&2
      return 1
    fi
    echo "[alpine-asan] 重试 ${attempt}/${max_attempts}: $*" >&2
    attempt=$((attempt + 1))
    sleep 3
  done
}

ensure_container() {
  if ! podman image exists "$IMAGE"; then
    retry 3 podman pull "$IMAGE"
  fi

  if ! podman container exists "$CONTAINER_NAME"; then
    podman create \
      --name "$CONTAINER_NAME" \
      --workdir /work \
      --volume "$PROJECT_ROOT:/work" \
      "$IMAGE" \
      sleep infinity >/dev/null
  fi

  podman start "$CONTAINER_NAME" >/dev/null
}

if ! command -v podman >/dev/null 2>&1; then
  echo "[alpine-asan] 未找到 podman，请在 Linux 环境安装后再执行。" >&2
  exit 1
fi

ensure_container

podman exec \
  -e BUILD_DIR="$BUILD_DIR" \
  -e TEST_TIMEOUT="$TEST_TIMEOUT" \
  -e JOBS="$JOBS" \
  -e CLEAN_BUILD="$CLEAN_BUILD" \
  "$CONTAINER_NAME" sh -lc '
set -eu

retry() {
  max_attempts="$1"
  shift
  attempt=1
  while true; do
    if "$@"; then
      return 0
    fi
    if [ "$attempt" -ge "$max_attempts" ]; then
      echo "[alpine-asan] 命令失败，已达到最大重试次数: $*" >&2
      return 1
    fi
    echo "[alpine-asan] 重试 ${attempt}/${max_attempts}: $*" >&2
    attempt=$((attempt + 1))
    sleep 3
  done
}

if [ ! -f /var/tmp/.async_uv_deps_ready ] || ! apk info -e openssl-dev >/dev/null 2>&1; then
  sed -i "s#https\\?://dl-cdn.alpinelinux.org/alpine#https://mirrors.cernet.edu.cn/alpine#g" /etc/apk/repositories
  retry 5 apk update
  retry 5 apk add --no-cache \
    bash \
    build-base \
    clang \
    cmake \
    compiler-rt \
    git \
    linux-headers \
    lld \
    ninja-build \
    ninja-is-really-ninja \
    openssl-dev
  touch /var/tmp/.async_uv_deps_ready
fi

if [ "$CLEAN_BUILD" = "1" ]; then
  rm -rf "$BUILD_DIR"
fi

retry 3 cmake -S . -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DASYNC_UV_BUILD_EXAMPLES=ON \
  -DASYNC_UV_BUILD_TESTS=ON \
  -DASYNC_UV_USE_MIMALLOC=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"

retry 2 cmake --build "$BUILD_DIR" -j"$JOBS"

ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:check_initialization_order=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout "$TEST_TIMEOUT" -E async_uv_smoke_test

ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:check_initialization_order=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
sh -lc "cd /tmp && /work/$BUILD_DIR/tests/async_uv_smoke_test"
'
