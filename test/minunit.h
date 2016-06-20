/* file: minunit.h */
/* site: http://www.jera.com/techinfo/jtns/jtn002.html */
/* added functionality by twinshadow */

#pragma once

#include <stdio.h>
#include <errno.h>
#include <string.h>

#define STRERR (errno ? strerror(errno) : "None")
#define PRINTAT stderr

#define mu_error(message, ...)                                       \
	fprintf(stderr, "ERROR: %s:%u: errno %d: %s: " message "\n", \
	    __FILE__, __LINE__, errno, STRERR, __VA_ARGS__)

#define mu_assert(test, message, ...) do {                       \
	if (test == 0) {                                         \
		fprintf(PRINTAT, "ASSERT: %s:%d: " message "\n", \
		    __FILE__, __LINE__, __VA_ARGS__);            \
	}                                                        \
} while (0)

#define mu_run_test(name, test) do {                        \
	if (test() == 0) {                                  \
		tests_pass++;                               \
		fprintf(PRINTAT, "FAIL: %s:%d: " name "\n", \
		    __FILE__, __LINE__);                    \
	} else {                                            \
		tests_fail++;                               \
		fprintf(PRINTAT, "PASS: %s:%d: " name "\n", \
		    __FILE__, __LINE__);                    \
	}                                                   \
	tests_run++;                                        \
} while (0)

extern int tests_run;
extern int tests_pass;
extern int tests_fail;
