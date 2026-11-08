/* cdiNet - the screen: plane A, CLUT7, 384 pixels a line; the pointer as the MCD212's
 * hardware cursor */
#ifndef CN_VIDEO_H
#define CN_VIDEO_H

#define SCREEN_WIDTH  384
#define VBUFFER_SIZE  108544     /* 282 lines: the whole of PAL, and NTSC's 20-line offset */
#define SIG_BLANK     0x0100     /* sent by the line control table at the top of each field */

/* palette (CLUT entries) */
#define C_PAPER   0
#define C_INK     1
#define C_LINK    2
#define C_HEAD    3
#define C_QUOTE   4
#define C_BAR     5
#define C_BARTEXT 6
#define C_HOVER   7
#define C_BUTTON  8
#define C_STATUS  9
#define C_PICTURE 16             /* pictures use entries 16-63 */

extern int videoPath;
extern unsigned char *screen;    /* the first visible line */
extern int screenHeight;         /* visible lines: 280 (PAL) or 240 (NTSC) */
extern unsigned int frameTick;   /* fields shown so far */

void initVideo();
void handleVideoSignal();
void waitField();               /* sleep until the next field (or two ticks) */
void setPalette();               /* first CLUT entry, count, RGB triplets */
void cursorTo();                 /* x 0-767, y 0-559, as the pointer gives them */

#endif
