#include "qdc_domain.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static yyjson_doc *records(int iteration) {
    yyjson_mut_doc *d = qdc_json_array();
    yyjson_mut_val *a = yyjson_mut_doc_get_root(d);
    for (unsigned i = 0; i < 300; i++) {
        yyjson_mut_val *v = yyjson_mut_obj(d);
        char key[32];
        snprintf(key, sizeof(key), "test-memory-%u", i);
        yyjson_mut_obj_add_strcpy(d, v, "key", key);
        yyjson_mut_obj_add_str(d, v, "body", "synthetic memory test");
        yyjson_mut_obj_add_str(d, v, "sender", "synthetic");
        yyjson_mut_obj_add_str(d, v, "time", "2026-11-15 00:00:00");
        yyjson_mut_obj_add_str(d, v, "direction", "in");
        yyjson_mut_obj_add_str(d, v, "status", iteration % 2 ? "已读" : "未读");
        yyjson_mut_obj_add_int(d, v, "index", iteration + i);
        yyjson_mut_arr_add_val(a, v);
    }
    yyjson_doc *out = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    return out;
}
int main(void) {
    char dir[] = "/tmp/qdc-store-memory-XXXXXX";
    assert(mkdtemp(dir));
    qdc_domain domain = {0};
    qdc_app app = {.domain = &domain, .config = {.data_dir = dir}};
    qdc_error e = {0};
    assert(!qdc_store_init(&app, &e));
    yyjson_doc *d = records(0);
    int added;
    assert(!qdc_store_merge(&app, yyjson_doc_get_root(d), &added, &e) && added == 300);
    size_t baseline = qdc_store_test_pool(&app);
    for (unsigned i = 0; i < 1000; i++) {
        assert(!qdc_store_merge(&app, yyjson_doc_get_root(d), &added, &e) && added == 0);
    }
    size_t steady = qdc_store_test_pool(&app);
    printf("store pool baseline=%zu steady=%zu\n", baseline, steady);
    fflush(stdout);
    assert(steady <= baseline + 4096);
    yyjson_doc_free(d);
    for (unsigned i = 0; i < 1000; i++) {
        d = records((int)i);
        assert(!qdc_store_merge(&app, yyjson_doc_get_root(d), &added, &e) && added == 0);
        yyjson_doc_free(d);
        assert(qdc_store_test_pool(&app) <= baseline + 8192);
    }
    qdc_store_free(&app);
    char file[4096];
    qdc_path(file, sizeof(file), dir, "messages.json");
    unlink(file);
    rmdir(dir);
    puts("store 300 messages / 2000 polls bounded: PASS");
    return 0;
}
