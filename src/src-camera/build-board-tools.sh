#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
OUTPUT_DIR="$PROJECT_DIR/bin"
mkdir -p "$OUTPUT_DIR"
cc -O2 -Wall -Wextra "$SCRIPT_DIR/isp3a-daemon.c" \
    -o "$OUTPUT_DIR/isp3a-daemon" -lAWIspApi -lpthread
cc -O2 -Wall -Wextra "$SCRIPT_DIR/vin-nv12-normalizer.c" \
    -o "$OUTPUT_DIR/vin-nv12-normalizer" -lAWIspApi -lpthread
cc -O2 -Wall -Wextra "$SCRIPT_DIR/nv12-timestamp.c" \
    -o "$OUTPUT_DIR/nv12-timestamp"
echo "Board helper binaries installed in $OUTPUT_DIR"
