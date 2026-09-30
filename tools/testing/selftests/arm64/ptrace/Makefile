# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES) -I$(top_srcdir)/tools/include

TEST_GEN_PROGS := single_step_signal_handler \
		  single_step_spsr_ss \
		  single_step_syscall_stop \
		  sysemu_singlestep \
		  rseq_syscall_pc

include ../../lib.mk

$(OUTPUT)/rseq_syscall_pc: rseq_syscall_pc.c rseq_syscall_pc_asm.S
