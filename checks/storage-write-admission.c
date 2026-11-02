/* Host harness for the OS session admission of a CPU-return image: links the image's own session decoder and validator
 * (apps/os/session.c, os-image/return-admission.c) against the image's manifest section and runs them on a session. Usage:
 *   storage-write-admission SESSION     prints "decode=D valid=V board=B" */
#include "os.h"
#include "boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a733_storage_digest.h"
int os_session_valid(const struct os_session *);
int a733_board_session_valid(const struct a733_board_session *, uint64_t now);
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned char *raw = calloc(1, 8192);
    size_t n = fread(raw, 1, 8192, f);
    fclose(f);
    struct os_session d;
    memset(&d, 0, sizeof d);
    int decode = os_session_decode(raw, n, &d);
    printf("decode=%d valid=%d board=%d\n", decode, decode ? os_session_valid(&d) : 0, decode ? a733_board_session_valid(&d.board, 0) : 0);
    return 0;
}
