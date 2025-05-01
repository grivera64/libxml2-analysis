#ifndef XML_HTML_H_PRIVATE__
#define XML_HTML_H_PRIVATE__

#include <libxml/xmlversion.h>

#ifdef LIBXML_HTML_ENABLED

#define DATA_RCDATA         1
#define DATA_RAWTEXT        2
#define DATA_PLAINTEXT      3
#define DATA_SCRIPT         4
#define DATA_SCRIPT_ESC1    5
#define DATA_SCRIPT_ESC2    6

XML_HIDDEN xmlNodePtr
htmlCtxtParseContentInternal(xmlParserCtxtPtr ctxt, xmlParserInputPtr input);

#endif /* LIBXML_HTML_ENABLED */

#endif /* XML_HTML_H_PRIVATE__ */

