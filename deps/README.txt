Building
--------

Ubuntu:
```
sudo apt install build-essential cmake libboost-dev libboost-doc libgtest-dev libboost-chrono-dev \
    libboost-locale-dev libboost-filesystem-dev
```

Other 3rd party dependencies.

# SIMDCompressionAndIntersection is only dependency here currently
# and it currently is a manual setup.

#For linux/unix: in the "deps" directory
$ git clone git@github.com:lemire/SIMDCompressionAndIntersection.git simdcomp
$ cd simdcomp
$ DEBUG=1 make
$ cp libSIMDCompressionAndIntersection.a libsimdcomp_ad.a
$ make clean
$ make
$ cp libSIMDCompressionAndIntersection.a libsimdcomp_a.a
$ make clean

