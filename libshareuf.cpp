/* A derivative of unixfilesystem in wich directories/files are
 * created with mode 0755/0655 on the resource filesystem so system
 * processes can read them.
 *
 * Example usage:
 *  iadmin mkresc share shareuf host.irods.vm:/tmp/shareResc
 *
 * mheiges@uga.edu
 * EuPathDB Bioinformatics Resource Center, 2016
 */
// =-=-=-=-=-=-=-
// irods includes
#include "msParam.h"
#include "reGlobalsExtern.hpp"
#include "rcConnect.h"
#include "readServerConfig.hpp"
#include "miscServerFunct.hpp"
#include "generalAdmin.h"

// =-=-=-=-=-=-=-
#include "irods_resource_plugin.hpp"
#include "irods_file_object.hpp"
#include "irods_physical_object.hpp"
#include "irods_collection_object.hpp"
#include "irods_string_tokenize.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_resource_redirect.hpp"
#include "irods_stacktrace.hpp"
#include "irods_server_properties.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_kvp_string_parser.hpp"

// =-=-=-=-=-=-=-
// stl includes
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

// =-=-=-=-=-=-=-
// boost includes
#include <boost/function.hpp>
#include <boost/any.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

// =-=-=-=-=-=-=-
// system includes
#ifndef _WIN32
#include <sys/file.h>
#include <sys/param.h>
#endif
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/types.h>
#if defined(osx_platform)
#include <sys/malloc.h>
#else
#include <malloc.h>
#endif
#include <fcntl.h>
#ifndef _WIN32
#include <sys/file.h>
#include <unistd.h>
#endif
#include <dirent.h>

#if defined(solaris_platform)
#include <sys/statvfs.h>
#endif

#if defined(osx_platform)
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if defined(linux_platform)
#include <sys/vfs.h>
#endif
#include <sys/stat.h>

#include <string.h>
#include <sstream>


// =-=-=-=-=-=-=-
// 1. Define utility functions that the operations might need
const std::string DEFAULT_VAULT_DIR_MODE( "default_vault_directory_mode_kw" );
const std::string HIGH_WATER_MARK( "high_water_mark" );

// =-=-=-=-=-=-=-
// NOTE: All storage resources must do this on the physical path stored in the file object and then update
//       the file object's physical path with the full path

static irods::error shareuf_copy_plugin(
    int mode,
    const char* srcFileName,
    const char* destFileName ) {

    irods::error result = SUCCESS();

    int inFd = open( srcFileName, O_RDONLY, 0 );
    int err_status = UNIX_FILE_OPEN_ERR - errno;
    if ( inFd < 0 ) {
        std::stringstream msg_stream;
        msg_stream << "Open error for srcFileName \"" << srcFileName << "\", status = " << err_status;
        result = ERROR( err_status, msg_stream.str() );
    }
    else {
        struct stat statbuf;
        int status = stat( srcFileName, &statbuf );
        err_status = errno;
        if ( status < 0 ) {
            std::stringstream msg_stream;
            msg_stream << "stat failed on \"" << srcFileName << "\", status: " << err_status;
            result = ERROR( UNIX_FILE_STAT_ERR, msg_stream.str() );
        }
        else if ( ( statbuf.st_mode & S_IFREG ) == 0 ) {
            std::stringstream msg_stream;
            msg_stream << "srcFileName \"" << srcFileName << "\" is not a regular file.";
            result = ERROR( UNIX_FILE_STAT_ERR, msg_stream.str() );
        }
        else {
            int outFd = open( destFileName, O_WRONLY | O_CREAT | O_TRUNC, mode );
            err_status = UNIX_FILE_OPEN_ERR - errno;
            if ( outFd < 0 ) {
                std::stringstream msg_stream;
                msg_stream << "Open error for destFileName \"" << destFileName << "\", status = " << err_status;
                result = ERROR( err_status, msg_stream.str() );
            }
            else {
                int trans_buff_size = 0;
                irods::error ret = irods::get_advanced_setting<int>(
                                       irods::CFG_TRANS_BUFFER_SIZE_FOR_PARA_TRANS,
                                       trans_buff_size );
                if ( !ret.ok() ) {
                    close( inFd );
                    close( outFd );
                    return PASS( ret );
                }
                trans_buff_size *= 1024 * 1024;


                std::vector<char> myBuf( trans_buff_size );
                int bytesRead;
                rodsLong_t bytesCopied = 0;
                while ( result.ok() && ( bytesRead = read( inFd, ( void * ) myBuf.data(), trans_buff_size ) ) > 0 ) {
                    int bytesWritten = write( outFd, ( void * ) myBuf.data(), bytesRead );
                    err_status = UNIX_FILE_WRITE_ERR - errno;
                    if ( ( result = ASSERT_ERROR( bytesWritten > 0, err_status, "Write error for srcFileName %s, status = %d",
                                                  destFileName, status ) ).ok() ) {
                        bytesCopied += bytesWritten;
                    }
                }

                close( outFd );

                if ( result.ok() ) {
                    result = ASSERT_ERROR( bytesCopied == statbuf.st_size, SYS_COPY_LEN_ERR, "Copied size %lld does not match source size %lld of %s",
                                           bytesCopied, statbuf.st_size, srcFileName );
                }
            }
        }
        close( inFd );
    }
    return result;
}



// =-=-=-=-=-=-=-
/// @brief Generates a full path name from the partial physical path and the specified resource's vault path
irods::error unix_generate_full_path(
    irods::plugin_property_map& _prop_map,
    const std::string&           _phy_path,
    std::string&                 _ret_string ) {
    irods::error result = SUCCESS();
    irods::error ret;
    std::string vault_path;
    // TODO - getting vault path by property will not likely work for coordinating nodes
    ret = _prop_map.get<std::string>( irods::RESOURCE_PATH, vault_path );
    if ( ( result = ASSERT_ERROR( ret.ok(), SYS_INVALID_INPUT_PARAM, "resource has no vault path." ) ).ok() ) {
        if ( _phy_path.compare( 0, 1, "/" ) != 0 &&
                _phy_path.compare( 0, vault_path.size(), vault_path ) != 0 ) {
            _ret_string  = vault_path;
            _ret_string += "/";
            _ret_string += _phy_path;
        }
        else {
            // The physical path already contains the vault path
            _ret_string = _phy_path;
        }
    }

    return result;

} // unix_generate_full_path

