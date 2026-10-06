# XeCrypt services

`runtime/src/hle_xboxkrnl_crypto_more.cpp` (added for Halo 3, 3 Oct 2026). SHA-1
(`XeCryptSha*`) stays in `hle_xboxkrnl_strings.cpp`, which registers this unit.
Inaccessible guest ranges are guest-access fatals (the console would fault).

| Export | Ordinal | Behaviour |
| --- | --- | --- |
| `XeCryptAesKey(state, key[16])` | 0x159 | FIPS-197 AES-128 key expansion into `XECRYPT_AES_STATE` (below). |
| `XeCryptAesCbc(state, in, cb, out, feed[16], encrypt)` | 0x15B | CBC over whole blocks; `feed` is the IV on entry and the last ciphertext block on return, so calls chain. `in == out` allowed. A length that is not a multiple of 16 has no established contract: explicit `UNIMPLEMENTED` fatal. |
| `XeCryptRandom(buffer, cb)` | 0x18A | `cb` bytes from the host random source (`src/host_random.h`: RDRAND, else `getentropy`); no source is a platform fatal, never predictable bytes. |
| `XeCryptBnQw_SwapDwQwLeBe(in, out, cqw)` | 0x170 | Byte-reverses each qword, qword order kept: little-endian byte string <-> native qword bignum. `in == out` allowed. |
| `XeCryptBnQwNeRsaPubCrypt(a, b, key) -> BOOL` | 0x16D | `b = a^e mod n` (Montgomery, 64-bit limbs). FALSE, `b` untouched, when `a >= n` (outside the RSA primitive's domain, RFC 8017 RSAEP/RSAVP1). A key size of 0 or above 64 qwords, an even or unit modulus: no contract, explicit fatal. `a == b` allowed. |
| `XeCryptBnDwLePkcs1Format(hash[20], type, sig, cb)` | 0x162 | Writes the PKCS#1 v1.5 block for a SHA-1 hash (below). Type above 2 or `cb` outside 39..512: explicit fatal. |
| `XeCryptBnDwLePkcs1Verify(hash[20], sig, cb) -> BOOL` | 0x163 | TRUE when `sig` is the block of `hash` in any of the three forms; FALSE otherwise and for `cb` outside 39..512. |

## Layouts

- `XECRYPT_AES_STATE` (0x160 bytes): `+0x000 keytabenc[11][16]`, `+0x0B0
  keytabdec[11][16]`. `keytabenc[r]` is round key `r` as the FIPS-197 byte
  sequence (big-endian words, the Xenon layout). `keytabdec` is the equivalent
  inverse cipher schedule (FIPS-197 5.3.5): `dec[0] = enc[10]`, `dec[r] =
  InvMixColumns(enc[10-r])`, `dec[10] = enc[0]`; decryption uses it, so a state
  the title copies around keeps working. The layout follows rexglue-sdk c94f5eb
  / Xenia 95a5c3e `xboxkrnl_crypt` (BSD-3, read only), whose own note says the
  console's table order is unverified: NOT TESTED against a console dump.
- `XECRYPT_RSA`: `+0 DWORD cqw`, `+4 DWORD public exponent`, `+8 QWORD
  reserved`, `+0x10 QWORD modulus[cqw]`. Bignums ("QwNe") are qword arrays,
  least significant qword first, each qword big-endian.
- PKCS#1 block ("DwLe"): `EM = 00 01 FF..FF 00 || T || H` (RFC 8017
  EMSA-PKCS1-v1_5) stored little-endian (guest byte `i` = `EM[cb-1-i]`). `T`
  by type: 0 none (bare hash), 1 SHA-1 DigestInfo without NULL parameters
  (`30 1F 30 07 06 05 2B 0E 03 02 1A 04 14`), 2 with NULL parameters
  (`30 21 30 09 06 05 2B 0E 03 02 1A 05 00 04 14`). The type numbering is not
  confirmed by a primary source: R-comp's reading is that type 0 is the bare
  form, consistent with Halo 3 using type 0 with a zero hash to wrap a 16-byte
  key for RSA transport (0x822FEFEC). Verify accepts all three forms, so it does
  not depend on that numbering.

## ABI evidence

Halo 3 (4D5307E6, `isos/Halo 3.iso`, recompiled locally with the pinned
XenonRecomp v19 tools; build output stays under `build/`):

- `XeCryptAesKey(r3 state, r4 key)` 0x821A7E94; `XeCryptAesCbc(r3 state, r4 in,
  r5 cb = 4096, r6 out = in, r7 feed, r8 encrypt)` 0x821A8050.
- `XeCryptRandom(r3 buffer, r4 16)` 0x822FEC90.
- Signature check 0x8219F244..0x8219F27C: `SwapDwQwLeBe(sig, sig, 32)`,
  `RsaPubCrypt(sig, sig, key)`, `SwapDwQwLeBe(sig, sig, 32)`,
  `Pkcs1Verify(hash, sig, 256)`.
- Key transport 0x822FEFEC..0x822FF024: `Pkcs1Format(zero hash, 0, block, 256)`,
  16 key bytes copied over the low end, swap, `RsaPubCrypt` with a title key.

Generator note for PRIME: the Halo 3 recompile needed `savefpr_14_address =
0x82593D10` / `restfpr_14_address = 0x82593D5C` (the CRT uses `stfd f14,
-0x90(r12)`, which `tools/m6_inventory.py`'s r1 pattern does not find) and two
invalid switch tables dropped (0x824F2F50, 0x824A1E20). Scratch only; no tool
was changed.

## Host evidence

`runtime/tests/test_crypto_more.cpp`: FIPS-197 A.1 key schedule (round 1 and 10
keys) and C.1 block, NIST SP 800-38A F.2.1/F.2.2 CBC encrypt/decrypt (chained,
in place and out of place), random bytes, qword swap, RSA against independent
Python big-integer oracles (`tests/crypto_rsa_oracle.py` ->
`tests/crypto_rsa_vectors.inc`: 1024-bit e=65537 and e=3), the out-of-domain
FALSE, all three PKCS#1 forms at 39/128/256/512 bytes with tamper rejection,
and Halo 3's full verification sequence on an independently made 2048-bit
signature of SHA-1("abc") (and a forged one). PS5: NOT TESTED.
