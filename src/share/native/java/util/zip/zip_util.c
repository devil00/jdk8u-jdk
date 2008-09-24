/*
 * Copyright 1995-2006 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Sun designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Sun in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * Support for reading ZIP/JAR files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>

#include "jni.h"
#include "jni_util.h"
#include "jlong.h"
#include "jvm.h"
#include "io_util.h"
#include "io_util_md.h"
#include "zip_util.h"
#include "zlib.h"

/* USE_MMAP means mmap the CEN & ENDHDR part of the zip file. */
#ifdef USE_MMAP
#include <sys/mman.h>
#endif

#define MAXREFS 0xFFFF  /* max number of open zip file references */

#define MCREATE()      JVM_RawMonitorCreate()
#define MLOCK(lock)    JVM_RawMonitorEnter(lock)
#define MUNLOCK(lock)  JVM_RawMonitorExit(lock)
#define MDESTROY(lock) JVM_RawMonitorDestroy(lock)

#define CENSIZE(cen) (CENHDR + CENNAM(cen) + CENEXT(cen) + CENCOM(cen))

static jzfile *zfiles = 0;      /* currently open zip files */
static void *zfiles_lock = 0;

static void freeCEN(jzfile *);

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static jint INITIAL_META_COUNT = 2;   /* initial number of entries in meta name array */

/*
 * The ZFILE_* functions exist to provide some platform-independence with
 * respect to file access needs.
 */

/*
 * Opens the named file for reading, returning a ZFILE.
 *
 * Compare this with winFileHandleOpen in windows/native/java/io/io_util_md.c.
 * This function does not take JNIEnv* and uses CreateFile (instead of
 * CreateFileW).  The expectation is that this function will be called only
 * from ZIP_Open_Generic, which in turn is used by the JVM, where we do not
 * need to concern ourselves with wide chars.
 */
static ZFILE
ZFILE_Open(const char *fname, int flags) {
#ifdef WIN32
    const DWORD access =
        (flags & O_RDWR)   ? (GENERIC_WRITE | GENERIC_READ) :
        (flags & O_WRONLY) ?  GENERIC_WRITE :
        GENERIC_READ;
    const DWORD sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE;
    const DWORD disposition =
        /* Note: O_TRUNC overrides O_CREAT */
        (flags & O_TRUNC) ? CREATE_ALWAYS :
        (flags & O_CREAT) ? OPEN_ALWAYS   :
        OPEN_EXISTING;
    const DWORD  maybeWriteThrough =
        (flags & (O_SYNC | O_DSYNC)) ?
        FILE_FLAG_WRITE_THROUGH :
        FILE_ATTRIBUTE_NORMAL;
    const DWORD maybeDeleteOnClose =
        (flags & O_TEMPORARY) ?
        FILE_FLAG_DELETE_ON_CLOSE :
        FILE_ATTRIBUTE_NORMAL;
    const DWORD flagsAndAttributes = maybeWriteThrough | maybeDeleteOnClose;

    return (jlong) CreateFile(
        fname,          /* Wide char path name */
        access,         /* Read and/or write permission */
        sharing,        /* File sharing flags */
        NULL,           /* Security attributes */
        disposition,        /* creation disposition */
        flagsAndAttributes, /* flags and attributes */
        NULL);
#else
    return JVM_Open(fname, flags, 0);
#endif
}

/*
 * The io_util_md.h files do not provide IO_CLOSE, hence we use platform
 * specifics.
 */
static void
ZFILE_Close(ZFILE zfd) {
#ifdef WIN32
    CloseHandle((HANDLE) zfd);
#else
    JVM_Close(zfd);
#endif
}

static jlong
ZFILE_Lseek(ZFILE zfd, off_t offset, int whence) {
    return IO_Lseek(zfd, offset, whence);
}

static int
ZFILE_read(ZFILE zfd, char *buf, jint nbytes) {
#ifdef WIN32
    return (int) IO_Read(zfd, buf, nbytes);
#else
    /*
     * Calling JVM_Read will return JVM_IO_INTR when Thread.interrupt is called
     * only on Solaris. Continue reading jar file in this case is the best
     * thing to do since zip file reading is relatively fast and it is very onerous
     * for a interrupted thread to deal with this kind of hidden I/O. However, handling
     * JVM_IO_INTR is tricky and could cause undesired side effect. So we decided
     * to simply call "read" on Solaris/Linux. See details in bug 6304463.
     */
    return read(zfd, buf, nbytes);
#endif
}

/*
 * Initialize zip file support. Return 0 if successful otherwise -1
 * if could not be initialized.
 */
static jint
InitializeZip()
{
    static jboolean inited = JNI_FALSE;

    // Initialize errno to 0.  It may be set later (e.g. during memory
    // allocation) but we can disregard previous values.
    errno = 0;

    if (inited)
        return 0;
    zfiles_lock = MCREATE();
    if (zfiles_lock == 0) {
        return -1;
    }
    inited = JNI_TRUE;

    return 0;
}

