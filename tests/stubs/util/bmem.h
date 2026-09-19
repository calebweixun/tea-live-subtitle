#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
	char *copy = malloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static inline void bfree(void *ptr)
{
	free(ptr);
}
