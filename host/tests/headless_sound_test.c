#include <assert.h>
#include <limits.h>

#include <compiler.h>
#include <soundmng.h>

int main(void) {
	soundmng_play();
	soundmng_stop();
	soundmng_sync();

	assert(soundmng_pcmplay(SOUND_PCMSEEK, FALSE) == SUCCESS);
	assert(soundmng_pcmplay(SOUND_PCMSEEK, TRUE) == SUCCESS);
	assert(soundmng_pcmplay(SOUND_PCMSEEK1, FALSE) == SUCCESS);
	assert(soundmng_pcmplay(SOUND_PCMSEEK1, TRUE) == SUCCESS);

	soundmng_pcmstop(SOUND_PCMSEEK);
	soundmng_pcmstop(SOUND_PCMSEEK1);
	assert(soundmng_pcmplay(UINT_MAX, FALSE) == FAILURE);
	soundmng_pcmstop(UINT_MAX);
	return 0;
}
