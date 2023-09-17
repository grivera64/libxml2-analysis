#ifndef XML_XPATH_H_PRIVATE__
#define XML_XPATH_H_PRIVATE__

XML_HIDDEN void
xmlInitXPathInternal(void);

XML_HIDDEN int
xmlXPathIsSatisfiableOnSchema(xmlXPathContextPtr ctx,
    const xmlChar* str,
    const xmlRegexpPtr closure);

#endif /* XML_XPATH_H_PRIVATE__ */
