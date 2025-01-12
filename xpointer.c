/*
 * xpointer.c : Code to handle XML Pointer
 *
 * Base implementation was made accordingly to
 * W3C Candidate Recommendation 7 June 2000
 * http://www.w3.org/TR/2000/CR-xptr-20000607
 *
 * Added support for the element() scheme described in:
 * W3C Proposed Recommendation 13 November 2002
 * http://www.w3.org/TR/2002/PR-xptr-element-20021113/
 *
 * See Copyright for the status of this software.
 *
 * daniel@veillard.com
 */

/* To avoid EBCDIC trouble when parsing on zOS */
#if defined(__MVS__)
#pragma convert("ISO8859-1")
#endif

#define IN_LIBXML
#include "libxml.h"

/*
 * TODO: better handling of error cases, the full expression should
 *       be parsed beforehand instead of a progressive evaluation
 * TODO: Access into entities references are not supported now ...
 *       need a start to be able to pop out of entities refs since
 *       parent is the entity declaration, not the ref.
 */

#include <string.h>
#include <libxml/xpointer.h>
#include <libxml/xmlmemory.h>
#include <libxml/parserInternals.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlerror.h>

#ifdef LIBXML_XPTR_ENABLED

/* Add support of the xmlns() xpointer scheme to initialize the namespaces */
#define XPTR_XMLNS_SCHEME

#include "private/error.h"
#include "private/parser.h"
#include "private/xpath.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

typedef struct {
    const xmlChar *cur;			/* the current char being parsed */
    const xmlChar *base;		/* the full expression */

    int error;				/* error code */

    xmlXPathContextPtr  context;	/* the evaluation context */
    xmlXPathObjectPtr     value;	/* the current value */
} xmlXPtrEvalCtxt;

/************************************************************************
 *									*
 *		Some factorized error routines				*
 *									*
 ************************************************************************/

/**
 * xmlXPtrErrMemory:
 * @ctxt:  an XPath parser context
 *
 * Handle a memory allocation failure.
 */
static void
xmlXPtrErrMemory(xmlXPtrEvalCtxt *ctxt)
{
    if (ctxt == NULL)
        return;
    ctxt->error = XML_ERR_NO_MEMORY;
    xmlXPathErrMemory(ctxt->context);
}

/**
 * xmlXPtrErr:
 * @ctxt:  an XPTR evaluation context
 * @extra:  extra information
 *
 * Handle an XPointer error
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlXPtrErr(xmlXPtrEvalCtxt *ctxt, int code,
           const char * msg, const xmlChar *extra)
{
    xmlStructuredErrorFunc serror = NULL;
    void *data = NULL;
    xmlNodePtr node = NULL;
    int res;

    if (ctxt == NULL)
        return;
    /* Only report the first error */
    if (ctxt->error != 0)
        return;

    ctxt->error = code;

    if (ctxt->context != NULL) {
        xmlErrorPtr err = &ctxt->context->lastError;

        /* cleanup current last error */
        xmlResetError(err);

        err->domain = XML_FROM_XPOINTER;
        err->code = code;
        err->level = XML_ERR_ERROR;
        err->str1 = (char *) xmlStrdup(ctxt->base);
        if (err->str1 == NULL) {
            xmlXPtrErrMemory(ctxt);
            return;
        }
        err->int1 = ctxt->cur - ctxt->base;

        serror = ctxt->context->serror;
        data = ctxt->context->userData;
    }

    res = xmlRaiseError(serror, NULL, data, NULL, node,
                        XML_FROM_XPOINTER, code, XML_ERR_ERROR, NULL, 0,
                        (const char *) extra, (const char *) ctxt->base,
                        NULL, ctxt->cur - ctxt->base, 0,
                        msg, extra);
    if (res < 0)
        xmlXPtrErrMemory(ctxt);
}

/************************************************************************
 *									*
 *		A few helper functions for child sequences		*
 *									*
 ************************************************************************/

/**
 * xmlXPtrGetNthChild:
 * @cur:  the node
 * @no:  the child number
 *
 * Returns the @no'th element child of @cur or NULL
 */
