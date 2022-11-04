
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