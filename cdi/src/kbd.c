/* cdiNet - the on-screen keyboard (see kbd.h).
 *
 * Keys are boxes 26 x 20 pixels on a 28 x 22 pitch, twelve to a row; wider keys take
 * several of those units. Everything stays on the CD-i until Go: then main.c sends the
 * text to the proxy, which decides whether it is an address or a search. */
#include <strings.h>
#include "video.h"
#include "draw.h"
#include "kbd.h"

#define UNIT_W  28
#define UNIT_H  22
#define KEY_H   20
#define NKEYS   64

/* special keys */
#define K_SHIFT  -1
#define K_SPACE  -2
#define K_COM    -3
#define K_DEL    -4
#define K_CLEAR  -5
#define K_CANCEL -6
#define K_GO     -7

char kbdText[KBD_TEXT_MAX + 1];

static char *charRows[4] = {
	"1234567890-=",
	"qwertyuiop/_",
	"asdfghjkl:.'",
	"zxcvbnm,?&@!"
};
static struct { char *label; int code, units; } specials[] = {
	{ "Shift", K_SHIFT, 2 }, { "Space", K_SPACE, 4 }, { ".com", K_COM, 2 },
	{ "Del", K_DEL, 2 }, { "Clear", K_CLEAR, 2 },
	{ "Cancel", K_CANCEL, 6 }, { "Go", K_GO, 6 }
};
#define NSPECIALS 7

static struct key { short x, y, w, code; char label[8]; } keys[NKEYS];
static int nkeys;
static int shift;
static int hoverKey = -1;
static int fieldX, fieldY;
static int areaX, areaY, areaW, areaH;

static void drawKey(k)
	int k;
{
	register struct key *p = &keys[k];
	int paper = k == hoverKey ? C_HOVER : C_BUTTON;
	int ink = k == hoverKey ? C_INK : C_BARTEXT;
	int len = strlen(p->label);
	int tx = p->x + ((p->w - len * CELL_W) / 2 & ~1);

	fillRect(p->x, p->y, p->w, KEY_H, paper);
	drawText(tx, p->y + 5, p->label, ink, paper, A_BOLD, 8);
}

static void drawField()
{
	int len = strlen(kbdText), shown = len > 57 ? 57 : len;
	int x;

	fillRect(areaX, fieldY - 2, areaW, CELL_H + 4, C_BAR);
	x = drawText(fieldX, fieldY, kbdText + len - shown, C_BARTEXT, C_BAR, 0, 57);
	drawChar(x, fieldY, '_', C_STATUS, C_BAR, A_BOLD);
}

/* the letter keys' labels follow Shift */
static void label(k)
	int k;
{
	struct key *p = &keys[k];

	if (p->code > 0) {
		p->label[0] = (shift && p->code >= 'a' && p->code <= 'z') ? p->code - 'a' + 'A' : p->code;
		p->label[1] = 0;
	}
}

void kbdOpen(x, y, w, h)
	int x, y, w, h;
{
	int r, i, kx, ky, x0;
	char *s;

	areaX = x;
	areaY = y;
	areaW = w;
	areaH = h;
	fillRect(x, y, w, h, C_PAPER);
	drawText(x, y + 2, "Type an address, or words to search:", C_HEAD, C_PAPER, A_BOLD, 60);
	fieldX = x + 4;
	fieldY = y + 18;

	x0 = (x + (w - 12 * UNIT_W) / 2) & ~1;
	ky = fieldY + CELL_H + 8;
	nkeys = 0;
	for (r = 0; r < 4; r++, ky += UNIT_H)
		for (s = charRows[r], kx = x0; *s; s++, kx += UNIT_W) {
			keys[nkeys].x = kx;
			keys[nkeys].y = ky;
			keys[nkeys].w = UNIT_W - 2;
			keys[nkeys].code = *s;
			label(nkeys);
			nkeys++;
		}
	for (i = 0, kx = x0; i < NSPECIALS; i++) {
		if (i == 5) {           /* Cancel and Go: a row of their own */
			kx = x0;
			ky += UNIT_H;
		}
		keys[nkeys].x = kx;
		keys[nkeys].y = ky;
		keys[nkeys].w = specials[i].units * UNIT_W - 2;
		keys[nkeys].code = specials[i].code;
		strcpy(keys[nkeys].label, specials[i].label);
		nkeys++;
		kx += specials[i].units * UNIT_W;
	}
	kbdText[0] = 0;
	shift = 0;
	hoverKey = -1;
	drawField();
	for (i = 0; i < nkeys; i++)
		drawKey(i);
}

static int keyAt(x, y)
	int x, y;
{
	int i;

	for (i = 0; i < nkeys; i++)
		if (x >= keys[i].x && x < keys[i].x + keys[i].w && y >= keys[i].y && y < keys[i].y + KEY_H)
			return i;
	return -1;
}

static void type(s)
	char *s;
{
	int len = strlen(kbdText);

	while (*s && len < KBD_TEXT_MAX)
		kbdText[len++] = *s++;
	kbdText[len] = 0;
}

int kbdPointer(x, y, pressed)
	int x, y, pressed;
{
	int k = keyAt(x, y), old = hoverKey, i, len;
	char c[2];

	if (k != hoverKey) {
		hoverKey = k;
		if (old >= 0)
			drawKey(old);
		if (k >= 0)
			drawKey(k);
	}
	if (pressed & 2)
		return KBD_CANCEL;
	if (!(pressed & 1) || k < 0)
		return KBD_NONE;

	switch (keys[k].code) {
	case K_SHIFT:
		shift = !shift;
		for (i = 0; i < nkeys; i++)
			if (keys[i].code >= 'a' && keys[i].code <= 'z') {
				label(i);
				drawKey(i);
			}
		return KBD_NONE;
	case K_SPACE:
		type(" ");
		break;
	case K_COM:
		type(".com");
		break;
	case K_DEL:
		len = strlen(kbdText);
		if (len)
			kbdText[len - 1] = 0;
		break;
	case K_CLEAR:
		kbdText[0] = 0;
		break;
	case K_CANCEL:
		return KBD_CANCEL;
	case K_GO:
		return KBD_GO;
	default:
		c[0] = keys[k].label[0];
		c[1] = 0;
		type(c);
		break;
	}
	drawField();
	return KBD_NONE;
}
