# build.bat / setup.ps1: fails under an MSYS2/Cygwin cmake, which reads Windows paths such as D:/Games as
# relative (the build then looks for /d/<folder>/D:/Games/...). A native Windows CMake passes.
if(NOT CMAKE_HOST_WIN32 OR NOT IS_ABSOLUTE "D:/x")
  message(FATAL_ERROR "not a native Windows CMake")
endif()
