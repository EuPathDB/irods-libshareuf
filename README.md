# libshareuf

iRODS shared unixfilesystem plugin.

This iRODS plugin is a unix filesystem driver that creates files and
directories with world readable permissions. This permits directly
sharing iRODS vault data with system processes.

It is derived from the libunixfilesystem plug included with iRODS.

The `irods-dev` and `irods-runtime` packages are required to compile.

### Installation

To install this plugin:

    $ make
    $ sudo make install

### Example usage

    $ iadmin mkresc shareufResc shareuf host.irods.vm:/var/lib/shareufResc
