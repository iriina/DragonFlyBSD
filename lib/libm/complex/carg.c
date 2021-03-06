/* $NetBSD: carg.c,v 1.1 2007/08/20 16:01:31 drochner Exp $ */

/*
 * Written by Matthias Drochner <drochner@NetBSD.org>.
 * Public domain.
 */


#include <complex.h>
#include <math.h>

double
carg(double complex z)
{

	return atan2(cimag(z), creal(z));
}
