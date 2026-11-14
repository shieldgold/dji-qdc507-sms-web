#ifndef QDC_IKE_COEXIST_H
#define QDC_IKE_COEXIST_H
#include "proc.h"
struct qdc_coexist {
    char run[49], token[49], boot[37], http[16];
    struct proc_id supervisor, original_sms, guardian, sms;
    int seconds, active;
};
int qdc_coexist_parse(const char *, struct qdc_coexist *);
int qdc_coexist_load(const char *, const char *, struct qdc_coexist *);
int qdc_coexist_preflight(const char *, struct qdc_coexist *);
int qdc_coexist_begin(const char *, struct qdc_coexist *);
/* 0: waiting for restore; 1: restored and healthy; -1: unsafe. */
int qdc_coexist_restored(const char *, struct qdc_coexist *);
int qdc_coexist_alive(const struct qdc_coexist *);
#endif
