
Simple tool to test OpenMP runtime compliance with the OMPT 5.2 standard.

To use with AMD or NVHPC compilers, source the appropriate `sourceme-*.sh` file
and then:
```
mkdir build
cd build
cmake ..
make
./simple
```
At the UO, we can use gilgamesh for NVIDIA and CUDA, or jupiter for Intel, or Omnia for all three.

For more verbose output, run with VERBOSE=1 (environment variable):
```
VERBOSE=1 ./build/simple
```
or:
```
export VERBOSE=1
./build/simple
```