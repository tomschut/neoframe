#!/bin/sh
set -eu
: "${IDF_PATH:?Activate ESP-IDF 4.4.4 first, or run this script in its container}"
mkdir -p build/host-tests
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/stubs -Imain -I"$IDF_PATH/components/json/cJSON" \
    tests/test_config.c main/config.c main/core.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
    -lm -o build/host-tests/config
build/host-tests/config
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/stubs -Imain tests/test_http.c main/http.c -o build/host-tests/http
build/host-tests/http
