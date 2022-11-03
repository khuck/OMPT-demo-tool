module load rocmmod5.2.0
export CC=amdclang
export CXX=hipcc
export OMP_TOOL=enabled
export OMP_TOOL_LIBRARIES=./libompt_tt.so
export OMP_TOOL_VERBOSE_INIT=stdout
export OMP_DISPLAY_ENV=true