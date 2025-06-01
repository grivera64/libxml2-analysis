/*
 * example_oob.c: C program to run libxml2 example where
 *       the xmlMallocLoc() function can write out-of-bounds
 *
 * To compile on Unixes:
 * cc -o example_oob `xml2-config --cflags` example_oob.c `xml2-config --libs` -lpthread
 *
 * See Copyright for the status of this software.
 *
 * daniel@veillard.com
 */

#include "libxml.h"
#include <stdint.h>
#include <stdio.h>

#if !defined(_WIN32) || defined(__CYGWIN__)
#include <unistd.h>
#endif
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#ifdef LIBXML_READER_ENABLED
#include <libxml/xmlreader.h>
#endif

#define MEMHDR_SIZE 40UL

int main(void) {
    size_t size = SIZE_MAX - MEMHDR_SIZE + 2; // Size such that RESERVE_SIZE + size overflows to 1
    void *ptr = xmlMallocLoc(size, __FILE__, __LINE__);
    ptr = xmlReallocLoc(ptr, size - MEMHDR_SIZE, __FILE__, __LINE__);
    xmlFree(ptr);
    return 0;
}
