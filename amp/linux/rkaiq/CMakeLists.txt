# 只编 rkaiq 库和 rkaiq_3A_server，源码用厂商 SDK 里的原样（不拷进仓库）。
# SDK 顶层的 CMakeLists 还会拉 rkisp_demo/smart_ir/rk_stream 等，要 RGA/DRM，
# 第三步用不到，所以自己写这个外壳。由 build-rkaiq.sh 调用。
cmake_minimum_required(VERSION 3.10)
project(k7_rkaiq C CXX)

set(RK_AIQ_IS_SUBDIR TRUE)
add_subdirectory(${RKAIQ_SRC}/rkaiq rkaiq)
add_subdirectory(${RKAIQ_SRC}/rkaiq_3A_server rkaiq_3A_server)
