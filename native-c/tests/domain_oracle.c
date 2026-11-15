#include "qdc_domain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        qdc_error e = {0};
        yyjson_doc *in = qdc_json_read(line, (size_t)n, &e);
        if (!in)
            return 2;
        yyjson_val *r = yyjson_doc_get_root(in);
        qdc_pdus p = {0};
        int rc = qdc_pdu_submit(qdc_json_slice(yyjson_obj_get(r, "number")),
                                qdc_json_slice(yyjson_obj_get(r, "text")),
                                (unsigned)yyjson_get_uint(yyjson_obj_get(r, "ref")), &p, &e);
        yyjson_mut_doc *d = qdc_json_object();
        yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
        yyjson_mut_obj_add_bool(d, o, "ok", rc == 0);
        if (!rc) {
            yyjson_mut_val *a = yyjson_mut_obj_add_arr(d, o, "pdus");
            for (size_t i = 0; i < p.len; i++)
                yyjson_mut_arr_add_strncpy(d, a, (char *)p.items[i].data, p.items[i].len);
        }
        size_t len;
        char *out = yyjson_mut_write(d, 0, &len);
        fwrite(out, 1, len, stdout);
        putchar('\n');
        free(out);
        yyjson_mut_doc_free(d);
        yyjson_doc_free(in);
        qdc_pdus_free(&p);
    }
    free(line);
}
