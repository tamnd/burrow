#include "burrow/burrow.h"

#include <string.h>

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: read
    MetricsSample s[4];
    memset(s, 0, sizeof s);
    s[0].name = BURROW_S("/gc/gogc:percent");
    s[1].name = BURROW_S("/gc/stack/starting-size:bytes");
    s[2].name = BURROW_S("/gc/heap/allocs-by-size:bytes");
    s[3].name = BURROW_S("/no/such:metric");
    metrics_read(a, slice_from(s, 4, 4, TYPE_METRICS_SAMPLE));

    fmt_printf_v("%s %d\n", s[0].name, metrics_value_uint64(s[0].value));
    fmt_printf_v("%s %d\n", s[1].name, metrics_value_uint64(s[1].value));
    MetricsFloat64Histogram *h = metrics_value_float64_histogram(s[2].value);
    fmt_printf_v("%s %d buckets\n", s[2].name, h->buckets.len);
    bool bad = metrics_value_kind(s[3].value) == METRICS_KIND_BAD;
    fmt_printf_v("%s bad: %t\n", s[3].name, bad);
    // doc: end
    arena_free(&ar);
    return 0;
}

/* Output:
/gc/gogc:percent 100
/gc/stack/starting-size:bytes 262144
/gc/heap/allocs-by-size:bytes 69 buckets
/no/such:metric bad: true
*/
