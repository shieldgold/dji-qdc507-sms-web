/* Computer-only synthetic AT model. It has no device or network operations. */
#ifndef SMS_LAB_FAKE_MODEM_H
#define SMS_LAB_FAKE_MODEM_H
#include <stddef.h>
struct sms_fake {
    unsigned commands, polls, submitted, cancelled, unlocks;
    int pin_locked, awaiting_pdu, messages;
};
size_t sms_fake_command(struct sms_fake *, const char *, char *, size_t);
size_t sms_fake_control(struct sms_fake *, unsigned char, char *, size_t);
#endif
