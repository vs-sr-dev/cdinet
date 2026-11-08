/* cdiNet - a web browser for the Philips CD-i, through a proxy on the serial line.
 *
 * The proxy (proxy/cdinet_proxy.py) fetches and simplifies the pages and sends them a
 * screen at a time: rows of ISO 8859-1 text with links and styles marked
 * (docs/protocol.md). This program draws them, follows the pointer, lights the link
 * under it, and asks for what the user clicks.
 *
 * Screen (visible lines; 384 pixels wide, 60 columns in a 12-pixel safe margin):
 *   8 lines margin, the title bar (12), the page (rows of 10), the button bar (12). */
#include <stdio.h>
#include <csd.h>
#include <sysio.h>
#include <strings.h>
#include "video.h"
#include "draw.h"
#include "link.h"
#include "kbd.h"
#include "picture.h"

#define MARGIN_X  12
#define COLS      60
#define TITLE_Y   8
#define BAR_H     12
#define PAGE_Y    (TITLE_Y + BAR_H + 2)
#define MAX_ROWS  25
#define MAX_SPANS 200
#define SCREEN_MAX 4096

/* the button bar: label, first column */
#define B_BACK 0
#define B_HOME 1
#define B_PREV 2
#define B_NEXT 3
#define B_GO   4
#define NBUTTONS 5
static char *buttonLabel[NBUTTONS] = { "Back", "Home", "Prev", "Next", " Go" };
static int buttonCol[NBUTTONS] = { 0, 6, 12, 18, 24 };
#define BUTTON_W 5
#define STATUS_COL 31

/* states of the conversation with the proxy */
#define S_HELLO   0             /* H sent */
#define S_NAV     1             /* G, L, B or P sent: W, C or E to come */
#define S_COLLECT 2             /* N sent for the rest of a screen */
#define S_IDLE    3
#define S_WAIT    4             /* the proxy said W: ask again soon */
#define S_DEAD    5             /* the proxy does not answer */
#define S_KBD     6             /* the keyboard is up */

static int state;
static unsigned int waitUntil;
static int rows;                /* page rows on this screen (PAL 23, NTSC 19) */
static int barY;

/* the screen being shown, as the proxy sent it */
static unsigned char blob[SCREEN_MAX];
static int blobTotal, blobGot, blobId;
static int blobComplete;        /* blob holds a whole screen (it can be drawn again) */
static int page, pages;
static unsigned char *rowPtr[MAX_ROWS];
static int nrows;
static char title[64];

/* where the links are on the screen */
static struct span { char row, c0, c1; unsigned short id; } spans[MAX_SPANS];
static int nspans;
static int hoverId;

static int ptrPath = -1;
static int lastButtons;
static int lastX = -1, lastY = -1;

int intHandler(sigCode)
	int sigCode;
{
	handleVideoSignal(sigCode);
}

/* ---- bars ---- */

static void status(s)
	char *s;
{
	int x = MARGIN_X + STATUS_COL * CELL_W;
	fillRect(x, barY, SCREEN_WIDTH - x, BAR_H, C_BAR);
	drawText(x, barY + 1, s, C_STATUS, C_BAR, 0, COLS - STATUS_COL);
}

static void drawButtons()
{
	int i, x;

	fillRect(0, barY, SCREEN_WIDTH, BAR_H, C_BAR);
	for (i = 0; i < NBUTTONS; i++) {
		x = MARGIN_X + buttonCol[i] * CELL_W;
		fillRect(x, barY, BUTTON_W * CELL_W, BAR_H, C_BUTTON);
		drawText(x + 2, barY + 1, buttonLabel[i], C_BARTEXT, C_BUTTON, A_BOLD, BUTTON_W);
	}
}

static void drawTitle(s)
	char *s;
{
	fillRect(0, TITLE_Y, SCREEN_WIDTH, BAR_H, C_BAR);
	drawText(MARGIN_X, TITLE_Y + 1, s, C_BARTEXT, C_BAR, A_BOLD, COLS);
}

static void pageStatus()
{
	char s[40];

	if (pages > 1)
		sprintf(s, "screen %d of %d", page + 1, pages);
	else
		s[0] = 0;
	status(s);
}

/* ---- the page ---- */

