#!/bin/bash
# GFANC 黄金回归测试 + 单元测试 (R-31)
# 用法: ./test/run_tests.sh              → 运行测试, 与现有基线对比
#       ./test/run_tests.sh --accept     → 接受当前输出为新的黄金基线

set -e
cd "$(dirname "$0")/.."

GOLDEN_FILE="test/golden.sha256"
TEST_WAV="test/test_signal.wav"
FAILED=0
ACCEPT=0
[ "$1" = "--accept" ] && ACCEPT=1

echo "=== GFANC Golden Regression Tests (R-31) ==="
echo

# ── 1. 编译 ──
echo "── 1. Build ──"
gcc -O2 -Iinclude main.c src/scene_controller.c src/fxnlms_mimo.c \
    src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c \
    src/howling_detect.c -lm -o main.exe
echo "  main.exe OK"

gcc -O2 test/gen_test_wav.c -lm -o test/gen_test_wav.exe
echo "  gen_test_wav.exe OK"

gcc -O2 -Iinclude test/test_fir.c src/fir_filter.c src/fxnlms_mimo.c \
    -lm -o test/test_fir.exe
echo "  test_fir.exe OK"

# ── 2. 单元测试 ──
echo
echo "── 2. Unit Tests ──"
./test/test_fir.exe || { echo "  UNIT TESTS FAILED"; FAILED=1; }

# ── 3. 黄金回归 ──
echo
echo "── 3. Golden Regression ──"
./test/gen_test_wav.exe "$TEST_WAV"
echo "  Running main.exe..."
./main.exe "$TEST_WAV" > /dev/null 2>&1 || { echo "  main.exe CRASHED"; exit 1; }

ANTI_HASH=$(sha256sum anti_out.wav | awk '{print $1}')
ERR_HASH=$(sha256sum error_out.wav | awk '{print $1}')
echo "  anti_out.wav  sha256: $ANTI_HASH"
echo "  error_out.wav sha256: $ERR_HASH"

if [ "$ACCEPT" -eq 1 ]; then
    echo "$ANTI_HASH  anti_out.wav"  > "$GOLDEN_FILE"
    echo "$ERR_HASH   error_out.wav" >> "$GOLDEN_FILE"
    echo
    echo "  Golden baseline ACCEPTED → $GOLDEN_FILE"
else
    if [ ! -f "$GOLDEN_FILE" ]; then
        echo
        echo "  No golden baseline found. Run with --accept to create:"
        echo "    ./test/run_tests.sh --accept"
        FAILED=1
    else
        EXPECT_ANTI=$(awk '{print $1}' "$GOLDEN_FILE" | head -1)
        EXPECT_ERR=$(awk '{print $1}' "$GOLDEN_FILE" | tail -1)
        if [ "$ANTI_HASH" = "$EXPECT_ANTI" ]; then
            echo "  PASS: anti_out.wav matches golden"
        else
            echo "  FAIL: anti_out.wav hash mismatch!"
            echo "    expected: $EXPECT_ANTI"
            echo "    got:      $ANTI_HASH"
            FAILED=1
        fi
        if [ "$ERR_HASH" = "$EXPECT_ERR" ]; then
            echo "  PASS: error_out.wav matches golden"
        else
            echo "  FAIL: error_out.wav hash mismatch!"
            echo "    expected: $EXPECT_ERR"
            echo "    got:      $ERR_HASH"
            FAILED=1
        fi
    fi
fi

# ── 4. 清理临时文件 ──
rm -f "$TEST_WAV" anti_out.wav error_out.wav

echo
if [ "$FAILED" -eq 0 ]; then
    echo "=== ALL TESTS PASSED ==="
else
    echo "=== TESTS FAILED ==="
fi
exit $FAILED
