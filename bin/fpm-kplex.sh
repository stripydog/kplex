fpm -s dir -t deb -n seatop-kplex -C rootfs -a armhf --version 1.3.4 --iteration 4 -d isc-dhcp-server
mv `ls -tr *.deb` ./deb

