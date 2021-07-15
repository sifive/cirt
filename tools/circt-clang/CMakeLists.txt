set(LLVM_LINK_COMPONENTS
  Support
  )

INCLUDE_DIRECTORIES(/workspace/peano/llvm-project/clang/include/)
include_directories(/workspace/build-container/peano/tools/clang/include)

set(CLANG_DIR "/workspace/build-container/peano/lib/cmake/clang")
find_package(Clang REQUIRED CONFIG PATHS ${CLANG_DIR} NO_DEFAULT_PATH)

add_llvm_executable(circt-clang
 circt-clang.cpp
)

llvm_update_compile_flags(circt-clang)

target_link_libraries(circt-clang
  PRIVATE
  libclang
  clangFrontend
  clangTooling
  clangBasic
  #clangASTMatchers
)