/*
 * Reads len bytes of data into buf.
 * Returns 0 if all bytes could be read, otherwise returns -1.
 */
static int
readFully(ZFILE zfd, void *buf, jlong len) {
  char *bp = (char *) buf;

  while (len > 0) {
        jlong limit = ((((jlong) 1) << 31) - 1);
        jint count = (len < limit) ?
            (jint) len :
            (jint) limit;
        jint n = ZFILE_read(zfd, bp, count);
        if (n > 0) {
            bp += n;
            len -= n;
        } else if (n == JVM_IO_ERR && errno == EINTR) {
          /* Retry after EINTR (interrupted by signal).
             We depend on the fact that JVM_IO_ERR == -1. */
            continue;
        } else { /* EOF or IO error */
            return -1;
        }
    }
    return 0;
}

/*
 * Reads len bytes of data from the specified offset into buf.
 * Returns 0 if all bytes could be read, otherwise returns -1.
 */
static int
readFullyAt(ZFILE zfd, void *buf, jlong len, jlong offset)
{
    if (ZFILE_Lseek(zfd, (off_t) offset, SEEK_SET) == -1) {
        return -1; /* lseek failure. */
    }

    return readFully(zfd, buf, len);
}

/*
 * Allocates a new zip file object for the specified file name.
 * Returns the zip file object or NULL if not enough memory.
 */
static jzfile *
allocZip(const char *name)
{
    jzfile *zip;
    if (((zip = calloc(1, sizeof(jzfile))) != NULL) &&
        ((zip->name = strdup(name))        != NULL) &&
        ((zip->lock = MCREATE())           != NULL)) {
        zip->zfd = -1;
        return zip;
    }

    if (zip != NULL) {
        free(zip->name);
        free(zip);
    }
    return NULL;
}

/*
 * Frees all native resources owned by the specified zip file object.
 */
static void
freeZip(jzfile *zip)
{
    /* First free any cached jzentry */
    ZIP_FreeEntry(zip,0);
    if (zip->lock != NULL) MDESTROY(zip->lock);
    free(zip->name);
    freeCEN(zip);
#ifdef USE_MMAP
    if (zip->maddr != NULL) munmap((char *)zip->maddr, zip->mlen);
#else
    free(zip->cencache.data);
#endif
    if (zip->zfd != -1) ZFILE_Close(zip->zfd);
    free(zip);
}

/* The END header is followed by a variable length comment of size < 64k. */
static const jlong END_MAXLEN = 0xFFFF + ENDHDR;

#define READBLOCKSZ 128

/*
 * Searches for end of central directory (END) header. The contents of
 * the END header will be read and placed in endbuf. Returns the file
 * position of the END header, otherwise returns 0 if the END header
 * was not found or -1 if an error occurred.
 */
static jlong
findEND(jzfile *zip, void *endbuf)
{
    char buf[READBLOCKSZ];
    jlong pos;
    const jlong len = zip->len;
    const ZFILE zfd = zip->zfd;
    const jlong minHDR = len - END_MAXLEN > 0 ? len - END_MAXLEN : 0;
    const jlong minPos = minHDR - (sizeof(buf)-ENDHDR);

    for (pos = len - sizeof(buf); pos >= minPos; pos -= (sizeof(buf)-ENDHDR)) {

        int i;
        jlong off = 0;
        if (pos < 0) {
            /* Pretend there are some NUL bytes before start of file */
            off = -pos;
            memset(buf, '\0', off);
        }

        if (readFullyAt(zfd, buf + off, sizeof(buf) - off,
                        pos + off) == -1) {
            return -1;  /* System error */
        }

        /* Now scan the block backwards for END header signature */
        for (i = sizeof(buf) - ENDHDR; i >= 0; i--) {
            if (buf[i+0] == 'P'    &&
                buf[i+1] == 'K'    &&
                buf[i+2] == '\005' &&
                buf[i+3] == '\006' &&
                (pos + i + ENDHDR + ENDCOM(buf + i) == len)) {
                    /* Found END header */
                    memcpy(endbuf, buf + i, ENDHDR);
                    return pos + i;
            }
        }
    }
    return 0; /* END header not found */
}

/*
 * Returns a hash code value for a C-style NUL-terminated string.
 */
static unsigned int
hash(const char *s)
{
    int h = 0;
    while (*s != '\0')
        h = 31*h + *s++;
    return h;
}

/*
 * Returns a hash code value for a string of a specified length.
 */
static unsigned int
hashN(const char *s, int length)
{
    int h = 0;
    while (length-- > 0)
        h = 31*h + *s++;
    return h;
}

static unsigned int
hash_append(unsigned int hash, char c)
{
    return ((int)hash)*31 + c;
}

/*
 * Returns true if the specified entry's name begins with the string
 * "META-INF/" irrespective of case.
 */
