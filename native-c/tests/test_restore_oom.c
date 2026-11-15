#include "qdc_domain.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool fail_zero_int;
static yyjson_mut_val *injected_int(yyjson_mut_doc *, int64_t);
#define yyjson_mut_int injected_int
#include "../src/store.c"
#undef yyjson_mut_int
static yyjson_mut_val *injected_int(yyjson_mut_doc *d, int64_t n) {
    if (fail_zero_int && n == 0) {
        fail_zero_int = false;
        return NULL;
    }
    return yyjson_mut_int(d, n);
}
int main(void) {
    char dir[] = "/tmp/qdc-review-restore-XXXXXX";
    assert(mkdtemp(dir));
    qdc_domain domain = {0};
    qdc_app app = {.domain = &domain, .config = {.data_dir = dir}};
    qdc_error e = {0};
    assert(!qdc_store_init(&app, &e));
    const char *text = "[{\"key\":\"import\",\"sender\":\"10010\",\"body\":\"hello\",\"direction\":"
                       "\"in\",\"index\":99}]";
    yyjson_doc *input = qdc_json_read(text, strlen(text), &e);
    assert(input);
    int added = 0;
    fail_zero_int = true;
    int rc = qdc_store_restore(&app, yyjson_doc_get_root(input), &added, &e);
    yyjson_doc_free(input);
    yyjson_doc *m = qdc_store_get(&app, (qdc_slice){"import", 6});
    assert(!m);
    assert(rc == -1 && added == 0);
    qdc_store_free(&app);
    char path[QDC_PATH_MAX];
    qdc_path(path, sizeof(path), dir, "messages.json");
    unlink(path);
    qdc_path(path, sizeof(path), dir, "messages.before-restore.json");
    unlink(path);
    rmdir(dir);
    assert(rc == -1 && added == 0);
    return 0;
}
