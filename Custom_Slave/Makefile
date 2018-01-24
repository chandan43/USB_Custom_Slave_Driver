#obj-m := zero.o
obj-m := mslave.o
obj-m += led.o

KDIR=/home/elinux/linux-4.4.96

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) SUBDIRS=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

# make ARCH=arm CROSS_COMPILE=arm-linux-
