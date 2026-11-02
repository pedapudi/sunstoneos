// SHA-256 compression of whole 64-byte blocks with the ARMv8 Cryptographic Extension (SHA256H, SHA256H2, SHA256SU0,
// SHA256SU1), for a core whose ID_AA64ISAR0_EL1.SHA2 field is nonzero (drivers/storage/storage_digest.c checks it
// through storage_digest_use_extensions). The round constants are FIPS 180-4's.
//   void storage_sha256_ce(uint32_t h[8], const uint8_t *p, size_t blocks);
// h holds the state a..h as host words. Uses v0-v7 and v16-v19 only; no memory but h, the blocks and the constants.
    .arch armv8-a+crypto
    .text
    .global storage_sha256_ce
    .type storage_sha256_ce, %function
    .align 4
storage_sha256_ce:
    cbz     x2, 9f
    adrp    x3, .Lk
    add     x3, x3, :lo12:.Lk
    ld1     {v0.4s, v1.4s}, [x0]
1:  mov     v18.16b, v0.16b
    mov     v19.16b, v1.16b
    ld1     {v4.16b-v7.16b}, [x1], #64
    rev32   v4.16b, v4.16b
    rev32   v5.16b, v5.16b
    rev32   v6.16b, v6.16b
    rev32   v7.16b, v7.16b
    mov     x4, x3
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v4.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v5.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v6.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v7.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v4.4s, v5.4s
    sha256su1 v4.4s, v6.4s, v7.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v4.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v5.4s, v6.4s
    sha256su1 v5.4s, v7.4s, v4.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v5.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v6.4s, v7.4s
    sha256su1 v6.4s, v4.4s, v5.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v6.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v7.4s, v4.4s
    sha256su1 v7.4s, v5.4s, v6.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v7.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v4.4s, v5.4s
    sha256su1 v4.4s, v6.4s, v7.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v4.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v5.4s, v6.4s
    sha256su1 v5.4s, v7.4s, v4.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v5.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v6.4s, v7.4s
    sha256su1 v6.4s, v4.4s, v5.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v6.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v7.4s, v4.4s
    sha256su1 v7.4s, v5.4s, v6.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v7.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v4.4s, v5.4s
    sha256su1 v4.4s, v6.4s, v7.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v4.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v5.4s, v6.4s
    sha256su1 v5.4s, v7.4s, v4.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v5.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v6.4s, v7.4s
    sha256su1 v6.4s, v4.4s, v5.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v6.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    sha256su0 v7.4s, v4.4s
    sha256su1 v7.4s, v5.4s, v6.4s
    ld1     {v16.4s}, [x4], #16
    add     v17.4s, v7.4s, v16.4s
    mov     v2.16b, v0.16b
    sha256h  q0, q1, v17.4s
    sha256h2 q1, q2, v17.4s
    add     v0.4s, v0.4s, v18.4s
    add     v1.4s, v1.4s, v19.4s
    subs    x2, x2, #1
    b.ne    1b
    st1     {v0.4s, v1.4s}, [x0]
9:  ret
    .size storage_sha256_ce, . - storage_sha256_ce
    .section .rodata
    .align 4
.Lk:
    .word 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    .word 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    .word 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    .word 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    .word 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    .word 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    .word 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    .word 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    .word 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    .word 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    .word 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    .word 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    .word 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    .word 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    .word 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    .word 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
