#!/bin/bash
# Tests tests/fat32_check.py. Run in a container: docker run --rm -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_fat32_check_test.sh
set -e
apt-get update -qq
apt-get install -y -qq dosfstools mtools python3 >/dev/null
python3 tests/fat32_check_test.py
