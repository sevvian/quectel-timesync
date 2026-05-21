/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>

#define READ_WAIT_TIMEOUT  (1000)      /* microsec between read retries */
#define WRITE_WAIT_TIMEOUT (200000)    /* microsec to wait after sending a command */
#define RESPONSE_TIMEOUT   (20)        /* seconds to wait for a response */
#define MODEM_READY_TIMEOUT (10)       /* seconds to wait for modem to answer AT */

int debug = 0;

int open_serial_port(const char *port_name)
{
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("open_serial_port: Unable to open port");
        return -1;
    }

    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;
    /* Disable hardware flow control (common for Quectel AT ports) */
    options.c_cflag &= ~CRTSCTS;
    tcsetattr(fd, TCSANOW, &options);

    /* Flush any stale data */
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int write_command(int fd, const char *command)
{
    int len = write(fd, command, strlen(command));
    if (len < 0) {
        perror("write_command: Unable to write command");
        return -1;
    }
    tcdrain(fd);                      /* wait until all data is sent */
    usleep(WRITE_WAIT_TIMEOUT);       /* give modem processing time */
    return 0;
}

/*
 * Read lines until we see one that contains the given marker.
 * Returns line length (excluding terminator) or -1 on timeout.
 */
int read_line_until(int fd, char *buf, int buf_size, const char *marker, int timeout_sec)
{
    char ch;
    char *ptr = buf;
    time_t start = time(NULL);
    int line_started = 0;

    while (1) {
        if (time(NULL) - start > timeout_sec) {
            if (debug) fprintf(stderr, "read_line_until: Timeout waiting for '%s'\n", marker);
            return -1;
        }

        int n = read(fd, &ch, 1);
        if (n < 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }
        if (n == 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (!line_started) continue;
            *ptr = '\0';

            /* Trim trailing whitespace and carriage returns */
            char *end = ptr - 1;
            while (end >= buf && (*end == ' ' || *end == '\t' || *end == '\r'))
                *end-- = '\0';

            if (debug) fprintf(stderr, "Received line: '%s'\n", buf);

            if (strstr(buf, marker) != NULL)
                return ptr - buf;

            ptr = buf;
            line_started = 0;
            continue;
        }

        line_started = 1;
        if (ptr - buf < buf_size - 1)
            *ptr++ = ch;
    }
}

/*
 * Check if modem is alive by sending AT and waiting for OK.
 * Returns 0 on success, -1 if no response.
 */
int modem_ready(int fd)
{
    char buf[128];

    /* Disable echo first (may echo once) */
    write_command(fd, "ATE0\r\n");
    usleep(100000);

    /* Flush any residual echo lines */
    while (read_line_until(fd, buf, sizeof(buf), "OK", 1) == 0) {
        /* Discard lines until we see OK */
    }

    /* Send plain AT */
    write_command(fd, "AT\r\n");
    if (read_line_until(fd, buf, sizeof(buf), "OK", MODEM_READY_TIMEOUT) < 0) {
        if (debug) fprintf(stderr, "Modem not ready\n");
        return -1;
    }
    return 0;
}

/*
 * Flexible parser for Quectel time responses.
 * Accepted formats (with or without double quotes):
 *   YYYY/MM/DD,hh:mm:ss±zz[,ds]
 *   YY/MM/DD,hh:mm:ss±zz[,ds]
 */
