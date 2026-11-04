# avbench: make -C tools/avbench  -> avbench (WarpOS executable)
MGL_DIR ?= /home/mirek/MiniGL_WOS_V19_mglQ3
CPU ?= 603e
include $(MGL_DIR)/Makefile.wos.gcc.common
CFLAGS := -std=gnu99 $(WOS_ARCH_CFLAGS) -O2 -fno-common -msdata=none -G0 -Wall \
          $(WOS_CPPFLAGS) -Uamiga -Uamigaos
# the vector file: G4 instructions; the stack is not trusted (no vector spills)
VFLAGS := $(subst -mcpu=$(CPU),-mcpu=7400,$(CFLAGS)) -maltivec -mabi=altivec

all: avbench
avbench.o: avbench.c ; $(CC) $(CFLAGS) -c $< -o $@
avvec.o: avvec.c ; $(CC) $(VFLAGS) -c $< -o $@
avstress.o: avstress.s ; $(CC) -x assembler-with-cpp -mcpu=7400 -maltivec -c $< -o $@
avalign.o: avalign.s ; $(CC) -x assembler-with-cpp -mcpu=7400 -c $< -o $@
avbench.elf: avbench.o avvec.o avstress.o avalign.o
	$(LD) $(WOS_LINK_FLAGS) $(WOS_LIBRARY_DIRS) -o $@ $(WOS_STARTFILE) $^ $(WOS_RUNTIME_LIBS) $(WOS_ENDFILE)
	$(WOS_CROSS)objcopy --strip-debug $@
	$(call WOS_VALIDATE_ETREL,$@)
avbench: avbench.elf
	python3 -I ../elf2warpos/elf2warpos.py $< $@
clean: ; rm -f *.o *.elf avbench