static int
isMetaName(const char *name, int length)
{
    const char *s;
    if (length < sizeof("META-INF/") - 1)
        return 0;
    for (s = "META-INF/"; *s != '\0'; s++) {
        char c = *name++;
        // Avoid toupper; it's locale-dependent
        if (c >= 'a' && c <= 'z') c += 'A' - 'a';
        if (*s != c)
            return 0;
    }
    return 1;
}

/*
 * Increases the capacity of zip->metanames.
 * Returns non-zero in case of allocation error.
 */
static int
growMetaNames(jzfile *zip)
{
    jint i;
    /* double the meta names array */
    const jint new_metacount = zip->metacount << 1;
    zip->metanames =
        realloc(zip->metanames, new_metacount * sizeof(zip->metanames[0]));
    if (zip->metanames == NULL) return -1;
    for (i = zip->metacount; i < new_metacount; i++)
        zip->metanames[i] = NULL;
    zip->metacurrent = zip->metacount;
    zip->metacount = new_metacount;
    return 0;
}

/*
 * Adds name to zip->metanames.
 * Returns non-zero in case of allocation error.
 */
static int
addMetaName(jzfile *zip, const char *name, int length)
{
    jint i;
    if (zip->metanames == NULL) {
      zip->metacount = INITIAL_META_COUNT;
      zip->metanames = calloc(zip->metacount, sizeof(zip->metanames[0]));
      if (zip->metanames == NULL) return -1;
      zip->metacurrent = 0;
    }

    i = zip->metacurrent;

    /* current meta name array isn't full yet. */
    if (i < zip->metacount) {
      zip->metanames[i] = (char *) malloc(length+1);
      if (zip->metanames[i] == NULL) return -1;
      memcpy(zip->metanames[i], name, length);
      zip->metanames[i][length] = '\0';
      zip->metacurrent++;
      return 0;
    }

    /* No free entries in zip->metanames? */
    if (growMetaNames(zip) != 0) return -1;
    return addMetaName(zip, name, length);
}

static void
freeMetaNames(jzfile *zip)
{
    if (zip->metanames) {
        jint i;
        for (i = 0; i < zip->metacount; i++)
            free(zip->metanames[i]);
        free(zip->metanames);
        zip->metanames = NULL;
    }
}

/* Free Zip data allocated by readCEN() */
static void
freeCEN(jzfile *zip)
{
    free(zip->entries); zip->entries = NULL;
    free(zip->table);   zip->table   = NULL;
    freeMetaNames(zip);
}

/*
 * Counts the number of CEN headers in a central directory extending
 * from BEG to END.  Might return a bogus answer if the zip file is
 * corrupt, but will not crash.
 */
static jint
countCENHeaders(unsigned char *beg, unsigned char *end)
{
    jint count = 0;
    ptrdiff_t i;
    for (i = 0; i + CENHDR < end - beg; i += CENSIZE(beg + i))
        count++;
    return count;
}

#define ZIP_FORMAT_ERROR(message) \
if (1) { zip->msg = message; goto Catch; } else ((void)0)

/*
 * Reads zip file central directory. Returns the file position of first
 * CEN header, otherwise returns 0 if central directory not found or -1
 * if an error occurred. If zip->msg != NULL then the error was a zip
 * format error and zip->msg has the error text.
 * Always pass in -1 for knownTotal; it's used for a recursive call.
 */
