/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * tls.c --
 *
 *      Support for OpenSSL support (SSL and TLS), mostly for HTTPS
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
# include "nsopenssl.h"
# include <openssl/ssl.h>
# include <openssl/err.h>

/*
 * OpenSSL < 0.9.8f does not have SSL_set_tlsext_host_name() In some
 * versions, this function is defined as a macro, on some versions as
 * a library call, which complicates detection via m4.
 */
# if OPENSSL_VERSION_NUMBER > 0x00908070
#  define HAVE_SSL_set_tlsext_host_name 1
# endif

static void ReportError(Tcl_Interp *interp, const char *fmt, ...)
    NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(2,3);

/*
 * OpenSSL callback functions.
 */
static int SSL_serverNameCB(SSL *ssl, int *al, void *arg);
static DH *SSL_dhCB(SSL *ssl, int isExport, int keyLength);
static int SSLPassword(char *buf, int num, int rwflag, void *userdata);
#ifdef HAVE_OPENSSL_PRE_1_1
static void SSL_infoCB(const SSL *ssl, int where, int ret);
#endif


/*
 * Callback implementations.
 */
/*
 * Callback used for ephemeral DH keys
 */
static DH *
SSL_dhCB(SSL *ssl, int isExport, int keyLength) {
    NsSSLConfig *cfgPtr;
    DH          *key;
    SSL_CTX     *ctx;

    ctx = SSL_get_SSL_CTX(ssl);

    Ns_Log(Debug, "SSL_dhCB: isExport %d keyLength %d", isExport, keyLength);
    cfgPtr = (NsSSLConfig *)SSL_CTX_get_app_data(ctx);
    assert(cfgPtr != NULL);

    switch (keyLength) {
    case 512:
        key = cfgPtr->dhKey512;
        break;

    case 1024:
    default:
        key = cfgPtr->dhKey1024;
    }
    Ns_Log(Debug, "SSL_dhCB: returns %p\n", (void *)key);
    return key;
}

#ifdef HAVE_OPENSSL_PRE_1_1
/*
 * The renegotiation issue was fixed in recent versions of OpenSSL,
 * and the flag was removed, therefore, this function is just for
 * compatibility with old version of OpenSSL (flag removed in OpenSSL
 * 1.1.*).
 */
