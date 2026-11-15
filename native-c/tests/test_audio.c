#include "qdc_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char dir[] = "/tmp/qdc-audio-XXXXXX";
    assert(mkdtemp(dir));
    char proc[4096], marker[4096], pid[4096], stat[4096], cmd[4096];
    qdc_path(proc, sizeof(proc), dir, "proc");
    assert(!mkdir(proc, 0700));
    qdc_path(pid, sizeof(pid), proc, "12");
    assert(!mkdir(pid, 0700));
    qdc_path(marker, sizeof(marker), dir, "ready");
    qdc_path(stat, sizeof(stat), pid, "stat");
    qdc_path(cmd, sizeof(cmd), pid, "cmdline");
    qdc_error e = {0};
    assert(!qdc_atomic_write(marker, "12 987\n", 7, &e));
    const char *good = "12 (test (with) spaces) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 987 0\n";
    assert(!qdc_atomic_write(stat, good, strlen(good), &e));
    assert(!qdc_atomic_write(cmd, "/usr/bin/alsaucm_test\0x", 23, &e));
    assert(qdc_calibration_alive(marker, proc));
    assert(!qdc_atomic_write(marker, "12 988\n", 7, &e));
    assert(!qdc_calibration_alive(marker, proc));
    unlink(marker);
    unlink(stat);
    unlink(cmd);
    rmdir(pid);
    rmdir(proc);
    rmdir(dir);
    puts("audio identity: PASS");
    return 0;
}