static jlong
readCEN(jzfile *zip, jint knownTotal)
{
    /* Following are unsigned 32-bit */
    jlong endpos, cenpos, cenlen;
    /* Following are unsigned 16-bit */
    jint total, tablelen, i, j;
    unsigned char *cenbuf = NULL;
    unsigned char *cenend;
    unsigned char *cp;
#ifdef USE_MMAP
    static jlong pagesize;
    off_t offset;
#endif
    unsigned char endbuf[ENDHDR];
    jzcell *entries;
    jint *table;

    /* Clear previous zip error */
    zip->msg = NULL;

    /* Get position of END header */
    if ((endpos = findEND(zip, endbuf)) == -1)
        return -1; /* system error */

    if (endpos == 0) return 0;  /* END header not found */

    freeCEN(zip);

   /* Get position and length of central directory */
    cenlen = ENDSIZ(endbuf);
    if (cenlen > endpos)
        ZIP_FORMAT_ERROR("invalid END header (bad central directory size)");
    cenpos = endpos - cenlen;

    /* Get position of first local file (LOC) header, taking into
     * account that there may be a stub prefixed to the zip file. */
    zip->locpos = cenpos - ENDOFF(endbuf);
    if (zip->locpos < 0)
        ZIP_FORMAT_ERROR("invalid END header (bad central directory offset)");

#ifdef USE_MMAP
     /* On Solaris & Linux prior to JDK 6, we used to mmap the whole jar file to
      * read the jar file contents. However, this greatly increased the perceived
      * footprint numbers because the mmap'ed pages were adding into the totals shown
      * by 'ps' and 'top'. We switched to mmaping only the central directory of jar
      * file while calling 'read' to read the rest of jar file. Here are a list of
      * reasons apart from above of why we are doing so:
      * 1. Greatly reduces mmap overhead after startup complete;
      * 2. Avoids dual path code maintainance;
      * 3. Greatly reduces risk of address space (not virtual memory) exhaustion.
      */
    if (pagesize == 0) {
        pagesize = (jlong)sysconf(_SC_PAGESIZE);
        if (pagesize == 0) goto Catch;
    }
    if (cenpos > pagesize) {
        offset = cenpos & ~(pagesize - 1);
    } else {
        offset = 0;
    }
    /* When we are not calling recursively, knownTotal is -1. */
    if (knownTotal == -1) {
        void* mappedAddr;
        /* Mmap the CEN and END part only. We have to figure
           out the page size in order to make offset to be multiples of
           page size.
        */
        zip->mlen = cenpos - offset + cenlen + ENDHDR;
        zip->offset = offset;
        mappedAddr = mmap(0, zip->mlen, PROT_READ, MAP_SHARED, zip->zfd, offset);
        zip->maddr = (mappedAddr == (void*) MAP_FAILED) ? NULL :
            (unsigned char*)mappedAddr;

        if (zip->maddr == NULL) {
            jio_fprintf(stderr, "mmap failed for CEN and END part of zip file\n");
            goto Catch;
        }
    }
    cenbuf = zip->maddr + cenpos - offset;
#else
    if ((cenbuf = malloc((size_t) cenlen)) == NULL ||
        (readFullyAt(zip->zfd, cenbuf, cenlen, cenpos) == -1))
        goto Catch;
#endif
    cenend = cenbuf + cenlen;

    /* Initialize zip file data structures based on the total number
     * of central directory entries as stored in ENDTOT.  Since this
     * is a 2-byte field, but we (and other zip implementations)
     * support approx. 2**31 entries, we do not trust ENDTOT, but
     * treat it only as a strong hint.  When we call ourselves
     * recursively, knownTotal will have the "true" value. */
    total = (knownTotal != -1) ? knownTotal : ENDTOT(endbuf);
    entries  = zip->entries  = calloc(total, sizeof(entries[0]));
    tablelen = zip->tablelen = ((total/2) | 1); // Odd -> fewer collisions
    table    = zip->table    = malloc(tablelen * sizeof(table[0]));
    if (entries == NULL || table == NULL) goto Catch;
    for (j = 0; j < tablelen; j++)
        table[j] = ZIP_ENDCHAIN;

    /* Iterate through the entries in the central directory */
    for (i = 0, cp = cenbuf; cp <= cenend - CENHDR; i++, cp += CENSIZE(cp)) {
        /* Following are unsigned 16-bit */
        jint method, nlen;
        unsigned int hsh;

        if (i >= total) {
            /* This will only happen if the zip file has an incorrect
             * ENDTOT field, which usually means it contains more than
             * 65535 entries. */
            cenpos = readCEN(zip, countCENHeaders(cenbuf, cenend));
            goto Finally;
        }

        method = CENHOW(cp);
        nlen   = CENNAM(cp);

        if (GETSIG(cp) != CENSIG)
            ZIP_FORMAT_ERROR("invalid CEN header (bad signature)");
        if (CENFLG(cp) & 1)
            ZIP_FORMAT_ERROR("invalid CEN header (encrypted entry)");
        if (method != STORED && method != DEFLATED)
            ZIP_FORMAT_ERROR("invalid CEN header (bad compression method)");
        if (cp + CENHDR + nlen > cenend)
            ZIP_FORMAT_ERROR("invalid CEN header (bad header size)");

        /* if the entry is metadata add it to our metadata names */
        if (isMetaName((char *)cp+CENHDR, nlen))
            if (addMetaName(zip, (char *)cp+CENHDR, nlen) != 0)
                goto Catch;

        /* Record the CEN offset and the name hash in our hash cell. */
        entries[i].cenpos = cenpos + (cp - cenbuf);
        entries[i].hash = hashN((char *)cp+CENHDR, nlen);

        /* Add the entry to the hash table */
        hsh = entries[i].hash % tablelen;
        entries[i].next = table[hsh];
        table[hsh] = i;
    }
    if (cp != cenend)
        ZIP_FORMAT_ERROR("invalid CEN header (bad header size)");

    zip->total = i;

    goto Finally;

 Catch:
    freeCEN(zip);
    cenpos = -1;

 Finally:
#ifndef USE_MMAP
    free(cenbuf);
#endif
    return cenpos;
}

/*
 * Opens a zip file with the specified mode. Returns the jzfile object
 * or NULL if an error occurred. If a zip error occurred then *pmsg will
 * be set to the error message text if pmsg != 0. Otherwise, *pmsg will be
 * set to NULL.
 */
