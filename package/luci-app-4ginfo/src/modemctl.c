#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <uci.h>

#define MODEM_LOCK "/tmp/edgenode/modem.lock"
#define MODEM_TIMEOUT_MS 5000
#define AT_PORT_DEFAULT "/dev/ttyUSB2"

struct modem_profile {
	char port[128];
	char apn[101];
	char pdp_type[16];
	char auth_type[16];
	char username[129];
	char password[129];
	char pin_code[9];
	bool automatic_apn;
	bool redial_after_apply;
};

static int64_t monotonic_milliseconds(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;

	return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool write_all(int fd, const char *data, size_t size)
{
	while (size != 0U) {
		ssize_t written = write(fd, data, size);

		if (written > 0) {
			data += (size_t)written;
			size -= (size_t)written;
			continue;
		}

		if (written < 0 && errno == EINTR)
			continue;

		return false;
	}

	return true;
}

static bool configure_port(int fd, struct termios *original)
{
	struct termios value;

	if (tcgetattr(fd, original) != 0)
		return false;

	value = *original;
	value.c_iflag = IGNPAR;
	value.c_oflag = 0;
	value.c_lflag = 0;
	value.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
	value.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
	value.c_cflag |= CS8 | CREAD | CLOCAL;
	value.c_cc[VMIN] = 0;
	value.c_cc[VTIME] = 0;

	if (cfsetispeed(&value, B115200) != 0 || cfsetospeed(&value, B115200) != 0 ||
		tcsetattr(fd, TCSANOW, &value) != 0)
		return false;

	tcflush(fd, TCIOFLUSH);
	return true;
}

static int lock_modem(void)
{
	int lock;

	if (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST)
		return -1;

	lock = open(MODEM_LOCK, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX) != 0) {
		if (lock >= 0)
			close(lock);
		return -1;
	}

	return lock;
}

static bool run_at_command(int fd, const char *command, bool require_ok)
{
	char response[512] = { 0 };
	size_t used = 0U;
	int64_t deadline;

	/* A transaction is exactly one command followed by its final response. */
	(void)tcflush(fd, TCIFLUSH);
	if (!write_all(fd, command, strlen(command)))
		return false;

	deadline = monotonic_milliseconds() + MODEM_TIMEOUT_MS;
	while (used + 1U < sizeof(response)) {
		struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
		int64_t remaining = deadline - monotonic_milliseconds();
		int ready;
		ssize_t count;

		if (remaining <= 0)
			break;

		ready = poll(&poll_fd, 1, (int)remaining);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			break;

		count = read(fd, response + used, sizeof(response) - used - 1U);
		if (count > 0) {
			used += (size_t)count;
			response[used] = '\0';
			if (strstr(response, "ERROR") != NULL ||
				strstr(response, "+CME ERROR") != NULL)
				return false;
			if (strstr(response, "\r\nOK\r\n") != NULL)
				return true;
			continue;
		}

		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		break;
	}

	return !require_ok;
}

static struct uci_section *find_section(struct uci_package *package, const char *name)
{
	struct uci_element *element;

	uci_foreach_element(&package->sections, element) {
		struct uci_section *section = uci_to_section(element);

		if (strcmp(section->e.name, name) == 0)
			return section;
	}

	return NULL;
}

static const char *option(struct uci_context *context, struct uci_section *section,
				  const char *name, const char *fallback)
{
	const char *value = section != NULL ? uci_lookup_option_string(context, section, name) : NULL;

	return value != NULL ? value : fallback;
}

static bool copy_option(char *destination, size_t capacity, const char *value)
{
	size_t length = strlen(value);

	if (length >= capacity)
		return false;

	memcpy(destination, value, length + 1U);
	return true;
}

