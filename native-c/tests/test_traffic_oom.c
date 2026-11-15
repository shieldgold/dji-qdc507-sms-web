#include "qdc_monitor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool fail_download;
static bool injected_uint(yyjson_mut_doc *, yyjson_mut_val *, const char *, uint64_t);
#define yyjson_mut_obj_add_uint injected_uint
#include "../src/traffic.c"
#undef yyjson_mut_obj_add_uint
static bool injected_uint(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, uint64_t n) {
    if (fail_download && !strcmp(key, "download")) {
        fail_download = false;
        return false;
    }
    return yyjson_mut_obj_add_uint(d, o, key, n);
}
int main(void) {
    char dir[] = "/tmp/qdc-review-traffic-XXXXXX";
    assert(mkdtemp(dir));
    qdc_error e = {0};
    qdc_traffic *t = qdc_traffic_load(dir, &e);
    assert(t);
    t->total = (qdc_count){123, 456};
    assert(!qdc_traffic_write(t, &e));
    fail_download = true;
    int rc = qdc_traffic_write(t, &e);
    qdc_traffic_free(t);
    t = qdc_traffic_load(dir, &e);
    assert(t);
    printf("write_rc=%d reload_failed=%d total_up=%llu total_down=%llu\n", rc, t->load_failed,
           (unsigned long long)t->total.upload, (unsigned long long)t->total.download);
    assert(rc == -1 && !t->load_failed && t->total.upload == 123 && t->total.download == 456);
    char path[QDC_PATH_MAX];
    qdc_path(path, sizeof(path), dir, "traffic.json");
    qdc_traffic_free(t);
    unlink(path);
    rmdir(dir);
    return 0;
}
