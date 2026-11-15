#ifndef QDC_MONITOR_H
#define QDC_MONITOR_H
#include "qdc.h"
typedef struct {
    char updated_at[40], technology[256], operator_name[256], band[256], channel[256];
    bool registered;
    char registration[256], rsrp[256], rsrq[256], sinr[256], apn[256], ims[256], error[256];
} qdc_network;
typedef struct {
    uint64_t upload, download;
} qdc_count;
typedef struct {
    char key[16];
    qdc_count count;
} qdc_period;
typedef struct qdc_traffic {
    pthread_mutex_t mu;
    char path[QDC_PATH_MAX], boot[128], since[40];
    bool initialized, load_failed;
    uint64_t last_rx, last_tx;
    int64_t last_ms;
    qdc_count total, session, rate;
    qdc_period *days, *months;
    size_t day_count, month_count;
} qdc_traffic;
int qdc_network_apply(qdc_network *, const char *);
qdc_traffic *qdc_traffic_load(const char *, qdc_error *);
void qdc_traffic_free(qdc_traffic *);
int qdc_traffic_sample(qdc_traffic *, const char *, uint64_t, uint64_t, int64_t);
yyjson_mut_doc *qdc_traffic_view(qdc_traffic *, int64_t);
int qdc_traffic_write(qdc_traffic *, qdc_error *);
#endif
