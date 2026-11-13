#include <stddef.h>
struct qdc_mem { long rss, hwm, vsize, threads; };
int qdc_parse_mem(const char *text, struct qdc_mem *out);
struct qdc_smaps { long rss, pss, anonymous, dirty, clean; };
int qdc_parse_smaps(const char *text, struct qdc_smaps *out);
