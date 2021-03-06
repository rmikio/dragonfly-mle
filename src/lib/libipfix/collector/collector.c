/*
$$LIC$$
 */
/*
 *    dragonfly-collector.c - ipfix collector.  Desgined to work with the Machine Learning Engine (MLE)
 *
 *    $Revision: 1.12 $
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <libgen.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netdb.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <ipfix.h>

#include "ipfix_mle.h"

#include "ipfix_col.h"
#include "ipfix_def_fokus.h"
#include "ipfix_fields_fokus.h"

#include "mlog.h"
#include "mpoll.h"

/*------ defines ---------------------------------------------------------*/

#define CAFILE          "rootcert.pem"
#define CADIR           NULL
#define KEYFILE         "server.pem"
#define CERTFILE        "server.pem"

/*------ stuctures -------------------------------------------------------*/

typedef struct ipfix_collector_opts
{
    char           progname[30];
    int            debug;        /* some debug output */
    char           *logfile;
    char	       *datadir;
    char           *zout;

    int            udp;          /* support udp clients  */
    int            tcp;          /* support tcp packets  */
    int            sctp;         /* support sctp clients */
    int            ssl;          /* ipfix over TLS/SSL   */
    char           *cafile;
    char           *cadir;
    char           *keyfile;     /* private key */
    char           *certfile;    /* certificate */
    int            port;         /* port number */
    int            maxcon;       /* backlog parameter for listen(2) */
    int            family;       /* AF_UNSPEC, _INET, _INET6 */

} ipfix_col_opts_t;

/*------ globals ---------------------------------------------------------*/

ipfix_col_opts_t  par;
int               verbose_level = 0;
int               *tcp_s=NULL, ntcp_s=0;       /* socket */
int               *udp_s=NULL, nudp_s=0;
int               *sctp_s=NULL, nsctp_s=0;
ipfix_col_t       *scol=NULL;

/*------ prototypes ------------------------------------------------------*/

/*------ static funcs ----------------------------------------------------*/

static void usage( char *taskname)
{
    const char helptxt[] =
        "[options]\n\n"
        "options:\n"
        "  -h                  this help\n"
        "  -4                  accept connections via AF_INET socket\n"
        "  -6                  accept connections via AF_INET6 socket\n"
        "  -o <datadir>        store files of collected data in this dir\n"
        "  -p <portno>         listen on this port (default=4739)\n"
        "  -s                  support SCTP clients\n"
        "  -t                  support TCP clients\n"
        "  -u                  support UDP clients\n"
        "  -v                  increase verbose level\n"
        "  -z                  gzip/json output to stdout\n"
#ifdef SSLSUPPORT
        "ssl options:\n"
        "  --ssl                expect tls/ssl clients\n"
        "  --key    <file>      private key file to use\n"
        "  --cert   <file>      certificate file to use\n"
        "  --cafile <file>      file of CAs\n"
        "  --cadir  <dir>       directory of CAs\n"
#endif
        "\n";

    fprintf( stderr, "\nipfix collector (%s %s)\n",
             "$Revision: 1.12 $", __DATE__ );

    fprintf( stderr,"\nusage: %s %sexample: %s -stu -vv -o . \n\n",
             taskname, helptxt, taskname );

}/*usage*/

void exit_func ( int retval )
{
    int i;

    if ( par.tcp && tcp_s ) {
        for( i=0; i<ntcp_s; i++ ) {
            ipfix_col_close( tcp_s[i] );
        }
        free( tcp_s );
    }
    if ( par.udp && udp_s ) {
        for( i=0; i<nudp_s; i++ ) {
            ipfix_col_close( udp_s[i] );
        }
        free( udp_s );
    }
    if ( par.sctp && sctp_s ) {
        for( i=0; i<nsctp_s; i++ ) {
            ipfix_col_close( sctp_s[i] );
        }
        free( sctp_s );
    }
    if ( par.ssl && scol ) {
        ipfix_col_close_ssl( scol );
    }

    ipfix_stop_mle();
    
    ipfix_col_cleanup();
    ipfix_cleanup();
    mlog_close();
    exit( retval );
}

