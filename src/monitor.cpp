#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include <map>
#include <string>

static const int OVERSPEED_LIMIT = 100;
static const int HARD_BRAKE_LEVEL = 3;
static const int BRAKE_SPEED_FLOOR = 50;
static const double HEARTBEAT_TIMEOUT_S = 2.0;
static const uint64_t POLL_NS = 500000000ULL;   // wake every 0.5 s to check the clock

static double now_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void publish(int fd, const std::string &name, const std::string &value)
{
    std::string line = name + "=" + value + "\n";
    if (write(fd, line.c_str(), line.size()) == -1) {
        perror("monitor: write");
    }
}

// Decide the fault state from the latest known values.
static std::string evaluate(const std::map<std::string, int> &v, bool heartbeat_stale)
{
    if (heartbeat_stale) return "SENSOR_TIMEOUT";

    int speed = 0;
    int brake = 0;
    auto s = v.find("speed");
    auto b = v.find("brake");
    if (s != v.end()) speed = s->second;
    if (b != v.end()) brake = b->second;

    if (speed > OVERSPEED_LIMIT) return "OVERSPEED";
    if (brake >= HARD_BRAKE_LEVEL && speed > BRAKE_SPEED_FLOOR) return "HARD_BRAKE";
    return "OK";
}

int main()
{
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
    bool   hb_seen = false;
    double last_hb = 0.0;

    publish(out, "fault", fault);
    printf("monitor: running, fault=%s\n", fault.c_str());
    fflush(stdout);

    char buf[1024];
    while (1) {
        // Arm a timeout on the NEXT blocking call (the read below).
        uint64_t timeout = POLL_NS;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL);

        ssize_t n = read(in, buf, sizeof buf - 1);

        std::string last_name, last_value;
        if (n > 0) {
            // One reply may hold several "name=value" lines.
            std::string text(buf, (size_t)n);
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
                std::string name  = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                if (name == "fault") {
                    continue;   // our own output; ignoring it avoids a feedback loop
                }
                if (name == "heartbeat") {
                    hb_seen = true;
                    last_hb = now_s();
                }
                values[name] = atoi(value.c_str());
                last_name  = name;
                last_value = value;
            }
        } else if (n == -1 && (errno == ETIMEDOUT || errno == EINTR)) {
            // Nothing arrived within the poll period. Fall through and
            // re-check the clock.
        } else {
            break;   // end of stream or a real error
        }

        bool stale = hb_seen && (now_s() - last_hb > HEARTBEAT_TIMEOUT_S);
        std::string now = evaluate(values, stale);
        if (now != fault) {
            fault = now;
            publish(out, "fault", fault);
            if (last_name.empty()) {
                printf("monitor: fault -> %s (no update, errno=%d)\n",
                       fault.c_str(), errno);
            } else {
                printf("monitor: fault -> %s (after %s=%s)\n",
                       fault.c_str(), last_name.c_str(), last_value.c_str());
            }
            fflush(stdout);
        }
    }

    close(in);
    close(out);
    return EXIT_SUCCESS;
}