/* draw one row; with collect, note where its links are */
static void drawRow(r, collect)
	int r, collect;
{
	register unsigned char *p;
	unsigned char *end;
	int col = 0, style = 0, link = 0, ink, paper, attr, start = 0, y, x;

	y = PAGE_Y + r * CELL_H;
	x = MARGIN_X;
	if (r < nrows) {
		p = rowPtr[r];
		end = p + 1 + *p;
		p++;
		while (p < end && col < COLS) {
			if (*p == 1) {
				link = (p[1] << 8) | p[2];
				start = col;
				p += 3;
				continue;
			}
			if (*p == 2) {
				if (collect && link && nspans < MAX_SPANS) {
					spans[nspans].row = r;
					spans[nspans].c0 = start;
					spans[nspans].c1 = col;
					spans[nspans].id = link;
					nspans++;
				}
				link = 0;
				p++;
				continue;
			}
			if (*p == 3) {
				style = p[1];
				p += 2;
				continue;
			}
			if (link) {
				ink = C_LINK;
				attr = A_UNDERLINE;
			} else {
				ink = (style & 2) ? C_HEAD : (style & 4) ? C_QUOTE : C_INK;
				attr = 0;
			}
			if (style & 1)
				attr |= A_BOLD;
			paper = (link && link == hoverId) ? C_HOVER : C_PAPER;
			drawChar(x, y, *p++, ink, paper, attr);
			x += CELL_W;
			col++;
		}
	}
	if (x < MARGIN_X + COLS * CELL_W)
		fillRect(x, y, MARGIN_X + COLS * CELL_W - x, CELL_H, C_PAPER);
}

/* the screen's header: page, pages, title; the offset of its row count (the URL is
 * skipped), or -1 while not all of it has come */
static int parseHeader()
{
	int p = 4, n;

	if (blobGot <= p)
		return -1;
	p += 1 + blob[p];
	if (blobGot <= p)
		return -1;
	p += 1 + blob[p];
	if (blobGot <= p)
		return -1;
	page = (blob[0] << 8) | blob[1];
	pages = (blob[2] << 8) | blob[3];
	n = blob[4];
	if (n > sizeof(title) - 1)
		n = sizeof(title) - 1;
	strncpy(title, (char *)blob + 5, n);
	title[n] = 0;
	return p;
}

/* a text screen has come: take it apart and draw it */
static void showScreen()
{
	unsigned char *p, *end = blob + blobTotal;
	int r;

	p = blob + parseHeader();
	nrows = *p++;
	if (nrows > rows)
		nrows = rows;
	for (r = 0; r < nrows && p < end; r++) {
		rowPtr[r] = p;
		p += 1 + *p;
	}
	nrows = r;
	blobComplete = 1;

	hoverId = 0;
	nspans = 0;
	drawTitle(title[0] ? title : "cdiNet");
	for (r = 0; r < rows; r++)
		drawRow(r, 1);
	pageStatus();
}

/* ---- pictures ---- */

static int picture;             /* the screen coming (or shown) is a picture */
static int pictureAt;           /* where its LZSS data starts in the screen */
static int pictureW, pictureH;
static int chunks;              /* received for this screen: the progress shows every 8 */

/* the screen is a picture once its header says so (row count FF) and its palette has
 * come: clear the page, load the palette, start decoding what is already here */
static void checkPicture()
{
	int r = parseHeader(), n, k;

	if (r < 0 || blob[r] != 0xff || blobGot < r + 6)
		return;
	n = blob[r + 5];
	if (blobGot < r + 6 + 3 * n)
		return;
	pictureW = (blob[r + 1] << 8) | blob[r + 2];
	pictureH = (blob[r + 3] << 8) | blob[r + 4];
	if (pictureW > COLS * CELL_W)
		pictureW = COLS * CELL_W;
	if (pictureH > rows * CELL_H)
		pictureH = rows * CELL_H;
	pictureAt = r + 6 + 3 * n;
	picture = 1;
	nspans = 0;
	hoverId = 0;
	drawTitle(title[0] ? title : "picture");
	fillRect(MARGIN_X, PAGE_Y, COLS * CELL_W, rows * CELL_H, C_PAPER);
	setPalette(C_PICTURE, n, blob + r + 6);
	pictureStart(MARGIN_X + (COLS * CELL_W - pictureW) / 2, PAGE_Y + (rows * CELL_H - pictureH) / 2,
		pictureW, (blob[r + 3] << 8) | blob[r + 4]);
	for (k = pictureAt; k < blobGot && k < SCREEN_MAX; k++)
		pictureByte(blob[k]);
}

