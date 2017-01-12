# libshareuf

iRODS shared unixfilesystem plugin.

This iRODS plugin is a unix filesystem driver that creates files and
directories with world readable permissions. This permits directly
sharing iRODS vault data with system processes.

It is derived from the libunixfilesystem plug included with iRODS.

### Dependencies for make

Compilation requires CMake >= 3.5 and `libquadmath` library. For EL7,
the following packages from `base` and `epel` meet the requirements.

    cmake3
    libquadmath-devel


The following packages from RENCI are required.

    irods-devel
    irods-runtime
    irods-externals-boost1.60.0-0
    irods-externals-boost1.60.0-0-1.0-1.x86_64
    irods-externals-jansson2.7-0-1.0-1.x86_64

RENCI's boost package includes feature patches to stock boost. It's
probably ok to have the standard `boost` package installed along with
RENCI's but I remove any stock packages for good measure.

### Compile and Install

To install this plugin:

    $ mkdir build
    $ cd build
    $ cmake3 -DCMAKE_INSTALL_PREFIX=/ ..
    $ make clean
    $ make
    $ sudo make install

### Example usage

    $ iadmin mkresc shareufResc shareuf host.irods.vm:/var/lib/shareufResc