static xmlNodePtr
xmlXPtrGetNthChild(xmlNodePtr cur, int no) {
    int i;
    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL))
	return(cur);
    cur = cur->children;
    for (i = 0;i <= no;cur = cur->next) {
	if (cur == NULL)
	    return(cur);
	if ((cur->type == XML_ELEMENT_NODE) ||
	    (cur->type == XML_DOCUMENT_NODE) ||
	    (cur->type == XML_HTML_DOCUMENT_NODE)) {
	    i++;
	    if (i == no)
		break;
	}
    }
    return(cur);
}

/************************************************************************
 *									*
 *			The parser					*
 *									*
 ************************************************************************/

static void xmlXPtrEvalChildSeq(xmlXPtrEvalCtxt *ctxt, const xmlChar *name);

/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one need to make assumption on the context to use them
 *
 *   CUR     returns the current xmlChar value, i.e. a 8 bit value
 *           in ISO-Latin or UTF-8.
 *           This should be used internally by the parser
 *           only to compare to ASCII values otherwise it would break when
 *           running with UTF-8 encoding.
 *   NXT(n)  returns the n'th next xmlChar. Same as CUR is should be used only
 *           to compare on ASCII based substring.
 *   SKIP(n) Skip n xmlChar, and must also be used only to skip ASCII defined
 *           strings within the parser.
 *   CURRENT Returns the current char value, with the full decoding of
 *           UTF-8 if we are using this mode. It returns an int.
 *   NEXT    Skip to the next character, this does the proper decoding
 *           in UTF-8 mode. It also pop-up unfinished entities on the fly.
 *           It returns the pointer to the current xmlChar.
 */

#define CUR (*ctxt->cur)
#define SKIP(val) ctxt->cur += (val)
#define NXT(val) ctxt->cur[(val)]

#define SKIP_BLANKS							\
    while (IS_BLANK_CH(*(ctxt->cur))) NEXT

#define CURRENT (*ctxt->cur)
#define NEXT ((*ctxt->cur) ?  ctxt->cur++: ctxt->cur)

static xmlChar *
xmlXPtrParseName(xmlXPtrEvalCtxt *ctxt, int exclude) {
    const xmlChar *start = ctxt->cur;
    xmlChar *ret;
    size_t size;

    size = xmlScanXmlName(start, SIZE_MAX, exclude);
    if (size == 0)
        return(NULL);
    if (size > XML_MAX_NAME_LENGTH)
        return(NULL);

    ctxt->cur += size;

    ret = xmlStrndup(start, size);
    if (ret == NULL)
        xmlXPtrErrMemory(ctxt);

    return(ret);
}

/*
 * xmlXPtrGetChildNo:
 * @ctxt:  the XPointer Parser context
 * @index:  the child number
 *
 * Move the current node of the nodeset on the stack to the
 * given child if found
 */
static void
xmlXPtrGetChildNo(xmlXPtrEvalCtxt *ctxt, int indx) {
    xmlNodePtr cur = NULL;
    xmlXPathObjectPtr obj;
    xmlNodeSetPtr nodeset;

    obj = ctxt->value;
    if ((obj == NULL) || (obj->type != XPATH_NODESET)) {
        xmlXPtrErr(ctxt, XML_ERR_INTERNAL_ERROR, "invalid type", NULL);
        return;
    }
    nodeset = obj->nodesetval;
    if ((indx <= 0) || (nodeset == NULL) || (nodeset->nodeNr != 1)) {
	xmlXPathNodeSetClear(nodeset, 1);
	return;
    }
    cur = xmlXPtrGetNthChild(nodeset->nodeTab[0], indx);
    if (cur == NULL) {
	xmlXPathNodeSetClear(nodeset, 1);
	return;
    }
    nodeset->nodeTab[0] = cur;
}

