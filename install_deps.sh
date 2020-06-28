#!/bin/bash

SOURCE_DIR="`pwd`/_source"
BUILD_DIR="`pwd`/_build"
INSTALL_DIR="`pwd`/_install"

set -ex

mkdir -p $SOURCE_DIR
mkdir -p $BUILD_DIR
mkdir -p $INSTALL_DIR

CMAKE_VERSION="3.17.3"
GRPC_VERSION="1.27.0"
CLI11_VERSION="1.9.1"
SPDLOG_VERSION="1.4.2"
JSON_VERSION="3.8.0"
WEBRTC_VERSION="84.4147.7.3"
BOOST_VERSION="1.73.0"
GGRPC_VERSION="0.4.0"

if [ -z "$JOBS" ]; then
  set +e
  # Linux
  JOBS=`nproc 2>/dev/null`
  if [ -z "$JOBS" ]; then
    # macOS
    JOBS=`sysctl -n hw.logicalcpu_max 2>/dev/null`
    if [ -z "$JOBS" ]; then
      JOBS=1
    fi
  fi
  set -e
fi

# CMake が古いとビルド出来ないので、CMake のバイナリをダウンロードする
CMAKE_VERSION_FILE="$INSTALL_DIR/cmake.version"
CMAKE_CHANGED=0
if [ ! -e $CMAKE_VERSION_FILE -o "$CMAKE_VERSION" != "`cat $CMAKE_VERSION_FILE`" ]; then
  CMAKE_CHANGED=1
fi
if [ $CMAKE_CHANGED -eq 1 -o ! -e $INSTALL_DIR/cmake/bin/cmake ]; then
  if [ "`uname`" = "Darwin" ]; then
    _URL=https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-Darwin-x86_64.tar.gz
    _FILE=$SOURCE_DIR/cmake-${CMAKE_VERSION}-Darwin-x86_64.tar.gz
    _DIR=cmake-${CMAKE_VERSION}-Darwin-x86_64
    _INSTALL=$INSTALL_DIR/CMake.app
  else
    _URL=https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-Linux-x86_64.tar.gz
    _FILE=$SOURCE_DIR/cmake-${CMAKE_VERSION}-Linux-x86_64.tar.gz
    _DIR=cmake-${CMAKE_VERSION}-Linux-x86_64
    _INSTALL=$INSTALL_DIR/cmake
  fi
  if [ ! -e $_FILE ]; then
    echo "file(DOWNLOAD $_URL $_FILE)" > $SOURCE_DIR/tmp.cmake
    cmake -P $SOURCE_DIR/tmp.cmake
    rm $SOURCE_DIR/tmp.cmake
  fi

  pushd $SOURCE_DIR
    rm -rf $_DIR
    cmake -E tar xf $_FILE
  popd

  rm -rf $_INSTALL
  mv $SOURCE_DIR/$_DIR $_INSTALL
fi
echo $CMAKE_VERSION > $CMAKE_VERSION_FILE

if [ "`uname`" = "Darwin" ]; then
  export PATH=$INSTALL_DIR/CMake.app/Contents/bin:$PATH
else
  export PATH=$INSTALL_DIR/cmake/bin:$PATH
fi

# CLI11
CLI11_VERSION_FILE="$INSTALL_DIR/cli11.version"
CLI11_CHANGED=0
if [ ! -e $CLI11_VERSION_FILE -o "$CLI11_VERSION" != "`cat $CLI11_VERSION_FILE`" ]; then
  CLI11_CHANGED=1
fi
if [ $CLI11_CHANGED -eq 1 -o  ! -e $INSTALL_DIR/CLI11/include ]; then
  rm -rf $INSTALL_DIR/CLI11
  git clone --branch v$CLI11_VERSION --depth 1 https://github.com/CLIUtils/CLI11.git $INSTALL_DIR/CLI11
fi
echo $CLI11_VERSION > $CLI11_VERSION_FILE