void sig_func( int signo )
{
    if ( verbose_level )
        fprintf( stderr, "\n[%s] got signo %d, bye.\n\n", par.progname, signo );

    exit_func( 1 );
}

int do_collect()
{
    int      i, retval = -1;
    ipfix_ssl_opts_t opts;

    if ( par.ssl ) {
        opts.cafile  = par.cafile;
        opts.cadir   = par.cadir;
        opts.keyfile = par.keyfile;
        opts.certfile= par.certfile;
    }

    /** activate file export
     */
    if ( par.datadir )
        (void) ipfix_col_init_fileexport( par.datadir );

    if ( par.zout ) {
        if (ipfix_start_zout (par.zout) < 0) {
            mlogf( 0, "[%s] initialize zout %s\n", par.progname, par.zout);
            return -1;
        }
    }

    /** open ipfix collector port(s)
     */
    if ( par.tcp ) {
        if ( par.ssl ) {
            if ( ipfix_col_listen_ssl( &scol, IPFIX_PROTO_TCP, 
                                       par.port, par.family, par.maxcon,
                                       &opts ) <0 ) {
                fprintf( stderr, "ipfix_listen_ssl(tcp) failed: %s\n",
                         strerror(errno) );
                return -1;
            }
        }
        else if ( ipfix_col_listen( &ntcp_s, &tcp_s, IPFIX_PROTO_TCP, 
                               par.port, par.family, par.maxcon ) <0 ) {
            fprintf( stderr, "ipfix_listen(tcp) failed: %s\n",
                     strerror(errno) );
            return -1;
        }
    }

    if ( par.udp ) {
        if ( par.ssl ) {
            if ( ipfix_col_listen_ssl( &scol, IPFIX_PROTO_UDP,
                                       par.port, par.family, 0, &opts ) <0 ) {
                fprintf( stderr, "ipfix_listen_ssl(udp) failed: %s\n",
                         strerror(errno) );
                return -1;
            }
        }
        else if ( ipfix_col_listen( &nudp_s, &udp_s, IPFIX_PROTO_UDP,
                                    par.port, par.family, 0 ) <0 ) {
            fprintf( stderr, "ipfix_listen(udp) failed: %s\n",
                     strerror(errno) );
            goto end;
        }
    }

    if ( par.sctp ) {
        if ( ipfix_col_listen( &nsctp_s, &sctp_s, IPFIX_PROTO_SCTP,
                               par.port, par.family, par.maxcon ) <0 ) {
            fprintf( stderr, "ipfix_listen(sctp) failed: %s\n",
                     strerror(errno) );
            goto end;
        }
    }

    /** event loop
     */
    for (;;)
    {
        if ( mpoll_loop( 10 ) <0 )
            break;

    } /* forever */

 end:
    if ( par.tcp && tcp_s ) {
        for( i=0; i<ntcp_s; i++ )
            ipfix_col_close( tcp_s[i] );
    }
    if ( par.udp && udp_s ) {
        for( i=0; i<nudp_s; i++ )
            ipfix_col_close( udp_s[i] );
    }
    if ( par.sctp && sctp_s ) {
        for( i=0; i<nsctp_s; i++ )
            ipfix_col_close( sctp_s[i] );
    }
    return retval;
}

