Building
--------

Installing dependencies via vcpkg:
$ cd /opt/vcpkg
$ ./vcpkg install boost-core boost-sort boost-thread gtest benchmark xxhash gtl protobuf grpc spdlog lz4 cli11 faiss
$ ./vcpkg install robin-hood-hashing   #optional... see MapBM.cpp

NOTE: when using address sanitizer, newer gRCP/proto will be hit with "use after poison" errors
if the libraries themselves are not built with address sanitizer.  Easiest way is this:
diff --git a/triplets/x64-linux.cmake b/triplets/x64-linux.cmake
index 8822134560..777ce1ea65 100644
--- a/triplets/x64-linux.cmake
+++ b/triplets/x64-linux.cmake
@@ -4,3 +4,7 @@ set(VCPKG_LIBRARY_LINKAGE static)

 set(VCPKG_CMAKE_SYSTEM_NAME Linux)

+#YCS
+set(VCPKG_CXX_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address")
+set(VCPKG_C_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address")
+set(VCPKG_LINKER_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address")



Ubuntu:
```
sudo apt install libtbb-dev    #TODO - try the tbb in vcpkg
```

Other 3rd party dependencies:
TBB: the vcpkg version for TBB is currently out of date. On Ubuntu 22.04, use sudo apt install libtbb-dev

SIMDCompressionAndIntersection 
NOTE: The debugging version of libsimdcomp is currently built with -D_GLIBCXX_DEBUG, which is
incompatible with c++ source built without that flag (things crash). Out current workaround
is to remove that flag from the Makefile of libsimdcomp.

Easiest way to get all debugging libs built this way is to modify the vcpkg toolchain:

/opt/vcpkg$ git diff
diff --git a/scripts/toolchains/linux.cmake b/scripts/toolchains/linux.cmake
index fb5666538..35732451e 100644
--- a/scripts/toolchains/linux.cmake
+++ b/scripts/toolchains/linux.cmake
@@ -39,7 +39,8 @@ if(NOT _CMAKE_IN_TRY_COMPILE)
     string(APPEND CMAKE_C_FLAGS_INIT " -fPIC ${VCPKG_C_FLAGS} ")
     string(APPEND CMAKE_CXX_FLAGS_INIT " -fPIC ${VCPKG_CXX_FLAGS} ")
     string(APPEND CMAKE_C_FLAGS_DEBUG_INIT " ${VCPKG_C_FLAGS_DEBUG} ")
-    string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " ${VCPKG_CXX_FLAGS_DEBUG} ")
+    #string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " ${VCPKG_CXX_FLAGS_DEBUG} ")
+    string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " -D_GLIBCXX_DEBUG ${VCPKG_CXX_FLAGS_DEBUG} ")
     string(APPEND CMAKE_C_FLAGS_RELEASE_INIT " ${VCPKG_C_FLAGS_RELEASE} ")
     string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " ${VCPKG_CXX_FLAGS_RELEASE} ")


#For linux/unix: in the "deps" directory
### for simdcomp (TODO: automate / integrate into build system if we keep the whole thing)
# remove _GLIBCXX_DEBUG flags from Makefile
$ git clone git@github.com:lemire/SIMDCompressionAndIntersection.git simdcomp
# apply simdcomp.diff
  cd simdcomp
  DEBUG=1 make
  cp libSIMDCompressionAndIntersection.a libsimdcomp_ad.a
  make clean
  make
  cp libSIMDCompressionAndIntersection.a libsimdcomp_a.a
  make clean
