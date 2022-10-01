#!/bin/bash
nice make -j8 && ./python ~/prog/python/bitfields/test.py && ./python -m pytest ~/prog/python/bitfields/test.py && ./python -E  /home/matthias/prog/python/cpythons/bitfields/Tools/scripts/run_tests.py test_ctypes
