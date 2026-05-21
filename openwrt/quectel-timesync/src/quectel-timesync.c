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
#include <sys/ioctl.h>

#define READ_WAIT_TIMEOUT 	(1000)
#define WRITE_WAIT_TIMEOUT	(READ_WAIT_TIMEOUT * 10)
#define RESPONSE_TIMEOUT	(20)   /* generous timeout for network reply */

int debug = 0;

int open_serial_port(const char* port_name)
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
	options.c_cflag &= ~CRTSCTS;
	tcsetattr(fd, TCSANOW, &options);
	tcflush(fd, TCIOFLUSH);

	/* Request exclusive access – fail if port is already open by another process */
	int excl = 1;
	if (ioctl(fd, TIOCEXCL, &excl) < 0) {
		perror("TIOCEXCL");
		close(fd);
		return -1;
	}

	return fd;
}

int write_command(int fd, const char* command)
{
	int len = write(fd, command, strlen(command));
	if (len < 0) {
		perror("write_command: Unable to write command");
		return -1;
	}
	tcdrain(fd);                     /* wait until all data is sent */
	usleep(WRITE_WAIT_TIMEOUT);      /* let modem process the command */
	return 0;
}

/*
 * Original, proven state-machine reader that extracts the line containing
 * "+QLTS:" (or any given prefix). It does NOT rely on newline characters,
 * which makes it immune to line-ending variations.
 */
int read_response(int fd, const char *response, char *buf, int buf_size)
{
	enum read_state {
		READ_STATE_IDLE = 0,
		READ_STATE_PREFIX,
		READ_STATE_SEP_SPACE,
		READ_STATE_CONTENT,
	};
	enum read_state state;
	char prefix_buf[32];
	char input_char;
	char *ptr, *prefix_ptr = NULL;
	time_t start_time, current_time;

	start_time = time(NULL);
	state = READ_STATE_IDLE;
	ptr = buf;

	while (1) {
		current_time = time(NULL);
		if (current_time - start_time > RESPONSE_TIMEOUT) {
			if (debug)
				fprintf(stderr, "read_response: Timeout\n");
			return -1;
		}

		int len = read(fd, &input_char, 1);
		if (len < 0) {
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
				/* Check the collected prefix against expected */
				if (prefix_ptr - prefix_buf != strlen(response)) {
					state = READ_STATE_IDLE;
					break;
				}
				if (strncmp(prefix_buf, response, strlen(response)) != 0) {
					state = READ_STATE_IDLE;
					break;
				}
				state = READ_STATE_SEP_SPACE;
			} else {
				if (prefix_ptr - prefix_buf < (int)sizeof(prefix_buf) - 1)
					*prefix_ptr++ = input_char;
				else
					state = READ_STATE_IDLE;  /* overflow, restart */
			}
			break;
		case READ_STATE_SEP_SPACE:
			if (input_char == ' ') {
				state = READ_STATE_CONTENT;
			} else {
				state = READ_STATE_IDLE;
			}
			break;
		case READ_STATE_CONTENT:
			if (input_char == '\n' || input_char == '\r') {
				*ptr = '\0';
				/* Trim any trailing carriage return left in the buffer */
				if (ptr > buf && *(ptr-1) == '\r')
					*(ptr-1) = '\0';
				return ptr - buf;
			} else {
				if (ptr - buf < buf_size - 1)
					*ptr++ = input_char;
				else
					break;  /* buffer full, discard extra */
			}
			break;
		}
	}
}

/*
 * Flexible parser – accepts any Quectel date/time format.
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
		if (debug)
			fprintf(stderr, "Failed to parse response: %s\n", response);
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

	/* Use the proven state-machine reader that searches for "+QLTS:" */
	if (read_response(serial_fd, "QLTS", buf, sizeof(buf)) < 0) {
		fprintf(stderr, "Unable to read response\n");
		return -1;
	}

	if (debug)
		fprintf(stdout, "Read from serial: %s\n", buf);

	/*
	 * The buffer now contains the content after the "+QLTS: " prefix.
	 * The original code left the quotes and date as-is. Our parser
	 * handles quotes and no quotes. So we can pass buf directly.
	 */
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

	while ((c = getopt (argc, argv, "d:p:v")) != -1) {
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
