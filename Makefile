all:
	cd src/usr.sbin/ndisload && make
	cd src/sys/modules/ndis && make
install:
	rm -f /boot/kernel/if_ndis.ko /boot/kernel/if_ndis.ko.symbols
	cd src/sys/modules/ndis && make install
	cd src/usr.sbin/ndisload && make install
clean:
	cd src/sys/modules/ndis && make clean
	cd src/usr.sbin/ndisload && make clean
load:
	cd src/sys/modules/ndis && make load
unload:
	cd src/sys/modules/ndis && make unload
