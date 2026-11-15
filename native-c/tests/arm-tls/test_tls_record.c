#include "tls_record.h"
#include <assert.h>
#include <stdio.h>

static struct lab_tls_records parse(const unsigned char *p, size_t n) {
    struct lab_tls_records records = {0};
    /* Exercise every TLS record boundary as a one-byte socket fragment. */
    for (size_t i = 0; i < n; i++) lab_tls_records_feed(&records, p + i, 1);
    return records;
}
int main(void) {
    const unsigned char combined[] = {22,3,1,0,3,1,2,3,21,3,3,0,2,2,48};
    struct lab_tls_records r = parse(combined, sizeof(combined));
    assert(r.certificate_alert && r.description == 48 && !r.malformed);
    const unsigned descriptions[] = {42,46,48};
    for (size_t i = 0; i < sizeof(descriptions)/sizeof(descriptions[0]); i++) {
        unsigned char record[] = {21,3,3,0,2,2,(unsigned char)descriptions[i]};
        r = parse(record, sizeof(record));
        assert(r.certificate_alert && r.description == descriptions[i]);
    }
    const unsigned char bad[][7] = {
        {23,3,3,0,2,2,48}, /* encrypted application data is not an alert */
        {21,3,3,0,2,1,48}, /* a warning is not fatal */
        {21,3,3,0,2,2,20}, /* bad_record_mac is not certificate rejection */
        {21,3,1,0,2,2,48}, /* this fixture requires the negotiated TLS 1.2 record version */
        {21,3,3,0,3,2,48}, /* incomplete and wrong-sized record */
        {21,3,3,255,255,2,48}, /* oversized record cannot be accepted */
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        r = parse(bad[i], sizeof(bad[i]));
        assert(!r.certificate_alert);
    }
    puts("ARM TLS record unit: PASS (fragmented concrete certificate fatal alerts only)");
    return 0;
}
