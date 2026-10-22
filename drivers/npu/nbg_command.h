#ifndef SUNSTONE_NBG_COMMAND_H
#define SUNSTONE_NBG_COMMAND_H
/* Private templates and byte helpers shared by the NBG command assemblers.
 * Include only from drivers/npu sources; the short names are file-local. */
#include <stddef.h>
#include <stdint.h>

/* Command prefix/footer are the existing SDK preparation profile. The INIT
 * record is recognized in each model before being omitted from the command;
 * the native cold bootstrap remains a separate command. */
static const uint32_t prefix[] = {
    0x08010e02, 0x701, 0x48000000, 0x701, 0x0801022c, 0x1f,
    0x08010e03, 0xc23, 0x08010e02, 0x701, 0x48000000, 0x701
};
static const uint32_t footer[] = {
    0x08010e02, 0x30000701, 0x48000000, 0x30000701,
    0x08010e03, 0xc23, 0x08010e01, 0x40, 0x10000000, 0, 0, 0, 0, 0
};
static const uint32_t initial[] = {
    0x0801028a, 0x11, 0x08010e13, 2, 0x08010e4e, 0,
    0x08010e4f, 0, 0x08010e50, 0
};
static inline uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void put(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
static inline void words(unsigned char *p, const uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; ++i) put(p + i * 4, v[i]);
}
#endif