jzfile *
ZIP_Open_Generic(const char *name, char **pmsg, int mode, jlong lastModified)
{
    jzfile *zip = NULL;

    /* Clear zip error message */
    if (pmsg != 0) {
        *pmsg = NULL;
    }

    zip = ZIP_Get_From_Cache(name, pmsg, lastModified);

    if (zip == NULL && *pmsg == NULL) {
        ZFILE zfd = ZFILE_Open(name, mode);
        zip = ZIP_Put_In_Cache(name, zfd, pmsg, lastModified);
    }
    return zip;
}

/*
 * Returns the jzfile corresponding to the given file name from the cache of
 * zip files, or NULL if the file is not in the cache.  If the name is longer
 * than PATH_MAX or a zip error occurred then *pmsg will be set to the error
 * message text if pmsg != 0. Otherwise, *pmsg will be set to NULL.
 */
jzfile *
ZIP_Get_From_Cache(const char *name, char **pmsg, jlong lastModified)
{
    static char errbuf[256];
    char buf[PATH_MAX];
    jzfile *zip;

    if (InitializeZip()) {
        return NULL;
    }

    /* Clear zip error message */
    if (pmsg != 0) {
        *pmsg = NULL;
    }

    if (strlen(name) >= PATH_MAX) {
        if (pmsg) {
            *pmsg = "zip file name too long";
        }
        return NULL;
    }
    strcpy(buf, name);
    JVM_NativePath(buf);
    name = buf;

    MLOCK(zfiles_lock);
    for (zip = zfiles; zip != NULL; zip = zip->next) {
        if (strcmp(name, zip->name) == 0
            && (zip->lastModified == lastModified || zip->lastModified == 0)
            && zip->refs < MAXREFS) {
            zip->refs++;
            break;
        }
    }
    MUNLOCK(zfiles_lock);
    return zip;
}

/*
 * Reads data from the given file descriptor to create a jzfile, puts the
 * jzfile in a cache, and returns that jzfile.  Returns NULL in case of error.
 * If a zip error occurs, then *pmsg will be set to the error message text if
 * pmsg != 0. Otherwise, *pmsg will be set to NULL.
 */
jzfile *
ZIP_Put_In_Cache(const char *name, ZFILE zfd, char **pmsg, jlong lastModified)
{
    static char errbuf[256];
    jlong len;
    jzfile *zip;

    if ((zip = allocZip(name)) == NULL) {
        return NULL;
    }

    zip->refs = 1;
    zip->lastModified = lastModified;

    if (zfd == -1) {
        if (pmsg && JVM_GetLastErrorString(errbuf, sizeof(errbuf)) > 0)
            *pmsg = errbuf;
        freeZip(zip);
        return NULL;
    }

    len = zip->len = ZFILE_Lseek(zfd, 0, SEEK_END);
    if (len == -1) {
        if (pmsg && JVM_GetLastErrorString(errbuf, sizeof(errbuf)) > 0)
            *pmsg = errbuf;
        ZFILE_Close(zfd);
        freeZip(zip);
        return NULL;
    }

    zip->zfd = zfd;
    if (readCEN(zip, -1) <= 0) {
        /* An error occurred while trying to read the zip file */
        if (pmsg != 0) {
            /* Set the zip error message */
            *pmsg = zip->msg;
        }
        freeZip(zip);
        return NULL;
    }
    MLOCK(zfiles_lock);
    zip->next = zfiles;
    zfiles = zip;
    MUNLOCK(zfiles_lock);

    return zip;
}

/*
 * Opens a zip file for reading. Returns the jzfile object or NULL
 * if an error occurred. If a zip error occurred then *msg will be
 * set to the error message text if msg != 0. Otherwise, *msg will be
 * set to NULL.
 */
jzfile * JNICALL
ZIP_Open(const char *name, char **pmsg)
{
    return ZIP_Open_Generic(name, pmsg, O_RDONLY, 0);
}

/*
 * Closes the specified zip file object.
 */
void JNICALL
ZIP_Close(jzfile *zip)
{
    MLOCK(zfiles_lock);
    if (--zip->refs > 0) {
        /* Still more references so just return */
        MUNLOCK(zfiles_lock);
        return;
    }
    /* No other references so close the file and remove from list */
    if (zfiles == zip) {
        zfiles = zfiles->next;
    } else {
        jzfile *zp;
        for (zp = zfiles; zp->next != 0; zp = zp->next) {
            if (zp->next == zip) {
                zp->next = zip->next;
                break;
            }
        }
    }
    MUNLOCK(zfiles_lock);
    freeZip(zip);
    return;
}

#ifndef USE_MMAP

/* Empirically, most CEN headers are smaller than this. */
#define AMPLE_CEN_HEADER_SIZE 160

