# CMake cross-compilation toolchain for Linux -> Windows x64 (llvm-mingw UCRT + Qt 6.8)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "/home/antigravity/tools/llvm-mingw" CACHE PATH "Path to llvm-mingw root")
endif()
if(NOT DEFINED QT_WIN64_DIR)
    set(QT_WIN64_DIR "/home/antigravity/tools/qt/6.8.3/llvm-mingw_64" CACHE PATH "Path to Qt6 win64_llvm_mingw")
endif()
if(NOT DEFINED QT_HOST_PATH)
    set(QT_HOST_PATH "/home/antigravity/tools/qt/6.8.3/gcc_64" CACHE PATH "Path to host Linux Qt6")
endif()
if(NOT DEFINED WIN64_DEPS_DIR)
    set(WIN64_DEPS_DIR "/home/antigravity/tools/win64-deps" CACHE PATH "Path to cross-compiled Win64 deps (OpenSSL)")
endif()

set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-strip" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH
    "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32"
    "${QT_WIN64_DIR}"
    "${WIN64_DEPS_DIR}"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Force static OpenSSL 3 lookup from WIN64_DEPS_DIR
set(OPENSSL_ROOT_DIR "${WIN64_DEPS_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
