#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>

static std::map<std::string, std::string> g_values;

static void apply_text(const std::string &text)
{
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
        g_values[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

static void load_snapshot()
{
    int fd = open("/dev/vsb", O_RDONLY);
    if (fd == -1) {
        perror("dashboard: open /dev/vsb");
        exit(EXIT_FAILURE);
    }
    std::string text;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        text.append(buf, (size_t)n);
    }
    close(fd);
    apply_text(text);
}

static void draw()
{
    printf("\033[2J\033[H");
    printf("QNX VEHICLE SIGNAL BUS\n");
    printf("======================\n");

    for (const auto &kv : g_values) {
        if (kv.first == "fault") {
            continue;
        }
        printf("  %-12s %s\n", kv.first.c_str(), kv.second.c_str());
    }

    std::string fault = "OK";
    auto f = g_values.find("fault");
    if (f != g_values.end()) {
        fault = f->second;
    }
    const char *color = (fault == "OK") ? "\033[32m" : "\033[31m";
    printf("\n  STATUS: %s%s\033[0m\n", color, fault.c_str());
    fflush(stdout);
}

int main()
{
    int in = open("/dev/vsb_events", O_RDONLY);
    if (in == -1) {
        perror("dashboard: open /dev/vsb_events");
        return EXIT_FAILURE;
    }

    load_snapshot();
    draw();

    char buf[1024];
    while (1) {
        ssize_t n = read(in, buf, sizeof buf - 1);
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        apply_text(std::string(buf, (size_t)n));
        draw();
    }

    close(in);
    return EXIT_SUCCESS;
}
