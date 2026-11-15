#include "fake_modem.h"
#include <stdio.h>
#include <string.h>

/* Public, synthetic vectors from pdu_test.go; never read real SMS state. */
static const char pdu_a[] = "0891683108000000F0400881019600000008620140210591233E0500032A02013010793A4F8B79D16280301160A876849A8C8BC17801662F0020003600350034003300320031FF0C003500205206949F5185670965483002";
static const char pdu_b[] = "0891683108000000F040088101960000000862014021059123220500032A02025982975E672C4EBA64CD4F5CFF0C8BF75FFD7565672C77ED4FE13002";

static size_t result(char *out, size_t cap, const char *value) {
    size_t n = strlen(value);
    if (n >= cap) return 0;
    memcpy(out, value, n+1);
    return n;
}

size_t sms_fake_command(struct sms_fake *s, const char *command, char *out, size_t cap) {
    s->commands++;
    if (!strcmp(command, "AT+CPIN?"))
        return result(out, cap, s->pin_locked ? "\r\n+CPIN: SIM PIN\r\nOK\r\n" : "\r\n+CPIN: READY\r\nOK\r\n");
    if (!strncmp(command, "AT+CPIN=", 8)) {
        if (strcmp(command, "AT+CPIN=\"1234\"")) return result(out, cap, "\r\n+CME ERROR: 16\r\n");
        s->pin_locked = 0; s->unlocks++;
        return result(out, cap, "\r\nOK\r\n");
    }
    if (!strcmp(command, "AT+CMGL=4")) {
        s->polls++;
        if (s->pin_locked) return result(out, cap, "\r\n+CME ERROR: 11\r\n");
        if (!s->messages) return result(out, cap, "\r\nOK\r\n");
        int n = snprintf(out, cap, "\r\n+CMGL: 1,0,,80\r\n%s\r\n+CMGL: 2,0,,48\r\n%s\r\nOK\r\n", pdu_a, pdu_b);
        return n > 0 && (size_t)n < cap ? (size_t)n : 0;
    }
    if (!strncmp(command, "AT+CMGS=", 8)) {
        if (s->pin_locked) return result(out, cap, "\r\n+CME ERROR: 11\r\n");
        s->awaiting_pdu = 1;
        return result(out, cap, "\r\n> "); /* Deliberately no trailing newline. */
    }
    if (!strncmp(command, "AT+CMGD=", 8)) {
        s->messages = 0;
        return result(out, cap, "\r\nOK\r\n");
    }
    struct { const char *command, *value; } table[] = {
        {"AT", ""}, {"ATE0", ""}, {"AT+CMGF=0", ""},
        {"AT+CPMS=\"ME\",\"ME\",\"ME\"", "+CPMS: 2,100,2,100,2,100\r\n"},
        {"AT+CNMI=2,1,0,0,0", ""},
        {"AT+CSQ", "+CSQ: 20,99\r\n"},
        {"AT+COPS?", "+COPS: 0,0,\"SYNTHETIC LAB\",7\r\n"},
        {"AT+CGMI", "SYNTHETIC MODEM\r\n"},
        {"AT+CGMR", "LAB-ONLY\r\n"},
        {"AT+CPMS?", "+CPMS: \"ME\",2,100,\"ME\",2,100,\"ME\",2,100\r\n"},
        {"AT+QCCID", "+QCCID: 0000000000000000001\r\n"},
        {"AT+CCID", "+CCID: 0000000000000000001\r\n"},
        {"AT+ICCID", "+ICCID: 0000000000000000001\r\n"},
        {"AT+CIMI", "001010000000001\r\n"},
        {"AT+CNUM", "+CNUM: \"LAB\",\"+15550000001\",145\r\n"},
        {"AT+QNWINFO", "+QNWINFO: \"FDD LTE\",\"00101\",\"LAB BAND\",100\r\n"},
        {"AT+CEREG?", "+CEREG: 0,1\r\n"},
        {"AT+QCSQ", "+QCSQ: \"LTE\",59,-86,212,-5\r\n"},
        {"AT+CGDCONT?", "+CGDCONT: 1,\"IP\",\"synthetic.invalid\"\r\n"},
        {"AT+QCFG=\"ims\"", "+QCFG: \"ims\",0,0\r\n"},
        {"AT+CLCC", ""}
    };
    for (size_t i=0; i<sizeof table/sizeof table[0]; i++) {
        if (strcmp(command, table[i].command)) continue;
        int n = snprintf(out, cap, "\r\n%sOK\r\n", table[i].value);
        return n > 0 && (size_t)n < cap ? (size_t)n : 0;
    }
    return result(out, cap, "\r\nERROR\r\n");
}

size_t sms_fake_control(struct sms_fake *s, unsigned char control, char *out, size_t cap) {
    if (!s->awaiting_pdu) return 0;
    if (control == 0x1b) {
        s->awaiting_pdu = 0; s->cancelled++;
        return result(out, cap, "\r\nOK\r\n");
    }
    if (control == 0x1a) {
        s->awaiting_pdu = 0; s->submitted++;
        return result(out, cap, "\r\n+CMGS: 1\r\nOK\r\n");
    }
    return 0;
}
