#include <compiler.h>
#include <pccore.h>

#include <taskmng_control.h>

int main(void)
{
	np2_host_taskmng_reset();
	pccore_init();
	pccore_term();
	return 0;
}
