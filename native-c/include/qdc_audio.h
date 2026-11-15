#ifndef QDC_AUDIO_H
#define QDC_AUDIO_H
#include "qdc.h"
bool qdc_calibration_alive(const char *marker, const char *proc_root);
#ifdef QDC_TESTING
int qdc_audio_test_paths(qdc_app *, const char *pcm, const char *marker, const char *proc,
                         const char *log, unsigned port);
#endif
#endif
