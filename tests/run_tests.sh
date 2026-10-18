#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "===================================================="
echo " 1. Running Test Suite (Native Host Execution)"
echo "===================================================="
./xcc --run tests/test_suite.c

echo ""
echo "===================================================="
echo " 2. Running Test Suite (Cross-Platform IR VM)"
echo "===================================================="
./xcc --run-ir tests/test_suite.c

echo ""
echo "===================================================="
echo " 3. Running Custom Hardened IR Obfuscation (--obf)"
echo "===================================================="
./xcc --obf --run tests/test_suite.c
./xcc --obf --run-ir tests/test_suite.c

echo ""
echo "===================================================="
echo " 4. Running Polymorphic VM Virtualizer (--obf-vm)"
echo "===================================================="
./xcc --obf-vm --run tests/test_suite.c
./xcc --obf-vm --run-ir tests/test_suite.c

echo ""
echo "===================================================="
echo " 5. Running Dual-Track Protection (--obf-all)"
echo "===================================================="
./xcc --obf-all --run examples/anti_crack_demo.c

TMP_DIR="/tmp/xcc_test_$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT

./xcc --obf-all examples/anti_crack_demo.c -o "$TMP_DIR/hardened_bin"
if strings "$TMP_DIR/hardened_bin" | grep -qE "XCC-ENTERPRISE-ADMIN|ACCESS GRANTED"; then
    echo "[FAIL] Plaintext secret strings leaked in hardened binary!"
    exit 1
else
    echo "[OK] Zero plaintext secret strings found in hardened binary (strings check passed)"
fi

echo ""
echo "===================================================="
echo " 6. Verifying Cross-Target Assembly & Object Files"
echo "===================================================="
for target in arm64-macos arm64-linux x86_64-macos x86_64-linux; do
    ./xcc --obf-all -target "$target" -S tests/test_suite.c -o "$TMP_DIR/suite_${target}.s"
    ./xcc --obf-all -target "$target" -c tests/test_suite.c -o "$TMP_DIR/suite_${target}.o"
    echo "[OK] $target (Hardened Custom IR + VM) -> $(file -b "$TMP_DIR/suite_${target}.o")"
done

./xcc --obf-all -target x86_64-windows -S tests/test_suite.c -o "$TMP_DIR/suite_x86_64-windows.s"
echo "[OK] x86_64-windows (Hardened Custom IR + VM) -> Assembly generated ($(wc -l < "$TMP_DIR/suite_x86_64-windows.s" | tr -d ' ') lines)"

echo ""
echo "All tests, anti-reversing checks, and cross-compilation checks passed!"
