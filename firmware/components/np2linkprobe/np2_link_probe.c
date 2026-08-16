/* Bounded Phase 2 link probe; no NP2 lifecycle function is called. */
#include <compiler.h>
#include <pccore.h>

void np2_phase2_link_probe(void)
{
	void (*volatile init_anchor)(void) = pccore_init;
	void (*volatile term_anchor)(void) = pccore_term;
	void (*volatile reset_anchor)(void) = pccore_reset;
	void (*volatile exec_anchor)(BOOL) = pccore_exec;

	(void)init_anchor;
	(void)term_anchor;
	(void)reset_anchor;
	(void)exec_anchor;
}
