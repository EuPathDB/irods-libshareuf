GCC = g++ 
INC=-I/usr/include/irods/ -I/usr/include/irods/boost -I/usr/include/irods/jansson/src

all:
	${GCC} ${INC} -Dlinux_platform -fPIC -shared -o libshareuf.so libshareuf.cpp /usr/lib/libirods_client_plugins.a

install:
	@cp libshareuf.so /var/lib/irods/plugins/resources
	@chown irods:irods /var/lib/irods/plugins/resources/libshareuf.so

clean:
	@rm -f libshareuf.so