static bool load_profile(struct modem_profile *profile)
{
	struct uci_context *context = NULL;
	struct uci_package *edgenode = NULL;
	struct uci_package *four_g = NULL;
	struct uci_section *modem;
	const char *value;
	bool success = false;

	memset(profile, 0, sizeof(*profile));
	if (!copy_option(profile->port, sizeof(profile->port), AT_PORT_DEFAULT))
		return false;
	if (!copy_option(profile->pdp_type, sizeof(profile->pdp_type), "ipv4") ||
		!copy_option(profile->auth_type, sizeof(profile->auth_type), "none"))
		return false;
	profile->automatic_apn = true;

	context = uci_alloc_context();
	if (context == NULL || uci_load(context, "edgenode", &edgenode) != UCI_OK ||
		uci_load(context, "4ginfo", &four_g) != UCI_OK)
		goto out;

	modem = find_section(edgenode, "modem");
	value = option(context, modem, "at_port", AT_PORT_DEFAULT);
	if (!copy_option(profile->port, sizeof(profile->port), value))
		goto out;

	modem = find_section(four_g, "modem");
	value = option(context, modem, "automatic_apn", "1");
	profile->automatic_apn = strcmp(value, "0") != 0;
	if (!copy_option(profile->apn, sizeof(profile->apn), option(context, modem, "apn", "")) ||
		!copy_option(profile->pdp_type, sizeof(profile->pdp_type),
				 option(context, modem, "pdp_type", "ipv4")) ||
		!copy_option(profile->auth_type, sizeof(profile->auth_type),
				 option(context, modem, "auth_type", "none")) ||
		!copy_option(profile->username, sizeof(profile->username),
				 option(context, modem, "username", "")) ||
		!copy_option(profile->password, sizeof(profile->password),
				 option(context, modem, "password", "")) ||
		!copy_option(profile->pin_code, sizeof(profile->pin_code),
				 option(context, modem, "pin_code", "")))
		goto out;
	profile->redial_after_apply = strcmp(option(context, modem, "redial_after_apply", "0"), "0") != 0;
	success = true;

out:
	if (four_g != NULL)
		uci_unload(context, four_g);
	if (edgenode != NULL)
		uci_unload(context, edgenode);
	if (context != NULL)
		uci_free_context(context);
	return success;
}

static bool valid_apn(const char *apn, bool allow_empty)
{
	size_t label_length = 0U;
	const unsigned char *cursor;

	if (apn == NULL || strlen(apn) > 100U)
		return false;
	if (*apn == '\0')
		return allow_empty;

	for (cursor = (const unsigned char *)apn; *cursor != '\0'; ++cursor) {
		if (*cursor == '.') {
			if (label_length == 0U || label_length > 63U || cursor[-1] == '-')
				return false;
			label_length = 0U;
			continue;
		}
		if (!( (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
			(*cursor >= '0' && *cursor <= '9') || *cursor == '-'))
			return false;
		if (label_length == 0U && *cursor == '-')
			return false;
		++label_length;
	}

	return label_length != 0U && label_length <= 63U && apn[strlen(apn) - 1U] != '-';
}

static bool valid_pin(const char *pin)
{
	size_t length;
	const char *cursor;

	if (pin == NULL || *pin == '\0')
		return true;
	length = strlen(pin);
	if (length < 4U || length > 8U)
		return false;
	for (cursor = pin; *cursor != '\0'; ++cursor)
		if (*cursor < '0' || *cursor > '9')
			return false;
	return true;
}

static bool valid_credential(const char *value)
{
	const unsigned char *cursor;

	for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor)
		if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == '"')
			return false;
	return true;
}

static bool validate_profile(const struct modem_profile *profile)
{
	bool auth_none = strcmp(profile->auth_type, "none") == 0;

	if (strcmp(profile->pdp_type, "ipv4") != 0 && strcmp(profile->pdp_type, "ipv6") != 0 &&
		strcmp(profile->pdp_type, "ipv4v6") != 0)
		return false;
	if (!auth_none && strcmp(profile->auth_type, "pap") != 0 &&
		strcmp(profile->auth_type, "chap") != 0 && strcmp(profile->auth_type, "pap_or_chap") != 0)
		return false;
	if (!valid_apn(profile->apn, profile->automatic_apn) ||
		(profile->automatic_apn && profile->apn[0] != '\0') ||
		(!profile->automatic_apn && profile->apn[0] == '\0') || !valid_pin(profile->pin_code) ||
		!valid_credential(profile->username) || !valid_credential(profile->password))
		return false;
	if ((auth_none && (profile->username[0] != '\0' || profile->password[0] != '\0')) ||
		(!auth_none && (profile->username[0] == '\0' || profile->password[0] == '\0')))
		return false;

	return profile->port[0] != '\0';
}

