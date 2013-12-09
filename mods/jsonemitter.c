#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <ctype.h>
#include <stdarg.h>
#include "json.h"

#define PAGE_SIZE 4096

/* generated by running:
 * $ perl -ne '/payload_(start|new)_\w+\(\w+, \"(\w+)\"/ && print "$2\n"' *.c | sort -u | gperf -m 10000 -H hash_output_key -N in_output_word_set -C -r > output_hash_raw.c
 *
 * After updating output_hash_raw.c, you must update json.h, so that the size of 
 * payload->hashed_keys == (MAX_HASH_VALUE/32) + 1
 */
#include "output_hash_raw.c"

#define WORD_OFFSET(b) ((b) / 32)
#define BIT_OFFSET(b)  ((b) % 32)

void adjust_payload_len(struct payload * po, size_t len) {
	if(po->bufused + len < po->buflen)
		return;
	po->buflen += ((len / PAGE_SIZE) + 1) * PAGE_SIZE;
	po->json_buf = realloc(po->json_buf, po->buflen);
}

struct payload * payload_new() {
	struct payload * ret = malloc(sizeof(struct payload));
	memset(ret, 0, sizeof(struct payload));
	adjust_payload_len(ret, sizeof("{ "));
	ret->bufused = sprintf(ret->json_buf, "{ ");
	ret->keep_auxdata = 1;
	return ret;
}

int payload_add_key(struct payload * po, char * key) {
	if(key == NULL)
		return 1;
	size_t keylen = strlen(key);
	if(po->use_hash) {
		unsigned int hashval = hash_output_key(key, keylen);
		if(!(po->hashed_keys[WORD_OFFSET(hashval)] & (1 << BIT_OFFSET(hashval))))
			return 0;
	}

	adjust_payload_len(po, keylen + sizeof("\"\": "));
	po->bufused += sprintf(po->json_buf + po->bufused,
		"\"%s\": ", key);
	return 1;
}

void payload_new_string(struct payload * po, char * key, char * val) {
	if(!payload_add_key(po, key))
		return;
	if(val == NULL) {
		adjust_payload_len(po, sizeof("null, "));
		po->bufused += sprintf(po->json_buf + po->bufused,
			"null, ");
		return;
	}

	size_t len = 0;
	char * ptr = val, *out;
	char * save;
	unsigned char token;
	while((token=*ptr) && ++len) {
		if(strchr("\"\\\b\f\n\r\t",token))
			len++;
		else if(token < 32 || (token > 127 && token < 160))
			len += 5;
		ptr++;
	}
	
	adjust_payload_len(po, len + sizeof("\"\", "));
	ptr = val;
	po->bufused += sprintf(po->json_buf + po->bufused, "\"");
	out = po->json_buf + po->bufused;
	save = out;
	while(*ptr != '\0') {
		if ((unsigned char)*ptr>31 && *ptr!='\"' && *ptr!='\\')
			*(out++)=*ptr++;
		else
		{
			*(out++) = '\\';
			switch (token=*ptr++)
			{
				case '\\': *(out++)='\\'; break;
				case '\"': *(out++)='\"'; break;
				case '\b': *(out++)='b'; break;
				case '\f': *(out++)='f'; break;
				case '\n': *(out++)='n'; break;
				case '\r': *(out++)='r'; break;
				case '\t': *(out++)='t'; break;
				default: 
					sprintf(out,"u%04x",token);
					out += 5;
					break;	/* escape and print */
			}
		}
	}
	*out = '\0';
	if(po->keep_auxdata) {
		if(key && strcmp(key, "type") == 0)
			po->type = strdup(save);
		else if(key && strcmp(key, "host_name") == 0)
			po->host_name = strdup(save);
		else if(key && strcmp(key, "service_description") == 0)
			po->service_description = strdup(save);
	}
	po->bufused += out - save;
	po->bufused += sprintf(po->json_buf + po->bufused, "\", ");
}

void payload_new_integer(struct payload * po, char * key, long long val) {
	if(!payload_add_key(po, key))
		return;
	adjust_payload_len(po, sizeof("INT64_MAX, "));
	po->bufused += sprintf(po->json_buf + po->bufused,
		"%lli, ", val);
}

