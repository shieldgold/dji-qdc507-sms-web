/* Disposable Linux-container fixtures only. Never upload these executables. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef BUILD_ID
#define BUILD_ID "original"
#endif
static volatile sig_atomic_t stop, delegated;
static void stopped(int sig) { if (sig==SIGUSR1) delegated=1; stop=1; }
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc==5 && !strcmp(argv[1],"resume-clock")) {
        signal(SIGTERM,stopped); signal(SIGINT,stopped); signal(SIGUSR1,stopped);
        printf("RESUME_GUARD_READY pid=%ld supervisor=%s start=%s seconds=300 restore_clock=1\n",(long)getpid(),argv[2],argv[3]);
        time_t until=time(NULL)+300;
        while (!stop && time(NULL)<until) usleep(10000);
        /* Proof fixture: deliberately does NOT change any system clock. */
        if (getenv("SMS_SWAP_FIXTURE_CLOCK_FAIL")) { puts("RECOVERY_REQUIRED synthetic_clock_failure"); return 1; }
        printf("CLOCK_RESTORED epoch=%lld before_supervisor_cont=1\n",(long long)time(NULL));
        if (getenv("SMS_SWAP_FIXTURE_CLOCK_OVERLAY") && mount("/fixtures/foreign","/data/smsweb/smsweb",NULL,MS_BIND,NULL)) return 1;
        if (delegated) { puts("RESUME_DELEGATED verify_supervisor_before_cont=1"); return 0; }
        kill((pid_t)strtol(argv[2],NULL,10),SIGCONT);
        puts("RESUME_SENT verify_sms_health_separately=1"); return 0;
    }
    if (argc==4 && !strcmp(argv[1],"/bin/sh") && !strcmp(argv[2],"/data/smsweb/start.sh") && !strcmp(argv[3],"supervise")) {
        for (;;) {
            pid_t child=fork();
            if (child<0) return 1;
            if (!child) { execl("/data/smsweb/smsweb","/data/smsweb/smsweb",(char *)NULL); _exit(127); }
            while (waitpid(child,NULL,0)<0) {}
            usleep(10000);
        }
    }
    FILE *out=fopen("/run/sms-swap-fixture-starts","a");
    if (out) { fprintf(out,"%s pid=%ld parent=%ld\n",BUILD_ID,(long)getpid(),(long)getppid()); fclose(out); }
    for (;;) pause();
}
