#!/bin/bash

set -ex

cd `dirname $0`
INSTALL_DIR="`pwd`/_install"
MODULE_PATH="`pwd`/cmake"
PROJECT_DIR="`pwd`"

BUILD_DIR="_build/goa"

export PATH=$INSTALL_DIR/cmake/bin:$PATH

if [ -z "$JOBS" ]; then
  set +e
  JOBS=`nproc`
  if [ -z "$JOBS" ]; then
    JOBS=`sysctl -n hw.logicalcpu_max`
    if [ -z "$JOBS" ]; then
      JOBS=1
    fi
  fi
  set -e
fi

mkdir -p $BUILD_DIR
pushd $BUILD_DIR
  cmake $PROJECT_DIR \
    -DSPDLOG_ROOT_DIR="$INSTALL_DIR/spdlog" \
    -DCLI11_ROOT_DIR="$INSTALL_DIR/cli11" \
    -DJSON_ROOT_DIR="$INSTALL_DIR/json" \
    -DGGRPC_ROOT_DIR="$INSTALL_DIR/ggrpc" \
    -DCMAKE_PREFIX_PATH="$INSTALL_DIR/grpc;$INSTALL_DIR/boost" \
    -DWEBRTC_INCLUDE_DIR=$INSTALL_DIR/webrtc/include \
    -DWEBRTC_LIBRARY_DIR=$INSTALL_DIR/webrtc/lib \
    -DCMAKE_MODULE_PATH=$MODULE_PATH \
    -DCMAKE_BUILD_TYPE=Debug \
    "$@"
  make -j$JOBS goa_client goa_server
popd
