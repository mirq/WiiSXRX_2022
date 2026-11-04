# paltest: make -C tools/paltest -> paltest (WarpOS executable)
MGL_DIR ?= /home/mirek/MiniGL_WOS_V19_mglQ3
CPU ?= 603e
include $(MGL_DIR)/Makefile.wos.gcc.common
WOS_CPPFLAGS_NOSDL := $(subst -isystem $(WOS_SDK_ROOT)/include/sdl,,$(WOS_CPPFLAGS))
CFLAGS := -std=gnu99 $(WOS_ARCH_CFLAGS) -O2 -fno-common -msdata=none -G0 -w \
          $(WOS_CPPFLAGS_NOSDL) -Uamiga -Uamigaos -I$(MGL_DIR)/include
all: paltest
paltest.o: paltest.c ; $(CC) $(CFLAGS) -c $< -o $@
paltest.elf: paltest.o
	$(LD) $(WOS_LINK_FLAGS) $(WOS_LIBRARY_DIRS) -o $@ $(WOS_STARTFILE) $^ $(MGL_DIR)/lib/libminigl.a $(WOS_RUNTIME_LIBS) $(WOS_ENDFILE)
	$(WOS_CROSS)objcopy --strip-debug $@
	$(call WOS_VALIDATE_ETREL,$@)
paltest: paltest.elf
	python3 -I ../elf2warpos/elf2warpos.py $< $@
clean: ; rm -f *.o *.elf paltest
