#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <uci.h>

#define MODEM_LOCK "/tmp/edgenode/modem.lock"
#define DETAIL_DIR "/tmp/4ginfo"
#define DETAIL_STATUS "/tmp/4ginfo/modem.status"
#define DETAIL_STATUS_TMP "/tmp/4ginfo/modem.status.tmp"
#define DETAIL_RAW "/tmp/4ginfo/modem.raw"
#define AT_PORT_DEFAULT "/dev/ttyUSB2"
#define AT_RESPONSE_SIZE 8192U
#define AT_COMMAND_MAX 256U
#define AT_TIMEOUT_MS 2500
#define AT_OPTIONAL_TIMEOUT_MS 900

struct modem_profile {
	char port[128];
	char apn[101];
	char pdp_type[16];
	char auth_type[16];
	char username[129];
	char password[129];
	char pin_code[9];
	char radio_function[16];
	char operator_mode[16];
	char operator_mccmnc[8];
	bool automatic_apn;
	bool redial_after_apply;
};

struct at_result {
	bool ok;
	char response[AT_RESPONSE_SIZE];
};

struct probe_command {
	const char *label;
	const char *command;
	int timeout_ms;
	bool required;
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

static int final_response_state(const char *response)
{
	const char *cursor = response;

	while (cursor != NULL && *cursor != '\0') {
		const char *end = strchr(cursor, '\n');
		size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);

		while (length > 0U && (cursor[length - 1U] == '\r' || cursor[length - 1U] == ' ' ||
			cursor[length - 1U] == '\t'))
			--length;
		while (length > 0U && (*cursor == ' ' || *cursor == '\t')) {
			++cursor;
			--length;
		}
		if (length == 2U && memcmp(cursor, "OK", 2U) == 0)
			return 1;
		if ((length >= 5U && memcmp(cursor, "ERROR", 5U) == 0) ||
			(length >= 10U && memcmp(cursor, "+CME ERROR", 10U) == 0) ||
			(length >= 10U && memcmp(cursor, "+CMS ERROR", 10U) == 0))
			return -1;
		if (end == NULL)
			break;
		cursor = end + 1;
	}

	return 0;
}

/* Every invocation is one AT transaction: one write, then one final response. */
static bool run_at_command(int fd, const char *command, int timeout_ms,
	struct at_result *result)
{
	int64_t deadline;
	size_t used = 0U;

	memset(result, 0, sizeof(*result));
	(void)tcflush(fd, TCIFLUSH);
	if (!write_all(fd, command, strlen(command)))
		return false;
	deadline = monotonic_milliseconds() + timeout_ms;
	while (used + 1U < AT_RESPONSE_SIZE) {
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
		count = read(fd, result->response + used, AT_RESPONSE_SIZE - used - 1U);
		if (count > 0) {
			used += (size_t)count;
			result->response[used] = '\0';
			if (final_response_state(result->response) != 0) {
				result->ok = final_response_state(result->response) > 0;
				return result->ok;
			}
			continue;
		}
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		break;
	}
	return false;
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
	bool success = false;

	memset(profile, 0, sizeof(*profile));
	if (!copy_option(profile->port, sizeof(profile->port), AT_PORT_DEFAULT) ||
		!copy_option(profile->pdp_type, sizeof(profile->pdp_type), "ipv4") ||
		!copy_option(profile->auth_type, sizeof(profile->auth_type), "none") ||
		!copy_option(profile->radio_function, sizeof(profile->radio_function), "full") ||
		!copy_option(profile->operator_mode, sizeof(profile->operator_mode), "auto"))
		return false;
	profile->automatic_apn = true;
	context = uci_alloc_context();
	if (context == NULL || uci_load(context, "edgenode", &edgenode) != UCI_OK ||
		uci_load(context, "4ginfo", &four_g) != UCI_OK)
		goto out;
	modem = find_section(edgenode, "modem");
	if (!copy_option(profile->port, sizeof(profile->port), option(context, modem, "at_port", AT_PORT_DEFAULT)))
		goto out;
	modem = find_section(four_g, "modem");
	profile->automatic_apn = strcmp(option(context, modem, "automatic_apn", "1"), "0") != 0;
	if (!copy_option(profile->apn, sizeof(profile->apn), option(context, modem, "apn", "")) ||
		!copy_option(profile->pdp_type, sizeof(profile->pdp_type), option(context, modem, "pdp_type", "ipv4")) ||
		!copy_option(profile->auth_type, sizeof(profile->auth_type), option(context, modem, "auth_type", "none")) ||
		!copy_option(profile->username, sizeof(profile->username), option(context, modem, "username", "")) ||
		!copy_option(profile->password, sizeof(profile->password), option(context, modem, "password", "")) ||
		!copy_option(profile->pin_code, sizeof(profile->pin_code), option(context, modem, "pin_code", "")) ||
		!copy_option(profile->radio_function, sizeof(profile->radio_function), option(context, modem, "radio_function", "full")) ||
		!copy_option(profile->operator_mode, sizeof(profile->operator_mode), option(context, modem, "operator_mode", "auto")) ||
		!copy_option(profile->operator_mccmnc, sizeof(profile->operator_mccmnc), option(context, modem, "operator_mccmnc", "")))
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
		if (!(isalnum(*cursor) || *cursor == '-'))
			return false;
		if (label_length == 0U && *cursor == '-')
			return false;
		++label_length;
	}
	return label_length != 0U && label_length <= 63U && apn[strlen(apn) - 1U] != '-';
}

