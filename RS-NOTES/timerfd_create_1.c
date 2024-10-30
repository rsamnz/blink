#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>

int main(int argc, char **argv) {
    int fd = timerfd_create(CLOCK_REALTIME, 0);
    assert(fd == -1);
}

