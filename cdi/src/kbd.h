/* cdiNet - the on-screen keyboard: an address or words to search, typed with the pointer */
#ifndef CN_KBD_H
#define CN_KBD_H

#define KBD_TEXT_MAX 96

#define KBD_NONE   0
#define KBD_GO     1
#define KBD_CANCEL 2

extern char kbdText[KBD_TEXT_MAX + 1];

void kbdOpen();         /* x, y, w, h: the area it covers (the page) */
int  kbdPointer();      /* x, y in screen pixels, newly pressed buttons: KBD_NONE, _GO or _CANCEL */

#endif