int main (int argc, char *argv[])
{
    char          arg;          /* short options: character */
    int           loptidx=0;    /* long options: arg==0 and index */
    char          opt[] = "64stuhl:p:vo:z:";
#ifdef HAVE_GETOPT_LONG
    struct option lopt[] = { 
        { "ssl", 0, 0, 0},
        { "key", 1, 0, 0},
        { "cert", 1, 0, 0},
        { "cafile", 1, 0, 0},
        { "cadir", 1, 0, 0},
        { "help", 0, 0, 0},
        { 0, 0, 0, 0 } 
    };
#endif

    /** set default options
     */
    par.tcp     = 0;
    par.udp     = 0;
    par.sctp    = 0;
    par.ssl     = 0;
    par.cafile  = CAFILE;
    par.cadir   = CADIR;
    par.keyfile = KEYFILE;
    par.certfile= CERTFILE;
    par.port    = 0;
    par.family  = AF_UNSPEC;
    par.logfile = NULL;
    par.zout = 0;
    par.maxcon  = 10;
    par.datadir  = NULL;

    snprintf( par.progname, sizeof(par.progname), "%s", basename( argv[0]) );

    /* --- command line parsing ---
     */
#ifdef HAVE_GETOPT_LONG
    while ((arg=getopt_long( argc, argv, opt, lopt, &loptidx)) >=0 )
#else
    while( (arg=getopt( argc, argv, opt )) != EOF )
#endif
    {
        switch (arg) 
        {
          case 0: 
              switch (loptidx) {
                case 5: /* ssl */
                    par.ssl = 1;
                    break;
                case 6: /* key */
                    par.keyfile = optarg;
                    break;
                case 7: /* cert */
                    par.certfile = optarg;
                    break;
                case 8: /* cafile */
                    par.cafile = optarg;
                    break;
                case 9: /* cadir */
                    par.cadir = optarg;
                    break;
                case 10:
                    usage(par.progname);
                    exit(1);
              }
              break;

          case '4':
#ifdef INET6
              par.family = (par.family==AF_INET6)? AF_UNSPEC : AF_INET;
              break;

          case '6':
              par.family = (par.family==AF_INET)? AF_UNSPEC : AF_INET6;
#endif
              break;

          case 'l':
              par.logfile = optarg;
              break;

          case 's':
              par.sctp ++;
              break;

          case 't':
              par.tcp ++;
              break;

          case 'u':
              par.udp ++;
              break;

          case 'o':
              par.datadir = optarg;
              break;

          case 'p':
              if ((par.port=atoi(optarg)) <0)
              {
                  fprintf( stderr, "Invalid -p argument!\n" );
                  exit(1);
              }
              break;

          case 'v':
              verbose_level ++;
              break;

          case 'z':
              par.zout = optarg;
              if ( access( optarg, W_OK ) <0 ) {
                  fprintf( stderr, "cannot access file '%s': %s!\n",
                           optarg, strerror(errno) );
                  exit(1);
              }
              break;

          case 'h':
          default:
              usage(par.progname);
              exit(1);
        }
    }

    if ( !par.udp && !par.tcp && !par.sctp )
        par.tcp++;

    if ( !par.datadir && !par.zout) {
        fprintf( stderr, "info: message dump, no data storage.\n" );
        fflush( stderr );
    }

    if ( par.port==0 ) {
        par.port = par.ssl?IPFIX_TLS_PORTNO:IPFIX_PORTNO;
    }

    /** init loggin
     */
    mlog_set_vlevel( verbose_level );
    if ( par.logfile )
        (void) mlog_open( par.logfile, NULL );
    if ( (!par.datadir && !par.zout)
         || (verbose_level >2) )
        (void) ipfix_col_start_msglog( stderr );

    if (par.zout)
    {
    	mlogf( 1, "[%s] listen on port %d, write to %s ...\n", par.progname, par.port, par.zout);
    }
    else
    {
        mlogf( 1, "[%s] listen on port %d, write to %s ...\n",
           par.progname, par.port,par.datadir?"files":"stdout" );
    }

    /** init ipfix lib
     */
    if ( ipfix_init() <0 ) {
        fprintf( stderr, "ipfix_init() failed: %s\n", strerror(errno) );
        exit(1);
    }
    if ( ipfix_add_vendor_information_elements( ipfix_ft_fokus ) <0 ) {
        fprintf( stderr, "ipfix_add_ie() failed: %s\n", strerror(errno) );
        exit(1);
    }

    /** do the work
     */
    if ( do_collect() <0 )
        exit_func(1);

    exit_func(0);
    return 0;
}
