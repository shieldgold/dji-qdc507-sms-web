#include "fake_modem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct sms_fake fake = {.pin_locked=1, .messages=1};
    char out[4096];
    size_t n = sms_fake_command(&fake, "AT", out, sizeof out);
    assert(n == 6 && !strcmp(out, "\r\nOK\r\n"));
    sms_fake_command(&fake, "AT+CPIN?", out, sizeof out);
    assert(strstr(out, "SIM PIN") && fake.pin_locked);
    sms_fake_command(&fake, "AT+CPIN=\"0000\"", out, sizeof out);
    assert(strstr(out, "ERROR") && fake.pin_locked && fake.unlocks == 0);
    sms_fake_command(&fake, "AT+CPIN=\"1234\"", out, sizeof out);
    assert(strstr(out, "OK") && !fake.pin_locked && fake.unlocks == 1);
    sms_fake_command(&fake, "AT+CMGL=4", out, sizeof out);
    assert(strstr(out, "+CMGL: 1,0,,") && strstr(out, "+CMGL: 2,0,,") && fake.polls == 1);
    sms_fake_command(&fake, "AT+CMGS=20", out, sizeof out);
    assert(!strcmp(out, "\r\n> ") && !strchr(out+2, '\n') && fake.awaiting_pdu);
    sms_fake_control(&fake, 0x1b, out, sizeof out);
    assert(!fake.awaiting_pdu && fake.cancelled == 1 && fake.submitted == 0);
    sms_fake_command(&fake, "AT+CMGS=20", out, sizeof out);
    sms_fake_control(&fake, 0x1a, out, sizeof out);
    assert(strstr(out, "+CMGS: 1") && fake.submitted == 1 && !fake.awaiting_pdu);
    sms_fake_command(&fake, "AT+NOT_SUPPORTED", out, sizeof out);
    assert(strstr(out, "ERROR"));
    assert(sms_fake_command(&fake, "AT", out, 3) == 0);
    puts("FAKE_MODEM_TEST_PASS");
    return 0;
}
