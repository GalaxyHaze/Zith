# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/build/_deps/unordered_dense-src"
  "/workspace/build/_deps/unordered_dense-build"
  "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix"
  "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/tmp"
  "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/src/unordered_dense-populate-stamp"
  "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/src"
  "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/src/unordered_dense-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/src/unordered_dense-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/build/_deps/unordered_dense-subbuild/unordered_dense-populate-prefix/src/unordered_dense-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
