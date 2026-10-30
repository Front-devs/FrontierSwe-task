#!/bin/bash
# Oracle: installs the reference implementation (solution/minidb.c) into /workspace.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd /workspace

cp "$HERE/minidb.c" /workspace/minidb.c

cat > Makefile << 'MAKEFILE'
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lm

minidb: minidb.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f minidb

.PHONY: clean
MAKEFILE

make clean
make

echo "Reference solution built successfully."
