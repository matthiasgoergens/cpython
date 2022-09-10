#!/bin/bash
set -e
set -u
set -o pipefail
set -x

dir="builds/dict-reuse"
here="$(realpath .)"

mkdir --parent "${dir}"
pushd "${dir}"

export CC="clang"
export LD="clang"
export ASAN_OPTIONS=detect_leaks=0
export MSAN_OPTIONS=poison_in_dtor=0
# export CFLAGS="-fsanitize-recover"
nice "${here}/configure" \
    --with-assertions \
    --with-pydebug \
    --disable-ipv6 \
    --with-trace-refs

    # --with-undefined-behavior-sanitizer \
    # --with-memory-sanitizer \
#     --with-address-sanitizer \


nice make -j8
nice make test
