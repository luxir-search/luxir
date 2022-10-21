Building
--------

Installing dependencies via vcpkg:
$ cd /opt/vcpkg
$ ./vcpkg install boost gtest benchmark xxhash gtl protobuf grpc spdlog
$ ./vcpkg install robin-hood-hashing   #optional... see MapBM.cpp
# TODO: while having all of boost installed is useful for development, it drags in a ton of dependencies +
# build time in vcpkg.  We should narrow this to just the parts of boost we need.

Ubuntu:
```
sudo apt install build-essential cmake libboost-dev libboost-doc libgtest-dev libboost-chrono-dev \
    libboost-locale-dev libboost-filesystem-dev
```

Other 3rd party dependencies:
TBB: the vcpkg version is currently out of date. On Ubuntu 22.04, use sudo apt install libtbb-dev

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
$ cd simdcomp
$ DEBUG=1 make
$ cp libSIMDCompressionAndIntersection.a libsimdcomp_ad.a
$ make clean
$ make
$ cp libSIMDCompressionAndIntersection.a libsimdcomp_a.a
$ make clean
### for CRoaring
$ cd deps; git clone https://github.com/RoaringBitmap/CRoaring
$ cd CRoaring; git co tags/v0.4.0

