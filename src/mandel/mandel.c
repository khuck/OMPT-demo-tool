#include <stdio.h>
#include <stdlib.h>

extern void mytimer_(int *);

#include <omp.h>
#include "ppm.h"

field iterations;
int main(int argc, char *argv[]) {
  double xmin, xmax, ymin, ymax, dx, dy;
  int numpe, maxiter, ix, iy;

  if ( argc != 6 ) {
    fprintf(stderr, "%s: xmin xmax ymin ymax maxiter\n", argv[0]);
    fprintf(stderr, "Using defaults: -.59 -.56 .47 .5 216\n");
    xmin = -.59; xmax = -.56; ymin = .47; ymax = .5; maxiter = 216;
  }
  else {
    xmin = strtod(argv[1], 0);
    xmax = strtod(argv[2], 0);
    ymin = strtod(argv[3], 0);
    ymax = strtod(argv[4], 0);
    maxiter =  atoi(argv[5]);
  }

  /* --- initialization -- */
  numpe = 1;
  dx = (xmax - xmin) / width;
  dy = (ymax - ymin) / height;

  /* --- calculate mandelbrot set -- */
  mytimer_(0);
#pragma omp parallel
  {
#ifdef _OPENMP
    numpe = omp_get_num_threads();
#endif
#pragma omp for private(ix,iy)
  for (ix=0; ix<width; ++ix) {
    double x = xmin + ix*dx;
    for (iy=0; iy<height; ++iy) {
      double y = ymin + iy*dy;
      double zx, zy, cx, cy, ox, oy;
      int count;
      zx = 0.0; zy = 0.0;
      ox = 0.0; oy = 0.0;
      cx = x;   cy = y;
      count = 0;
      while ( (ox*ox + oy*oy) < 16 && count < maxiter ) {
	zx = ox*ox-oy*oy+cx;
        zy = ox*oy+ox*oy+cy;
        ++count;
	ox = zx;
	oy = zy;
      }
      iterations[ix][iy] = count;
    }
  }
  }
  mytimer_(&numpe);

  /* --- generate ppm file -- */
  printf("Writing picture ...\n");
  ppmwrite("mandel.ppm", iterations, maxiter);
  exit(0);
}

