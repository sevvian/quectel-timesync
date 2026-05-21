/* SPDX-License-Identifier: GPL-2.0-only */

#define _DEFAULT_SOURCE /* Required for timegm() */

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

#define READ_WAIT_TIMEOUT   (1000)      /* 1ms loop sleep */
#define WRITE_WAIT_TIMEOUT  (100000)    /* 100ms command delay */
#define RESPONSE_TIMEOUT    (10)        /* 10 second serial timeout */

int debug = 0;

/* 
 * Configures the serial port for RAW mode.
 * Crucial for avoiding timeouts caused by Linux terminal processing.
 */
int configure_serial_port(int fd) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;

    cfmakeraw(&tty);
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    /* Non-blocking: return immediately if no data */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;
    return 0;
}

int open_serial_port(const char* port_name) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd == -1) return -1;
    if (configure_serial_port(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 
 * State machine reader: 
 * Filters out command echoes and extracts data after '+PREFIX: '
 */
int read_response(int fd, const char *target, char *buf, int buf_size) {
    char input_char, prefix_buf[32], *prefix_ptr = prefix_buf, *ptr = buf;
    int state = 0; // 0:IDLE, 1:PREFIX, 2:SEP, 3:CONTENT
    time_t start = time(NULL);

    while (time(NULL) - start < RESPONSE_TIMEOUT) {
        if (read(fd, &input_char, 1) <= 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }

        switch (state) {
            case 0: // Looking for '+'
                if (input_char == '+') { state = 1; prefix_ptr = prefix_buf; }
                break;
            case 1: // Capturing prefix until ':'
                if (input_char == ':') {
                    *prefix_ptr = '\0';
                    state = (strcmp(prefix_buf, target) == 0) ? 2 : 0;
                } else if (prefix_ptr - prefix_buf < 31) {
                    *prefix_ptr++ = input_char;
                }
                break;
            case 2: // Consume space after colon
                if (input_char == ' ') state = 3;
                else if (input_char != '\r' && input_char != '\n') {
                    state = 3; *ptr++ = input_char; // No space, start content
                }
                break;
            case 3: // Capturing content until newline
                if (input_char == '\r' || input_char == '\n') {
                    if (ptr > buf) { *ptr = '\0'; return 0; }
                } else if (ptr - buf < buf_size - 1) {
                    *ptr++ = input_char;
                }
                break;
        }
    }
    return -1;
}

/*
 * Cascading Parser: 
 * Corrects Local Time to UTC using the 15-minute timezone quarters.
 */
int parse_and_set_system_time(const char *res) {
    int yr, mon, day, hr, min, sec, tz_q, dst = 0;
    char sign;
    struct tm tm = {0};
    struct timeval tv;

    /* Try multiple formats (Quotes/No Quotes, DST/No DST) */
    if (sscanf(res, " \"%d/%d/%d,%d:%d:%d%c%d,%d\"", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q, &dst) < 8 &&
        sscanf(res, " \"%d/%d/%d,%d:%d:%d%c%d\"", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q) < 8 &&
        sscanf(res, "%d/%d/%d,%d:%d:%d%c%d,%d", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q, &dst) < 8 &&
        sscanf(res, "%d/%d/%d,%d:%d:%d%c%d", &yr, &mon, &day, &hr, &min, &sec, &sign, &tz_q) < 8) {
        return -1;
    }

    if (yr < 100) yr += 2000;
    if (yr < 2024) return -2; // Network time not yet acquired

    tm.tm_year = yr - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hr;
    tm.tm_min  = min;
    tm.tm_sec  = sec;
    tm.tm_isdst = -1;

    time_t epoch = timegm(&tm);
    if (epoch == (time_t)-1) return -1;

    /* Offset math: UTC = Local - Offset */
    int offset_seconds = tz_q * 15 * 60;
    if (sign == '-') epoch += offset_seconds;
    else             epoch -= offset_seconds;

    tv.tv_sec = epoch;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) < 0) return -1;

    /* ImmortalWrt Fix: Update file timestamp so time persists better across reboots */
    system("touch /etc/config/system 2>/dev/null");

    if (debug) {
        printf("Sync Successful. Mode: No-RTC (RAM Only)\n");
        printf("UTC Time Set: %s", asctime(gmtime(&epoch)));
    }

    return 0;
}

int main(int argc, char *argv[]) {
    char *port_path = NULL;
    int daemon_interval = 0;
    int c, fd;
    char buf[256];

    while ((c = getopt(argc, argv, "d:p:v")) != -1) {
        switch (c) {
            case 'd': daemon_interval = atoi(optarg); break;
            case 'p': port_path = optarg; break;
            case 'v': debug = 1; break;
            default:  return -1;
        }
    }

    if (!port_path) {
        fprintf(stderr, "Usage: %s -p <port> [-d <interval>] [-v]\n", argv[0]);
        return -1;
    }

    while (1) {
        fd = open_serial_port(port_path);
        if (fd >= 0) {
            /* 1. Flush old echos */
            tcflush(fd, TCIOFLUSH);

            /* 2. Disable echo (ATE0) */
            write(fd, "ATE0\r\n", 6);
            usleep(WRITE_WAIT_TIMEOUT);
            tcflush(fd, TCIFLUSH);

            /* 3. Query Time */
            write(fd, "AT+QLTS=1\r\n", 11);

            if (read_response(fd, "QLTS", buf, sizeof(buf)) == 0) {
                int res = parse_and_set_system_time(buf);
                if (res == -2) {
                    if (debug) fprintf(stderr, "Modem has no network sync yet.\n");
                } else if (res < 0) {
                    if (debug) fprintf(stderr, "Failed to parse/set time: %s\n", buf);
                }
            } else {
                if (debug) fprintf(stderr, "No response from modem.\n");
            }
            close(fd);
        } else {
            if (debug) perror("Serial open failed");
        }

        if (daemon_interval < 10) break;
        sleep(daemon_interval);
    }

    return 0;
}
