#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Simulates a sensor that reports it is alive once per second.
int main()
{
    int fd = open("/dev/vsb", O_WRONLY);
    if (fd == -1) {
        perror("heartbeat: open /dev/vsb");
        return EXIT_FAILURE;
    }
    for (unsigned long n = 0;; n++) {
        char line[64];
        int len = snprintf(line, sizeof line, "heartbeat=%lu\n", n);
        if (write(fd, line, len) == -1) {
            perror("heartbeat: write");
            break;
        }
        sleep(1);
    }
    close(fd);
    return EXIT_SUCCESS;
}
