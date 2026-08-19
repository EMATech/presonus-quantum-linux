# Out-of-tree ALSA PCI driver for PreSonus Quantum Thunderbolt Family
# Build: make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
# Load:  sudo insmod snd-quantum.ko  (or modprobe after install)

obj-m := snd-quantum.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
UCM2_DIR ?= /usr/share/alsa/ucm2
WIREPLUMBER_MAIN_LUA_DIR ?= /usr/share/wireplumber/main.lua.d
WIREPLUMBER_MAIN_CONF_DIR ?= /usr/share/wireplumber/wireplumber.conf.d

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install-module: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

install-ucm:
	install -D -m 0644 ../alsa/ucm2/Quantum/Common.conf \
		$(DESTDIR)$(UCM2_DIR)/Quantum/Common.conf
	install -D -m 0644 ../alsa/ucm2/Quantum2/HiFi.conf \
		$(DESTDIR)$(UCM2_DIR)/Quantum2/HiFi.conf
	install -D -m 0644 ../alsa/ucm2/conf.d/snd-quantum/snd-quantum.conf \
        $(DESTDIR)$(UCM2_DIR)/conf.d/snd-quantum/snd-quantum.conf

# FIXME: detect Wireplumber version. 0.4 -> install-wp4, 0.5 -> install-wp5
install-wp:
install-wp4:
	install -D -m 0644 \
		../alsa/wireplumber/51-quantum.lua \
		$(DESTDIR)$(WIREPLUMBER_MAIN_LUA_DIR)/51-quantum.lua

install-wp5:
	install -D -m 0644 \
		../alsa/wireplumber/51-quantum.conf \
		$(DESTDIR)$(WIREPLUMBER_MAIN_CONF_DIR)/51-quantum.conf

install-audio: install-ucm install-wp5

install: install-module install-audio

.PHONY: all clean install install-module install-ucm install-wp5 install-audio
