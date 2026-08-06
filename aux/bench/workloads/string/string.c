#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *strdup_(const char *s)
{
	char *d = malloc(strlen(s) + 1);
	strcpy(d, s);
	return d;
}

/* Naive O(len) realloc+strcat per append, mirroring string-append's cost
 * shape in the other variants (a fresh allocation sized to the combined
 * length every call), not an amortized-growth buffer. */
static char *append(char *acc, const char *add)
{
	size_t alen = acc ? strlen(acc) : 0;
	size_t blen = strlen(add);
	char *buf = malloc(alen + blen + 1);
	buf[0] = '\0';
	if (acc) strcat(buf, acc);
	strcat(buf, add);
	free(acc);
	return buf;
}

int main(void)
{
	char *acc = strdup_("");
	long matches = 0;
	for (long n = 6000; n != 0; n--) {
		char chunk[32];
		snprintf(chunk, sizeof(chunk), "%ld", n);
		char with_comma[34];
		snprintf(with_comma, sizeof(with_comma), "%s,", chunk);
		acc = append(acc, with_comma);
		if (chunk[0] == '1') matches++;
	}
	printf("%ld\n", (long)strlen(acc) + matches);
	free(acc);
	return 0;
}
