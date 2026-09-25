#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/math.h"

int main(void) {
    // doc: basic
    double h = math_hypot(3, 4);        /* 5 */
    double r = math_round(-2.5);        /* -3, half away from zero */
    double e = math_round_to_even(2.5); /* 2 */
    double p = math_pow(2, 0.5);        /* the square root of 2 */
    // doc: end
    printf("%g %g %g %.17g\n", h, r, e, p);

    // doc: two
    Int exp;
    double frac = math_frexp(48, &exp); /* 0.75 and 6, since 48 is 0.75 * 2^6 */
    double c;
    double s = math_sincos(MATH_PI / 6, &c);
    double whole = math_modf(-3.25, NULL); /* -3, the fraction is not wanted */
    // doc: end
    printf("%g %d %.17g %.17g %g\n", frac, (int)exp, s, c, whole);

    // doc: special
    double nan = math_sqrt(-1);
    bool isnan = math_is_nan(nan);
    bool inf = math_is_inf(math_log(0), -1); /* log(0) is -Inf */
    // doc: end
    printf("%d %d\n", isnan, inf);
    return 0;
}

/* Output:
5 -3 2 1.4142135623730951
0.75 6 0.49999999999999994 0.86602540378443871 -3
1 1
*/
