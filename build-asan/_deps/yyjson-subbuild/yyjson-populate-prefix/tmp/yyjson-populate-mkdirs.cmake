# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/liu/Netune-beta/build-asan/_deps/yyjson-src")
  file(MAKE_DIRECTORY "/home/liu/Netune-beta/build-asan/_deps/yyjson-src")
endif()
file(MAKE_DIRECTORY
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-build"
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix"
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/tmp"
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/src/yyjson-populate-stamp"
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/src"
  "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/src/yyjson-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/src/yyjson-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/liu/Netune-beta/build-asan/_deps/yyjson-subbuild/yyjson-populate-prefix/src/yyjson-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
