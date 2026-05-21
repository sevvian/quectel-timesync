/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdio.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define READ_WAIT_TIMEOUT 	(1000)
#define WRITE_WAIT_TIMEOUT	(READ_WAIT_TIMEOUT * 10)
#define RESPONSE_TIMEOUT	(10)

int debug = 0;

int open_serial_port(const char* port_name)
{
	int fd = open(port_name, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd == -1) {
		perror("open_serial_port: Unable to open port");
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

	usleep(WRITE_WAIT_TIMEOUT);

	return 0;
}

enum read_state {
	READ_STATE_IDLE = 0,
	READ_STATE_PREFIX,
	READ_STATE_SEP_SPACE,
	READ_STATE_CONTENT,
};

int read_response(int fd, const char *response, char *buf, int buf_size)
{
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
					if (prefix_ptr - prefix_buf != strlen(response)) {
						state = READ_STATE_IDLE;
						break;
					}

					if (strncmp(prefix_buf, response, strlen(response)) != 0) {
						state = READ_STATE_IDLE;
						break;
					}

					state = READ_STATE_SEP_SPACE;
				}
				*prefix_ptr++ = input_char;
				break;
			case READ_STATE_SEP_SPACE:
				if (input_char == ' ') {
					state = READ_STATE_CONTENT;
				} else {
					state = READ_STATE_IDLE;
				}
				break;
			case READ_STATE_CONTENT:
				if (input_char == '\n') {
					*ptr = '\0';
					return ptr - buf;
				} else {
					*ptr++ = input_char;
				}
				break;
		}
	}
}

int validate_response(const char *response, int response_len) {
	if (strlen("\"2023/10/07,23:07:16+08,1\"") != response_len) {
		return -1;
	}

	return 0;
}

enum datetime_field {
	DATE_TIME_FIELD_YEAR = 0,
	DATE_TIME_FIELD_MONTH,
	DATE_TIME_FIELD_DAY,
	DATE_TIME_FIELD_HOUR,
	DATE_TIME_FIELD_MINUTE,
	DATE_TIME_FIELD_SECOND,
	__DATE_TIME_FIELD_MAX,
};

int copy_and_parse_field(const char *src, int offset, int len, uint16_t *output) {
	char fieldbuf[16];
	long val;
	int i;
	for (i = 0; i < len; i++) {
		fieldbuf[i] = src[offset + i];
	}

	fieldbuf[i] = '\0';

	val = strtoul(fieldbuf, NULL, 10);

	*output = val;

	return -1;
}

int parse_response(const char *response, uint16_t *output) {	
	copy_and_parse_field(response, 1, 4, &output[DATE_TIME_FIELD_YEAR]);
	copy_and_parse_field(response, 6, 2, &output[DATE_TIME_FIELD_MONTH]);
	copy_and_parse_field(response, 9, 2, &output[DATE_TIME_FIELD_DAY]);
	copy_and_parse_field(response, 12, 2, &output[DATE_TIME_FIELD_HOUR]);
	copy_and_parse_field(response, 15, 2, &output[DATE_TIME_FIELD_MINUTE]);
	copy_and_parse_field(response, 18, 2, &output[DATE_TIME_FIELD_SECOND]);

	if (debug) {
		fprintf(stdout, "Parsed DateTime:\n");
		#define PRINT_FIELD(label, value) fprintf(stdout, "  %s: %d\n", label, value)
		PRINT_FIELD("Year", output[DATE_TIME_FIELD_YEAR]);
		PRINT_FIELD("Month", output[DATE_TIME_FIELD_MONTH]);
		PRINT_FIELD("Day", output[DATE_TIME_FIELD_DAY]);
		PRINT_FIELD("Hour", output[DATE_TIME_FIELD_HOUR]);
		PRINT_FIELD("Minute", output[DATE_TIME_FIELD_MINUTE]);
		PRINT_FIELD("Second", output[DATE_TIME_FIELD_SECOND]);
		#undef PRINT_FIELD
	}

	return 0;
}

int set_date_and_time(uint16_t *fields) {
	char buf[96];

	snprintf(buf, sizeof(buf), "date -u \"%04d-%02d-%02d %02d:%02d:%02d\"", 
		fields[DATE_TIME_FIELD_YEAR],
		fields[DATE_TIME_FIELD_MONTH],
		fields[DATE_TIME_FIELD_DAY],
		fields[DATE_TIME_FIELD_HOUR],
		fields[DATE_TIME_FIELD_MINUTE],
		fields[DATE_TIME_FIELD_SECOND]);
	
	if (debug)
		fprintf(stdout, "Execute: %s\n", buf);

	system(buf);

	return 0;
}

int print_usage(char *app) {
	fprintf(stderr, "Usage: %s [-d <interval>] [-p <serial port>] [-v]\n", app);
	return 0;
}

int perform_timesync(int serial_fd) {
	uint16_t fields[__DATE_TIME_FIELD_MAX];
	char buf[256];

	write_command(serial_fd, "ATE0\r\n");
	write_command(serial_fd, "AT+QLTS=1\r\n");

	if (read_response(serial_fd, "QLTS", buf, sizeof(buf)) < 0) {
		fprintf(stderr, "Unable to read response\n");
		return -1;
	}

	if (validate_response(buf, strlen(buf)) != 0) {
		fprintf(stderr, "Invalid response: %s\n", buf);
		return -1;
	}

	if (debug) {
		fprintf(stdout, "Read from serial: %s\n", buf);
	}

	if (parse_response(buf, fields)) {
		fprintf(stderr, "Unable to parse response\n");
		return -1;
	}

	set_date_and_time(fields);

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
