/* q1psx — psxetc's interrupt API over the game's own exception vector
   (engine/irq.s; docs/porting-notes.md §81.1).

   The calls and their effects are PSn00bSDK v0.24 psxetc's (libpsxetc
   interrupts.c), whose object the link then leaves out: psxgpu, psxcd and
   psxspu register their callbacks here as before. Only the way in differs:
   ResetCallback points the RAM exception vector (0x80000080) at q_exc,
   which runs the callbacks itself, the kernel's dispatch skipped (no
   HookEntryInt, no ChangeClearPAD / ChangeClearRCnt: the kernel no longer
   sees an interrupt). The calls nothing makes are left out (psxetc's
   GetInterruptCallback, GetDMACallback, GetDMAPriority, RestartCallback,
   StopCallback, until docs/porting-notes.md §119): a library wanting one
   would pull psxetc's object in, and the link would stop on the twice
   defined calls. */

#include <stdint.h>
#include <stdio.h>
#include <psxapi.h>
#include <psxetc.h>
#include <hwregs_c.h>

#include "mem.h"

extern void (*q_irq_handlers[11])(void);
extern void (*q_dma_handlers[7])(void);
extern uint32_t q_bios_vec[4];
extern const uint32_t q_vec_tpl[4];
extern uint8_t q_isr_stack[];
extern void q_dma_isr(void);

#define VECTOR ((volatile uint32_t *) 0x80000080)
#define Q_ISR_STACK 1024                  /* irq.s's */

static int      installed, num_dma, bios_saved;
static uint16_t saved_mask;
static uint32_t saved_dpcr, saved_dicr;

void *InterruptCallback(IRQ_Channel irq, void (*func)(void))
{
	void *old = (void *) q_irq_handlers[irq];
	q_irq_handlers[irq] = func;
	if (func)
		IRQ_MASK |= (uint16_t) (1 << irq);
	else
		IRQ_MASK &= (uint16_t) ~(1 << irq);
	return old;
}

/* (psxetc's own order and register writes, the flags DICR holds written
   back as they read too) */
void *DMACallback(DMA_Channel dma, void (*func)(void))
{
	void *old = (void *) q_dma_handlers[dma];
	q_dma_handlers[dma] = func;
	if (old) {
		if (func)
			return old;
		if (--num_dma) {
			DMA_DICR &= ~(0x10000u << dma);
		} else {
			DMA_DICR = 0;
			IRQ_MASK &= (uint16_t) ~(1 << IRQ_DMA);
			q_irq_handlers[IRQ_DMA] = 0;
		}
		return old;
	}
	if (!func)
		return old;
	DMA_DICR |= (0x10000u << dma) | 0x800000u;
	if (!num_dma++) {
		q_irq_handlers[IRQ_DMA] = q_dma_isr;
		IRQ_MASK |= 1 << IRQ_DMA;
	}
	return old;
}

int SetDMAPriority(DMA_Channel dma, int priority)
{
	int      sh  = dma * 4;
	uint32_t reg = DMA_DPCR, was = reg >> sh;
	reg &= ~(15u << sh);
	if (priority >= 0)
		reg |= (uint32_t) ((priority & 7) | 8) << sh;
	DMA_DPCR = reg;
	return was & 8 ? (int) (was & 7) : -1;
}

/* the vector: the kernel's four words kept (run from q_bios_vec for every
   exception but an interrupt), ours in their place */
static Q_COLD void vector_install(void)
{
	if (!bios_saved) {
		for (int i = 0; i < 4; i++)
			q_bios_vec[i] = VECTOR[i];
		bios_saved = 1;
		/* (lui k0 / addiu or ori k0 / jr k0: position-free, as on every
		   kernel the game met: Sony's, OpenBIOS) */
		if ((q_bios_vec[0] >> 16) != 0x3c1a || ((q_bios_vec[1] >> 16) != 0x275a &&
		    (q_bios_vec[1] >> 16) != 0x375a) || q_bios_vec[2] != 0x03400008)
			printf("irq: the kernel's vector is not lui / addiu / jr k0: %08x %08x %08x %08x\n",
			       (unsigned) q_bios_vec[0], (unsigned) q_bios_vec[1], (unsigned) q_bios_vec[2],
			       (unsigned) q_bios_vec[3]);
#if defined(Q1PSX_BENCH)
		for (int i = 0; i < Q_ISR_STACK; i++)    /* (irq_stack_room's mark) */
			q_isr_stack[i] = 0xa5;
#endif
	}
	for (int i = 0; i < 4; i++)
		VECTOR[i] = q_vec_tpl[i];
	FlushCache();
}

static Q_COLD void restart(void)
{
	IRQ_STAT = 0;
	IRQ_MASK = saved_mask;
	DMA_DPCR = saved_dpcr;
	DMA_DICR = saved_dicr;
	vector_install();
	installed = 1;
	ExitCriticalSection();
}

Q_COLD int ResetCallback(void)
{
	if (installed)
		return -1;
	EnterCriticalSection();
	saved_dpcr = 0x03333333;
	saved_mask = 0;
	saved_dicr = 0;
	for (int i = 0; i < 11; i++)
		q_irq_handlers[i] = 0;
	BUS_COM_DELAY = 0x1325;
	for (int i = 0; i < 7; i++)
		q_dma_handlers[i] = 0;
	_96_remove();
	restart();
	return 0;
}

/* the bench: the interrupt stack's bytes no callback reached */
int irq_stack_room(void)
{
	int n = 0;
	while (n < Q_ISR_STACK && q_isr_stack[n] == 0xa5)
		n++;
	return n;
}
