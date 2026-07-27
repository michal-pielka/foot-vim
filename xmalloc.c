#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "xmalloc.h"
#include "debug.h"

static void *
check_alloc(void *alloc)
{
    FATAL_ERROR_ON(alloc == NULL, ENOMEM);
    return alloc;
}

void *
xmalloc(size_t size)
{
    return check_alloc(malloc(likely(size) ? size : 1));
}

void *
xcalloc(size_t nmemb, size_t size)
{
    xassert(size != 0);
    return check_alloc(calloc(likely(nmemb) ? nmemb : 1, size));
}

void *
xrealloc(void *ptr, size_t size)
{
    xassert(size != 0);
    return check_alloc(realloc(ptr, size));
}

// reallocarray(3) was only added to POSIX in Issue 8 (2024) and isn't
// present on some platforms (e.g. Android API levels < 29), so this
// function is both for portability and ENOMEM handling.
void *
xreallocarray(void *ptr, size_t nmemb, size_t size)
{
    FATAL_ERROR_ON(nmemb == 0 || size == 0, EINVAL);
    FATAL_ERROR_ON(size > SIZE_MAX / nmemb, EOVERFLOW);
    return xrealloc(ptr, nmemb * size);
}

char *
xstrdup(const char *str)
{
    return check_alloc(strdup(str));
}

char *
xstrndup(const char *str, size_t n)
{
    return check_alloc(strndup(str, n));
}

char32_t *
xc32dup(const char32_t *str)
{
    return check_alloc(c32dup(str));
}

static VPRINTF(2) int
xvasprintf_(char **strp, const char *format, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, format, ap2);
    FATAL_ERROR_ON(n < 0, EILSEQ);
    va_end(ap2);
    *strp = xmalloc(n + 1);
    return vsnprintf(*strp, n + 1, format, ap);
}

char *
xvasprintf(const char *format, va_list ap)
{
    char *str;
    xvasprintf_(&str, format, ap);
    return str;
}

char *
xasprintf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char *str = xvasprintf(format, ap);
    va_end(ap);
    return str;
}