/* A good buffer size when we want to read CEN headers sequentially. */
#define CENCACHE_PAGESIZE 8192

static char *
readCENHeader(jzfile *zip, jlong cenpos, jint bufsize)
{
    jint censize;
    ZFILE zfd = zip->zfd;
    char *cen;
    if (bufsize > zip->len - cenpos)
        bufsize = zip->len - cenpos;
    if ((cen = malloc(bufsize)) == NULL)       goto Catch;
    if (readFullyAt(zfd, cen, bufsize, cenpos) == -1)     goto Catch;
    censize = CENSIZE(cen);
    if (censize <= bufsize) return cen;
    if ((cen = realloc(cen, censize)) == NULL)              goto Catch;
    if (readFully(zfd, cen+bufsize, censize-bufsize) == -1) goto Catch;
    return cen;

 Catch:
    free(cen);
    return NULL;
}

static char *
sequentialAccessReadCENHeader(jzfile *zip, jlong cenpos)
{
    cencache *cache = &zip->cencache;
    char *cen;
    if (cache->data != NULL
        && (cenpos >= cache->pos)
        && (cenpos + CENHDR <= cache->pos + CENCACHE_PAGESIZE))
    {
        cen = cache->data + cenpos - cache->pos;
        if (cenpos + CENSIZE(cen) <= cache->pos + CENCACHE_PAGESIZE)
            /* A cache hit */
            return cen;
    }

    if ((cen = readCENHeader(zip, cenpos, CENCACHE_PAGESIZE)) == NULL)
        return NULL;
    free(cache->data);
    cache->data = cen;
    cache->pos  = cenpos;
    return cen;
}
#endif /* not USE_MMAP */

typedef enum { ACCESS_RANDOM, ACCESS_SEQUENTIAL } AccessHint;

/*
 * Return a new initialized jzentry corresponding to a given hash cell.
 * In case of error, returns NULL.
 * We already sanity-checked all the CEN headers for ZIP format errors
 * in readCEN(), so we don't check them again here.
 * The ZIP lock should be held here.
 */
static jzentry *
newEntry(jzfile *zip, jzcell *zc, AccessHint accessHint)
{
    jint nlen, elen, clen;
    jzentry *ze;
    char *cen;

    if ((ze = (jzentry *) malloc(sizeof(jzentry))) == NULL) return NULL;
    ze->name    = NULL;
    ze->extra   = NULL;
    ze->comment = NULL;

#ifdef USE_MMAP
    cen = (char*) zip->maddr + zc->cenpos - zip->offset;
#else
    if (accessHint == ACCESS_RANDOM)
        cen = readCENHeader(zip, zc->cenpos, AMPLE_CEN_HEADER_SIZE);
    else
        cen = sequentialAccessReadCENHeader(zip, zc->cenpos);
    if (cen == NULL) goto Catch;
#endif

    nlen      = CENNAM(cen);
    elen      = CENEXT(cen);
    clen      = CENCOM(cen);
    ze->time  = CENTIM(cen);
    ze->size  = CENLEN(cen);
    ze->csize = (CENHOW(cen) == STORED) ? 0 : CENSIZ(cen);
    ze->crc   = CENCRC(cen);
    ze->pos   = -(zip->locpos + CENOFF(cen));

    if ((ze->name = malloc(nlen + 1)) == NULL) goto Catch;
    memcpy(ze->name, cen + CENHDR, nlen);
    ze->name[nlen] = '\0';

    if (elen > 0) {
        /* This entry has "extra" data */
        if ((ze->extra = malloc(elen + 2)) == NULL) goto Catch;
        ze->extra[0] = (unsigned char) elen;
        ze->extra[1] = (unsigned char) (elen >> 8);
        memcpy(ze->extra+2, cen + CENHDR + nlen, elen);
    }

    if (clen > 0) {
        /* This entry has a comment */
        if ((ze->comment = malloc(clen + 1)) == NULL) goto Catch;
        memcpy(ze->comment, cen + CENHDR + nlen + elen, clen);
        ze->comment[clen] = '\0';
    }
    goto Finally;

 Catch:
    free(ze->name);
    free(ze->extra);
    free(ze->comment);
    free(ze);
    ze = NULL;

 Finally:
#ifndef USE_MMAP
    if (cen != NULL && accessHint == ACCESS_RANDOM) free(cen);
#endif
    return ze;
}

/*
 * Free the given jzentry.
 * In fact we maintain a one-entry cache of the most recently used
 * jzentry for each zip.  This optimizes a common access pattern.
 */

void
ZIP_FreeEntry(jzfile *jz, jzentry *ze)
{
    jzentry *last;
    ZIP_Lock(jz);
    last = jz->cache;
    jz->cache = ze;
    ZIP_Unlock(jz);
    if (last != NULL) {
        /* Free the previously cached jzentry */
        free(last->name);
        if (last->extra)   free(last->extra);
        if (last->comment) free(last->comment);
        free(last);
    }
}

