#ifndef XML_XPATH_H_PRIVATE__
#define XML_XPATH_H_PRIVATE__

#include <libxml/xpath.h>

#ifdef LIBXML_XPATH_ENABLED

XML_HIDDEN void
xmlInitXPathInternal(void);

XML_HIDDEN void
xmlXPathErrMemory(xmlXPathContextPtr ctxt);
XML_HIDDEN void
xmlXPathPErrMemory(xmlXPathParserContextPtr ctxt);

XML_HIDDEN void
xmlXPathNodeSetClear(xmlNodeSetPtr set, int hasNsNodes);

#endif /* LIBXML_XPATH_ENABLED */

#endif /* XML_XPATH_H_PRIVATE__ */
