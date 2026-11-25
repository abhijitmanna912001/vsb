#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>

static const int OVERSPEED_LIMIT = 100;
static const int HARD_BRAKE_LEVEL = 3;
static const int BRAKE_SPEED_FLOOR = 50;

static void publish(int fd, const std::string &name, const std::string &value) {
  std::string line = name + "=" + value + "\n";
  if (write(fd, line.c_str(), line.size()) == -1) {
    perror("monitor: write");
  }
}

// Decide the fault state from the latest known values.
static std::string evaluate(const std::map<std::string, int> &v) {
  int speed = 0;
  int brake = 0;
  auto s = v.find("speed");
  auto b = v.find("brake");
  if (s != v.end())
    speed = s->second;
  if (b != v.end())
    brake = b->second;

  if (speed > OVERSPEED_LIMIT)
    return "OVERSPEED";
  if (brake >= HARD_BRAKE_LEVEL && speed > BRAKE_SPEED_FLOOR)
    return "HARD_BRAKE";
  return "OK";
}

int main() {
  int in = open("/dev/vsb_events", O_RDONLY);
  if (in == -1) {
    perror("monitor: open /dev/vsb_events");
    return EXIT_FAILURE;
  }
  int out = open("/dev/vsb", O_WRONLY);
  if (out == -1) {
    perror("monitor: open /dev/vsb");
    return EXIT_FAILURE;
  }

  std::map<std::string, int> values;
  std::string fault = "OK";
  publish(out, "fault", fault);
  printf("monitor: running, fault=%s\n", fault.c_str());
  fflush(stdout);

  char buf[1024];
  while (1) {
    // Blocks only if there is nothing new for us.
    ssize_t n = read(in, buf, sizeof buf - 1);
    if (n == -1 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }

    // One reply may hold several "name=value" lines.
    std::string text(buf, (size_t)n);
    std::string last_name, last_value;
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos) {
        end = text.size();
      }
      std::string line = text.substr(start, end - start);
      start = end + 1;

      size_t eq = line.find('=');
      if (eq == std::string::npos || eq == 0) {
        continue;
      }
      std::string name = line.substr(0, eq);
      std::string value = line.substr(eq + 1);
      if (name == "fault") {
        continue; // our own output; ignoring it avoids a feedback loop
      }
      values[name] = atoi(value.c_str());
      last_name = name;
      last_value = value;
    }

    std::string now = evaluate(values);
    if (now != fault) {
      fault = now;
      publish(out, "fault", fault);
      printf("monitor: fault -> %s (after %s=%s)\n", fault.c_str(),
             last_name.c_str(), last_value.c_str());
      fflush(stdout);
    }
  }

  close(in);
  close(out);
  return EXIT_SUCCESS;
}