/*
 * Returns the zip entry corresponding to the specified name, or
 * NULL if not found.
 */
jzentry *
ZIP_GetEntry(jzfile *zip, char *name, jint ulen)
{
    unsigned int hsh = hash(name);
    jint idx = zip->table[hsh % zip->tablelen];
    jzentry *ze;

    ZIP_Lock(zip);

    /*
     * This while loop is an optimization where a double lookup
     * for name and name+/ is being performed. The name char
     * array has enough room at the end to try again with a
     * slash appended if the first table lookup does not succeed.
     */
    while(1) {

        /* Check the cached entry first */
        ze = zip->cache;
        if (ze && strcmp(ze->name,name) == 0) {
            /* Cache hit!  Remove and return the cached entry. */
            zip->cache = 0;
            ZIP_Unlock(zip);
            return ze;
        }
        ze = 0;

        /*
         * Search down the target hash chain for a cell whose
         * 32 bit hash matches the hashed name.
         */
        while (idx != ZIP_ENDCHAIN) {
            jzcell *zc = &zip->entries[idx];

            if (zc->hash == hsh) {
                /*
                 * OK, we've found a ZIP entry whose 32 bit hashcode
                 * matches the name we're looking for.  Try to read
                 * its entry information from the CEN.  If the CEN
                 * name matches the name we're looking for, we're
                 * done.
                 * If the names don't match (which should be very rare)
                 * we keep searching.
                 */
                ze = newEntry(zip, zc, ACCESS_RANDOM);
                if (ze && strcmp(ze->name, name)==0) {
                    break;
                }
                if (ze != 0) {
                    /* We need to release the lock across the free call */
                    ZIP_Unlock(zip);
                    ZIP_FreeEntry(zip, ze);
                    ZIP_Lock(zip);
                }
                ze = 0;
            }
            idx = zc->next;
        }

        /* Entry found, return it */
        if (ze != 0) {
            break;
        }

        /* If no real length was passed in, we are done */
        if (ulen == 0) {
            break;
        }

        /* Slash is already there? */
        if (name[ulen-1] == '/') {
            break;
        }

        /* Add slash and try once more */
        name[ulen] = '/';
        name[ulen+1] = '\0';
        hsh = hash_append(hsh, '/');
        idx = zip->table[hsh % zip->tablelen];
        ulen = 0;
    }

    ZIP_Unlock(zip);
    return ze;
}

/*
 * Returns the n'th (starting at zero) zip file entry, or NULL if the
 * specified index was out of range.
 */
jzentry * JNICALL
ZIP_GetNextEntry(jzfile *zip, jint n)
{
    jzentry *result;
    if (n < 0 || n >= zip->total) {
        return 0;
    }
    ZIP_Lock(zip);
    result = newEntry(zip, &zip->entries[n], ACCESS_SEQUENTIAL);
    ZIP_Unlock(zip);
    return result;
}

/*
 * Locks the specified zip file for reading.
 */
void
ZIP_Lock(jzfile *zip)
{
    MLOCK(zip->lock);
}

/*
 * Unlocks the specified zip file.
 */
void
ZIP_Unlock(jzfile *zip)
{
    MUNLOCK(zip->lock);
}

/*
 * Returns the offset of the entry data within the zip file.
 * Returns -1 if an error occurred, in which case zip->msg will
 * contain the error text.
 */
jlong
ZIP_GetEntryDataOffset(jzfile *zip, jzentry *entry)
{
    /* The Zip file spec explicitly allows the LOC extra data size to
     * be different from the CEN extra data size, although the JDK
     * never creates such zip files.  Since we cannot trust the CEN
     * extra data size, we need to read the LOC to determine the entry
     * data offset.  We do this lazily to avoid touching the virtual
     * memory page containing the LOC when initializing jzentry
     * objects.  (This speeds up javac by a factor of 10 when the JDK
     * is installed on a very slow filesystem.)
     */
    if (entry->pos <= 0) {
        unsigned char loc[LOCHDR];
        if (readFullyAt(zip->zfd, loc, LOCHDR, -(entry->pos)) == -1) {
            zip->msg = "error reading zip file";
            return -1;
        }
        if (GETSIG(loc) != LOCSIG) {
            zip->msg = "invalid LOC header (bad signature)";
            return -1;
        }
        entry->pos = (- entry->pos) + LOCHDR + LOCNAM(loc) + LOCEXT(loc);
    }
    return entry->pos;
}

/*
 * Reads bytes from the specified zip entry. Assumes that the zip
 * file had been previously locked with ZIP_Lock(). Returns the
 * number of bytes read, or -1 if an error occurred. If zip->msg != 0
 * then a zip error occurred and zip->msg contains the error text.
 */
