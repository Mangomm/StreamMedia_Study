#ifndef IJKSDL_MISC_H
#define IJKSDL_MISC_H
#include <stdlib.h>
#include <memory.h>

#ifndef IJKMAX
#define IJKMAX(a, b)    ((a) > (b) ? (a) : (b))
#endif

#ifndef IJKMIN
#define IJKMIN(a, b)    ((a) < (b) ? (a) : (b))
#endif

#ifndef IJKALIGN
#define IJKALIGN(x, align) ((( x ) + (align) - 1) / (align) * (align))
#endif

#define IJK_CHECK_RET(condition__, retval__, ...) \
    if (!(condition__)) { \
        ALOGE(__VA_ARGS__); \
        return (retval__); \
    }

#ifndef NELEM
#define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))
#endif

inline static void *mallocz(size_t size)
{
    void *mem = malloc(size);
    if (!mem)
        return mem;

    memset(mem, 0, size);
    return mem;
}

inline static void freep(void **mem)
{
    if (mem && *mem) {
        free(*mem);
        *mem = NULL;
    }
}
#endif // IJKSDL_MISC_H
