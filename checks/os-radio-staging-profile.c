#include "../apps/bluetooth/bluetooth.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>

/* Admission of the exact immutable profile the host staging fixture emits. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct bluetooth_profile profile;
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(&profile, 1, sizeof(profile), file) == sizeof(profile));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);
    uint8_t digest[32];
    storage_digest_bytes(&profile, sizeof(profile), digest);
    assert(bluetooth_profile_valid(&profile, digest));
    puts("Staged OS headset profile admitted by native validator");
    return 0;
}
