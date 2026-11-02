// SHA-256 of one 64-byte message block from the standard initial state, with the ARMv8 Cryptographic Extension
// (SHA256H, SHA256H2, SHA256SU0, SHA256SU1). The caller has checked ID_AA64ISAR0_EL1.SHA2 != 0.
//   void sha256_block_ce(const uint8_t block[64], uint8_t digest[32]);
// The 64-byte block holds big-endian message words; the digest is written big-endian. Uses v0-v7 and v16-v23.
    .arch armv8-a+crypto
    .text
    .global sha256_block_ce
    .type sha256_block_ce, %function
    .align 4
sha256_block_ce:
    adrp    x2, 3f
    add     x2, x2, :lo12:3f              // K constants
    adrp    x3, 4f
    add     x3, x3, :lo12:4f              // initial state: a b c d, e f g h
    ld1     {v0.4s, v1.4s}, [x3]          // v0 = abcd, v1 = efgh
    mov     v18.16b, v0.16b               // saved state
    mov     v19.16b, v1.16b
    ld1     {v4.16b-v7.16b}, [x0]         // message words
    rev32   v4.16b, v4.16b
    rev32   v5.16b, v5.16b
    rev32   v6.16b, v6.16b
    rev32   v7.16b, v7.16b
    mov     x4, #4                        // four groups of sixteen rounds
1:
    ld1     {v16.4s-v17.4s}, [x2], #32    // K[i..i+7]
    // rounds 0-3 of the group use message v4
    add     v20.4s, v4.4s, v16.4s
    sha256su0 v4.4s, v5.4s
    mov     v22.16b, v0.16b
    sha256h   q0, q1, v20.4s
    sha256h2  q1, q22, v20.4s
    sha256su1 v4.4s, v6.4s, v7.4s
    // rounds 4-7 use message v5
    add     v21.4s, v5.4s, v17.4s
    sha256su0 v5.4s, v6.4s
    mov     v22.16b, v0.16b
    sha256h   q0, q1, v21.4s
    sha256h2  q1, q22, v21.4s
    sha256su1 v5.4s, v7.4s, v4.4s
    ld1     {v16.4s-v17.4s}, [x2], #32    // K[i+8..i+15]
    // rounds 8-11 use message v6
    add     v20.4s, v6.4s, v16.4s
    sha256su0 v6.4s, v7.4s
    mov     v22.16b, v0.16b
    sha256h   q0, q1, v20.4s
    sha256h2  q1, q22, v20.4s
    sha256su1 v6.4s, v4.4s, v5.4s
    // rounds 12-15 use message v7
    add     v21.4s, v7.4s, v17.4s
    sha256su0 v7.4s, v4.4s
    mov     v22.16b, v0.16b
    sha256h   q0, q1, v21.4s
    sha256h2  q1, q22, v21.4s
    sha256su1 v7.4s, v5.4s, v6.4s
    subs    x4, x4, #1
    b.ne    1b
    add     v0.4s, v0.4s, v18.4s
    add     v1.4s, v1.4s, v19.4s
    rev32   v0.16b, v0.16b
    rev32   v1.16b, v1.16b
    st1     {v0.16b, v1.16b}, [x1]
    ret
    .size sha256_block_ce, .-sha256_block_ce
    .section .rodata
    .align 4
3:  .word 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
    .word 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
    .word 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
    .word 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
    .word 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
    .word 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
    .word 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
    .word 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
4:  .word 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