static void
SSL_infoCB(const SSL *ssl, int where, int UNUSED(ret)) {
    if ((where & SSL_CB_HANDSHAKE_DONE)) {

        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
}
#endif

/*
 * ServerNameCallback for SNI
 */
static int
SSL_serverNameCB(SSL *ssl, int *al, void *UNUSED(arg))
{
    const char  *serverName;
    int          result = SSL_TLSEXT_ERR_NOACK;

    serverName = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    if (serverName != NULL) {
        Ns_Sock  *sockPtr = (Ns_Sock*)SSL_get_app_data(ssl);
        Driver   *drvPtr = (Driver *)(sockPtr->driver);
        bool      doSNI = ((drvPtr->opts & NS_DRIVER_SNI) != 0u);

        //ctx = SSL_get_SSL_CTX(ssl);
        //cfgPtr = (NsSSLConfig *) SSL_CTX_get_app_data(ctx);

        /*
         * The default for *al is initialized by SSL_AD_UNRECOGNIZED_NAME = 112.
         * Find info about these codes via:
         *    fgrep -r --include=*.h 112 /usr/local/src/openssl/ | fgrep AD
         */
        Ns_Log(Notice, "SSL_serverNameCB got server name <%s> al %d sockPtr %p drv %p doSNI %d",
               serverName,
               (al != NULL ? *al : 0),
               (void*)sockPtr,
               (void*)(sockPtr != NULL ? sockPtr->driver : NULL),
               doSNI);

        /*
         * Perform lookup from host table only, when doSNI is true
         * (i.e. when per virtual server certificates were specified.
         */
        if (doSNI) {
            Tcl_DString     ds;
            unsigned short  port = Ns_SockGetPort(sockPtr);
            NS_TLS_SSL_CTX *ctx;

            /*
             * The virtual host entries are specified canonically, via
             * host:port. Since the provided "serverName" is specified by the
             * client, we can't precompute the strings to save a few cycles.
             */
            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "%s:%hu", serverName, port);

            ctx = NsDriverLookupHostCtx(ds.string, sockPtr->driver);

            Ns_Log(Notice, "SSL_serverNameCB lookup of <%s> location %s port %hu -> %p",
                   serverName, ds.string, port, (void*)ctx);

            /*
             * When the lookup succeeds, we have the alternate SSL_CTX
             * that we will use. Otherwise, do not acknowledge the
             * servername request.  Return the same value as when not
             * servername was provided (SSL_TLSEXT_ERR_NOACK).
             */
            if (ctx != NULL) {
                Ns_Log(Notice, "SSL_serverNameCB switches server context");
                SSL_set_SSL_CTX(ssl, ctx);
                result = SSL_TLSEXT_ERR_OK;
            }
            Tcl_DStringFree(&ds);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsOpenSSLInit --
 *
 *      Library entry point for OpenSSL. This routine calls various
 *      initialization functions for OpenSSL. OpenSSL cannot be used
 *      before this function is called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Numerous inside OpenSSL.
 *
 *----------------------------------------------------------------------
 */
void NsInitOpenSSL(void)
{
# ifdef HAVE_OPENSSL_EVP_H
    static int initialized = 0;

    if (!initialized) {
#  if OPENSSL_VERSION_NUMBER < 0x10100000L
        CRYPTO_set_mem_functions(ns_malloc, ns_realloc, ns_free);
#  endif
        /*
         * With OpenSSL 1.1.0 or above the OpenSSL library initializes
         * itself automatically.
         */
#  if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_1_0_2)
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
#   if OPENSSL_VERSION_NUMBER < 0x010100000 || defined(LIBRESSL_1_0_2)
        SSL_library_init();
#   else
        OPENSSL_init_ssl(0, NULL);
#   endif
#  endif
        initialized = 1;
        Ns_Log(Notice, "%s initialized", SSLeay_version(SSLEAY_VERSION));
    }
# endif
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxClientCreate --
 *
 *   Create and Initialize OpenSSL context
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX *ctx;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctxPtr != NULL);

    ctx = SSL_CTX_new(SSLv23_client_method());
    *ctxPtr = ctx;
    if (ctx == NULL) {
        char errorBuffer[256];

        Ns_TclPrintfResult(interp, "ctx init failed: %s", ERR_error_string(ERR_get_error(), errorBuffer));
        return TCL_ERROR;
    }

    SSL_CTX_set_default_verify_paths(ctx);
    if (caFile != NULL || caPath != NULL) {
        SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    }
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (cert != NULL) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            char errorBuffer[256];

            Ns_TclPrintfResult(interp, "certificate load error: %s",
                               ERR_error_string(ERR_get_error(), errorBuffer));
            goto fail;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            Ns_TclPrintfResult(interp, "private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
    }

    return TCL_OK;

 fail:
    SSL_CTX_free(ctx);
    *ctxPtr = NULL;

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxFree --
 *
 *   Free OpenSSL context
 *
 * Results:
 *   none
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *ctx)
{
    NS_NONNULL_ASSERT(ctx != NULL);

    SSL_CTX_free(ctx);
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_SSLConnect --
 *
 *   Initialize a socket as ssl socket and wait until the socket is
 *   usable (is connected, handshake performed)
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *   None
 *
 *----------------------------------------------------------------------
 */

int
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                  const char *sni_hostname,
                  NS_TLS_SSL **sslPtr)
{
    NS_TLS_SSL     *ssl;
    int             result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(sslPtr != NULL);

    ssl = SSL_new(ctx);
    *sslPtr = ssl;
    if (ssl == NULL) {
        Ns_TclPrintfResult(interp, "SSLCreate failed: %s", ERR_error_string(ERR_get_error(), NULL));
        result = TCL_ERROR;

    } else {
        if (sni_hostname != NULL) {
# if HAVE_SSL_set_tlsext_host_name
            Ns_Log(Debug, "tls: setting SNI hostname '%s'", sni_hostname);
            if (SSL_set_tlsext_host_name(ssl, sni_hostname) != 1) {
                Ns_Log(Warning, "tls: setting SNI hostname '%s' failed, value ignored", sni_hostname);
            }
# else
            Ns_Log(Warning, "tls: SNI hostname '%s' is not supported by version of OpenSSL", sni_hostname);
# endif
        }
        SSL_set_fd(ssl, sock);
        SSL_set_connect_state(ssl);

        for (;;) {
            int sslRc, err;

            Ns_Log(Debug, "ssl connect on sock %d", sock);
            sslRc = SSL_connect(ssl);
            err   = SSL_get_error(ssl, sslRc);
            //fprintf(stderr, "### ssl connect sock %d returned err %d\n", sock, err);
            if ((err == SSL_ERROR_WANT_WRITE) || (err == SSL_ERROR_WANT_READ)) {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */
                (void) Ns_SockTimedWait(sock,
                                        ((unsigned int)NS_SOCK_WRITE|(unsigned int)NS_SOCK_READ),
                                        &timeout);
                //fprintf(stderr, "### ssl connect retry on %d\n", sock);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            Ns_TclPrintfResult(interp, "ssl connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
            result = TCL_ERROR;
        } else {
            //const char *verifyString = X509_verify_cert_error_string(SSL_get_verify_result(ssl));
            //fprintf(stderr, "### SSL certificate verify: %s\n", verifyString);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SSLPassword --
 *
 *      Get the SSL password from the console (used by the OpenSSLs
 *      default_passwd_cb)
 *
 * Results:
 *      Length of the password.
 *
 * Side effects:
 *      Password passed back in buf.
 *
 *----------------------------------------------------------------------
 */

static int
SSLPassword(char *buf, int num, int UNUSED(rwflag), void *UNUSED(userdata))
{
    const char *pwd;

    fprintf(stdout, "Enter SSL password:");
    pwd = fgets(buf, num, stdin);
    return (pwd != NULL ? (int)strlen(buf) : 0);
}

static void
ReportError(Tcl_Interp *interp, const char *fmt, ...)
{
    va_list     ap;
    Tcl_DString ds;

    NS_NONNULL_ASSERT(fmt != NULL);

    Tcl_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    if (interp != NULL) {
        Tcl_DStringResult(interp, &ds);
    } else {
        Ns_Log(Warning, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerInit --
 *
 *   Read config information, vreate and initialize OpenSSL context.
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_CtxServerInit(const char *path, Tcl_Interp *interp,
                     unsigned int flags,
                     void *app_data,
                     NS_TLS_SSL_CTX **ctxPtr)
{
    int         result;
    const char *cert;

    cert = Ns_ConfigGetValue(path, "certificate");
    Ns_Log(Notice, "=== load certificate from <%s>", path);

    if (cert == NULL) {
        Ns_Log(Error, "nsssl: certificate parameter must be specified in the config file under %s", path);
        result = NS_ERROR;
    } else {
        const char *ciphers, *ciphersuites, *protocols;

        ciphers      = Ns_ConfigGetValue(path, "ciphers");
        ciphersuites = Ns_ConfigGetValue(path, "ciphersuites");
        protocols    = Ns_ConfigGetValue(path, "protocols");

        result = Ns_TLS_CtxServerCreate(interp, cert,
                                        NULL /*caFile*/, NULL /*caPath*/,
                                        Ns_ConfigBool(path, "verify", 0),
                                        ciphers, ciphersuites, protocols,
                                        ctxPtr);
        if (result == TCL_OK) {
            if (app_data != NULL) {
                SSL_CTX_set_app_data(*ctxPtr, app_data);
            }
            SSL_CTX_set_session_id_context(*ctxPtr, (const unsigned char *)&nsconf.pid, sizeof(pid_t));
            SSL_CTX_set_session_cache_mode(*ctxPtr, SSL_SESS_CACHE_SERVER);

#ifdef HAVE_OPENSSL_PRE_1_1
            SSL_CTX_set_info_callback(*ctxPtr, SSL_infoCB);
#endif

            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_SSLv2);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_SINGLE_DH_USE);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

            /*
             * Prefer server ciphers to secure against BEAST attack.
             */
            SSL_CTX_set_options(*ctxPtr, SSL_OP_CIPHER_SERVER_PREFERENCE);
            /*
             * Disable compression to avoid CRIME attack.
             */
#ifdef SSL_OP_NO_COMPRESSION
            SSL_CTX_set_options(*ctxPtr, SSL_OP_NO_COMPRESSION);
#endif

            /*
             * Obsolete since 1.1.0 but also supported in 3.0
             */
            SSL_CTX_set_options(*ctxPtr, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_D5_BUG);
            SSL_CTX_set_options(*ctxPtr, SSL_OP_TLS_BLOCK_PADDING_BUG);

            if ((flags & NS_DRIVER_SNI) != 0) {
                SSL_CTX_set_tlsext_servername_callback(*ctxPtr, SSL_serverNameCB);
                /* SSL_CTX_set_tlsext_servername_arg(cfgPtr->ctx, app_data); // not really needed */
            }
            SSL_CTX_set_tmp_dh_callback(*ctxPtr, SSL_dhCB);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_CtxServerCreate --
 *
 *   Create and Initialize OpenSSL context
 *
 * Results:
 *   Result code.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */
int
Ns_TLS_CtxServerCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath,
                       bool verify, const char *ciphers, const char *ciphersuites,
                       const char *protocols,
                       NS_TLS_SSL_CTX **ctxPtr)
{
    NS_TLS_SSL_CTX   *ctx;
    const SSL_METHOD *server_method;
    int rc;

    NS_NONNULL_ASSERT(ctxPtr != NULL);

#ifdef HAVE_OPENSSL_PRE_1_0_2
    server_method = SSLv23_server_method();
#else
    server_method = TLS_server_method();
#endif

    ctx = SSL_CTX_new(server_method);
    *ctxPtr = ctx;
    if (ctx == NULL) {
        ReportError(interp, "ssl ctx init failed: %s", ERR_error_string(ERR_get_error(), NULL));
        return TCL_ERROR;
    }

    if (cert == NULL && caFile == NULL) {
        ReportError(interp, "At least one of certificate or cafile must be specified!");
        goto fail;
    }

    if (ciphers != NULL) {
        rc = SSL_CTX_set_cipher_list(ctx, ciphers);
        if (rc == 0) {
            ReportError(interp, "ssl ctx invalid cipher list '%s': %s",
                        ciphers, ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
    }

    if (ciphersuites != NULL) {
        rc = SSL_CTX_set_ciphersuites(ctx, ciphersuites);
        if (rc == 0) {
            ReportError(interp, "ssl ctx invalid ciphersuites specification '%s': %s",
                        ciphersuites, ERR_error_string(ERR_get_error(), NULL));
        }
    }

    /*
     * Parse SSL protocols
     */
    {
        unsigned long n = SSL_OP_ALL;

        if (protocols != NULL) {
            if (strstr(protocols, "!SSLv2") != NULL) {
                n |= SSL_OP_NO_SSLv2;
                Ns_Log(Notice, "nsssl: disabling SSLv2");
            }
            if (strstr(protocols, "!SSLv3") != NULL) {
                n |= SSL_OP_NO_SSLv3;
                Ns_Log(Notice, "nsssl: disabling SSLv3");
            }
            if (strstr(protocols, "!TLSv1.0") != NULL) {
                n |= SSL_OP_NO_TLSv1;
                Ns_Log(Notice, "nsssl: disabling TLSv1.0");
            }
            if (strstr(protocols, "!TLSv1.1") != NULL) {
                n |= SSL_OP_NO_TLSv1_1;
                Ns_Log(Notice, "nsssl: disabling TLSv1.1");
            }
#ifdef SSL_OP_NO_TLSv1_2
            if (strstr(protocols, "!TLSv1.2") != NULL) {
                n |= SSL_OP_NO_TLSv1_2;
                Ns_Log(Notice, "nsssl: disabling TLSv1.2");
            }
#endif
#ifdef SSL_OP_NO_TLSv1_3
            if (strstr(protocols, "!TLSv1.3") != NULL) {
                n |= SSL_OP_NO_TLSv1_3;
                Ns_Log(Notice, "nsssl: disabling TLSv1.3");
            }
#endif
        }
        SSL_CTX_set_options(ctx, n);
    }

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_load_verify_locations(ctx, caFile, caPath);
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_default_passwd_cb(ctx, SSLPassword);

    if (cert != NULL) {
        /*
         * Load certificate and private key
         */
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
            ReportError(interp, "certificate load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
            ReportError(interp, "private key load error: %s", ERR_error_string(ERR_get_error(), NULL));
            goto fail;
        }
        /*
         * Get DH parameters from .pem file
         */
        {
            BIO *bio = BIO_new_file(cert, "r");
            DH  *dh  = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
            BIO_free(bio);

            if (dh != NULL) {
                if (SSL_CTX_set_tmp_dh(ctx, dh) < 0) {
                    Ns_Log(Error, "nsssl: Couldn't set DH parameters");
                    return NS_ERROR;
                }
                DH_free(dh);
            }
        }
    }
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    SSL_CTX_set_quiet_shutdown(ctx, 1);
#endif

    return TCL_OK;

 fail:
    SSL_CTX_free(ctx);
    *ctxPtr = NULL;

    return TCL_ERROR;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TLS_SSLAccept --
 *
 *   Initialize a socket as ssl socket and wait until the socket
 *   is usable (is accepted, handshake performed)
 *
 * Results:
 *   Tcl result code.
 *
 * Side effects:
 *   None
 *
 *----------------------------------------------------------------------
 */

int
Ns_TLS_SSLAccept(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                 NS_TLS_SSL **sslPtr)
{
    NS_TLS_SSL     *ssl;
    int             result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(sslPtr != NULL);

    ssl = SSL_new(ctx);
    *sslPtr = ssl;
    if (ssl == NULL) {
        char *errMsg, errorBuffer[256];

        errMsg = ERR_error_string(ERR_get_error(), errorBuffer);
        Ns_TclPrintfResult(interp, "SSLAccept failed: %s", errMsg);
        Ns_Log(Debug, "SSLAccept failed: %s", errMsg);
        result = TCL_ERROR;

    } else {

        SSL_set_fd(ssl, sock);
        SSL_set_accept_state(ssl);

        for (;;) {
            int rc, err;

            rc = SSL_do_handshake(ssl);
            err = SSL_get_error(ssl, rc);

            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                unsigned int st;
                Ns_Time      timeout = { 0, 10000 }; /* 10ms */

                st = (unsigned int)NS_SOCK_WRITE | (unsigned int)NS_SOCK_READ;
                (void) Ns_SockTimedWait(sock, st, &timeout);
                continue;
            }
            break;
        }

        if (!SSL_is_init_finished(ssl)) {
            char *errMsg, errorBuffer[256];

            errMsg = ERR_error_string(ERR_get_error(), errorBuffer);
            Ns_TclPrintfResult(interp, "ssl accept failed: %s", errMsg);
            Ns_Log(Debug, "SSLAccept failed: %s", errMsg);

            SSL_free(ssl);
            *sslPtr = NULL;
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SSLRecvBufs2 --
 *
 *      Read data from a nonblocking socket into a vector of buffers.
 *      Ns_SockRecvBufs2() is similar to Ns_SockRecvBufs() with the following
 *      differences:
 *        a) the first argument is a SSL *
 *        b) it performs no timeout handliong
 *        c) it returns the sockstate in its last argument
 *
 * Results:
 *      Number of bytes read or -1 on error.  The return
 *      value will be 0 when the peer has performed an orderly shutdown.
 *      The resulting sockstate has one of the following codes:
 *
 *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SSLRecvBufs2(SSL *sslPtr, struct iovec *bufs, int UNUSED(nbufs),
                Ns_SockState *sockStatePtr)
{
    ssize_t       nRead = 0;
    int           got = 0, sock, n = 0, err = SSL_ERROR_NONE;
    char         *buf = NULL;
    unsigned long sslError;
    char          errorBuffer[256];
    Ns_SockState  sockState = NS_SOCK_READ;

    NS_NONNULL_ASSERT(sslPtr != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);
    NS_NONNULL_ASSERT(sockStatePtr != NULL);

    buf = (char *)bufs->iov_base;
    sock = SSL_get_fd(sslPtr);

    ERR_clear_error();
    n = SSL_read(sslPtr, buf + got, (int)bufs->iov_len - got);
    err = SSL_get_error(sslPtr, n);
    //Ns_Log(Notice, "=== SSL_read(%d) received:%d, err:%d", sock, n, err);

    switch (err) {
    case SSL_ERROR_NONE:
        if (n < 0) {
            Ns_Log(Debug, "SSL_read(%d) received:%d, but have not SSL_ERROR", sock, n);
            nRead = n;
        } else {
            got += n;
            Ns_Log(Debug, "SSL_read(%d) got:%d", sock, got);
            nRead = got;
        }
        break;

    case SSL_ERROR_ZERO_RETURN:

        Ns_Log(Debug, "SSL_read(%d) ERROR_ZERO_RETURN got:%d", sock, got);

        nRead = got;
        sockState = NS_SOCK_DONE;
        break;

    case SSL_ERROR_WANT_READ:

        Ns_Log(Debug, "SSL_read(%d) ERROR_WANT_READ got:%d", sock, got);

        nRead = got;
        sockState = NS_SOCK_AGAIN;
        break;

    case SSL_ERROR_SYSCALL:

        sslError = ERR_get_error();
        Ns_Log(Debug, "SSL_read(%d) SSL_ERROR_SYSCALL got:%d sslError %lu: %s", sock, got,
               sslError, ERR_error_string(sslError, errorBuffer));

        if (sslError == 0) {
            Ns_Log(Debug, "SSL_read(%d) ERROR_SYSCALL (eod?), got:%d", sock, got);
            nRead = got;
            sockState = NS_SOCK_DONE;
            break;
        } else {
            const char *ioerr;

            ioerr = ns_sockstrerror(ns_sockerrno);
            Ns_Log(Debug, "SSL_read(%d) ERROR_SYSCALL %s", sock, ioerr);
        }
        NS_FALL_THROUGH; /* fall through */

    default:
        sslError = ERR_get_error();

        Ns_Log(Debug, "SSL_read(%d) error handler err %d sslError %lu",
               sock, err, sslError);
        /*
         * Starting with the commit in OpenSSL 1.1.1 branch
         * OpenSSL_1_1_1-stable below, at least https client requests
         * answered without an explicit content length start to
         * fail. This can be tested with:
         *
         *       ns_logctl severity Debug(task) on
         *       ns_http run https://www.google.com/
         *
         * The fix below just triggers for exactly this condition to
         * provide a graceful end for these requests.
         *
         * https://github.com/openssl/openssl/commit/db943f43a60d1b5b1277e4b5317e8f288e7a0a3a
         */
        if (err == SSL_ERROR_SSL) {
            int reasonCode = ERR_GET_REASON(sslError);

            Ns_Log(Debug, "SSL_read(%d) error handler SSL_ERROR_SSL sslError %lu reason code %d",
                   sock, sslError, reasonCode);
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
            if (reasonCode == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                Ns_Log(Notice, "SSL_read(%d) ERROR_SYSCALL sees UNEXPECTED_EOF_WHILE_READING", sock);
                nRead = got;
                sockState = NS_SOCK_DONE;
                break;
            }
#endif
        }
        /*
         * Report all sslErrors from the OpenSSL error stack as
         * "notices" in the system log file.
         */
        while (sslError != 0u) {
            Ns_Log(Notice, "SSL_read(%d) error received:%d, got:%d, err:%d,"
                   " get_error:%lu, %s", sock, n, got, err, sslError,
                   ERR_error_string(sslError, errorBuffer));
            sslError = ERR_get_error();
        }

        SSL_set_shutdown(sslPtr, SSL_RECEIVED_SHUTDOWN);
        //Ns_Log(Notice, "SSL_read(%d) error after shutdown", sock);
        nRead = -1;
        break;

    }

    if (nRead < 0) {
        sockState = NS_SOCK_EXCEPTION;
    }

    *sockStatePtr = sockState;
    Ns_Log(Debug, "### SSL_read(%d) return:%ld sockState:%.2x", sock, nRead, sockState);

    return nRead;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SSLSendBufs2 --
 *
 *      Send a vector of buffers on a nonblocking TLS socket.
 *      It is similar to Ns_SockSendBufs() except that it
 *        a) receives a SSL * as first argument
 *        b) it does not care about partial writes,
 *           it simply returns the number of bytes sent.
 *        c) it never blocks
 *        d) it does not try corking
 *
 * Results:
 *      Number of bytes sent (which might be also 0 on EAGAIN cases)
 *      or -1 on error.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
ssize_t

Ns_SSLSendBufs2(SSL *ssl, const struct iovec *bufs, int nbufs)
{
    ssize_t sent = -1;

    NS_NONNULL_ASSERT(ssl != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    if (nbufs > 1) {
        Ns_Fatal("Ns_SSLSendBufs2: can handle at most one buffer at the time");
    } else if (bufs[0].iov_len == 0) {
        sent = 0;
    } else {
        int  err;

        sent = SSL_write(ssl, bufs[0].iov_base, (int)bufs[0].iov_len);
        err = SSL_get_error(ssl, (int)sent);

        if (err == SSL_ERROR_WANT_WRITE) {
            sent = 0;
        } else if (err == SSL_ERROR_SYSCALL) {
            const char *ioerr;

            ioerr = ns_sockstrerror(ns_sockerrno);
            Ns_Log(Debug, "SSL_write ERROR_SYSCALL %s", ioerr);
        } else if (err != SSL_ERROR_NONE) {
            Ns_Log(Debug, "SSL_write: sent:%ld, error:%d", sent, err);
        }
    }

    return sent;
}

#else

void NsInitOpenSSL(void)
{
    Ns_Log(Notice, "No support for OpenSSL compiled in");
}

/*
 * Dummy stub functions, for the case, when NaviServer is built without
 * OpenSSL support, e.g. when built for the option --without-openssl.
 */

int
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET UNUSED(sock), NS_TLS_SSL_CTX *UNUSED(ctx),
                  const char *UNUSED(sni_hostname),
                  NS_TLS_SSL **UNUSED(sslPtr))
{
    Ns_TclPrintfResult(interp, "SSLCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_SSLAccept(Tcl_Interp *interp, NS_SOCKET UNUSED(sock), NS_TLS_SSL_CTX *UNUSED(ctx),
                 NS_TLS_SSL **UNUSED(sslPtr))
{
    Ns_TclPrintfResult(interp, "SSLAccept failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath), bool UNUSED(verify),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    Ns_TclPrintfResult(interp, "CtxCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

int
Ns_TLS_CtxServerCreate(Tcl_Interp *interp,
                       const char *UNUSED(cert), const char *UNUSED(caFile), const char *UNUSED(caPath),
                       bool UNUSED(verify), const char *UNUSED(ciphers), const char *UNUSED(ciphersuites),
                       const char *UNUSED(protocols),
                       NS_TLS_SSL_CTX **UNUSED(ctxPtr))
{
    ReportError(interp, "CtxServerCreate failed: no support for OpenSSL built in");
    return TCL_ERROR;
}

void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *UNUSED(ctx))
{
    /* dummy stub */
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