// =-=-=-=-=-=-=-
/// @brief update the physical path in the file object
irods::error unix_check_path(
    irods::resource_plugin_context& _ctx ) {
    irods::error result = SUCCESS();

    // =-=-=-=-=-=-=-
    // try dynamic cast on ptr, throw error otherwise
    irods::data_object_ptr data_obj = boost::dynamic_pointer_cast< irods::data_object >( _ctx.fco() );
    if ( ( result = ASSERT_ERROR( data_obj.get(), SYS_INVALID_INPUT_PARAM, "Failed to cast fco to data_object." ) ).ok() ) {
        // =-=-=-=-=-=-=-
        // NOTE: Must do this for all storage resources
        std::string full_path;
        irods::error ret = unix_generate_full_path( _ctx.prop_map(),
                           data_obj->physical_path(),
                           full_path );
        if ( ( result = ASSERT_PASS( ret, "Failed generating full path for object." ) ).ok() ) {
            data_obj->physical_path( full_path );
        }
    }

    return result;

} // unix_check_path

// =-=-=-=-=-=-=-
/// @brief Checks the basic operation parameters and updates the physical path in the file object
template< typename DEST_TYPE >
irods::error unix_check_params_and_path(
    irods::resource_plugin_context& _ctx ) {

    irods::error result = SUCCESS();
    irods::error ret;

    // =-=-=-=-=-=-=-
    // verify that the resc context is valid
    ret = _ctx.valid< DEST_TYPE >();
    if ( ( result = ASSERT_PASS( ret, "resource context is invalid." ) ).ok() ) {
        result = unix_check_path( _ctx );
    }

    return result;

} // unix_check_params_and_path

// =-=-=-=-=-=-=-
/// @brief Checks the basic operation parameters and updates the physical path in the file object
irods::error unix_check_params_and_path(
    irods::resource_plugin_context& _ctx ) {

    irods::error result = SUCCESS();
    irods::error ret;

    // =-=-=-=-=-=-=-
    // verify that the resc context is valid
    ret = _ctx.valid();
    if ( ( result = ASSERT_PASS( ret, "unix_check_params_and_path - resource context is invalid" ) ).ok() ) {
        result = unix_check_path( _ctx );
    }

    return result;

} // unix_check_params_and_path

// =-=-=-=-=-=-=-
//@brief Recursively make all of the dirs in the path
irods::error shareuf_mkdir_r(
    const std::string& path,
    mode_t mode ) {
    irods::error result = SUCCESS();
    std::string subdir;
    std::size_t pos = 0;
    bool done = false;

    mode_t myMask = umask( ( mode_t ) 0000 );

    while ( !done && result.ok() ) {
        pos = path.find_first_of( '/', pos + 1 );
        if ( pos > 0 ) {
            subdir = path.substr( 0, pos );

            int status = mkdir( subdir.c_str(), mode );

            struct stat fStat;
            stat( subdir.c_str(), &fStat );
            rodsLog( LOG_DEBUG, "shareuf_mkdir_r '%s' made with mode %o",
                     subdir.c_str(), fStat.st_mode );

            // =-=-=-=-=-=-=-
            // handle error cases
            result = ASSERT_ERROR( status >= 0 || errno == EEXIST, UNIX_FILE_RENAME_ERR - errno, "mkdir error for \"%s\", errno = \"%s\", status = %d.",
                                   subdir.c_str(), strerror( errno ), status );
        }
        if ( pos == std::string::npos ) {
            done = true;
        }
    }

    // reset the old mask
    umask( ( mode_t ) myMask );

    return result;

} // shareuf_mkdir_r

extern "C" {

    // =-=-=-=-=-=-=-
    // 2. Define operations which will be called by the file*
    //    calls declared in server/driver/include/fileDriver.h
    // =-=-=-=-=-=-=-

    // =-=-=-=-=-=-=-
    // NOTE :: to access properties in the _prop_map do the
    //      :: following :
    //      :: double my_var = 0.0;
    //      :: irods::error ret = _prop_map.get< double >( "my_key", my_var );
    // =-=-=-=-=-=-=-

    /// =-=-=-=-=-=-=-
    /// @brief interface to notify of a file registration
    irods::error shareuf_registered_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        result = ASSERT_PASS( ret, "Invalid parameters or physical path." );

        // NOOP
        return result;
    }

    /// =-=-=-=-=-=-=-
    /// @brief interface to notify of a file unregistration
    irods::error shareuf_unregistered_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        result = ASSERT_PASS( ret, "Invalid parameters or physical path." );

        // NOOP
        return result;
    }

    /// =-=-=-=-=-=-=-
    /// @brief interface to notify of a file modification
    irods::error shareuf_modified_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        result = ASSERT_PASS( ret, "Invalid parameters or physical path." );

        // NOOP
        return result;
    }

    /// =-=-=-=-=-=-=-
    /// @brief interface to notify of a file operation
    irods::error shareuf_notify_plugin(
        irods::resource_plugin_context& _ctx,
        const std::string* ) {
        irods::error result = SUCCESS();
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        result = ASSERT_PASS( ret, "Invalid parameters or physical path." );

        // NOOP
        return result;
    }

    // =-=-=-=-=-=-=-
    // interface to determine free space on a device given a path
    irods::error shareuf_get_fsfreespace_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the hierarchy to the desired object
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );
            size_t found = fco->physical_path().find_last_of( "/" );
            std::string path = fco->physical_path().substr( 0, found + 1 );
            int status = -1;
            rodsLong_t fssize = USER_NO_SUPPORT_ERR;
#if defined(solaris_platform)
            struct statvfs statbuf;
#else
            struct statfs statbuf;
#endif

#if not defined(windows_platform)
#if defined(solaris_platform)
            status = statvfs( path.c_str(), &statbuf );
#elif defined(sgi_platform)
            status = statfs( path.c_str(), &statbuf, sizeof( struct statfs ), 0 );
#elif defined(aix_platform) || defined(linux_platform) || defined(osx_platform)
            status = statfs( path.c_str(), &statbuf );
#endif

            // =-=-=-=-=-=-=-
            // handle error, if any
            int err_status = UNIX_FILE_GET_FS_FREESPACE_ERR - errno;
            if ( ( result = ASSERT_ERROR( status >= 0, err_status, "Statfs error for \"%s\", status = %d.",
                                          path.c_str(), err_status ) ).ok() ) {

#if defined(sgi_platform)
                if ( statbuf.f_frsize > 0 ) {
                    fssize = statbuf.f_frsize;
                }
                else {
                    fssize = statbuf.f_bsize;
                }
                fssize *= statbuf.f_bavail;
#endif

#if defined(aix_platform) || defined(osx_platform) ||   \
    defined(linux_platform)
                fssize = statbuf.f_bavail * statbuf.f_bsize;
#elif defined(sgi_platform)
                fssize = statbuf.f_bfree * statbuf.f_bsize;
#endif
                result.code( fssize );
            }

