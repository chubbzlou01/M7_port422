#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#if __LINUX_ARM_ARCH__ < 6
#error SMP not supported on pre-ARMv6 CPUs
#endif

#include <asm/processor.h>

extern int msm_krait_need_wfe_fixup;

#define ALT_SMP(smp, up)					\
	"9998:	" smp "\n"					\
	"	.pushsection \".alt.smp.init\", \"a\"\n"	\
	"	.long	9998b\n"				\
	"	" up "\n"					\
	"	.popsection\n"

#ifdef CONFIG_THUMB2_KERNEL
#define SEV		ALT_SMP("sev.w", "nop.w")
#define WFE()		ALT_SMP(		\
	"wfe.w",				\
	"nop.w"					\
)
#else
#define SEV		ALT_SMP("sev", "nop")
#define WFE()		ALT_SMP("wfe", "nop")
#endif

#ifdef CONFIG_MSM_KRAIT_WFE_FIXUP
#define WFE_SAFE(fixup, tmp) 				\
"	mrs	" tmp ", cpsr\n"			\
"	cmp	" fixup ", #0\n"			\
"	wfeeq\n"					\
"	beq	10f\n"					\
"	cpsid   f\n"					\
"	mrc	p15, 7, " fixup ", c15, c0, 5\n"	\
"	bic	" fixup ", " fixup ", #0x10000\n"	\
"	mcr	p15, 7, " fixup ", c15, c0, 5\n"	\
"	isb\n"						\
"	wfe\n"						\
"	orr	" fixup ", " fixup ", #0x10000\n"	\
"	mcr	p15, 7, " fixup ", c15, c0, 5\n"	\
"	isb\n"						\
"10:	msr	cpsr_cf, " tmp "\n"
#else
#define WFE_SAFE(fixup, tmp)	"	wfe\n"
#endif

static inline void dsb_sev(void)
{
#if __LINUX_ARM_ARCH__ >= 7
	__asm__ __volatile__ (
		"dsb\n"
		SEV
	);
#else
	__asm__ __volatile__ (
		"mcr p15, 0, %0, c7, c10, 4\n"
		SEV
		: : "r" (0)
	);
#endif
}

#ifndef CONFIG_ARM_TICKET_LOCKS

#define arch_spin_is_locked(x)		((x)->lock != 0)
#define arch_spin_unlock_wait(lock) \
	do { while (arch_spin_is_locked(lock)) cpu_relax(); } while (0)

#define arch_spin_lock_flags(lock, flags) arch_spin_lock(lock)

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
	unsigned long tmp, fixup = msm_krait_need_wfe_fixup;

	__asm__ __volatile__(
"1:	ldrex	%[tmp], [%[lock]]\n"
"	teq	%[tmp], #0\n"
"	beq	2f\n"
	WFE_SAFE("%[fixup]", "%[tmp]")
"2:\n"
"	strexeq	%[tmp], %[bit0], [%[lock]]\n"
"	teqeq	%[tmp], #0\n"
"	bne	1b"
	: [tmp] "=&r" (tmp), [fixup] "+r" (fixup)
	: [lock] "r" (&lock->lock), [bit0] "r" (1)
	: "cc");

	smp_mb();
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]"
	: "=&r" (tmp)
	: "r" (&lock->lock), "r" (1)
	: "cc");

	if (tmp == 0) {
		smp_mb();
		return 1;
	} else {
		return 0;
	}
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
	smp_mb();

	__asm__ __volatile__(
"	str	%1, [%0]\n"
	:
	: "r" (&lock->lock), "r" (0)
	: "cc");

	dsb_sev();
}
#else

#define TICKET_SHIFT	16
#define TICKET_BITS	16
#define	TICKET_MASK	0xFFFF

#define arch_spin_lock_flags(lock, flags) arch_spin_lock(lock)

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
	unsigned long tmp, ticket, next_ticket;
	unsigned long fixup = msm_krait_need_wfe_fixup;

	
	__asm__ __volatile__(
"1:	ldrex	%[ticket], [%[lockaddr]]\n"
"	uadd16	%[next_ticket], %[ticket], %[val1]\n"
"	strex	%[tmp], %[next_ticket], [%[lockaddr]]\n"
"	teq	%[tmp], #0\n"
"	bne	1b\n"
"	uxth	%[ticket], %[ticket]\n"
"2:\n"
#ifdef CONFIG_CPU_32v6K
"	beq	3f\n"
	WFE_SAFE("%[fixup]", "%[tmp]")
"3:\n"
#endif
"	ldr	%[tmp], [%[lockaddr]]\n"
"	cmp	%[ticket], %[tmp], lsr #16\n"
"	bne	2b"
	: [ticket]"=&r" (ticket), [tmp]"=&r" (tmp),
	  [next_ticket]"=&r" (next_ticket), [fixup]"+r" (fixup)
	: [lockaddr]"r" (&lock->lock), [val1]"r" (1)
	: "cc");
	smp_mb();
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
	unsigned long tmp, ticket, next_ticket;

	
	__asm__ __volatile__(
"	ldrex	%[ticket], [%[lockaddr]]\n"
"	ror	%[tmp], %[ticket], #16\n"
"	eors	%[tmp], %[tmp], %[ticket]\n"
"	bne	1f\n"
"	uadd16	%[next_ticket], %[ticket], %[val1]\n"
"	strex	%[tmp], %[next_ticket], [%[lockaddr]]\n"
"1:"
	: [ticket]"=&r" (ticket), [tmp]"=&r" (tmp),
	  [next_ticket]"=&r" (next_ticket)
	: [lockaddr]"r" (&lock->lock), [val1]"r" (1)
	: "cc");
	if (!tmp)
		smp_mb();
	return !tmp;
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
	unsigned long ticket, tmp;

	smp_mb();

	
	__asm__ __volatile__(
"1:	ldrex	%[ticket], [%[lockaddr]]\n"
"	uadd16	%[ticket], %[ticket], %[serving1]\n"
"	strex	%[tmp], %[ticket], [%[lockaddr]]\n"
"	teq	%[tmp], #0\n"
"	bne	1b"
	: [ticket]"=&r" (ticket), [tmp]"=&r" (tmp)
	: [lockaddr]"r" (&lock->lock), [serving1]"r" (0x00010000)
	: "cc");
	dsb_sev();
}

