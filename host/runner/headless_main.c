#include <compiler.h>
#include <dosio.h>
#include <pccore.h>

#include <taskmng_control.h>

static void configure_stage1_machine(void)
{
	static const UINT8 stage1_dipsw[3] = {0x3e, 0xe3, 0x7b};
	static const UINT8 stage1_memsw[8] = {0x48, 0x05, 0x04, 0x08, 0x01, 0x00, 0x00, 0x6e};
	static const UINT8 stage1_wait[6] = {1, 1, 6, 1, 8, 1};
	unsigned i;

	file_cpyname(np2cfg.model, OEMTEXT("VX"), NELEMENTS(np2cfg.model));
	np2cfg.baseclock = PCBASECLOCK25;
	np2cfg.multiple = 20;
	for (i = 0; i < 3; ++i) {
		np2cfg.dipsw[i] = stage1_dipsw[i];
	}
	for (i = 0; i < 8; ++i) {
		np2cfg.memsw[i] = stage1_memsw[i];
	}
	np2cfg.EXTMEM = 13;
	np2cfg.fddequip = 3;
	np2cfg.memcheckspeed = 8;
	np2cfg.ITF_WORK = 1;
	np2cfg.emuspeed = 100;
	np2cfg.DISPSYNC = 1;
	for (i = 0; i < 6; ++i) {
		np2cfg.wait[i] = stage1_wait[i];
	}

	np2cfg.usebios = 0;
	np2cfg.biospath[0] = '\0';
	np2cfg.fontfile[0] = '\0';
	np2cfg.fontface[0] = '\0';
	for (i = 0; i < 4; ++i) {
		np2cfg.fddfile[i][0] = '\0';
	}
	for (i = 0; i < 2; ++i) {
		np2cfg.sasihdd[i][0] = '\0';
	}
}

int main(void)
{
	configure_stage1_machine();
	np2_host_taskmng_reset();
	pccore_init();
	pccore_reset();
	pccore_term();
	return 0;
}
