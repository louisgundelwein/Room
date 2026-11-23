/* Reproduce the review's publication boundary with a cooperating writer. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

int rename(const char *source, const char *target)
{
    int (*publish)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
    if (strstr(source, ".xschem-")) {
        char *path;
        if (asprintf(&path, "%s.lock", target) < 0) abort();
        int fd = open(path, O_RDWR);
        free(path);
        if (fd < 0) abort();
        if (flock(fd, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
            fprintf(stderr, "FAIL publication was not locked\n");
            abort();
        }
        close(fd);
        fprintf(stderr, "PASS competing writer excluded immediately before rename\n");
    }
    return publish(source, target);
}
