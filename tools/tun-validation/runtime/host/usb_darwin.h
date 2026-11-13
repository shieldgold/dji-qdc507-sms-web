#include <stdint.h>
typedef struct qdc_usb qdc_usb;
qdc_usb *qdc_open(uint16_t vid, uint16_t pid, uint32_t location);
void qdc_close(qdc_usb *h);
int qdc_write(qdc_usb *h, const void *p, uint32_t len);
int qdc_read(qdc_usb *h, void *p, uint32_t len, uint32_t timeout,
             uint32_t *count);
const char *qdc_error(void);
