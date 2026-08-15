#include <compiler.h>
#include <sysmng.h>

int main(void) {
	sysmng_update(SYS_UPDATECFG);
	sysmng_update(SYS_UPDATEFDD);
	sysmng_update(SYS_UPDATEHDD);
	sysmng_update(SYS_UPDATEHDD | SYS_UPDATECFG);
	sysmng_cpureset();

	sysmng_fddaccess(0);
	sysmng_hddaccess(0);
	return 0;
}
