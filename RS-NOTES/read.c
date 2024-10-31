#include <stdio.h>
#include <fcntl.h> // open
#include <unistd.h> // read
#include <assert.h>

int main(int argc, char **argv) {
    int fd = open("data.txt", 0 /* flags */, O_RDONLY);
    assert(fd != -1);

    char buffer[23] = {0};
    ssize_t bytes = read(fd, &buffer, sizeof(buffer));
    printf("buffer: \"%s\"\nread: %ld bytes in total\n", buffer, bytes);

    return 0;
}

