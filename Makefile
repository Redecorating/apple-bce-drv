obj-m += apple-bce.o
obj-m += apple-bce-vhci.o
obj-m += aaudio.o

apple-bce-objs := apple_bce.o mailbox.o queue.o queue_dma.o
apple-bce-vhci-objs := vhci/vhci.o vhci/queue.o vhci/transfer.o
aaudio-objs := audio/audio.o audio/protocol.o audio/protocol_bce.o audio/pcm.o

MY_CFLAGS += -DWITHOUT_NVME_PATCH
#MY_CFLAGS += -g -DDEBUG
ccflags-y += ${MY_CFLAGS}
CC += ${MY_CFLAGS}

KVERSION := $(KERNELRELEASE)
ifeq ($(origin KERNELRELEASE), undefined)
KVERSION := $(shell uname -r)
endif

KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

.PHONY: all

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