# spdlog
SPDLOG_VERSION_FILE="$INSTALL_DIR/spdlog.version"
SPDLOG_CHANGED=0
if [ ! -e $SPDLOG_VERSION_FILE -o "$SPDLOG_VERSION" != "`cat $SPDLOG_VERSION_FILE`" ]; then
  SPDLOG_CHANGED=1
fi
if [ $SPDLOG_CHANGED -eq 1 -o  ! -e $INSTALL_DIR/spdlog/include ]; then
  rm -rf $INSTALL_DIR/spdlog
  git clone --branch v$SPDLOG_VERSION --depth 1 https://github.com/gabime/spdlog.git $INSTALL_DIR/spdlog
fi
echo $SPDLOG_VERSION > $SPDLOG_VERSION_FILE

# nlohmann/json
JSON_VERSION_FILE="$INSTALL_DIR/json.version"
JSON_CHANGED=0
if [ ! -e $JSON_VERSION_FILE -o "$JSON_VERSION" != "`cat $JSON_VERSION_FILE`" ]; then
  JSON_CHANGED=1
fi
if [ $JSON_CHANGED -eq 1 -o ! -e $INSTALL_DIR/json/include/nlohmann/json.hpp ]; then
  pushd $INSTALL_DIR
    rm -rf json
    git clone --branch v$JSON_VERSION --depth 1 https://github.com/nlohmann/json.git
  popd
fi
echo $JSON_VERSION > $JSON_VERSION_FILE

# ggrpc
GGRPC_VERSION_FILE="$INSTALL_DIR/ggrpc.version"
GGRPC_CHANGED=0
if [ ! -e $GGRPC_VERSION_FILE -o "$GGRPC_VERSION" != "`cat $GGRPC_VERSION_FILE`" ]; then
  GGRPC_CHANGED=1
fi
if [ $GGRPC_CHANGED -eq 1 -o ! -e $INSTALL_DIR/ggrpc/include/ggrpc/ggrpc.hpp ]; then
  pushd $INSTALL_DIR
    rm -rf ggrpc
    git clone --branch $GGRPC_VERSION --depth 1 https://github.com/melpon/ggrpc.git
  popd
fi
echo $GGRPC_VERSION > $GGRPC_VERSION_FILE


# WebRTC
WEBRTC_VERSION_FILE="$INSTALL_DIR/webrtc.version"
WEBRTC_CHANGED=0
if [ ! -e $WEBRTC_VERSION_FILE -o "$WEBRTC_VERSION" != "`cat $WEBRTC_VERSION_FILE`" ]; then
  WEBRTC_CHANGED=1
fi
if [ $WEBRTC_CHANGED -eq 1 -o ! -e $INSTALL_DIR/webrtc/lib/libwebrtc.a ]; then
  rm -rf $INSTALL_DIR/webrtc
  _PACKAGE_NAME=macos

  if [ ! -e $SOURCE_DIR/webrtc.${_PACKAGE_NAME}.${WEBRTC_VERSION}.tar.gz ]; then
    curl -Lo $SOURCE_DIR/webrtc.${_PACKAGE_NAME}.${WEBRTC_VERSION}.tar.gz https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/m${WEBRTC_VERSION}/webrtc.${_PACKAGE_NAME}.tar.gz
  fi

  pushd $INSTALL_DIR
    tar xf $SOURCE_DIR/webrtc.${_PACKAGE_NAME}.${WEBRTC_VERSION}.tar.gz
  popd
fi
echo $WEBRTC_VERSION > $WEBRTC_VERSION_FILE

# 特定バージョンの clang, libcxx を利用する
source $INSTALL_DIR/webrtc/VERSIONS
mkdir -p $INSTALL_DIR/llvm
pushd $INSTALL_DIR/llvm
  if [ ! -e tools/.git ]; then
    git clone https://chromium.googlesource.com/chromium/src/tools
  fi
  pushd tools
    git fetch
    git reset --hard $WEBRTC_SRC_TOOLS_COMMIT
    python clang/scripts/update.py --output-dir=$INSTALL_DIR/llvm/clang
  popd

  if [ ! -e libcxx/.git ]; then
    git clone https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx
  fi
  pushd libcxx
    git fetch
    git reset --hard $WEBRTC_SRC_BUILDTOOLS_THIRD_PARTY_LIBCXX_TRUNK
  popd
