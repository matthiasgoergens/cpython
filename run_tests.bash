#!/bin/bash
set -e
set -u
set -o pipefail
set -x
nice make -j8
./python ~/prog/python/bitfields/test.py

# exit 0

./python -m pytest ~/prog/python/bitfields/test.py
./python -E  /home/matthias/prog/python/cpythons/bitfields/Tools/scripts/run_tests.py test_ctypes
nice make test