static unsigned auth_number(const char *auth_type)
{
	if (strcmp(auth_type, "pap") == 0)
		return 1U;
	if (strcmp(auth_type, "chap") == 0)
		return 2U;
	if (strcmp(auth_type, "pap_or_chap") == 0)
		return 3U;
	return 0U;
}

static const char *pdp_name(const char *pdp_type)
{
	if (strcmp(pdp_type, "ipv6") == 0)
		return "IPV6";
	if (strcmp(pdp_type, "ipv4v6") == 0)
		return "IPV4V6";
	return "IP";
}

static bool execute_commands(const struct modem_profile *profile, const char *action)
{
	struct termios original;
	char command[384];
	int lock = -1;
	int fd = -1;
	bool success = false;
	unsigned auth;

	lock = lock_modem();
	if (lock < 0) {
		fprintf(stderr, "modem is busy\n");
		return false;
	}

	fd = open(profile->port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cannot open modem AT port %s: %s\n", profile->port, strerror(errno));
		goto out;
	}
	if (!configure_port(fd, &original)) {
		fprintf(stderr, "cannot configure modem AT port %s: %s\n", profile->port, strerror(errno));
		goto out;
	}

	if (strcmp(action, "probe") == 0) {
		static const char *const commands[] = {
			"AT\r", "AT+CPIN?\r", "AT+CEREG?\r", "AT+GSN\r", "AT+QCCID\r",
			"AT+CSQ\r", "AT+CGDCONT?\r", "AT+COPS?\r"
		};
		size_t index;

		success = true;
		for (index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index)
			if (!run_at_command(fd, commands[index], true)) {
				fprintf(stderr, "AT transaction failed at probe step %zu\n", index + 1U);
				success = false;
				break;
			}
		if (success)
			printf("modem probe succeeded on %s\n", profile->port);
	} else if (strcmp(action, "redial") == 0) {
		success = run_at_command(fd, "AT+CFUN=1,1\r", false);
		if (success)
			printf("modem reconnecting\n");
	} else {
		if (profile->pin_code[0] != '\0') {
			snprintf(command, sizeof(command), "AT+CPIN=\"%s\"\r", profile->pin_code);
			success = run_at_command(fd, command, true);
			if (!success)
				fprintf(stderr, "AT transaction failed at SIM PIN\n");
		} else {
			success = true;
		}
		if (success && !profile->automatic_apn) {
			snprintf(command, sizeof(command), "AT+CGDCONT=1,\"%s\",\"%s\"\r",
				pdp_name(profile->pdp_type), profile->apn);
			success = run_at_command(fd, command, true);
			if (!success)
				fprintf(stderr, "AT transaction failed at PDP/APN\n");
		}
		if (success) {
			auth = auth_number(profile->auth_type);
			if (auth == 0U)
				snprintf(command, sizeof(command), "AT+CGAUTH=1,0\r");
			else
				snprintf(command, sizeof(command), "AT+CGAUTH=1,%u,\"%s\",\"%s\"\r",
					auth, profile->username, profile->password);
			success = run_at_command(fd, command, true);
			if (!success)
				fprintf(stderr, "AT transaction failed at authentication\n");
		}
		if (success && profile->redial_after_apply)
			success = run_at_command(fd, "AT+CFUN=1,1\r", false);
		if (success)
			printf(profile->redial_after_apply ?
				"mobile profile applied; modem reconnecting\n" : "mobile profile applied\n");
	}

	if (!success)
		fprintf(stderr, "modem rejected the requested command\n");

out:
	if (fd >= 0) {
		tcsetattr(fd, TCSANOW, &original);
		close(fd);
	}
	if (lock >= 0)
		close(lock);
	return success;
}

int main(int argc, char **argv)
{
	struct modem_profile profile;

	if (argc != 2 || (strcmp(argv[1], "probe") != 0 && strcmp(argv[1], "apply") != 0 &&
		strcmp(argv[1], "redial") != 0)) {
		fprintf(stderr, "usage: %s probe|apply|redial\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (!load_profile(&profile)) {
		fprintf(stderr, "cannot load 4G configuration\n");
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "apply") == 0 && !validate_profile(&profile)) {
		fprintf(stderr, "invalid 4G connection configuration\n");
		return EXIT_FAILURE;
	}

	return execute_commands(&profile, argv[1]) ? EXIT_SUCCESS : EXIT_FAILURE;
}
