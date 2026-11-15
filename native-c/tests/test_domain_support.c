#include "qdc.h"
#include <assert.h>
/* Deliberate external-module seams: no push transport/audio device is exercised. */
void qdc_push_bind(qdc_app *a, const char *id) {
    (void)id;
    assert(a->push == NULL);
}
bool qdc_audio_available(qdc_app *a) {
    (void)a;
    return true;
}
yyjson_doc *qdc_audio_snapshot(qdc_app *a) {
    (void)a;
    yyjson_mut_doc *m = qdc_json_object();
    yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
    yyjson_mut_obj_add_false(m, r, "available");
    yyjson_mut_obj_add_str(m, r, "state", "idle");
    yyjson_mut_obj_add_uint(m, r, "up_bytes", 0);
    yyjson_mut_obj_add_uint(m, r, "down_bytes", 0);
    yyjson_doc *d = yyjson_mut_doc_imut_copy(m, NULL);
    yyjson_mut_doc_free(m);
    return d;
}
