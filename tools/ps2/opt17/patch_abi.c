#include "doomdef.h"
#include "r_patch.h"
#include <stddef.h>

_Static_assert(sizeof(void *) == 4, "EE pointer size");
_Static_assert(sizeof(patch_t) == 48, "audit patch size");
_Static_assert(sizeof(column_t) == 12, "audit column size");
_Static_assert(sizeof(post_t) == 12, "audit post size");
_Static_assert(_Alignof(column_t) == 4, "audit column alignment");
_Static_assert(_Alignof(post_t) == 4, "audit post alignment");
const unsigned opt17_patch_abi[] __attribute__((section(".opt17_patch_abi"),used)) = {
    sizeof(patch_t), sizeof(column_t), sizeof(post_t),
    _Alignof(column_t), _Alignof(post_t),
    offsetof(patch_t, pixels), offsetof(patch_t, columns), offsetof(patch_t, posts),
    offsetof(post_t, topdelta), offsetof(post_t, length), offsetof(post_t, data_offset)
};
