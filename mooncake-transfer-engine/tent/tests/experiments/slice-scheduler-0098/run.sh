#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
base=db6f14bd019d7fe08b2497e88046b72384af287f
ylt_commit=7801bc9ad9021781f15217552214e325a1cf7373
output=${1:-"$repo/build/slice-scheduler-0098"}
test ! -e "$output" || { printf 'Use a new output directory: %s\n' "$output" >&2; exit 2; }
mkdir -p "$output"/{source,downloads,results,objects}
output=$(cd -- "$output" && pwd)
git -C "$repo" archive --format=tar --output="$output/upstream.tar" "$base" \
  mooncake-transfer-engine/tent
tar -xf "$output/upstream.tar" -C "$output/source"
tent="$output/source/mooncake-transfer-engine/tent"
if [[ -n "${PRERESEARCH_DEPS_ROOT:-}" ]]; then
    deps=$(cd -- "$PRERESEARCH_DEPS_ROOT" && pwd)
else
    test "$(dpkg --print-architecture)" = amd64
    mkdir -p "$output/deps"
    (
        cd "$output/downloads"
        apt download libgflags2.2=2.2.2-2 libgflags-dev=2.2.2-2 \
          libgoogle-glog0v5=0.5.0+really0.4.0-2 \
          libgoogle-glog-dev=0.5.0+really0.4.0-2 libgtest-dev=1.11.0-3 \
          libnuma-dev=2.0.14-3ubuntu2
        for package in ./*.deb; do
            dpkg-deb -x "$package" "$output/deps"
            dpkg-deb -f "$package" Package Version Architecture
        done > "$output/results/dependencies.txt"
        sha256sum ./*.deb > "$output/results/packages.sha256"
    )
    deps="$output/deps/usr"
fi
test -f "$deps/lib/x86_64-linux-gnu/libgtest.a"
test -f "$deps/include/glog/logging.h"
curl --fail --location --retry 2 --connect-timeout 15 --max-time 180 \
  "https://codeload.github.com/alibaba/yalantinglibs/tar.gz/$ylt_commit" \
  -o "$output/downloads/ylt.tar.gz"
tar -xf "$output/downloads/ylt.tar.gz" -C "$output/downloads"
ylt="$output/downloads/yalantinglibs-$ylt_commit"
export LD_LIBRARY_PATH="$deps/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
flags=(-std=c++20 -O2 -g -pthread -ffunction-sections -fdata-sections
       -I"$tent/include" -I"$deps/include" -I"$ylt/include"
       -I"$ylt/include/ylt/thirdparty")
sources=(src/common/status.cpp src/common/config.cpp src/runtime/topology.cpp
         src/transport/rdma/quota.cpp src/transport/rdma/shared_quota.cpp
         src/transport/rdma/rail_monitor.cpp src/transport/rdma/gdr_reachability.cpp)
for source in "${sources[@]}"; do
    g++ "${flags[@]}" -c "$tent/$source" -o "$output/objects/${source//\//_}.o"
done
tests=(device_selector_test strict_local_numa_test bw_arbitration_test
       quota_accounting_test rail_monitor_test)
test_objects=()
for name in "${tests[@]}"; do
    g++ "${flags[@]}" -c "$tent/tests/$name.cpp" -o "$output/objects/$name.o"
    test_objects+=("$output/objects/$name.o")
done
g++ -pthread "$output"/objects/src_*.o "${test_objects[@]}" -Wl,--gc-sections \
  -L"$deps/lib/x86_64-linux-gnu" -lgtest_main -lgtest -lglog -lgflags -lnuma \
  -o "$output/upstream-tests"
timeout 180 "$output/upstream-tests" --gtest_output="xml:$output/results/upstream-gtest.xml"
node "$here/path-model.cjs" --extract-slice "$output/source" "$output/generated"
g++ -std=c++17 -O2 -g -pthread -ffunction-sections -fdata-sections \
  -I"$output/generated" -I"$tent/include" -I"$deps/include" \
  "$here/device-selector-probe.cpp" "$output"/objects/src_*.o -Wl,--gc-sections \
  -L"$deps/lib/x86_64-linux-gnu" -lglog -lgflags -lnuma -o "$output/device-selector-probe"
timeout 60 "$output/device-selector-probe" > "$output/results/device-selector.json"
node "$here/path-model.cjs" "$output/results/path-model.json"
g++ -std=c++17 -O2 -g -pthread -Wall -Wextra -Werror -pedantic \
  "$here/mechanism-probe.cpp" -o "$output/mechanism-probe"
timeout 60 "$output/mechanism-probe" > "$output/results/mechanism.json"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer \
  -fsanitize=address,undefined "$here/mechanism-probe.cpp" -o "$output/mechanism-sanitized"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  timeout 60 "$output/mechanism-sanitized" > "$output/results/mechanism-sanitized.json"
cmp "$output/results/mechanism.json" "$output/results/mechanism-sanitized.json"
g++ --version | sed '/^$/d' > "$output/results/compiler.txt"
node --version > "$output/results/node-version.txt"
printf '%s\n' "$base" > "$output/results/base-commit.txt"
printf '%s\n' "$ylt_commit" > "$output/results/ylt-commit.txt"
(
    cd "$here"
    sha256sum run.sh mechanism-probe.cpp device-selector-probe.cpp path-model.cjs
) > "$output/results/source.sha256"
(
    cd "$output"
    sha256sum upstream.tar downloads/ylt.tar.gz upstream-tests \
      device-selector-probe mechanism-probe mechanism-sanitized \
      results/upstream-gtest.xml results/device-selector.json \
      results/path-model.json results/mechanism.json results/mechanism-sanitized.json
) > "$output/results/artifacts.sha256"
printf 'Results: %s/results\n' "$output"
