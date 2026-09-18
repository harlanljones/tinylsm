# Builds the tinylsm engine for the browser (section 7 of the design document).
# Configure with `emcmake cmake -S wasm -B build-wasm` or use scripts/build_wasm.sh.
cmake_minimum_required(VERSION 3.20)
project(tinylsm_wasm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure this project with emcmake (Emscripten) so MEMFS and pthreads are available.")
endif()

add_executable(tinylsm_wasm
  tinylsm_wasm.cpp
  ../src/db.cpp
  ../src/table.cpp
  ../src/workers.cpp
  ../src/recovery.cpp)
target_include_directories(tinylsm_wasm PRIVATE ../include ../src ../app)
target_compile_options(tinylsm_wasm PRIVATE -O3 -Wall -Wextra)
# No pthreads: the engine's background FlushWorker/CompactWorker threads would
# make every maintenance call block the calling thread, and Emscripten proxies
# worker syscalls through the main thread. The browser build therefore compiles
# TINYLSM_INLINE_MAINTENANCE and runs the same policies on the caller.
target_link_options(tinylsm_wasm PRIVATE
  -sMODULARIZE=1
  -sEXPORT_NAME=createTinylsm
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sSTACK_SIZE=1048576
  -sFORCE_FILESYSTEM=1
  -sFILESYSTEM=1
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,lengthBytesUTF8,stringToUTF8,HEAPU8
  -sEXPORTED_FUNCTIONS=_malloc,_free,_tinylsm_open,_tinylsm_recover,_tinylsm_reset,_tinylsm_command,_tinylsm_last_error,_tinylsm_close,_tinylsm_is_open
  -O3)
set_target_properties(tinylsm_wasm PROPERTIES OUTPUT_NAME tinylsm_wasm SUFFIX ".js")
