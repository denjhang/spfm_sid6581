/* MinGW shim for wothke/websid.
 *
 * src/base.h does `typedef __uint32_t uint32_t;` outside the EMSCRIPTEN branch.
 * __uint32_t is a glibc internal type, so on MinGW that name does not exist and
 * any use of int32_t/int64_t fails as well (base.h never pulls stdint.h).
 * Including stdint.h first and defining __uint32_t keeps src/* byte-identical
 * to upstream.
 */
#ifndef WEBSID_WIN_COMPAT_H
#define WEBSID_WIN_COMPAT_H
#include <stdint.h>
#ifndef __uint32_t
#define __uint32_t unsigned
#endif
#endif