int parse_response(const char *response, struct tm *utc_tm)
{
    int year, month, day, hour, min, sec, tz_quarters, dst;
    char sign;

    if (sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d,%d\"",
               &year, &month, &day, &hour, &min, &sec,
               &sign, &tz_quarters, &dst) == 9) {
    } else if (sscanf(response, "%d/%d/%d,%d:%d:%d%c%d,%d",
                      &year, &month, &day, &hour, &min, &sec,
                      &sign, &tz_quarters, &dst) == 9) {
    } else if (sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d\"",
                      &year, &month, &day, &hour, &min, &sec,
                      &sign, &tz_quarters) == 8) {
        dst = 0;
    } else if (sscanf(response, "%d/%d/%d,%d:%d:%d%c%d",
                      &year, &month, &day, &hour, &min, &sec,
                      &sign, &tz_quarters) == 8) {
        dst = 0;
    } else {
        if (debug) fprintf(stderr, "Failed to parse response: %s\n", response);
        return -1;
    }

    if (year < 100) year += 2000;

    int tz_seconds = tz_quarters * 15 * 60;
    if (sign == '-') tz_seconds = -tz_seconds;

    memset(utc_tm, 0, sizeof(*utc_tm));
    utc_tm->tm_year = year - 1900;
    utc_tm->tm_mon  = month - 1;
    utc_tm->tm_mday = day;
    utc_tm->tm_hour = hour;
    utc_tm->tm_min  = min;
    utc_tm->tm_sec  = sec;

    time_t local_time = timegm(utc_tm);
    if (local_time == (time_t)-1) return -1;
    local_time -= tz_seconds;

    if (gmtime_r(&local_time, utc_tm) == NULL) return -1;

    if (debug) {
        fprintf(stdout, "Parsed UTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
                utc_tm->tm_year + 1900, utc_tm->tm_mon + 1,
                utc_tm->tm_mday, utc_tm->tm_hour,
                utc_tm->tm_min, utc_tm->tm_sec);
    }
    return 0;
}

int set_system_time(const struct tm *utc_tm)
{
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

int perform_timesync(int serial_fd)
{
    char buf[256];
    struct tm utc_tm;

    /* Wait until modem is ready (may retry inside the daemon loop) */
    if (modem_ready(serial_fd) != 0) {
        fprintf(stderr, "Modem not ready on this port\n");
        return -1;
    }

    write_command(serial_fd, "AT+QLTS=1\r\n");
    if (read_line_until(serial_fd, buf, sizeof(buf), "+QLTS:", RESPONSE_TIMEOUT) < 0) {
        fprintf(stderr, "Unable to read +QLTS: response\n");
        return -1;
    }

    if (debug) fprintf(stdout, "Read from serial: %s\n", buf);

    /* Strip the leading "+QLTS:" if present */
    char *value = buf;
    if (strncasecmp(buf, "+QLTS:", 6) == 0) {
        value = buf + 6;
        while (*value == ' ' || *value == '\t') value++;
    }

    if (parse_response(value, &utc_tm) != 0) {
        fprintf(stderr, "Invalid response: %s\n", buf);
        return -1;
    }

    if (set_system_time(&utc_tm) != 0) {
        fprintf(stderr, "Failed to set system time\n");
        return -1;
    }

    return 0;
}

void print_usage(char *app)
{
    fprintf(stderr, "Usage: %s [-d <interval>] [-p <serial port>] [-v]\n", app);
}

int main(int argc, char *argv[])
{
    const char *serial_path = NULL;
    int serial_fd = -1;
    int daemon_interval = 0;
    int ret = 0;
    int c;

    while ((c = getopt(argc, argv, "d:p:v")) != -1) {
        switch (c) {
        case 'd': daemon_interval = atoi(optarg); break;
        case 'v': debug = 1; break;
        case 'p': serial_path = optarg; break;
        default: print_usage(argv[0]); return -1;
        }
    }

    if (daemon_interval && daemon_interval < 10) {
        fprintf(stderr, "Invalid daemon interval. Minimum: 10\n");
        return -1;
    }
    if (!serial_path) {
        print_usage(argv[0]);
        fprintf(stderr, "No serial port specified\n");
        return -1;
    }

    while (1) {
        serial_fd = open_serial_port(serial_path);
        if (serial_fd < 0) {
            fprintf(stderr, "Could not open serial port %s\n", serial_path);
            ret = -1;
        } else {
            ret = perform_timesync(serial_fd);
            close(serial_fd);
        }

        if (!daemon_interval)
            return ret;

        sleep(daemon_interval);
    }

    return 0;
}