/**
 * xmlXPtrEvalXPtrPart:
 * @ctxt:  the XPointer Parser context
 * @name:  the preparsed Scheme for the XPtrPart
 *
 * XPtrPart ::= 'xpointer' '(' XPtrExpr ')'
 *            | Scheme '(' SchemeSpecificExpr ')'
 *
 * Scheme   ::=  NCName - 'xpointer' [VC: Non-XPointer schemes]
 *
 * SchemeSpecificExpr ::= StringWithBalancedParens
 *
 * StringWithBalancedParens ::=
 *              [^()]* ('(' StringWithBalancedParens ')' [^()]*)*
 *              [VC: Parenthesis escaping]
 *
 * XPtrExpr ::= Expr [VC: Parenthesis escaping]
 *
 * VC: Parenthesis escaping:
 *   The end of an XPointer part is signaled by the right parenthesis ")"
 *   character that is balanced with the left parenthesis "(" character
 *   that began the part. Any unbalanced parenthesis character inside the
 *   expression, even within literals, must be escaped with a circumflex (^)
 *   character preceding it. If the expression contains any literal
 *   occurrences of the circumflex, each must be escaped with an additional
 *   circumflex (that is, ^^). If the unescaped parentheses in the expression
 *   are not balanced, a syntax error results.
 *
 * Parse and evaluate an XPtrPart. Basically it generates the unescaped
 * string and if the scheme is 'xpointer' it will call the XPath interpreter.
 *
 * TODO: there is no new scheme registration mechanism
 */

static void
xmlXPtrEvalXPtrPart(xmlXPtrEvalCtxt *ctxt, const xmlChar *name) {
    xmlChar *buffer, *cur;
    int len;
    int level;

    if (CUR != '(') {
        xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "expected (", NULL);
        return;
    }
    NEXT;
    level = 1;

    len = xmlStrlen(ctxt->cur);
    len++;
    buffer = xmlMalloc(len);
    if (buffer == NULL) {
        xmlXPtrErrMemory(ctxt);
	return;
    }

    cur = buffer;
    while (CUR != 0) {
	if (CUR == ')') {
	    level--;
	    if (level == 0) {
		NEXT;
		break;
	    }
	} else if (CUR == '(') {
	    level++;
	} else if (CUR == '^') {
            if ((NXT(1) == ')') || (NXT(1) == '(') || (NXT(1) == '^')) {
                NEXT;
            }
	}
        *cur++ = CUR;
	NEXT;
    }
    *cur = 0;

    if ((level != 0) && (CUR == 0)) {
        xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "unexpected eof", NULL);
	xmlFree(buffer);
        return;
    }

    if (xmlStrEqual(name, (xmlChar *) "xpointer") ||
        xmlStrEqual(name, (xmlChar *) "xpath1")) {
	/*
	 * To evaluate an xpointer scheme element (4.3) we need:
	 *   context initialized to the root
	 *   context position initialized to 1
	 *   context size initialized to 1
	 */
	ctxt->context->node = (xmlNodePtr)ctxt->context->doc;
	ctxt->context->proximityPosition = 1;
	ctxt->context->contextSize = 1;

	ctxt->value = xmlXPathEval(buffer, ctxt->context);
        if (ctxt->context->lastError.code == XML_ERR_NO_MEMORY)
            xmlXPtrErrMemory(ctxt);
    } else if (xmlStrEqual(name, (xmlChar *) "element")) {
	const xmlChar *oldBase = ctxt->base;
	const xmlChar *oldCur = ctxt->cur;
	xmlChar *name2;

	ctxt->cur = ctxt->base = buffer;
	if (buffer[0] == '/') {
            ctxt->value = xmlXPathNewNodeSet((xmlNodePtr) ctxt->context->doc);
            if (ctxt->value == NULL)
                xmlXPtrErrMemory(ctxt);
            xmlXPtrEvalChildSeq(ctxt, NULL);
	} else {
	    name2 = xmlXPtrParseName(ctxt, 0);
	    if (name2 == NULL) {
                xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "invalid name", NULL);
                ctxt->base = oldBase;
                ctxt->cur = oldCur;
		xmlFree(buffer);
		return;
	    }
	    xmlXPtrEvalChildSeq(ctxt, name2);
            xmlFree(name2);
	}
	ctxt->base = oldBase;
        ctxt->cur = oldCur;
#ifdef XPTR_XMLNS_SCHEME
    } else if (xmlStrEqual(name, (xmlChar *) "xmlns")) {
	const xmlChar *oldBase = ctxt->base;
	const xmlChar *oldCur = ctxt->cur;
	xmlChar *prefix;

	ctxt->cur = ctxt->base = buffer;
        prefix = xmlXPtrParseName(ctxt, ':');
	if (prefix == NULL) {
            xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "invalid name", NULL);
            ctxt->base = oldBase;
            ctxt->cur = oldCur;
	    xmlFree(buffer);
            return;
	}
	SKIP_BLANKS;
	if (CUR != '=') {
            xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "expected =", NULL);
            ctxt->base = oldBase;
            ctxt->cur = oldCur;
	    xmlFree(prefix);
	    xmlFree(buffer);
            return;
	}
	NEXT;
	SKIP_BLANKS;

	if (xmlXPathRegisterNs(ctxt->context, prefix, ctxt->cur) < 0)
            xmlXPtrErrMemory(ctxt);
        ctxt->base = oldBase;
        ctxt->cur = oldCur;
	xmlFree(prefix);