jint
ZIP_Read(jzfile *zip, jzentry *entry, jlong pos, void *buf, jint len)
{
    jlong entry_size = (entry->csize != 0) ? entry->csize : entry->size;
    jlong start;

    /* Clear previous zip error */
    zip->msg = NULL;

    /* Check specified position */
    if (pos < 0 || pos > entry_size - 1) {
        zip->msg = "ZIP_Read: specified offset out of range";
        return -1;
    }

    /* Check specified length */
    if (len <= 0)
        return 0;
    if (len > entry_size - pos)
        len = entry_size - pos;

    /* Get file offset to start reading data */
    start = ZIP_GetEntryDataOffset(zip, entry);
    if (start < 0)
        return -1;
    start += pos;

    if (start + len > zip->len) {
        zip->msg = "ZIP_Read: corrupt zip file: invalid entry size";
        return -1;
    }

    if (readFullyAt(zip->zfd, buf, len, start) == -1) {
        zip->msg = "ZIP_Read: error reading zip file";
        return -1;
    }
    return len;
}


/* The maximum size of a stack-allocated buffer.
 */
#define BUF_SIZE 4096

/*
 * This function is used by the runtime system to load compressed entries
 * from ZIP/JAR files specified in the class path. It is defined here
 * so that it can be dynamically loaded by the runtime if the zip library
 * is found.
 */
jboolean
InflateFully(jzfile *zip, jzentry *entry, void *buf, char **msg)
{
    z_stream strm;
    char tmp[BUF_SIZE];
    jlong pos = 0;
    jlong count = entry->csize;
    jboolean status;

    *msg = 0; /* Reset error message */

    if (count == 0) {
        *msg = "inflateFully: entry not compressed";
        return JNI_FALSE;
    }

    memset(&strm, 0, sizeof(z_stream));
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        *msg = strm.msg;
        return JNI_FALSE;
    }

    strm.next_out = buf;
    strm.avail_out = entry->size;

    while (count > 0) {
        jint n = count > (jlong)sizeof(tmp) ? (jint)sizeof(tmp) : count;
        ZIP_Lock(zip);
        n = ZIP_Read(zip, entry, pos, tmp, n);
        ZIP_Unlock(zip);
        if (n <= 0) {
            if (n == 0) {
                *msg = "inflateFully: Unexpected end of file";
            }
            inflateEnd(&strm);
            return JNI_FALSE;
        }
        pos += n;
        count -= n;
        strm.next_in = (Bytef *)tmp;
        strm.avail_in = n;
        do {
            switch (inflate(&strm, Z_PARTIAL_FLUSH)) {
            case Z_OK:
                break;
            case Z_STREAM_END:
                if (count != 0 || strm.total_out != entry->size) {
                    *msg = "inflateFully: Unexpected end of stream";
                    inflateEnd(&strm);
                    return JNI_FALSE;
                }
                break;
            default:
                break;
            }
        } while (strm.avail_in > 0);
    }
    inflateEnd(&strm);
    return JNI_TRUE;
}

jzentry * JNICALL
ZIP_FindEntry(jzfile *zip, char *name, jint *sizeP, jint *nameLenP)
{
    jzentry *entry = ZIP_GetEntry(zip, name, 0);
    if (entry) {
        *sizeP = entry->size;
        *nameLenP = strlen(entry->name);
    }
    return entry;
}

/*
 * Reads a zip file entry into the specified byte array
 * When the method completes, it releases the jzentry.
 * Note: this is called from the separately delivered VM (hotspot/classic)
 * so we have to be careful to maintain the expected behaviour.
 */
jboolean JNICALL
ZIP_ReadEntry(jzfile *zip, jzentry *entry, unsigned char *buf, char *entryname)
{
    char *msg;

    strcpy(entryname, entry->name);
    if (entry->csize == 0) {
        /* Entry is stored */
        jlong pos = 0;
        jlong size = entry->size;
        while (pos < size) {
            jint n;
            jlong limit = ((((jlong) 1) << 31) - 1);
            jint count = (size - pos < limit) ?
                /* These casts suppress a VC++ Internal Compiler Error */
                (jint) (size - pos) :
                (jint) limit;
            ZIP_Lock(zip);
            n = ZIP_Read(zip, entry, pos, buf, count);
            msg = zip->msg;
            ZIP_Unlock(zip);
            if (n == -1) {
                jio_fprintf(stderr, "%s: %s\n", zip->name,
                            msg != 0 ? msg : strerror(errno));
                return JNI_FALSE;
            }
            buf += n;
            pos += n;
        }
    } else {
        /* Entry is compressed */
        int ok = InflateFully(zip, entry, buf, &msg);
        if (!ok) {
            if ((msg == NULL) || (*msg == 0)) {
                msg = zip->msg;
            }
            jio_fprintf(stderr, "%s: %s\n", zip->name,
                        msg != 0 ? msg : strerror(errno));
            return JNI_FALSE;
        }
    }

    ZIP_FreeEntry(zip, entry);

    return JNI_TRUE;
}
