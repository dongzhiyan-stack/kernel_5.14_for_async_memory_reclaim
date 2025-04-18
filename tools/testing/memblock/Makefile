# SPDX-License-Identifier: GPL-2.0

# Memblock simulator requires AddressSanitizer (libasan) and liburcu development
# packages installed
CFLAGS += -I. -I../../include -Wall -O2 -fsanitize=address \
	  -fsanitize=undefined -D CONFIG_PHYS_ADDR_T_64BIT
LDFLAGS += -fsanitize=address -fsanitize=undefined
TARGETS = main
OFILES = main.o memblock.o lib/slab.o mmzone.o slab.o
EXTR_SRC = ../../../mm/memblock.c

ifeq ($(BUILD), 32)
	CFLAGS += -m32
	LDFLAGS += -m32
endif

# Process user parameters
include scripts/Makefile.include

main: $(OFILES)

$(OFILES): include

include: ../../../include/linux/memblock.h ../../include/linux/*.h \
	../../include/asm/*.h

	@mkdir -p linux
	test -L linux/memblock.h || ln -s ../../../../include/linux/memblock.h linux/memblock.h
	test -L asm/cmpxchg.h || ln -s ../../../arch/x86/include/asm/cmpxchg.h asm/cmpxchg.h

memblock.c: $(EXTR_SRC)
	test -L memblock.c || ln -s $(EXTR_SRC) memblock.c

clean:
	$(RM) $(TARGETS) $(OFILES) linux/memblock.h memblock.c asm/cmpxchg.h

help:
	@echo  'Memblock simulator'
	@echo  ''
	@echo  'Available targets:'
	@echo  '  main		  - Build the memblock simulator'
	@echo  '  clean		  - Remove generated files and symlinks in the directory'
	@echo  ''
	@echo  'Configuration:'
	@echo  '  make NUMA=1               - simulate enabled NUMA'
	@echo  '  make MOVABLE_NODE=1       - override `movable_node_is_enabled`'
	@echo  '                              definition to simulate movable NUMA nodes'
	@echo  '  make 32BIT_PHYS_ADDR_T=1  - Use 32 bit physical addresses'

vpath %.c ../../lib

.PHONY: clean include help
