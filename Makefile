obj-m := usbtest.o
obj-m += uslave.o
uslave-m += utest_slave.o

KDIR=/lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules 
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 	 