void payload_new_boolean(struct payload * po, char * key, int val) {
	if(!payload_add_key(po, key))
		return;
	adjust_payload_len(po, sizeof("false, "));
	if(val > 0)
		po->bufused += sprintf(po->json_buf + po->bufused,
			"true, ");
	else
		po->bufused += sprintf(po->json_buf + po->bufused,
			"false, ");
}

void payload_new_double(struct payload * po, char * key, double val) {
	if(!payload_add_key(po, key))
		return;
	adjust_payload_len(po, DBL_MAX_10_EXP + sizeof(", "));
	po->bufused += snprintf(po->json_buf + po->bufused,
		DBL_MAX_10_EXP, "%f", val) - 1;
	po->bufused += sprintf(po->json_buf + po->bufused,
		", ");
}

void payload_new_timestamp(struct payload * po,
	char* key, struct timeval * tv) {
	if(!payload_add_key(po, key))
		return;
	adjust_payload_len(po, sizeof("{  }, "));
	po->bufused += sprintf(po->json_buf + po->bufused, "{ ");
	payload_new_integer(po, "tv_sec", tv->tv_sec);
	payload_new_integer(po, "tv_usec", tv->tv_usec);
	po->bufused -= 2;
	po->bufused += sprintf(po->json_buf + (po->bufused),
		" }, ");
}

void payload_new_statestr(struct payload * ret, char * key, int state,
	int checked, int svc) {
	char * service_state_strings[] = { "OK", "WARNING", "CRITICAL", "UNKNOWN" };
	char * host_state_strings[] = { "UP", "DOWN", "UNREACHABLE" };

	if(!checked) {
		payload_new_string(ret, key, "PENDING");
		return;
	}
	if(svc)
		payload_new_string(ret, key, service_state_strings[state]);
	else
		payload_new_string(ret, key, host_state_strings[state]);
}


int payload_start_array(struct payload * po, char * key) {
	if(!payload_add_key(po, key))
		return 0;
	adjust_payload_len(po, sizeof("[ "));
	po->bufused += sprintf(po->json_buf + po->bufused, "[ ");
	return 1;
}

void payload_end_array(struct payload * po) {
	adjust_payload_len(po, sizeof(", "));
	if(*(po->json_buf + po->bufused - 2) != '[')
		po->bufused -= 2;
	po->bufused += sprintf(po->json_buf + po->bufused, " ], ");
}

int payload_start_object(struct payload * po, char * key) {
	if(!payload_add_key(po, key))
		return 0;
	adjust_payload_len(po, sizeof("{ "));
	po->bufused += sprintf(po->json_buf + po->bufused, "{ ");
	return 1;
}

void payload_end_object(struct payload * po) {
	adjust_payload_len(po, sizeof(", "));
	if(*(po->json_buf + po->bufused - 2) != '{')
		po->bufused -= 2;
	po->bufused += sprintf(po->json_buf + po->bufused, " }, ");
}

void payload_finalize(struct payload * po) {
	size_t offset = po->bufused;
	if(offset > 2)
		offset -= 2;
	adjust_payload_len(po, sizeof("] "));
	if(po->json_buf[0] == '[')
		sprintf(po->json_buf + offset, " ]");
	else
		sprintf(po->json_buf + offset, " }");
	if(offset == 2)
		po->bufused += 2;
}

void payload_hash_key(struct payload * po, const char * key) {
	size_t keylen = strlen(key);
	unsigned int hashval;

	if(!in_output_word_set(key, keylen))
		return;

	if(!po->use_hash) {
		memset(po->hashed_keys, 0, sizeof(po->hashed_keys));
		po->use_hash = 1;
	}

	hashval = hash_output_key(key, keylen);
	po->hashed_keys[WORD_OFFSET(hashval)] |= (1 << BIT_OFFSET(hashval));
}

int payload_has_keys(struct payload * po, ...) {
	va_list ap;
	char * key;
	int okay = 0;

	if(!po->use_hash)
		return 1;

	va_start(ap, po);
	while((key = va_arg(ap, char*)) != NULL) {
		unsigned int keylen = strlen(key);
		unsigned int hashval = hash_output_key(key, keylen);
		if(po->hashed_keys[WORD_OFFSET(hashval)] & (1 << BIT_OFFSET(hashval)))
			okay++;
	}
    va_end(ap);
	return okay;
}