/* the page again, after the keyboard covered it (a picture is asked for again) */
static void redrawPage()
{
	unsigned char a[2];

	if (picture) {
		a[0] = a[1] = 0;
		linkRequest('P', a, 2);
		state = S_NAV;
		status("the picture again");
	} else if (blobComplete)
		showScreen();
	else
		fillRect(MARGIN_X, PAGE_Y, COLS * CELL_W, rows * CELL_H, C_PAPER);
}

/* light the link under the pointer (id 0: none) */
static void hover(id)
	int id;
{
	int i, old = hoverId, lastRow = -1;

	if (id == hoverId)
		return;
	hoverId = id;
	for (i = 0; i < nspans; i++)
		if ((spans[i].id == old || spans[i].id == id) && spans[i].row != lastRow) {
			drawRow(spans[i].row, 0);
			lastRow = spans[i].row;
		}
}

static int linkAt(x, y)
	int x, y;
{
	int r, c, i;

	if (y < PAGE_Y || x < MARGIN_X)
		return 0;
	r = (y - PAGE_Y) / CELL_H;
	c = (x - MARGIN_X) / CELL_W;
	for (i = 0; i < nspans; i++)
		if (spans[i].row == r && c >= spans[i].c0 && c < spans[i].c1)
			return spans[i].id;
	return 0;
}

static int buttonAt(x, y)
	int x, y;
{
	int i, c;

	if (y < barY || y >= barY + BAR_H || x < MARGIN_X)
		return -1;
	c = (x - MARGIN_X) / CELL_W;
	for (i = 0; i < NBUTTONS; i++)
		if (c >= buttonCol[i] && c < buttonCol[i] + BUTTON_W)
			return i;
	return -1;
}

/* ---- talking to the proxy ---- */

static void navigate(type, args, n, what)
	int type;
	unsigned char *args;
	int n;
	char *what;
{
	linkRequest(type, args, n);
	state = S_NAV;
	status(what);
}

static void askChunk(off)
	int off;
{
	unsigned char a[2];

	a[0] = off >> 8;
	a[1] = off;
	linkRequest('N', a, 2);
}

static void showError()
{
	char s[COLS + 1];
	int n = replyLen < COLS - STATUS_COL ? replyLen : COLS - STATUS_COL;

	strncpy(s, (char *)replyData, n);
	s[n] = 0;
	status(s);
	state = S_IDLE;
}

/* a chunk of a screen: text is kept whole (up to SCREEN_MAX), a picture goes to the
 * decoder as it comes */
static void chunk()
{
	int sid = replyData[0];
	int off = (replyData[1] << 8) | replyData[2];
	int total = (replyData[3] << 8) | replyData[4];
	int n = replyLen - 5, i, pos;
	char s[40];

	if (off == 0) {
		blobComplete = 0;
		picture = 0;
		chunks = 0;
		blobId = sid;
		blobTotal = total;
		blobGot = 0;
	}
	if (sid != blobId || off != blobGot) {
		askChunk(blobGot);      /* not the one asked for: ask again */
		state = S_COLLECT;
		return;
	}
	if (off + n > blobTotal)
		n = blobTotal - off;
	/* ask for the next chunk before handling this one: the reply comes while we decode
	 * and draw (one reply at most, 106 bytes, always fits the driver's 128) */
	if (off + n < blobTotal) {
		askChunk(off + n);
		state = S_COLLECT;
	}
	for (i = 0; i < n; i++) {
		pos = off + i;
		if (picture && pos >= pictureAt)
			pictureByte(replyData[5 + i]);
		else if (pos < SCREEN_MAX)
			blob[pos] = replyData[5 + i];
	}
	blobGot = off + n;
	if (!picture)
		checkPicture();
	if (blobGot < blobTotal) {
		if ((++chunks & 7) == 1) {
			if (picture)
				sprintf(s, "picture %dx%d: %d%%", pictureW, pictureH, blobGot * 100 / blobTotal);
			else
				sprintf(s, "receiving %d%%", blobGot * 100 / blobTotal);
			status(s);
		}
		return;
	}
	if (picture) {
		sprintf(s, "picture %dx%d", pictureW, pictureH);
		status(s);
	} else if (blobTotal <= SCREEN_MAX)
		showScreen();
	else
		status("screen too big");
	state = S_IDLE;
}