#endif /* XPTR_XMLNS_SCHEME */
    } else {
        xmlXPtrErr(ctxt, XML_XPTR_UNKNOWN_SCHEME,
		   "unsupported scheme '%s'\n", name);
    }
    xmlFree(buffer);
}

/**
 * xmlXPtrEvalFullXPtr:
 * @ctxt:  the XPointer Parser context
 * @name:  the preparsed Scheme for the first XPtrPart
 *
 * FullXPtr ::= XPtrPart (S? XPtrPart)*
 *
 * As the specs says:
 * -----------
 * When multiple XPtrParts are provided, they must be evaluated in
 * left-to-right order. If evaluation of one part fails, the nexti
 * is evaluated. The following conditions cause XPointer part failure:
 *
 * - An unknown scheme
 * - A scheme that does not locate any sub-resource present in the resource
 * - A scheme that is not applicable to the media type of the resource
 *
 * The XPointer application must consume a failed XPointer part and
 * attempt to evaluate the next one, if any. The result of the first
 * XPointer part whose evaluation succeeds is taken to be the fragment
 * located by the XPointer as a whole. If all the parts fail, the result
 * for the XPointer as a whole is a sub-resource error.
 * -----------
 *
 * Parse and evaluate a Full XPtr i.e. possibly a cascade of XPath based
 * expressions or other schemes.
 */
static void
xmlXPtrEvalFullXPtr(xmlXPtrEvalCtxt *ctxt, const xmlChar *name) {
    xmlChar *buf = NULL;

    while (name != NULL) {
	ctxt->error = XML_ERR_OK;
	xmlXPtrEvalXPtrPart(ctxt, name);

	/* in case of syntax error, break here */
	if ((ctxt->error != XML_ERR_OK) &&
            (ctxt->error != XML_XPTR_UNKNOWN_SCHEME))
	    break;

	/*
	 * If the returned value is a non-empty nodeset
	 * or location set, return here.
	 */
	if (ctxt->value != NULL) {
	    xmlXPathObjectPtr obj = ctxt->value;

	    if (obj->type == XPATH_NODESET) {
                xmlNodeSetPtr nodeset = obj->nodesetval;

                if ((nodeset != NULL) && (nodeset->nodeNr > 0))
                    break;
	    }

	    /*
	     * Evaluating to improper values is equivalent to
	     * a sub-resource error, clean-up the value
	     */
	    xmlXPathFreeObject(obj);
            ctxt->value = NULL;
	}

	/*
	 * Is there another XPointer part.
	 */
        if (buf != NULL)
            xmlFree(buf);
	SKIP_BLANKS;
	buf = xmlXPtrParseName(ctxt, 0);
        name = buf;
    }

    if (buf != NULL)
        xmlFree(buf);
}

/**
 * xmlXPtrEvalChildSeq:
 * @ctxt:  the XPointer Parser context
 * @name:  a possible ID name of the child sequence
 *
 *  ChildSeq ::= '/1' ('/' [0-9]*)*
 *             | Name ('/' [0-9]*)+
 *
 * Parse and evaluate a Child Sequence. This routine also handle the
 * case of a Bare Name used to get a document ID.
 */
