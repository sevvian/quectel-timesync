/* SPDX-License-Identifier: GPL-2.0-only */

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

#define READ_WAIT_TIMEOUT   (1000)      // 1ms usleep
#define WRITE_WAIT_TIMEOUT  (100000)    // 100ms
#define RESPONSE_TIMEOUT    (10)        // 10 seconds

int debug = 0;

/* 
 * Configures the serial port for RAW mode. 
 * This is crucial to prevent the OS from waiting for newlines 
 * or echoing characters back.
 */
int configure_serial_port(int fd) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&tty);            // Set raw mode
    cfsetospeed(&tty, B115200); // Standard Quectel baud rate
    cfsetispeed(&tty, B115200);

    tty.c_cflag |= (CLOCAL | CREAD); // Enable receiver
    tty.c_cflag &= ~PARENB;          // No parity
    tty.c_cflag &= ~CSTOPB;          // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;              // 8 data bits

    // Set non-blocking read behavior
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1; // 0.1 second read timeout

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
        perror("write_command: Unable to write command");
        return -1;
    }
    tcdrain(fd); // Wait until all data is transmitted
    return 0;
}

enum read_state {
    READ_STATE_IDLE = 0,
    READ_STATE_PREFIX,
    READ_STATE_SEP_SPACE,
    READ_STATE_CONTENT,
};

int read_response(int fd, const char *response, char *buf, int buf_size) {
    enum read_state state = READ_STATE_IDLE;
    char prefix_buf[32];
    char input_char;
    char *ptr = buf;
    char *prefix_ptr = NULL;
    time_t start_time = time(NULL);

    while (1) {
        if (time(NULL) - start_time > RESPONSE_TIMEOUT) {
            fprintf(stderr, "read_response: Timeout reached\n");
            return -1;
        }

        int len = read(fd, &input_char, 1);
        if (len <= 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }

        switch (state) {
            case READ_STATE_IDLE:
                if (input_char == '+') {
                    state = READ_STATE_PREFIX;
                    prefix_ptr = prefix_buf;
                }
                break;

            case READ_STATE_PREFIX:
                if (input_char == ':') {
                    *prefix_ptr = '\0'; // Null terminate captured prefix
                    if (strcmp(prefix_buf, response) == 0) {
                        state = READ_STATE_SEP_SPACE;
                    } else {
                        state = READ_STATE_IDLE; // False alarm, reset
                    }
                } else {
                    if (prefix_ptr - prefix_buf < (int)sizeof(prefix_buf) - 1)
                        *prefix_ptr++ = input_char;
                    else
                        state = READ_STATE_IDLE;
                }
                break;

            case READ_STATE_SEP_SPACE:
                if (input_char == ' ') {
                    state = READ_STATE_CONTENT;
                } else if (input_char != '\r' && input_char != '\n') {
                    // If no space but actual data, jump to content
                    state = READ_STATE_CONTENT;
                    goto capture;
                }
                break;

            case READ_STATE_CONTENT:
            capture:
                if (input_char == '\n' || input_char == '\r') {
                    if (ptr > buf) { // Only finish if we actually got data
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

int parse_response(const char *response, struct tm *utc_tm) {
    int year, month, day, hour, min, sec, tz_quarters, dst;
    char sign;

    // Quectel RM521F-GL usually returns: "2026/05/21,03:17:14-28,1"
    if (sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d,%d\"",
               &year, &month, &day, &hour, &min, &sec,
               &sign, &tz_quarters, &dst) < 8) {
        if (debug) fprintf(stderr, "Failed to parse: %s\n", response);
        return -1;
    }

    if (year < 100) year += 2000;

    // Timezone is in quarters of an hour (1 quarter = 15 mins)
    int tz_seconds = tz_quarters * 15 * 60;
    if (sign == '-') tz_seconds = -tz_seconds;

    memset(utc_tm, 0, sizeof(*utc_tm));
    utc_tm->tm_year = year - 1900;
    utc_tm->tm_mon  = month - 1;
    utc_tm->tm_mday = day;
    utc_tm->tm_hour = hour;
    utc_tm->tm_min  = min;
    utc_tm->tm_sec  = sec;
    utc_tm->tm_isdst = -1;

    // Convert local time to UTC using timegm (ignores TZ env)
    time_t local_time = timegm(utc_tm);
    if (local_time == (time_t)-1) return -1;

    // Apply the offset to get actual UTC
    local_time -= tz_seconds;

    if (gmtime_r(&local_time, utc_tm) == NULL) return -1;

    if (debug) {
        fprintf(stdout, "Parsed UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                utc_tm->tm_year + 1900, utc_tm->tm_mon + 1,
                utc_tm->tm_mday, utc_tm->tm_hour,
                utc_tm->tm_min, utc_tm->tm_sec);
    }
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
    system("hwclock -w");
    return 0;
}

int perform_timesync(int serial_fd) {
    char buf[256];
    struct tm utc_tm;

    // 1. Clear any junk or echos currently in the buffer
    tcflush(serial_fd, TCIOFLUSH);

    // 2. Disable Echo (ATE0)
    write_command(serial_fd, "ATE0\r\n");
    usleep(WRITE_WAIT_TIMEOUT);
    tcflush(serial_fd, TCIFLUSH); // Flush the "OK" from ATE0

    // 3. Request Time
    write_command(serial_fd, "AT+QLTS=1\r\n");

    if (read_response(serial_fd, "QLTS", buf, sizeof(buf)) < 0) {
        return -1;
    }

    if (debug) fprintf(stdout, "Modem Response: %s\n", buf);

    if (parse_response(buf, &utc_tm) != 0) {
        fprintf(stderr, "Invalid response format: %s\n", buf);
        return -1;
    }

    if (set_system_time(&utc_tm) != 0) {
        return -1;
    }

    if (debug) fprintf(stdout, "System time updated successfully.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    const char *serial_path = NULL;
    int serial_fd = -1;
    int daemon_interval = 0;
    int c;

    while ((c = getopt(argc, argv, "d:p:v")) != -1) {
        switch (c) {
            case 'd': daemon_interval = atoi(optarg); break;
            case 'v': debug = 1; break;
            case 'p': serial_path = optarg; break;
            default: return -1;
        }
    }

    if (!serial_path) {
        fprintf(stderr, "Usage: %s -p <port> [-d <interval>] [-v]\n", argv[0]);
        return -1;
    }

    while (1) {
        serial_fd = open_serial_port(serial_path);
        if (serial_fd >= 0) {
            perform_timesync(serial_fd);
            close(serial_fd);
        }

        if (!daemon_interval) break;
        sleep(daemon_interval);
    }

    return 0;
}
