#ifndef QDC_SMS_MANUAL_CORE_H
#define QDC_SMS_MANUAL_CORE_H
#include "../swap/swap_core.h"

#define QDC_SMS_MANUAL_SECONDS 5400
#define QDC_SMS_SOAK_SECONDS 3600
int manual_control_parse(const char *, const char *, struct swap_control *);
int manual_due(double, int);
int manual_commit_parse(const char *, const char *);
int manual_soak_proof(const char *, const char *);
struct manual_soak_control {
 char run[49],boot[37],http[16];
 pid_t supervisor,sms;
 uint64_t sup_start,sms_start,source_dev,source_ino,source_bytes;
 long baseline;
};
int manual_soak_control_parse(const char *,const char *,struct manual_soak_control *);
int manual_esp_changed(const char *,const char *);
#endif
