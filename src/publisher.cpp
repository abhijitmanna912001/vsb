#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Usage: publisher <name> <from> <to> <delay_ms>
// Writes name=from, name=from+1, ... name=to (or counting down), one per delay.
int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s <name> <from> <to> <delay_ms>\n", argv[0]);
    return EXIT_FAILURE;
  }

  const char *name = argv[1];
  int from = atoi(argv[2]);
  int to = atoi(argv[3]);
  int delay = atoi(argv[4]);

  int fd = open("/dev/vsb", O_WRONLY);
  if (fd == -1) {
    perror("open /dev/vsb");
    return EXIT_FAILURE;
  }

  int step = (from <= to) ? 1 : -1;
  for (int v = from; v != to + step; v += step) {
    char line[64];
    int n = snprintf(line, sizeof line, "%s=%d\n", name, v);
    if (write(fd, line, n) == -1) {
      perror("write");
      break;
    }
    usleep((useconds_t)delay * 1000);
  }

  close(fd);
  return EXIT_SUCCESS;
}