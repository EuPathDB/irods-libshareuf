# libshareuf

iRODS shared unixfilesystem plugin.

This iRODS plugin is a unix filesystem driver that creates files and
directories with world readable permissions. This permits directly
sharing iRODS vault data with system processes.

It is derived from the libunixfilesystem plug included with iRODS.

### Dependencies for make

Compilation requires CMake >= 3.5 and `libquadmath` library as well as
iRODS development libraries. For EL7, the required packages can be found
in `base`, `epel` and [`renci-irods`](https://packages.irods.org) YUM repos.

    sudo yum install -y \
      cmake3 \
      libquadmath \
      openssl-devel \
      rpm-build \
      irods-devel \
      irods-externals-avro1.7.7-0 \
      irods-externals-boost1.60.0-0 \
      irods-externals-clang-runtime3.8-0 \
      irods-externals-clang3.8-0 \
      irods-externals-cppzmq4.1-0 \
      irods-externals-jansson2.7-0 \
      irods-externals-libarchive3.1.2-0 \
      irods-externals-zeromq4-14.1.3-0 \
      irods-icommands \
      irods-runtime  \
      irods-server

RENCI's boost package includes feature patches to stock boost. It's
probably ok to have the standard `boost` package installed along with
RENCI's but I remove any stock packages for good measure.

### Compile and Install

To install this plugin:

    $ git clone https://github.com/EuPathDB/irods-libshareuf.git
    $ cd irods-libshareuf
    $ mkdir build
    $ cd build
    $ cmake3 -DCMAKE_INSTALL_PREFIX=/ ..
    $ make clean
    $ make
    $ sudo make install

To make RPM

Confirm correct values in `CMakeLists.txt` for
`IRODS_PLUGIN_VERSION_MAJOR`, `IRODS_PLUGIN_VERSION_MINOR`,
`IRODS_PLUGIN_VERSION_PATCH`, `CPACK_RPM_PACKAGE_RELEASE`

Use CMake packaging routines to build binary package.

    $ make package
    $ rpm --addsign irods-resource-plugin-shareuf-*.rpm

### Testing

    $ cd irods-libshareuf
    $ ./makeAndRunTests

### Example usage

    $ iadmin mkresc shareufResc shareuf host.irods.vm:/var/lib/shareufResc


