/* SPDX-License-Identifier: GPL-2.0-only */

#define _DEFAULT_SOURCE /* For timegm() */

#include <stdio.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <getopt.h>
#include <errno.h>

#define READ_WAIT_TIMEOUT   (1000)      /* 1ms loop wait */
#define WRITE_WAIT_TIMEOUT  (100000)    /* 100ms between commands */
#define RESPONSE_TIMEOUT    (10)        /* 10 second total timeout */

int debug = 0;

/* 
 * Configures serial port to Raw Mode.
 * This prevents the OS from waiting for newlines and allows
 * byte-by-byte processing without interference.
 */
int configure_serial_port(int fd) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&tty);
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    /* Non-blocking read with 0.1s inter-character timeout */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

int open_serial_port(const char* port_name) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd == -1) {
        perror("open_serial_port: Unable to open port");
        return -1;
    }
    
    if (configure_serial_port(fd) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

int write_command(int fd, const char* command) {
    int len = write(fd, command, strlen(command));
    if (len < 0) {
        perror("write_command: Unable to write");
        return -1;
    }
    tcdrain(fd);
    return 0;
}

enum read_state {
    STATE_IDLE = 0,
    STATE_PREFIX,
    STATE_SEP_SPACE,
    STATE_CONTENT,
};

/*
 * State-machine reader. Correctly handles:
 * 1. Modem echoes (AT+QLTS=1)
 * 2. Leading/Trailing spaces
 * 3. Carriage returns
 */
int read_response(int fd, const char *target_prefix, char *buf, int buf_size) {
    enum read_state state = STATE_IDLE;
    char prefix_buf[32];
    char input_char;
    char *ptr = buf;
    char *prefix_ptr = NULL;
    time_t start_time = time(NULL);

    while (1) {
        if (time(NULL) - start_time > RESPONSE_TIMEOUT) {
            return -1;
        }

        int len = read(fd, &input_char, 1);
        if (len <= 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }

        switch (state) {
            case STATE_IDLE:
                if (input_char == '+') {
                    state = STATE_PREFIX;
                    prefix_ptr = prefix_buf;
                }
                break;

            case STATE_PREFIX:
                if (input_char == ':') {
                    *prefix_ptr = '\0';
                    if (strcmp(prefix_buf, target_prefix) == 0) {
                        state = STATE_SEP_SPACE;
                    } else {
                        state = STATE_IDLE; /* Not the prefix we wanted */
                    }
                } else {
                    if (prefix_ptr - prefix_buf < (int)sizeof(prefix_buf) - 1)
                        *prefix_ptr++ = input_char;
                }
                break;

            case STATE_SEP_SPACE:
                if (input_char == ' ') {
                    state = STATE_CONTENT;
                } else if (input_char != '\r' && input_char != '\n') {
                    state = STATE_CONTENT;
                    goto capture; /* Jump to content if modem skipped space */
                }
                break;

            case STATE_CONTENT:
            capture:
                if (input_char == '\n' || input_char == '\r') {
                    if (ptr > buf) {
                        *ptr = '\0';
                        return ptr - buf;
                    }
                } else {
                    if (ptr - buf < buf_size - 1)
                        *ptr++ = input_char;
                }
                break;
        }
    }
}

/*
 * Universal Parser: Handles Timezone Quarters and DST flags.
 * Calculates true UTC by applying the offset to Local Time.
 */
int parse_quectel_time(const char *response, struct tm *utc_tm) {
    int yr, mon, day, hr, min, sec, tz_q, dst;
    char sign;
    int matched = 0;

    /* Pattern 1: "2026/05/21,03:17:14-28,1" (Quotes, TZ, DST) */
    matched = sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d,%d\"", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q, &dst);
    
    /* Pattern 2: 2026/05/21,03:17:14-28,1 (No quotes) */
    if (matched < 8)
        matched = sscanf(response, "%d/%d/%d,%d:%d:%d%c%d,%d", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q, &dst);

    /* Pattern 3: "2026/05/21,03:17:14-28" (Quotes, TZ, no DST) */
    if (matched < 8)
        matched = sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d\"", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q);

    /* Pattern 4: 2026/05/21,03:17:14-28 (No quotes, TZ, no DST) */
    if (matched < 8)
        matched = sscanf(response, "%d/%d/%d,%d:%d:%d%c%d", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q);

    if (matched < 8) return -1;

    if (yr < 100) yr += 2000;
    if (yr < 2024) {
        fprintf(stderr, "Error: Modem reports date before 2024 (No network sync yet).\n");
        return -1;
    }

    struct tm local_tm = {0};
    local_tm.tm_year = yr - 1900;
    local_tm.tm_mon  = mon - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hr;
    local_tm.tm_min  = min;
    local_tm.tm_sec  = sec;
    local_tm.tm_isdst = -1;

    /* Convert to linear epoch */
    time_t epoch = timegm(&local_tm);
    if (epoch == (time_t)-1) return -1;

    /* Apply TZ offset (1 quarter = 15 minutes) */
    int offset_sec = tz_q * 15 * 60;
    if (sign == '-') epoch += offset_sec; /* Subtracting a negative = adding */
    else             epoch -= offset_sec;

    if (gmtime_r(&epoch, utc_tm) == NULL) return -1;

    return 0;
}

int set_system_time(const struct tm *utc_tm) {
    struct timeval tv;
    tv.tv_sec = timegm((struct tm *)utc_tm);
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) < 0) {
        perror("settimeofday");
        return -1;
    }
    
    /* Sync hardware RTC (Silent if fails) */
    if (system("hwclock -w 2>/dev/null") != 0 && debug) {
        fprintf(stderr, "Warning: hwclock -w failed.\n");
    }
    return 0;
}

int perform_sync(int fd) {
    char buf[256];
    struct tm utc_tm;

    /* 1. Flush echos/garbage */
    tcflush(fd, TCIOFLUSH);

    /* 2. Disable Echo */
    write_command(fd, "ATE0\r\n");
    usleep(WRITE_WAIT_TIMEOUT);
    tcflush(fd, TCIFLUSH);

    /* 3. Query Time */
    write_command(fd, "AT+QLTS=1\r\n");

    if (read_response(fd, "QLTS", buf, sizeof(buf)) < 0) {
        fprintf(stderr, "Error: No +QLTS response from modem (check network reg).\n");
        return -1;
    }

    if (debug) printf("Raw Data: %s\n", buf);

    if (parse_quectel_time(buf, &utc_tm) != 0) {
        fprintf(stderr, "Error: Failed to parse: %s\n", buf);
        return -1;
    }

    if (set_system_time(&utc_tm) == 0) {
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", &utc_tm);
        printf("Success: System time set to %s\n", time_str);
        return 0;
    }

    return -1;
}

int main(int argc, char *argv[]) {
    const char *port = NULL;
    int interval = 0;
    int c;

    while ((c = getopt(argc, argv, "d:p:v")) != -1) {
        switch (c) {
            case 'd': interval = atoi(optarg); break;
            case 'p': port = optarg; break;
            case 'v': debug = 1; break;
            default:  break;
        }
    }

    if (!port) {
        fprintf(stderr, "Usage: %s -p <port> [-d <interval>] [-v]\n", argv[0]);
        return -1;
    }

    while (1) {
        int fd = open_serial_port(port);
        if (fd >= 0) {
            perform_sync(fd);
            close(fd);
        } else {
            fprintf(stderr, "Could not open %s\n", port);
        }

        if (interval < 10) break;
        sleep(interval);
    }

    return 0;
}
