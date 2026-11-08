/* cdiNet - the screen (see video.h). The plane set-up follows our hello world, which
 * followed TwBurn's Nobelia: plane A in CLUT7 from one buffer, plane B present but
 * contributing nothing. */
#include <strings.h>
#include <csd.h>
#include <sysio.h>
#include <ucm.h>
#include <stdio.h>
#include <memory.h>
#include "video.h"

#define FCT_SIZE 160
#define LCT_SIZE (280 * 2 + 2)

int videoPath;
unsigned char *screen;
int screenHeight;
unsigned int frameTick = 0;

static int fctA, lctA, fctB, lctB;
static u_int fctBuffer[FCT_SIZE];
static unsigned char *videoBuffer;
static int fctUsed;              /* plane A commands before any picture palette */

/* an arrow, 11 pixels wide; in normal resolution each bit is two of the cursor's pixels */
static unsigned short arrow[16] = {
	0x8000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00,
	0xf800, 0xd800, 0x8c00, 0x0c00, 0x0600, 0x0600, 0x0000, 0x0000
};

static void setupPlaneA()
{
	int n;

	fctA = dc_crfct(videoPath, PA, FCT_SIZE, 0);
	lctA = dc_crlct(videoPath, PA, LCT_SIZE, 0);
	dc_nop(videoPath, lctA, 0, 0, LCT_SIZE, 8);
	dc_flnk(videoPath, fctA, lctA, 0);

	fctBuffer[0]  = cp_icm(ICM_CLUT7, ICM_CLUT7, NM_1, EV_OFF, CS_A);
	fctBuffer[1]  = cp_tci(MIX_OFF, TR_CKEY_T, TR_CKEY_T);
	fctBuffer[2]  = cp_po(PR_AB);
	fctBuffer[3]  = cp_bkcol(BK_BLACK, BK_LOW);
	fctBuffer[4]  = cp_tcol(PA, 0, 0, 0);
	fctBuffer[5]  = cp_mcol(PA, 0, 0, 0);
	fctBuffer[6]  = cp_yuv(PA, 16, 128, 128);
	fctBuffer[7]  = cp_phld(PA, PH_OFF, 1);
	fctBuffer[8]  = cp_icf(PA, ICF_MAX);
	fctBuffer[9]  = cp_matte(0, MO_END, MF_MF0, ICF_MAX, 0);
	fctBuffer[10] = cp_dprm(RMS_NORMAL, PRF_X2, BP_NORMAL);
	n = 11;
	fctBuffer[n++] = cp_clut(C_PAPER,   236, 232, 220);
	fctBuffer[n++] = cp_clut(C_INK,      24,  24,  32);
	fctBuffer[n++] = cp_clut(C_LINK,     20,  60, 180);
	fctBuffer[n++] = cp_clut(C_HEAD,    140,  20,  36);
	fctBuffer[n++] = cp_clut(C_QUOTE,    28, 104,  48);
	fctBuffer[n++] = cp_clut(C_BAR,      24,  44,  92);
	fctBuffer[n++] = cp_clut(C_BARTEXT, 236, 236, 236);
	fctBuffer[n++] = cp_clut(C_HOVER,   252, 216,  92);
	fctBuffer[n++] = cp_clut(C_BUTTON,   72, 100, 164);
	fctBuffer[n++] = cp_clut(C_STATUS,  252, 196,  80);
	dc_wrfct(videoPath, fctA, 0, n, fctBuffer);
	fctUsed = n;
}

/* CLUT entries first.. (at most 63: bank 0) from R, G, B triplets, as more commands at
 * the end of plane A's field control table: they take effect from the next field */
void setPalette(first, n, rgb)
	int first, n;
	unsigned char *rgb;
{
	int i;

	if (first + n > 64)
		n = 64 - first;
	for (i = 0; i < n; i++, rgb += 3)
		fctBuffer[i] = cp_clut(first + i, rgb[0], rgb[1], rgb[2]);
	dc_wrfct(videoPath, fctA, fctUsed, n, fctBuffer);
}

static void setupPlaneB()
{
	fctB = dc_crfct(videoPath, PB, FCT_SIZE, 0);
	lctB = dc_crlct(videoPath, PB, LCT_SIZE, 0);
	dc_nop(videoPath, lctB, 0, 0, LCT_SIZE, 8);
	dc_flnk(videoPath, fctB, lctB, 0);

	fctBuffer[0]  = cp_nop();
	fctBuffer[1]  = cp_nop();
	fctBuffer[2]  = cp_nop();
	fctBuffer[3]  = cp_nop();
	fctBuffer[4]  = cp_tcol(PB, 0, 0, 0);
	fctBuffer[5]  = cp_mcol(PB, 0, 0, 0);
	fctBuffer[6]  = cp_yuv(PB, 16, 128, 128);
	fctBuffer[7]  = cp_phld(PB, PH_OFF, 1);
	fctBuffer[8]  = cp_icf(PB, ICF_MIN);
	fctBuffer[9]  = cp_nop();
	fctBuffer[10] = cp_dprm(RMS_NORMAL, PRF_X2, BP_NORMAL);
	dc_wrfct(videoPath, fctB, 0, 11, fctBuffer);
}

void initVideo()
{
	char *devName, *devParam;
	int pixelStart;

	devName = csd_devname(DT_VIDEO, 1);
	videoPath = open(devName, UPDAT_);
	devParam = csd_devparam(devName);
	if (findstr(1, devParam, "LI=\"625\":")) {
		dc_setcmp(videoPath, 0);          /* PAL: 280 lines */
		pixelStart = 0;
		screenHeight = 280;
	} else {
		dc_setcmp(videoPath, findstr(1, devParam, "TV") ? 0 : 1);
		pixelStart = 20 * SCREEN_WIDTH;   /* NTSC: 240 lines, 20 down */
		screenHeight = 240;
	}
	free(devName);
	free(devParam);

	dc_intl(videoPath, 0);
	setupPlaneA();
	setupPlaneB();

	videoBuffer = (unsigned char *)srqcmem(VBUFFER_SIZE, VIDEO1);
	screen = videoBuffer + pixelStart;
	memset(videoBuffer, C_PAPER, VBUFFER_SIZE);

	dc_wrli(videoPath, lctA, 0, 1, cp_dadr((int)videoBuffer + pixelStart));
	dc_wrli(videoPath, lctA, 0, 0, cp_sig());
	dc_exec(videoPath, fctA, fctB);

	gc_ptn(videoPath, 0, 0, 16, 16, 0, arrow);
	gc_col(videoPath, 0x0f);
	gc_pos(videoPath, 384, 280);
	gc_show(videoPath);
}

void cursorTo(x, y)
	int x, y;
{
	gc_pos(videoPath, x, y);
}

/* SIG_BLANK is a one-shot: armed again after each field. tsleep rather than sleep(0),
 * so that a field signalled between arming and sleeping cannot leave us asleep. */
static int armed;

void waitField()
{
	if (!armed) {
		armed = 1;
		dc_ssig(videoPath, SIG_BLANK, 0);
	}
	tsleep(2);
}

void handleVideoSignal(sigCode)
	int sigCode;
{
	if (sigCode == SIG_BLANK) {
		frameTick++;
		armed = 0;
	}
}
