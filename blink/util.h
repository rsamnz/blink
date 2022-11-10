#ifndef BLINK_UTIL_H_
#define BLINK_UTIL_H_
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

bool mulo(uint64_t, uint64_t, uint64_t *);
bool startswith(const char *, const char *);
const char *doublenul(const char *, unsigned);
int popcount(uint64_t);
ssize_t readansi(int, char *, size_t);

#endif /* BLINK_UTIL_H_ */