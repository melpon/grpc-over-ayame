cmake_minimum_required(VERSION 3.16)

set(_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_install")

set(CMAKE_C_COMPILER "${_INSTALL_DIR}/llvm/clang/bin/clang")
set(CMAKE_CXX_COMPILER "${_INSTALL_DIR}/llvm/clang/bin/clang++")
set(WEBRTC_LIBRARY_NAME webrtc)

project(grpc-over-ayame C CXX)

find_package(WebRTC REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(CLI11 REQUIRED)
find_package(Spdlog REQUIRED)
find_package(JSON REQUIRED)
find_package(Boost REQUIRED)
find_package(Ggrpc REQUIRED)

set_target_properties(gRPC::grpc++ PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_INSTALL_DIR}/grpc/include"
  INTERFACE_LINK_LIBRARIES "gRPC::ssl;gRPC::crypto;m;pthread;gRPC::grpc;gRPC::gpr;gRPC::upb"
)

# --- goa.pb.cc などの生成

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto")
add_custom_command(
  OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.pb.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.pb.h"
    "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.grpc.pb.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.grpc.pb.h"
  COMMAND $<TARGET_FILE:protobuf::protoc>
  ARGS
    --grpc_out "${CMAKE_CURRENT_BINARY_DIR}/proto"
    --cpp_out "${CMAKE_CURRENT_BINARY_DIR}/proto"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    --plugin=protoc-gen-grpc="$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/goa.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/goa.proto")

set(GOA_PROTO
  "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.pb.cc"
  "${CMAKE_CURRENT_BINARY_DIR}/proto/goa.grpc.pb.cc")

# ---- goa_client, goa_server

add_executable(goa_client)
target_sources(goa_client PRIVATE src/goa_client.cpp)

add_executable(goa_server)
target_sources(goa_server PRIVATE src/goa_server.cpp)

foreach(_target IN ITEMS goa_client goa_server)
  target_sources(${_target} PRIVATE ${GOA_PROTO})
  #target_sources(${_target} PRIVATE src/ayame_transport.cpp)
  target_sources(${_target} PRIVATE src/ayame_signaling.cpp)
  target_compile_definitions(${_target} PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    WEBRTC_POSIX
    WEBRTC_MAC
  )
  target_compile_options(${_target} PRIVATE "-nostdinc++" "-isystem${_INSTALL_DIR}/llvm/libcxx/include")
  set_target_properties(${_target} PROPERTIES CXX_STANDARD 17 C_STANDARD 99)
  target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/proto")
  target_link_libraries(${_target}
    PRIVATE
      WebRTC::WebRTC
      JSON::JSON
      CLI11::CLI11
      gRPC::grpc++
      Spdlog::Spdlog
      Boost::headers
      Ggrpc::Ggrpc
  )

  target_compile_options(${_target} PRIVATE -fconstant-string-class=NSConstantString)
  target_link_options(${_target} PRIVATE -ObjC)
  set_target_properties(${_target} PROPERTIES CXX_VISIBILITY_PRESET hidden)

  target_link_libraries(${_target}
    PRIVATE
      "-framework Foundation"
      "-framework AVFoundation"
      # "-framework CoreServices"
      # "-framework CoreFoundation"
      # "-framework AudioUnit"
      "-framework AudioToolbox"
      "-framework CoreAudio"
      "-framework CoreGraphics"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
      # "-framework AppKit"
      # "-framework Metal"
      # "-framework MetalKit"
      "-framework OpenGL"
  )
endforeach()
