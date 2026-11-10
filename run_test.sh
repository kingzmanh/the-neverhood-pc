#!/bin/sh
# Hidden, muted test run: ./run_test.sh <seconds> ; extra NH_* env vars pass through.
cd "$(dirname "$0")"
S=${1:-15}
NH_HIDDEN=1 NH_MUTE=1 NH_QUIT=$((S*1000)) NH_REGFILE=${NH_REGFILE:-test_registry.txt} timeout $((S+20)) ./Neverhood.exe
echo "exit $?"
tail -n ${TAIL:-40} neverhood.log
