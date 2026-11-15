#include "qdc_monitor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    qdc_network n = {0};
    assert(qdc_network_apply(&n, "+QNWINFO: \"FDD LTE\",\"46001\",\"LTE BAND 3\",1650") == 0);
    assert(!strcmp(n.technology, "FDD LTE") && !strcmp(n.band, "LTE BAND 3"));
    assert(qdc_network_apply(&n, "+CEREG: 2,5") == 0 && n.registered);
    assert(qdc_network_apply(&n, "+QCSQ: \"LTE\",-65,-91,100,-11") == 0 &&
           !strcmp(n.sinr, "0.0 dB"));
    assert(qdc_network_apply(&n, "+QCSQ: \"NOSERVICE\"") == 0 && !n.sinr[0]);
    char dir[] = "/tmp/qdc-traffic-XXXXXX";
    assert(mkdtemp(dir));
    qdc_error e = {0};
    qdc_traffic *t = qdc_traffic_load(dir, &e);
    assert(t);
    int64_t now;
    assert(qdc_parse_rfc3339((qdc_slice){"2026-11-15T15:59:59Z", 20}, &now) == 0);
    assert(!qdc_traffic_sample(t, "a", 100, 200, now));
    assert(!qdc_traffic_sample(t, "a", 150, 270, now + 2000));
    assert(t->total.upload == 50 && t->total.download == 70 && t->rate.upload == 25);
    assert(!qdc_traffic_sample(t, "a", 1, 2, now + 4000) && t->total.upload == 51);
    assert(!qdc_traffic_sample(t, "b", 3, 4, now + 6000) && t->session.upload == 3 &&
           t->total.upload == 54);
    t->total.upload = UINT64_MAX;
    assert(!qdc_traffic_write(t, &e));
    qdc_traffic_free(t);
    t = qdc_traffic_load(dir, &e);
    assert(t && t->total.upload == UINT64_MAX);
    yyjson_mut_doc *v = qdc_traffic_view(t, now);
    assert(v);
    yyjson_mut_doc_free(v);
    qdc_traffic_free(t);
    char p[4096];
    qdc_path(p, sizeof(p), dir, "traffic.json");
    unlink(p);
    rmdir(dir);
    puts("monitor/traffic: PASS");
    return 0;
}
