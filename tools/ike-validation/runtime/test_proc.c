#define _GNU_SOURCE
#include "proc.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    struct proc_id id;
    assert(!prctl(PR_SET_NAME, "a ) odd ( name", 0, 0, 0));
    assert(!proc_identity(getpid(), &id));
    assert(id.pid == getpid() && id.start > 0 && id.state != 'Z');
    assert(proc_matches(getpid(), id.start, "/definitely/not/the/executable") == 0);
    assert(proc_matches(getpid(), id.start + 1, id.exe) == 0);
    assert(proc_matches(getpid(), id.start, id.exe) == 1);
    assert(memory_available() > 0);
    assert(memory_safe(6144, 9000));
    assert(!memory_safe(6143, 9000));
    assert(!memory_safe(-1, 9000));
    assert(!memory_safe(6200, 10000));
    char path[] = "/tmp/qdc-proc-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    char data[12000]; memset(data, 'x', sizeof(data));
    assert(write(fd, data, sizeof(data)) == sizeof(data)); close(fd);
    char out[13000];
    assert(read_complete(path, out, sizeof(out)) == 12000);
    assert(!memcmp(out, data, 12000));
    assert(read_complete(path, out, 1000) < 0);
    unlink(path);
    assert(!mkfifo(path, 0600));
    pid_t writer = fork(); assert(writer >= 0);
    if (!writer) {
        int pipefd = open(path, O_WRONLY); assert(pipefd >= 0);
        for (int offset = 0; offset < 12000; offset += 1000) {
            assert(write(pipefd, data+offset, 1000) == 1000);
            usleep(5000);
        }
        close(pipefd); _exit(0);
    }
    assert(read_complete(path, out, sizeof(out)) == 12000);
    assert(!memcmp(out, data, 12000));
    int status; assert(waitpid(writer, &status, 0) == writer && WIFEXITED(status) && !WEXITSTATUS(status));
    unlink(path);
    puts("PROC_TESTS_OK");
    return 0;
}
