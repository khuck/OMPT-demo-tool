module purge
module load nvhpc/22.11 cmake
export CC=nvc
export CXX=nvc++
export OMP_TOOL=enabled
export OMP_TOOL_LIBRARIES=./libompt_tt.so
export OMP_TOOL_VERBOSE_INIT=stdout
export OMP_DISPLAY_ENV=true
export OMP_NUM_THREADS=4