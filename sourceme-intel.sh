module purge
module load cmake
source /opt/intel/oneapi/setvars.sh
export CC=icx
export CXX=icpx
export OMP_TOOL=enabled
export OMP_TOOL_LIBRARIES=./libompt_tt.so
export OMP_TARGET_OFFLOAD=mandatory
#export OMP_TARGET_DEBUG=2
#export OMP_TARGET_INFO=4
#export OMP_TOOL_VERBOSE_INIT=stdout
export OMP_DISPLAY_ENV=true
export OMP_NUM_THREADS=4
PATH=/usr/local/packages/gdb/12.1/bin:${PATH}