#include <compiler.h>
#include <soundmng.h>

void soundmng_play(void) {
}

void soundmng_stop(void) {
}

void soundmng_sync(void) {
}

BRESULT soundmng_pcmplay(UINT number, BOOL loop) {
	(void)loop;
	if ((number == SOUND_PCMSEEK) || (number == SOUND_PCMSEEK1)) {
		return SUCCESS;
	}
	return FAILURE;
}

void soundmng_pcmstop(UINT number) {
	(void)number;
}