static bool valid_pin(const char *pin)
{
	size_t length = strlen(pin);
	const char *cursor;

	if (length == 0U)
		return true;
	if (length < 4U || length > 8U)
		return false;
	for (cursor = pin; *cursor != '\0'; ++cursor)
		if (!isdigit((unsigned char)*cursor))
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

static bool valid_digits(const char *value, size_t minimum, size_t maximum)
{
	size_t length = strlen(value);
	const char *cursor;

	if (length < minimum || length > maximum)
		return false;
	for (cursor = value; *cursor != '\0'; ++cursor)
		if (!isdigit((unsigned char)*cursor))
			return false;
	return true;
}

static bool validate_profile(const struct modem_profile *profile)
{
	bool auth_none = strcmp(profile->auth_type, "none") == 0;
	bool operator_auto = strcmp(profile->operator_mode, "auto") == 0;

	if (strcmp(profile->pdp_type, "ipv4") != 0 && strcmp(profile->pdp_type, "ipv6") != 0 && strcmp(profile->pdp_type, "ipv4v6") != 0)
		return false;
	if (!auth_none && strcmp(profile->auth_type, "pap") != 0 && strcmp(profile->auth_type, "chap") != 0 && strcmp(profile->auth_type, "pap_or_chap") != 0)
		return false;
	if (strcmp(profile->radio_function, "full") != 0 && strcmp(profile->radio_function, "minimum") != 0 && strcmp(profile->radio_function, "offline") != 0)
		return false;
	if (!operator_auto && strcmp(profile->operator_mode, "manual") != 0)
		return false;
	if (!operator_auto && !valid_digits(profile->operator_mccmnc, 5U, 6U))
		return false;
	if (!valid_apn(profile->apn, profile->automatic_apn) ||
		(profile->automatic_apn && profile->apn[0] != '\0') || (!profile->automatic_apn && profile->apn[0] == '\0') ||
		!valid_pin(profile->pin_code) || !valid_credential(profile->username) || !valid_credential(profile->password))
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

static const char *radio_function_value(const char *radio_function)
{
	if (strcmp(radio_function, "minimum") == 0)
		return "0";
	if (strcmp(radio_function, "offline") == 0)
		return "4";
	return "1";
}

static void trim_copy(char *destination, size_t capacity, const char *value, size_t length)
{
	while (length > 0U && isspace((unsigned char)value[0])) {
		++value;
		--length;
	}
	while (length > 0U && isspace((unsigned char)value[length - 1U]))
		--length;
	if (length >= capacity)
		length = capacity - 1U;
	memcpy(destination, value, length);
	destination[length] = '\0';
}

static bool value_after_prefix(const char *response, const char *prefix,
	char *destination, size_t capacity)
{
	const char *start = strstr(response, prefix);
	const char *end;

	if (start == NULL)
		return false;
	start += strlen(prefix);
	end = strpbrk(start, "\r\n");
	trim_copy(destination, capacity, start, end != NULL ? (size_t)(end - start) : strlen(start));
	return destination[0] != '\0';
}

static bool first_numeric_value(const char *response, char *destination, size_t capacity)
{
	const char *cursor = response;

	while (cursor != NULL && *cursor != '\0') {
		const char *end = strchr(cursor, '\n');
		size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
		char value[64];

		trim_copy(value, sizeof(value), cursor, length);
		if (valid_digits(value, 1U, 32U)) {
			(void)copy_option(destination, capacity, value);
			return true;
		}
		if (end == NULL)
			break;
		cursor = end + 1;
	}
	return false;
}

static void write_value(FILE *status, const char *key, const char *value)
{
	const char *cursor = value != NULL ? value : "";
	char sanitized[1024];
	size_t used = 0U;

	while (*cursor != '\0' && used + 1U < sizeof(sanitized)) {
		sanitized[used++] = (*cursor == '\r' || *cursor == '\n' || *cursor == '=') ? ' ' :
			((unsigned char)*cursor < 0x20U || (unsigned char)*cursor == 0x7fU ? ' ' : *cursor);
		++cursor;
	}
	sanitized[used] = '\0';
	fprintf(status, "%s=%s\n", key, sanitized);
}

static void write_response_value(FILE *status, const char *key, const struct at_result *result)
{
	const char *cursor;
	const char *start;
	const char *end;
	char value[1024];

	if (!result->ok) {
		write_value(status, key, "unsupported");
		return;
	}
	start = result->response;
	while (start != NULL && *start != '\0') {
		end = strpbrk(start, "\r\n");
		trim_copy(value, sizeof(value), start, end != NULL ? (size_t)(end - start) : strlen(start));
		if (value[0] != '\0' && strcmp(value, "OK") != 0 && strncmp(value, "ERROR", 5U) != 0 &&
			!(strncmp(value, "AT", 2U) == 0 && (value[2] == '\0' || value[2] == '+'))) {
			write_value(status, key, value);
			return;
		}
		cursor = end != NULL ? end + 1 : NULL;
		start = cursor;
	}
	write_value(status, key, "unsupported");
}

static void parse_registration(FILE *status, const char *prefix, const char *response, const char *key)
{
	const char *value = strstr(response, prefix);
	int mode = -1;
	int registration = -1;
	char area[32] = "";
	char cell[32] = "";
	int act = -1;
	char number[32];

	if (value == NULL || (sscanf(value, "%*[^:]: %d,%d,\"%31[^\"]\",\"%31[^\"]\",%d", &mode, &registration, area, cell, &act) < 2 &&
		sscanf(value, "%*[^:]: %d,%d", &mode, &registration) < 2)) {
		write_value(status, key, "unsupported");
		return;
	}
	snprintf(number, sizeof(number), "%d", registration);
	write_value(status, key, number);
	snprintf(number, sizeof(number), "%d", mode);
	write_value(status, "registration_mode", number);
	if (area[0] != '\0')
		write_value(status, "location_area_code", area);
	if (cell[0] != '\0')
		write_value(status, "cell_id", cell);
	if (act >= 0) {
		snprintf(number, sizeof(number), "%d", act);
		write_value(status, "registration_act", number);
	}
}

static void parse_probe_result(FILE *status, const char *label, const struct at_result *result)
{
	const char *value;
	char text[256];
	char number[32];
	int first;
	int second;

	if (strcmp(label, "manufacturer") == 0) {
		if (value_after_prefix(result->response, "+CGMI:", text, sizeof(text)))
			write_value(status, "manufacturer", text);
		else
			write_response_value(status, "manufacturer", result);
	} else if (strcmp(label, "model") == 0) {
		if (value_after_prefix(result->response, "+CGMM:", text, sizeof(text)))
			write_value(status, "model", text);
		else
			write_response_value(status, "model", result);
	} else if (strcmp(label, "firmware") == 0) {
		if (value_after_prefix(result->response, "+CGMR:", text, sizeof(text)))
			write_value(status, "firmware", text);
		else
			write_response_value(status, "firmware", result);
	} else if (strcmp(label, "imei") == 0 || strcmp(label, "imsi") == 0) {
		if (first_numeric_value(result->response, text, sizeof(text)))
			write_value(status, label, text);
	} else if (strcmp(label, "iccid") == 0) {
		if (value_after_prefix(result->response, "+QCCID:", text, sizeof(text)))
			write_value(status, "iccid", text);
		else
			write_response_value(status, "iccid", result);
	} else if (strcmp(label, "sim") == 0) {
		if (value_after_prefix(result->response, "+CPIN:", text, sizeof(text)))
			write_value(status, "sim_state_name", text);
	} else if (strcmp(label, "cereg") == 0) {
		parse_registration(status, "+CEREG:", result->response, "cereg_status");
	} else if (strcmp(label, "creg") == 0) {
		parse_registration(status, "+CREG:", result->response, "creg_status");
	} else if (strcmp(label, "cgreg") == 0) {
		parse_registration(status, "+CGREG:", result->response, "cgreg_status");
	} else if (strcmp(label, "csq") == 0) {
		value = strstr(result->response, "+CSQ:");
		if (value != NULL && sscanf(value, "+CSQ: %d,%d", &first, &second) == 2) {
			snprintf(number, sizeof(number), "%d", first);
			write_value(status, "csq", number);
			snprintf(number, sizeof(number), "%d", second);
			write_value(status, "ber", number);
			if (first >= 0 && first <= 31) {
				snprintf(number, sizeof(number), "%d", -113 + first * 2);
				write_value(status, "rssi_dbm", number);
				snprintf(number, sizeof(number), "%d", first * 100 / 31);
				write_value(status, "signal_percent", number);
			}
		}
	} else if (strcmp(label, "cesq") == 0) {
		write_response_value(status, "cesq", result);
	} else if (strcmp(label, "qcsq") == 0) {
		value = strstr(result->response, "+QCSQ:");
		if (value != NULL) {
			char network[32] = "";
			int rssi = -1;
			int rsrp = -1;
			int rsrq = -1;
			int sinr = -1;

			if (sscanf(value, "+QCSQ: \"%31[^\"]\",%d,%d,%d,%d", network, &rssi, &rsrp, &rsrq, &sinr) >= 2) {
				write_value(status, "network_type", network);
				snprintf(number, sizeof(number), "%d", rssi);
				write_value(status, "qcsq_rssi", number);
				snprintf(number, sizeof(number), "%d", rsrp);
				write_value(status, "rsrp_dbm", number);
				snprintf(number, sizeof(number), "%d", rsrq);
				write_value(status, "rsrq_db", number);
				snprintf(number, sizeof(number), "%d", sinr);
				write_value(status, "sinr_db", number);
			}
		}
	} else if (strcmp(label, "qnwinfo") == 0) {
		value = strstr(result->response, "+QNWINFO:");
		if (value != NULL) {
			char mode[32] = "";
			char plmn[32] = "";
			char band[64] = "";
			int channel = -1;

			if (sscanf(value, "+QNWINFO: \"%31[^\"]\",\"%31[^\"]\",\"%63[^\"]\",%d", mode, plmn, band, &channel) >= 3) {
				write_value(status, "network_mode", mode);
				write_value(status, "plmn", plmn);
				write_value(status, "band", band);
				if (channel >= 0) {
					snprintf(number, sizeof(number), "%d", channel);
					write_value(status, "channel", number);
				}
			}
		}
	} else if (strcmp(label, "cops") == 0) {
		value = strstr(result->response, "+COPS:");
		if (value != NULL) {
			int mode = -1;
			int format = -1;
			char name[96] = "";
			int act = -1;

			if (sscanf(value, "+COPS: %d,%d,\"%95[^\"]\",%d", &mode, &format, name, &act) >= 2) {
				snprintf(number, sizeof(number), "%d", mode);
				write_value(status, "operator_selection_mode", number);
				if (name[0] != '\0')
					write_value(status, "operator_name", name);
				if (act >= 0) {
					snprintf(number, sizeof(number), "%d", act);
					write_value(status, "access_technology", number);
				}
			}
		}
	} else if (strcmp(label, "cgdcont") == 0) {
		value = strstr(result->response, "+CGDCONT:");
		if (value != NULL) {
			char pdp[16] = "";
			char apn[128] = "";
			char address[128] = "";

			if (sscanf(value, "+CGDCONT: %*d,\"%15[^\"]\",\"%127[^\"]\",\"%127[^\"]\"", pdp, apn, address) >= 2) {
				write_value(status, "pdp_type", pdp);
				write_value(status, "apn", apn);
				if (address[0] != '\0')
					write_value(status, "pdp_address", address);
			}
		}
	} else if (strcmp(label, "cgpaddr") == 0) {
		value = strstr(result->response, "+CGPADDR:");
		if (value != NULL && sscanf(value, "+CGPADDR: %*d,\"%255[^\"]\"", text) == 1)
			write_value(status, "pdp_address", text);
	} else if (strcmp(label, "cfun") == 0) {
		value = strstr(result->response, "+CFUN:");
		if (value != NULL && sscanf(value, "+CFUN: %d", &first) == 1) {
			snprintf(number, sizeof(number), "%d", first);
			write_value(status, "radio_function", number);
		}
	} else if (strcmp(label, "cbc") == 0) {
		value = strstr(result->response, "+CBC:");
		int voltage = -1;

		if (value != NULL && sscanf(value, "+CBC: %d,%d,%d", &first, &second, &voltage) >= 2) {
			snprintf(number, sizeof(number), "%d", second);
			write_value(status, "battery_percent", number);
			if (voltage >= 0) {
				snprintf(number, sizeof(number), "%d", voltage);
				write_value(status, "battery_voltage_mv", number);
			}
		}
	} else if (strcmp(label, "cnum") == 0) {
		write_response_value(status, "msisdn", result);
	} else if (strcmp(label, "cimi") == 0) {
		if (first_numeric_value(result->response, text, sizeof(text)))
			write_value(status, "imsi", text);
	} else if (strcmp(label, "cgatt") == 0) {
		if (value_after_prefix(result->response, "+CGATT:", text, sizeof(text)))
			write_value(status, "packet_attached", text);
	} else if (strcmp(label, "cgact") == 0) {
		if (value_after_prefix(result->response, "+CGACT:", text, sizeof(text)))
			write_value(status, "pdp_active", text);
	} else if (strcmp(label, "qsimstat") == 0) {
		write_response_value(status, "sim_presence", result);
	} else if (strcmp(label, "qgps") == 0) {
		write_response_value(status, "gps_status", result);
	} else if (strcmp(label, "qgpsloc") == 0) {
		write_response_value(status, "gps_location", result);
	} else if (strcmp(label, "qcfg_mode") == 0) {
		write_response_value(status, "preferred_mode", result);
	} else if (strcmp(label, "qcfg_band") == 0) {
		write_response_value(status, "preferred_band", result);
	} else if (strcmp(label, "cgcontrdp") == 0) {
		write_response_value(status, "cgcontrdp", result);
	} else if (strcmp(label, "qiact") == 0) {
		write_response_value(status, "qiact", result);
	} else if (strcmp(label, "cgnapn") == 0) {
		write_response_value(status, "cgnapn", result);
	} else if (strcmp(label, "qca_info") == 0) {
		write_response_value(status, "qca_info", result);
	} else if (strcmp(label, "cpms") == 0) {
		write_response_value(status, "cpms", result);
	} else if (strcmp(label, "cscs") == 0) {
		write_response_value(status, "cscs", result);
	} else if (strcmp(label, "cmgf") == 0) {
		write_response_value(status, "cmgf", result);
	} else if (strcmp(label, "cnmi") == 0) {
		write_response_value(status, "cnmi", result);
	} else if (strcmp(label, "qeng_serving") == 0) {
		write_response_value(status, "serving_cell", result);
	} else if (strcmp(label, "qeng_neighbour") == 0) {
		write_response_value(status, "neighbour_cell", result);
	} else if (strcmp(label, "qpref_mode") == 0) {
		write_response_value(status, "preferred_mode", result);
	}
}

static bool probe_modem(const struct modem_profile *profile, int fd)
{
	static const struct probe_command commands[] = {
		{ "at", "AT\r", AT_TIMEOUT_MS, true },
		{ "manufacturer", "AT+CGMI\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "model", "AT+CGMM\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "firmware", "AT+CGMR\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "imei", "AT+GSN\r", AT_TIMEOUT_MS, true },
		{ "imsi", "AT+CIMI\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "iccid", "AT+QCCID\r", AT_TIMEOUT_MS, true },
		{ "sim", "AT+CPIN?\r", AT_TIMEOUT_MS, true },
		{ "qsimstat", "AT+QSIMSTAT?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cereg", "AT+CEREG?\r", AT_TIMEOUT_MS, true },
		{ "creg", "AT+CREG?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgreg", "AT+CGREG?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "csq", "AT+CSQ\r", AT_TIMEOUT_MS, true },
		{ "cesq", "AT+CESQ\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qcsq", "AT+QCSQ\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cops", "AT+COPS?\r", AT_TIMEOUT_MS, false },
		{ "qnwinfo", "AT+QNWINFO\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qeng_serving", "AT+QENG=\"servingcell\"\r", AT_TIMEOUT_MS, false },
		{ "qeng_neighbour", "AT+QENG=\"neighbourcell\"\r", AT_TIMEOUT_MS, false },
		{ "qca_info", "AT+QCAINFO\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cfun", "AT+CFUN?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cbc", "AT+CBC\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cnum", "AT+CNUM\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgdcont", "AT+CGDCONT?\r", AT_TIMEOUT_MS, false },
		{ "cgauth", "AT+CGAUTH?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgnapn", "AT+CGNAPN\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgatt", "AT+CGATT?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgact", "AT+CGACT?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgpaddr", "AT+CGPADDR=1\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cgcontrdp", "AT+CGCONTRDP\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qiact", "AT+QIACT?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cpms", "AT+CPMS?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cscs", "AT+CSCS?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cmgf", "AT+CMGF?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "cnmi", "AT+CNMI?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qgps", "AT+QGPS?\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qgpsloc", "AT+QGPSLOC?\r", AT_TIMEOUT_MS, false },
		{ "qcfg_mode", "AT+QCFG=\"nwscanmode\"\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qcfg_band", "AT+QCFG=\"band\"\r", AT_OPTIONAL_TIMEOUT_MS, false },
		{ "qpref_mode", "AT+QNWPREFCFG=\"mode_pref\"\r", AT_OPTIONAL_TIMEOUT_MS, false }
	};
	FILE *status = NULL;
	FILE *raw = NULL;
	bool base_ok = true;
	size_t index;

	if (mkdir(DETAIL_DIR, 0700) != 0 && errno != EEXIST)
		return false;
	status = fopen(DETAIL_STATUS_TMP, "w");
	raw = fopen(DETAIL_RAW ".tmp", "w");
	if (status == NULL || raw == NULL)
		goto fail;
	(void)chmod(DETAIL_STATUS_TMP, 0600);
	(void)chmod(DETAIL_RAW ".tmp", 0600);
	write_value(status, "at_port", profile->port);
	write_value(status, "updated_at", "0");
	fprintf(raw, "4G AT probe; one command per transaction\n\n");
	for (index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
		struct at_result result;

		if (!run_at_command(fd, commands[index].command, commands[index].timeout_ms, &result)) {
			fprintf(raw, "[%s] %s\n<unsupported or timeout>\n\n", commands[index].label, commands[index].command);
			write_value(status, commands[index].label, "unsupported");
			if (commands[index].required)
				base_ok = false;
			continue;
		}
		fprintf(raw, "[%s] %s\n%s\n", commands[index].label, commands[index].command, result.response);
		parse_probe_result(status, commands[index].label, &result);
	}
	{
		char value[32];
		snprintf(value, sizeof(value), "%ld", (long)time(NULL));
		write_value(status, "updated_at", value);
	}
	write_value(status, "probe_ok", base_ok ? "1" : "0");
	write_value(status, "available", base_ok ? "1" : "0");
	write_value(status, "raw_path", DETAIL_RAW);
	{
		int status_rc = fclose(status);
		int raw_rc = fclose(raw);

		status = NULL;
		raw = NULL;
		if (status_rc != 0 || raw_rc != 0)
			goto fail_closed;
	}
	if (rename(DETAIL_STATUS_TMP, DETAIL_STATUS) != 0 || rename(DETAIL_RAW ".tmp", DETAIL_RAW) != 0)
		return false;
	return base_ok;
fail:
	if (status != NULL)
		fclose(status);
	if (raw != NULL)
		fclose(raw);
fail_closed:
	(void)unlink(DETAIL_STATUS_TMP);
	(void)unlink(DETAIL_RAW ".tmp");
	return false;
}

static bool execute_profile(const struct modem_profile *profile, int fd)
{
	struct at_result result;
	char command[384];
	unsigned auth;

	if (profile->pin_code[0] != '\0') {
		snprintf(command, sizeof(command), "AT+CPIN=\"%s\"\r", profile->pin_code);
		if (!run_at_command(fd, command, AT_TIMEOUT_MS, &result)) {
			fprintf(stderr, "AT transaction failed at SIM PIN\n");
			return false;
		}
	}
	if (!profile->automatic_apn) {
		snprintf(command, sizeof(command), "AT+CGDCONT=1,\"%s\",\"%s\"\r", pdp_name(profile->pdp_type), profile->apn);
		if (!run_at_command(fd, command, AT_TIMEOUT_MS, &result)) {
			fprintf(stderr, "AT transaction failed at PDP/APN\n");
			return false;
		}
	}
	if (strcmp(profile->auth_type, "none") != 0) {
		auth = auth_number(profile->auth_type);
		snprintf(command, sizeof(command), "AT+CGAUTH=1,%u,\"%s\",\"%s\"\r", auth, profile->username, profile->password);
		if (!run_at_command(fd, command, AT_TIMEOUT_MS, &result)) {
			fprintf(stderr, "AT transaction failed at authentication\n");
			return false;
		}
	}
	if (strcmp(profile->operator_mode, "auto") == 0) {
		if (!run_at_command(fd, "AT+COPS=0\r", AT_TIMEOUT_MS, &result)) {
			fprintf(stderr, "AT transaction failed at automatic operator selection\n");
			return false;
		}
	} else {
		snprintf(command, sizeof(command), "AT+COPS=1,2,\"%s\"\r", profile->operator_mccmnc);
		if (!run_at_command(fd, command, AT_TIMEOUT_MS, &result)) {
			fprintf(stderr, "AT transaction failed at manual operator selection\n");
			return false;
		}
	}
	snprintf(command, sizeof(command), "AT+CFUN=%s\r", radio_function_value(profile->radio_function));
	if (!run_at_command(fd, command, AT_TIMEOUT_MS, &result)) {
		fprintf(stderr, "AT transaction failed at radio function\n");
		return false;
	}
	if (profile->redial_after_apply && !run_at_command(fd, "AT+CFUN=1,1\r", AT_TIMEOUT_MS, &result)) {
		fprintf(stderr, "AT transaction failed at modem redial\n");
		return false;
	}
	return true;
}

static bool valid_custom_command(const char *command)
{
	size_t length = strlen(command);
	const unsigned char *cursor;

	if (length < 2U || length > AT_COMMAND_MAX || strncmp(command, "AT", 2U) != 0)
		return false;
	for (cursor = (const unsigned char *)command; *cursor != '\0'; ++cursor)
		if (*cursor < 0x20U || *cursor == 0x7fU)
			return false;
	return true;
}

static bool execute_command(const struct modem_profile *profile, const char *action,
	const char *custom_command)
{
	struct termios original;
	struct at_result result;
	char command[AT_COMMAND_MAX + 2U];
	int lock = -1;
	int fd = -1;
	bool success = false;

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
		success = probe_modem(profile, fd);
		printf(success ? "complete 4G probe succeeded on %s\n" : "4G probe completed with required AT failures on %s\n", profile->port);
	} else if (strcmp(action, "redial") == 0) {
		success = run_at_command(fd, "AT+CFUN=1,1\r", AT_TIMEOUT_MS, &result);
		if (success)
			printf("modem reconnecting\n");
	} else if (strcmp(action, "at") == 0) {
		snprintf(command, sizeof(command), "%s\r", custom_command);
		success = run_at_command(fd, command, AT_TIMEOUT_MS, &result);
		if (result.response[0] != '\0')
			fputs(result.response, stdout);
	} else {
		success = execute_profile(profile, fd);
		if (success)
			printf(profile->redial_after_apply ? "mobile profile applied; modem reconnecting\n" : "mobile profile applied\n");
	}
	if (!success && strcmp(action, "probe") != 0)
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
	const char *action;

	if (argc < 2 || (strcmp(argv[1], "probe") != 0 && strcmp(argv[1], "apply") != 0 &&
		strcmp(argv[1], "redial") != 0 && strcmp(argv[1], "at") != 0)) {
		fprintf(stderr, "usage: %s probe|apply|redial|at <AT command>\n", argv[0]);
		return EXIT_FAILURE;
	}
	action = argv[1];
	if (strcmp(action, "at") == 0 && (argc != 3 || !valid_custom_command(argv[2]))) {
		fprintf(stderr, "AT command must start with AT and contain no control characters\n");
		return EXIT_FAILURE;
	}
	if (!load_profile(&profile)) {
		fprintf(stderr, "cannot load 4G configuration\n");
		return EXIT_FAILURE;
	}
	if (strcmp(action, "apply") == 0 && !validate_profile(&profile)) {
		fprintf(stderr, "invalid 4G connection configuration\n");
		return EXIT_FAILURE;
	}
	return execute_command(&profile, action, strcmp(action, "at") == 0 ? argv[2] : NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
}