static void reply()
{
	switch (state) {
	case S_HELLO:
		navigate('G', (unsigned char *)"", 0, "loading the home page");
		return;
	case S_NAV:
	case S_COLLECT:
		if (replyType == 'W') {
			state = S_WAIT;
			waitUntil = frameTick + 25;
			if (state == S_WAIT)
				status("loading...");
		} else if (replyType == 'C')
			chunk();
		else if (replyType == 'E')
			showError();
		else
			state = S_IDLE;
		return;
	}
}

/* ---- the pointer ---- */

static void pointer()
{
	int buttons, x, y, sx, sy, id, b;
	unsigned char a[2];

	if (ptrPath < 0)
		return;
	pt_coord(ptrPath, &buttons, &x, &y);
	if (x != lastX || y != lastY) {
		cursorTo(x, y);
		lastX = x;
		lastY = y;
	}
	sx = x / 2;                 /* the pointer moves in 768 x 560 */
	sy = y / 2;
	if (state == S_IDLE)
		hover(linkAt(sx, sy));
	b = buttons & ~lastButtons;
	lastButtons = buttons;
	if (state == S_KBD) {
		switch (kbdPointer(sx, sy, b)) {
		case KBD_CANCEL:
			state = S_IDLE;
			redrawPage();           /* a picture: asked for again */
			break;
		case KBD_GO:
			state = S_IDLE;
			if (!kbdText[0]) {
				redrawPage();
				break;
			}
			if (picture)
				fillRect(MARGIN_X, PAGE_Y, COLS * CELL_W, rows * CELL_H, C_PAPER);
			else
				redrawPage();
			navigate('G', (unsigned char *)kbdText, strlen(kbdText), "looking it up");
			break;
		}
		return;
	}
	if (!b || state != S_IDLE)
		return;
	if (b & 2) {
		navigate('B', a, 0, "going back");
		return;
	}
	if (hoverId) {
		a[0] = hoverId >> 8;
		a[1] = hoverId;
		navigate('L', a, 2, "following the link");
		return;
	}
	switch (buttonAt(sx, sy)) {
	case B_BACK:
		navigate('B', a, 0, "going back");
		break;
	case B_HOME:
		navigate('G', a, 0, "loading the home page");
		break;
	case B_PREV:
		if (page > 0) {
			a[0] = (page - 1) >> 8;
			a[1] = page - 1;
			navigate('P', a, 2, "previous screen");
		}
		break;
	case B_NEXT:
		if (page + 1 < pages) {
			a[0] = (page + 1) >> 8;
			a[1] = page + 1;
			navigate('P', a, 2, "next screen");
		}
		break;
	case B_GO:
		hover(0);
		kbdOpen(MARGIN_X, PAGE_Y, COLS * CELL_W, rows * CELL_H);
		status("button 2: cancel");
		state = S_KBD;
		break;
	}
}

int main(argc, argv)
	int argc;
	char **argv;
{
	char *dev;
	unsigned char hello[3];
	int r;

	intercept(intHandler);
	initVideo();
	rows = (screenHeight - PAGE_Y - 2 - BAR_H - 8) / CELL_H;
	if (rows > MAX_ROWS)
		rows = MAX_ROWS;
	barY = PAGE_Y + rows * CELL_H + 2;

	dev = csd_devname(DT_PTR, 1);
	if (dev) {
		ptrPath = open(dev, UPDAT_);
		free(dev);
	}

	drawTitle("cdiNet");
	drawButtons();
	if (!linkOpen()) {
		status("no serial port");
		for (;;)
			sleep(0);
	}
	hello[0] = 1;
	hello[1] = COLS;
	hello[2] = rows;
	linkRequest('H', hello, 3);
	state = S_HELLO;
	status("calling the proxy...");

	for (;;) {
		waitField();
		r = linkPoll();
		if (r > 0)
			reply();
		else if (r < 0) {
			status("the proxy does not answer");
			state = S_DEAD;
		}
		if (state == S_WAIT && (int)(frameTick - waitUntil) >= 0) {
			askChunk(0);
			state = S_COLLECT;
		}
		if (state == S_DEAD && (frameTick & 255) == 0) {
			linkRequest('H', hello, 3);     /* try again now and then */
			state = S_HELLO;
		}
		pointer();
	}
}
