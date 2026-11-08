/* cdiNet - serial echo test.
 *
 * Opens the player's serial port (/t2, the SCC68070's UART through the BIOS's u68070
 * driver), turns off everything SCF does to a terminal line (echo, line editing, pause,
 * the ^C / ^E signals, XON/XOFF), says hello, then sends back every byte it receives,
 * letters in upper case. Nothing on screen: the player shell stays up. */
#include <stdio.h>
#include <sgstat.h>
#include <sysio.h>

/* "\n" is CR (0x0D) in Microware C: the line end is spelled out */
static char hello[] = "cdiNet serecho ready\x0d\x0a";

int main(argc, argv)
	int argc;
	char **argv;
{
	struct sgbuf opt;
	char buf[64];
	int path, n, i;

	path = open("/t2", UPDAT_);
	if (path < 0)
		return 1;

	_gs_opt(path, &opt);
	opt.sg_echo = 0;
	opt.sg_alf = 0;
	opt.sg_pause = 0;
	opt.sg_backsp = 0;
	opt.sg_bspch = 0;
	opt.sg_dlnch = 0;
	opt.sg_eorch = 0;
	opt.sg_eofch = 0;
	opt.sg_rlnch = 0;
	opt.sg_dulnch = 0;
	opt.sg_psch = 0;
	opt.sg_kbich = 0;
	opt.sg_kbach = 0;
	opt.sg_xon = 0;
	opt.sg_xoff = 0;
	_ss_opt(path, &opt);

	write(path, hello, sizeof(hello) - 1);
	for (;;) {
		n = _gs_rdy(path);
		if (n <= 0)
			n = 1;              /* nothing waiting: sleep in read until a byte comes */
		if (n > sizeof(buf))
			n = sizeof(buf);
		n = read(path, buf, n);
		if (n <= 0)
			continue;
		for (i = 0; i < n; i++)
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] -= 'a' - 'A';
		write(path, buf, n);
	}
}
