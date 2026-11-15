#include "qdc_domain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#define TEST_OPTIONAL __attribute__((weak_import))
#else
#define TEST_OPTIONAL __attribute__((weak))
#endif
extern int qdc_pdu_submit(qdc_slice, qdc_slice, unsigned, qdc_pdus *, qdc_error *) TEST_OPTIONAL;
extern int qdc_pdu_deliver(qdc_slice, qdc_part *, qdc_error *) TEST_OPTIONAL;
extern void qdc_pdus_free(qdc_pdus *) TEST_OPTIONAL;
extern void qdc_part_free(qdc_part *) TEST_OPTIONAL;

int main(void) {
    assert(qdc_pdu_submit && "PDU submit is missing: exact-byte/NUL contract unimplemented");
    qdc_error e = {0};
    qdc_pdus p = {0};
    assert(qdc_pdu_submit((qdc_slice){"10010", 5}, (qdc_slice){"123456", 6}, 1, &p, &e) == 0);
    assert(p.len == 1);
    assert(p.items[0].len == 34);
    assert(!memcmp(p.items[0].data, "00010005810110F000000631D98C56B301", 34));
    qdc_pdus_free(&p);
    const char text[] = {'A', 0, 'B'};
    assert(qdc_pdu_submit((qdc_slice){"10010", 5}, (qdc_slice){text, sizeof text}, 1, &p, &e) == 0);
    assert(p.len == 1 && strstr((char *)p.items[0].data, "0806004100000042"));
    qdc_pdus_free(&p);
    assert(qdc_pdu_submit((qdc_slice){"10010;ATD", 9}, (qdc_slice){"test", 4}, 1, &p, &e) < 0);
    assert(qdc_pdu_deliver);
    const char *raw =
        "0891683108000000F0400881019600000008620140210591233E0500032A02013010793A4F8B79D16280301160"
        "A876849A8C8BC17801662F0020003600350034003300320031FF0C003500205206949F5185670965483002";
    qdc_part part = {0};
    assert(qdc_pdu_deliver((qdc_slice){raw, strlen(raw)}, &part, &e) == 0);
    assert(part.ref == 42 && part.seq == 1 && part.total == 2);
    assert(part.sender.len == 8 && !memcmp(part.sender.data, "10690000", 8));
    assert(strstr((char *)part.body.data, "654321"));
    qdc_part_free(&part);
    qdc_part otp = {0};
    const char raw_body[] = {'A', 0, '1', '2', '3', '4'};
    assert(!qdc_buf_puts(&otp.sender, "10010"));
    assert(!qdc_buf_append(&otp.body, raw_body, sizeof raw_body));
    otp.ref = -1;
    otp.total = otp.seq = 1;
    strcpy(otp.status, "0");
    yyjson_doc *merged = qdc_merge_parts(&otp, 1);
    assert(merged);
    yyjson_val *message = yyjson_arr_get(yyjson_doc_get_root(merged), 0);
    assert(qdc_slice_eq(qdc_json_slice(yyjson_obj_get(message, "code")), "1234"));
    qdc_slice key = qdc_json_slice(yyjson_obj_get(message, "key"));
    assert(key.len >= 12 && !memcmp(key.ptr + key.len - 12, "410031323334", 12));
    yyjson_doc_free(merged);
    qdc_part_free(&otp);
    puts("DOMAIN_PDU_PASS exact_bytes NUL invalid_number deliver");
}