static void
xmlXPtrEvalChildSeq(xmlXPtrEvalCtxt *ctxt, const xmlChar *name) {
    /*
     * XPointer don't allow by syntax to address in multirooted trees
     * this might prove useful in some cases, warn about it.
     */
    if ((name == NULL) && (CUR == '/') && (NXT(1) != '1')) {
        xmlXPtrErr(ctxt, XML_XPTR_CHILDSEQ_START,
		   "warning: ChildSeq not starting by /1\n", NULL);
    }

    if (name != NULL) {
        xmlAttrPtr attr;
        xmlNodePtr node = NULL;

        attr = xmlGetID(ctxt->context->doc, name);
        if (attr != NULL)
            node = attr->parent;
        ctxt->value = xmlXPathNewNodeSet(node);
        if (ctxt->value == NULL) {
            xmlXPtrErrMemory(ctxt);
            return;
        }
    }

    while (CUR == '/') {
	int child = 0, overflow = 0;
	NEXT;

	while ((CUR >= '0') && (CUR <= '9')) {
            int d = CUR - '0';
            if (child > INT_MAX / 10)
                overflow = 1;
            else
                child *= 10;
            if (child > INT_MAX - d)
                overflow = 1;
            else
                child += d;
	    NEXT;
	}
        if (overflow)
            child = 0;
	xmlXPtrGetChildNo(ctxt, child);
    }
}


/**
 * xmlXPtrEvalXPointer:
 * @ctxt:  the XPointer Parser context
 *
 *  XPointer ::= Name
 *             | ChildSeq
 *             | FullXPtr
 *
 * Parse and evaluate an XPointer
 */
static void
xmlXPtrEvalXPointer(xmlXPtrEvalCtxt *ctxt) {
    ctxt->value = NULL;

    SKIP_BLANKS;
    if (CUR == '/') {
        ctxt->value = xmlXPathNewNodeSet((xmlNodePtr) ctxt->context->doc);
        if (ctxt->value == NULL)
            xmlXPtrErrMemory(ctxt);
        xmlXPtrEvalChildSeq(ctxt, NULL);
    } else {
	xmlChar *name;

	name = xmlXPtrParseName(ctxt, 0);
	if (name == NULL) {
            xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "invalid name", NULL);
            return;
        }
	if (CUR == '(') {
	    xmlXPtrEvalFullXPtr(ctxt, name);
	    /* Short evaluation */
            xmlFree(name);
	    return;
	} else {
	    /* this handle both Bare Names and Child Sequences */
	    xmlXPtrEvalChildSeq(ctxt, name);
	}
        xmlFree(name);
    }
    SKIP_BLANKS;
    if (CUR != 0)
        xmlXPtrErr(ctxt, XML_XPTR_SYNTAX_ERROR, "expected eof", NULL);
}


/************************************************************************
 *									*
 *			General routines				*
 *									*
 ************************************************************************/

/**
 * xmlXPtrNewContext:
 * @doc:  the XML document
 * @here:  the node that directly contains the XPointer being evaluated or NULL
 * @origin:  the element from which a user or program initiated traversal of
 *           the link, or NULL.
 *
 * Create a new XPointer context
 *
 * Returns the xmlXPathContext just allocated.
 */
xmlXPathContextPtr
xmlXPtrNewContext(xmlDocPtr doc, xmlNodePtr here, xmlNodePtr origin) {
    xmlXPathContextPtr ret;
    (void) here;
    (void) origin;

    ret = xmlXPathNewContext(doc);
    if (ret == NULL)
	return(ret);

    return(ret);
}

/**
 * xmlXPtrEval:
 * @str:  the XPointer expression
 * @ctx:  the XPointer context
 *
 * Evaluate the XPath Location Path in the given context.
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
xmlXPathObjectPtr
xmlXPtrEval(const xmlChar *str, xmlXPathContextPtr ctx) {
    xmlXPtrEvalCtxt ctxt;
    xmlXPathObjectPtr res = NULL;

    xmlInitParser();

    if ((ctx == NULL) || (str == NULL))
	return(NULL);

    xmlResetError(&ctx->lastError);

    memset(&ctxt, 0, sizeof(ctxt));
    ctxt.base = str;
    ctxt.cur = str;
    ctxt.context = ctx;

    xmlXPtrEvalXPointer(&ctxt);

    if ((ctx->lastError.code == XML_ERR_OK) &&
        ((ctxt.value == NULL) ||
	 (ctxt.value->type != XPATH_NODESET))) {
        xmlXPtrErr(&ctxt, XML_XPTR_EVAL_FAILED,
		"xmlXPtrEval: evaluation failed to return a node set\n",
		   NULL);
    }

    if (ctx->lastError.code == XML_ERR_OK) {
        res = ctxt.value;
    } else {
	res = NULL;
	xmlXPathFreeObject(ctxt.value);
    }

    return(res);
}

#endif

