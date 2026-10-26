#ifndef __ecc25519_donna_h__
#define __ecc25519_donna_h__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef int64_t felem;

void curve25519_donna(u8 *donna_publickey, const u8 *secret, const u8 *basepoint);

#ifdef __cplusplus
}
#endif
#endif  /* __ecc25519_donna_h__ */