popd

# grpc (cmake)
GRPC_VERSION_FILE="$INSTALL_DIR/grpc.version"
GRPC_CHANGED=0
if [ ! -e $GRPC_VERSION_FILE -o "$GRPC_VERSION" != "`cat $GRPC_VERSION_FILE`" ]; then
  GRPC_CHANGED=1
fi
if [ $GRPC_CHANGED -eq 1 -o ! -e $INSTALL_DIR/grpc/lib/libgrpc++.a ]; then
  # gRPC のソース
  if [ ! -e $SOURCE_DIR/grpc/.git ]; then
    git clone https://github.com/grpc/grpc.git $SOURCE_DIR/grpc
  fi
  pushd $SOURCE_DIR/grpc
    git fetch
    git reset --hard v$GRPC_VERSION
    git submodule update -i --recursive
    pushd third_party/protobuf
      git reset --hard v3.9.0
    popd
    # デバッグモードだとなぜかエラーになるので
    sed -i -e 's/const auto flag/const auto\& flag/g' third_party/boringssl/crypto/impl_dispatch_test.cc
  popd

  rm -rf $BUILD_DIR/grpc-build
  mkdir -p $BUILD_DIR/grpc-build
  pushd $BUILD_DIR/grpc-build
    cmake $SOURCE_DIR/grpc \
      -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR/grpc \
      -DgRPC_BUILD_CSHARP_EXT=OFF \
      -DBENCHMARK_ENABLE_TESTING=0 \
      -DCMAKE_C_COMPILER="$INSTALL_DIR/llvm/clang/bin/clang" \
      -DCMAKE_CXX_COMPILER="$INSTALL_DIR/llvm/clang/bin/clang++" \
      -DCMAKE_CXX_FLAGS="-Wno-range-loop-construct" \
      -DCMAKE_BUILD_TYPE=Debug
    make -j$JOBS
    make install
  popd
  # src 側にあるヘッダーファイルも利用する
  rsync -av --prune-empty-dirs --include='*/' --include='*.h' --exclude='*' _source/grpc/src/ _install/grpc/include/src/
fi
echo $GRPC_VERSION > $GRPC_VERSION_FILE

# Boost
BOOST_VERSION_FILE="$INSTALL_DIR/boost.version"
BOOST_CHANGED=0
if [ ! -e $BOOST_VERSION_FILE -o "$BOOST_VERSION" != "`cat $BOOST_VERSION_FILE`" ]; then
  BOOST_CHANGED=1
fi

if [ $BOOST_CHANGED -eq 1 -o ! -e $INSTALL_DIR/boost/include/boost/version.hpp ]; then
  _VERSION_UNDERSCORE=${BOOST_VERSION//./_}
  rm -rf $BUILD_DIR/boost_${_VERSION_UNDERSCORE}
  rm -rf $INSTALL_DIR/boost

  _URL=https://dl.bintray.com/boostorg/release/${BOOST_VERSION}/source/boost_${_VERSION_UNDERSCORE}.tar.gz
  _FILE=$SOURCE_DIR/boost_${_VERSION_UNDERSCORE}.tar.gz
  if [ ! -e $_FILE ]; then
    echo "file(DOWNLOAD $_URL $_FILE)" > $BUILD_DIR/tmp.cmake
    cmake -P $BUILD_DIR/tmp.cmake
    rm $BUILD_DIR/tmp.cmake
  fi
  pushd $BUILD_DIR
    rm -rf boost_${_VERSION_UNDERSCORE}
    cmake -E tar xf $_FILE

    pushd boost_${_VERSION_UNDERSCORE}
      ./bootstrap.sh
      ./b2 headers
      mkdir -p $INSTALL_DIR/boost/include
      cp -r boost/ $INSTALL_DIR/boost/include/boost
    popd
  popd
fi
echo $BOOST_VERSION > $BOOST_VERSION_FILE