#endif /* solaris_platform, sgi_platform .... */
        }

        return result;

    } // shareuf_get_fsfreespace_plugin

    irods::error stat_vault_path(
            const std::string& _path,
            struct statfs&     _sb ) {
        namespace bfs = boost::filesystem;

        bfs::path vp( _path );

        // walk the vault path until we find an
        // existing directory which we can stat
        while( !bfs::exists( vp ) ) {
            vp = vp.parent_path();
        }

        if( vp.empty() ) {
            std::string msg( "path does not exist [" );
            msg += _path + "]";
            return ERROR(
                       SYS_INVALID_INPUT_PARAM,
                       msg );
        }

        int err = statfs( vp.string().c_str(), &_sb );
        if( err < 0 ) {
            std::stringstream msg;
            msg << "statfs failed for ["
                << _path << "] errno " << errno;
            return ERROR(
                    errno,
                    msg.str() );
        }

        return SUCCESS();

    } // stat_vault_path

    static bool replica_exceeds_high_water_mark(
        irods::resource_plugin_context& _ctx,
        rodsLong_t                      _file_size ) {
        namespace bt = boost;
        namespace fs = boost::filesystem;

        std::string hwm_str;
        irods::error ret = _ctx.prop_map().get<std::string>(
                               HIGH_WATER_MARK,
                               hwm_str );
        if( !ret.ok() ) {
            return false;
        }

        rodsLong_t hwm_val = 0;
        try {
            hwm_val = bt::lexical_cast<rodsLong_t>(hwm_str);
        } catch ( const bt::bad_lexical_cast& ) {
            rodsLog(
                LOG_ERROR,
                "malformed high water mark [%s]",
                hwm_str.c_str() );
            return false;
        }

        std::string vault_path;
        ret = _ctx.prop_map().get<std::string>(
                  irods::RESOURCE_PATH,
                  vault_path );
        if( !ret.ok() ) {
            rodsLog(
                LOG_ERROR,
                "missing vault path" );
            return false;
        }

        struct statfs sb;
        ret = stat_vault_path(
                  vault_path,
                  sb );
        if( !ret.ok() ) {
            irods::log( PASS( ret ) );
            return false;
        }

        fsblkcnt_t cap  = sb.f_bsize * sb.f_blocks;
        fsblkcnt_t free = sb.f_bsize * sb.f_bavail;

        uintmax_t used_space = cap - free;
        uintmax_t new_used_space = _file_size + used_space;
        if( new_used_space > hwm_val ) {
            return true;
        }

        return false;

    } // replica_exceeds_high_water_mark

    // =-=-=-=-=-=-=-
    // interface for POSIX create
    irods::error shareuf_create_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            char* kvp_str = getValByKey(
                                &fco->cond_input(),
                                KEY_VALUE_PASSTHROUGH_KW );
            if ( kvp_str ) {
                irods::kvp_map_t kvp;
                ret = irods::parse_kvp_string(
                          kvp_str,
                          kvp );
                if ( !ret.ok() ) {
                    irods::log( PASS( ret ) );
                }
                else {
                    irods::kvp_map_t::iterator itr = kvp.begin();
                    for ( ; itr != kvp.end(); ++ itr ) {
                        rodsLog(
                            LOG_DEBUG,
                            "shareuf_create_plugin - kv_pass :: key [%s] - value [%s]",
                            itr->first.c_str(),
                            itr->second.c_str() );
                    } // for itr
                }
            }

            ret = shareuf_get_fsfreespace_plugin( _ctx );
            if ( ( result = ASSERT_PASS( ret, "Error determining freespace on system." ) ).ok() ) {
                rodsLong_t file_size = fco->size();
                if( replica_exceeds_high_water_mark( _ctx, file_size ) ) {
                    std::stringstream msg;
                    msg << "file size " << file_size << " exceeds high water mark";
                    return ERROR(
                               USER_FILE_TOO_LARGE,
                               msg.str() );
                }

                if ( ( result = ASSERT_ERROR( file_size < 0 || ret.code() >= file_size, USER_FILE_TOO_LARGE, "File size: %ld is greater than space left on device: %ld",
                                              file_size, ret.code() ) ).ok() ) {
                    // =-=-=-=-=-=-=-
                    // make call to umask & open for create
                    mode_t myMask = umask( ( mode_t ) 0000 );
                    int    fd     = open( fco->physical_path().c_str(), O_RDWR | O_CREAT | O_EXCL, 0644 );
                    int errsav = errno;

                    // =-=-=-=-=-=-=-
                    // reset the old mask
                    ( void ) umask( ( mode_t ) myMask );

                    // =-=-=-=-=-=-=-
                    // if we got a 0 descriptor, try again
                    if ( fd == 0 ) {

                        close( fd );
                        int null_fd = open( "/dev/null", O_RDWR, 0 );

                        // =-=-=-=-=-=-=-
                        // make call to umask & open for create
                        mode_t myMask = umask( ( mode_t ) 0000 );
                        fd = open( fco->physical_path().c_str(), O_RDWR | O_CREAT | O_EXCL, fco->mode() );
                        errsav = errno;
                        if ( null_fd >= 0 ) {
                            close( null_fd );
                        }
                        rodsLog( LOG_NOTICE, "shareuf_create_plugin: 0 descriptor" );

                        // =-=-=-=-=-=-=-
                        // reset the old mask
                        ( void ) umask( ( mode_t ) myMask );
                    }

                    // =-=-=-=-=-=-=-
                    // trap error case with bad fd
                    if ( fd < 0 ) {
                        int status = UNIX_FILE_CREATE_ERR - errsav;
                        std::stringstream msg;
                        msg << "create error for \"";
                        msg << fco->physical_path();
                        msg << "\", errno = \"";
                        msg << strerror( errsav );
                        msg << "\".";
                        // =-=-=-=-=-=-=-
                        // WARNING :: Major Assumptions are made upstream and use the FD also as a
                        //         :: Status, if this is not done EVERYTHING BREAKS!!!!111one
                        fco->file_descriptor( status );
                        result = ERROR( status, msg.str() );
                    }
                    else {
                        // =-=-=-=-=-=-=-
                        // cache file descriptor in out-variable
                        fco->file_descriptor( fd );
                        result.code( fd );
                    }
                }
            }
        }
        // =-=-=-=-=-=-=-
        // declare victory!
        return result;

    } // shareuf_create_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Open
    irods::error shareuf_open_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            char* kvp_str = getValByKey(
                                &fco->cond_input(),
                                KEY_VALUE_PASSTHROUGH_KW );
            if ( kvp_str ) {
                irods::kvp_map_t kvp;
                ret = irods::parse_kvp_string(
                          kvp_str,
                          kvp );
                if ( !ret.ok() ) {
                    irods::log( PASS( ret ) );
                }
                else {
                    irods::kvp_map_t::iterator itr = kvp.begin();
                    for ( ; itr != kvp.end(); ++ itr ) {
                        rodsLog(
                            LOG_DEBUG,
                            "shareuf_open_plugin - kv_pass :: key [%s] - value [%s]",
                            itr->first.c_str(),
                            itr->second.c_str() );
                    } // for itr
                }
            }

            // =-=-=-=-=-=-=-
            // handle OSX weirdness...
            int flags = fco->flags();

