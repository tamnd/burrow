#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/math/cmplx.h"

int main(void) {
    // doc: basic
    Complex128 z = {3, 4};
    double r = cmplx_abs(z);                            /* 5 */
    Complex128 w = cmplx_sqrt((Complex128){-4, 0});     /* 0+2i */
    Complex128 e = cmplx_exp((Complex128){0, MATH_PI}); /* -1, nearly */
    // doc: end
    printf("%g (%g%+gi) (%g%+.3gi)\n", r, w.re, w.im, e.re, e.im);

    // doc: ops
    Complex128 a = {1, 2}, b = {3, 4};
    Complex128 p = cmplx_mul(a, b);                    /* -5+10i */
    Complex128 q = cmplx_div(a, b);                    /* 0.44+0.08i */
    Complex128 inf = cmplx_div(a, (Complex128){0, 0}); /* +Inf+Inf i, not a crash */
    // doc: end
    printf("(%g%+gi) (%g%+gi) (%g%+gi)\n", p.re, p.im, q.re, q.im, inf.re, inf.im);

    // doc: polar
    double theta;
    double m = cmplx_polar((Complex128){0, 2}, &theta); /* 2 and Pi/2 */
    Complex128 back = cmplx_rect(m, theta);
    // doc: end
    printf("%g %g (%.3g%+gi)\n", m, theta / MATH_PI, back.re, back.im);
    return 0;
}

/* Output:
5 (0+2i) (-1+1.22e-16i)
(-5+10i) (0.44+0.08i) (inf+infi)
2 0.5 (1.22e-16+2i)
*/
