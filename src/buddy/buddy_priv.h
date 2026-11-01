/* Buddy internals, shared by buddy.c and face.c. Not part of the module API. */
#ifndef NEXUS_BUDDY_PRIV_H_
#define NEXUS_BUDDY_PRIV_H_

#include "face_anim.h"

/** Eyes, mouth and extras for @p f, clipped to the live band like any op. */
void face_draw(const struct face_frame *f);

#endif /* NEXUS_BUDDY_PRIV_H_ */
