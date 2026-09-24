#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* util/base.h log levels, as seen by code that includes <util/bmem.h>. */
#ifndef LOG_ERROR
#define LOG_ERROR 100
#define LOG_WARNING 200
#define LOG_INFO 300
#define LOG_DEBUG 400
#endif

static inline void *bzalloc(size_t size)
{
	return calloc(1, size);
}

static inline void *bmalloc(size_t size)
{
	return malloc(size);
}

static inline char *bstrdup(const char *text)
{
	if (!text)
		return NULL;
	size_t size = strlen(text) + 1;
	char *copy = (char *)malloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static inline void bfree(void *ptr)
{
	free(ptr);
}
