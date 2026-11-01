/* Exercise the cold-session validator with an exactly sized allocation. */
#include "../boot/a733/stage2/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 2;
    long length = ftell(f);
    if (length < 0 || fseek(f, 0, SEEK_SET)) return 2;
    size_t bytes = (size_t)length;
    uint8_t *raw = malloc(bytes ? bytes : 1), *copy = malloc(bytes ? bytes : 1);
    if (!raw || !copy || fread(raw, 1, bytes, f) != bytes || fclose(f)) return 2;
    memcpy(copy, raw, bytes);
    int result = a733_stage2_session_valid(raw, bytes, strtoull(argv[2], NULL, 0));
    int expected = atoi(argv[3]);
    int failed = result != expected || memcmp(copy, raw, bytes);
    free(copy);
    free(raw);
    return failed;
}