static inline void arch_spin_unlock_wait(arch_spinlock_t *lock)
{
	unsigned long ticket, tmp, fixup = msm_krait_need_wfe_fixup;

	
	__asm__ __volatile__(
#ifdef CONFIG_CPU_32v6K
"	cmpne	%[lockaddr], %[lockaddr]\n"
"1:\n"
"	beq	2f\n"
	WFE_SAFE("%[fixup]", "%[tmp]")
"2:\n"
#else
"1:\n"
#endif
"	ldr	%[ticket], [%[lockaddr]]\n"
"	eor	%[ticket], %[ticket], %[ticket], lsr #16\n"
"	uxth	%[ticket], %[ticket]\n"
"	cmp	%[ticket], #0\n"
"	bne	1b"
	: [ticket]"=&r" (ticket), [tmp]"=&r" (tmp),
	  [fixup]"+r" (fixup)
	: [lockaddr]"r" (&lock->lock)
	: "cc");
}

static inline int arch_spin_is_locked(arch_spinlock_t *lock)
{
	unsigned long tmp = ACCESS_ONCE(lock->lock);
	return (((tmp >> TICKET_SHIFT) ^ tmp) & TICKET_MASK) != 0;
}

static inline int arch_spin_is_contended(arch_spinlock_t *lock)
{
	unsigned long tmp = ACCESS_ONCE(lock->lock);
	return ((tmp - (tmp >> TICKET_SHIFT)) & TICKET_MASK) > 1;
}
#endif


static inline void arch_write_lock(arch_rwlock_t *rw)
{
	unsigned long tmp, fixup = msm_krait_need_wfe_fixup;

	__asm__ __volatile__(
"1:	ldrex	%[tmp], [%[lock]]\n"
"	teq	%[tmp], #0\n"
"	beq	2f\n"
	WFE_SAFE("%[fixup]", "%[tmp]")
"2:\n"
"	strexeq	%[tmp], %[bit31], [%[lock]]\n"
"	teq	%[tmp], #0\n"
"	bne	1b"
	: [tmp] "=&r" (tmp), [fixup] "+r" (fixup)
	: [lock] "r" (&rw->lock), [bit31] "r" (0x80000000)
	: "cc");

	smp_mb();
}

static inline int arch_write_trylock(arch_rwlock_t *rw)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]"
	: "=&r" (tmp)
	: "r" (&rw->lock), "r" (0x80000000)
	: "cc");

	if (tmp == 0) {
		smp_mb();
		return 1;
	} else {
		return 0;
	}
}

static inline void arch_write_unlock(arch_rwlock_t *rw)
{
	smp_mb();

	__asm__ __volatile__(
	"str	%1, [%0]\n"
	:
	: "r" (&rw->lock), "r" (0)
	: "cc");

	dsb_sev();
}

#define arch_write_can_lock(x)		((x)->lock == 0)

static inline void arch_read_lock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2, fixup = msm_krait_need_wfe_fixup;

	__asm__ __volatile__(
"1:	ldrex	%[tmp], [%[lock]]\n"
"	adds	%[tmp], %[tmp], #1\n"
"	strexpl	%[tmp2], %[tmp], [%[lock]]\n"
"	bpl	2f\n"
	WFE_SAFE("%[fixup]", "%[tmp]")
"2:\n"
"	rsbpls	%[tmp], %[tmp2], #0\n"
"	bmi	1b"
	: [tmp] "=&r" (tmp), [tmp2] "=&r" (tmp2), [fixup] "+r" (fixup)
	: [lock] "r" (&rw->lock)
	: "cc");

	smp_mb();
}

static inline void arch_read_unlock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2;

	smp_mb();

	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	sub	%0, %0, #1\n"
"	strex	%1, %0, [%2]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&rw->lock)
	: "cc");

	if (tmp == 0)
		dsb_sev();
}

static inline int arch_read_trylock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2 = 1;

	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	adds	%0, %0, #1\n"
"	strexpl	%1, %0, [%2]\n"
	: "=&r" (tmp), "+r" (tmp2)
	: "r" (&rw->lock)
	: "cc");

	smp_mb();
	return tmp2 == 0;
}

#define arch_read_can_lock(x)		((x)->lock < 0x80000000)

#define arch_read_lock_flags(lock, flags) arch_read_lock(lock)
#define arch_write_lock_flags(lock, flags) arch_write_lock(lock)

#define arch_spin_relax(lock)	cpu_relax()
#define arch_read_relax(lock)	cpu_relax()
#define arch_write_relax(lock)	cpu_relax()

#endif 
