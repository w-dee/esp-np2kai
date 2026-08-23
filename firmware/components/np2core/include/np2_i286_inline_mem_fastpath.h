#ifndef NP2_I286_INLINE_MEM_FASTPATH_H
#define NP2_I286_INLINE_MEM_FASTPATH_H

/*
 * The I286/V30 engine shares this access layer.  Keep the switch at compile
 * time so the reference and optimized A/B builds cannot diverge at runtime.
 * The candidate was informed by mochimochi-man/np2_espresso commit
 * 95124685f4518fb59b18864565833de8faba5cf2; this implementation is
 * independently expressed and keeps NP2kai's existing fallback semantics.
 */
#ifndef NP2_I286C_INLINE_MEM_FASTPATH
#define NP2_I286C_INLINE_MEM_FASTPATH 0
#endif

#if (NP2_I286C_INLINE_MEM_FASTPATH != 0) && \
    (NP2_I286C_INLINE_MEM_FASTPATH != 1)
#error "NP2_I286C_INLINE_MEM_FASTPATH must be exactly 0 or 1"
#endif

#if NP2_I286C_INLINE_MEM_FASTPATH

static INLINE __attribute__((always_inline)) REG8
np2_i286_inline_memoryread8(UINT32 address) {
	if (address < I286_MEMREADMAX) {
		return (REG8)mem[address];
	}
	return memp_read8(address);
}

static INLINE __attribute__((always_inline)) REG16
np2_i286_inline_memoryread16(UINT32 address) {
	if (address < (I286_MEMREADMAX - 1)) {
		return (REG16)LOADINTELWORD(mem + address);
	}
	return memp_read16(address);
}

static INLINE __attribute__((always_inline)) void
np2_i286_inline_memorywrite8(UINT32 address, REG8 value) {
	if (address < I286_MEMWRITEMAX) {
		mem[address] = (UINT8)value;
		return;
	}
	memp_write8(address, value);
}

static INLINE __attribute__((always_inline)) void
np2_i286_inline_memorywrite16(UINT32 address, REG16 value) {
	if (address < (I286_MEMWRITEMAX - 1)) {
		STOREINTELWORD(mem + address, value);
		return;
	}
	memp_write16(address, value);
}

#define NP2_I286_MEMORYREAD8(address) \
	np2_i286_inline_memoryread8((address))
#define NP2_I286_MEMORYREAD16(address) \
	np2_i286_inline_memoryread16((address))
#define NP2_I286_MEMORYWRITE8(address, value) \
	np2_i286_inline_memorywrite8((address), (value))
#define NP2_I286_MEMORYWRITE16(address, value) \
	np2_i286_inline_memorywrite16((address), (value))

#else

#define NP2_I286_MEMORYREAD8(address) memp_read8(address)
#define NP2_I286_MEMORYREAD16(address) memp_read16(address)
#define NP2_I286_MEMORYWRITE8(address, value) memp_write8(address, value)
#define NP2_I286_MEMORYWRITE16(address, value) memp_write16(address, value)

#endif

#endif /* NP2_I286_INLINE_MEM_FASTPATH_H */
