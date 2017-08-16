#!/usr/bin/env bash

set -e

timeout='timeout'
if [[ "$OSTYPE" == "darwin"* ]]; then
  timeout='gtimeout'
fi

if [ ! -s ./build/frame_fuzzer ]; then
    echo "./build/frame_fuzzer binary not found!"
    exit 1
fi
if [ ! -s ./build/frame_fuzzer_net ]; then
    echo "./build/frame_fuzzer_net binary not found!"
    exit 1
fi

echo "killing existing fuzzer binary..."
pkill frame_fuzzer_net || true

for fuzzcase in ./test/fuzzer_testcases/frame_fuzzer/*; do
  echo "testing via stdin with $fuzzcase..."
  ./build/frame_fuzzer --v=100 < $fuzzcase

  rm -f build/frame_fuzzer_net_out
  rm -f build/frame_fuzzer_nc_out

  echo "testing via tcp with $fuzzcase..."
  ./build/frame_fuzzer_net --v=3 >./build/frame_fuzzer_net_out 2>&1 &
  SERVER_PID=$!
  echo "PID: ${SERVER_PID}"
  sleep 0.25

  echo "sending testcase..."
  cat $fuzzcase | nc localhost 9898 | xxd >./build/frame_fuzzer_nc_out 2>&1

  sleep 1

  # check if the server crashed
  if ! ps -p 0 $SERVER_PID >/dev/null; then
    echo "$fuzzcase caused a crash,"
    echo "see ./build/frame_fuzzer_net_out for stdout/stderr of server"
    echo "and ./build/frame_fuzzer_nc_out for stdout/stderr of client"

    kill $SERVER_PID >/dev/null 2>&1 || true

    exit 1
  fi

  echo "killing..."
  kill $SERVER_PID || true
done

# all passed succesfully
rm -f ./build/frame_fuzzer_net_out