#if defined(osx_platform)
            // For osx, O_TRUNC = 0x0400, O_TRUNC = 0x200 for other system
            if ( flags & 0x200 ) {
                flags = flags ^ 0x200;
                flags = flags | O_TRUNC;
            }
#endif
            // =-=-=-=-=-=-=-
            // make call to open
            errno = 0;
            int fd = open( fco->physical_path().c_str(), flags, fco->mode() );
            int errsav = errno;

            // =-=-=-=-=-=-=-
            // if we got a 0 descriptor, try again
            if ( fd == 0 ) {
                close( fd );
                int null_fd = open( "/dev/null", O_RDWR, 0 );
                fd = open( fco->physical_path().c_str(), flags, fco->mode() );
                errsav = errno;
                if ( null_fd >= 0 ) {
                    close( null_fd );
                }
                rodsLog( LOG_NOTICE, "shareuf_open_plugin: 0 descriptor" );
            }

            // =-=-=-=-=-=-=-
            // trap error case with bad fd
            if ( fd < 0 ) {
                int status = UNIX_FILE_OPEN_ERR - errsav;
                std::stringstream msg;
                msg << "Open error for \"";
                msg << fco->physical_path();
                msg << "\", errno = \"";
                msg << strerror( errsav );
                msg << "\", status = \"";
                msg << status;
                msg << "\", flags = \"";
                msg << flags;
                msg << "\".";
                result = ERROR( status, msg.str() );
            }
            else {
                // =-=-=-=-=-=-=-
                // cache status in the file object
                fco->file_descriptor( fd );
                result.code( fd );
            }
        }

        // =-=-=-=-=-=-=-
        // declare victory!
        return result;

    } // shareuf_open_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Read
    irods::error shareuf_read_plugin(
        irods::resource_plugin_context& _ctx,
        void*                               _buf,
        int                                 _len ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to read
            int status = read( fco->file_descriptor(), _buf, _len );

            // =-=-=-=-=-=-=-
            // pass along an error if it was not successful
            int err_status = UNIX_FILE_READ_ERR - errno;
            if ( !( result = ASSERT_ERROR( status >= 0, err_status, "Read error for file: \"%s\", errno = \"%s\".",
                                           fco->physical_path().c_str(), strerror( errno ) ) ).ok() ) {
                result.code( err_status );
            }
            else {
                result.code( status );
            }
        }

        // =-=-=-=-=-=-=-
        // win!
        return result;

    } // shareuf_read_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Write
    irods::error shareuf_write_plugin(
        irods::resource_plugin_context& _ctx,
        void*                               _buf,
        int                                 _len ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to write
            int status = write( fco->file_descriptor(), _buf, _len );

            // =-=-=-=-=-=-=-
            // pass along an error if it was not successful
            int err_status = UNIX_FILE_WRITE_ERR - errno;
            if ( !( result = ASSERT_ERROR( status >= 0, err_status, "Write file: \"%s\", errno = \"%s\", status = %d.",
                                           fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {
                result.code( err_status );
            }
            else {
                result.code( status );
            }
        }

        // =-=-=-=-=-=-=-
        // win!
        return result;

    } // shareuf_write_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Close
    irods::error shareuf_close_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to close
            int status = close( fco->file_descriptor() );

            // =-=-=-=-=-=-=-
            // log any error
            int err_status = UNIX_FILE_CLOSE_ERR - errno;
            if ( !( result = ASSERT_ERROR( status >= 0, err_status, "Close error for file: \"%s\", errno = \"%s\", status = %d.",
                                           fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {
                result.code( err_status );
            }
            else {
                result.code( status );
            }
        }

        return result;

    } // shareuf_close_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Unlink
    irods::error shareuf_unlink_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::data_object_ptr fco = boost::dynamic_pointer_cast< irods::data_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to unlink
            int status = unlink( fco->physical_path().c_str() );

            // =-=-=-=-=-=-=-
            // error handling
            int err_status = UNIX_FILE_UNLINK_ERR - errno;
            if ( !( result = ASSERT_ERROR( status >= 0, err_status, "Unlink error for \"%s\", errno = \"%s\", status = %d.",
                                           fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {

                result.code( err_status );
            }
            else {
                result.code( status );
            }
        }

        return result;

    } // shareuf_unlink_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Stat
    irods::error shareuf_stat_plugin(
        irods::resource_plugin_context& _ctx,
        struct stat*                        _statbuf ) {
        irods::error result = SUCCESS();
        // =-=-=-=-=-=-=-
        // NOTE:: this function assumes the object's physical path is
        //        correct and should not have the vault path
        //        prepended - hcj

        irods::error ret = _ctx.valid();
        if ( ( result = ASSERT_PASS( ret, "resource context is invalid." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::data_object_ptr fco = boost::dynamic_pointer_cast< irods::data_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to stat
            int status = stat( fco->physical_path().c_str(), _statbuf );

            // =-=-=-=-=-=-=-
            // return an error if necessary
            int err_status = UNIX_FILE_STAT_ERR - errno;
            if ( ( result = ASSERT_ERROR( status >= 0, err_status, "Stat error for \"%s\", errno = \"%s\", status = %d.",
                                          fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {
                result.code( status );
            }
        }

        return result;

    } // shareuf_stat_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX lseek
    irods::error shareuf_lseek_plugin(
        irods::resource_plugin_context& _ctx,
        long long                           _offset,
        int                                 _whence ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // get ref to fco
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to lseek
            long long status = lseek( fco->file_descriptor(),  _offset, _whence );

            // =-=-=-=-=-=-=-
            // return an error if necessary
            long long err_status = UNIX_FILE_LSEEK_ERR - errno;
            if ( ( result = ASSERT_ERROR( status >= 0, err_status, "Lseek error for \"%s\", errno = \"%s\", status = %ld.",
                                          fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {
                result.code( status );
            }
        }

        return result;

    } // shareuf_lseek_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX mkdir
    irods::error shareuf_mkdir_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // NOTE :: this function assumes the object's physical path is correct and
        //         should not have the vault path prepended - hcj

        irods::error ret = _ctx.valid< irods::collection_object >();
        if ( ( result = ASSERT_PASS( ret, "resource context is invalid." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the chain to our understood object type
            irods::collection_object_ptr fco = boost::dynamic_pointer_cast< irods::collection_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to mkdir & umask
            mode_t myMask = umask( ( mode_t ) 0000 );
            int    status = mkdir( fco->physical_path().c_str(), 0755 );

            // =-=-=-=-=-=-=-
            // reset the old mask
            umask( ( mode_t ) myMask );

            // =-=-=-=-=-=-=-
            // return an error if necessary
            result.code( status );
            int err_status = UNIX_FILE_MKDIR_ERR - errno;
            if ( ( result = ASSERT_ERROR( status >= 0, err_status, "Mkdir error for \"%s\", errno = \"%s\", status = %d.",
                                          fco->physical_path().c_str(), strerror( errno ), err_status ) ).ok() ) {
                result.code( status );
            }
        }
        return result;

    } // shareuf_mkdir_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX rmdir
    irods::error shareuf_rmdir_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the chain to our understood object type
            irods::collection_object_ptr fco = boost::dynamic_pointer_cast< irods::collection_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to rmdir
            int status = rmdir( fco->physical_path().c_str() );

            // =-=-=-=-=-=-=-
            // return an error if necessary
            int err_status = UNIX_FILE_RMDIR_ERR - errno;
            result = ASSERT_ERROR( status >= 0, err_status, "Rmdir error for \"%s\", errno = \"%s\", status = %d.",
                                   fco->physical_path().c_str(), strerror( errno ), err_status );
        }

        return result;

    } // shareuf_rmdir_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX opendir
    irods::error shareuf_opendir_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path< irods::collection_object >( _ctx );
        if ( !ret.ok() ) {
            return PASSMSG( "Invalid parameters or physical path.", ret );
        }

        // =-=-=-=-=-=-=-
        // cast down the chain to our understood object type
        irods::collection_object_ptr fco = boost::dynamic_pointer_cast< irods::collection_object >( _ctx.fco() );

        // =-=-=-=-=-=-=-
        // make the call to opendir
        DIR* dir_ptr = opendir( fco->physical_path().c_str() );
        int errsav = errno;

        // =-=-=-=-=-=-=-
        // trap error case with bad fd
        if ( NULL == dir_ptr ) {
            int status = UNIX_FILE_CREATE_ERR - errsav;
            std::stringstream msg;
            msg << "Open error for \"";
            msg << fco->physical_path();
            msg << "\", errno = \"";
            msg << strerror( errsav );
            msg << "\", status = \"";
            msg << status;
            msg << "\".";
            result = ERROR( status, msg.str() );
        }
        else {
            // =-=-=-=-=-=-=-
            // cache dir_ptr in the out-variable
            fco->directory_pointer( dir_ptr );
        }


        return result;

    } // shareuf_opendir_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX closedir
    irods::error shareuf_closedir_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path< irods::collection_object >( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the chain to our understood object type
            irods::collection_object_ptr fco = boost::dynamic_pointer_cast< irods::collection_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the callt to opendir
            int status = closedir( fco->directory_pointer() );

            // =-=-=-=-=-=-=-
            // return an error if necessary
            int err_status = UNIX_FILE_CLOSEDIR_ERR - errno;
            result = ASSERT_ERROR( status >= 0, err_status, "Closedir error for \"%s\", errno = \"%s\", status = %d.",
                                   fco->physical_path().c_str(), strerror( errno ), err_status );
        }

        return result;

    } // shareuf_closedir_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX readdir
    irods::error shareuf_readdir_plugin(
        irods::resource_plugin_context& _ctx,
        struct rodsDirent**                 _dirent_ptr ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path< irods::collection_object >( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the chain to our understood object type
            irods::collection_object_ptr fco = boost::dynamic_pointer_cast< irods::collection_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // zero out errno?
            errno = 0;

            // =-=-=-=-=-=-=-
            // make the call to readdir
            struct dirent * tmp_dirent = readdir( fco->directory_pointer() );

            // =-=-=-=-=-=-=-
            // handle error cases
            if ( ( result = ASSERT_ERROR( tmp_dirent != NULL, -1, "End of directory list reached." ) ).ok() ) {

                // =-=-=-=-=-=-=-
                // alloc dirent as necessary
                if ( !( *_dirent_ptr ) ) {
                    ( *_dirent_ptr ) = ( rodsDirent_t* ) malloc( sizeof( rodsDirent_t ) );
                }

                // =-=-=-=-=-=-=-
                // convert standard dirent to rods dirent struct
                int status = direntToRodsDirent( ( *_dirent_ptr ), tmp_dirent );
                if ( status < 0 ) {
                    irods::log( ERROR( status, "direntToRodsDirent failed." ) );
                }


#if defined(solaris_platform)
                rstrcpy( ( *_dirent_ptr )->d_name, tmp_dirent->d_name, MAX_NAME_LEN );
#endif
            }
            else {
                // =-=-=-=-=-=-=-
                // cache status in out variable
                int status = UNIX_FILE_READDIR_ERR - errno;
                if ( ( result = ASSERT_ERROR( errno == 0, status, "Readdir error, status = %d, errno= \"%s\".",
                                              status, strerror( errno ) ) ).ok() ) {
                    result.code( -1 );
                }
            }
        }

        return result;

    } // shareuf_readdir_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX readdir
    irods::error shareuf_rename_plugin(
        irods::resource_plugin_context& _ctx,
        const char*                         _new_file_name ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // manufacture a new path from the new file name
            std::string new_full_path;
            ret = unix_generate_full_path( _ctx.prop_map(), _new_file_name, new_full_path );
            if ( ( result = ASSERT_PASS( ret, "Unable to generate full path for destination file: \"%s\".",
                                         _new_file_name ) ).ok() ) {

                // =-=-=-=-=-=-=-
                // cast down the hierarchy to the desired object
                irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

                // =-=-=-=-=-=-=-
                // get the default directory mode
                mode_t mode = 0755;
                ret = _ctx.prop_map().get<mode_t>(
                          DEFAULT_VAULT_DIR_MODE,
                          mode );
                if ( !ret.ok() ) {
                    return PASS( ret );

                }

                // =-=-=-=-=-=-=-
                // make the directories in the path to the new file
                std::string new_path = new_full_path;
                std::size_t last_slash = new_path.find_last_of( '/' );
                new_path.erase( last_slash );
                ret = shareuf_mkdir_r( new_path.c_str(), 0755 );
                if ( ( result = ASSERT_PASS( ret, "Mkdir error for \"%s\".", new_path.c_str() ) ).ok() ) {

                }

                // =-=-=-=-=-=-=-
                // make the call to rename
                int status = rename( fco->physical_path().c_str(), new_full_path.c_str() );

                // =-=-=-=-=-=-=-
                // handle error cases
                int err_status = UNIX_FILE_RENAME_ERR - errno;
                if ( ( result = ASSERT_ERROR( status >= 0, err_status, "Rename error for \"%s\" to \"%s\", errno = \"%s\", status = %d.",
                                              fco->physical_path().c_str(), new_full_path.c_str(), strerror( errno ), err_status ) ).ok() ) {
                    result.code( status );
                }
            }
        }

        return result;

    } // shareuf_rename_plugin

    // =-=-=-=-=-=-=-
    // interface for POSIX truncate
    irods::error shareuf_truncate_plugin(
        irods::resource_plugin_context& _ctx ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path< irods::file_object >( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the chain to our understood object type
            irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            // =-=-=-=-=-=-=-
            // make the call to rename
            int status = truncate( file_obj->physical_path().c_str(), file_obj->size() );

            // =-=-=-=-=-=-=-
            // handle any error cases
            int err_status = UNIX_FILE_TRUNCATE_ERR - errno;
            result = ASSERT_ERROR( status >= 0, err_status, "Truncate error for: \"%s\", errno = \"%s\", status = %d.",
                                   file_obj->physical_path().c_str(), strerror( errno ), err_status );
        }

        return result;

    } // shareuf_truncate_plugin


    // =-=-=-=-=-=-=-
    // unixStageToCache - This routine is for testing the TEST_STAGE_FILE_TYPE.
    // Just copy the file from filename to cacheFilename. optionalInfo info
    // is not used.
    irods::error shareuf_stagetocache_plugin(
        irods::resource_plugin_context& _ctx,
        const char*                      _cache_file_name ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the hierarchy to the desired object
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            ret = shareuf_copy_plugin( fco->mode(), fco->physical_path().c_str(), _cache_file_name );
            result = ASSERT_PASS( ret, "Failed" );
        }
        return result;
    } // shareuf_stagetocache_plugin

    // =-=-=-=-=-=-=-
    // unixSyncToArch - This routine is for testing the TEST_STAGE_FILE_TYPE.
    // Just copy the file from cacheFilename to filename. optionalInfo info
    // is not used.
    irods::error shareuf_synctoarch_plugin(
        irods::resource_plugin_context& _ctx,
        char*                            _cache_file_name ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // Check the operation parameters and update the physical path
        irods::error ret = unix_check_params_and_path( _ctx );
        if ( ( result = ASSERT_PASS( ret, "Invalid parameters or physical path." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // cast down the hierarchy to the desired object
            irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

            ret = shareuf_copy_plugin( fco->mode(), _cache_file_name, fco->physical_path().c_str() );
            result = ASSERT_PASS( ret, "Failed" );
        }

        return result;

    } // shareuf_synctoarch_plugin

    // =-=-=-=-=-=-=-
    // redirect_create - code to determine redirection for create operation
    irods::error shareuf_redirect_create(
        irods::resource_plugin_context& _ctx,
        const std::string&              _resc_name,
        const std::string&              _curr_host,
        float&                          _out_vote ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // determine if the resource is down
        int resc_status = 0;
        irods::error get_ret = _ctx.prop_map().get< int >( irods::RESOURCE_STATUS, resc_status );
        if ( ( result = ASSERT_PASS( get_ret, "Failed to get \"status\" property." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // if the status is down, vote no.
            if ( INT_RESC_STATUS_DOWN == resc_status ) {
                _out_vote = 0.0;
                result.code( SYS_RESC_IS_DOWN );
                // result = PASS( result );
            }
            else {
                // vote no if the file size exceeds the high water mark
                irods::file_object_ptr fco = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );
                rodsLong_t file_size = fco->size();
                if( replica_exceeds_high_water_mark( _ctx, file_size ) ) {
                    _out_vote = 0.0;
                    return SUCCESS();
                }

                // =-=-=-=-=-=-=-
                // get the resource host for comparison to curr host
                std::string host_name;
                get_ret = _ctx.prop_map().get< std::string >( irods::RESOURCE_LOCATION, host_name );
                if ( ( result = ASSERT_PASS( get_ret, "Failed to get \"location\" property." ) ).ok() ) {

                    // =-=-=-=-=-=-=-
                    // vote higher if we are on the same host
                    if ( _curr_host == host_name ) {
                        _out_vote = 1.0;
                    }
                    else {
                        _out_vote = 0.5;
                    }
                }

                rodsLog(
                    LOG_DEBUG,
                    "create :: resc name [%s] curr host [%s] resc host [%s] vote [%f]",
                    _resc_name.c_str(),
                    _curr_host.c_str(),
                    host_name.c_str(),
                    _out_vote );

            }
        }
        return result;

    } // shareuf_redirect_create

    // =-=-=-=-=-=-=-
    // redirect_open - code to determine redirection for open operation
    irods::error shareuf_redirect_open(
        irods::plugin_property_map&   _prop_map,
        irods::file_object_ptr        _file_obj,
        const std::string&             _resc_name,
        const std::string&             _curr_host,
        float&                         _out_vote ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // initially set a good default
        _out_vote = 0.0;

        // =-=-=-=-=-=-=-
        // determine if the resource is down
        int resc_status = 0;
        irods::error get_ret = _prop_map.get< int >( irods::RESOURCE_STATUS, resc_status );
        if ( ( result = ASSERT_PASS( get_ret, "Failed to get \"status\" property." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // if the status is down, vote no.
            if ( INT_RESC_STATUS_DOWN != resc_status ) {

                // =-=-=-=-=-=-=-
                // get the resource host for comparison to curr host
                std::string host_name;
                get_ret = _prop_map.get< std::string >( irods::RESOURCE_LOCATION, host_name );
                if ( ( result = ASSERT_PASS( get_ret, "Failed to get \"location\" property." ) ).ok() ) {

                    // =-=-=-=-=-=-=-
                    // set a flag to test if were at the curr host, if so we vote higher
                    bool curr_host = ( _curr_host == host_name );

                    // =-=-=-=-=-=-=-
                    // make some flags to clarify decision making
                    bool need_repl = ( _file_obj->repl_requested() > -1 );

                    // =-=-=-=-=-=-=-
                    // set up variables for iteration
                    irods::error final_ret = SUCCESS();
                    std::vector< irods::physical_object > objs = _file_obj->replicas();
                    std::vector< irods::physical_object >::iterator itr = objs.begin();

                    // =-=-=-=-=-=-=-
                    // check to see if the replica is in this resource, if one is requested
                    for ( ; itr != objs.end(); ++itr ) {
                        // =-=-=-=-=-=-=-
                        // run the hier string through the parser and get the last
                        // entry.
                        std::string last_resc;
                        irods::hierarchy_parser parser;
                        parser.set_string( itr->resc_hier() );
                        parser.last_resc( last_resc );

                        // =-=-=-=-=-=-=-
                        // more flags to simplify decision making
                        bool repl_us  = ( _file_obj->repl_requested() == itr->repl_num() );
                        bool resc_us  = ( _resc_name == last_resc );
                        bool is_dirty = ( itr->is_dirty() != 1 );

                        // =-=-=-=-=-=-=-
                        // success - correct resource and don't need a specific
                        //           replication, or the repl nums match
                        if ( resc_us ) {
                            // =-=-=-=-=-=-=-
                            // if a specific replica is requested then we
                            // ignore all other criteria
                            if ( need_repl ) {
                                if ( repl_us ) {
                                    _out_vote = 1.0;
                                }
                                else {
                                    // =-=-=-=-=-=-=-
                                    // repl requested and we are not it, vote
                                    // very low
                                    _out_vote = 0.25;
                                }
                            }
                            else {
                                // =-=-=-=-=-=-=-
                                // if no repl is requested consider dirty flag
                                if ( is_dirty ) {
                                    // =-=-=-=-=-=-=-
                                    // repl is dirty, vote very low
                                    _out_vote = 0.25;
                                }
                                else {
                                    // =-=-=-=-=-=-=-
                                    // if our repl is not dirty then a local copy
                                    // wins, otherwise vote middle of the road
                                    if ( curr_host ) {
                                        _out_vote = 1.0;
                                    }
                                    else {
                                        _out_vote = 0.5;
                                    }
                                }
                            }

                            rodsLog(
                                LOG_DEBUG,
                                "open :: resc name [%s] curr host [%s] resc host [%s] vote [%f]",
                                _resc_name.c_str(),
                                _curr_host.c_str(),
                                host_name.c_str(),
                                _out_vote );

                            break;

                        } // if resc_us

                    } // for itr
                }
            }
            else {
                result.code( SYS_RESC_IS_DOWN );
                result = PASS( result );
            }
        }

        return result;

    } // shareuf_redirect_open

    // =-=-=-=-=-=-=-
    // used to allow the resource to determine which host
    // should provide the requested operation
    irods::error shareuf_redirect_plugin(
        irods::resource_plugin_context& _ctx,
        const std::string*                  _opr,
        const std::string*                  _curr_host,
        irods::hierarchy_parser*           _out_parser,
        float*                              _out_vote ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // check the context validity
        irods::error ret = _ctx.valid< irods::file_object >();
        if ( ( result = ASSERT_PASS( ret, "Invalid resource context." ) ).ok() ) {

            // =-=-=-=-=-=-=-
            // check incoming parameters
            if ( ( result = ASSERT_ERROR( _opr && _curr_host && _out_parser && _out_vote, SYS_INVALID_INPUT_PARAM, "Invalid input parameter." ) ).ok() ) {
                // =-=-=-=-=-=-=-
                // cast down the chain to our understood object type
                irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

                // =-=-=-=-=-=-=-
                // get the name of this resource
                std::string resc_name;
                ret = _ctx.prop_map().get< std::string >( irods::RESOURCE_NAME, resc_name );
                if ( ( result = ASSERT_PASS( ret, "Failed in get property for name." ) ).ok() ) {

                    // =-=-=-=-=-=-=-
                    // test the operation to determine which choices to make
                    if ( irods::OPEN_OPERATION  == ( *_opr ) ||
                            irods::WRITE_OPERATION == ( *_opr ) ||
                            irods::UNLINK_OPERATION == ( *_opr )) {
                        // =-=-=-=-=-=-=-
                        // call redirect determination for 'get' operation
                        ret = shareuf_redirect_open( _ctx.prop_map(), file_obj, resc_name, ( *_curr_host ), ( *_out_vote ) );
                        result = ASSERT_PASS( ret, "Failed redirecting for open." );

                    }
                    else if ( irods::CREATE_OPERATION == ( *_opr ) ) {
                        // =-=-=-=-=-=-=-
                        // call redirect determination for 'create' operation
                        ret = shareuf_redirect_create( _ctx, resc_name, ( *_curr_host ), ( *_out_vote ) );
                        result = ASSERT_PASS( ret, "Failed redirecting for create." );
                    }

                    else {
                        // =-=-=-=-=-=-=-
                        // must have been passed a bad operation
                        result = ASSERT_ERROR( false, INVALID_OPERATION, "Operation not supported." );
                    }
                }

                // add ourselves to the hierarchy if we have any vote
                if( *_out_vote > 0 && result.ok() ) {
                    _out_parser->add_child( resc_name );
                }
            }
        }

        return result;

    } // shareuf_redirect_plugin

    // =-=-=-=-=-=-=-
    // shareuf_rebalance - code which would rebalance the subtree
    irods::error shareuf_rebalance(
        irods::resource_plugin_context& _ctx ) {
        return update_resource_object_count(
                   _ctx.comm(),
                   _ctx.prop_map() );

    } // shareuf_rebalancec

    // =-=-=-=-=-=-=-
    // 3. create derived class to handle unix file system resources
    //    necessary to do custom parsing of the context string to place
    //    any useful values into the property map for reference in later
    //    operations.  semicolon is the preferred delimiter
    class shareuf_resource : public irods::resource {
            // =-=-=-=-=-=-=-
            // 3a. create a class to provide maintenance operations, this is only for example
            //     and will not be called.
            class maintenance_operation {
                public:
                    maintenance_operation( const std::string& _n ) : name_( _n ) {
                    }

                    maintenance_operation( const maintenance_operation& _rhs ) {
                        name_ = _rhs.name_;
                    }

                    maintenance_operation& operator=( const maintenance_operation& _rhs ) {
                        name_ = _rhs.name_;
                        return *this;
                    }

                    irods::error operator()( rcComm_t* ) {
                        rodsLog( LOG_NOTICE, "shareuf_resource::post_disconnect_maintenance_operation - [%s]",
                                 name_.c_str() );
                        return SUCCESS();
                    }

                private:
                    std::string name_;

            }; // class maintenance_operation

        public:
            shareuf_resource(
                const std::string& _inst_name,
                const std::string& _context ) :
                irods::resource(
                    _inst_name,
                    _context ) {
                properties_.set<mode_t>( DEFAULT_VAULT_DIR_MODE, 0750 );
                // =-=-=-=-=-=-=-
                // parse context string into property pairs assuming a ; as a separator
                std::vector< std::string > props;
                irods::kvp_map_t kvp;
                irods::parse_kvp_string(
                    _context,
                    kvp );

                // =-=-=-=-=-=-=-
                // copy the properties from the context to the prop map
                irods::kvp_map_t::iterator itr = kvp.begin();
                for( ; itr != kvp.end(); ++itr ) {
                    properties_.set< std::string >(
                        itr->first,
                        itr->second );
                } // for itr

            } // ctor

            irods::error need_post_disconnect_maintenance_operation( bool& _b ) {
                _b = false;
                return SUCCESS();
            }

            // =-=-=-=-=-=-=-
            // 3b. pass along a functor for maintenance work after
            //     the client disconnects, uncomment the first two lines for effect.
            irods::error post_disconnect_maintenance_operation( irods::pdmo_type& ) {
                irods::error result = SUCCESS();
                return ERROR( -1, "nop" );
            }
    }; // class shareuf_resource

    // =-=-=-=-=-=-=-
    // 4. create the plugin factory function which will return a dynamically
    //    instantiated object of the previously defined derived resource.  use
    //    the add_operation member to associate a 'call name' to the interfaces
    //    defined above.  for resource plugins these call names are standardized
    //    as used by the irods facing interface defined in
    //    server/drivers/src/fileDriver.c
    irods::resource* plugin_factory( const std::string& _inst_name, const std::string& _context ) {

        // =-=-=-=-=-=-=-
        // 4a. create shareuf_resource
        shareuf_resource* resc = new shareuf_resource( _inst_name, _context );

        // =-=-=-=-=-=-=-
        // 4b. map function names to operations.  this map will be used to load
        //     the symbols from the shared object in the delay_load stage of
        //     plugin loading.
        resc->add_operation( irods::RESOURCE_OP_CREATE,       "shareuf_create_plugin" );
        resc->add_operation( irods::RESOURCE_OP_OPEN,         "shareuf_open_plugin" );
        resc->add_operation( irods::RESOURCE_OP_READ,         "shareuf_read_plugin" );
        resc->add_operation( irods::RESOURCE_OP_WRITE,        "shareuf_write_plugin" );
        resc->add_operation( irods::RESOURCE_OP_CLOSE,        "shareuf_close_plugin" );
        resc->add_operation( irods::RESOURCE_OP_UNLINK,       "shareuf_unlink_plugin" );
        resc->add_operation( irods::RESOURCE_OP_STAT,         "shareuf_stat_plugin" );
        resc->add_operation( irods::RESOURCE_OP_LSEEK,        "shareuf_lseek_plugin" );
        resc->add_operation( irods::RESOURCE_OP_MKDIR,        "shareuf_mkdir_plugin" );
        resc->add_operation( irods::RESOURCE_OP_RMDIR,        "shareuf_rmdir_plugin" );
        resc->add_operation( irods::RESOURCE_OP_OPENDIR,      "shareuf_opendir_plugin" );
        resc->add_operation( irods::RESOURCE_OP_CLOSEDIR,     "shareuf_closedir_plugin" );
        resc->add_operation( irods::RESOURCE_OP_READDIR,      "shareuf_readdir_plugin" );
        resc->add_operation( irods::RESOURCE_OP_RENAME,       "shareuf_rename_plugin" );
        resc->add_operation( irods::RESOURCE_OP_TRUNCATE,     "shareuf_truncate_plugin" );
        resc->add_operation( irods::RESOURCE_OP_FREESPACE,    "shareuf_get_fsfreespace_plugin" );
        resc->add_operation( irods::RESOURCE_OP_STAGETOCACHE, "shareuf_stagetocache_plugin" );
        resc->add_operation( irods::RESOURCE_OP_SYNCTOARCH,   "shareuf_synctoarch_plugin" );
        resc->add_operation( irods::RESOURCE_OP_REGISTERED,   "shareuf_registered_plugin" );
        resc->add_operation( irods::RESOURCE_OP_UNREGISTERED, "shareuf_unregistered_plugin" );
        resc->add_operation( irods::RESOURCE_OP_MODIFIED,     "shareuf_modified_plugin" );
        resc->add_operation( irods::RESOURCE_OP_NOTIFY,       "shareuf_notify_plugin" );

        resc->add_operation( irods::RESOURCE_OP_RESOLVE_RESC_HIER,     "shareuf_redirect_plugin" );
        resc->add_operation( irods::RESOURCE_OP_REBALANCE,             "shareuf_rebalance" );

        // =-=-=-=-=-=-=-
        // set some properties necessary for backporting to iRODS legacy code
        resc->set_property< int >( irods::RESOURCE_CHECK_PATH_PERM, 2 );//DO_CHK_PATH_PERM );
        resc->set_property< int >( irods::RESOURCE_CREATE_PATH,     1 );//CREATE_PATH );

        // =-=-=-=-=-=-=-
        // 4c. return the pointer through the generic interface of an
        //     irods::resource pointer
        return dynamic_cast<irods::resource*>( resc );

    } // plugin_factory

}; // extern "C"