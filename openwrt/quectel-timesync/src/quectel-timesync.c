/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>

#define READ_WAIT_TIMEOUT  (1000)
#define WRITE_WAIT_TIMEOUT (READ_WAIT_TIMEOUT * 10)
#define RESPONSE_TIMEOUT   (10)

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
    tcsetattr(fd, TCSANOW, &options);

    return fd;
}

int write_command(int fd, const char *command)
{
    int len = write(fd, command, strlen(command));
    if (len < 0) {
        perror("write_command: Unable to write command");
        return -1;
    }
    usleep(WRITE_WAIT_TIMEOUT);
    return 0;
}

/* Read until we see a line containing "+QLTS: " or timeout */
int read_response(int fd, char *buf, int buf_size)
{
    char ch;
    char *ptr = buf;
    time_t start = time(NULL);

    while (1) {
        if (time(NULL) - start > RESPONSE_TIMEOUT) {
            fprintf(stderr, "read_response: Timeout\n");
            return -1;
        }
        if (read(fd, &ch, 1) < 0) {
            usleep(READ_WAIT_TIMEOUT);
            continue;
        }

        if (ch == '\n') {
            *ptr = '\0';
            if (strstr(buf, "+QLTS:") != NULL)
                return ptr - buf;
            else
                ptr = buf;  // reset for next line
        } else if (ptr - buf < buf_size - 1) {
            *ptr++ = ch;
        }
    }
}

/*
 * Flexible parser for Quectel time responses.
 * Accepted formats (with or without double quotes):
 *   YYYY/MM/DD,hh:mm:ss±zz[,ds]
 *   YY/MM/DD,hh:mm:ss±zz[,ds]
 * Timezone offset is given in quarters of an hour (range -48..+48).
 * DST flag (0 or 1) is optional.
 */
int parse_response(const char *response, struct tm *utc_tm)
{
    int year, month, day, hour, min, sec, tz_quarters, dst;
    char sign;

    /* Try with quotes */
    if (sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d,%d\"",
               &year, &month, &day, &hour, &min, &sec,
               &sign, &tz_quarters, &dst) == 9) {
        /* all fields parsed */
    }
    /* Try without quotes */
    else if (sscanf(response, "%d/%d/%d,%d:%d:%d%c%d,%d",
                     &year, &month, &day, &hour, &min, &sec,
                     &sign, &tz_quarters, &dst) == 9) {
        /* all fields parsed */
    }
    /* Try without DST flag */
    else if (sscanf(response, "\"%d/%d/%d,%d:%d:%d%c%d\"",
                     &year, &month, &day, &hour, &min, &sec,
                     &sign, &tz_quarters) == 8) {
        dst = 0;
    }
    else if (sscanf(response, "%d/%d/%d,%d:%d:%d%c%d",
                     &year, &month, &day, &hour, &min, &sec,
                     &sign, &tz_quarters) == 8) {
        dst = 0;
    }
    else {
        if (debug)
            fprintf(stderr, "Failed to parse response: %s\n", response);
        return -1;
    }

    /* Some Quectel modules report year as 2 digits */
    if (year < 100)
        year += 2000;

    /* Convert timezone offset from quarters to seconds */
    int tz_seconds = tz_quarters * 15 * 60;
    if (sign == '-')
        tz_seconds = -tz_seconds;

    /* Fill in broken-down UTC time */
    memset(utc_tm, 0, sizeof(*utc_tm));
    utc_tm->tm_year = year - 1900;
    utc_tm->tm_mon  = month - 1;
    utc_tm->tm_mday = day;
    utc_tm->tm_hour = hour;
    utc_tm->tm_min  = min;
    utc_tm->tm_sec  = sec;

    /* Convert local time (as reported) to UTC */
    time_t local_time = timegm(utc_tm);
    if (local_time == (time_t)-1)
        return -1;
    local_time -= tz_seconds;

    if (gmtime_r(&local_time, utc_tm) == NULL)
        return -1;

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
    tv.tv_sec = timegm((struct tm *)utc_tm);  /* cast away const */
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) < 0) {
        perror("settimeofday");
        return -1;
    }

    /* Also sync the hardware clock */
    system("hwclock -w");
    return 0;
}

int print_usage(char *app)
{
    fprintf(stderr, "Usage: %s [-d <interval>] [-p <serial port>] [-v]\n", app);
    return 0;
}

int perform_timesync(int serial_fd)
{
    char buf[256];
    struct tm utc_tm;

    write_command(serial_fd, "ATE0\r\n");
    write_command(serial_fd, "AT+QLTS=1\r\n");

    if (read_response(serial_fd, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "Unable to read response\n");
        return -1;
    }

    if (debug)
        fprintf(stdout, "Read from serial: %s\n", buf);

    if (parse_response(buf, &utc_tm) != 0) {
        fprintf(stderr, "Invalid response: %s\n", buf);
        return -1;
    }

    if (set_system_time(&utc_tm) != 0) {
        fprintf(stderr, "Failed to set system time\n");
        return -1;
    }

    return 0;
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
        case 'd':
            daemon_interval = atoi(optarg);
            break;
        case 'v':
            debug = 1;
            break;
        case 'p':
            serial_path = optarg;
            break;
        default:
            print_usage(argv[0]);
            return -1;
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
