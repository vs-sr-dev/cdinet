/* cdiNet - text and boxes (see draw.h). A cell is 6 pixels: at an even x that is three
 * 16-bit writes per glyph row, through a table of the four pixel pairs in the current
 * colours. */
#include "video.h"
#include "draw.h"

extern unsigned char font[224][10];

static unsigned short pairs[4];
static int pairInk = -1, pairPaper = -1;

static void setColours(ink, paper)
	int ink, paper;
{
	if (ink == pairInk && paper == pairPaper)
		return;
	pairInk = ink;
	pairPaper = paper;
	pairs[0] = (paper << 8) | paper;
	pairs[1] = (paper << 8) | ink;
	pairs[2] = (ink << 8) | paper;
	pairs[3] = (ink << 8) | ink;
}

void fillRect(x, y, w, h, colour)
	int x, y, w, h, colour;
{
	register unsigned char *p;
	register unsigned long *l, c4;
	register int i, n;

	c4 = colour & 0xff;
	c4 |= c4 << 8;
	c4 |= c4 << 16;
	for (; h > 0; h--, y++) {
		p = screen + y * SCREEN_WIDTH + x;
		n = w;
		for (; n > 0 && ((long)p & 3); n--)       /* bytes up to a long boundary, */
			*p++ = colour;
		l = (unsigned long *)p;
		for (i = n >> 2; i > 0; i--)            /* then longs, */
			*l++ = c4;
		p = (unsigned char *)l;
		for (n &= 3; n > 0; n--)                /* then what is left */
			*p++ = colour;
	}
}

/* x must be even */
void drawChar(x, y, c, ink, paper, attr)
	int x, y, c, ink, paper, attr;
{
	register unsigned short *p;
	register unsigned char *g;
	register int b, r;

	c &= 0xff;
	if (c < 0x20)
		c = '?';
	setColours(ink, paper);
	g = font[c - 0x20];
	p = (unsigned short *)(screen + y * SCREEN_WIDTH + x);
	for (r = 0; r < CELL_H; r++) {
		b = g[r];
		if (attr & A_BOLD)
			b |= b >> 1;
		if (r == CELL_H - 1 && (attr & A_UNDERLINE))
			b = 0xfc;
		p[0] = pairs[(b >> 6) & 3];
		p[1] = pairs[(b >> 4) & 3];
		p[2] = pairs[(b >> 2) & 3];
		p += SCREEN_WIDTH / 2;
	}
}

int drawText(x, y, s, ink, paper, attr, n)
	int x, y;
	char *s;
	int ink, paper, attr, n;
{
	while (*s && n-- > 0) {
		drawChar(x, y, *s++, ink, paper, attr);
		x += CELL_W;
	}
	return x;
}
