#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>

#include <map>
#include <string>

static const int OVERSPEED_LIMIT = 100;
static const int HARD_BRAKE_LEVEL = 3;
static const int BRAKE_SPEED_FLOOR = 50;
static const double HEARTBEAT_TIMEOUT_S = 2.0;
static const uint64_t POLL_NS = 500000000ULL;   // wake every 0.5 s to check the clock
static const double STATS_PERIOD_S = 5.0;

static double now_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Faults are also written to the QNX system logger. Read them with:
//   slog2info -b vsb
static slog2_buffer_t g_slog = NULL;

static void init_slog()
{
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.buffer_set_name = "vsb";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "monitor";
    cfg.buffer_config[0].num_pages = 1;
    if (slog2_register(&cfg, &g_slog, 0) == -1) {
        perror("monitor: slog2_register");
        g_slog = NULL;
    }
}

static void log_fault(const std::string &fault)
{
    if (g_slog == NULL) {
        return;
    }
    slog2f(g_slog, 0, fault == "OK" ? SLOG2_INFO : SLOG2_ERROR,
           "fault -> %s", fault.c_str());
}

static int get_prio()
{
    int policy;
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
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

int main(int argc, char **argv)
{
    bool stats = false;
    int prio = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stats = true;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prio = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: monitor [-s] [-p priority]\n");
            return EXIT_FAILURE;
        }
    }
    if (prio > 0 && pthread_setschedprio(pthread_self(), prio) != 0) {
        perror("monitor: setprio");
        return EXIT_FAILURE;
    }

    init_slog();

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

    // Wake-up lateness statistics (-s): how much later than 0.5 s did
    // the timed-out read actually return?
    double stat_start = now_s();
    double max_late_ms = 0.0;
    int    ticks = 0;

    publish(out, "fault", fault);
    printf("monitor: running, fault=%s, priority=%d\n", fault.c_str(), get_prio());
    fflush(stdout);

    char buf[1024];
    while (1) {
        double t0 = now_s();

        // Arm a timeout on the NEXT blocking call (the read below).
        uint64_t timeout = POLL_NS;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL);

        ssize_t n = read(in, buf, sizeof buf - 1);
        double t1 = now_s();

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
            // Nothing arrived within the poll period.
            double late_ms = (t1 - t0 - (double)POLL_NS / 1e9) * 1000.0;
            if (late_ms > max_late_ms) {
                max_late_ms = late_ms;
            }
            ticks++;
        } else {
            break;   // end of stream or a real error
        }

        bool stale = hb_seen && (now_s() - last_hb > HEARTBEAT_TIMEOUT_S);
        std::string now = evaluate(values, stale);
        if (now != fault) {
            fault = now;
            log_fault(fault);
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

        if (stats && now_s() - stat_start >= STATS_PERIOD_S) {
            printf("monitor: priority=%d, %d ticks, max wake-up lateness %.1f ms\n",
                   get_prio(), ticks, max_late_ms);
            fflush(stdout);
            stat_start = now_s();
            max_late_ms = 0.0;
            ticks = 0;
        }
    }

    close(in);
    close(out);
    return EXIT_SUCCESS;
}
