ifeq ($(strip $(DEVKITPPC)),)
$(error Install devkitPro and set DEVKITPPC, e.g. export DEVKITPPC=/opt/devkitpro/devkitPPC)
endif
include $(DEVKITPPC)/wii_rules

TARGET  := boot
CFLAGS  := -O2 -Wall $(MACHDEP) -I$(LIBOGC_INC)
LDFLAGS := $(MACHDEP) -L$(LIBOGC_LIB)
LIBS    := -lwiiuse -lbte -lfat -logc -lm

all: package

source/main.o: source/main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: source/main.o
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

package: $(TARGET).dol
	mkdir -p apps/mathcanvas
	cp $(TARGET).dol meta.xml apps/mathcanvas/

clean:
	rm -rf source/*.o *.elf *.dol apps

.PHONY: all clean package
